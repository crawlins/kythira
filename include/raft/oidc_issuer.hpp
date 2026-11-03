// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// OIDC issuer signing-key model for ca_cluster_node
// (`.kiro/specs/iam-compatible-auth/`, Requirement 1, design Component 1).
//
// Everything here is pure: no clock, no randomness, no I/O. The replicated
// key set lives in ca_state_machine (record_oidc_key / retire_oidc_key); the
// leader's maintenance thread feeds it, with its own wall clock, to
// plan_oidc_rotation() and proposes whatever single step comes back. Tests
// drive the same planner with an injected clock.
//
// Key life cycle, in leader-stamped Unix seconds:
//
//   publish_after   in the JWKS from here on (relying parties can cache it);
//   sign_after      eligible to sign; the newest eligible key signs;
//   retire_after    once set, the key no longer signs; it leaves the JWKS
//                   at this time, and the replicated state drops it when the
//                   next OIDC command stamped at or after it is applied.
//
// A successor is recorded `publish_lead` (24 h) before it starts signing, and
// the key it supersedes is retired `max_token_lifetime` after the successor
// took over, so every token the old key ever signed can still be verified
// until it expires (Requirement 1.5).

#include <raft/jwt.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kythira::oidc {

struct oidc_key_record {
    std::string kid;  // RFC 7638 thumbprint of public_jwk
    jose::jws_alg alg{jose::jws_alg::rs256};
    boost::json::object public_jwk;  // public members only (kty, n/e or crv/x/y)
    // Output of raft::testing::encrypt_ca_private_key() over the key's PEM:
    // sealed under the unseal passphrase, exactly like the CA key.
    std::string sealed_private_key;
    std::int64_t publish_after{0};
    std::int64_t sign_after{0};
    std::optional<std::int64_t> retire_after;

    friend auto operator==(const oidc_key_record&, const oidc_key_record&) -> bool = default;
};

[[nodiscard]] inline auto key_record_to_json(const oidc_key_record& k) -> boost::json::object {
    boost::json::object obj;
    obj["kid"] = k.kid;
    obj["alg"] = std::string(jose::alg_name(k.alg));
    obj["public_jwk"] = k.public_jwk;
    obj["sealed_private_key"] = k.sealed_private_key;
    obj["publish_after"] = k.publish_after;
    obj["sign_after"] = k.sign_after;
    if (k.retire_after) {
        obj["retire_after"] = *k.retire_after;
    }
    return obj;
}

/// Throws std::invalid_argument for a structurally bad record.
[[nodiscard]] inline auto key_record_from_json(const boost::json::object& obj) -> oidc_key_record {
    oidc_key_record k;
    k.kid = std::string(obj.at("kid").as_string());
    auto alg = jose::parse_alg(obj.at("alg").as_string());
    if (!alg) {
        throw std::invalid_argument("oidc: unsupported key alg");
    }
    k.alg = *alg;
    k.public_jwk = obj.at("public_jwk").as_object();
    k.sealed_private_key = std::string(obj.at("sealed_private_key").as_string());
    k.publish_after = obj.at("publish_after").to_number<std::int64_t>();
    k.sign_after = obj.at("sign_after").to_number<std::int64_t>();
    if (const auto* r = obj.if_contains("retire_after")) {
        k.retire_after = r->to_number<std::int64_t>();
    }
    if (k.kid.empty() || k.sealed_private_key.empty()) {
        throw std::invalid_argument("oidc: key record with an empty kid or sealed key");
    }
    if (k.sign_after < k.publish_after) {
        throw std::invalid_argument("oidc: key record signs before it is published");
    }
    return k;
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

/// The key that signs at `now`: among keys that are not retiring and whose
/// sign_after has passed, the one with the latest sign_after (ties broken by
/// kid so every member picks the same one). nullptr if none.
[[nodiscard]] inline auto signing_key(const std::vector<oidc_key_record>& keys, std::int64_t now)
    -> const oidc_key_record* {
    const oidc_key_record* best = nullptr;
    for (const auto& k : keys) {
        if (k.retire_after || k.sign_after > now) {
            continue;
        }
        if (best == nullptr || k.sign_after > best->sign_after ||
            (k.sign_after == best->sign_after && k.kid > best->kid)) {
            best = &k;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Rotation planner
// ---------------------------------------------------------------------------

struct rotation_config {
    std::chrono::seconds rotation_period{std::chrono::hours(24 * 90)};
    std::chrono::seconds publish_lead{std::chrono::hours(24)};
    std::chrono::seconds max_token_lifetime{std::chrono::hours(12)};
};

/// Propose a new key: generate it, seal it, and submit record_oidc_key with
/// these times.
struct create_key {
    std::int64_t publish_after;
    std::int64_t sign_after;
};

/// Propose retire_oidc_key for `kid`.
struct retire_key {
    std::string kid;
    std::int64_t retire_after;
};

using rotation_step = std::variant<std::monostate, create_key, retire_key>;

/// The single next step for the key set at leader time `now`. Called again
/// after each step commits, it converges: retire superseded keys first, then
/// create a successor when the current signer is due.
///
///  - No usable key at all (fresh cluster, or an old snapshot restored with
///    no keys): create one that is published and signs immediately. Nobody
///    can hold a token yet, so there is nothing to give notice to.
///  - A non-retiring key older than the current signer is superseded:
///    retire it `max_token_lifetime` from now. The leader switched to the new
///    signer no earlier than its sign_after, which is <= now, so this is at
///    least max_token_lifetime after the old key's last signature.
///  - No successor pending and the signer is within publish_lead of its
///    rotation date: create the successor, published now and signing
///    publish_lead from now.
[[nodiscard]] inline auto plan_oidc_rotation(const std::vector<oidc_key_record>& keys,
                                             std::int64_t now, const rotation_config& cfg)
    -> rotation_step {
    const auto* signer = signing_key(keys, now);
    bool pending = false;
    for (const auto& k : keys) {
        if (!k.retire_after && k.sign_after > now) {
            pending = true;
        }
    }
    if (signer == nullptr) {
        if (pending) {
            return std::monostate{};  // a successor is on its way; wait for it
        }
        return create_key{now, now};
    }
    for (const auto& k : keys) {
        if (&k != signer && !k.retire_after && k.sign_after <= signer->sign_after) {
            return retire_key{k.kid, now + cfg.max_token_lifetime.count()};
        }
    }
    const auto due = signer->sign_after + cfg.rotation_period.count() - cfg.publish_lead.count();
    if (!pending && now >= due) {
        return create_key{now, now + cfg.publish_lead.count()};
    }
    return std::monostate{};
}

// ---------------------------------------------------------------------------
// Published documents
// ---------------------------------------------------------------------------

/// Checks an --oidc-issuer-url (Requirement 1.2): https, a host, no query,
/// no fragment, no trailing slash (so "<issuer>/.well-known/..." is exact and
/// `iss` matches what relying parties configure). Returns an error message,
/// or std::nullopt if it is acceptable.
[[nodiscard]] inline auto issuer_url_error(std::string_view url) -> std::optional<std::string> {
    constexpr std::string_view scheme = "https://";
    if (!url.starts_with(scheme)) {
        return "the issuer URL must start with https://";
    }
    const auto rest = url.substr(scheme.size());
    if (rest.empty() || rest.front() == '/') {
        return "the issuer URL has no host";
    }
    if (url.find('?') != std::string_view::npos || url.find('#') != std::string_view::npos) {
        return "the issuer URL must not have a query string or fragment";
    }
    if (url.back() == '/') {
        return "the issuer URL must not end with '/'";
    }
    for (char c : url) {
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7f) {
            return "the issuer URL must be printable ASCII without spaces";
        }
    }
    return std::nullopt;
}

inline constexpr std::string_view k_discovery_path = "/.well-known/openid-configuration";
inline constexpr std::string_view k_jwks_path = "/.well-known/jwks.json";

/// The claims a Kythira token can carry (Requirement 1.8).
inline constexpr std::array<std::string_view, 10> k_claims_supported{"iss",
                                                                     "sub",
                                                                     "aud",
                                                                     "iat",
                                                                     "nbf",
                                                                     "exp",
                                                                     "jti",
                                                                     "kythira:node_id",
                                                                     "kythira:workload",
                                                                     "kythira:cert_serial"};

/// The JWKS at `now`: every key whose publish_after has passed and whose
/// retire_after (if any) has not, each with kid, alg and use=sig. Sorted by
/// kid so the document is byte-identical on every member.
///
/// The retire_after filter matters: the replicated state only drops a
/// retired key when the next OIDC command applies, which in steady state is
/// the next rotation, months later. The published set must not wait for it.
[[nodiscard]] inline auto jwks_document(std::vector<oidc_key_record> keys, std::int64_t now)
    -> boost::json::object {
    std::ranges::sort(keys, {}, &oidc_key_record::kid);
    boost::json::array out;
    for (const auto& k : keys) {
        if (k.publish_after > now || (k.retire_after && *k.retire_after <= now)) {
            continue;
        }
        auto jwk = k.public_jwk;
        jwk["kid"] = k.kid;
        jwk["alg"] = std::string(jose::alg_name(k.alg));
        jwk["use"] = "sig";
        out.emplace_back(std::move(jwk));
    }
    return boost::json::object{{"keys", std::move(out)}};
}

/// The discovery document (Requirement 1.1). `jwks_uri` is passed in so the
/// bucket-publishing mode (task 2) can point it somewhere other than the
/// issuer itself.
[[nodiscard]] inline auto discovery_document(std::string_view issuer, std::string_view jwks_uri,
                                             jose::jws_alg alg) -> boost::json::object {
    boost::json::array claims;
    for (auto c : k_claims_supported) {
        claims.emplace_back(c);
    }
    return boost::json::object{
        {"issuer", issuer},
        {"jwks_uri", jwks_uri},
        {"response_types_supported", boost::json::array{"id_token"}},
        {"subject_types_supported", boost::json::array{"public"}},
        {"id_token_signing_alg_values_supported", boost::json::array{jose::alg_name(alg)}},
        {"claims_supported", std::move(claims)},
    };
}

}  // namespace kythira::oidc
