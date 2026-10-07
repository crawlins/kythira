// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file coap_cantcoap_dtls.hpp
/// @brief DTLS 1.2 for the cantcoap CoAP backend, run over that backend's own
///        UDP socket (.kiro/specs/coap-transport-cantcoap/ Requirement 6.1).
///
/// WHY THIS EXISTS
/// ---------------
/// cantcoap is a message codec and nothing else, so there is no DTLS for this
/// backend to switch on: the libcoap backend gets DTLS from libcoap, and the
/// libnyoci backend from libnyoci's OpenSSL plugin. `coap_security_provider`
/// cannot be reused either -- its whole interface is libcoap types, and this
/// backend cannot include a libcoap header (see coap_transport_cantcoap_impl.hpp).
///
/// What the backend *does* own is every datagram on both sides of its socket,
/// which is exactly the seam DTLS needs. So this file sits between the socket
/// and the CoAP message layer: ciphertext in from recvfrom(), plaintext CoAP
/// datagrams out to the existing parser; plaintext CoAP datagrams in from the
/// message layer, records out through sendto(). Everything above it --
/// retransmission, duplicate suppression, block-wise -- is unchanged, which is
/// RFC 7252 Section 9.1's layering: CoAP's own reliability runs *over* DTLS.
///
/// HOW OPENSSL IS DRIVEN
/// ---------------------
/// One `SSL` per peer, each over a small custom BIO rather than a socket BIO or
/// a memory BIO:
///
/// - A socket BIO would want to own the socket, and there is only one socket
///   per client or server, shared by every peer.
/// - A memory BIO (`BIO_s_mem`) is a byte stream, so a handshake flight that
///   OpenSSL writes as several datagrams comes out concatenated, and the
///   datagram-preserving `BIO_s_dgram_mem` only arrived in OpenSSL 3.2.
///
/// The custom BIO's write is one sendto() to that session's peer, and its read
/// pops one queued datagram -- so record boundaries are datagram boundaries in
/// both directions, as DTLS requires, on every OpenSSL 3.x. This is also how
/// libcoap drives OpenSSL.
///
/// The handshake's own retransmission is OpenSSL's (`DTLSv1_handle_timeout`),
/// ticked from the backend's existing poll loop, so this adds no thread.
///
/// WHAT IS AND IS NOT PROVIDED
/// ---------------------------
/// - dtls_psk: identity and key from `psk_credentials`; the server answers
///   only the one identity it was configured with.
/// - dtls_pki: certificate chain, private key and CA from `pki_credentials`,
///   `verify_peer_cert` mapped to `SSL_VERIFY_PEER` (and, on a server, to
///   requiring a client certificate), `cipher_suites` to the cipher list, and
///   `revocation` then `cn_validator` run on the peer's certificate once the
///   handshake is done, before the session carries any CoAP message. Peers are addressed by IP, so
///   there is no hostname check; that is what `cn_validator` is for.
/// - dtls_rpk: RFC 7250 raw public keys, which OpenSSL only grew in 3.2. On an
///   older OpenSSL it is refused at construction, naming the version.
/// - The server answers every ClientHello with a HelloVerifyRequest cookie
///   (RFC 6347 Section 4.2.1) keyed on the peer's address, so it cannot be
///   used as an amplifier by a spoofed source. Per-peer state is still
///   allocated for the first ClientHello, so the session table is capped and
///   the least recently used session is evicted past the cap.
///
/// THREADING
/// ---------
/// None of its own. Every call happens on the owning backend's loop thread,
/// which is also the only thread that touches the socket.

#include <raft/coap_exceptions.hpp>
#include <raft/coap_dtls_cipher_suites.hpp>
#include <raft/coap_security.hpp>
#include <raft/coap_revocation.hpp>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/opensslv.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace kythira::cantcoap_detail {

/// A peer's socket address, of either family.
///
/// The backend prefers one AF_INET6 socket that also carries v4 peers
/// v4-mapped, but falls back to AF_INET on a kernel with no IPv6 at all, so the
/// address type has to hold both.
struct peer_address {
    sockaddr_storage storage{};
    socklen_t length{0};

    [[nodiscard]] auto as_sockaddr() const -> const sockaddr* {
        return reinterpret_cast<const sockaddr*>(&storage);
    }

    /// The bytes that identify this peer: family, address and port, without
    /// the padding and the IPv6 flow label that would make two datagrams from
    /// the same peer compare unequal.
    [[nodiscard]] auto key() const -> std::string {
        std::string out;
        if (storage.ss_family == AF_INET6) {
            const auto& v6 = reinterpret_cast<const sockaddr_in6&>(storage);
            out.push_back('6');
            out.append(reinterpret_cast<const char*>(&v6.sin6_addr), sizeof(v6.sin6_addr));
            out.append(reinterpret_cast<const char*>(&v6.sin6_port), sizeof(v6.sin6_port));
            out.append(reinterpret_cast<const char*>(&v6.sin6_scope_id), sizeof(v6.sin6_scope_id));
        } else if (storage.ss_family == AF_INET) {
            const auto& v4 = reinterpret_cast<const sockaddr_in&>(storage);
            out.push_back('4');
            out.append(reinterpret_cast<const char*>(&v4.sin_addr), sizeof(v4.sin_addr));
            out.append(reinterpret_cast<const char*>(&v4.sin_port), sizeof(v4.sin_port));
        }
        return out;
    }

    /// "127.0.0.1:5684" or "[::1]:5684", for error messages.
    [[nodiscard]] auto to_string() const -> std::string {
        std::array<char, INET6_ADDRSTRLEN> text{};
        if (storage.ss_family == AF_INET6) {
            const auto& v6 = reinterpret_cast<const sockaddr_in6&>(storage);
            ::inet_ntop(AF_INET6, &v6.sin6_addr, text.data(), text.size());
            return "[" + std::string{text.data()} + "]:" + std::to_string(ntohs(v6.sin6_port));
        }
        const auto& v4 = reinterpret_cast<const sockaddr_in&>(storage);
        ::inet_ntop(AF_INET, &v4.sin_addr, text.data(), text.size());
        return std::string{text.data()} + ":" + std::to_string(ntohs(v4.sin_port));
    }
};

/// Upper bound on concurrent DTLS sessions a server keeps. A Raft cluster has a
/// handful of peers; the cap exists so a flood of ClientHellos from distinct
/// spoofed sources costs a bounded amount of memory.
inline constexpr std::size_t dtls_max_sessions = 256;

/// How long a handshake may take, end to end, before it is abandoned. OpenSSL
/// retransmits a lost flight on its own doubling schedule (1 s, 2 s, 4 s ...);
/// this bounds the total so a peer that never answers fails the exchange
/// instead of holding it until the RPC's own timeout.
inline constexpr std::chrono::seconds dtls_handshake_timeout{30};

/// The link MTU handed to OpenSSL. Every record must fit one datagram the
/// backend can read (cantcoap_max_datagram, 1500), and the largest plaintext
/// it sends is one 1024-byte block plus CoAP headers, which fits with room for
/// the record overhead.
inline constexpr long dtls_link_mtu = 1400;

/// Pull the most recent OpenSSL error off the thread's queue as text, and clear
/// the rest so a later failure does not report this one.
[[nodiscard]] inline auto openssl_error_text(const char* fallback) -> std::string {
    const unsigned long code = ERR_get_error();
    ERR_clear_error();
    if (code == 0) {
        return fallback;
    }
    std::array<char, 256> text{};
    ERR_error_string_n(code, text.data(), text.size());
    return text.data();
}

class dtls_layer {
public:
    /// Writes one datagram to the socket. Supplied by the backend, which owns
    /// the socket.
    using send_function =
        std::function<void(const peer_address&, const std::uint8_t*, std::size_t)>;

    /// Builds the `SSL_CTX` for `config.mode` and loads every credential up
    /// front, so bad key material fails here rather than on the first
    /// handshake. Throws coap_security_config_error when the credentials are
    /// the wrong variant for the mode, and coap_security_error when the
    /// material itself cannot be used.
    dtls_layer(const coap_security_config& config, coap_security_role role, send_function send)
        : _role(role), _mode(config.mode), _send(std::move(send)) {
        if (RAND_bytes(_cookie_secret.data(), static_cast<int>(_cookie_secret.size())) != 1) {
            throw coap_security_error("failed to generate the DTLS cookie secret");
        }
        _ctx.reset(SSL_CTX_new(DTLS_method()));
        if (!_ctx) {
            throw coap_security_error("failed to create the DTLS context: " +
                                      openssl_error_text("SSL_CTX_new failed"));
        }
        SSL_CTX_set_min_proto_version(_ctx.get(), DTLS1_2_VERSION);
        SSL_CTX_set_app_data(_ctx.get(), this);
        // Partial writes would split one CoAP message across two records,
        // which the peer would read as two messages.
        SSL_CTX_clear_mode(_ctx.get(), SSL_MODE_ENABLE_PARTIAL_WRITE);
        // Replaces OpenSSL's DEFAULT list; configure_psk() narrows it to PSK
        // suites and configure_pki() to the configured cipher_suites, if any.
        detail::apply_default_dtls_cipher_list(_ctx.get(), detail::default_dtls_cert_cipher_list);

        switch (config.mode) {
            case coap_auth_mode::dtls_psk:
                configure_psk(credentials_as<psk_credentials>(config));
                break;
            case coap_auth_mode::dtls_pki:
                configure_pki(credentials_as<pki_credentials>(config));
                break;
            case coap_auth_mode::dtls_rpk:
                configure_rpk(credentials_as<rpk_credentials>(config));
                break;
            default:
                throw coap_security_config_error("dtls_layer constructed for a non-DTLS mode: " +
                                                 kythira::to_string(config.mode));
        }

        if (_role == coap_security_role::server) {
            SSL_CTX_set_cookie_generate_cb(_ctx.get(), &dtls_layer::generate_cookie);
            SSL_CTX_set_cookie_verify_cb(_ctx.get(), &dtls_layer::verify_cookie);
        }

        _method.reset(
            BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "kythira-cantcoap-dtls"));
        if (!_method) {
            throw coap_security_error("failed to create the DTLS datagram BIO method");
        }
        BIO_meth_set_write(_method.get(), &dtls_layer::bio_write);
        BIO_meth_set_read(_method.get(), &dtls_layer::bio_read);
        BIO_meth_set_ctrl(_method.get(), &dtls_layer::bio_ctrl);
        BIO_meth_set_create(_method.get(), [](BIO* bio) {
            BIO_set_init(bio, 1);
            return 1;
        });
        BIO_meth_set_destroy(_method.get(), [](BIO*) { return 1; });
    }

    dtls_layer(const dtls_layer&) = delete;
    auto operator=(const dtls_layer&) -> dtls_layer& = delete;
    dtls_layer(dtls_layer&&) = delete;
    auto operator=(dtls_layer&&) -> dtls_layer& = delete;

    /// Sessions are torn down silently here; call close_all() first while the
    /// socket is still open if the peers should be told.
    ~dtls_layer() = default;

    /// Encrypt and send one plaintext CoAP datagram to `peer`.
    ///
    /// A client with no session to `peer` starts the handshake and queues the
    /// datagram until it completes. A server never initiates, so a datagram to
    /// a peer it has no established session with is dropped -- which can only
    /// happen when that session was evicted, and the client's retransmission
    /// recovers it.
    auto send(const peer_address& peer, const std::vector<std::byte>& plain) -> void {
        auto* session = find(peer);
        if (session == nullptr) {
            if (_role == coap_security_role::server) {
                return;
            }
            session = &open_session(peer);
            drive_handshake(*session);
            session = find(peer);  // a handshake can fail synchronously
            if (session == nullptr) {
                return;
            }
        }
        if (!session->established) {
            // CoAP retransmits the identical datagram while the handshake is
            // still running; queue it once, not once per attempt.
            if (std::find(session->queued.begin(), session->queued.end(), plain) ==
                session->queued.end()) {
                session->queued.push_back(plain);
            }
            return;
        }
        write_record(*session, plain);
    }

    /// Feed one received datagram through `peer`'s session and return the
    /// plaintext CoAP datagrams it yielded -- none while a handshake is still
    /// running, and none for a record that fails to authenticate.
    [[nodiscard]] auto receive(const peer_address& peer, const std::uint8_t* data,
                               std::size_t length) -> std::vector<std::vector<std::byte>> {
        std::vector<std::vector<std::byte>> plaintext;
        auto* session = find(peer);
        if (session == nullptr) {
            // Only a server takes a session from the wire, and only for a
            // ClientHello: anything else from an unknown peer is a stale record
            // from a session this side no longer has (it restarted, or evicted
            // it), and answering it would only invite more.
            if (_role != coap_security_role::server || !is_client_hello(data, length)) {
                return plaintext;
            }
            evict_if_full();
            session = &open_session(peer);
        }
        session->last_activity = std::chrono::steady_clock::now();
        session->inbound.emplace_back(data, data + length);

        if (!session->established) {
            if (!drive_handshake(*session)) {
                return plaintext;
            }
            session = find(peer);
            if (session == nullptr || !session->established) {
                return plaintext;
            }
        }
        read_records(*session, plaintext);
        return plaintext;
    }

    /// Retransmit lost handshake flights and abandon handshakes that have run
    /// too long. Called from every tick of the backend's poll loop.
    auto service_timers() -> void {
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::string> expired;
        for (auto& [key, session] : _sessions) {
            if (session->established) {
                continue;
            }
            if (now - session->started > dtls_handshake_timeout) {
                expired.push_back(key);
                continue;
            }
            // Returns 0 when no timer is due; > 0 having resent the flight.
            if (DTLSv1_handle_timeout(session->ssl.get()) < 0) {
                expired.push_back(key);
            }
        }
        for (const auto& key : expired) {
            if (const auto it = _sessions.find(key); it != _sessions.end()) {
                fail(*it->second, "the DTLS handshake timed out");
            }
        }
    }

    /// Forget `peer`'s session so the next send() handshakes afresh. Used by a
    /// client whose exchange went unanswered: the server may have restarted and
    /// lost its half, in which case no record this side sends can ever be read
    /// again.
    auto reset(const peer_address& peer) -> void { _sessions.erase(peer.key()); }

    /// Send close_notify on every established session and drop them all. Call
    /// while the socket is still open; it is what tells a client its server
    /// is going away, so the client's next request re-handshakes rather than
    /// timing out against a session that no longer exists.
    auto close_all() -> void {
        for (auto& [key, session] : _sessions) {
            if (session->established) {
                SSL_shutdown(session->ssl.get());
            }
        }
        _sessions.clear();
        ERR_clear_error();
    }

    /// Handshake failures since the last call, as (peer, reason). The client
    /// rejects the exchanges waiting on each peer instead of letting them run
    /// to their timeout; a server has nobody to tell and ignores them.
    [[nodiscard]] auto take_failures() -> std::vector<std::pair<peer_address, std::string>> {
        return std::exchange(_failures, {});
    }

    [[nodiscard]] auto has_session(const peer_address& peer) const -> bool {
        return _sessions.contains(peer.key());
    }

    [[nodiscard]] auto is_established(const peer_address& peer) const -> bool {
        const auto it = _sessions.find(peer.key());
        return it != _sessions.end() && it->second->established;
    }

    [[nodiscard]] auto mode() const -> coap_auth_mode { return _mode; }

private:
    struct ssl_deleter {
        auto operator()(SSL* ssl) const -> void { SSL_free(ssl); }
    };
    struct ctx_deleter {
        auto operator()(SSL_CTX* ctx) const -> void { SSL_CTX_free(ctx); }
    };
    struct method_deleter {
        auto operator()(BIO_METHOD* method) const -> void { BIO_meth_free(method); }
    };

    struct session {
        dtls_layer* owner{nullptr};
        peer_address peer;
        std::unique_ptr<SSL, ssl_deleter> ssl;
        /// Received datagrams not yet consumed by OpenSSL, one per BIO read.
        std::deque<std::vector<std::uint8_t>> inbound;
        /// Plaintext waiting for the handshake to finish (client only).
        std::deque<std::vector<std::byte>> queued;
        bool established{false};
        std::chrono::steady_clock::time_point started;
        std::chrono::steady_clock::time_point last_activity;
    };

    template<typename Credentials>
    [[nodiscard]] static auto credentials_as(const coap_security_config& config)
        -> const Credentials& {
        if (!std::holds_alternative<Credentials>(config.credentials)) {
            throw coap_security_config_error("security mode '" + kythira::to_string(config.mode) +
                                             "' was given credentials of the wrong kind");
        }
        return std::get<Credentials>(config.credentials);
    }

    // ---- credential loading -------------------------------------------------

    auto configure_psk(const psk_credentials& creds) -> void {
        if (creds.identity.empty() || creds.key.empty()) {
            throw coap_security_config_error("dtls_psk needs both an identity and a key");
        }
        validate_psk_key_length(creds.key.size());
        if (creds.key.size() > PSK_MAX_PSK_LEN || creds.identity.size() > PSK_MAX_IDENTITY_LEN) {
            throw coap_security_config_error(
                "dtls_psk identity or key is longer than OpenSSL "
                "accepts");
        }
        _psk = creds;
        // PSK suites only: with no certificate configured, anything else would
        // fail the handshake anyway, just later and less legibly.
        detail::apply_default_dtls_cipher_list(_ctx.get(), detail::default_dtls_psk_cipher_list);
        if (_role == coap_security_role::client) {
            SSL_CTX_set_psk_client_callback(_ctx.get(), &dtls_layer::psk_client);
        } else {
            SSL_CTX_set_psk_server_callback(_ctx.get(), &dtls_layer::psk_server);
        }
    }

    auto configure_pki(const pki_credentials& creds) -> void {
        validate_pki_peer_policy(creds);
        if (_role == coap_security_role::server &&
            (creds.cert_file.empty() || creds.key_file.empty())) {
            throw coap_security_config_error("a dtls_pki server needs a certificate and a key");
        }
        if (!creds.cert_file.empty()) {
            if (SSL_CTX_use_certificate_chain_file(_ctx.get(), creds.cert_file.c_str()) != 1) {
                throw coap_security_error("failed to load the DTLS certificate '" +
                                          creds.cert_file +
                                          "': " + openssl_error_text("unreadable"));
            }
            if (SSL_CTX_use_PrivateKey_file(_ctx.get(), creds.key_file.c_str(), SSL_FILETYPE_PEM) !=
                1) {
                throw coap_security_error("failed to load the DTLS private key '" + creds.key_file +
                                          "': " + openssl_error_text("unreadable"));
            }
            if (SSL_CTX_check_private_key(_ctx.get()) != 1) {
                throw coap_security_error("the DTLS private key '" + creds.key_file +
                                          "' does not match the certificate '" + creds.cert_file +
                                          "'");
            }
        }
        if (!creds.ca_file.empty()) {
            if (SSL_CTX_load_verify_locations(_ctx.get(), creds.ca_file.c_str(), nullptr) != 1) {
                throw coap_security_error("failed to load the DTLS CA file '" + creds.ca_file +
                                          "': " + openssl_error_text("unreadable"));
            }
        } else if (creds.verify_peer_cert) {
            SSL_CTX_set_default_verify_paths(_ctx.get());
        }
        detail::apply_dtls_cipher_list(_ctx.get(), detail::dtls_cipher_list(creds.cipher_suites));
        if (creds.verify_peer_cert) {
            int mode = SSL_VERIFY_PEER;
            if (_role == coap_security_role::server) {
                // Mutual authentication: a server that verifies peers must
                // also insist on there being a certificate to verify.
                mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
            }
            SSL_CTX_set_verify(_ctx.get(), mode, nullptr);
        } else {
            SSL_CTX_set_verify(_ctx.get(), SSL_VERIFY_NONE, nullptr);
        }
        _cn_validator = creds.cn_validator;
        _ca_file = creds.ca_file;
        _revocation = creds.revocation;
    }

    auto configure_rpk(const rpk_credentials& creds) -> void {
#if OPENSSL_VERSION_NUMBER >= 0x30200000L
        if (creds.private_key.empty()) {
            throw coap_security_config_error("dtls_rpk needs a private key");
        }
        auto key = load_private_key(creds.private_key);
        if (SSL_CTX_use_PrivateKey(_ctx.get(), key.get()) != 1) {
            throw coap_security_error("failed to install the DTLS raw private key: " +
                                      openssl_error_text("rejected"));
        }
        // Offer and accept only raw public keys (RFC 7250), in both directions.
        const std::array<unsigned char, 1> rpk_only{TLSEXT_cert_type_rpk};
        if (SSL_CTX_set1_client_cert_type(_ctx.get(), rpk_only.data(), rpk_only.size()) != 1 ||
            SSL_CTX_set1_server_cert_type(_ctx.get(), rpk_only.data(), rpk_only.size()) != 1) {
            throw coap_security_error("failed to restrict DTLS to raw public keys: " +
                                      openssl_error_text("rejected"));
        }
        for (const auto& trusted : creds.trusted_peer_keys) {
            _trusted_rpks.push_back(trusted);
        }
        // There is no chain to verify; trust is the pinned-key comparison done
        // in check_established_peer(). VERIFY_PEER still makes the peer send
        // its key (and makes a client-side server ask for one).
        int mode = SSL_VERIFY_PEER;
        if (_role == coap_security_role::server) {
            mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        }
        SSL_CTX_set_verify(_ctx.get(), mode, [](int, X509_STORE_CTX*) { return 1; });
        _rpk = true;
#else
        (void)creds;
        throw coap_unsupported_security_mode_error(
            coap_auth_mode::dtls_rpk,
            std::string("RFC 7250 raw public keys need OpenSSL 3.2 or later, and this build has ") +
                OPENSSL_VERSION_TEXT +
                ". Use dtls_pki or dtls_psk, or rebuild against a newer "
                "OpenSSL.");
#endif
    }

#if OPENSSL_VERSION_NUMBER >= 0x30200000L
    struct pkey_deleter {
        auto operator()(EVP_PKEY* key) const -> void { EVP_PKEY_free(key); }
    };

    /// `rpk_credentials` holds PEM, matching the libcoap provider's
    /// COAP_PKI_KEY_PEM_BUF use; accept DER too, since a raw key is often
    /// handled as bare bytes.
    [[nodiscard]] static auto load_private_key(const std::vector<std::byte>& bytes)
        -> std::unique_ptr<EVP_PKEY, pkey_deleter> {
        std::unique_ptr<BIO, decltype(&BIO_free)> bio(
            BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size())), &BIO_free);
        EVP_PKEY* key = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
        if (key == nullptr) {
            ERR_clear_error();
            const auto* start = reinterpret_cast<const unsigned char*>(bytes.data());
            key = d2i_AutoPrivateKey(nullptr, &start, static_cast<long>(bytes.size()));
        }
        if (key == nullptr) {
            throw coap_security_error("failed to parse the DTLS raw private key: " +
                                      openssl_error_text("neither PEM nor DER"));
        }
        return std::unique_ptr<EVP_PKEY, pkey_deleter>(key);
    }

    /// Normalise a configured trusted key to DER SubjectPublicKeyInfo, which is
    /// what the peer's key is compared as. Accepts PEM or DER.
    [[nodiscard]] static auto spki_der(const std::vector<std::byte>& bytes)
        -> std::vector<std::byte> {
        std::unique_ptr<BIO, decltype(&BIO_free)> bio(
            BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size())), &BIO_free);
        EVP_PKEY* key = PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
        if (key == nullptr) {
            ERR_clear_error();
            return bytes;  // already DER (or garbage, which then never matches)
        }
        auto der = public_key_der(key);
        EVP_PKEY_free(key);
        return der;
    }
#endif

    [[nodiscard]] static auto public_key_der(EVP_PKEY* key) -> std::vector<std::byte> {
        unsigned char* der = nullptr;
        const int length = i2d_PUBKEY(key, &der);
        if (length <= 0 || der == nullptr) {
            return {};
        }
        std::vector<std::byte> out(reinterpret_cast<std::byte*>(der),
                                   reinterpret_cast<std::byte*>(der) + length);
        OPENSSL_free(der);
        return out;
    }

    // ---- sessions -----------------------------------------------------------

    [[nodiscard]] auto find(const peer_address& peer) -> session* {
        const auto it = _sessions.find(peer.key());
        return it == _sessions.end() ? nullptr : it->second.get();
    }

    auto open_session(const peer_address& peer) -> session& {
        auto created = std::make_unique<session>();
        created->owner = this;
        created->peer = peer;
        created->started = std::chrono::steady_clock::now();
        created->last_activity = created->started;
        created->ssl.reset(SSL_new(_ctx.get()));
        if (!created->ssl) {
            throw coap_security_error("failed to create a DTLS session: " +
                                      openssl_error_text("SSL_new failed"));
        }
        BIO* bio = BIO_new(_method.get());
        if (bio == nullptr) {
            throw coap_security_error("failed to create the DTLS datagram BIO");
        }
        BIO_set_data(bio, created.get());
        SSL_set_bio(created->ssl.get(), bio, bio);  // the SSL now owns the BIO
        SSL_set_app_data(created->ssl.get(), created.get());
        // Our BIO cannot discover the path MTU, so tell OpenSSL what it is.
        SSL_set_options(created->ssl.get(), SSL_OP_NO_QUERY_MTU);
        DTLS_set_link_mtu(created->ssl.get(), dtls_link_mtu);
        if (_role == coap_security_role::server) {
            SSL_set_options(created->ssl.get(), SSL_OP_COOKIE_EXCHANGE);
            SSL_set_accept_state(created->ssl.get());
        } else {
            SSL_set_connect_state(created->ssl.get());
        }
        auto& slot = _sessions[peer.key()];
        slot = std::move(created);
        return *slot;
    }

    auto evict_if_full() -> void {
        if (_sessions.size() < dtls_max_sessions) {
            return;
        }
        const auto oldest = std::min_element(
            _sessions.begin(), _sessions.end(), [](const auto& left, const auto& right) {
                return left.second->last_activity < right.second->last_activity;
            });
        _sessions.erase(oldest);
    }

    /// Step the handshake. Returns true once it has completed (and the peer
    /// passed every check), false while it is still running or after it
    /// failed, in which case the session is gone.
    auto drive_handshake(session& current) -> bool {
        ERR_clear_error();
        const int result = SSL_do_handshake(current.ssl.get());
        if (result == 1) {
            if (const auto refusal = check_established_peer(current); !refusal.empty()) {
                fail(current, refusal);
                return false;
            }
            current.established = true;
            // Flush what the message layer queued while this was running.
            auto queued = std::move(current.queued);
            current.queued.clear();
            for (const auto& plain : queued) {
                write_record(current, plain);
            }
            return true;
        }
        const int error = SSL_get_error(current.ssl.get(), result);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            return false;
        }
        fail(current, openssl_error_text("the DTLS handshake failed"));
        return false;
    }

    /// Checks the handshake itself cannot make: the RPK pin, the CRL check and
    /// the application's own certificate validator. Returns why the peer is
    /// refused, or an empty string. Runs before the session is marked
    /// established, so nothing queued or inbound crosses a refused one.
    [[nodiscard]] auto check_established_peer(session& current) -> std::string {
#if OPENSSL_VERSION_NUMBER >= 0x30200000L
        if (_rpk) {
            EVP_PKEY* peer_key = SSL_get0_peer_rpk(current.ssl.get());
            if (peer_key == nullptr) {
                return "the peer presented no raw public key";
            }
            const auto der = public_key_der(peer_key);
            for (const auto& trusted : _trusted_rpks) {
                if (spki_der(trusted) == der) {
                    return {};
                }
            }
            return "the peer's raw public key is not one this side trusts";
        }
#endif
        // Revocation first, so a revoked peer is refused without the
        // validator ever seeing it (as dtls_pki_provider::validate_cn does).
        if (_revocation.enabled) {
            X509* peer_cert = SSL_get0_peer_certificate(current.ssl.get());
            if (peer_cert == nullptr) {
                return "the peer presented no certificate for the revocation check";
            }
            // The peer's chain as sent; on a server it excludes the leaf,
            // which is exactly what "untrusted intermediates" wants.
            if (const auto refusal = coap_revocation::check(
                    peer_cert, _ca_file, _revocation, SSL_get_peer_cert_chain(current.ssl.get()))) {
                return "the peer's certificate failed the revocation check: " + *refusal;
            }
        }
        if (_cn_validator) {
            X509* peer_cert = SSL_get0_peer_certificate(current.ssl.get());
            if (peer_cert == nullptr) {
                return "the peer presented no certificate for the configured validator";
            }
            std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), &BIO_free);
            PEM_write_bio_X509(bio.get(), peer_cert);
            char* pem = nullptr;
            const long length = BIO_get_mem_data(bio.get(), &pem);
            bool accepted = false;
            try {
                accepted = _cn_validator(std::string(pem, static_cast<std::size_t>(length)));
            } catch (...) {
                accepted = false;
            }
            if (!accepted) {
                return "the peer's certificate was refused by the configured validator";
            }
        }
        return {};
    }

    auto write_record(session& current, const std::vector<std::byte>& plain) -> void {
        ERR_clear_error();
        const int written =
            SSL_write(current.ssl.get(), plain.data(), static_cast<int>(plain.size()));
        if (written <= 0) {
            // A record too large for the MTU, or a session that has failed:
            // either way nothing went out, and CoAP's retransmission or timeout
            // is what the caller sees.
            ERR_clear_error();
        }
    }

    auto read_records(session& current, std::vector<std::vector<std::byte>>& out) -> void {
        std::array<std::uint8_t, 2048> buffer{};
        while (true) {
            ERR_clear_error();
            const int read =
                SSL_read(current.ssl.get(), buffer.data(), static_cast<int>(buffer.size()));
            if (read > 0) {
                const auto* start = reinterpret_cast<const std::byte*>(buffer.data());
                out.emplace_back(start, start + read);
                continue;
            }
            const int error = SSL_get_error(current.ssl.get(), read);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                return;
            }
            // close_notify from the peer, or a fatal alert: the session is
            // over. Dropping it means the next send() handshakes again.
            const auto key = current.peer.key();
            ERR_clear_error();
            _sessions.erase(key);
            return;
        }
    }

    auto fail(session& current, const std::string& reason) -> void {
        _failures.emplace_back(current.peer, reason);
        _sessions.erase(current.peer.key());
    }

    /// A DTLS record header is 13 bytes, and a ClientHello is a handshake
    /// record (content type 22) whose first handshake message is type 1.
    [[nodiscard]] static auto is_client_hello(const std::uint8_t* data, std::size_t length)
        -> bool {
        return length > 13 && data[0] == 22 && data[13] == 1;
    }

    // ---- OpenSSL callbacks --------------------------------------------------

    [[nodiscard]] static auto owner_of(SSL* ssl) -> dtls_layer* {
        return static_cast<dtls_layer*>(SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)));
    }

    static auto bio_write(BIO* bio, const char* data, int length) -> int {
        auto* current = static_cast<session*>(BIO_get_data(bio));
        if (current == nullptr || length <= 0) {
            return 0;
        }
        current->owner->_send(current->peer, reinterpret_cast<const std::uint8_t*>(data),
                              static_cast<std::size_t>(length));
        return length;
    }

    static auto bio_read(BIO* bio, char* data, int length) -> int {
        auto* current = static_cast<session*>(BIO_get_data(bio));
        BIO_clear_retry_flags(bio);
        if (current == nullptr || current->inbound.empty()) {
            BIO_set_retry_read(bio);
            return -1;
        }
        auto datagram = std::move(current->inbound.front());
        current->inbound.pop_front();
        // A datagram larger than the read is truncated, exactly as recvfrom()
        // on a socket BIO would; DTLS then discards the damaged record.
        const auto copied =
            std::min<std::size_t>(datagram.size(), static_cast<std::size_t>(length));
        std::memcpy(data, datagram.data(), copied);
        return static_cast<int>(copied);
    }

    static auto bio_ctrl(BIO* bio, int command, long, void* pointer) -> long {
        auto* current = static_cast<session*>(BIO_get_data(bio));
        switch (command) {
            case BIO_CTRL_FLUSH:
                return 1;
            case BIO_CTRL_PENDING:
                return current != nullptr && !current->inbound.empty()
                           ? static_cast<long>(current->inbound.front().size())
                           : 0;
            case BIO_CTRL_WPENDING:
                return 0;
            case BIO_CTRL_DGRAM_QUERY_MTU:
            case BIO_CTRL_DGRAM_GET_FALLBACK_MTU:
                return dtls_link_mtu;
            case BIO_CTRL_DGRAM_GET_MTU_OVERHEAD:
                // IPv6 (40) + UDP (8); the larger of the two families.
                return 48;
            case BIO_CTRL_DGRAM_GET_PEER:
                if (current != nullptr && pointer != nullptr) {
                    std::memcpy(pointer, &current->peer.storage, current->peer.length);
                    return current->peer.length;
                }
                return 0;
            case BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT:
            case BIO_CTRL_DGRAM_SET_CONNECTED:
            case BIO_CTRL_DGRAM_SET_PEER:
                return 1;
            default:
                return 0;
        }
    }

    /// HMAC of the peer's address under a per-process secret: stateless to
    /// check, and useless to anyone who cannot receive at that address.
    [[nodiscard]] auto cookie_for(const session& current) const -> std::vector<unsigned char> {
        const auto key = current.peer.key();
        std::vector<unsigned char> mac(EVP_MAX_MD_SIZE);
        unsigned int length = 0;
        HMAC(EVP_sha256(), _cookie_secret.data(), static_cast<int>(_cookie_secret.size()),
             reinterpret_cast<const unsigned char*>(key.data()), key.size(), mac.data(), &length);
        mac.resize(length);
        return mac;
    }

    static auto generate_cookie(SSL* ssl, unsigned char* cookie, unsigned int* length) -> int {
        const auto* current = static_cast<const session*>(SSL_get_app_data(ssl));
        if (current == nullptr) {
            return 0;
        }
        const auto mac = owner_of(ssl)->cookie_for(*current);
        std::memcpy(cookie, mac.data(), mac.size());
        *length = static_cast<unsigned int>(mac.size());
        return 1;
    }

    static auto verify_cookie(SSL* ssl, const unsigned char* cookie, unsigned int length) -> int {
        const auto* current = static_cast<const session*>(SSL_get_app_data(ssl));
        if (current == nullptr) {
            return 0;
        }
        const auto expected = owner_of(ssl)->cookie_for(*current);
        return expected.size() == length && CRYPTO_memcmp(expected.data(), cookie, length) == 0 ? 1
                                                                                                : 0;
    }

    static auto psk_client(SSL* ssl, const char*, char* identity, unsigned int max_identity_length,
                           unsigned char* psk, unsigned int max_psk_length) -> unsigned int {
        const auto& creds = owner_of(ssl)->_psk;
        if (creds.identity.size() + 1 > max_identity_length || creds.key.size() > max_psk_length) {
            return 0;
        }
        std::memcpy(identity, creds.identity.c_str(), creds.identity.size() + 1);
        std::memcpy(psk, creds.key.data(), creds.key.size());
        return static_cast<unsigned int>(creds.key.size());
    }

    static auto psk_server(SSL* ssl, const char* identity, unsigned char* psk,
                           unsigned int max_psk_length) -> unsigned int {
        const auto& creds = owner_of(ssl)->_psk;
        // An unknown identity returns 0, which OpenSSL turns into an
        // unknown_psk_identity alert: the handshake fails, nothing is served.
        if (identity == nullptr || creds.identity != identity ||
            creds.key.size() > max_psk_length) {
            return 0;
        }
        std::memcpy(psk, creds.key.data(), creds.key.size());
        return static_cast<unsigned int>(creds.key.size());
    }

    coap_security_role _role;
    coap_auth_mode _mode;
    send_function _send;
    std::unique_ptr<SSL_CTX, ctx_deleter> _ctx;
    std::unique_ptr<BIO_METHOD, method_deleter> _method;
    std::array<unsigned char, 32> _cookie_secret{};

    psk_credentials _psk;
    std::function<bool(const std::string&)> _cn_validator;
    std::string _ca_file;
    certificate_revocation_config _revocation;
    [[maybe_unused]] bool _rpk{false};  // read only where OpenSSL has RFC 7250
    std::vector<std::vector<std::byte>> _trusted_rpks;

    std::unordered_map<std::string, std::unique_ptr<session>> _sessions;
    std::vector<std::pair<peer_address, std::string>> _failures;
};

}  // namespace kythira::cantcoap_detail
