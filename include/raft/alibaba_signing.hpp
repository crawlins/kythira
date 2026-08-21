// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file alibaba_signing.hpp
/// @brief Alibaba Cloud OpenAPI V3 request signing (ACS3-HMAC-SHA256) — the
///        scheme every RPC-style control-plane call (ESS, ECS, STS) must carry.
///
/// Hand-rolled rather than taken from an SDK, which is the whole reason this
/// feature adds nothing to `vcpkg.json`: the scheme is a canonical string, an
/// HMAC-SHA256 over it, and one header — strictly simpler than OCI's
/// RSA-based Signature v1, with no key parsing at all. OpenSSL is already a
/// hard dependency, so there is nothing left to depend on.
///
/// **The canonical form is confirmed against the vendor's own worked
/// example, recomputed independently** (spike-notes.md Finding 1): the
/// vendor's V3 documentation publishes a complete example with a fixed
/// AccessKeySecret, and both the canonical-request hash and the final
/// signature reproduce exactly. The unit test pins that vector. It matters
/// for the same reason OCI's spike-confirmed header order did: a signature
/// over a plausible-but-wrong canonical string yields a well-formed request
/// the service rejects with an error naming neither side of the mismatch.
///
/// Canonical request (each part joined with `"\n"`):
///
///     HTTPRequestMethod
///     CanonicalURI                      ("/" for RPC-style calls)
///     CanonicalQueryString              (sorted, percent-encoded pairs)
///     CanonicalHeaders                  ("{lowercase-name}:{value}\n" each,
///                                        sorted; note the trailing \n)
///     SignedHeaders                     (lowercase names joined with ";")
///     HashedRequestPayload              (lowercase-hex SHA-256 of the body)
///
/// string-to-sign = "ACS3-HMAC-SHA256\n" + lowercase-hex SHA-256 of the
/// canonical request; signature = lowercase-hex
/// HMAC-SHA256(AccessKeySecret, string-to-sign).
///
/// The signed set is fixed: `host`, `x-acs-action`, `x-acs-content-sha256`,
/// `x-acs-date`, `x-acs-signature-nonce`, `x-acs-version`, plus
/// `x-acs-security-token` when STS credentials are in play. Percent-encoding
/// is RFC 3986 unreserved-only (`A-Z a-z 0-9 - _ . ~` literal, everything
/// else `%XX` uppercase hex, space as `%20` never `+`).

#include <raft/alibaba_client_config.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace kythira::alibaba_signing {

namespace detail {

inline constexpr const char* k_hex_digits = "0123456789abcdef";

[[nodiscard]] inline auto to_lower(std::string_view in) -> std::string {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// Lowercase-hex SHA-256, the form both the payload hash and the
/// canonical-request hash use.
[[nodiscard]] inline auto sha256_hex(std::string_view data) -> std::string {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest.data());
    std::string out;
    out.reserve(2 * digest.size());
    for (const unsigned char b : digest) {
        out.push_back(k_hex_digits[b >> 4U]);
        out.push_back(k_hex_digits[b & 0x0FU]);
    }
    return out;
}

/// Raw HMAC-SHA256 (for the OSS V4 key-derivation chain, which feeds one
/// HMAC's output into the next as key material).
[[nodiscard]] inline auto hmac_sha256(std::string_view key, std::string_view msg) -> std::string {
    std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
    unsigned int mac_len = 0;
    if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), mac.data(),
             &mac_len) == nullptr) {
        throw std::runtime_error("alibaba_signing: HMAC-SHA256 failed");
    }
    return {reinterpret_cast<const char*>(mac.data()), mac_len};
}

[[nodiscard]] inline auto to_hex(std::string_view raw) -> std::string {
    std::string out;
    out.reserve(2 * raw.size());
    for (const char c : raw) {
        const auto b = static_cast<unsigned char>(c);
        out.push_back(k_hex_digits[b >> 4U]);
        out.push_back(k_hex_digits[b & 0x0FU]);
    }
    return out;
}

[[nodiscard]] inline auto hmac_sha256_hex(std::string_view key, std::string_view msg)
    -> std::string {
    return to_hex(hmac_sha256(key, msg));
}

/// RFC 3986 percent-encoding, unreserved characters literal, uppercase %XX
/// for everything else. The vendor's rule set ("replace + with %20, * with
/// %2A, %7E with ~") describes fixing up a Java URLEncoder; encoding
/// correctly in the first place is equivalent and simpler.
[[nodiscard]] inline auto percent_encode(std::string_view in) -> std::string {
    static constexpr const char* k_upper_hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        const auto b = static_cast<unsigned char>(c);
        const bool unreserved =
            (std::isalnum(b) != 0) || b == '-' || b == '_' || b == '.' || b == '~';
        if (unreserved) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(k_upper_hex[b >> 4U]);
            out.push_back(k_upper_hex[b & 0x0FU]);
        }
    }
    return out;
}

/// The `x-acs-date` timestamp: ISO 8601 basic UTC, second precision,
/// `YYYY-MM-DDTHH:MM:SSZ`. Formatted from the broken-down time explicitly —
/// the same locale-independence reasoning as `oci_signing::rfc1123_now`.
[[nodiscard]] inline auto iso8601_utc(std::chrono::system_clock::time_point when) -> std::string {
    const std::time_t t = std::chrono::system_clock::to_time_t(when);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    std::array<char, 32> buf{};
    const int written = std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                                      tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                                      tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    if (written <= 0) {
        throw std::runtime_error("alibaba_signing: failed to format x-acs-date");
    }
    return std::string(buf.data(), static_cast<std::size_t>(written));
}

}  // namespace detail

/// Builds the sorted, percent-encoded canonical query string from raw
/// (unencoded) parameter names and values. `std::map` ordering is byte-wise
/// on the raw names, matching the vendor's sort-then-encode rule for the
/// parameter sets this provider sends (ASCII names throughout).
[[nodiscard]] inline auto canonical_query_string(const std::map<std::string, std::string>& query)
    -> std::string {
    std::string out;
    for (const auto& [name, value] : query) {
        if (!out.empty()) {
            out.push_back('&');
        }
        out += detail::percent_encode(name);
        out.push_back('=');
        out += detail::percent_encode(value);
    }
    return out;
}

/// Assembles the canonical request from already-final header values.
/// `headers` must contain exactly the to-be-signed set, lowercase-keyed —
/// `sign_request` below is the intended caller; this is exposed separately so
/// the unit test can pin the vendor's worked example at each stage.
[[nodiscard]] inline auto canonical_request(std::string_view method, std::string_view path,
                                            const std::string& query_string,
                                            const std::map<std::string, std::string>& headers,
                                            std::string_view payload_hash) -> std::string {
    std::string canonical_headers;
    std::string signed_headers;
    for (const auto& [name, value] : headers) {
        canonical_headers += name;
        canonical_headers.push_back(':');
        canonical_headers += value;
        canonical_headers.push_back('\n');
        if (!signed_headers.empty()) {
            signed_headers.push_back(';');
        }
        signed_headers += name;
    }
    std::string out;
    out += method;
    out.push_back('\n');
    out += path;
    out.push_back('\n');
    out += query_string;
    out.push_back('\n');
    out += canonical_headers;
    out.push_back('\n');
    out += signed_headers;
    out.push_back('\n');
    out += payload_hash;
    return out;
}

[[nodiscard]] inline auto string_to_sign(std::string_view canonical) -> std::string {
    return std::string("ACS3-HMAC-SHA256\n") + detail::sha256_hex(canonical);
}

/// Signs one RPC-style call and returns every header to attach, the
/// `Authorization` header included. `when` and `nonce` are parameters — not
/// sampled here — so tests can pin the vendor's worked example exactly
/// (Requirement 1.2).
///
/// @throws std::invalid_argument when the AccessKey pair is incomplete.
[[nodiscard]] inline auto sign_request(
    const alibaba_client_config& cfg, std::string_view method, std::string_view host,
    std::string_view path, const std::map<std::string, std::string>& query, std::string_view action,
    std::string_view version, std::string_view body, std::chrono::system_clock::time_point when,
    std::string_view nonce) -> std::map<std::string, std::string> {
    if (cfg.access_key_id.empty()) {
        throw std::invalid_argument("alibaba_signing: access_key_id is empty");
    }
    if (cfg.access_key_secret.empty()) {
        throw std::invalid_argument("alibaba_signing: access_key_secret is empty");
    }

    const std::string payload_hash = detail::sha256_hex(body);

    // The signed set, lowercase-keyed; std::map keeps it sorted, which is the
    // canonical-header order the scheme demands.
    std::map<std::string, std::string> signed_headers{
        {"host", std::string(host)},
        {"x-acs-action", std::string(action)},
        {"x-acs-content-sha256", payload_hash},
        {"x-acs-date", detail::iso8601_utc(when)},
        {"x-acs-signature-nonce", std::string(nonce)},
        {"x-acs-version", std::string(version)},
    };
    if (!cfg.security_token.empty()) {
        signed_headers.emplace("x-acs-security-token", cfg.security_token);
    }

    const std::string canonical = canonical_request(method, path, canonical_query_string(query),
                                                    signed_headers, payload_hash);
    const std::string signature =
        detail::hmac_sha256_hex(cfg.access_key_secret, string_to_sign(canonical));

    std::string signed_names;
    for (const auto& [name, value] : signed_headers) {
        if (!signed_names.empty()) {
            signed_names.push_back(';');
        }
        signed_names += name;
    }

    auto headers = signed_headers;
    headers.emplace("Authorization", "ACS3-HMAC-SHA256 Credential=" + cfg.access_key_id +
                                         ",SignedHeaders=" + signed_names +
                                         ",Signature=" + signature);
    return headers;
}

}  // namespace kythira::alibaba_signing
