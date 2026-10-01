// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file ca_http_helpers.hpp
/// @brief JSON/TLS-peer-certificate helpers shared by the `/v1/*` HTTP route
///        handlers of `cmd/ca_service/main.cpp` (--serve mode) and
///        `cmd/ca_cluster_node/main.cpp` — the same wire format and mTLS
///        renewal semantics apply to both (Requirement 17.6: "the same
///        client-facing HTTP API as ca_service --serve").

#include <raft/certificate_authority.hpp>

#include <boost/json.hpp>

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#endif

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace raft::testing {

namespace ca_http_helpers_detail {
struct x509_deleter {
    void operator()(X509* c) const noexcept {
        if (c != nullptr) {
            X509_free(c);
        }
    }
};
}  // namespace ca_http_helpers_detail

using x509_ptr_generic = std::unique_ptr<X509, ca_http_helpers_detail::x509_deleter>;

inline auto pem_material_to_json(const pem_material& mat) -> boost::json::object {
    boost::json::object obj;
    obj["certificate_pem"] = mat.certificate_pem;
    obj["chain_pem"] = mat.chain_pem;
    return obj;
}

// Upper bound on a caller-requested leaf lifetime. Requests used to be passed
// straight through as `hours(24 * int)`, so a caller (including an
// unauthenticated-by-token /renew caller) could mint a near-permanent
// certificate, or overflow the int multiplication. 825 days matches the
// historical CA/Browser Forum ceiling for publicly-trusted leaves.
inline constexpr int k_max_validity_days = 825;

// Parses an optional `validity_days` JSON field into a validity duration.
// Returns std::nullopt when absent; throws std::invalid_argument when present
// but not an integer in [1, k_max_validity_days].
inline auto parse_validity_days(const boost::json::object& obj)
    -> std::optional<std::chrono::seconds> {
    const auto* v = obj.if_contains("validity_days");
    if (v == nullptr) {
        return std::nullopt;
    }
    std::int64_t days = 0;
    if (v->is_int64()) {
        days = v->as_int64();
    } else if (v->is_uint64() &&
               v->as_uint64() <= static_cast<std::uint64_t>(k_max_validity_days)) {
        days = static_cast<std::int64_t>(v->as_uint64());
    } else if (!v->is_uint64()) {
        throw std::invalid_argument("validity_days must be an integer");
    } else {
        days = k_max_validity_days + 1;
    }
    if (days < 1 || days > k_max_validity_days) {
        throw std::invalid_argument("validity_days must be between 1 and " +
                                    std::to_string(k_max_validity_days));
    }
    return std::chrono::hours(24 * days);
}

// Throws std::invalid_argument (mapped to HTTP 400 by every caller) for a
// malformed field or an out-of-range validity_days; SAN syntax itself is
// validated when the certificate is built (detail::validate_san_entries).
inline auto parse_csr_signing_options(const boost::json::object& obj) -> csr_signing_options {
    csr_signing_options opts;
    if (const auto* v = obj.if_contains("dns_names"); (v != nullptr) && v->is_array()) {
        for (const auto& e : v->as_array()) {
            opts.dns_names.push_back(std::string(e.as_string()));
        }
    }
    if (const auto* v = obj.if_contains("ip_addresses"); (v != nullptr) && v->is_array()) {
        for (const auto& e : v->as_array()) {
            opts.ip_addresses.push_back(std::string(e.as_string()));
        }
    }
    if (const auto* v = obj.if_contains("server_auth"); (v != nullptr) && v->is_bool()) {
        opts.server_auth = v->as_bool();
    }
    if (const auto* v = obj.if_contains("client_auth"); (v != nullptr) && v->is_bool()) {
        opts.client_auth = v->as_bool();
    }
    if (auto validity = parse_validity_days(obj)) {
        opts.validity = *validity;
    }
    return opts;
}

// ── Raft peer enrollment authentication (ca_cluster_node) ───────────────────
//
// ca_cluster_node mints each node's Raft RPC identity through the same
// /v1/certificates route that ordinary API clients use, and RPC TLS trusts
// certificates chaining to the cluster root. Gating peer enrollment only on
// the shared client bearer token meant ANY API client could request a
// certificate naming "ca-cluster-node-<n>" and then speak Raft RPC as that
// node — including InstallSnapshot, which replaces the replicated CA state
// wholesale (the same shape as OpenBao's cert-auth → snapshot-force chain).
//
// Peer enrollment is therefore authenticated with a key only cluster nodes
// hold: an HMAC key derived from the unseal passphrase every node already
// needs (to decrypt the CA key when it becomes leader). API clients never see
// that passphrase, so the bearer token alone can no longer mint a peer
// identity. The same key authenticates the root certificate a follower
// fetches over the (unverified-TLS) intra-cluster HTTP path, so a network
// attacker can no longer substitute its own root as the RPC trust anchor.

/// DNS SAN prefix reserved for Raft peer identities. Only an authenticated
/// peer enrollment may obtain a certificate carrying a name with this prefix.
inline constexpr std::string_view k_reserved_peer_dns_prefix = "ca-cluster-node-";

/// The single DNS SAN (and CN) a node's Raft RPC identity carries.
[[nodiscard]] inline auto peer_identity_dns_name(std::uint64_t node_id) -> std::string {
    return std::string(k_reserved_peer_dns_prefix) + std::to_string(node_id);
}

/// True iff `name` falls in the reserved peer-identity namespace. Compared
/// case-insensitively, because DNS names are, and a "CA-Cluster-Node-1" SAN
/// must not slip past a case-sensitive check while still matching the peer
/// name in any consumer that compares the way DNS does.
[[nodiscard]] inline auto is_reserved_peer_dns_name(std::string_view name) -> bool {
    if (name.size() < k_reserved_peer_dns_prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < k_reserved_peer_dns_prefix.size(); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != k_reserved_peer_dns_prefix[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline auto hex_encode(const unsigned char* data, std::size_t len) -> std::string {
    static constexpr char k_digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(k_digits[data[i] >> 4]);
        out.push_back(k_digits[data[i] & 0x0f]);
    }
    return out;
}

[[nodiscard]] inline auto hmac_sha256_hex(const std::string& key, const std::string& message)
    -> std::string {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;
    if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(message.data()), message.size(), mac,
             &mac_len) == nullptr) {
        throw std::runtime_error("ca_http_helpers: HMAC-SHA256 failed");
    }
    return hex_encode(mac, mac_len);
}

/// Constant-time equality for secrets/MACs (length is not secret).
[[nodiscard]] inline auto constant_time_equals(std::string_view a, std::string_view b) -> bool {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

/// Derives the peer-enrollment HMAC key from the cluster unseal passphrase.
/// Domain-separated so the MAC key is never the passphrase itself.
[[nodiscard]] inline auto derive_peer_enrollment_key(const std::string& unseal_passphrase)
    -> std::string {
    if (unseal_passphrase.empty()) {
        throw std::invalid_argument("ca_http_helpers: empty unseal passphrase");
    }
    return hmac_sha256_hex(unseal_passphrase, "kythira ca_cluster_node peer-enrollment v1");
}

/// HTTP header carrying a peer-enrollment MAC on POST /v1/certificates.
inline constexpr const char* k_peer_enrollment_header = "X-Kythira-Peer-Enrollment";
/// Request header carrying a follower's fresh nonce on GET /v1/root-ca, and
/// the response header carrying the leader's MAC over (nonce, root PEM).
inline constexpr const char* k_peer_nonce_header = "X-Kythira-Peer-Nonce";
inline constexpr const char* k_peer_root_mac_header = "X-Kythira-Peer-Root-Mac";

/// MAC binding a peer-enrollment request to the node id it enrolls and the
/// exact CSR being signed: replaying a captured request only ever yields a
/// certificate for the victim node's own key, which the replayer lacks.
[[nodiscard]] inline auto peer_enrollment_mac(const std::string& key, std::uint64_t node_id,
                                              const std::string& csr_pem) -> std::string {
    return hmac_sha256_hex(key, "enroll\n" + std::to_string(node_id) + "\n" + csr_pem);
}

/// Request header authenticating a peer's GET /v1/root-ca in place of the
/// client bearer token (see peer_root_request_mac).
inline constexpr const char* k_peer_request_mac_header = "X-Kythira-Peer-Request-Mac";

/// MAC a peer attaches to GET /v1/root-ca so it need not send the client
/// bearer token over an intra-cluster link whose TLS it does not verify. A
/// replay only fetches the (public) root again, so binding to the request
/// nonce alone suffices; the response is separately MAC'd (peer_root_mac).
[[nodiscard]] inline auto peer_root_request_mac(const std::string& key, const std::string& nonce)
    -> std::string {
    return hmac_sha256_hex(key, "root-ca-request\n" + nonce);
}

/// MAC over a /v1/root-ca response body, bound to the requester's nonce so a
/// stale or attacker-chosen root cannot be replayed into a later fetch.
[[nodiscard]] inline auto peer_root_mac(const std::string& key, const std::string& nonce,
                                        const std::string& root_pem) -> std::string {
    return hmac_sha256_hex(key, "root-ca\n" + nonce + "\n" + root_pem);
}

/// A fresh random nonce, hex encoded.
[[nodiscard]] inline auto random_nonce_hex() -> std::string {
    unsigned char buf[16];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        throw std::runtime_error("ca_http_helpers: RAND_bytes failed");
    }
    return hex_encode(buf, sizeof(buf));
}

/// Accepts only short lowercase/uppercase hex nonces, so a requester cannot
/// steer the MAC'd message's framing.
[[nodiscard]] inline auto is_well_formed_nonce(std::string_view nonce) -> bool {
    if (nonce.empty() || nonce.size() > 128) {
        return false;
    }
    for (char c : nonce) {
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

/// Result of classifying a POST /v1/certificates request.
struct peer_enrollment_decision {
    bool allowed{true};
    std::string error;                     ///< Set when !allowed.
    std::optional<std::uint64_t> node_id;  ///< Set for an authenticated peer enrollment.
};

/// Decides whether a /v1/certificates request may proceed, given its parsed
/// options, the optional `rpc_tls_ready_node_id` body field, the enrollment
/// MAC header (empty if absent), the cluster's node ids and the enrollment
/// key. Ordinary client requests (no reserved name, no node id) pass
/// unchanged. Any request touching peer identity — a reserved
/// "ca-cluster-node-*" SAN, or an `rpc_tls_ready_node_id` (which commits
/// replicated cutover state) — must be a well-formed enrollment for exactly
/// one configured node and carry a valid MAC.
[[nodiscard]] inline auto classify_peer_enrollment(
    const csr_signing_options& options, std::optional<std::uint64_t> ready_node_id,
    const std::string& mac_header, const std::set<std::uint64_t>& cluster_node_ids,
    const std::string& enrollment_key, const std::string& csr_pem) -> peer_enrollment_decision {
    bool wants_reserved_name = false;
    for (const auto& d : options.dns_names) {
        if (is_reserved_peer_dns_name(d)) {
            wants_reserved_name = true;
        }
    }
    if (!wants_reserved_name && !ready_node_id.has_value()) {
        return {};
    }
    auto deny = [](std::string why) {
        return peer_enrollment_decision{false, std::move(why), std::nullopt};
    };
    if (!ready_node_id.has_value()) {
        return deny("names with the \"ca-cluster-node-\" prefix are reserved for Raft peers");
    }
    if (!cluster_node_ids.contains(*ready_node_id)) {
        return deny("rpc_tls_ready_node_id is not a member of this cluster");
    }
    if (options.dns_names.size() != 1 ||
        options.dns_names.front() != peer_identity_dns_name(*ready_node_id) ||
        !options.ip_addresses.empty()) {
        return deny("a peer enrollment must request exactly the node's own peer name");
    }
    if (mac_header.empty() ||
        !constant_time_equals(mac_header,
                              peer_enrollment_mac(enrollment_key, *ready_node_id, csr_pem))) {
        return deny("peer enrollment is not authenticated");
    }
    return peer_enrollment_decision{true, {}, ready_node_id};
}

// Computes the colon-separated, uppercase-hex SHA-256 fingerprint of `cert`
// (Requirement 19.1) — the standard "openssl x509 -fingerprint -sha256"
// format, e.g. "AA:BB:CC:...". Uses X509_digest rather than hashing the DER
// bytes manually, matching how OpenSSL's own CLI computes it.
[[nodiscard]] inline auto sha256_fingerprint_hex(X509* cert) -> std::string {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (X509_digest(cert, EVP_sha256(), digest, &digest_len) != 1) {
        throw std::runtime_error("ca_http_helpers: X509_digest failed");
    }
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned int i = 0; i < digest_len; ++i) {
        if (i > 0) {
            out << ':';
        }
        out.width(2);
        out.fill('0');
        out << static_cast<unsigned int>(digest[i]);
    }
    return out.str();
}

// Parses every PEM certificate block in `pem_bundle` (a --tls-cert file may
// contain just a leaf, or a leaf+intermediates+root chain) and returns the
// LAST one — by PEM bundle convention the root/topmost issuer — unless an
// earlier block is self-signed (issuer == subject), in which case that one
// is returned instead, since a self-signed cert is unambiguously a root
// regardless of its position in the file. Throws if the bundle contains no
// parseable certificate.
[[nodiscard]] inline auto root_cert_from_pem_bundle(const std::string& pem_bundle)
    -> x509_ptr_generic {
    BIO* bio = BIO_new_mem_buf(pem_bundle.data(), static_cast<int>(pem_bundle.size()));
    if (bio == nullptr) {
        throw std::runtime_error("ca_http_helpers: BIO_new_mem_buf failed");
    }
    std::vector<x509_ptr_generic> certs;
    while (true) {
        X509* c = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        if (c == nullptr) {
            break;
        }
        certs.emplace_back(c);
    }
    BIO_free(bio);
    if (certs.empty()) {
        throw std::runtime_error("ca_http_helpers: no certificate found in PEM bundle");
    }
    for (auto& c : certs) {
        if (X509_NAME_cmp(X509_get_subject_name(c.get()), X509_get_issuer_name(c.get())) == 0) {
            return std::move(c);
        }
    }
    return std::move(certs.back());
}

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT

// Extracts dns_names/ip_addresses/server_auth/client_auth from a presented
// certificate's subjectAltName/extendedKeyUsage extensions, for
// POST /v1/certificates/renew (Requirement 15.3): "the returned certificate
// SHALL carry the same subject and SAN entries as the presented certificate."
inline auto options_from_presented_cert(X509* cert) -> csr_signing_options {
    csr_signing_options opts;

    auto* san_ext =
        static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (san_ext != nullptr) {
        for (int i = 0; i < sk_GENERAL_NAME_num(san_ext); ++i) {
            GENERAL_NAME* name = sk_GENERAL_NAME_value(san_ext, i);
            if (name->type == GEN_DNS) {
                auto* s = name->d.dNSName;
                opts.dns_names.emplace_back(reinterpret_cast<const char*>(ASN1_STRING_get0_data(s)),
                                            static_cast<std::size_t>(ASN1_STRING_length(s)));
            } else if (name->type == GEN_IPADD) {
                auto* s = name->d.iPAddress;
                const unsigned char* bytes = ASN1_STRING_get0_data(s);
                int len = ASN1_STRING_length(s);
                char buf[INET6_ADDRSTRLEN] = {};
                if (len == 4 && inet_ntop(AF_INET, bytes, buf, sizeof(buf)) != nullptr) {
                    opts.ip_addresses.emplace_back(buf);
                } else if (len == 16 && inet_ntop(AF_INET6, bytes, buf, sizeof(buf)) != nullptr) {
                    opts.ip_addresses.emplace_back(buf);
                }
            }
        }
        GENERAL_NAMES_free(san_ext);
    }

    bool has_eku = (X509_get_extension_flags(cert) & EXFLAG_XKUSAGE) != 0u;
    if (has_eku) {
        std::uint32_t xkusage = X509_get_extended_key_usage(cert);
        opts.server_auth = (xkusage & XKU_SSL_SERVER) != 0;
        opts.client_auth = (xkusage & XKU_SSL_CLIENT) != 0;
    } else {
        opts.server_auth = true;
        opts.client_auth = true;
    }
    return opts;
}

// Chain-verifies `cert` against `root_pem`. Returns true iff it verifies.
inline auto cert_chains_to_root(X509* cert, const std::string& root_pem) -> bool {
    BIO* bio = BIO_new_mem_buf(root_pem.data(), static_cast<int>(root_pem.size()));
    X509* root = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (root == nullptr) {
        return false;
    }

    X509_STORE* store = X509_STORE_new();
    X509_STORE_add_cert(store, root);
    X509_STORE_CTX* ctx = X509_STORE_CTX_new();
    X509_STORE_CTX_init(ctx, store, cert, nullptr);
    int rc = X509_verify_cert(ctx);
    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);
    X509_free(root);
    return rc == 1;
}

// POST /v1/certificates/renew re-issues "the same subject and SAN entries as
// the presented certificate". SANs/EKU are copied from the presented cert, but
// the subject used to come from the caller's fresh CSR, so any certificate
// holder could renew into an arbitrary subject DN (e.g. the subject another
// service maps to an admin identity, such as redis_gateway's `cert=` users).
// Returns true iff the CSR's subject is byte-for-byte the presented subject.
[[nodiscard]] inline auto csr_subject_matches_cert(X509* presented, const std::string& csr_pem)
    -> bool {
    BIO* bio = BIO_new_mem_buf(csr_pem.data(), static_cast<int>(csr_pem.size()));
    if (bio == nullptr) {
        return false;
    }
    X509_REQ* req = PEM_read_bio_X509_REQ(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (req == nullptr) {
        return false;
    }
    bool same =
        X509_NAME_cmp(X509_REQ_get_subject_name(req), X509_get_subject_name(presented)) == 0;
    X509_REQ_free(req);
    return same;
}

// The presented certificate's serial as a uint64, or std::nullopt if it does
// not fit (this CA only ever issues 64-bit serials).
[[nodiscard]] inline auto cert_serial_u64(X509* cert) -> std::optional<std::uint64_t> {
    std::uint64_t serial = 0;
    if (ASN1_INTEGER_get_uint64(&serial, X509_get0_serialNumber(cert)) != 1) {
        return std::nullopt;
    }
    return serial;
}

// True iff `cert`'s serial appears in the PEM CRL `crl_pem`. An unparseable
// CRL is treated as "revoked" (fail closed) by the renew handlers' callers.
[[nodiscard]] inline auto cert_revoked_in_crl(X509* cert, const std::string& crl_pem) -> bool {
    BIO* bio = BIO_new_mem_buf(crl_pem.data(), static_cast<int>(crl_pem.size()));
    if (bio == nullptr) {
        return true;
    }
    X509_CRL* crl = PEM_read_bio_X509_CRL(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (crl == nullptr) {
        return true;
    }
    X509_REVOKED* entry = nullptr;
    int found = X509_CRL_get0_by_serial(crl, &entry, X509_get0_serialNumber(cert));
    X509_CRL_free(crl);
    return found == 1;
}

// True iff `cert` carries at least one DNS SAN contained in `names`.
[[nodiscard]] inline auto cert_has_dns_san_in(X509* cert, const std::set<std::string>& names)
    -> bool {
    auto* sans =
        static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (sans == nullptr) {
        return false;
    }
    bool hit = false;
    for (int i = 0; i < sk_GENERAL_NAME_num(sans) && !hit; ++i) {
        GENERAL_NAME* gn = sk_GENERAL_NAME_value(sans, i);
        if (gn->type != GEN_DNS) {
            continue;
        }
        std::string value(reinterpret_cast<const char*>(ASN1_STRING_get0_data(gn->d.dNSName)),
                          static_cast<std::size_t>(ASN1_STRING_length(gn->d.dNSName)));
        hit = names.contains(value);
    }
    GENERAL_NAMES_free(sans);
    return hit;
}

// The TLS layer only *requests* a client certificate (Requirement 15.3's
// design: SSL_VERIFY_PEER without SSL_VERIFY_FAIL_IF_NO_PEER_CERT) so that
// bearer-token routes keep accepting connections with no client certificate
// at all. OpenSSL's default verify logic would otherwise abort the handshake
// for ANY presented certificate, since no CA trust store is configured on
// this SSL_CTX — the real chain-to-root check happens at the application
// level in the /v1/certificates/renew handler via cert_chains_to_root().
// This callback simply lets the handshake complete so that check can run.
inline auto accept_any_peer_certificate(int, X509_STORE_CTX*) -> int {
    return 1;
}

#endif  // CPPHTTPLIB_OPENSSL_SUPPORT

}  // namespace raft::testing
