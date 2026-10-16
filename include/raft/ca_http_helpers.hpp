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
#include <raft/certificate_serial.hpp>
#include <raft/certificate_serial_x509.hpp>
#include <raft/composite_node_id.hpp>

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
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
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
    // Named like the revoke request field that takes the same encoding.
    // Left out when the provider could not read it, rather than naming
    // serial 00.
    if (!mat.serial.is_zero()) {
        obj["serial_hex"] = mat.serial.to_hex();
    }
    return obj;
}

/// How a /v1/certificates/revoke endpoint has always read "serial".
enum class legacy_revoke_serial {
    /// A non-negative JSON integer, or a string of decimal digits that fits
    /// 64 bits: the in-house CA's endpoints (`ca_service --provider local`,
    /// `ca_cluster_node`).
    decimal,
    /// A hex string: `ca_service --provider aws-acm-pca`, which passed it to
    /// ACM Private CA verbatim.
    hex,
};

/// Parses the serial of a /v1/certificates/revoke body
/// (`.kiro/specs/cloud-composite-node-ids/` Requirement 12.5).
///
/// Exactly one of two fields must be present:
/// - "serial" keeps the meaning it has always had on that endpoint
///   (`legacy`). Decimal is strict: digits only, no sign, whitespace or
///   overflow, so a hex string such as "0a1b" is refused rather than read as
///   some other certificate's serial.
/// - "serial_hex" is any serial of up to 20 octets, in
///   `certificate_serial::parse`'s forms ("0a1b2c", "0A:1B:2C", "0a 1b 2c").
///   It is how a cloud CA's wide serial is named, and the field issuance and
///   renewal responses report.
///
/// Throws std::invalid_argument (a 400) when both or neither are present, or
/// when the value present is malformed.
[[nodiscard]] inline auto parse_revoke_serial(
    const boost::json::object& body, legacy_revoke_serial legacy = legacy_revoke_serial::decimal)
    -> certificate_serial {
    const auto* serial = body.if_contains("serial");
    const auto* serial_hex = body.if_contains("serial_hex");
    if (serial != nullptr && serial_hex != nullptr) {
        throw std::invalid_argument("give one of serial and serial_hex, not both");
    }
    if (serial_hex != nullptr) {
        const auto* text = serial_hex->if_string();
        auto parsed = text == nullptr
                          ? std::nullopt
                          : certificate_serial::parse(std::string_view(text->data(), text->size()));
        if (!parsed.has_value()) {
            throw std::invalid_argument(
                "serial_hex must be hex of at most 20 octets, e.g. \"0a1b2c\" or \"0a:1b:2c\"");
        }
        return *parsed;
    }
    if (serial == nullptr) {
        throw std::invalid_argument("serial or serial_hex is required");
    }
    if (legacy == legacy_revoke_serial::hex) {
        const auto* text = serial->if_string();
        auto parsed = text == nullptr
                          ? std::nullopt
                          : certificate_serial::parse(std::string_view(text->data(), text->size()));
        if (!parsed.has_value()) {
            throw std::invalid_argument("serial is required as a hex string");
        }
        return *parsed;
    }
    if (const auto* u = serial->if_uint64(); u != nullptr) {
        return certificate_serial::from_u64(*u);
    }
    if (const auto* i = serial->if_int64(); i != nullptr && *i >= 0) {
        return certificate_serial::from_u64(static_cast<std::uint64_t>(*i));
    }
    if (const auto* text = serial->if_string(); text != nullptr && !text->empty()) {
        std::uint64_t value = 0;
        bool ok = true;
        for (char c : *text) {
            if (c < '0' || c > '9') {
                ok = false;
                break;
            }
            const auto digit = static_cast<std::uint64_t>(c - '0');
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                ok = false;
                break;
            }
            value = value * 10 + digit;
        }
        if (ok) {
            return certificate_serial::from_u64(value);
        }
    }
    throw std::invalid_argument(
        "serial must be a non-negative integer or a decimal string that fits 64 bits; "
        "send a hex serial as serial_hex");
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
    if (const auto* v = obj.if_contains("uri_names"); (v != nullptr) && v->is_array()) {
        for (const auto& e : v->as_array()) {
            opts.uri_names.push_back(std::string(e.as_string()));
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
// identity. The same key authenticates the RPC trust state (root, cutover,
// revoked peer serials) a follower fetches from the leader, so even a peer
// link without verified TLS cannot have its trust anchor substituted.

/// DNS SAN prefix reserved for Raft peer identities. Only an authenticated
/// peer enrollment may obtain a certificate carrying a name with this prefix.
inline constexpr std::string_view k_reserved_peer_dns_prefix = "ca-cluster-node-";

/// URI SAN prefix reserved for the peer identities of textual node ids
/// (`.kiro/specs/cloud-composite-node-ids/` Requirement 12.2). Reserved the
/// same way as `k_reserved_peer_dns_prefix`.
inline constexpr std::string_view k_reserved_peer_uri_prefix = "urn:kythira:node:";

/// The single DNS SAN (and CN) a node's Raft RPC identity carries.
[[nodiscard]] inline auto peer_identity_dns_name(std::uint64_t node_id) -> std::string {
    return std::string(k_reserved_peer_dns_prefix) + std::to_string(node_id);
}

/// The URI SAN a textual node id's Raft RPC identity carries:
/// `urn:kythira:node:` and the id's canonical text, percent-encoded per
/// RFC 3986. Canonical text is not a valid DNS name (it holds ':' and '%'),
/// hence a URI rather than a DNS SAN. Unreserved characters, ':' and '@'
/// stay as they are; every other byte, '%' included, becomes an upper-case
/// %XX escape, so the mapping is injective and the URI decodes back to
/// exactly the canonical text.
[[nodiscard]] inline auto peer_identity_uri(std::string_view canonical_text) -> std::string {
    static constexpr char k_hex[] = "0123456789ABCDEF";
    std::string out(k_reserved_peer_uri_prefix);
    out.reserve(out.size() + canonical_text.size());
    for (const char ch : canonical_text) {
        const auto c = static_cast<unsigned char>(ch);
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' ||
                          c == ':' || c == '@';
        if (keep) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(k_hex[c >> 4]);
            out.push_back(k_hex[c & 0x0f]);
        }
    }
    return out;
}

/// Whether a node id type's peer identity is a URI SAN (textual ids) rather
/// than a DNS SAN (numeric ids). A cluster uses exactly one of the two.
template<typename NodeId>
inline constexpr bool peer_identity_is_uri = kythira::node_id_traits<NodeId>::is_textual;

/// The reserved SAN naming `node_id` as a Raft peer: `peer_identity_dns_name`
/// for a numeric id, `peer_identity_uri` of its canonical text otherwise.
template<typename NodeId>
[[nodiscard]] auto peer_identity_name(const NodeId& node_id) -> std::string {
    if constexpr (peer_identity_is_uri<NodeId>) {
        return peer_identity_uri(kythira::node_id_traits<NodeId>::to_text(node_id));
    } else {
        return peer_identity_dns_name(static_cast<std::uint64_t>(node_id));
    }
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

/// True iff `uri` falls in the reserved `urn:kythira:node:` namespace. URI
/// schemes and URN namespace ids are case-insensitive, and RFC 3986 makes a
/// percent-escaped unreserved character equivalent to the character itself,
/// so the prefix is compared after decoding escapes and folding case: a
/// "URN:Kythira:%6Eode:" spelling is reserved too.
[[nodiscard]] inline auto is_reserved_peer_uri(std::string_view uri) -> bool {
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::size_t pos = 0;
    for (const char expected : k_reserved_peer_uri_prefix) {
        if (pos >= uri.size()) {
            return false;
        }
        char c = uri[pos];
        if (c == '%' && pos + 2 < uri.size() && hex_value(uri[pos + 1]) >= 0 &&
            hex_value(uri[pos + 2]) >= 0) {
            c = static_cast<char>(hex_value(uri[pos + 1]) * 16 + hex_value(uri[pos + 2]));
            pos += 3;
        } else {
            ++pos;
        }
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != expected) {
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

/// PBKDF2 iteration count for derive_peer_enrollment_key. Matches the
/// at-rest CA key encryption (ca_state_machine's k_pbkdf2_iterations), so a
/// MAC observed on the wire is no cheaper to brute-force than the encrypted
/// CA key in the replicated log.
inline constexpr int k_peer_enrollment_kdf_iterations = 200000;

/// Derives the peer-enrollment HMAC key from the cluster unseal passphrase.
/// A slow KDF, not a single HMAC: every enrollment and trust-state MAC is
/// computed under this key, so anyone who captures one (nonce, MAC) pair can
/// test passphrase guesses offline, and a guessed passphrase yields both a
/// peer identity and the CA private key. Domain-separated by a fixed salt
/// so the key is never the passphrase itself, nor the at-rest encryption
/// key (which uses a random per-record salt).
[[nodiscard]] inline auto derive_peer_enrollment_key(const std::string& unseal_passphrase)
    -> std::string {
    if (unseal_passphrase.empty()) {
        throw std::invalid_argument("ca_http_helpers: empty unseal passphrase");
    }
    static constexpr std::string_view k_salt = "kythira ca_cluster_node peer-enrollment v2";
    unsigned char key[32];
    if (PKCS5_PBKDF2_HMAC(unseal_passphrase.data(), static_cast<int>(unseal_passphrase.size()),
                          reinterpret_cast<const unsigned char*>(k_salt.data()),
                          static_cast<int>(k_salt.size()), k_peer_enrollment_kdf_iterations,
                          EVP_sha256(), sizeof(key), key) != 1) {
        throw std::runtime_error("ca_http_helpers: PBKDF2 key derivation failed");
    }
    return hex_encode(key, sizeof(key));
}

/// HTTP header carrying a peer-enrollment MAC on POST /v1/certificates.
inline constexpr const char* k_peer_enrollment_header = "X-Kythira-Peer-Enrollment";

/// MAC binding a peer-enrollment request to the node id it enrolls and the
/// exact CSR being signed: replaying a captured request only ever yields a
/// certificate for the victim node's own key, which the replayer lacks.
/// Computed over the id's `node_id_traits` text: decimal for a numeric id,
/// exactly as before textual ids existed, so existing enrollments stay
/// valid; canonical text for a textual one (Requirement 12.3).
[[nodiscard]] inline auto peer_enrollment_mac(const std::string& key, std::uint64_t node_id,
                                              const std::string& csr_pem) -> std::string {
    return hmac_sha256_hex(key, "enroll\n" + std::to_string(node_id) + "\n" + csr_pem);
}

template<typename NodeId>
requires kythira::node_id_traits<NodeId>::is_textual
[[nodiscard]] auto peer_enrollment_mac(const std::string& key, const NodeId& node_id,
                                       const std::string& csr_pem) -> std::string {
    return hmac_sha256_hex(
        key, "enroll\n" + kythira::node_id_traits<NodeId>::to_text(node_id) + "\n" + csr_pem);
}

// ── Peer RPC trust state (GET /v1/peer/rpc-trust) ───────────────────────────
//
// What a node needs from the leader to run its own RPC TLS trust policy: the
// cluster root (trust anchor), whether every node has cut over to a
// CA-issued peer identity (so the shared bootstrap credential can stop being
// accepted), and which peer certificates have been revoked. Only peers may
// fetch it: the request carries a MAC over a fresh nonce, and the response
// body is MAC'd over (nonce, body). A client bearer token gets nothing here,
// so it cannot collect MACs to brute-force the passphrase offline.

/// Path of the peer-only trust-state route.
inline constexpr const char* k_peer_rpc_trust_path = "/v1/peer/rpc-trust";
/// Request header carrying a peer's fresh nonce, request header carrying the
/// MAC over that nonce, and response header carrying the MAC over the body.
inline constexpr const char* k_peer_nonce_header = "X-Kythira-Peer-Nonce";
inline constexpr const char* k_peer_request_mac_header = "X-Kythira-Peer-Request-Mac";
inline constexpr const char* k_peer_trust_mac_header = "X-Kythira-Peer-Trust-Mac";

/// MAC a peer attaches to GET /v1/peer/rpc-trust in place of the client
/// bearer token. A replay only fetches the (non-secret) trust state again,
/// so binding to the nonce alone suffices; the response is separately MAC'd.
[[nodiscard]] inline auto peer_trust_request_mac(const std::string& key, const std::string& nonce)
    -> std::string {
    return hmac_sha256_hex(key, "rpc-trust-request\n" + nonce);
}

/// MAC over a /v1/peer/rpc-trust response body, bound to the requester's
/// nonce so a stale or attacker-chosen state cannot be replayed into a later
/// fetch.
[[nodiscard]] inline auto peer_trust_mac(const std::string& key, const std::string& nonce,
                                         const std::string& body) -> std::string {
    return hmac_sha256_hex(key, "rpc-trust\n" + nonce + "\n" + body);
}

/// The replicated facts a node's RPC TLS trust policy is built from.
struct peer_rpc_trust_state {
    std::string root_pem;
    /// Every cluster member has enrolled a CA-issued peer identity.
    bool cutover_complete{false};
    /// Serials of revoked certificates that carry a reserved peer name.
    std::set<std::uint64_t> revoked_peer_serials;

    auto operator==(const peer_rpc_trust_state&) const -> bool = default;
};

[[nodiscard]] inline auto encode_peer_rpc_trust_state(const peer_rpc_trust_state& state)
    -> std::string {
    boost::json::array revoked;
    for (auto serial : state.revoked_peer_serials) {
        // As a string: JSON numbers above 2^53 do not survive every parser.
        revoked.emplace_back(std::to_string(serial));
    }
    return boost::json::serialize(boost::json::object{
        {"root_pem", state.root_pem},
        {"cutover_complete", state.cutover_complete},
        {"revoked_peer_serials", std::move(revoked)},
    });
}

/// Throws std::invalid_argument on a malformed body.
[[nodiscard]] inline auto decode_peer_rpc_trust_state(const std::string& body)
    -> peer_rpc_trust_state {
    try {
        const auto obj = boost::json::parse(body).as_object();
        peer_rpc_trust_state state;
        state.root_pem = std::string(obj.at("root_pem").as_string());
        state.cutover_complete = obj.at("cutover_complete").as_bool();
        for (const auto& v : obj.at("revoked_peer_serials").as_array()) {
            state.revoked_peer_serials.insert(std::stoull(std::string(v.as_string())));
        }
        if (state.root_pem.empty()) {
            throw std::invalid_argument("empty root_pem");
        }
        return state;
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const std::exception& ex) {
        throw std::invalid_argument(std::string("malformed peer RPC trust state: ") + ex.what());
    }
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
template<typename NodeId = std::uint64_t> struct basic_peer_enrollment_decision {
    bool allowed{true};
    std::string error;              ///< Set when !allowed.
    std::optional<NodeId> node_id;  ///< Set for an authenticated peer enrollment.
};
using peer_enrollment_decision = basic_peer_enrollment_decision<>;

/// Decides whether a /v1/certificates request may proceed, given its parsed
/// options, the optional `rpc_tls_ready_node_id` body field, the enrollment
/// MAC header (empty if absent), the cluster's node ids and the enrollment
/// key. Ordinary client requests (no reserved name, no node id) pass
/// unchanged. Any request touching peer identity — a reserved
/// "ca-cluster-node-*" DNS SAN or "urn:kythira:node:*" URI SAN, or an
/// `rpc_tls_ready_node_id` (which commits replicated cutover state) — must
/// be a well-formed enrollment for exactly one configured node and carry a
/// valid MAC. A well-formed enrollment requests exactly the node's
/// `peer_identity_name` and nothing else: the DNS form for a numeric
/// `NodeId`, the URI form for a textual one, never both.
template<typename NodeId>
[[nodiscard]] auto classify_peer_enrollment(
    const csr_signing_options& options, std::optional<std::type_identity_t<NodeId>> ready_node_id,
    const std::string& mac_header, const std::set<NodeId>& cluster_node_ids,
    const std::string& enrollment_key, const std::string& csr_pem)
    -> basic_peer_enrollment_decision<NodeId> {
    bool wants_reserved_name = false;
    for (const auto& d : options.dns_names) {
        if (is_reserved_peer_dns_name(d)) {
            wants_reserved_name = true;
        }
    }
    for (const auto& u : options.uri_names) {
        if (is_reserved_peer_uri(u)) {
            wants_reserved_name = true;
        }
    }
    if (!wants_reserved_name && !ready_node_id.has_value()) {
        return {};
    }
    auto deny = [](std::string why) {
        return basic_peer_enrollment_decision<NodeId>{false, std::move(why), std::nullopt};
    };
    if (!ready_node_id.has_value()) {
        return deny(
            "names with the \"ca-cluster-node-\" or \"urn:kythira:node:\" prefix are "
            "reserved for Raft peers");
    }
    if (!cluster_node_ids.contains(*ready_node_id)) {
        return deny("rpc_tls_ready_node_id is not a member of this cluster");
    }
    const auto& own = peer_identity_is_uri<NodeId> ? options.uri_names : options.dns_names;
    const auto& other = peer_identity_is_uri<NodeId> ? options.dns_names : options.uri_names;
    if (own.size() != 1 || own.front() != peer_identity_name(*ready_node_id) || !other.empty() ||
        !options.ip_addresses.empty()) {
        return deny("a peer enrollment must request exactly the node's own peer name");
    }
    if (mac_header.empty() ||
        !constant_time_equals(mac_header,
                              peer_enrollment_mac(enrollment_key, *ready_node_id, csr_pem))) {
        return deny("peer enrollment is not authenticated");
    }
    return basic_peer_enrollment_decision<NodeId>{true, {}, ready_node_id};
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

// Extracts dns_names/ip_addresses/uri_names/server_auth/client_auth from a presented
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
            } else if (name->type == GEN_URI) {
                auto* s = name->d.uniformResourceIdentifier;
                opts.uri_names.emplace_back(reinterpret_cast<const char*>(ASN1_STRING_get0_data(s)),
                                            static_cast<std::size_t>(ASN1_STRING_length(s)));
            } else if (name->type == GEN_IPADD) {
                auto* s = name->d.iPAddress;
                const unsigned char* bytes = ASN1_STRING_get0_data(s);
                int len = ASN1_STRING_length(s);
                char buf[INET6_ADDRSTRLEN] = {};
                const int family = len == 4 ? AF_INET : len == 16 ? AF_INET6 : AF_UNSPEC;
                if (family != AF_UNSPEC && inet_ntop(family, bytes, buf, sizeof(buf)) != nullptr) {
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

// True iff `cert` carries at least one SAN of GENERAL_NAME type `type`
// (GEN_DNS or GEN_URI, both IA5Strings) whose value is in `names`.
[[nodiscard]] inline auto cert_has_ia5_san_in(X509* cert, int type,
                                              const std::set<std::string>& names) -> bool {
    auto* sans =
        static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (sans == nullptr) {
        return false;
    }
    bool hit = false;
    for (int i = 0; i < sk_GENERAL_NAME_num(sans) && !hit; ++i) {
        GENERAL_NAME* gn = sk_GENERAL_NAME_value(sans, i);
        if (gn->type != type) {
            continue;
        }
        const ASN1_STRING* str = type == GEN_DNS ? gn->d.dNSName : gn->d.uniformResourceIdentifier;
        std::string value(reinterpret_cast<const char*>(ASN1_STRING_get0_data(str)),
                          static_cast<std::size_t>(ASN1_STRING_length(str)));
        hit = names.contains(value);
    }
    GENERAL_NAMES_free(sans);
    return hit;
}

// True iff `cert` carries at least one DNS SAN contained in `names`.
[[nodiscard]] inline auto cert_has_dns_san_in(X509* cert, const std::set<std::string>& names)
    -> bool {
    return cert_has_ia5_san_in(cert, GEN_DNS, names);
}

// True iff `cert` carries at least one URI SAN contained in `names`.
[[nodiscard]] inline auto cert_has_uri_san_in(X509* cert, const std::set<std::string>& names)
    -> bool {
    return cert_has_ia5_san_in(cert, GEN_URI, names);
}

// True iff `cert` carries one of `names` as the kind of SAN a `NodeId`
// peer identity uses (`peer_identity_is_uri`): a URI SAN for textual ids, a
// DNS SAN for numeric ones. Only that kind counts, so a peer is accepted
// through exactly one of the two forms (Requirement 12.2).
template<typename NodeId>
[[nodiscard]] auto cert_has_peer_name_in(X509* cert, const std::set<std::string>& names) -> bool {
    return peer_identity_is_uri<NodeId> ? cert_has_uri_san_in(cert, names)
                                        : cert_has_dns_san_in(cert, names);
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
