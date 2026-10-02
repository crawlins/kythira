// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Certificate revocation checking for the CoAP transport's DTLS peers
// (coap-transport Requirements 6.5 and 11.3).
//
// libcoap's own `check_cert_revocation` only consults CRLs that happen to be
// bundled into the CA file, and every caller in this tree pairs it with
// `allow_no_crl = 1`, so without a CRL in that file it checks nothing. This
// header does the check against an explicitly configured CRL file, from the
// CN-validation callback every PKI path already runs, so a revoked peer is
// rejected during the handshake.
//
// OpenSSL does the actual work: the CRL is added to the same X509_STORE as
// the trust anchors, and X509_verify_cert() with X509_V_FLAG_CRL_CHECK_ALL
// verifies each CRL's signature against its issuer, rejects an expired CRL,
// and fails a certificate whose serial is listed. Nothing here parses a CRL
// by hand.
//
// OCSP is deliberately not implemented. It needs an HTTP client on the
// handshake path and a policy for an unreachable responder, and nothing in
// this deployment model runs one; a CRL distributed with the CA covers the
// same need offline.

#include <raft/coap_security.hpp>

#include <optional>
#include <string>

#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

namespace kythira::coap_revocation {

namespace detail {
// Installed only when the configuration allows a missing CRL: turns
// "no CRL for this issuer" into a pass while leaving every other failure,
// a listed serial included, fatal.
inline auto tolerate_missing_crl(int ok, X509_STORE_CTX* ctx) -> int {
    if (ok == 0 && X509_STORE_CTX_get_error(ctx) == X509_V_ERR_UNABLE_TO_GET_CRL) {
        X509_STORE_CTX_set_error(ctx, X509_V_OK);
        return 1;
    }
    return ok;
}
}  // namespace detail

// Checks `cert` against the CRLs `config` names, building its chain from
// `ca_file` (plus `untrusted`, if the caller has the peer's intermediates).
// Returns std::nullopt when the certificate is acceptable, otherwise a
// human-readable reason. A disabled config always passes.
//
// Fails closed: a CRL file that cannot be read, a missing ca_file, or (unless
// allow_missing_crl) an issuer with no CRL all reject the certificate. A
// revocation check that silently passes when it cannot run is worse than
// none, because it is believed.
[[nodiscard]] inline auto check(X509* cert, const std::string& ca_file,
                                const certificate_revocation_config& config,
                                STACK_OF(X509) * untrusted = nullptr)
    -> std::optional<std::string> {
    if (!config.enabled) {
        return std::nullopt;
    }
    if (cert == nullptr) {
        return std::string{"no certificate to check"};
    }
    if (ca_file.empty()) {
        return std::string{"revocation checking needs ca_file to verify the CRL's signature"};
    }

    X509_STORE* store = X509_STORE_new();
    X509_STORE_CTX* ctx = nullptr;
    std::optional<std::string> failure;

    if (store == nullptr) {
        failure = "failed to create X509 store";
    } else if (X509_STORE_load_locations(store, ca_file.c_str(), nullptr) != 1) {
        failure = "failed to load CA file: " + ca_file;
    } else if (!config.crl_file.empty()) {
        // X509_load_crl_file reads every CRL in a PEM bundle, so one file can
        // carry the CRLs of a whole chain.
        X509_LOOKUP* lookup = X509_STORE_add_lookup(store, X509_LOOKUP_file());
        if (lookup == nullptr ||
            X509_load_crl_file(lookup, config.crl_file.c_str(), X509_FILETYPE_PEM) <= 0) {
            failure = "failed to load CRL file: " + config.crl_file;
        }
    }

    if (!failure) {
        X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
        if (config.allow_missing_crl) {
            X509_STORE_set_verify_cb(store, &detail::tolerate_missing_crl);
        }
        ctx = X509_STORE_CTX_new();
        if (ctx == nullptr || X509_STORE_CTX_init(ctx, store, cert, untrusted) != 1) {
            failure = "failed to initialise certificate verification";
        } else if (X509_verify_cert(ctx) != 1) {
            const int error = X509_STORE_CTX_get_error(ctx);
            failure = error == X509_V_ERR_CERT_REVOKED ? std::string{"certificate revoked"}
                                                       : std::string{"revocation check failed: "} +
                                                             X509_verify_cert_error_string(error);
        }
    }

    if (ctx != nullptr) {
        X509_STORE_CTX_free(ctx);
    }
    if (store != nullptr) {
        X509_STORE_free(store);
    }
    return failure;
}

}  // namespace kythira::coap_revocation
