#pragma once

/// @file ca_bootstrap_client.hpp
/// @brief Pinned-fingerprint first-contact bootstrap helper (Requirement 19).
///
/// An operator distributing a bearer token to a new instance so it can
/// request its first certificate has no prior trust relationship to
/// chain-verify a `ca_service --serve` / `ca_cluster_node`'s TLS listener
/// against — it hasn't been issued anything by that CA yet. This helper
/// closes that gap: given the SAME out-of-band channel already used to
/// distribute the bearer token (an EnvironmentFile, user-data, a secrets
/// manager entry), also distribute the server's root-certificate SHA-256
/// fingerprint (printed by `--print-root-fingerprint`, Requirement 19.2).
/// `fetch_trusted_root()` connects over TLS with normal chain verification
/// disabled, inspects the actual presented certificate chain's root during
/// the handshake, and accepts the connection ONLY on an exact fingerprint
/// match — never falling back to unpinned verification. On success it fetches
/// `GET /v1/root-ca` over that same already-validated connection and returns
/// the root PEM for the caller to use for ALL subsequent, ordinary
/// chain-verified connections (this function is meant to run exactly once
/// per instance lifetime).

#include <httplib.h>

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>

namespace raft::testing {

struct ca_bootstrap_result {
    std::string root_certificate_pem;
};

namespace ca_bootstrap_detail {

// Strips colons and upper-cases — accepts both "AA:BB:CC" and "AABBCC" forms
// (Requirement 19.3's "compare case-insensitively... accepting both
// colon-separated and bare hex").
[[nodiscard]] inline auto normalize_fingerprint(const std::string& fp) -> std::string {
    std::string out;
    out.reserve(fp.size());
    for (char c : fp) {
        if (c == ':') {
            continue;
        }
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

[[nodiscard]] inline auto is_well_formed_sha256_hex(const std::string& normalized) -> bool {
    if (normalized.size() != 64) {
        return false;
    }
    return std::all_of(normalized.begin(), normalized.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

[[nodiscard]] inline auto sha256_fingerprint_hex_bare(X509* cert) -> std::string {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (X509_digest(cert, EVP_sha256(), digest, &digest_len) != 1) {
        throw std::runtime_error("ca_bootstrap_client: X509_digest failed");
    }
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned int i = 0; i < digest_len; ++i) {
        out.width(2);
        out.fill('0');
        out << static_cast<unsigned int>(digest[i]);
    }
    return out.str();
}

// The root of a presented chain: the self-signed member if one is present,
// else the last (topmost) certificate — same convention as
// ca_http_helpers.hpp's root_cert_from_pem_bundle(), applied to a live
// STACK_OF(X509) chain instead of a parsed PEM bundle.
[[nodiscard]] inline auto root_of_chain(STACK_OF(X509) * chain) -> X509* {
    int count = sk_X509_num(chain);
    for (int i = 0; i < count; ++i) {
        X509* c = sk_X509_value(chain, i);
        if (X509_NAME_cmp(X509_get_subject_name(c), X509_get_issuer_name(c)) == 0) {
            return c;
        }
    }
    return count > 0 ? sk_X509_value(chain, count - 1) : nullptr;
}

}  // namespace ca_bootstrap_detail

/// Connects to `base_url` over TLS WITHOUT chain verification, checks the
/// presented chain's root against `expected_root_fingerprint_sha256` (hex,
/// colon-separated or bare — both accepted, case-insensitive), and on an
/// exact match fetches `GET /v1/root-ca` over that same connection —
/// carrying `Authorization: Bearer <auth_token>` when non-empty, since a real
/// `ca_service --serve`/`ca_cluster_node` requires it on every route but
/// `/healthz` (Requirement 19.6: pinning and bearer-token auth are
/// independent and composable, not alternatives). Throws
/// `std::invalid_argument` on a malformed `expected_root_fingerprint_sha256`,
/// or `std::runtime_error` on any connection failure or fingerprint
/// mismatch — naming both the expected and observed fingerprints in the
/// latter case. Never falls back to unpinned verification.
[[nodiscard]] inline auto fetch_trusted_root(std::string base_url,
                                             std::string expected_root_fingerprint_sha256,
                                             std::string auth_token = "") -> ca_bootstrap_result {
    auto normalized_expected =
        ca_bootstrap_detail::normalize_fingerprint(expected_root_fingerprint_sha256);
    if (!ca_bootstrap_detail::is_well_formed_sha256_hex(normalized_expected)) {
        throw std::invalid_argument(
            "ca_bootstrap_client: expected_root_fingerprint_sha256 is not a well-formed SHA-256 "
            "fingerprint (64 hex digits, colons optional): " +
            expected_root_fingerprint_sha256);
    }

    httplib::Client client(base_url);
    // Deliberately NOT calling enable_server_certificate_verification(false):
    // per cpp-httplib's actual SSLClient::initialize_ssl(), that flag gates
    // the ENTIRE verification block, including the custom
    // server_certificate_verifier_ callback below — disabling it would skip
    // pinning too, not just the normal chain check. Verification stays
    // enabled; the callback below is what replaces the normal chain check
    // with the pinned-fingerprint decision, returning CertificateAccepted/
    // CertificateRejected explicitly so the normal chain-verification
    // fallback (for a NoDecisionMade return) never runs either.
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(10, 0);

    std::string observed_fingerprint;
    bool saw_chain = false;
    client.set_server_certificate_verifier([&](SSL* ssl) -> httplib::SSLVerifierResponse {
        auto* chain = SSL_get_peer_cert_chain(ssl);
        if (chain == nullptr) {
            return httplib::SSLVerifierResponse::CertificateRejected;
        }
        X509* root = ca_bootstrap_detail::root_of_chain(chain);
        if (root == nullptr) {
            return httplib::SSLVerifierResponse::CertificateRejected;
        }
        saw_chain = true;
        try {
            observed_fingerprint = ca_bootstrap_detail::sha256_fingerprint_hex_bare(root);
        } catch (const std::exception&) {
            return httplib::SSLVerifierResponse::CertificateRejected;
        }
        return observed_fingerprint == normalized_expected
                   ? httplib::SSLVerifierResponse::CertificateAccepted
                   : httplib::SSLVerifierResponse::CertificateRejected;
    });

    httplib::Headers headers;
    if (!auth_token.empty()) {
        headers = {{"Authorization", "Bearer " + auth_token}};
    }

    auto res = client.Get("/v1/root-ca", headers);
    if (!res) {
        if (saw_chain && observed_fingerprint != normalized_expected) {
            throw std::runtime_error("ca_bootstrap_client: root fingerprint mismatch for " +
                                     base_url + " — expected " + normalized_expected +
                                     ", observed " + observed_fingerprint);
        }
        throw std::runtime_error("ca_bootstrap_client: connection to " + base_url +
                                 " failed: " + httplib::to_string(res.error()));
    }
    if (res->status != 200) {
        throw std::runtime_error("ca_bootstrap_client: GET " + base_url + "/v1/root-ca returned " +
                                 std::to_string(res->status) + ": " + res->body);
    }
    return ca_bootstrap_result{res->body};
}

}  // namespace raft::testing
