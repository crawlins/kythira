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
#include <raft/oscore_sequence_store.hpp>

#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#ifdef LIBCOAP_AVAILABLE
#include <coap3/coap.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

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
// Requirement 4. libcoap applies OSCORE protection transparently within its
// own send/receive pipeline once a session/context is OSCORE-configured —
// there is no separate app-level "protect this PDU" entry point in this
// library's public API (coap_oscore.h exposes only session/context-level
// setup), the same way DTLS protection has no per-PDU call either. protect()
// / unprotect() therefore stay identity passthroughs (coap_security_provider
// base class default); the real work happens in configure_session() (server)
// and create_client_session() (client, since coap_new_client_session_oscore()
// takes the OSCORE configuration directly and consumes/frees it per call).

class oscore_provider final : public coap_security_provider {
public:
    oscore_provider(oscore_credentials creds, coap_security_role role)
        : _creds(std::move(creds)),
          _role(role),
          _sequence{oscore::sequence_store_for(_creds.sequence_state_dir),
                    oscore::security_context::state_key(_creds, "SSN", _creds.sender_id, {})} {}

    auto configure_session(coap_context_t* ctx) -> void override {
#ifdef LIBCOAP_AVAILABLE
        check_capability();
        if (_role == coap_security_role::server) {
            auto* conf = build_oscore_conf();
            if (coap_context_oscore_server(ctx, conf) == 0) {
                throw coap_security_error("Failed to configure OSCORE server context");
            }
        }
        // Client-side: the OSCORE context is built fresh per session in
        // create_client_session(), since coap_new_client_session_oscore()
        // consumes (frees) the coap_oscore_conf_t it's given.
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
        auto* conf = build_oscore_conf();
        return coap_new_client_session_oscore(ctx, local_if, server_addr,
                                              static_cast<coap_proto_t>(proto), conf);
#else
        (void)ctx;
        (void)local_if;
        (void)server_addr;
        (void)proto;
        throw coap_unsupported_security_mode_error(coap_auth_mode::oscore,
                                                   "libcoap not compiled into this build");
#endif
    }

    // Adds a known peer's recipient ID to an already-OSCORE-configured
    // server context (Requirement 4.3), e.g. once per Raft peer discovered
    // after startup.
    auto add_recipient(coap_context_t* ctx, const std::vector<std::byte>& recipient_id) -> void {
#ifdef LIBCOAP_AVAILABLE
        // coap_new_oscore_recipient() takes ownership of *rid (it stores
        // the pointer directly in its internal recipient chain and later
        // frees it via coap_delete_bin_const(), including on the
        // duplicate-recipient rejection path) — it must be a
        // coap_new_bin_const() heap allocation, not a stack-local struct,
        // or the eventual free() corrupts the heap.
        coap_bin_const_t* rid = coap_new_bin_const(
            reinterpret_cast<const uint8_t*>(recipient_id.data()), recipient_id.size());
        if (rid == nullptr) {
            throw coap_security_error("Failed to allocate OSCORE recipient ID");
        }
        if (coap_new_oscore_recipient(ctx, rid) == 0) {
            throw coap_security_error("Failed to add OSCORE recipient");
        }
#else
        (void)ctx;
        (void)recipient_id;
#endif
    }

    [[nodiscard]] auto mode() const -> coap_auth_mode override { return coap_auth_mode::oscore; }

    [[nodiscard]] auto credentials() const -> const oscore_credentials& { return _creds; }

private:
    // Where this provider's libcoap contexts take their Sender Sequence
    // Numbers from: the same store and key a security_context built from these
    // credentials would use. Outlives every libcoap context it is handed to,
    // since the transports free their coap_context_t before their provider.
    struct sequence_state {
        std::shared_ptr<oscore::sequence_store> store;
        std::string key;
    };

    // Numbers reserved per libcoap OSCORE context. libcoap counts on its own
    // from the start it is given and cannot be capped, so each context gets a
    // block large enough that it is not expected to run past it: 2^24 messages
    // to one peer, and 2^16 contexts before the 40-bit space is spent.
    static constexpr std::uint64_t libcoap_context_block = std::uint64_t{1} << 24;

#ifdef LIBCOAP_AVAILABLE
    // libcoap's coap_oscore_save_seq_num_t: called with a value at or above
    // every Sender Sequence Number the context has used, every ssn_freq
    // numbers. Inside the reserved block this writes nothing; past it, it
    // keeps the durable high-water above what was used, so a restart still
    // starts beyond it.
    static auto save_sequence(uint64_t sender_seq_num, void* param) -> int {
        auto* state = static_cast<sequence_state*>(param);
        try {
            state->store->raise_to(
                state->key, oscore::memory_sequence_store::saturating_add(sender_seq_num, 1));
            return 1;
        } catch (...) {
            return 0;
        }
    }

    static auto check_capability() -> void {
        if (coap_oscore_is_supported() == 0) {
            throw coap_unsupported_security_mode_error(coap_auth_mode::oscore,
                                                       "OSCORE not compiled into linked libcoap");
        }
    }

    [[nodiscard]] auto build_oscore_conf() const -> coap_oscore_conf_t* {
        std::ostringstream conf_text;
        conf_text << "master_secret,hex,\"" << detail::bytes_to_hex(_creds.master_secret) << "\"\n";
        if (!_creds.master_salt.empty()) {
            conf_text << "master_salt,hex,\"" << detail::bytes_to_hex(_creds.master_salt) << "\"\n";
        }
        conf_text << "sender_id,hex,\"" << detail::bytes_to_hex(_creds.sender_id) << "\"\n";
        conf_text << "recipient_id,hex,\"" << detail::bytes_to_hex(_creds.recipient_id) << "\"\n";
        conf_text << "aead_alg,text,\"" << _creds.aead_algorithm << "\"\n";
        // RFC 8613 Appendix B.1.2 stays on (libcoap's default). It is not
        // optional: libcoap's server only runs its replay window once a
        // recipient has answered an Echo challenge, so with it off every
        // request is accepted however often it is replayed. The challenge is
        // a 4.01 + Echo on first contact, which libcoap's own client answers
        // by resending with the Echo option, below this provider. It is also
        // what makes a restarted server safe: its window starts empty, and the
        // Echo round trip re-establishes it before anything is accepted.
        conf_text << "rfc8613_b_1_2,bool,true\n";
        // RFC 8613 Appendix B.1.1: start above every number this key has
        // used, in this process or before a restart, instead of at zero.
        conf_text << "ssn_freq,integer," << _sequence.store->sequence_block() << "\n";
        auto text = conf_text.str();
        coap_str_const_t conf_mem;
        conf_mem.s = reinterpret_cast<const uint8_t*>(text.c_str());
        conf_mem.length = text.size();
        const auto start = _sequence.store->reserve(_sequence.key, libcoap_context_block);
        auto* conf = coap_new_oscore_conf(conf_mem, &oscore_provider::save_sequence,
                                          const_cast<sequence_state*>(&_sequence), start);
        if (conf == nullptr) {
            throw coap_security_error("Failed to parse OSCORE configuration");
        }
        return conf;
    }
#endif
    oscore_credentials _creds;
    coap_security_role _role;
    sequence_state _sequence;
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
