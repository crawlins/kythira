// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Server-side AWS Signature Version 4 verification
// (`.kiro/specs/iam-compatible-auth/` Requirement 3, design Component 3).
//
// Independent of any HTTP library: a front end hands over the method, the
// path and query exactly as they arrived on the wire, the headers, and the
// body when it has it whole. Verification is split in two so the caller can
// look the access key up in between:
//
//   auto parsed = sigv4::parse(request, rules, now);   // syntax, scope, time
//   if (!parsed) return parsed.error();                 // AWS error to send
//   secret = lookup(parsed->access_key_id);             // InvalidClientTokenId if none
//   auto key = cache.get(parsed->access_key_id, secret, parsed->scope());
//   if (!sigv4::verify(*parsed, key)) return signature_mismatch(...);
//
// The canonical request follows AWS's documentation, and the AWS signing test
// suite (tests/sigv4_suite/) checks it case by case:
//
//  - Path, most services: the wire path has dot segments and repeated slashes
//    removed, then is URI-encoded again ("double encoding", since an SDK
//    already encoded it once to put it on the wire).
//  - Path, S3 (`s3_path_rules`): no normalisation and no second encoding. The
//    wire path is used as sent, with only the bytes that may not appear raw
//    in a URI percent-encoded, so an existing escape such as %7E is kept as
//    the client signed it.
//  - Query: every parameter is decoded and re-encoded with the strict SigV4
//    alphabet (A-Z a-z 0-9 - _ . ~), then sorted by key and value. `+` is a
//    literal plus, not a space. A presigned request leaves X-Amz-Signature
//    out.
//  - Headers: names lower-cased; values trimmed, with runs of spaces
//    collapsed to one; repeated headers joined with "," in arrival order.

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <list>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace kythira::sigv4 {

inline constexpr std::string_view k_algorithm = "AWS4-HMAC-SHA256";
inline constexpr std::string_view k_empty_sha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
inline constexpr std::chrono::seconds k_max_skew{15 * 60};
inline constexpr std::int64_t k_max_presign_expires = 604800;  // 7 days

// ---------------------------------------------------------------------------
// Request, rules, errors
// ---------------------------------------------------------------------------

struct request {
    std::string_view method;
    std::string_view raw_path;   // as received, before any decoding
    std::string_view raw_query;  // without the leading '?'
    // Header names in any case; values as received (folded lines already
    // joined by the HTTP layer). Order matters only for repeated names.
    std::vector<std::pair<std::string, std::string>> headers;
    std::optional<std::string_view> body;  // the whole body, when not streaming
};

/// The x-amz-content-sha256 forms a front end accepts (Requirement 3.4).
enum class payload_mode : std::uint8_t {
    hex_hash = 0,                    // a hex SHA-256 of the body (or no header: computed)
    unsigned_payload = 1,            // UNSIGNED-PAYLOAD
    streaming_signed = 2,            // STREAMING-AWS4-HMAC-SHA256-PAYLOAD
    streaming_unsigned_trailer = 3,  // STREAMING-UNSIGNED-PAYLOAD-TRAILER
    streaming_signed_trailer = 4,    // STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER
};

/// How errors are spelled on the wire (Requirement 3.7).
enum class error_protocol : std::uint8_t {
    query,  // IAM, STS: awsQuery XML (SignatureDoesNotMatch, InvalidClientTokenId)
    json,   // Kinesis, DynamoDB style JSON (InvalidSignatureException, ...)
    s3,     // S3 REST XML (SignatureDoesNotMatch, InvalidAccessKeyId, ...)
};

struct service_rules {
    std::string service;  // credential-scope service: "iam", "sts", "s3", ...
    // Regions the credential scope may name; empty accepts any (the caller
    // then routes on the claim's region).
    std::vector<std::string> regions;
    // The S3 canonical-path rule (no normalisation, no second encoding).
    // The other S3 behaviours (x-amz-content-sha256 required on header
    // signatures, UNSIGNED-PAYLOAD for presigned URLs, a signed session
    // token) follow `protocol == error_protocol::s3`.
    bool s3_path_rules = false;
    error_protocol protocol = error_protocol::query;
    std::bitset<5> payload_modes{(1U << static_cast<unsigned>(payload_mode::hex_hash)) |
                                 (1U << static_cast<unsigned>(payload_mode::unsigned_payload))};
    std::chrono::seconds max_skew = k_max_skew;

    [[nodiscard]] auto accepts(payload_mode m) const -> bool {
        return payload_modes.test(static_cast<std::size_t>(m));
    }
};

struct aws_error {
    std::string code;
    std::string message;
    int http_status = 403;

    friend auto operator==(const aws_error&, const aws_error&) -> bool = default;
};

// The AWS codes and wording per protocol. Messages copy AWS's where AWS
// documents or reliably returns them.

[[nodiscard]] inline auto incomplete_signature(error_protocol p, std::string message) -> aws_error {
    switch (p) {
        case error_protocol::query:
            return {"IncompleteSignature", std::move(message), 400};
        case error_protocol::json:
            return {"IncompleteSignatureException", std::move(message), 400};
        case error_protocol::s3:
            return {"AuthorizationHeaderMalformed", std::move(message), 400};
    }
    return {"IncompleteSignature", std::move(message), 400};
}

[[nodiscard]] inline auto signature_mismatch(error_protocol p) -> aws_error {
    constexpr std::string_view text =
        "The request signature we calculated does not match the signature you provided. Check "
        "your AWS Secret Access Key and signing method. Consult the service documentation for "
        "details.";
    switch (p) {
        case error_protocol::query:
            return {"SignatureDoesNotMatch", std::string(text), 403};
        case error_protocol::json:
            return {"InvalidSignatureException", std::string(text), 400};
        case error_protocol::s3:
            return {"SignatureDoesNotMatch",
                    "The request signature we calculated does not match the signature you "
                    "provided. Check your key and signing method.",
                    403};
    }
    return {"SignatureDoesNotMatch", std::string(text), 403};
}

/// A signature-class failure with a specific message (scope, skew, unsigned
/// headers), under the protocol's signature error code.
[[nodiscard]] inline auto signature_mismatch_with(error_protocol p, std::string message)
    -> aws_error {
    auto e = signature_mismatch(p);
    e.message = std::move(message);
    return e;
}

/// The access key id is not known (or is inactive).
[[nodiscard]] inline auto invalid_client_token_id(error_protocol p) -> aws_error {
    switch (p) {
        case error_protocol::query:
            return {"InvalidClientTokenId",
                    "The security token included in the request is invalid.", 403};
        case error_protocol::json:
            return {"UnrecognizedClientException",
                    "The security token included in the request is invalid.", 400};
        case error_protocol::s3:
            return {"InvalidAccessKeyId",
                    "The AWS Access Key Id you provided does not exist in our records.", 403};
    }
    return {"InvalidClientTokenId", "The security token included in the request is invalid.", 403};
}

/// The session token's credentials have expired.
[[nodiscard]] inline auto expired_token(error_protocol p) -> aws_error {
    switch (p) {
        case error_protocol::query:
            return {"ExpiredToken", "The security token included in the request is expired", 403};
        case error_protocol::json:
            return {"ExpiredTokenException",
                    "The security token included in the request is expired", 400};
        case error_protocol::s3:
            return {"ExpiredToken", "The provided token has expired.", 400};
    }
    return {"ExpiredToken", "The security token included in the request is expired", 403};
}

/// A presigned URL past its X-Amz-Expires.
[[nodiscard]] inline auto request_expired(error_protocol p) -> aws_error {
    if (p == error_protocol::s3) {
        return {"AccessDenied", "Request has expired", 403};
    }
    return {"RequestExpired", "Request has expired.", 400};
}

/// Authenticated, but the policy says no.
[[nodiscard]] inline auto access_denied(error_protocol p, std::string message) -> aws_error {
    if (p == error_protocol::json) {
        return {"AccessDeniedException", std::move(message), 400};
    }
    return {"AccessDenied", std::move(message), 403};
}

// ---------------------------------------------------------------------------
// Encoding helpers
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] constexpr auto is_unreserved(unsigned char c) -> bool {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == '~';
}

[[nodiscard]] constexpr auto hex_value(char c) -> int {
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

inline void append_escape(std::string& out, unsigned char c) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    out += '%';
    out += hex[c >> 4U];
    out += hex[c & 0x0FU];
}

/// SigV4 URI encoding: everything but the unreserved set is %XX (upper
/// case); `keep_slash` leaves '/' alone, for paths.
[[nodiscard]] inline auto uri_encode(std::string_view in, bool keep_slash) -> std::string {
    std::string out;
    out.reserve(in.size());
    for (char ch : in) {
        const auto c = static_cast<unsigned char>(ch);
        if (is_unreserved(c) || (keep_slash && c == '/')) {
            out += ch;
        } else {
            append_escape(out, c);
        }
    }
    return out;
}

/// Percent-decoding. A '%' not followed by two hex digits is kept as a
/// literal, as AWS does; '+' stays '+'.
[[nodiscard]] inline auto uri_decode(std::string_view in) -> std::string {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = hex_value(in[i + 1]);
            const int lo = hex_value(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += in[i];
    }
    return out;
}

/// The S3 path rule: the wire path as sent, with only bytes that cannot
/// appear raw in a URI path escaped. Valid %XX escapes and the sub-delims
/// S3 leaves alone are kept; a stray '%' is escaped.
[[nodiscard]] inline auto s3_canonical_path(std::string_view wire) -> std::string {
    std::string out;
    out.reserve(wire.size());
    for (std::size_t i = 0; i < wire.size(); ++i) {
        const auto c = static_cast<unsigned char>(wire[i]);
        if (c == '%' && i + 2 < wire.size() && hex_value(wire[i + 1]) >= 0 &&
            hex_value(wire[i + 2]) >= 0) {
            out += '%';
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(wire[i + 1])));
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(wire[i + 2])));
            i += 2;
        } else if (is_unreserved(c) || c == '/') {
            out += static_cast<char>(c);
        } else {
            append_escape(out, c);
        }
    }
    return out;
}

/// RFC 3986 §5.2.4 dot-segment removal plus collapsing of repeated slashes,
/// keeping a trailing slash. An empty path is "/".
[[nodiscard]] inline auto normalize_path(std::string_view path) -> std::string {
    std::vector<std::string_view> segments;
    std::size_t pos = 0;
    while (pos <= path.size()) {
        const auto next = path.find('/', pos);
        const auto seg =
            path.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
        if (seg == "..") {
            if (!segments.empty()) {
                segments.pop_back();
            }
        } else if (!seg.empty() && seg != ".") {
            segments.push_back(seg);
        }
        if (next == std::string_view::npos) {
            break;
        }
        pos = next + 1;
    }
    std::string out = "/";
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            out += '/';
        }
        out += segments[i];
    }
    // A trailing slash (or a trailing dot segment, which names a directory)
    // survives normalisation.
    const bool trailing =
        !path.empty() && (path.ends_with('/') || path.ends_with("/.") || path.ends_with("/.."));
    if (trailing && !segments.empty()) {
        out += '/';
    }
    return out;
}

[[nodiscard]] inline auto to_hex(std::span<const unsigned char> bytes) -> std::string {
    constexpr std::string_view hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        out += hex[b >> 4U];
        out += hex[b & 0x0FU];
    }
    return out;
}

[[nodiscard]] inline auto sha256_hex(std::string_view data) -> std::string {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest.data());
    return to_hex(digest);
}

[[nodiscard]] inline auto hmac_sha256(std::span<const unsigned char> key, std::string_view data)
    -> std::array<unsigned char, 32> {
    std::array<unsigned char, 32> out{};
    unsigned int len = 0;
    if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(), out.data(),
             &len) == nullptr ||
        len != out.size()) {
        throw std::runtime_error("sigv4: HMAC-SHA256 failed");
    }
    return out;
}

[[nodiscard]] inline auto lower(std::string_view s) -> std::string {
    std::string out(s);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Trim, and collapse each run of spaces and tabs to one space.
[[nodiscard]] inline auto canonical_header_value(std::string_view v) -> std::string {
    std::string out;
    out.reserve(v.size());
    bool pending_space = false;
    for (char c : v) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out += ' ';
            pending_space = false;
        }
        out += c;
    }
    return out;
}

/// "20150830T123600Z" to seconds since the epoch.
[[nodiscard]] inline auto parse_amz_date(std::string_view s)
    -> std::optional<std::chrono::sys_seconds> {
    if (s.size() != 16 || s[8] != 'T' || s[15] != 'Z') {
        return std::nullopt;
    }
    auto num = [&](std::size_t at, std::size_t len) -> int {
        int v = 0;
        for (std::size_t i = at; i < at + len; ++i) {
            if (s[i] < '0' || s[i] > '9') {
                return -1;
            }
            v = v * 10 + (s[i] - '0');
        }
        return v;
    };
    const int y = num(0, 4);
    const int mo = num(4, 2);
    const int d = num(6, 2);
    const int h = num(9, 2);
    const int mi = num(11, 2);
    const int se = num(13, 2);
    if (y < 0 || mo < 0 || d < 0 || h < 0 || mi < 0 || se < 0 || h > 23 || mi > 59 || se > 60) {
        return std::nullopt;
    }
    const std::chrono::year_month_day ymd{std::chrono::year{y},
                                          std::chrono::month{static_cast<unsigned>(mo)},
                                          std::chrono::day{static_cast<unsigned>(d)}};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{ymd} + std::chrono::hours{h} + std::chrono::minutes{mi} +
           std::chrono::seconds{se};
}

[[nodiscard]] inline auto format_amz_date(std::chrono::sys_seconds t) -> std::string {
    const auto days = std::chrono::floor<std::chrono::days>(t);
    const std::chrono::year_month_day ymd{days};
    const std::chrono::hh_mm_ss hms{t - days};
    // Sized for any int the compiler cannot rule out, not just a 4-digit year.
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d%02u%02uT%02d%02d%02dZ", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                  static_cast<int>(hms.hours().count()), static_cast<int>(hms.minutes().count()),
                  static_cast<int>(hms.seconds().count()));
    return buf;
}

[[nodiscard]] inline auto is_lower_hex(std::string_view s, std::size_t len) -> bool {
    return s.size() == len && std::ranges::all_of(s, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

[[nodiscard]] inline auto split(std::string_view s, char sep) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    while (true) {
        const auto next = s.find(sep, pos);
        out.push_back(
            s.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos));
        if (next == std::string_view::npos) {
            return out;
        }
        pos = next + 1;
    }
}

[[nodiscard]] inline auto trim(std::string_view s) -> std::string_view {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Canonical forms
// ---------------------------------------------------------------------------

[[nodiscard]] inline auto canonical_uri(std::string_view raw_path, bool s3_path_rules)
    -> std::string {
    if (raw_path.empty()) {
        return "/";
    }
    if (s3_path_rules) {
        return detail::s3_canonical_path(raw_path);
    }
    return detail::uri_encode(detail::normalize_path(raw_path), /*keep_slash=*/true);
}

/// The canonical query string; `drop_signature` leaves X-Amz-Signature out
/// (the presigned form).
[[nodiscard]] inline auto canonical_query(std::string_view raw_query, bool drop_signature,
                                          bool drop_security_token = false) -> std::string {
    std::vector<std::pair<std::string, std::string>> params;
    if (!raw_query.empty()) {
        for (auto part : detail::split(raw_query, '&')) {
            if (part.empty()) {
                continue;
            }
            const auto eq = part.find('=');
            auto key = detail::uri_decode(part.substr(0, eq));
            auto value = eq == std::string_view::npos ? std::string{}
                                                      : detail::uri_decode(part.substr(eq + 1));
            if ((drop_signature && key == "X-Amz-Signature") ||
                (drop_security_token && key == "X-Amz-Security-Token")) {
                continue;
            }
            params.emplace_back(detail::uri_encode(key, false), detail::uri_encode(value, false));
        }
    }
    std::ranges::sort(params);
    std::string out;
    for (const auto& [k, v] : params) {
        if (!out.empty()) {
            out += '&';
        }
        out += k;
        out += '=';
        out += v;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Claim
// ---------------------------------------------------------------------------

struct credential_scope {
    std::string date;  // yyyymmdd
    std::string region;
    std::string service;

    [[nodiscard]] auto text() const -> std::string {
        return date + "/" + region + "/" + service + "/aws4_request";
    }
    friend auto operator==(const credential_scope&, const credential_scope&) -> bool = default;
};

/// What a request claims, parsed and checked except for the signature.
struct claim {
    std::string access_key_id;
    credential_scope scope;
    std::string amz_date;  // as signed
    std::chrono::sys_seconds timestamp{};
    std::vector<std::string> signed_headers;
    std::string signature;                      // lower-case hex, 64 chars
    std::optional<std::string> security_token;  // X-Amz-Security-Token, if any
    bool presigned = false;
    std::optional<std::int64_t> expires;  // presigned only
    std::string payload_hash;             // the value that went into the canonical request
    payload_mode payload = payload_mode::hex_hash;
    std::string canonical_request;
    std::string string_to_sign;
    // Presigned only, outside S3, when X-Amz-Security-Token is in the query:
    // the string to sign with the token left out of the canonical query.
    // AWS documents that some services take the token added after signing
    // (the suite's post-sts-header-after), as the header form allows by
    // leaving it unsigned. verify() accepts either. The token is still
    // checked against the access key when credentials are looked up.
    std::optional<std::string> string_to_sign_without_token;
};

/// parse()'s result: a claim, or the AWS error to answer with. (Not
/// std::expected: clang 18 over libstdc++ 13, the static-analysis setup,
/// does not provide it.)
class parse_result {
public:
    parse_result(claim c) : _v(std::move(c)) {}      // NOLINT(google-explicit-constructor)
    parse_result(aws_error e) : _v(std::move(e)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] auto has_value() const -> bool { return _v.index() == 0; }
    explicit operator bool() const { return has_value(); }
    [[nodiscard]] auto value() const -> const claim& { return std::get<0>(_v); }
    [[nodiscard]] auto operator*() const -> const claim& { return value(); }
    [[nodiscard]] auto operator->() const -> const claim* { return &value(); }
    /// The error, or an empty one when there is a claim (so a log or test
    /// message can name it unconditionally).
    [[nodiscard]] auto error() const -> const aws_error& {
        static const aws_error none{};
        return has_value() ? none : std::get<1>(_v);
    }

private:
    std::variant<claim, aws_error> _v;
};

namespace detail {

struct header_view {
    // lower-cased name -> values in arrival order
    std::vector<std::pair<std::string, std::vector<std::string_view>>> entries;

    [[nodiscard]] auto find(std::string_view name) const -> const std::vector<std::string_view>* {
        for (const auto& [n, v] : entries) {
            if (n == name) {
                return &v;
            }
        }
        return nullptr;
    }
    [[nodiscard]] auto single(std::string_view name) const -> std::optional<std::string_view> {
        const auto* v = find(name);
        if (v == nullptr || v->empty()) {
            return std::nullopt;
        }
        return trim(v->front());
    }
};

[[nodiscard]] inline auto index_headers(const request& req) -> header_view {
    header_view out;
    for (const auto& [name, value] : req.headers) {
        auto lname = lower(name);
        auto it = std::ranges::find(out.entries, lname, &decltype(out.entries)::value_type::first);
        if (it == out.entries.end()) {
            out.entries.emplace_back(std::move(lname), std::vector<std::string_view>{value});
        } else {
            it->second.emplace_back(value);
        }
    }
    return out;
}

/// The query parameters decoded, for reading the presigned X-Amz-* fields.
[[nodiscard]] inline auto decoded_query(std::string_view raw_query)
    -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> out;
    if (raw_query.empty()) {
        return out;
    }
    for (auto part : split(raw_query, '&')) {
        if (part.empty()) {
            continue;
        }
        const auto eq = part.find('=');
        out.emplace_back(uri_decode(part.substr(0, eq)), eq == std::string_view::npos
                                                             ? std::string{}
                                                             : uri_decode(part.substr(eq + 1)));
    }
    return out;
}

[[nodiscard]] inline auto query_param(const std::vector<std::pair<std::string, std::string>>& q,
                                      std::string_view name) -> const std::string* {
    const std::string* found = nullptr;
    for (const auto& [k, v] : q) {
        if (k == name) {
            if (found != nullptr) {
                return nullptr;  // repeated: ambiguous, treated as missing
            }
            found = &v;
        }
    }
    return found;
}

[[nodiscard]] inline auto payload_mode_of(std::string_view v) -> std::optional<payload_mode> {
    if (v == "UNSIGNED-PAYLOAD") {
        return payload_mode::unsigned_payload;
    }
    if (v == "STREAMING-AWS4-HMAC-SHA256-PAYLOAD") {
        return payload_mode::streaming_signed;
    }
    if (v == "STREAMING-UNSIGNED-PAYLOAD-TRAILER") {
        return payload_mode::streaming_unsigned_trailer;
    }
    if (v == "STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER") {
        return payload_mode::streaming_signed_trailer;
    }
    if (is_lower_hex(v, 64)) {
        return payload_mode::hex_hash;
    }
    return std::nullopt;
}

}  // namespace detail

/// Parses and checks everything but the signature itself: the algorithm,
/// credential and scope (date, region, service), the signed-header list
/// (host and every x-amz-* header present must be signed), the clock-skew
/// window or presigned expiry, and the payload hash. Builds the canonical
/// request and string to sign. `now` is the server's clock.
[[nodiscard]] inline auto parse(const request& req, const service_rules& rules,
                                std::chrono::sys_seconds now) -> parse_result {
    using namespace detail;
    const auto p = rules.protocol;
    const auto headers = index_headers(req);
    const auto query = decoded_query(req.raw_query);

    claim c;
    std::string credential;
    std::string signed_headers_text;

    const auto auth = headers.single("authorization");
    const auto* q_algorithm = query_param(query, "X-Amz-Algorithm");
    if (auth.has_value() && q_algorithm != nullptr) {
        return incomplete_signature(
            p,
            "Only one auth mechanism allowed; only the X-Amz-Algorithm query parameter or the "
            "Authorization header should be specified");
    }

    if (auth.has_value()) {
        // AWS4-HMAC-SHA256 Credential=..., SignedHeaders=..., Signature=...
        auto text = *auth;
        if (!text.starts_with(k_algorithm) || text.size() == k_algorithm.size() ||
            (text[k_algorithm.size()] != ' ' && text[k_algorithm.size()] != '\t')) {
            return incomplete_signature(
                p, "Authorization header requires the AWS4-HMAC-SHA256 algorithm.");
        }
        text.remove_prefix(k_algorithm.size());
        std::optional<std::string> sig;
        for (auto part : split(text, ',')) {
            part = trim(part);
            const auto eq = part.find('=');
            if (eq == std::string_view::npos) {
                return incomplete_signature(p, "Authorization header is malformed.");
            }
            const auto key = part.substr(0, eq);
            const auto value = std::string(part.substr(eq + 1));
            if (key == "Credential") {
                credential = value;
            } else if (key == "SignedHeaders") {
                signed_headers_text = value;
            } else if (key == "Signature") {
                sig = value;
            }
        }
        std::string missing;
        if (credential.empty()) {
            missing += "Authorization header requires 'Credential' parameter. ";
        }
        if (signed_headers_text.empty()) {
            missing += "Authorization header requires 'SignedHeaders' parameter. ";
        }
        if (!sig) {
            missing += "Authorization header requires 'Signature' parameter. ";
        }
        if (!headers.single("x-amz-date")) {
            missing +=
                "Authorization header requires existence of either a 'X-Amz-Date' or a "
                "'Date' header. ";
        }
        if (!missing.empty()) {
            missing.pop_back();
            return incomplete_signature(p, missing);
        }
        c.signature = *sig;
        c.amz_date = std::string(*headers.single("x-amz-date"));
        if (auto tok = headers.single("x-amz-security-token")) {
            c.security_token = std::string(*tok);
        }
    } else if (q_algorithm != nullptr) {
        c.presigned = true;
        if (*q_algorithm != k_algorithm) {
            return incomplete_signature(p, "X-Amz-Algorithm only supports \"AWS4-HMAC-SHA256\"");
        }
        std::string missing;
        const auto* q_cred = query_param(query, "X-Amz-Credential");
        const auto* q_date = query_param(query, "X-Amz-Date");
        const auto* q_signed = query_param(query, "X-Amz-SignedHeaders");
        const auto* q_sig = query_param(query, "X-Amz-Signature");
        const auto* q_expires = query_param(query, "X-Amz-Expires");
        if (q_cred == nullptr) {
            missing += "AWS query-string parameters must include 'X-Amz-Credential'. ";
        }
        if (q_signed == nullptr) {
            missing += "AWS query-string parameters must include 'X-Amz-SignedHeaders'. ";
        }
        if (q_sig == nullptr) {
            missing += "AWS query-string parameters must include 'X-Amz-Signature'. ";
        }
        if (q_date == nullptr) {
            missing += "AWS query-string parameters must include 'X-Amz-Date'. ";
        }
        if (q_expires == nullptr) {
            missing += "AWS query-string parameters must include 'X-Amz-Expires'. ";
        }
        if (!missing.empty()) {
            missing += "Re-examine the query-string parameters.";
            return incomplete_signature(p, missing);
        }
        credential = *q_cred;
        signed_headers_text = *q_signed;
        c.signature = *q_sig;
        c.amz_date = *q_date;
        std::int64_t expires = 0;
        for (char ch : *q_expires) {
            if (ch < '0' || ch > '9' || expires > k_max_presign_expires) {
                expires = -1;
                break;
            }
            expires = expires * 10 + (ch - '0');
        }
        if (q_expires->empty() || expires < 1 || expires > k_max_presign_expires) {
            return incomplete_signature(p, "X-Amz-Expires must be between 1 and 604800 seconds");
        }
        c.expires = expires;
        if (const auto* tok = query_param(query, "X-Amz-Security-Token")) {
            c.security_token = *tok;
        }
    } else {
        return incomplete_signature(
            p, "Request is missing Authentication Token");  // what AWS says to an unsigned call
    }

    // Credential=<akid>/<date>/<region>/<service>/aws4_request
    const auto cred_parts = split(credential, '/');
    if (cred_parts.size() != 5 || cred_parts[0].empty() || cred_parts[4] != "aws4_request") {
        return incomplete_signature(
            p, "Credential should be scoped to a valid region, not '" +
                   (cred_parts.size() > 2 ? std::string(cred_parts[2]) : std::string()) + "'. ");
    }
    c.access_key_id = std::string(cred_parts[0]);
    c.scope = {std::string(cred_parts[1]), std::string(cred_parts[2]), std::string(cred_parts[3])};

    const auto ts = parse_amz_date(c.amz_date);
    if (!ts) {
        return incomplete_signature(
            p, "Date must be in ISO-8601 'basic format'. Got '" + c.amz_date + "'.");
    }
    c.timestamp = *ts;
    if (c.scope.date != c.amz_date.substr(0, 8)) {
        return signature_mismatch_with(
            p, "Credential should be scoped to a valid date, not '" + c.scope.date + "'.");
    }
    if (c.scope.service != rules.service) {
        return signature_mismatch_with(
            p, "Credential should be scoped to correct service: '" + rules.service + "'. ");
    }
    if (!rules.regions.empty() &&
        std::ranges::find(rules.regions, c.scope.region) == rules.regions.end()) {
        return signature_mismatch_with(
            p, "Credential should be scoped to a valid region, not '" + c.scope.region + "'. ");
    }
    if (!is_lower_hex(c.signature, 64)) {
        return signature_mismatch(p);
    }

    // Time: a header signature within the skew window; a presigned URL from
    // its date (not more than the skew early) until date + X-Amz-Expires.
    if (c.presigned) {
        if (c.timestamp - now > rules.max_skew) {
            return signature_mismatch_with(p, "Request is not yet valid");
        }
        if (now > c.timestamp + std::chrono::seconds{*c.expires}) {
            return request_expired(p);
        }
    } else if (now - c.timestamp > rules.max_skew) {
        if (p == error_protocol::s3) {
            return aws_error{"RequestTimeTooSkewed",
                             "The difference between the request time and the current time is too "
                             "large.",
                             403};
        }
        return signature_mismatch_with(
            p, "Signature expired: " + c.amz_date + " is now earlier than " +
                   format_amz_date(now - rules.max_skew) + " (" + format_amz_date(now) + " - " +
                   std::to_string(rules.max_skew.count() / 60) + " min.)");
    } else if (c.timestamp - now > rules.max_skew) {
        if (p == error_protocol::s3) {
            return aws_error{"RequestTimeTooSkewed",
                             "The difference between the request time and the current time is too "
                             "large.",
                             403};
        }
        return signature_mismatch_with(
            p, "Signature not yet current: " + c.amz_date + " is still later than " +
                   format_amz_date(now + rules.max_skew) + " (" + format_amz_date(now) + " + " +
                   std::to_string(rules.max_skew.count() / 60) + " min.)");
    }

    // Signed headers: lower case, sorted, unique, each present; host always;
    // every x-amz-* header present (except, outside S3, the security token,
    // which AWS documents may be added after signing).
    for (auto name : split(signed_headers_text, ';')) {
        if (name.empty() || lower(name) != name) {
            return incomplete_signature(p, "SignedHeaders must be lower case and ';'-separated.");
        }
        if (!c.signed_headers.empty() && !(c.signed_headers.back() < name)) {
            return incomplete_signature(p, "SignedHeaders must be sorted and unique.");
        }
        if (headers.find(name) == nullptr) {
            return signature_mismatch(p);
        }
        c.signed_headers.emplace_back(name);
    }
    auto is_signed = [&](std::string_view name) {
        return std::ranges::binary_search(c.signed_headers, name);
    };
    if (!is_signed("host")) {
        return signature_mismatch_with(p,
                                       "'Host' must be a 'SignedHeader' in the AWS "
                                       "Authorization.");
    }
    for (const auto& [name, values] : headers.entries) {
        if (!name.starts_with("x-amz-") || is_signed(name) || name == "x-amz-date" ||
            name == "x-amz-content-sha256") {
            continue;
        }
        if (name == "x-amz-security-token" && p != error_protocol::s3) {
            continue;
        }
        return signature_mismatch_with(p,
                                       "There were headers present in the request which were "
                                       "not signed: " +
                                           name);
    }
    // x-amz-date and x-amz-content-sha256 are covered by the string to sign
    // or the canonical request's last line, but AWS still requires them to
    // be signed when present on a header-signed request.
    if (!c.presigned) {
        for (std::string_view name : {"x-amz-date", "x-amz-content-sha256"}) {
            if (headers.find(name) != nullptr && !is_signed(name)) {
                return signature_mismatch_with(
                    p, "There were headers present in the request which were not signed: " +
                           std::string(name));
            }
        }
    }

    // Payload hash.
    if (auto declared = headers.single("x-amz-content-sha256")) {
        const auto mode = payload_mode_of(*declared);
        if (!mode || !rules.accepts(*mode)) {
            return aws_error{p == error_protocol::s3 ? "InvalidArgument" : "InvalidRequest",
                             "x-amz-content-sha256 must be UNSIGNED-PAYLOAD, a supported "
                             "STREAMING value, or a valid sha256 value.",
                             400};
        }
        c.payload = *mode;
        c.payload_hash = std::string(*declared);
        if (*mode == payload_mode::hex_hash && req.body.has_value() &&
            sha256_hex(*req.body) != c.payload_hash) {
            return aws_error{
                p == error_protocol::s3 ? "XAmzContentSHA256Mismatch" : "InvalidSignatureException",
                "The provided 'x-amz-content-sha256' header does not match what "
                "was computed.",
                400};
        }
    } else if (c.presigned && p == error_protocol::s3) {
        c.payload = payload_mode::unsigned_payload;
        c.payload_hash = "UNSIGNED-PAYLOAD";
    } else if (p == error_protocol::s3) {
        return aws_error{"InvalidRequest",
                         "Missing required header for this request: "
                         "x-amz-content-sha256",
                         400};
    } else {
        if (!req.body.has_value() && req.method != "GET" && req.method != "HEAD") {
            // Nothing to hash and no declared hash: the front end must pass
            // the body, or the client must declare the hash.
            return incomplete_signature(p, "The request body is required to verify the payload.");
        }
        c.payload_hash = req.body.has_value() ? sha256_hex(*req.body) : std::string(k_empty_sha256);
    }

    // Canonical request and string to sign.
    std::string canonical;
    canonical.reserve(512);
    canonical += req.method;
    canonical += '\n';
    canonical += canonical_uri(req.raw_path, rules.s3_path_rules);
    canonical += '\n';
    canonical += canonical_query(req.raw_query, c.presigned);
    canonical += '\n';
    for (const auto& name : c.signed_headers) {
        canonical += name;
        canonical += ':';
        const auto* values = headers.find(name);
        for (std::size_t i = 0; i < values->size(); ++i) {
            if (i > 0) {
                canonical += ',';
            }
            canonical += canonical_header_value((*values)[i]);
        }
        canonical += '\n';
    }
    canonical += '\n';
    canonical += signed_headers_text;
    canonical += '\n';
    canonical += c.payload_hash;
    c.canonical_request = std::move(canonical);

    const auto sts_prefix =
        std::string(k_algorithm) + "\n" + c.amz_date + "\n" + c.scope.text() + "\n";
    c.string_to_sign = sts_prefix + sha256_hex(c.canonical_request);
    if (c.presigned && c.security_token && p != error_protocol::s3) {
        auto without = c.canonical_request;
        const auto q_start = without.find('\n') + 1;
        const auto uri_end = without.find('\n', q_start);
        const auto query_end = without.find('\n', uri_end + 1);
        without.replace(uri_end + 1, query_end - uri_end - 1,
                        canonical_query(req.raw_query, true, true));
        c.string_to_sign_without_token = sts_prefix + sha256_hex(without);
    }
    return c;
}

// ---------------------------------------------------------------------------
// Keys and verification
// ---------------------------------------------------------------------------

using signing_key = std::array<unsigned char, 32>;

/// kSigning = HMAC(HMAC(HMAC(HMAC("AWS4" + secret, date), region), service),
/// "aws4_request").
[[nodiscard]] inline auto derive_signing_key(std::string_view secret, const credential_scope& scope)
    -> signing_key {
    std::string k0 = "AWS4";
    k0 += secret;
    const auto* raw = reinterpret_cast<const unsigned char*>(k0.data());
    auto k_date = detail::hmac_sha256({raw, k0.size()}, scope.date);
    OPENSSL_cleanse(k0.data(), k0.size());
    auto k_region = detail::hmac_sha256(k_date, scope.region);
    auto k_service = detail::hmac_sha256(k_region, scope.service);
    auto k_signing = detail::hmac_sha256(k_service, "aws4_request");
    OPENSSL_cleanse(k_date.data(), k_date.size());
    OPENSSL_cleanse(k_region.data(), k_region.size());
    OPENSSL_cleanse(k_service.data(), k_service.size());
    return k_signing;
}

/// The hex signature `key` gives the claim's string to sign.
[[nodiscard]] inline auto compute_signature(const claim& c, const signing_key& key) -> std::string {
    return detail::to_hex(detail::hmac_sha256(key, c.string_to_sign));
}

/// Whether the claim's signature is right, compared in constant time
/// (Requirement 3.5).
[[nodiscard]] inline auto verify(const claim& c, const signing_key& key) -> bool {
    auto matches = [&](const std::string& sts) {
        const auto expected = detail::to_hex(detail::hmac_sha256(key, sts));
        return c.signature.size() == expected.size() &&
               CRYPTO_memcmp(c.signature.data(), expected.data(), expected.size()) == 0;
    };
    // Both candidates are always computed, so timing does not reveal which
    // form a request used.
    const bool primary = matches(c.string_to_sign);
    const bool alternate =
        c.string_to_sign_without_token.has_value() && matches(*c.string_to_sign_without_token);
    return primary || alternate;
}

/// Derived signing keys per (access key, date, region, service), least
/// recently used first out (Requirement 3.6). A derived key is valid for a
/// whole day, so a busy key costs four HMACs once a day rather than per
/// request. Thread-safe.
class signing_key_cache {
public:
    explicit signing_key_cache(std::size_t capacity = 4096) : _capacity(capacity) {
        if (_capacity == 0) {
            throw std::invalid_argument("signing_key_cache: capacity must be positive");
        }
    }

    /// The cached key, or one derived from `secret` and cached.
    [[nodiscard]] auto get(std::string_view access_key_id, std::string_view secret,
                           const credential_scope& scope) -> signing_key {
        const auto id = cache_key(access_key_id, scope);
        std::lock_guard lock(_mu);
        if (auto it = _index.find(id); it != _index.end()) {
            _lru.splice(_lru.begin(), _lru, it->second);
            return it->second->key;
        }
        auto key = derive_signing_key(secret, scope);
        _lru.push_front(entry{id, std::string(access_key_id), key});
        _index.emplace(id, _lru.begin());
        while (_lru.size() > _capacity) {
            OPENSSL_cleanse(_lru.back().key.data(), _lru.back().key.size());
            _index.erase(_lru.back().id);
            _lru.pop_back();
        }
        return key;
    }

    /// Drops every key derived for `access_key_id`: call when it is
    /// deactivated, deleted or its secret changes.
    void invalidate(std::string_view access_key_id) {
        std::lock_guard lock(_mu);
        for (auto it = _lru.begin(); it != _lru.end();) {
            if (it->access_key_id == access_key_id) {
                OPENSSL_cleanse(it->key.data(), it->key.size());
                _index.erase(it->id);
                it = _lru.erase(it);
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]] auto size() const -> std::size_t {
        std::lock_guard lock(_mu);
        return _lru.size();
    }

private:
    struct entry {
        std::string id;
        std::string access_key_id;
        signing_key key;
    };

    [[nodiscard]] static auto cache_key(std::string_view akid, const credential_scope& s)
        -> std::string {
        // Length-prefixed, so no choice of bytes in one part can make two
        // different tuples collide.
        std::string id;
        for (std::string_view part : {akid, std::string_view(s.date), std::string_view(s.region),
                                      std::string_view(s.service)}) {
            id += std::to_string(part.size());
            id += ':';
            id += part;
        }
        return id;
    }

    std::size_t _capacity;
    mutable std::mutex _mu;
    std::list<entry> _lru;
    std::unordered_map<std::string, std::list<entry>::iterator> _index;
};

}  // namespace kythira::sigv4
