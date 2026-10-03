// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Provider implementations for the coap-transport-security spec. Split from
// coap_security.hpp the same way coap_transport_impl.hpp is split from
// coap_transport.hpp: this header needs real libcoap (and, for the PKI/RPK
// CN callbacks, OpenSSL) types, so it is only meaningful to include from a
// translation unit that also includes coap_transport_impl.hpp (or otherwise
// defines LIBCOAP_AVAILABLE before including it).
//
// NOTE on LIBCOAP_AVAILABLE: as with the rest of coap_transport_impl.hpp,
// nothing in the default build defines this macro today, so the #else stub
// branches below are what actually compile in CI. The dtls_psk_provider /
// dtls_pki_provider bodies mirror coap_transport_impl.hpp's
// setup_dtls_context() call-for-call (Property 3 of design.md) rather than
// calling it directly, since that method is a private member of the
// templated coap_client<Types>/coap_server<Types> classes and depends on
// nothing beyond config values — making it safe to give the same logic a
// second, non-templated home in dtls_psk_provider/dtls_pki_provider without
// risking the existing (never-compiled-differently) call sites.

#include <raft/coap_security.hpp>
#include <raft/coap_transport.hpp>
#include <raft/oscore.hpp>
#include <raft/oscore_group_contexts.hpp>

#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#ifdef LIBCOAP_AVAILABLE
#include <coap3/coap.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <raft/coap_dtls_cipher_suites.hpp>
#include <raft/coap_revocation.hpp>
#endif

namespace kythira {

namespace detail {

#ifdef LIBCOAP_AVAILABLE
// Makes `ca_file` the trust store DTLS peers are verified against.
//
// coap_context_set_pki() alone does not do this. libcoap only loads
// pki_key.key.pem.ca_file when check_common_ca is set (which also demands
// that the peer share our own issuer), and nothing here sets it, so with
// verify_peer_cert on every handshake failed with "unable to get local
// issuer certificate" -- PKI DTLS had never completed one. Root CAs are
// what the CA file actually is in this transport's configuration.
//
// Returns false if the file could not be loaded. That fails closed on its
// own -- with no trust anchors every chain verification fails -- so callers
// choose whether to reject the configuration up front or only report it.
[[nodiscard]] inline auto install_pki_trust_anchors(coap_context_t* ctx, const std::string& ca_file)
    -> bool {
    if (ca_file.empty()) {
        return true;
    }
    return coap_context_set_pki_root_cas(ctx, ca_file.c_str(), nullptr) != 0;
}

// ── Applying pki_credentials::cipher_suites through libcoap ───────────────
// libcoap has no "cipher list" setting: its OpenSSL backend hard-codes
// COAP_OPENSSL_CIPHERS. What it does offer is a way to reach the SSL object
// for each connection, and that is enough on both sides:
//
// - server: coap_dtls_pki_t::additional_tls_setup_call_back runs inside
//   OpenSSL's ClientHello callback, before the server picks a suite, so
//   restricting the SSL there decides what can be negotiated;
// - client: libcoap 4.3.5 never calls that hook for clients, so the client
//   restricts each new session through coap_session_get_tls() instead (see
//   restrict_client_session_ciphers()).
//
// The hook gets no user argument of its own, so it finds the list through
// the context's app data, which coap_client/coap_server point at their
// _dtls_cipher_list member. It is only installed when a list is configured,
// and everything unexpected fails the handshake rather than falling back to
// the defaults.

// The SSL* those functions hand out is only an SSL* when libcoap was built
// against OpenSSL; any other TLS library would need its own cipher API.
inline auto require_openssl_libcoap_for_cipher_suites() -> void {
    const coap_tls_version_t* tls = coap_get_tls_library_version();
    if (tls == nullptr || tls->type != COAP_TLS_LIBRARY_OPENSSL) {
        throw coap_security_config_error(
            "DTLS cipher_suites are configured, but this libcoap is not built with OpenSSL, so "
            "they cannot be applied");
    }
}

inline auto libcoap_cipher_list_hook(void* tls_session, coap_dtls_pki_t* /*setup_data*/) -> int {
    auto* ssl = static_cast<SSL*>(tls_session);
    if (ssl == nullptr) {
        return 0;
    }
    const auto* session = static_cast<const coap_session_t*>(SSL_get_app_data(ssl));
    const coap_context_t* ctx = session == nullptr ? nullptr : coap_session_get_context(session);
    const auto* list =
        ctx == nullptr ? nullptr : static_cast<const std::string*>(coap_context_get_app_data(ctx));
    if (list == nullptr || list->empty()) {
        return 0;
    }
    return apply_dtls_cipher_list(ssl, *list) ? 1 : 0;
}

// Restrict a freshly created client DTLS session to `list` (no-op for "").
//
// libcoap has already sent the first ClientHello by the time the session is
// returned, so that one still advertises the default suites. That is
// harmless: OpenSSL checks the ServerHello's choice against the *current*
// list and aborts with "wrong cipher returned" otherwise, and the retried
// ClientHello after a HelloVerifyRequest is built from the restricted list.
// Restricting the shared SSL_CTX as well means every later session offers
// only the configured suites from its first flight.
[[nodiscard]] inline auto restrict_client_session_ciphers(coap_session_t* session,
                                                          const std::string& list) -> bool {
    if (list.empty()) {
        return true;
    }
    coap_tls_library_t tls_lib{};
    auto* ssl = static_cast<SSL*>(coap_session_get_tls(session, &tls_lib));
    if (ssl == nullptr || tls_lib != COAP_TLS_LIBRARY_OPENSSL) {
        return false;
    }
    if (!apply_dtls_cipher_list(ssl, list)) {
        return false;
    }
    return SSL_CTX_set_cipher_list(SSL_get_SSL_CTX(ssl), list.c_str()) == 1;
}
#endif

inline auto bytes_to_hex(const std::vector<std::byte>& bytes) -> std::string {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto b : bytes) {
        out << std::setw(2) << static_cast<int>(std::to_integer<unsigned char>(b));
    }
    return out.str();
}

}  // namespace detail

// ── no_auth_provider ───────────────────────────────────────────────────────

class no_auth_provider final : public coap_security_provider {
public:
    auto configure_session(coap_context_t*) -> void override {}

    auto create_client_session(coap_context_t* ctx, const coap_address_t* local_if,
                               const coap_address_t* server_addr, std::uint8_t proto)
        -> coap_session_t* override {
#ifdef LIBCOAP_AVAILABLE
        return coap_new_client_session(ctx, local_if, server_addr,
                                       static_cast<coap_proto_t>(proto));
#else
        (void)ctx;
        (void)local_if;
        (void)server_addr;
        (void)proto;
        return nullptr;
#endif
    }

    [[nodiscard]] auto mode() const -> coap_auth_mode override { return coap_auth_mode::none; }
};

// ── dtls_psk_provider ──────────────────────────────────────────────────────
// Mirrors coap_transport_impl.hpp's setup_dtls_context() PSK branch: client
// (coap_transport_impl.hpp:816-846) uses the simple coap_context_set_psk();
// server (:2468-2531) uses coap_context_set_psk2() with an identity-matching
// callback. Both variants validate PSK key length (4-64 bytes) and identity
// length (<=128 chars) identically (Requirement 2.1).

class dtls_psk_provider final : public coap_security_provider {
public:
    dtls_psk_provider(psk_credentials creds, coap_security_role role)
        : _creds(std::move(creds)), _role(role) {
        if (_creds.key.size() < 4 || _creds.key.size() > 64) {
            throw coap_security_error("PSK key length must be between 4 and 64 bytes");
        }
        if (_creds.identity.length() > 128) {
            throw coap_security_error("PSK identity length must not exceed 128 characters");
        }
    }

    auto configure_session(coap_context_t* ctx) -> void override {
#ifdef LIBCOAP_AVAILABLE
        if (_role == coap_security_role::client) {
            if (coap_context_set_psk(ctx, _creds.identity.c_str(),
                                     reinterpret_cast<const uint8_t*>(_creds.key.data()),
                                     _creds.key.size()) == 0) {
                throw coap_security_error("Failed to configure DTLS PSK context");
            }
        } else {
            coap_dtls_spsk_t spsk_config;
            std::memset(&spsk_config, 0, sizeof(spsk_config));
            spsk_config.version = COAP_DTLS_SPSK_SETUP_VERSION;
            spsk_config.psk_info.hint.s = reinterpret_cast<const uint8_t*>(_creds.identity.c_str());
            spsk_config.psk_info.hint.length = _creds.identity.length();
            spsk_config.psk_info.key.s = reinterpret_cast<const uint8_t*>(_creds.key.data());
            spsk_config.psk_info.key.length = _creds.key.size();
            spsk_config.validate_id_call_back = &dtls_psk_provider::validate_id_callback;
            spsk_config.id_call_back_arg = this;
            if (coap_context_set_psk2(ctx, &spsk_config) == 0) {
                throw coap_security_error("Failed to configure server DTLS PSK context");
            }
        }
#else
        (void)ctx;
#endif
    }

    auto create_client_session(coap_context_t* ctx, const coap_address_t* local_if,
                               const coap_address_t* server_addr, std::uint8_t proto)
        -> coap_session_t* override {
#ifdef LIBCOAP_AVAILABLE
        return coap_new_client_session(ctx, local_if, server_addr,
                                       static_cast<coap_proto_t>(proto));
#else
        (void)ctx;
        (void)local_if;
        (void)server_addr;
        (void)proto;
        return nullptr;
#endif
    }

    [[nodiscard]] auto mode() const -> coap_auth_mode override { return coap_auth_mode::dtls_psk; }

    [[nodiscard]] auto credentials() const -> const psk_credentials& { return _creds; }

#ifdef LIBCOAP_AVAILABLE
    // The server-side identity-matching decision (Requirement 2.1), public
    // so it can be exercised directly by tests without needing a full DTLS
    // handshake to reach it — coap_context_set_psk2() only ever invokes this
    // from inside a real handshake.
    static auto validate_id_callback(coap_bin_const_t* identity, coap_session_t*, void* arg)
        -> const coap_bin_const_t* {
        auto* self = static_cast<dtls_psk_provider*>(arg);
        std::string client_identity(reinterpret_cast<const char*>(identity->s), identity->length);
        if (client_identity != self->_creds.identity) {
            return nullptr;
        }
        static thread_local coap_bin_const_t psk_key;
        psk_key.s = reinterpret_cast<const uint8_t*>(self->_creds.key.data());
        psk_key.length = self->_creds.key.size();
        return &psk_key;
    }
#endif

private:
    psk_credentials _creds;
    coap_security_role _role;
};

// ── dtls_pki_provider ──────────────────────────────────────────────────────
// Mirrors setup_dtls_context()'s PKI branch (client: :724-814, server:
// :2409-2466): mutual auth iff verify_peer_cert, chain validation to depth
// 10, CN-validation callback. The legacy code's CN callback additionally
// re-derived and independently re-verified the full X.509 chain
// (duplicating what cert_chain_validation=1 already asks libcoap's TLS
// backend to do); the provider trusts libcoap's own `validated` result by
// default and only does extra application-level work via the optional
// `pki_credentials::cn_validator` hook, which callers needing the old
// behavior can supply explicitly.

class dtls_pki_provider final : public coap_security_provider {
public:
    dtls_pki_provider(pki_credentials creds, coap_security_role role)
        : _creds(std::move(creds)), _role(role) {}

    auto configure_session(coap_context_t* ctx) -> void override {
#ifdef LIBCOAP_AVAILABLE
        coap_dtls_pki_t pki_config;
        std::memset(&pki_config, 0, sizeof(pki_config));
        pki_config.version = COAP_DTLS_PKI_SETUP_VERSION;
        // Note: coap_dtls_pki_t has no separate require_peer_cert field in
        // the libcoap version this project pins (>=4.3.5) — verify_peer_cert
        // alone drives mutual authentication.
        pki_config.verify_peer_cert = _creds.verify_peer_cert ? 1 : 0;
        pki_config.allow_self_signed = !_creds.verify_peer_cert ? 1 : 0;
        pki_config.allow_expired_certs = 0;
        pki_config.cert_chain_validation = 1;
        pki_config.cert_chain_verify_depth = 10;
        pki_config.check_cert_revocation = 1;
        pki_config.allow_no_crl = 1;
        pki_config.allow_expired_crl = 0;
        pki_config.pki_key.key_type = COAP_PKI_KEY_PEM;
        pki_config.pki_key.key.pem.public_cert = _creds.cert_file.c_str();
        pki_config.pki_key.key.pem.private_key = _creds.key_file.c_str();
        pki_config.pki_key.key.pem.ca_file =
            _creds.ca_file.empty() ? nullptr : _creds.ca_file.c_str();
        if (_creds.verify_peer_cert) {
            pki_config.validate_cn_call_back = &dtls_pki_provider::validate_cn;
            pki_config.cn_call_back_arg = this;
        }
        if (!_creds.cipher_suites.empty()) {
            pki_config.additional_tls_setup_call_back = &detail::libcoap_cipher_list_hook;
        }
        if (coap_context_set_pki(ctx, &pki_config) == 0) {
            throw coap_security_error("Failed to configure DTLS PKI context");
        }
        if (!detail::install_pki_trust_anchors(ctx, _creds.ca_file)) {
            throw coap_security_error("Failed to load DTLS trust anchors from: " + _creds.ca_file);
        }
#else
        (void)ctx;
#endif
    }

    auto create_client_session(coap_context_t* ctx, const coap_address_t* local_if,
                               const coap_address_t* server_addr, std::uint8_t proto)
        -> coap_session_t* override {
#ifdef LIBCOAP_AVAILABLE
        return coap_new_client_session(ctx, local_if, server_addr,
                                       static_cast<coap_proto_t>(proto));
#else
        (void)ctx;
        (void)local_if;
        (void)server_addr;
        (void)proto;
        return nullptr;
#endif
    }

    [[nodiscard]] auto mode() const -> coap_auth_mode override { return coap_auth_mode::dtls_pki; }

    [[nodiscard]] auto credentials() const -> const pki_credentials& { return _creds; }

#ifdef LIBCOAP_AVAILABLE
    // The CN-validation decision (Requirement 2.1/9.1's "trust libcoap's own
    // validated result by default" behavior), public so it can be exercised
    // directly by tests without a full handshake — see validate_cn's
    // class-level comment above for why this trusts `validated` by default.
    static auto validate_cn(const char*, const uint8_t* asn1_public_cert, std::size_t asn1_length,
                            coap_session_t*, unsigned, int validated, void* arg) -> int {
        auto* self = static_cast<dtls_pki_provider*>(arg);
        if (!self->_creds.cn_validator && !self->_creds.revocation.enabled) {
            return validated;
        }
        const uint8_t* cert_data = asn1_public_cert;
        X509* cert = d2i_X509(nullptr, &cert_data, static_cast<long>(asn1_length));
        if (cert == nullptr) {
            return 0;
        }
        // Revocation runs on top of libcoap's own chain validation, not in
        // place of it -- libcoap only sees CRLs bundled into the CA file.
        if (coap_revocation::check(cert, self->_creds.ca_file, self->_creds.revocation)) {
            X509_free(cert);
            return 0;
        }
        if (!self->_creds.cn_validator) {
            X509_free(cert);
            return validated;
        }
        BIO* bio = BIO_new(BIO_s_mem());
        if (bio == nullptr) {
            X509_free(cert);
            return 0;
        }
        if (PEM_write_bio_X509(bio, cert) == 0) {
            X509_free(cert);
            BIO_free(bio);
            return 0;
        }
        char* pem_data = nullptr;
        long pem_length = BIO_get_mem_data(bio, &pem_data);
        std::string cert_pem(pem_data, static_cast<std::size_t>(pem_length));
        X509_free(cert);
        BIO_free(bio);
        try {
            return self->_creds.cn_validator(cert_pem) ? 1 : 0;
        } catch (...) {
            return 0;
        }
    }
#endif

private:
    pki_credentials _creds;
    coap_security_role _role;
};

// ── dtls_rpk_provider ──────────────────────────────────────────────────────
// Requirement 3: shares dtls_pki_provider's coap_dtls_pki_t plumbing,
// differing only in pki_key.key_type (COAP_PKI_KEY_PEM_BUF with
// is_rpk_not_cert=1, since RPK "cannot be COAP_PKI_KEY_PEM" per
// coap_dtls.h) and in what "peer is trusted" means: raw-public-key byte
// equality against rpk_credentials::trusted_peer_keys rather than CA chain
// validation.

class dtls_rpk_provider final : public coap_security_provider {
public:
    dtls_rpk_provider(rpk_credentials creds, coap_security_role role)
        : _creds(std::move(creds)), _role(role) {}

    auto configure_session(coap_context_t* ctx) -> void override {
#ifdef LIBCOAP_AVAILABLE
        // libcoap's OpenSSL backend, the one the vcpkg port builds, has no
        // RFC 7250 support at all: coap_dtls_rpk_is_supported() is a
        // hard-coded 0 there. coap_context_set_pki() still accepts the
        // config, so without this check an RPK transport constructs cleanly
        // and then fails every handshake.
        if (coap_dtls_rpk_is_supported() == 0) {
            throw coap_unsupported_security_mode_error(
                coap_auth_mode::dtls_rpk,
                "the linked libcoap's TLS backend has no raw public key (RFC 7250) support; "
                "libcoap's OpenSSL, Mbed TLS and wolfSSL backends never have it, "
                "its GnuTLS and TinyDTLS backends do");
        }
        coap_dtls_pki_t pki_config;
        std::memset(&pki_config, 0, sizeof(pki_config));
        pki_config.version = COAP_DTLS_PKI_SETUP_VERSION;
        pki_config.verify_peer_cert = 1;
        pki_config.allow_self_signed = 0;
        pki_config.is_rpk_not_cert = 1;
        pki_config.pki_key.key_type = COAP_PKI_KEY_PEM_BUF;
        pki_config.pki_key.key.pem_buf.public_cert =
            reinterpret_cast<const uint8_t*>(_creds.public_key.data());
        pki_config.pki_key.key.pem_buf.public_cert_len = _creds.public_key.size();
        pki_config.pki_key.key.pem_buf.private_key =
            reinterpret_cast<const uint8_t*>(_creds.private_key.data());
        pki_config.pki_key.key.pem_buf.private_key_len = _creds.private_key.size();
        pki_config.validate_cn_call_back = &dtls_rpk_provider::validate_peer_key;
        pki_config.cn_call_back_arg = this;
        if (coap_context_set_pki(ctx, &pki_config) == 0) {
            throw coap_security_error("Failed to configure DTLS RPK context");
        }
#else
        (void)ctx;
#endif
    }

    auto create_client_session(coap_context_t* ctx, const coap_address_t* local_if,
                               const coap_address_t* server_addr, std::uint8_t proto)
        -> coap_session_t* override {
#ifdef LIBCOAP_AVAILABLE
        return coap_new_client_session(ctx, local_if, server_addr,
                                       static_cast<coap_proto_t>(proto));
#else
        (void)ctx;
        (void)local_if;
        (void)server_addr;
        (void)proto;
        return nullptr;
#endif
    }

    [[nodiscard]] auto mode() const -> coap_auth_mode override { return coap_auth_mode::dtls_rpk; }

    [[nodiscard]] auto credentials() const -> const rpk_credentials& { return _creds; }

    // The RPK-specific trust decision (Requirement 3.2), exposed directly
    // for testing (tests/coap_dtls_rpk_test.cpp) without needing a full
    // DTLS handshake to exercise the validate_cn_call_back path — session
    // establishment itself reuses the same, already-tested coap_dtls_pki_t
    // machinery as dtls_pki_provider (Property 4).
    [[nodiscard]] auto is_trusted_peer_key(const std::vector<std::byte>& peer_key) const -> bool {
        for (const auto& trusted : _creds.trusted_peer_keys) {
            if (trusted == peer_key) {
                return true;
            }
        }
        return false;
    }

#ifdef LIBCOAP_AVAILABLE
    // Public for the same testability reason as dtls_psk_provider::
    // validate_id_callback / dtls_pki_provider::validate_cn above.
    static auto validate_peer_key(const char*, const uint8_t* asn1_public_cert,
                                  std::size_t asn1_length, coap_session_t*, unsigned, int,
                                  void* arg) -> int {
        auto* self = static_cast<dtls_rpk_provider*>(arg);
        std::vector<std::byte> peer_key(asn1_length);
        std::memcpy(peer_key.data(), asn1_public_cert, asn1_length);
        return self->is_trusted_peer_key(peer_key) ? 1 : 0;
    }
#endif

private:
    rpk_credentials _creds;
    coap_security_role _role;
};

// ── oscore_provider ────────────────────────────────────────────────────────
// Requirement 4, and .kiro/specs/coap-transport-multi-raft/ tasks 9-11.
//
// OSCORE on the libcoap backend is Kythira's own (raft/oscore.hpp), the same
// code libnyoci and cantcoap use, not libcoap's. libcoap's server holds a fixed
// set of OSCORE contexts and, in every release up to 4.3.5, has no hook to
// derive one when an unknown `kid context` arrives. That is exactly what a
// context per Raft group needs: the peer's ID Context carries its boot nonce,
// which this node learns only from the peer's first request.
//
// So libcoap is a plain CoAP stack here. The OSCORE option (9) travels as an
// ordinary registered option, the transport protects each request and response
// itself (coap_transport_impl.hpp), and this provider owns the contexts:
//
//  - one base context from the configured credentials, for requests that name
//    no group, with the wire format the libnyoci backend already uses; and
//  - optionally a group_context_registry over the same credentials, for
//    requests that do (coap_oscore_group_config).
//
// libcoap must be built with its own OSCORE off. With it on, coap_dispatch()
// decrypts every request carrying option 9 before any handler runs, and drops
// any it has no context for, so this provider refuses to start rather than
// lose every request. vcpkg-overlays/libcoap builds it that way.

/// See the forward declaration in raft/coap_transport_config.hpp.
struct oscore_exchange {
    std::shared_ptr<oscore::security_context> context;
    oscore::request_binding binding;
};

class oscore_provider final : public coap_security_provider {
public:
    using context_ptr = std::shared_ptr<oscore::security_context>;

    oscore_provider(oscore_credentials creds, coap_security_role role,
                    coap_oscore_group_config groups = {})
        : _creds(std::move(creds)),
          _role(role),
          _base(std::make_shared<oscore::security_context>(_creds)) {
        if (groups.enabled) {
            if (_role == coap_security_role::server && !groups.hosts_group) {
                throw coap_security_config_error(
                    "oscore_groups.enabled on a server requires oscore_groups.hosts_group");
            }
            if (!_creds.id_context.empty()) {
                throw coap_security_config_error(
                    "oscore_groups.enabled requires oscore_credentials::id_context to be empty: "
                    "each group's ID Context is derived from the group id and the boot nonce");
            }
            auto hosts_group = groups.hosts_group
                                   ? std::move(groups.hosts_group)
                                   : std::function<bool(std::uint64_t)>{[](std::uint64_t) {
                                         // A client only ever derives sender
                                         // contexts, which never consult this.
                                         return false;
                                     }};
            _groups = std::make_unique<oscore::group_context_registry>(
                [creds = _creds](const std::string&) {
                    // Every group context is keyed by a fresh boot nonce, so
                    // persisting its sequence numbers would buy nothing and
                    // cost a write per group.
                    auto group_creds = creds;
                    group_creds.sequence_state_dir.clear();
                    return group_creds;
                },
                std::move(hosts_group),
                oscore::group_context_limits{groups.max_recipient_contexts_per_peer,
                                             groups.recipient_idle_ttl},
                groups.boot_nonce.empty() ? oscore::process_boot_nonce() : groups.boot_nonce);
        }
    }

    auto configure_session(coap_context_t* ctx) -> void override {
#ifdef LIBCOAP_AVAILABLE
        check_capability();
        // Without this, a libcoap built without OSCORE treats option 9 as an
        // unknown critical option and answers 4.02 Bad Option before any
        // handler runs; registered, it passes through like any other.
        coap_register_option(ctx, oscore::coap_option_oscore);
#else
        (void)ctx;
        throw coap_unsupported_security_mode_error(coap_auth_mode::oscore,
                                                   "libcoap not compiled into this build");
#endif
    }

    auto create_client_session(coap_context_t* ctx, const coap_address_t* local_if,
                               const coap_address_t* server_addr, std::uint8_t proto)
        -> coap_session_t* override {
#ifdef LIBCOAP_AVAILABLE
        check_capability();
        // A plain session: the transport protects each request itself.
        return coap_new_client_session(ctx, local_if, server_addr,
                                       static_cast<coap_proto_t>(proto));
#else
        (void)ctx;
        (void)local_if;
        (void)server_addr;
        (void)proto;
        throw coap_unsupported_security_mode_error(coap_auth_mode::oscore,
                                                   "libcoap not compiled into this build");
#endif
    }

    // Kept for source compatibility (Requirement 4.3). A server now accepts a
    // request whose kid is its configured recipient_id under any context it
    // can select; there is no per-recipient list in libcoap to add to.
    auto add_recipient(coap_context_t* ctx, const std::vector<std::byte>& recipient_id) -> void {
        (void)ctx;
        (void)recipient_id;
    }

    [[nodiscard]] auto mode() const -> coap_auth_mode override { return coap_auth_mode::oscore; }

    [[nodiscard]] auto credentials() const -> const oscore_credentials& { return _creds; }

    [[nodiscard]] auto base_context() const -> const context_ptr& { return _base; }

    [[nodiscard]] auto per_group_contexts() const -> bool { return _groups != nullptr; }

    /// The context that protects a request in `group` (nullopt: the request
    /// names none).
    ///
    /// Keyed by group alone, not by (peer, group). Every peer is reached with
    /// the one credential set this client was configured with, so two peers'
    /// contexts for one group would derive the same Sender Key and Common IV
    /// and count Partial IVs independently from zero: the same nonce under the
    /// same key, twice. One context per group shares one counter across peers,
    /// which is what the base context has always done for ungrouped traffic.
    [[nodiscard]] auto request_context(std::optional<std::uint64_t> group) -> context_ptr {
        if (!_groups || !group) {
            return _base;
        }
        return _groups->sender_context(std::string{k_registry_peer}, *group);
    }

    /// The context that verifies an incoming protected request, and the group
    /// it belongs to when the request named one. Throws
    /// oscore::verification_error, deriving nothing, when no context can be
    /// selected.
    [[nodiscard]] auto verifying_context(const oscore::coap_message& outer)
        -> std::pair<context_ptr, std::optional<std::uint64_t>> {
        oscore::option_fields fields;
        bool found = false;
        for (const auto& option : outer.options) {
            if (option.number == oscore::coap_option_oscore) {
                fields = oscore::decode_option(option.value);
                found = true;
                break;
            }
        }
        if (!found) {
            throw oscore::verification_error("request carries no OSCORE option");
        }
        // Before the registry is consulted, so an invented kid costs nothing:
        // the registry would otherwise create state for it.
        if (!fields.has_kid || fields.kid != _creds.recipient_id) {
            throw oscore::verification_error("no Recipient Context matches the request's kid");
        }
        if (!fields.has_kid_context) {
            return {_base, std::nullopt};
        }
        if (!_groups) {
            // A kid context with groups off can only be the configured one,
            // which security_context::unprotect_request() checks.
            return {_base, std::nullopt};
        }
        auto context = _groups->recipient_context(std::string{k_registry_peer}, fields.kid,
                                                  fields.kid_context);
        return {std::move(context), oscore::group_of_id_context(fields.kid_context)};
    }

    /// Wipes every context of `group_id` (Requirement 4.9). Call when the
    /// group's local replica is destroyed.
    auto forget_group(std::uint64_t group_id) -> void {
        if (_groups) {
            _groups->forget_group(group_id);
        }
    }

    [[nodiscard]] auto group_counters() const -> std::optional<oscore::group_context_counters> {
        if (!_groups) {
            return std::nullopt;
        }
        return _groups->counters();
    }

private:
    /// The registry is keyed by peer so that a deployment with a master secret
    /// per peer can use it; this provider has one credential set for every
    /// peer, so it uses one key.
    static constexpr std::string_view k_registry_peer = "configured-credentials";

#ifdef LIBCOAP_AVAILABLE
    static auto check_capability() -> void {
        if (coap_oscore_is_supported() != 0) {
            throw coap_unsupported_security_mode_error(
                coap_auth_mode::oscore,
                "the linked libcoap has its own OSCORE compiled in, which intercepts every "
                "OSCORE request before Kythira's handler can see it; build libcoap with "
                "-DENABLE_OSCORE=OFF (vcpkg-overlays/libcoap does)");
        }
    }
#endif
    oscore_credentials _creds;
    coap_security_role _role;
    context_ptr _base;
    std::unique_ptr<oscore::group_context_registry> _groups;
};

// ── factory ────────────────────────────────────────────────────────────────

inline auto make_security_provider(const coap_security_config& config, coap_security_role role)
    -> std::unique_ptr<coap_security_provider> {
    switch (config.mode) {
        case coap_auth_mode::none:
            return std::make_unique<no_auth_provider>();
        case coap_auth_mode::dtls_psk: {
            if (!std::holds_alternative<psk_credentials>(config.credentials)) {
                throw coap_security_config_error(
                    "security.mode == dtls_psk requires psk_credentials in security.credentials");
            }
            return std::make_unique<dtls_psk_provider>(
                std::get<psk_credentials>(config.credentials), role);
        }
        case coap_auth_mode::dtls_pki: {
            if (!std::holds_alternative<pki_credentials>(config.credentials)) {
                throw coap_security_config_error(
                    "security.mode == dtls_pki requires pki_credentials in security.credentials");
            }
            return std::make_unique<dtls_pki_provider>(
                std::get<pki_credentials>(config.credentials), role);
        }
        case coap_auth_mode::dtls_rpk: {
            if (!std::holds_alternative<rpk_credentials>(config.credentials)) {
                throw coap_security_config_error(
                    "security.mode == dtls_rpk requires rpk_credentials in security.credentials");
            }
            return std::make_unique<dtls_rpk_provider>(
                std::get<rpk_credentials>(config.credentials), role);
        }
        case coap_auth_mode::oscore: {
            if (!std::holds_alternative<oscore_credentials>(config.credentials)) {
                throw coap_security_config_error(
                    "security.mode == oscore requires oscore_credentials in "
                    "security.credentials");
            }
            return std::make_unique<oscore_provider>(
                std::get<oscore_credentials>(config.credentials), role);
        }
    }
    throw coap_security_config_error("Unknown coap_auth_mode");
}

// ── legacy field translation (Requirement 8) ──────────────────────────────
// translate_legacy_fields() moved to raft/coap_transport_config.hpp, which
// this header pulls in transitively via raft/coap_transport.hpp, so every
// existing call site keeps compiling unchanged. It moved because it is a pure
// function of coap_client_config/coap_server_config that touches no libcoap
// type, and the libnyoci backend has to reach the same verdict about a given
// config while being unable to include *this* header at all — libcoap's and
// libnyoci's C headers cannot share a translation unit (see the comment at the
// top of coap_transport_config.hpp).

}  // namespace kythira
