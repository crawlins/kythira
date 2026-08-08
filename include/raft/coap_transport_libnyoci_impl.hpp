#pragma once
//
// Alternate CoAP backend: libnyoci (darconeous/libnyoci, formerly SMCP).
// ---------------------------------------------------------------------------
// coap_libnyoci_client<Types> / coap_libnyoci_server<Types> are a *second*
// implementation of the same kythira::network_client / kythira::network_server
// concepts (include/raft/network.hpp) that the libcoap-backed
// coap_client/coap_server in include/raft/coap_transport.hpp already satisfy.
// They speak the same CoAP protocol on the wire — same POST-to-/raft/<rpc>
// resources, same 2.05 responses, same Content-Format/Accept negotiation — so
// the Raft layer consumes either interchangeably. See
// .kiro/specs/coap-transport-libnyoci/ for the spec and
// doc/coap_library_alternatives.md for the trade-off writeup.
//
// BRIDGE, DON'T BUILD
// -------------------
// libnyoci is a FULL CoAP stack: it owns the UDP socket, the message/token
// layer, retransmission of confirmable messages, duplicate suppression and
// Block2 transfer. So this adapter's job is *bridging*, not *building*:
// translate kythira RPCs into libnyoci transactions and route libnyoci's async
// callbacks back into future_template<...> promises. That is the defining
// contrast with the cantcoap sketch, where the adapter would have to implement
// all of the above by hand.
//
// WHY THIS FILE DOES NOT INCLUDE raft/coap_transport.hpp
// ------------------------------------------------------
// It cannot. coap_transport.hpp includes <coap3/coap.h> whenever
// LIBCOAP_AVAILABLE is defined, and libcoap's and libnyoci's C headers cannot
// coexist in one translation unit: libcoap spells the option numbers as
// object-like macros (`#define COAP_OPTION_IF_MATCH 1`) while libnyoci spells
// them as enumerators (`COAP_OPTION_IF_MATCH = 1,`), so libcoap's macros
// rewrite the middle of libnyoci's enum into `1 = 1,`. No include order fixes
// it. The types both backends genuinely share (coap_client_config,
// coap_server_config, pending_message, translate_legacy_fields) therefore live
// in raft/coap_transport_config.hpp, which is deliberately libcoap-free; this
// header includes that one instead. A translation unit picks a backend by
// which header it includes, and the two libraries do not collide at link time
// (Requirement 2.4).
//
// THREADING MODEL
// ---------------
// libnyoci's API is not thread-safe and most of it (every nyoci_inbound_* /
// nyoci_outbound_* call) is only legal from inside one of its own callbacks.
// Its "current instance" is a pthread-key thread-local. So each client and
// each server owns exactly one nyoci_t driven by exactly one std::jthread, and
// *every* libnyoci call happens on that thread. send_request_vote() and
// friends therefore do not touch libnyoci at all: they serialize, park a
// pending-RPC record on a queue and return the future. The loop thread picks
// the record up, starts the transaction, and later fulfils the promise from
// the response callback (Requirement 6.1, 6.3).
//
// Gating mirrors LIBCOAP_AVAILABLE exactly: this file compiles to a stub
// unless LIBNYOCI_AVAILABLE is defined by the build (root CMakeLists.txt's
// pkg_check_modules(LIBNYOCI ...) probe). The stub keeps the full concept
// surface — so the static_asserts at the bottom hold either way — and reports
// unavailability at run time instead of failing the build (Requirement 2.2).
//
#include <raft/types.hpp>
#include <raft/network.hpp>
// coap_client_config / coap_server_config / pending_message /
// translate_legacy_fields(), shared with the libcoap backend. NOT
// raft/coap_transport.hpp — see the header comment above.
#include <raft/coap_transport_config.hpp>
#include <raft/coap_exceptions.hpp>
#include <raft/coap_security.hpp>
#include <raft/coap_utils.hpp>
#include <raft/peer_capability_cache.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/future_default.hpp>
#include <concepts/future.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#ifdef LIBNYOCI_AVAILABLE
// libnyoci is plain C and its headers do not wrap themselves in extern "C".
extern "C" {
#include <libnyoci/libnyoci.h>
#include <libnyoci/nyoci-transaction.h>
}
// libnyoci's DTLS plugin only forward-declares struct ssl_ctx_st; the adapter
// builds and owns the SSL_CTX itself (see dtls_state), so it needs the real
// OpenSSL headers. network_simulator already links OpenSSL::SSL/Crypto, so this
// costs consumers nothing.
#include <openssl/ssl.h>
// For reserve_ephemeral_port() — see its comment for why libnyoci leaves us to
// find a DTLS port ourselves.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kythira {

// The CoAP resource each RPC is POSTed to. Identical to the paths
// coap_transport_impl.hpp registers, so a libnyoci client can talk to a
// libcoap server and vice versa.
inline constexpr const char* libnyoci_request_vote_path = "/raft/request_vote";
inline constexpr const char* libnyoci_append_entries_path = "/raft/append_entries";
inline constexpr const char* libnyoci_install_snapshot_path = "/raft/install_snapshot";

// How long the process-loop thread blocks in nyoci_plat_wait() before looking
// at its own queues again. libnyoci has no way to interrupt its poll() from
// another thread, so this is what bounds the latency between send_rpc()
// enqueuing a request and the loop thread starting the transaction, and
// between stop() being requested and the loop noticing.
inline constexpr int libnyoci_poll_interval_ms = 20;

namespace libnyoci_detail {

#ifdef LIBNYOCI_AVAILABLE

/// One decoded CoAP option from the packet currently being processed.
struct inbound_options {
    std::optional<std::uint16_t> content_format;
    std::vector<std::uint16_t> accepted_formats;
    std::optional<std::uint32_t> block2;
};

/// Walk the inbound packet's options once, picking out the three the transport
/// cares about. Only legal from inside a libnyoci callback, like every other
/// nyoci_inbound_* call.
[[nodiscard]] inline auto scan_inbound_options() -> inbound_options {
    inbound_options result;
    nyoci_inbound_reset_next_option();
    for (;;) {
        const std::uint8_t* value = nullptr;
        coap_size_t value_len = 0;
        const coap_option_key_t key = nyoci_inbound_next_option(&value, &value_len);
        if (key == COAP_OPTION_INVALID) {
            break;
        }
        switch (key) {
            case COAP_OPTION_CONTENT_TYPE:
                result.content_format = static_cast<std::uint16_t>(
                    coap_decode_uint32(value, static_cast<std::uint8_t>(value_len)));
                break;
            case COAP_OPTION_ACCEPT:
                result.accepted_formats.push_back(static_cast<std::uint16_t>(
                    coap_decode_uint32(value, static_cast<std::uint8_t>(value_len))));
                break;
            case COAP_OPTION_BLOCK2:
                result.block2 = coap_decode_uint32(value, static_cast<std::uint8_t>(value_len));
                break;
            default:
                break;
        }
    }
    return result;
}

/// Add a Block2 option carrying `num`/`more`/`szx`, encoded the way RFC 7959
/// wants it: a network-order minimum-length unsigned integer. Adding it as a
/// raw uint32 would put the value on the wire byte-swapped on a little-endian
/// host — the same bug the libcoap backend's Content-Format encoding had.
inline auto add_block2_option(std::uint32_t num, bool more, std::uint8_t szx) -> nyoci_status_t {
    const std::uint32_t encoded = (num << 4U) | (more ? 0x8U : 0x0U) | (szx & 0x7U);
    std::array<std::uint8_t, 4> bytes{
        static_cast<std::uint8_t>((encoded >> 24U) & 0xFFU),
        static_cast<std::uint8_t>((encoded >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((encoded >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(encoded & 0xFFU),
    };
    std::size_t first = 0;
    while (first < 3 && bytes[first] == 0) {
        ++first;
    }
    return nyoci_outbound_add_option(COAP_OPTION_BLOCK2,
                                     reinterpret_cast<const char*>(bytes.data() + first),
                                     static_cast<coap_size_t>(bytes.size() - first));
}

/// Add a Content-Format option, same minimum-length network-order encoding.
inline auto add_content_format_option(coap_option_key_t key, std::uint16_t format)
    -> nyoci_status_t {
    if (format <= 0xFF) {
        const auto byte = static_cast<std::uint8_t>(format);
        return nyoci_outbound_add_option(key, reinterpret_cast<const char*>(&byte), 1);
    }
    const std::array<std::uint8_t, 2> bytes{static_cast<std::uint8_t>((format >> 8U) & 0xFFU),
                                            static_cast<std::uint8_t>(format & 0xFFU)};
    return nyoci_outbound_add_option(key, reinterpret_cast<const char*>(bytes.data()), 2);
}

/// Translate a libnyoci status or CoAP response code into the same
/// coap_exceptions.hpp type the libcoap path throws for the same condition, so
/// callers observe identical failure semantics regardless of backend
/// (Requirement 4.3, design.md "Error Handling").
[[nodiscard]] inline auto exception_for_status(int statuscode, const std::string& context)
    -> std::exception_ptr {
    if (statuscode >= COAP_RESULT_500) {
        return std::make_exception_ptr(
            coap_server_error(static_cast<std::uint8_t>(statuscode),
                              context + ": server error " + coap_code_to_cstr(statuscode)));
    }
    if (statuscode >= COAP_RESULT_400) {
        return std::make_exception_ptr(
            coap_client_error(static_cast<std::uint8_t>(statuscode),
                              context + ": client error " + coap_code_to_cstr(statuscode)));
    }
    if (statuscode >= 0) {
        return std::make_exception_ptr(coap_protocol_error(context + ": unexpected response code " +
                                                           coap_code_to_cstr(statuscode)));
    }
    switch (statuscode) {
        case NYOCI_STATUS_TIMEOUT:
            // libnyoci reports both "no ACK after every retransmission" and
            // "transaction expiration elapsed" as NYOCI_STATUS_TIMEOUT; both
            // are the timeout the caller asked us to enforce.
            return std::make_exception_ptr(coap_timeout_error(
                context + ": transaction timed out or exhausted retransmissions"));
        case NYOCI_STATUS_RESET:
            return std::make_exception_ptr(coap_protocol_error(context + ": peer sent RST"));
        case NYOCI_STATUS_TRANSACTION_INVALIDATED:
            return std::make_exception_ptr(coap_transport_error(
                context + ": transaction cancelled before a response arrived"));
        // Everything that means "we could not reach, or could not address, the
        // peer" — name resolution, socket errors, and an endpoint URI libnyoci
        // will not parse.
        case NYOCI_STATUS_HOST_LOOKUP_FAILURE:
        case NYOCI_STATUS_BAD_HOSTNAME:
        case NYOCI_STATUS_ERRNO:
        case NYOCI_STATUS_H_ERRNO:
        case NYOCI_STATUS_UNSUPPORTED_URI:
        case NYOCI_STATUS_URI_PARSE_FAILURE:
            return std::make_exception_ptr(
                coap_network_error(context + ": " + nyoci_status_to_cstr(statuscode)));
        case NYOCI_STATUS_MESSAGE_TOO_BIG:
            // libnyoci implements Block2 (block-wise *responses*) but has no
            // Block1 at all, so an over-large request cannot be split. Say so
            // rather than leaving the caller to wonder.
            return std::make_exception_ptr(coap_transport_error(
                context +
                ": request payload exceeds libnyoci's maximum CoAP message size, and libnyoci "
                "implements no Block1 (block-wise request) support to split it"));
        default:
            return std::make_exception_ptr(
                coap_transport_error(context + ": " + nyoci_status_to_cstr(statuscode)));
    }
}

#endif  // LIBNYOCI_AVAILABLE

/// Which wire channel this backend will run `config` over.
enum class channel {
    plain,  //!< CoAP over UDP.
    dtls    //!< CoAP over DTLS, via libnyoci's OpenSSL plugin.
};

/// Decide the channel for `config`, or refuse it (Requirement 5).
///
/// The spec's original plan was to keep kythira's `coap_security_provider`
/// above a plain-CoAP libnyoci core. That is not achievable: every method of
/// that interface is expressed in libcoap types (`coap_context_t*`,
/// `coap_session_t*`, `coap_pdu_t*`), and its `protect`/`unprotect` hooks are
/// identity passthroughs precisely *because* every mode is implemented below
/// libcoap's PDU API rather than as a byte-level transform an adapter could
/// call. There is no seam to sit above.
///
/// What is achievable is libnyoci's own OpenSSL DTLS plugin, which turns out to
/// be enough for two of the four secured modes. The refusals below are
/// deliberate and specific: silently downgrading a node that asked for
/// encryption to plaintext Raft traffic is strictly worse than not starting.
///
/// Configuration handling stays shared with the libcoap backend either way —
/// `translate_legacy_fields()` is the same function, so the same malformed
/// configs raise the same `coap_security_config_error` (Requirement 5.4).
template<typename Config>
[[nodiscard]] inline auto plan_security(const Config& config, const char* role)
    -> std::pair<channel, coap_security_config> {
    coap_security_config effective = kythira::translate_legacy_fields(config);
    switch (effective.mode) {
        case coap_auth_mode::none:
            return {channel::plain, std::move(effective)};

        case coap_auth_mode::dtls_psk:
        case coap_auth_mode::dtls_pki:
#ifdef LIBNYOCI_AVAILABLE
            return {channel::dtls, std::move(effective)};
#else
            throw coap_security_error(
                std::string("DTLS was requested for this libnyoci CoAP ") + role +
                ", but the backend was built without libnyoci. Rebuild with the vcpkg "
                "'coap-libnyoci' feature.");
#endif

        case coap_auth_mode::dtls_rpk:
            // RFC 7250 raw public keys need the peer to negotiate a non-X.509
            // certificate type. libnyoci's plugin hands us nothing but an
            // SSL_CTX, and OpenSSL only grew raw-public-key support in 3.2
            // (SSL_CTX_set1_client_cert_type and friends) -- there is no way to
            // express RPK through the surface available here.
            throw coap_security_error(
                std::string("the libnyoci CoAP backend cannot provide DTLS-RPK for this ") + role +
                ". Raw public keys (RFC 7250) require the certificate-type extensions OpenSSL "
                "only added in 3.2, and libnyoci's DTLS plugin exposes nothing but an SSL_CTX. "
                "Use dtls_pki here, or the libcoap-backed coap_client/coap_server for RPK. See "
                ".kiro/specs/coap-transport-libnyoci/ Task 5.");

        case coap_auth_mode::oscore:
            // Not a libnyoci gap so much as a kythira one: kythira does not
            // implement OSCORE, it configures libcoap's
            // (coap_context_oscore_server / coap_new_client_session_oscore).
            // There is no backend-neutral OSCORE to reach for. Same for the
            // EDHOC bootstrap that feeds it -- the handshake itself is already
            // transport-neutral, but the OSCORE context consuming its output is
            // not. See doc/TODO.md.
            throw coap_security_error(
                std::string("the libnyoci CoAP backend cannot provide OSCORE for this ") + role +
                ". libnyoci ships no OSCORE, and kythira has no implementation of its own to "
                "fall back on -- oscore_provider delegates entirely to libcoap. Use the "
                "libcoap-backed coap_client/coap_server for OSCORE and EDHOC. See doc/TODO.md.");
    }
    throw coap_security_config_error("unknown coap_auth_mode");
}

/// Join an endpoint ("coap://127.0.0.1:5683", with or without scheme or a
/// trailing slash) and a resource path ("/raft/append_entries") into the
/// absolute URI nyoci_outbound_set_uri() wants.
///
/// The scheme is forced to match `secure` rather than taken from the endpoint
/// string, and that is the point: nyoci_outbound_set_uri() picks the session
/// type straight out of the scheme (via nyoci_session_type_from_uri_scheme), so
/// an endpoint left as "coap://" in a DTLS-configured client would quietly send
/// the request in the clear.
[[nodiscard]] inline auto build_request_uri(std::string endpoint, const std::string& resource_path,
                                            bool secure) -> std::string {
    for (const auto* scheme : {"coaps://", "coap://"}) {
        if (endpoint.rfind(scheme, 0) == 0) {
            endpoint.erase(0, std::strlen(scheme));
            break;
        }
    }
    while (!endpoint.empty() && endpoint.back() == '/') {
        endpoint.pop_back();
    }
    return (secure ? "coaps://" : "coap://") + endpoint + resource_path;
}

#ifdef LIBNYOCI_AVAILABLE

/// Everything a DTLS-secured nyoci_t needs kept alive for as long as it lives.
///
/// libnyoci never frees the SSL_CTX it is given -- there is no SSL_CTX_free
/// anywhere in its plugin and nyoci_plat_finalize() only closes file
/// descriptors -- so the adapter always builds its own and owns it. Passing
/// NYOCI_PLAT_TLS_DEFAULT_CONTEXT would make libnyoci allocate one it then
/// leaks, on top of leaving us no handle to configure PKI through.
struct dtls_state {
    // Held for the PSK callbacks, which libnyoci invokes with this object as
    // their void* context from inside the handshake.
    std::string psk_identity;
    std::vector<unsigned char> psk_key;
    std::string psk_hint;
    SSL_CTX* ssl_ctx{nullptr};

    dtls_state() = default;
    dtls_state(const dtls_state&) = delete;
    auto operator=(const dtls_state&) -> dtls_state& = delete;
    dtls_state(dtls_state&&) = delete;
    auto operator=(dtls_state&&) -> dtls_state& = delete;

    ~dtls_state() {
        if (ssl_ctx != nullptr) {
            SSL_CTX_free(ssl_ctx);
        }
    }
};

extern "C" inline auto libnyoci_client_psk_trampoline(void* context, const char* /*hint*/,
                                                      char* identity, unsigned int max_identity_len,
                                                      unsigned char* psk, unsigned int max_psk_len)
    -> unsigned int {
    const auto* state = static_cast<const dtls_state*>(context);
    if (state == nullptr || state->psk_identity.size() + 1 > max_identity_len ||
        state->psk_key.size() > max_psk_len) {
        return 0;  // Aborts the handshake.
    }
    std::memcpy(identity, state->psk_identity.c_str(), state->psk_identity.size() + 1);
    std::memcpy(psk, state->psk_key.data(), state->psk_key.size());
    return static_cast<unsigned int>(state->psk_key.size());
}

extern "C" inline auto libnyoci_server_psk_trampoline(void* context, const char* identity,
                                                      unsigned char* psk, unsigned int max_psk_len)
    -> unsigned int {
    const auto* state = static_cast<const dtls_state*>(context);
    if (state == nullptr || identity == nullptr || state->psk_identity != identity ||
        state->psk_key.size() > max_psk_len) {
        return 0;  // Unknown identity: abort rather than offer a wrong key.
    }
    std::memcpy(psk, state->psk_key.data(), state->psk_key.size());
    return static_cast<unsigned int>(state->psk_key.size());
}

/// A DTLS context with the same defaults libnyoci's own would use, but owned by
/// us. PSK ciphersuites stay in the list for the dtls_psk path; dtls_pki
/// narrows the verification settings afterwards.
[[nodiscard]] inline auto make_dtls_context() -> SSL_CTX* {
    SSL_CTX* ctx = SSL_CTX_new(DTLS_method());
    if (ctx == nullptr) {
        throw coap_security_error("failed to allocate a DTLS context for the libnyoci backend");
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    if (SSL_CTX_set_cipher_list(ctx, "ALL:!EXPORT:!LOW:!aNULL:!eNULL:!SSLv2:PSK") != 1) {
        SSL_CTX_free(ctx);
        throw coap_security_error("failed to set the DTLS cipher list for the libnyoci backend");
    }
    return ctx;
}

/// Apply PKI credentials to `ctx`. Deliberately mirrors what the libcoap
/// backend's dtls_pki_provider asks libcoap for, so a config that works there
/// means the same thing here: own certificate and key, optional CA bundle, and
/// peer verification unless explicitly disabled.
inline auto apply_pki_credentials(SSL_CTX* ctx, const pki_credentials& creds,
                                  coap_security_role role) -> void {
    if (creds.cert_file.empty() || creds.key_file.empty()) {
        throw coap_security_config_error(
            "dtls_pki requires both cert_file and key_file for the libnyoci backend");
    }
    if (SSL_CTX_use_certificate_chain_file(ctx, creds.cert_file.c_str()) != 1) {
        throw coap_security_error("failed to load the DTLS certificate: " + creds.cert_file);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, creds.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        throw coap_security_error("failed to load the DTLS private key: " + creds.key_file);
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        throw coap_security_error("the DTLS private key does not match the certificate: " +
                                  creds.key_file);
    }
    if (!creds.ca_file.empty()) {
        if (SSL_CTX_load_verify_locations(ctx, creds.ca_file.c_str(), nullptr) != 1) {
            throw coap_security_error("failed to load the DTLS CA bundle: " + creds.ca_file);
        }
    }
    if (creds.verify_peer_cert) {
        // A server also demanding a client certificate is what makes this
        // mutual, matching the libcoap path's require_peer_cert behaviour.
        const int mode = (role == coap_security_role::server)
                             ? (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT)
                             : SSL_VERIFY_PEER;
        SSL_CTX_set_verify(ctx, mode, nullptr);
    }
    if (!creds.cipher_suites.empty()) {
        std::string list;
        for (const auto& suite : creds.cipher_suites) {
            if (!list.empty()) {
                list += ':';
            }
            list += suite;
        }
        if (SSL_CTX_set_cipher_list(ctx, list.c_str()) != 1) {
            throw coap_security_error("failed to apply the configured DTLS cipher suites: " + list);
        }
    }
}

/// Build the dtls_state for `security` and install it on `instance`.
///
/// Order matters: nyoci_plat_tls_set_context() must run before the PSK setters,
/// because those install their callbacks onto whichever SSL_CTX the instance is
/// holding at the time.
inline auto configure_dtls(nyoci_t instance, const coap_security_config& security,
                           coap_security_role role) -> std::unique_ptr<dtls_state> {
    auto state = std::make_unique<dtls_state>();
    state->ssl_ctx = make_dtls_context();

    if (security.mode == coap_auth_mode::dtls_pki) {
        if (!std::holds_alternative<pki_credentials>(security.credentials)) {
            throw coap_security_config_error(
                "security.mode == dtls_pki requires pki_credentials in security.credentials");
        }
        apply_pki_credentials(state->ssl_ctx, std::get<pki_credentials>(security.credentials),
                              role);
    }

    if (nyoci_plat_tls_set_context(instance, state->ssl_ctx) != NYOCI_STATUS_OK) {
        throw coap_security_error("libnyoci rejected the DTLS context");
    }

    if (security.mode == coap_auth_mode::dtls_psk) {
        if (!std::holds_alternative<psk_credentials>(security.credentials)) {
            throw coap_security_config_error(
                "security.mode == dtls_psk requires psk_credentials in security.credentials");
        }
        const auto& creds = std::get<psk_credentials>(security.credentials);
        if (creds.identity.empty() || creds.key.empty()) {
            throw coap_security_config_error(
                "dtls_psk requires a non-empty identity and key for the libnyoci backend");
        }
        state->psk_identity = creds.identity;
        state->psk_key.reserve(creds.key.size());
        for (const auto byte : creds.key) {
            state->psk_key.push_back(static_cast<unsigned char>(byte));
        }
        if (role == coap_security_role::server) {
            // The hint is what the client's callback sees; libnyoci keeps only
            // the pointer's contents at call time, so it lives in dtls_state.
            state->psk_hint = creds.identity;
            nyoci_plat_tls_set_psk_hint(instance, state->psk_hint.c_str());
            nyoci_plat_tls_set_server_psk_callback(instance, &libnyoci_server_psk_trampoline,
                                                   state.get());
        } else {
            nyoci_plat_tls_set_client_psk_callback(instance, &libnyoci_client_psk_trampoline,
                                                   state.get());
        }
    }

    return state;
}

/// A free UDP port, obtained by binding one and letting it go.
///
/// Needed because nyoci_plat_get_port() reads plat.fd_udp only, so a DTLS
/// listener bound to port 0 has no way to report where it landed. Racy in
/// principle; in practice the kernel does not hand the same ephemeral port out
/// twice in quick succession, and this only runs when the caller asked for "any
/// port" in the first place.
[[nodiscard]] inline auto reserve_ephemeral_port() -> std::uint16_t {
    const int fd = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        throw coap_network_error("failed to open a socket to reserve a DTLS port");
    }
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    std::uint16_t port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        socklen_t length = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length) == 0) {
            port = ntohs(addr.sin6_port);
        }
    }
    ::close(fd);
    if (port == 0) {
        throw coap_network_error("failed to reserve an ephemeral port for the DTLS listener");
    }
    return port;
}

#endif  // LIBNYOCI_AVAILABLE

}  // namespace libnyoci_detail

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
// Templated on Types alone and constrained by transport_types<Types>, exactly
// like the libcoap coap_client — so callers get the same Future backend and the
// same metrics_type regardless of CoAP library (Requirement 1.3).
template<typename Types>
requires kythira::transport_types<Types>
class coap_libnyoci_client {
public:
    template<typename T> using future_template = typename Types::template future_template<T>;
    // Not required by kythira::transport_types (see coap_client's own note on
    // the same alias): needed only to build a promise/future pair matching
    // future_template<T>'s actual backend.
    template<typename T> using promise_template = typename Types::template promise_template<T>;
    using serializer_type = typename Types::serializer_type;
    using serializer_registry_type = typename Types::serializer_registry_type;
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;

    coap_libnyoci_client(std::unordered_map<std::uint64_t, std::string> node_id_to_endpoint_map,
                         kythira::coap_client_config config, metrics_type metrics)
        : _registry{},
          _node_id_to_endpoint{std::move(node_id_to_endpoint_map)},
          _config{std::move(config)},
          _metrics{std::move(metrics)} {
        kythira::coap_utils::validate_registry_content_formats(_registry);
        const auto [selected_channel, security] = libnyoci_detail::plan_security(_config, "client");
        _secure = selected_channel == libnyoci_detail::channel::dtls;
#ifdef LIBNYOCI_AVAILABLE
        _instance = nyoci_create();
        if (_instance == nullptr) {
            throw coap_transport_error("failed to create libnyoci instance for CoAP client");
        }
        try {
            if (_secure) {
                // Must precede the bind: the PSK setters attach their callbacks
                // to whichever SSL_CTX the instance holds at the time.
                _dtls = libnyoci_detail::configure_dtls(_instance, security,
                                                        coap_security_role::client);
            }
            // Port 0: the client only ever originates requests, so any
            // ephemeral source port will do.
            const auto session_type = _secure ? NYOCI_SESSION_TYPE_DTLS : NYOCI_SESSION_TYPE_UDP;
            if (nyoci_plat_bind_to_port(_instance, session_type, 0) != NYOCI_STATUS_OK) {
                throw coap_network_error(std::string("failed to bind a ") +
                                         (_secure ? "DTLS" : "UDP") +
                                         " socket for the libnyoci CoAP client");
            }
            // nyoci_plat_get_port() reads plat.fd_udp only, so it says nothing
            // useful about a DTLS-only instance. The client's source port is of
            // no interest to callers either way.
            _bound_port = _secure ? 0 : nyoci_plat_get_port(_instance);
        } catch (...) {
            _dtls.reset();
            nyoci_release(_instance);
            _instance = nullptr;
            throw;
        }
        _event_thread = std::jthread([this](std::stop_token stop) { run_event_loop(stop); });
#endif
    }

    ~coap_libnyoci_client() {
#ifdef LIBNYOCI_AVAILABLE
        if (_event_thread.joinable()) {
            _event_thread.request_stop();
            _event_thread.join();
        }
        // Anything still queued never reached libnyoci, so the loop's own
        // shutdown sweep could not have rejected it (Requirement 6.4).
        reject_queued("client destroyed before the request was sent");
        if (_instance != nullptr) {
            nyoci_release(_instance);
            _instance = nullptr;
        }
        // After nyoci_release(), never before: libnyoci holds the SSL_CTX
        // pointer and tears its sessions down inside release.
        _dtls.reset();
#endif
    }

    coap_libnyoci_client(const coap_libnyoci_client&) = delete;
    auto operator=(const coap_libnyoci_client&) -> coap_libnyoci_client& = delete;
    // The C callbacks hold `this`, and the event thread captures it, so the
    // object's address has to be stable for its whole life.
    coap_libnyoci_client(coap_libnyoci_client&&) = delete;
    auto operator=(coap_libnyoci_client&&) -> coap_libnyoci_client& = delete;

    // --- network_client concept surface (matches network.hpp exactly) ---

    auto send_request_vote(std::uint64_t target, const kythira::request_vote_request<>& request,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
        -> future_template<kythira::request_vote_response<>> {
        return send_rpc<kythira::request_vote_request<>, kythira::request_vote_response<>>(
            target, libnyoci_request_vote_path, request, timeout);
    }

    auto send_append_entries(std::uint64_t target, const kythira::append_entries_request<>& request,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
        -> future_template<kythira::append_entries_response<>> {
        return send_rpc<kythira::append_entries_request<>, kythira::append_entries_response<>>(
            target, libnyoci_append_entries_path, request, timeout);
    }

    auto send_install_snapshot(std::uint64_t target,
                               const kythira::install_snapshot_request<>& request,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds{30000})
        -> future_template<kythira::install_snapshot_response<>> {
        return send_rpc<kythira::install_snapshot_request<>, kythira::install_snapshot_response<>>(
            target, libnyoci_install_snapshot_path, request, timeout);
    }

    /// The ephemeral UDP source port libnyoci bound. Exposed for tests; 0 when
    /// the backend is compiled out.
    [[nodiscard]] auto bound_port() const -> std::uint16_t { return _bound_port; }

    /// True when the backend was compiled with libnyoci present. Tests use this
    /// to skip rather than fail (Requirement 7.5).
    [[nodiscard]] static constexpr auto backend_available() -> bool {
#ifdef LIBNYOCI_AVAILABLE
        return true;
#else
        return false;
#endif
    }

private:
    // A single in-flight RPC. Owned by the event thread once started; the
    // resolve/reject callbacks are type-erased here so this struct does not
    // have to be templated on Request/Response.
    struct pending_rpc {
        std::string uri;
        std::string resource_path;
        std::uint64_t target{0};
        std::vector<std::byte> payload;
        std::string request_media_type;
        std::vector<std::uint16_t> accept_formats;
        std::chrono::milliseconds timeout{5000};
        bool confirmable{true};

        /// Receives the reassembled response body and the media type the peer
        /// declared, exactly like pending_message::resolve_callback.
        std::function<void(std::vector<std::byte>, const std::string&)> resolve_callback;
        std::function<void(std::exception_ptr)> reject_callback;
        /// Resolves a Content-Format number against *this client's* registry.
        /// Bound at send time because the response callback is a C trampoline
        /// that only ever sees this context, never the client object.
        std::function<std::optional<std::string>(std::uint16_t)> media_type_for_format;

        // Filled in on the event thread as blocks arrive.
        std::vector<std::byte> accumulated;
        std::optional<std::uint16_t> response_format;
        bool settled{false};
        bool finished{false};
#ifdef LIBNYOCI_AVAILABLE
        // Storage for the transaction, owned by this record rather than
        // malloc'd by nyoci_transaction_init(nullptr, ...). Passing our own
        // buffer leaves libnyoci's should_dealloc clear, so libnyoci never
        // frees it -- and, more to the point, there is nothing to leak if
        // nyoci_transaction_begin() fails partway through (it sets active=1
        // before the step that can fail, so the transaction is neither in the
        // instance's list nor safely end-able at that point).
        struct nyoci_transaction_s transaction_storage{};
        nyoci_transaction_t transaction{nullptr};
#endif
    };

    template<typename Request, typename Response>
    auto send_rpc(std::uint64_t target, const std::string& resource_path, const Request& request,
                  std::chrono::milliseconds timeout) -> future_template<Response> {
        auto promise = std::make_shared<promise_template<Response>>();
        auto future = promise->getFuture();

#ifdef LIBNYOCI_AVAILABLE
        try {
            const auto endpoint = _node_id_to_endpoint.find(target);
            if (endpoint == _node_id_to_endpoint.end()) {
                throw coap_network_error("no CoAP endpoint configured for node " +
                                         std::to_string(target));
            }

            // Same negotiation the libcoap client does: send in whatever this
            // peer last answered in when the registry still supports it, and
            // advertise everything we can decode on every request.
            const std::string request_media_type =
                kythira::select_request_media_type(_registry, _capability_cache, target);
            const auto request_format =
                kythira::coap_utils::media_type_to_coap_content_format(request_media_type);
            if (!request_format) {
                throw coap_unsupported_content_format_error(request_media_type);
            }

            auto rpc = std::make_unique<pending_rpc>();
            rpc->uri = libnyoci_detail::build_request_uri(endpoint->second, resource_path, _secure);
            rpc->resource_path = resource_path;
            rpc->target = target;
            rpc->payload = _registry.encode_with(request_media_type, request);
            rpc->request_media_type = request_media_type;
            rpc->timeout = timeout;
            rpc->confirmable = _config.use_confirmable_messages;
            for (const auto& accepted : _registry.preferred_media_types()) {
                if (const auto format =
                        kythira::coap_utils::media_type_to_coap_content_format(accepted)) {
                    rpc->accept_formats.push_back(static_cast<std::uint16_t>(*format));
                }
            }
            rpc->resolve_callback = [promise, this, target](std::vector<std::byte> body,
                                                            const std::string& media_type) {
                try {
                    Response response = _registry.template decode_with<Response>(media_type, body);
                    // Only recorded on a successful decode: caching a type we
                    // could not read would make every later call to this peer
                    // fail the same way.
                    _capability_cache.record(target, media_type);
                    promise->setValue(std::move(response));
                } catch (const std::exception& e) {
                    promise->setException(std::make_exception_ptr(coap_transport_error(
                        "failed to deserialize CoAP response: " + std::string(e.what()))));
                }
            };
            rpc->reject_callback = [promise](std::exception_ptr error) {
                promise->setException(error);
            };
            rpc->media_type_for_format = [this](std::uint16_t format) {
                return kythira::coap_utils::registry_media_type_for_content_format(
                    _registry, kythira::coap_utils::parse_content_format(format));
            };

            {
                const std::lock_guard lock(_mutex);
                if (_shutting_down) {
                    throw coap_transport_error("libnyoci CoAP client is shutting down");
                }
                _queued.push_back(std::move(rpc));
            }

            auto request_metric = _metrics;
            request_metric.set_metric_name("coap.client.request.sent");
            request_metric.add_dimension("resource_path", resource_path);
            request_metric.add_dimension("target_node_id", std::to_string(target));
            request_metric.add_dimension("media_type", request_media_type);
            request_metric.add_one();
            request_metric.emit();
        } catch (...) {
            promise->setException(std::current_exception());
        }
#else
        (void)target;
        (void)resource_path;
        (void)request;
        (void)timeout;
        promise->setException(std::make_exception_ptr(coap_transport_error(
            "libnyoci CoAP backend unavailable: rebuild with the vcpkg 'coap-libnyoci' feature so "
            "LIBNYOCI_AVAILABLE is defined")));
#endif
        return std::move(future);
    }

#ifdef LIBNYOCI_AVAILABLE
    // ---- event loop (the only thread that ever touches libnyoci) ----

    auto run_event_loop(std::stop_token stop) -> void {
        nyoci_set_current_instance(_instance);
        while (!stop.stop_requested()) {
            start_queued_transactions();
            nyoci_plat_wait(_instance, libnyoci_poll_interval_ms);
            nyoci_plat_process(_instance);
            reap_finished();
        }
        // Cancel everything still in flight so no future is left unresolved
        // (Requirement 6.4). nyoci_transaction_end() runs the transaction's
        // delete path, which calls back with NYOCI_STATUS_TRANSACTION_
        // INVALIDATED -- that is where the rejection happens.
        {
            const std::lock_guard lock(_mutex);
            _shutting_down = true;
        }
        for (auto& rpc : _live) {
            if (rpc && !rpc->finished && rpc->transaction != nullptr) {
                nyoci_transaction_end(_instance, rpc->transaction);
            }
        }
        for (auto& rpc : _live) {
            if (rpc && !rpc->settled) {
                rpc->settled = true;
                rpc->reject_callback(std::make_exception_ptr(
                    coap_transport_error("libnyoci CoAP client stopped with the request in "
                                         "flight: " +
                                         rpc->resource_path)));
            }
        }
        _live.clear();
    }

    auto start_queued_transactions() -> void {
        std::deque<std::unique_ptr<pending_rpc>> batch;
        {
            const std::lock_guard lock(_mutex);
            batch.swap(_queued);
        }
        for (auto& rpc : batch) {
            auto* raw = rpc.get();
            // NYOCI_TRANSACTION_ALWAYS_INVALIDATE is not optional decoration:
            // libnyoci only continues a Block2 sequence (nyoci-transaction.c's
            // "Preparing to request next block") when that flag is set, and it
            // is also what guarantees a final callback we can use to reject a
            // transaction that ends without a response. The cost is that the
            // callback can fire more than once, which is why every settle path
            // below is guarded by `settled`.
            raw->transaction = nyoci_transaction_init(
                &raw->transaction_storage, NYOCI_TRANSACTION_ALWAYS_INVALIDATE,
                &coap_libnyoci_client::on_resend_trampoline,
                &coap_libnyoci_client::on_response_trampoline, raw);
            _live.push_back(std::move(rpc));
            const auto status = nyoci_transaction_begin(
                _instance, raw->transaction, static_cast<nyoci_cms_t>(raw->timeout.count()));
            if (status != NYOCI_STATUS_OK && !raw->settled) {
                raw->settled = true;
                raw->finished = true;
                raw->reject_callback(
                    libnyoci_detail::exception_for_status(status, "failed to start CoAP request"));
            }
        }
    }

    // Destroy contexts only here, on the event thread and *outside* any
    // libnyoci callback, so nothing frees a context libnyoci is still walking.
    auto reap_finished() -> void {
        std::erase_if(
            _live, [](const std::unique_ptr<pending_rpc>& rpc) { return !rpc || rpc->finished; });
    }

    auto reject_queued(const std::string& reason) -> void {
        std::deque<std::unique_ptr<pending_rpc>> batch;
        {
            const std::lock_guard lock(_mutex);
            _shutting_down = true;
            batch.swap(_queued);
        }
        for (auto& rpc : batch) {
            if (rpc && !rpc->settled) {
                rpc->settled = true;
                rpc->reject_callback(std::make_exception_ptr(coap_transport_error(reason)));
            }
        }
    }

    // ---- C trampolines ----

    /// Builds (and, on retransmission, rebuilds) the outbound POST. libnyoci
    /// calls this once per transmission attempt, which is exactly why the
    /// request body is kept in the context rather than built once up front.
    static auto on_resend_trampoline(void* context) -> nyoci_status_t {
        auto* rpc = static_cast<pending_rpc*>(context);
        nyoci_status_t status = nyoci_outbound_begin(
            nyoci_get_current_instance(), COAP_METHOD_POST,
            rpc->confirmable ? COAP_TRANS_TYPE_CONFIRMABLE : COAP_TRANS_TYPE_NONCONFIRMABLE);
        if (status != NYOCI_STATUS_OK) {
            return status;
        }
        // Emits Uri-Host/Uri-Port/Uri-Path (options 3/7/11). Everything added
        // afterwards has a higher option number, which is what libnyoci's
        // strictly-increasing option writer requires. Note that libnyoci adds
        // the Block2 option (23) itself while walking a block sequence, and it
        // does so late -- nyoci_outbound_get_content_ptr() flushes every
        // still-pending option before handing over the content buffer, which is
        // why appending the body below is what actually emits it, and why the
        // options here (12, 17) may be written before it despite being lower.
        status = nyoci_outbound_set_uri(rpc->uri.c_str(), 0);
        if (status != NYOCI_STATUS_OK) {
            return status;
        }
        status = libnyoci_detail::add_content_format_option(
            COAP_OPTION_CONTENT_TYPE,
            static_cast<std::uint16_t>(
                *kythira::coap_utils::media_type_to_coap_content_format(rpc->request_media_type)));
        if (status != NYOCI_STATUS_OK) {
            return status;
        }
        for (const auto format : rpc->accept_formats) {
            status = libnyoci_detail::add_content_format_option(COAP_OPTION_ACCEPT, format);
            if (status != NYOCI_STATUS_OK) {
                return status;
            }
        }
        // libnyoci has no Block1, so an over-large body cannot be split.
        // Checking the real remaining space (rather than a compile-time
        // constant) keeps this correct whatever NYOCI_MAX_CONTENT_LENGTH the
        // library was built with.
        if (rpc->payload.size() > nyoci_outbound_get_space_remaining()) {
            return NYOCI_STATUS_MESSAGE_TOO_BIG;
        }
        status = nyoci_outbound_append_content(reinterpret_cast<const char*>(rpc->payload.data()),
                                               static_cast<coap_size_t>(rpc->payload.size()));
        if (status != NYOCI_STATUS_OK) {
            return status;
        }
        return nyoci_outbound_send();
    }

    /// Correlation is by context pointer: libnyoci hands back exactly the
    /// transaction's own context, and it only does so after matching the
    /// response's message-id/token *and* remote address, so exactly one
    /// pending future is ever resolved by a given response and duplicates are
    /// suppressed before they reach us (Requirements 4.2, 4.4).
    static auto on_response_trampoline(int statuscode, void* context) -> nyoci_status_t {
        auto* rpc = static_cast<pending_rpc*>(context);

        if (statuscode == NYOCI_STATUS_TRANSACTION_INVALIDATED) {
            // Always the last callback for the transaction. If a response
            // already settled it this is just the teardown notification;
            // otherwise the transaction died without one.
            if (!rpc->settled) {
                rpc->settled = true;
                rpc->reject_callback(libnyoci_detail::exception_for_status(
                    statuscode, "CoAP request to " + rpc->uri));
            }
            rpc->finished = true;
            return NYOCI_STATUS_OK;
        }

        if (statuscode < 0 || statuscode >= COAP_RESULT_400) {
            if (!rpc->settled) {
                rpc->settled = true;
                rpc->reject_callback(libnyoci_detail::exception_for_status(
                    statuscode, "CoAP request to " + rpc->uri));
            }
            return NYOCI_STATUS_OK;
        }

        const auto options = libnyoci_detail::scan_inbound_options();
        if (!rpc->response_format.has_value() && options.content_format.has_value()) {
            rpc->response_format = options.content_format;
        }

        const coap_size_t length = nyoci_inbound_get_content_len();
        const char* content = nyoci_inbound_get_content_ptr();
        if (content != nullptr && length > 0) {
            const auto* bytes = reinterpret_cast<const std::byte*>(content);
            rpc->accumulated.insert(rpc->accumulated.end(), bytes, bytes + length);
        }

        // RFC 7959's "more" bit. Absent Block2 means the whole body arrived in
        // this one response; libnyoci drives the follow-up requests itself.
        const bool more = options.block2.has_value() && ((*options.block2 & 0x8U) != 0);
        if (more) {
            return NYOCI_STATUS_OK;
        }

        if (rpc->settled) {
            return NYOCI_STATUS_OK;
        }
        rpc->settled = true;

        // Which media type to decode as. A response that carries no
        // Content-Format is decoded as whatever we asked in -- the peer echoing
        // our own encoding is the only reading that does not amount to guessing.
        // A Content-Format this registry has no serializer for is a rejection,
        // not a silent decode with the wrong one.
        std::string response_media_type = rpc->request_media_type;
        if (rpc->response_format.has_value()) {
            const auto resolved = rpc->media_type_for_format(*rpc->response_format);
            if (!resolved) {
                rpc->reject_callback(std::make_exception_ptr(coap_unsupported_content_format_error(
                    "CoAP Content-Format " + std::to_string(*rpc->response_format) +
                    " (response from " + rpc->uri + ")")));
                return NYOCI_STATUS_OK;
            }
            response_media_type = *resolved;
        }
        rpc->resolve_callback(std::move(rpc->accumulated), response_media_type);
        return NYOCI_STATUS_OK;
    }
#endif  // LIBNYOCI_AVAILABLE

    /// See coap_client's own note: retained for parity with the libcoap
    /// backend's field set. Every negotiated request/response goes through
    /// `_registry`.
    serializer_type _serializer;
    serializer_registry_type _registry;
    peer_capability_cache<std::uint64_t> _capability_cache;
    std::unordered_map<std::uint64_t, std::string> _node_id_to_endpoint;
    kythira::coap_client_config _config;
    metrics_type _metrics;

    mutable std::mutex _mutex;
    std::deque<std::unique_ptr<pending_rpc>> _queued;
    bool _shutting_down{false};
    std::uint16_t _bound_port{0};
    /// True when plan_security() chose DTLS. Decides the socket type, and the
    /// URI scheme every request is built with.
    bool _secure{false};

#ifdef LIBNYOCI_AVAILABLE
    // Outlives _instance's use of it; destroyed after nyoci_release().
    std::unique_ptr<libnyoci_detail::dtls_state> _dtls;
    nyoci_t _instance{nullptr};
    // Touched only by the event thread, so it needs no lock.
    std::vector<std::unique_ptr<pending_rpc>> _live;
    std::jthread _event_thread;
#endif
};

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
template<typename Types>
requires kythira::transport_types<Types>
class coap_libnyoci_server {
public:
    template<typename T> using future_template = typename Types::template future_template<T>;
    using serializer_type = typename Types::serializer_type;
    using serializer_registry_type = typename Types::serializer_registry_type;
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;
    using address_type = std::string;
    using port_type = std::uint16_t;

    coap_libnyoci_server(std::string bind_address, std::uint16_t bind_port,
                         kythira::coap_server_config config, metrics_type metrics)
        : _registry{},
          _bind_address{std::move(bind_address)},
          _bind_port{bind_port},
          _actual_bound_port{bind_port},
          _config{std::move(config)},
          _metrics{std::move(metrics)} {
        kythira::coap_utils::validate_registry_content_formats(_registry);
        // Resolved (and refused, where it must be) at construction rather than
        // in start(), so a config this backend cannot honour fails as early as
        // it does on the libcoap side.
        auto [selected_channel, security] = libnyoci_detail::plan_security(_config, "server");
        _secure = selected_channel == libnyoci_detail::channel::dtls;
        _security = std::move(security);
    }

    ~coap_libnyoci_server() { stop(); }

    coap_libnyoci_server(const coap_libnyoci_server&) = delete;
    auto operator=(const coap_libnyoci_server&) -> coap_libnyoci_server& = delete;
    coap_libnyoci_server(coap_libnyoci_server&&) = delete;
    auto operator=(coap_libnyoci_server&&) -> coap_libnyoci_server& = delete;

    // --- network_server concept surface ---

    auto register_request_vote_handler(
        std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)>
            handler) -> void {
        const std::lock_guard lock(_mutex);
        _request_vote_handler = std::move(handler);
    }

    auto register_append_entries_handler(
        std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)>
            handler) -> void {
        const std::lock_guard lock(_mutex);
        _append_entries_handler = std::move(handler);
    }

    auto register_install_snapshot_handler(std::function<kythira::install_snapshot_response<>(
                                               const kythira::install_snapshot_request<>&)>
                                               handler) -> void {
        const std::lock_guard lock(_mutex);
        _install_snapshot_handler = std::move(handler);
    }

    auto start() -> void {
        if (_running.load()) {
            return;
        }
#ifdef LIBNYOCI_AVAILABLE
        _instance = nyoci_create();
        if (_instance == nullptr) {
            throw coap_transport_error("failed to create libnyoci instance for CoAP server");
        }
        try {
            if (_secure) {
                // Must precede the bind: the PSK setters attach their callbacks
                // to whichever SSL_CTX the instance holds at the time.
                _dtls = libnyoci_detail::configure_dtls(_instance, _security,
                                                        coap_security_role::server);
            }
            // libnyoci's POSIX backend binds one AF_INET6 socket per instance
            // and accepts v4-mapped peers through it; it exposes a port but not
            // a per-address bind, so _bind_address is recorded for parity with
            // the libcoap server and for diagnostics rather than narrowing the
            // bind.
            auto port = _bind_port;
            if (_secure && port == 0) {
                // nyoci_plat_get_port() reads plat.fd_udp only, so a DTLS
                // listener bound to port 0 could never report where it landed.
                // Pick the port up front so bound_port() stays meaningful.
                port = libnyoci_detail::reserve_ephemeral_port();
            }
            const auto session_type = _secure ? NYOCI_SESSION_TYPE_DTLS : NYOCI_SESSION_TYPE_UDP;
            if (nyoci_plat_bind_to_port(_instance, session_type, port) != NYOCI_STATUS_OK) {
                throw coap_network_error(std::string("failed to bind libnyoci CoAP server ") +
                                         (_secure ? "DTLS" : "UDP") + " socket to port " +
                                         std::to_string(port));
            }
            // Non-zero only after the bind, which is the point of bound_port():
            // constructing with port 0 means "any free port".
            _actual_bound_port = _secure ? port : nyoci_plat_get_port(_instance);
        } catch (...) {
            _dtls.reset();
            nyoci_release(_instance);
            _instance = nullptr;
            throw;
        }
        nyoci_set_default_request_handler(_instance, &coap_libnyoci_server::on_request_trampoline,
                                          this);
        _running.store(true);
        _event_thread = std::jthread([this](std::stop_token stop) {
            nyoci_set_current_instance(_instance);
            while (!stop.stop_requested()) {
                nyoci_plat_wait(_instance, libnyoci_poll_interval_ms);
                nyoci_plat_process(_instance);
            }
        });
#else
        throw coap_transport_error(
            "libnyoci CoAP backend unavailable: rebuild with the vcpkg 'coap-libnyoci' feature so "
            "LIBNYOCI_AVAILABLE is defined");
#endif
    }

    auto stop() -> void {
#ifdef LIBNYOCI_AVAILABLE
        if (_event_thread.joinable()) {
            _event_thread.request_stop();
            _event_thread.join();
        }
        if (_instance != nullptr) {
            // Closes the socket and ends every transaction the instance owns.
            nyoci_release(_instance);
            _instance = nullptr;
        }
        // After nyoci_release(), never before: libnyoci holds the SSL_CTX
        // pointer and tears its DTLS sessions down inside release. Cleared so a
        // restart builds a fresh context rather than reusing a torn-down one.
        _dtls.reset();
#endif
        _running.store(false);
    }

    [[nodiscard]] auto is_running() const -> bool { return _running.load(); }

    /// The UDP port actually bound by start(). Differs from the constructor's
    /// port when that was 0 (ephemeral). Before start() it reports the
    /// requested port unchanged, matching coap_server::bound_port().
    [[nodiscard]] auto bound_port() const -> port_type { return _actual_bound_port; }

    [[nodiscard]] static constexpr auto backend_available() -> bool {
#ifdef LIBNYOCI_AVAILABLE
        return true;
#else
        return false;
#endif
    }

private:
#ifdef LIBNYOCI_AVAILABLE
    static auto on_request_trampoline(void* context) -> nyoci_status_t {
        return static_cast<coap_libnyoci_server*>(context)->handle_request();
    }

    /// Runs on the event thread, inside libnyoci's request dispatch. Every
    /// nyoci_inbound_*/nyoci_outbound_* call below is only legal here.
    auto handle_request() -> nyoci_status_t {
        if (nyoci_inbound_get_code() != COAP_METHOD_POST) {
            // libnyoci turns this into a 4.05 for us.
            return NYOCI_STATUS_NOT_ALLOWED;
        }

        std::array<char, 256> path_buffer{};
        nyoci_inbound_get_path(path_buffer.data(), path_buffer.size(),
                               NYOCI_GET_PATH_LEADING_SLASH);
        const std::string resource_path{path_buffer.data()};

        const auto options = libnyoci_detail::scan_inbound_options();

        const coap_size_t length = nyoci_inbound_get_content_len();
        const char* content = nyoci_inbound_get_content_ptr();
        if (length > _config.max_request_size) {
            return NYOCI_STATUS_MESSAGE_TOO_BIG;
        }
        std::vector<std::byte> body;
        if (content != nullptr && length > 0) {
            const auto* bytes = reinterpret_cast<const std::byte*>(content);
            body.assign(bytes, bytes + length);
        }

        // Decode in whatever the peer said it sent; a Content-Format this
        // registry cannot decode is 4.15, exactly as the libcoap server
        // answers.
        std::string request_media_type = _registry.default_media_type();
        if (options.content_format.has_value()) {
            const auto resolved = kythira::coap_utils::registry_media_type_for_content_format(
                _registry, kythira::coap_utils::parse_content_format(*options.content_format));
            if (!resolved) {
                return NYOCI_STATUS_UNSUPPORTED_MEDIA_TYPE;
            }
            request_media_type = *resolved;
        }

        // Pick the response encoding from the peer's Accept list. An Accept
        // list we cannot satisfy is 4.06 Not Acceptable rather than a silent
        // fall back to our default -- the same answer the libcoap server gives.
        std::string response_media_type = request_media_type;
        if (!options.accepted_formats.empty()) {
            std::vector<std::string> accepted;
            accepted.reserve(options.accepted_formats.size());
            for (const auto format : options.accepted_formats) {
                if (const auto media_type =
                        kythira::coap_utils::registry_media_type_for_content_format(
                            _registry, kythira::coap_utils::parse_content_format(format))) {
                    accepted.push_back(*media_type);
                }
            }
            const auto selected = _registry.select_output_media_type(accepted);
            if (!selected) {
                return respond_not_acceptable();
            }
            response_media_type = *selected;
        }

        try {
            if (resource_path == libnyoci_request_vote_path) {
                return dispatch<kythira::request_vote_request<>, kythira::request_vote_response<>>(
                    copy_handler(_request_vote_handler), body, request_media_type,
                    response_media_type, options);
            }
            if (resource_path == libnyoci_append_entries_path) {
                return dispatch<kythira::append_entries_request<>,
                                kythira::append_entries_response<>>(
                    copy_handler(_append_entries_handler), body, request_media_type,
                    response_media_type, options);
            }
            if (resource_path == libnyoci_install_snapshot_path) {
                return dispatch<kythira::install_snapshot_request<>,
                                kythira::install_snapshot_response<>>(
                    copy_handler(_install_snapshot_handler), body, request_media_type,
                    response_media_type, options);
            }
        } catch (const std::exception&) {
            // A handler that threw, or a body this registry could not decode.
            // Dropping the response instead would leave the peer retransmitting
            // until its own timeout.
            return NYOCI_STATUS_FAILURE;
        }
        return NYOCI_STATUS_NOT_FOUND;
    }

    template<typename Handler> auto copy_handler(const Handler& handler) -> Handler {
        // Copied under the lock and invoked outside it, so a handler that
        // blocks cannot stall register_*_handler() (Requirement 6.3).
        const std::lock_guard lock(_mutex);
        return handler;
    }

    template<typename Request, typename Response, typename Handler>
    auto dispatch(Handler handler, const std::vector<std::byte>& body,
                  const std::string& request_media_type, const std::string& response_media_type,
                  const libnyoci_detail::inbound_options& options) -> nyoci_status_t {
        if (!handler) {
            return NYOCI_STATUS_NOT_IMPLEMENTED;
        }
        const Request request = _registry.template decode_with<Request>(request_media_type, body);
        const Response response = handler(request);
        const std::vector<std::byte> encoded = _registry.encode_with(response_media_type, response);
        return send_response(encoded, response_media_type, options);
    }

    auto respond_not_acceptable() -> nyoci_status_t {
        const auto status = nyoci_outbound_begin_response(COAP_RESULT_406_NOT_ACCEPTABLE);
        if (status != NYOCI_STATUS_OK) {
            return status;
        }
        return nyoci_outbound_send();
    }

    /// Emit a 2.05 carrying `payload`, slicing it into Block2 blocks when it
    /// does not fit in one datagram.
    ///
    /// libnyoci gives the *client* side of Block2 for free (its transaction
    /// layer requests the next block itself), but nothing on the server side:
    /// producing the blocks is ours to do. The block number comes from the
    /// request's own Block2 option, so this stays stateless -- no per-peer
    /// transfer state to expire, and a lost block just gets re-requested.
    auto send_response(const std::vector<std::byte>& payload, const std::string& media_type,
                       const libnyoci_detail::inbound_options& options) -> nyoci_status_t {
        const auto format = kythira::coap_utils::media_type_to_coap_content_format(media_type);
        if (!format) {
            return NYOCI_STATUS_UNSUPPORTED_MEDIA_TYPE;
        }

        nyoci_status_t status = nyoci_outbound_begin_response(COAP_RESULT_205_CONTENT);
        if (status != NYOCI_STATUS_OK) {
            return status;
        }
        status = libnyoci_detail::add_content_format_option(COAP_OPTION_CONTENT_TYPE,
                                                            static_cast<std::uint16_t>(*format));
        if (status != NYOCI_STATUS_OK) {
            return status;
        }

        // The block size to answer in. A client that already asked for a
        // specific block dictates it (RFC 7959: the server may shrink szx but
        // must not grow it mid-transfer, and shrinking is handled below);
        // otherwise the server's own configured maximum applies. szx caps at 6
        // (1024 bytes), the largest block RFC 7959 defines.
        std::uint8_t szx = 6;
        std::uint32_t block_number = 0;
        if (options.block2.has_value()) {
            szx = static_cast<std::uint8_t>(*options.block2 & 0x7U);
            block_number = *options.block2 >> 4U;
        } else if (_config.enable_block_transfer &&
                   kythira::coap_utils::is_valid_block_size(_config.max_block_size)) {
            szx = kythira::coap_utils::calculate_block_size_szx(_config.max_block_size);
        }

        // Whatever is left after the options, minus room for the Block2 option
        // we may still add. Derived from the live packet rather than a
        // compile-time constant, so it holds for whatever
        // NYOCI_MAX_CONTENT_LENGTH the library happened to be built with.
        constexpr coap_size_t block2_option_reserve = 8;
        const coap_size_t space = nyoci_outbound_get_space_remaining();
        const std::size_t usable =
            space > block2_option_reserve ? space - block2_option_reserve : 0;

        std::size_t block_size = kythira::coap_utils::szx_to_block_size(szx);
        while (block_size > usable && szx > 0) {
            --szx;
            block_size = kythira::coap_utils::szx_to_block_size(szx);
        }

        // Send whole when it fits in one block and the peer did not open a
        // block-wise exchange. Once either is false every response in the
        // exchange carries a Block2 option, including the last.
        if (!options.block2.has_value() && payload.size() <= block_size) {
            return append_and_send(payload.data(), payload.size());
        }

        const std::size_t offset = static_cast<std::size_t>(block_number) * block_size;
        if (offset >= payload.size() && offset != 0) {
            // The peer asked for a block past the end of the body.
            return NYOCI_STATUS_NOT_FOUND;
        }
        const std::size_t chunk = std::min(block_size, payload.size() - offset);
        const bool more = (offset + chunk) < payload.size();

        status = libnyoci_detail::add_block2_option(block_number, more, szx);
        if (status != NYOCI_STATUS_OK) {
            return status;
        }
        return append_and_send(payload.data() + offset, chunk);
    }

    static auto append_and_send(const std::byte* data, std::size_t length) -> nyoci_status_t {
        const auto status = nyoci_outbound_append_content(reinterpret_cast<const char*>(data),
                                                          static_cast<coap_size_t>(length));
        if (status != NYOCI_STATUS_OK) {
            return status;
        }
        return nyoci_outbound_send();
    }
#endif  // LIBNYOCI_AVAILABLE

    serializer_type _serializer;
    serializer_registry_type _registry;
    address_type _bind_address;
    port_type _bind_port;
    port_type _actual_bound_port;
    kythira::coap_server_config _config;
    metrics_type _metrics;
    /// Resolved once at construction (so an unsupportable mode is refused
    /// there), and re-applied on every start() — a restart rebuilds the DTLS
    /// context from it.
    coap_security_config _security{};
    bool _secure{false};

    std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)>
        _request_vote_handler;
    std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)>
        _append_entries_handler;
    std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)>
        _install_snapshot_handler;

    mutable std::mutex _mutex;
    std::atomic<bool> _running{false};

#ifdef LIBNYOCI_AVAILABLE
    nyoci_t _instance{nullptr};
    // Outlives _instance's use of it; destroyed after nyoci_release().
    std::unique_ptr<libnyoci_detail::dtls_state> _dtls;
    std::jthread _event_thread;
#endif
};

}  // namespace kythira
