// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Turns pki_credentials::cipher_suites (and the legacy
// coap_client_config/coap_server_config::cipher_suites it is translated from)
// into an OpenSSL cipher list, for every CoAP backend that runs DTLS over
// OpenSSL: libcoap (built with its OpenSSL TLS backend), cantcoap and
// libnyoci.
//
// Names are accepted in either spelling. The configuration documents and the
// tests use the IANA/RFC names ("TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256"),
// which OpenSSL's cipher-string parser does not understand, so each one is
// translated with OPENSSL_cipher_name() first; anything that is not an IANA
// name is passed through as an OpenSSL name ("ECDHE-ECDSA-AES128-GCM-SHA256").
//
// Every entry must select at least one suite DTLS 1.2 can negotiate in this
// OpenSSL. SSL_CTX_set_cipher_list() itself only fails when *nothing* in the
// list matches, so a typo next to one valid name would otherwise be dropped
// silently -- the very failure this header exists to stop. TLS 1.3 suite
// names (TLS_AES_128_GCM_SHA256 and friends) are rejected for the same
// reason: DTLS 1.2 cannot use them.

#include <raft/coap_security.hpp>

#include <openssl/ssl.h>

#include <memory>
#include <string>
#include <vector>

namespace kythira::detail {

// The suites a backend that owns its SSL_CTX (cantcoap, libnyoci) offers when
// no cipher_suites are configured. OpenSSL's own DEFAULT list, which those
// contexts used to fall back to, still carries static-RSA key exchange and
// CBC-SHA1 suites: no forward secrecy, and the MAC-then-encrypt construction
// behind Lucky13. libcoap hard-codes its own list (COAP_OPENSSL_CIPHERS), so
// these do not apply to it.
//
// Certificates and raw public keys: ECDHE key exchange with an AEAD only.
// ECDHE-ECDSA-AES128-CCM8, the suite RFC 7252 section 9.1.3.2 makes
// mandatory, is in the AESCCM group. No "!" exclusions: OpenSSL can never
// re-add a suite a "!" removed, and libnyoci appends the PSK list to this one.
// "ECDHE" already excludes ECDHE-PSK, and nothing selected is anonymous.
inline constexpr const char* default_dtls_cert_cipher_list =
    "ECDHE+AESGCM:ECDHE+CHACHA20:ECDHE+AESCCM";

// Pre-shared keys: AEAD suites authenticated by the PSK, including the
// forward-secret (EC)DHE-PSK ones where the peer offers them. PSK-AES128-CCM8,
// mandatory under RFC 7252 section 9.1.3.1, is in the AESCCM group. RSA-PSK is
// left out: it needs a server certificate a PSK deployment does not have.
inline constexpr const char* default_dtls_psk_cipher_list = "aPSK+AESGCM:aPSK+CHACHA20:aPSK+AESCCM";

/// Restrict `ctx` to one of the default lists above.
inline auto apply_default_dtls_cipher_list(SSL_CTX* ctx, const std::string& list) -> void {
    if (SSL_CTX_set_cipher_list(ctx, list.c_str()) != 1) {
        throw coap_security_error("no default DTLS cipher suite ('" + list +
                                  "') is available in this OpenSSL");
    }
}

/// The OpenSSL spelling of one configured cipher suite name.
[[nodiscard]] inline auto openssl_cipher_name_for(const std::string& configured) -> std::string {
    const char* translated = OPENSSL_cipher_name(configured.c_str());
    if (translated != nullptr && std::string(translated) != "(NONE)") {
        return translated;
    }
    return configured;
}

/// Build the OpenSSL cipher list for `suites`, or "" when none are
/// configured (meaning: keep the backend's defaults).
///
/// Throws coap_security_config_error naming the first entry that selects no
/// DTLS cipher suite in this OpenSSL.
[[nodiscard]] inline auto dtls_cipher_list(const std::vector<std::string>& suites) -> std::string {
    if (suites.empty()) {
        return {};
    }
    const std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> probe(SSL_CTX_new(DTLS_method()),
                                                                  &SSL_CTX_free);
    if (!probe) {
        throw coap_security_error("failed to allocate a DTLS context to check cipher suites");
    }
    std::string list;
    for (const auto& suite : suites) {
        if (suite.empty()) {
            throw coap_security_config_error("empty DTLS cipher suite name in configuration");
        }
        const auto name = openssl_cipher_name_for(suite);
        if (SSL_CTX_set_cipher_list(probe.get(), name.c_str()) != 1) {
            throw coap_security_config_error("DTLS cipher suite '" + suite +
                                             "' is not available for DTLS 1.2 in this OpenSSL");
        }
        if (!list.empty()) {
            list.push_back(':');
        }
        list += name;
    }
    return list;
}

/// Restrict `ctx` to `list` (from dtls_cipher_list()); a no-op for "".
inline auto apply_dtls_cipher_list(SSL_CTX* ctx, const std::string& list) -> void {
    if (list.empty()) {
        return;
    }
    if (SSL_CTX_set_cipher_list(ctx, list.c_str()) != 1) {
        throw coap_security_config_error("failed to apply the configured DTLS cipher suites ('" +
                                         list + "')");
    }
}

/// Restrict one connection to `list`; returns false if OpenSSL refused it.
[[nodiscard]] inline auto apply_dtls_cipher_list(SSL* ssl, const std::string& list) -> bool {
    return list.empty() || SSL_set_cipher_list(ssl, list.c_str()) == 1;
}

}  // namespace kythira::detail
