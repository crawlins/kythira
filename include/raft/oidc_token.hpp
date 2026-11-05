// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// OIDC token endpoint decisions for ca_cluster_node
// (`.kiro/specs/iam-compatible-auth/`, Requirements 1.6-1.8, task 2).
//
// `POST /v1/oidc/token` turns a Kythira-CA client certificate into a signed
// identity token that cloud STS services accept. ca_cluster_node does the
// I/O: it reads the TLS peer certificate, checks the chain against the CA
// root and the revocation ledger, picks the signing key and draws a random
// `jti`. Everything that decides *whether* a token is issued, and with which
// claims, is here and is pure (the caller passes `now`), so unit tests drive
// every refusal without a cluster.
//
// Status codes:
//   401  no client certificate, or one that does not chain to the CA, is
//        revoked, or is outside its validity period;
//   403  a valid certificate that names no eligible subject (criterion 1.6);
//   400  a malformed body, an audience off the allowlist (1.7) or a lifetime
//        the policy or the certificate does not allow (1.8).

#include <raft/jwt.hpp>
#include <raft/oidc_issuer.hpp>

#include <boost/json.hpp>

#include <openssl/asn1.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kythira::oidc {

inline constexpr std::string_view k_token_path = "/v1/oidc/token";
inline constexpr std::string_view k_token_type_jwt = "urn:ietf:params:oauth:token-type:jwt";

/// The shortest token anyone may ask for. Below this a token is likely to
/// expire in flight to the cloud STS.
inline constexpr std::int64_t k_min_token_lifetime_secs = 60;

struct token_policy {
    /// `--oidc-audience`, repeatable. Empty refuses every request.
    std::set<std::string> audiences;
    /// `--oidc-workload-uri-prefix`. Empty: workload certificates get no
    /// tokens, only node certificates do.
    std::string workload_uri_prefix;
    std::chrono::seconds default_lifetime{std::chrono::hours(1)};
    /// `--oidc-max-token-lifetime-secs`; never above 12 hours (1.8). The
    /// rotation planner retires a superseded key this long after the switch,
    /// so no token can outlive the key that verifies it.
    std::chrono::seconds max_lifetime{std::chrono::hours(12)};
};

/// Checks a --oidc-workload-uri-prefix. It must end in '/' or ':' so that a
/// prefix of "spiffe://kythira/wl/" cannot also admit "spiffe://kythira/wlx",
/// and it must not reach into the reserved peer namespace.
[[nodiscard]] inline auto workload_prefix_error(std::string_view prefix)
    -> std::optional<std::string> {
    if (prefix.empty()) {
        return "the workload URI prefix is empty";
    }
    const auto scheme_end = prefix.find(':');
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return "the workload URI prefix must start with a URI scheme";
    }
    if (prefix.back() != '/' && prefix.back() != ':') {
        return "the workload URI prefix must end with '/' or ':'";
    }
    if (prefix.starts_with("urn:kythira:node:") ||
        std::string_view("urn:kythira:node:").starts_with(prefix)) {
        return "the workload URI prefix overlaps the reserved urn:kythira:node: namespace";
    }
    for (char c : prefix) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7f || c == '?' || c == '#') {
            return "the workload URI prefix must be printable ASCII without spaces, '?' or '#'";
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Request body
// ---------------------------------------------------------------------------

struct token_request {
    std::string audience;
    std::optional<std::int64_t> lifetime_secs;
};

struct token_error {
    int status;
    std::string code;     // the JSON "error" member
    std::string message;  // the JSON "message" member
};

namespace token_detail {

[[nodiscard]] inline auto hex_value(char c) -> int {
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
}

/// application/x-www-form-urlencoded value decoding: '+' is a space and
/// %XX is a byte. std::nullopt for a truncated or non-hex escape.
[[nodiscard]] inline auto form_decode(std::string_view in) -> std::optional<std::string> {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%') {
            if (i + 2 >= in.size()) {
                return std::nullopt;
            }
            const int hi = hex_value(in[i + 1]);
            const int lo = hex_value(in[i + 2]);
            if (hi < 0 || lo < 0) {
                return std::nullopt;
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

[[nodiscard]] inline auto bad_request(std::string message) -> token_error {
    return {400, "invalid_request", std::move(message)};
}

/// Seconds since the epoch for an ASN1_TIME, or std::nullopt.
[[nodiscard]] inline auto unix_seconds(const ASN1_TIME* t) -> std::optional<std::int64_t> {
    if (t == nullptr) {
        return std::nullopt;
    }
    std::tm tm{};
    if (ASN1_TIME_to_tm(t, &tm) != 1) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(timegm(&tm));
}

}  // namespace token_detail

/// Parses `audience=<aud>[&lifetime=<seconds>]`. Each may appear once;
/// other parameters are ignored, as OAuth token endpoints do.
[[nodiscard]] inline auto parse_token_request(std::string_view body)
    -> std::variant<token_request, token_error> {
    token_request req;
    bool have_audience = false;
    bool have_lifetime = false;
    while (!body.empty()) {
        const auto amp = body.find('&');
        const auto pair = body.substr(0, amp);
        body = amp == std::string_view::npos ? std::string_view{} : body.substr(amp + 1);
        if (pair.empty()) {
            continue;
        }
        const auto eq = pair.find('=');
        const auto name = token_detail::form_decode(pair.substr(0, eq));
        const auto value = token_detail::form_decode(
            eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1));
        if (!name || !value) {
            return token_detail::bad_request("the form body has a malformed %-escape");
        }
        if (*name == "audience") {
            if (have_audience) {
                return token_detail::bad_request("audience may be given only once");
            }
            have_audience = true;
            req.audience = *value;
        } else if (*name == "lifetime") {
            if (have_lifetime) {
                return token_detail::bad_request("lifetime may be given only once");
            }
            have_lifetime = true;
            if (value->empty() || value->size() > 9 ||
                value->find_first_not_of("0123456789") != std::string::npos) {
                return token_detail::bad_request("lifetime must be a number of seconds");
            }
            req.lifetime_secs = std::stoll(*value);
        }
    }
    if (req.audience.empty()) {
        return token_detail::bad_request("audience is required");
    }
    return req;
}

// ---------------------------------------------------------------------------
// Client certificate
// ---------------------------------------------------------------------------

/// What the decision needs from the presented certificate.
struct cert_facts {
    std::vector<std::string> dns_sans;
    std::vector<std::string> uri_sans;
    std::string serial_hex;  // lower-case, minimal (certificate_serial::to_hex)
    std::int64_t not_before{0};
    std::int64_t not_after{0};
};

/// Reads SANs, validity and serial from `cert`. std::nullopt if the
/// validity cannot be read.
[[nodiscard]] inline auto cert_facts_of(X509* cert, std::string serial_hex)
    -> std::optional<cert_facts> {
    if (cert == nullptr) {
        return std::nullopt;
    }
    cert_facts f;
    f.serial_hex = std::move(serial_hex);
    auto nb = token_detail::unix_seconds(X509_get0_notBefore(cert));
    auto na = token_detail::unix_seconds(X509_get0_notAfter(cert));
    if (!nb || !na) {
        return std::nullopt;
    }
    f.not_before = *nb;
    f.not_after = *na;
    auto* sans =
        static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (sans != nullptr) {
        for (int i = 0; i < sk_GENERAL_NAME_num(sans); ++i) {
            const GENERAL_NAME* gn = sk_GENERAL_NAME_value(sans, i);
            const ASN1_STRING* str = nullptr;
            std::vector<std::string>* into = nullptr;
            if (gn->type == GEN_DNS) {
                str = gn->d.dNSName;
                into = &f.dns_sans;
            } else if (gn->type == GEN_URI) {
                str = gn->d.uniformResourceIdentifier;
                into = &f.uri_sans;
            }
            if (into != nullptr) {
                into->emplace_back(reinterpret_cast<const char*>(ASN1_STRING_get0_data(str)),
                                   static_cast<std::size_t>(ASN1_STRING_length(str)));
            }
        }
        GENERAL_NAMES_free(sans);
    }
    return f;
}

/// The checks only the caller can make: the chain against the CA root, and
/// the revocation ledger.
struct token_evidence {
    std::optional<cert_facts> cert;  // nullopt: no client certificate
    bool chains_to_ca{false};
    bool revoked{false};
};

// ---------------------------------------------------------------------------
// Subject
// ---------------------------------------------------------------------------

struct token_subject {
    std::string sub;                      // "node:<id>" or "workload:<uri>"
    std::optional<std::string> node_id;   // kythira:node_id
    std::optional<std::string> workload;  // kythira:workload
};

/// Who the token names (criterion 1.6).
///
/// `peer_names` maps each cluster member's reserved peer SAN (a
/// `ca-cluster-node-<n>` DNS name or a `urn:kythira:node:` URI, as
/// peer_identity_name() spells it) to that member's canonical node id text.
/// Only the CA's authenticated peer enrollment can put one of those names in
/// a certificate, so carrying one proves the holder is that node.
///
/// Otherwise exactly one URI SAN under `workload_prefix` names a workload.
/// The path after the prefix must be non-empty and free of '?', '#' and
/// dot segments, so the subject a cloud trust policy matches is the one the
/// certificate was issued for. Anything else is std::nullopt (403): no
/// eligible name, two different members' names, or two workload URIs.
[[nodiscard]] inline auto subject_for(const cert_facts& cert,
                                      const std::map<std::string, std::string>& peer_names,
                                      std::string_view workload_prefix)
    -> std::optional<token_subject> {
    std::optional<std::string> node;
    auto consider_peer = [&](const std::string& name) -> bool {
        auto it = peer_names.find(name);
        if (it == peer_names.end()) {
            return true;
        }
        if (node && *node != it->second) {
            return false;
        }
        node = it->second;
        return true;
    };
    for (const auto& d : cert.dns_sans) {
        if (!consider_peer(d)) {
            return std::nullopt;
        }
    }
    for (const auto& u : cert.uri_sans) {
        if (!consider_peer(u)) {
            return std::nullopt;
        }
    }
    if (node) {
        return token_subject{"node:" + *node, node, std::nullopt};
    }
    if (workload_prefix.empty()) {
        return std::nullopt;
    }
    std::optional<std::string> workload;
    for (const auto& u : cert.uri_sans) {
        if (!u.starts_with(workload_prefix) || u.size() == workload_prefix.size()) {
            continue;
        }
        const std::string_view rest = std::string_view(u).substr(workload_prefix.size());
        bool clean = true;
        for (char c : rest) {
            const auto b = static_cast<unsigned char>(c);
            if (b <= 0x20 || b >= 0x7f || c == '?' || c == '#') {
                clean = false;
            }
        }
        const std::string padded = "/" + std::string(rest) + "/";
        if (padded.find("/./") != std::string::npos || padded.find("/../") != std::string::npos) {
            clean = false;
        }
        if (!clean) {
            return std::nullopt;
        }
        if (workload && *workload != u) {
            return std::nullopt;
        }
        workload = u;
    }
    if (workload) {
        return token_subject{"workload:" + *workload, std::nullopt, workload};
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Decision
// ---------------------------------------------------------------------------

struct token_grant {
    boost::json::object claims;  // everything but jti, which the caller draws
    std::int64_t expires_at{0};
};

/// The whole decision for one request at `now`. On success the claims hold
/// iss, sub, aud, iat, nbf, exp, kythira:cert_serial and one of
/// kythira:node_id or kythira:workload (criterion 1.8); the caller adds
/// `jti` and signs.
///
/// Lifetime: an explicit `lifetime` must lie within
/// [k_min_token_lifetime_secs, max_lifetime] and end before the certificate
/// does, or the request is refused. With none, the default (capped at
/// max_lifetime) is shortened to the certificate's remaining validity.
[[nodiscard]] inline auto decide_token(const token_evidence& evidence, std::string_view body,
                                       std::string_view issuer, const token_policy& policy,
                                       const std::map<std::string, std::string>& peer_names,
                                       std::int64_t now) -> std::variant<token_grant, token_error> {
    if (!evidence.cert) {
        return token_error{401, "unauthorized", "a TLS client certificate is required"};
    }
    if (!evidence.chains_to_ca) {
        return token_error{401, "unauthorized",
                           "the client certificate does not chain to this CA's root"};
    }
    if (evidence.revoked) {
        return token_error{401, "unauthorized", "the client certificate has been revoked"};
    }
    const auto& cert = *evidence.cert;
    if (now < cert.not_before || now >= cert.not_after) {
        return token_error{401, "unauthorized",
                           "the client certificate is outside its validity period"};
    }
    auto subject = subject_for(cert, peer_names, policy.workload_uri_prefix);
    if (!subject) {
        return token_error{403, "forbidden",
                           "the client certificate names neither a cluster node nor a workload "
                           "under the configured prefix"};
    }
    auto parsed = parse_token_request(body);
    if (const auto* err = std::get_if<token_error>(&parsed)) {
        return *err;
    }
    const auto& req = std::get<token_request>(parsed);
    if (!policy.audiences.contains(req.audience)) {
        return token_error{400, "invalid_target", "the audience is not on this issuer's allowlist"};
    }
    const std::int64_t max_lifetime = policy.max_lifetime.count();
    const std::int64_t remaining = cert.not_after - now;
    std::int64_t lifetime = 0;
    if (req.lifetime_secs) {
        lifetime = *req.lifetime_secs;
        if (lifetime < k_min_token_lifetime_secs || lifetime > max_lifetime) {
            return token_detail::bad_request("lifetime must be between " +
                                             std::to_string(k_min_token_lifetime_secs) + " and " +
                                             std::to_string(max_lifetime) + " seconds");
        }
        if (lifetime > remaining) {
            return token_detail::bad_request(
                "lifetime would outlast the client certificate, which expires in " +
                std::to_string(remaining) + " seconds");
        }
    } else {
        lifetime = std::min({policy.default_lifetime.count(), max_lifetime, remaining});
    }

    token_grant grant;
    grant.expires_at = now + lifetime;
    auto& c = grant.claims;
    c["iss"] = issuer;
    c["sub"] = subject->sub;
    c["aud"] = req.audience;
    c["iat"] = now;
    c["nbf"] = now;
    c["exp"] = grant.expires_at;
    if (subject->node_id) {
        c["kythira:node_id"] = *subject->node_id;
    }
    if (subject->workload) {
        c["kythira:workload"] = *subject->workload;
    }
    c["kythira:cert_serial"] = cert.serial_hex;
    return grant;
}

/// RFC 3339 UTC, e.g. "2026-11-05T01:02:03Z".
[[nodiscard]] inline auto rfc3339_utc(std::int64_t unix_seconds) -> std::string {
    const auto t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    const auto n = std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf, n);
}

/// Signs `grant` with `key` (the record that signing_key() chose, and its
/// unsealed private key) and returns the response body:
/// {"token", "token_type", "expires_at", "expires_in"}.
[[nodiscard]] inline auto mint_token(token_grant grant, std::string_view jti,
                                     const oidc_key_record& key, EVP_PKEY* private_key,
                                     std::int64_t now) -> boost::json::object {
    grant.claims["jti"] = jti;
    boost::json::object header{{"typ", "JWT"}, {"kid", key.kid}};
    auto token = jose::sign_compact(std::move(header), boost::json::serialize(grant.claims),
                                    private_key, key.alg);
    return boost::json::object{
        {"token", std::move(token)},
        {"token_type", k_token_type_jwt},
        {"expires_at", rfc3339_utc(grant.expires_at)},
        {"expires_in", grant.expires_at - now},
    };
}

}  // namespace kythira::oidc
