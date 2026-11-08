// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_validation.hpp
/// @brief Request-parameter rules from AWS's IAM and STS API references
///        (`.kiro/specs/iam-compatible-auth/invariants.md`).
///
/// Pure functions, no state: the Query handlers (tasks 14-16) and the STS
/// actions (tasks 17-20 and 26) call them before building a command or
/// minting credentials, and answer `InvalidInput` / `ValidationError` (or
/// the code a function names) when one fails. Each function cites the
/// invariant ids it enforces; `iam_validation_test.cpp` tests every one at
/// and either side of its bounds.
///
/// Lengths are in characters (Unicode code points), as AWS counts them, so
/// text is decoded as UTF-8 first and malformed UTF-8 fails every check.
///
/// Where AWS's pattern uses Unicode categories (tag keys and values allow
/// `\p{L}\p{Z}\p{N}`), every non-ASCII code point is accepted: a category
/// table would be a large dependency for a rule AWS itself applies loosely.
/// Every ASCII character is held to the exact pattern.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::iam::validate {

namespace detail {

/// `s` as code points, or nothing if it is not well-formed UTF-8
/// (overlong forms, surrogates and values past U+10FFFF are refused).
[[nodiscard]] inline auto decode_utf8(std::string_view s) -> std::optional<std::u32string> {
    std::u32string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        char32_t cp = 0;
        std::size_t n = 0;
        if (b0 < 0x80) {
            cp = b0;
            n = 1;
        } else if ((b0 & 0xE0U) == 0xC0U) {
            cp = b0 & 0x1FU;
            n = 2;
        } else if ((b0 & 0xF0U) == 0xE0U) {
            cp = b0 & 0x0FU;
            n = 3;
        } else if ((b0 & 0xF8U) == 0xF0U) {
            cp = b0 & 0x07U;
            n = 4;
        } else {
            return std::nullopt;
        }
        if (i + n > s.size()) {
            return std::nullopt;
        }
        for (std::size_t k = 1; k < n; ++k) {
            const auto b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0U) != 0x80U) {
                return std::nullopt;
            }
            cp = (cp << 6U) | (b & 0x3FU);
        }
        constexpr char32_t min_for[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < min_for[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return std::nullopt;
        }
        out.push_back(cp);
        i += n;
    }
    return out;
}

/// `[A-Za-z0-9_]` (the regex `\w` in AWS's patterns) or one of `extra`.
[[nodiscard]] inline auto word_or(char c, std::string_view extra) -> bool {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' ||
           extra.find(c) != std::string_view::npos;
}

/// `[\w<extra>]*` with a length in [min, max]. These patterns are ASCII
/// only, so bytes are characters.
[[nodiscard]] inline auto word_set(std::string_view s, std::size_t min, std::size_t max,
                                   std::string_view extra) -> bool {
    return s.size() >= min && s.size() <= max &&
           std::ranges::all_of(s, [&](char c) { return word_or(c, extra); });
}

[[nodiscard]] inline auto starts_with_aws_prefix(std::string_view s) -> bool {
    if (s.size() < 4) {
        return false;
    }
    std::string head(s.substr(0, 4));
    std::ranges::transform(head, head.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return head == "aws:";
}

[[nodiscard]] inline auto fold(std::string_view s) -> std::string {
    std::string out(s);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// The characters AWS allows in a policy document (and an STS `Policy`):
/// tab, LF, CR and U+0020 through U+00FF.
[[nodiscard]] inline auto policy_char(char32_t c) -> bool {
    return c == 0x09 || c == 0x0A || c == 0x0D || (c >= 0x20 && c <= 0xFF);
}

/// Characters other than JSON whitespace: how AWS sizes a policy.
[[nodiscard]] inline auto non_whitespace_chars(const std::u32string& s) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        s, [](char32_t c) { return c != U' ' && c != U'\t' && c != U'\n' && c != U'\r'; }));
}

}  // namespace detail

// ── IAM entities ────────────────────────────────────────────────────────────

/// INV-NAME-4: an inline policy name is 1–128 Basic Latin characters other
/// than `\`, `/`, `*`, `?` and white space.
[[nodiscard]] inline auto inline_policy_name(std::string_view s) -> bool {
    return !s.empty() && s.size() <= 128 && std::ranges::all_of(s, [](char c) {
        return c > ' ' && c <= '~' && c != '\\' && c != '/' && c != '*' && c != '?';
    });
}

/// INV-ACCT-2: an account alias is 3–63 lower-case letters, digits and
/// hyphens, neither starting nor ending with a hyphen, with no two hyphens
/// in a row, and not a 12-digit number (it would read as an account id).
[[nodiscard]] inline auto account_alias(std::string_view a) -> bool {
    if (a.size() < 3 || a.size() > 63 || a.front() == '-' || a.back() == '-' ||
        a.find("--") != std::string_view::npos) {
        return false;
    }
    if (a.size() == 12 && std::ranges::all_of(a, [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    return std::ranges::all_of(
        a, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

namespace detail {
[[nodiscard]] inline auto tag_text(std::string_view s, std::size_t min, std::size_t max) -> bool {
    const auto cps = decode_utf8(s);
    if (!cps || cps->size() < min || cps->size() > max || starts_with_aws_prefix(s)) {
        return false;
    }
    return std::ranges::all_of(*cps, [](char32_t c) {
        if (c >= 0x80) {
            return true;  // \p{L}\p{Z}\p{N}, approximated (see the file comment)
        }
        const auto ch = static_cast<char>(c);
        return std::isalnum(static_cast<unsigned char>(ch)) != 0 ||
               std::string_view(" _.:/=+-@").find(ch) != std::string_view::npos;
    });
}
}  // namespace detail

/// INV-TAG-2, INV-TAG-4: a tag key is 1–128 characters of letters, numbers,
/// spaces and `_ . : / = + - @`, not starting with the reserved `aws:`.
[[nodiscard]] inline auto tag_key(std::string_view s) -> bool {
    return detail::tag_text(s, 1, 128);
}

/// INV-TAG-3, INV-TAG-4: a tag value is 0–256 characters of the same set,
/// not starting with `aws:`; an empty value is allowed.
[[nodiscard]] inline auto tag_value(std::string_view s) -> bool {
    return detail::tag_text(s, 0, 256);
}

/// Whether two tag keys name the same tag: case-insensitively on users,
/// roles and STS sessions (INV-TAG-5), exactly on every other resource.
enum class tag_keys : std::uint8_t {
    case_sensitive,
    case_insensitive
};

using tag_list = std::vector<std::pair<std::string, std::string>>;

/// INV-TAG-1..5: why a request's tags are refused, or nothing. At most
/// `max` tags, each key and value valid, and no key given twice. Any bad
/// tag fails the whole request (INV-TAG-6): callers check before mutating.
[[nodiscard]] inline auto tags_error(const tag_list& tags, tag_keys keys, std::size_t max = 50)
    -> std::optional<std::string> {
    if (tags.size() > max) {
        return "Cannot exceed quota for TagsPerEntity: " + std::to_string(max);
    }
    std::vector<std::string> seen;
    for (const auto& [k, v] : tags) {
        if (!tag_key(k)) {
            return "Invalid tag key: " + k;
        }
        if (!tag_value(v)) {
            return "Invalid tag value for key " + k;
        }
        auto norm = keys == tag_keys::case_insensitive ? detail::fold(k) : k;
        if (std::ranges::find(seen, norm) != seen.end()) {
            return "Duplicate tag key: " + k;
        }
        seen.push_back(std::move(norm));
    }
    return std::nullopt;
}

/// INV-ROLE-1: a role's MaxSessionDuration is 3600–43200 seconds.
[[nodiscard]] inline auto max_session_duration(std::int64_t s) -> bool {
    return s >= 3600 && s <= 43200;
}

/// INV-ROLE-2: a role description is at most 1000 characters of tab, LF,
/// CR, printable ASCII and U+00A1–U+00FF.
[[nodiscard]] inline auto role_description(std::string_view s) -> bool {
    const auto cps = detail::decode_utf8(s);
    return cps && cps->size() <= 1000 && std::ranges::all_of(*cps, [](char32_t c) {
               return c == 0x09 || c == 0x0A || c == 0x0D || (c >= 0x20 && c <= 0x7E) ||
                      (c >= 0xA1 && c <= 0xFF);
           });
}

/// INV-POL-1: a PolicyDocument parameter is 1–131072 characters of tab, LF,
/// CR and U+0020–U+00FF. (The per-type quotas, 6144 and so on, are the
/// state machine's; this is the API's outer bound.)
[[nodiscard]] inline auto policy_document_text(std::string_view s) -> bool {
    const auto cps = detail::decode_utf8(s);
    return cps && !cps->empty() && cps->size() <= 131072 &&
           std::ranges::all_of(*cps, detail::policy_char);
}

/// INV-POL-6: a policy VersionId is `v[1-9][0-9]*(\.[A-Za-z0-9-]*)?`.
[[nodiscard]] inline auto policy_version_id(std::string_view s) -> bool {
    if (s.size() < 2 || s[0] != 'v' || s[1] < '1' || s[1] > '9') {
        return false;
    }
    std::size_t i = 2;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        ++i;
    }
    if (i == s.size()) {
        return true;
    }
    if (s[i] != '.') {
        return false;
    }
    return std::ranges::all_of(s.substr(i + 1), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-';
    });
}

/// INV-ARN-1: an ARN parameter (RoleArn, PolicyArn, PermissionsBoundary)
/// is 20–2048 characters.
[[nodiscard]] inline auto arn_length(std::string_view s) -> bool {
    const auto cps = detail::decode_utf8(s);
    return cps && cps->size() >= 20 && cps->size() <= 2048;
}

// ── OIDC providers ──────────────────────────────────────────────────────────

/// INV-OIDC-1: a provider URL is 1–255 characters, begins `https://`, has
/// a host, and carries no query, fragment, port or user info. A path is
/// allowed.
[[nodiscard]] inline auto oidc_url(std::string_view s) -> bool {
    constexpr std::string_view scheme = "https://";
    if (s.size() > 255 || !s.starts_with(scheme)) {
        return false;
    }
    const auto rest = s.substr(scheme.size());
    const auto host = rest.substr(0, rest.find('/'));
    if (host.empty() || rest.find_first_of("?#") != std::string_view::npos ||
        host.find_first_of(":@") != std::string_view::npos) {
        return false;
    }
    return std::ranges::all_of(rest, [](char c) { return c > ' ' && c <= '~'; });
}

/// INV-OIDC-2: a client id (audience) is 1–255 characters.
[[nodiscard]] inline auto oidc_client_id(std::string_view s) -> bool {
    const auto cps = detail::decode_utf8(s);
    return cps && !cps->empty() && cps->size() <= 255;
}

/// INV-OIDC-3: a thumbprint is the 40 hex digits of a SHA-1 hash.
[[nodiscard]] inline auto oidc_thumbprint(std::string_view s) -> bool {
    return s.size() == 40 && std::ranges::all_of(s, [](char c) {
               return std::isxdigit(static_cast<unsigned char>(c));
           });
}

// ── STS parameters ──────────────────────────────────────────────────────────

/// INV-STS-1: RoleSessionName is 2–64 characters of `[\w+=,.@-]`.
[[nodiscard]] inline auto role_session_name(std::string_view s) -> bool {
    return detail::word_set(s, 2, 64, "+=,.@-");
}

/// INV-STS-2: GetFederationToken's Name is 2–32 characters of `[\w+=,.@-]`.
[[nodiscard]] inline auto federated_user_name(std::string_view s) -> bool {
    return detail::word_set(s, 2, 32, "+=,.@-");
}

/// INV-STS-3: ExternalId is 2–1224 characters of `[\w+=,.@:/-]`.
[[nodiscard]] inline auto external_id(std::string_view s) -> bool {
    return detail::word_set(s, 2, 1224, "+=,.@:/-");
}

/// INV-STS-4: an MFA SerialNumber is 9–256 characters of `[\w+=/:,.@-]`.
[[nodiscard]] inline auto mfa_serial_number(std::string_view s) -> bool {
    return detail::word_set(s, 9, 256, "+=/:,.@-");
}

/// INV-STS-5: an MFA TokenCode is exactly six digits.
[[nodiscard]] inline auto mfa_token_code(std::string_view s) -> bool {
    return s.size() == 6 && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

/// INV-STS-6: SourceIdentity is 2–64 characters of `[\w+=,.@-]` and does
/// not begin with the reserved `aws:`.
[[nodiscard]] inline auto source_identity(std::string_view s) -> bool {
    return detail::word_set(s, 2, 64, "+=,.@-") && !detail::starts_with_aws_prefix(s);
}

/// INV-STS-7: MinimumSessionTokenSize is 0–4096 bytes.
[[nodiscard]] inline auto minimum_session_token_size(std::int64_t n) -> bool {
    return n >= 0 && n <= 4096;
}

/// INV-WEB-1: a WebIdentityToken is 4–20000 characters.
[[nodiscard]] inline auto web_identity_token_length(std::string_view s) -> bool {
    return s.size() >= 4 && s.size() <= 20000;
}

// ── STS session durations ───────────────────────────────────────────────────

/// A session length in seconds, or why the request is refused. Not
/// std::expected: clang 18 over libstdc++ 13 (CI's clang builds and
/// static analysis) lacks it.
class duration {
public:
    duration(std::int64_t seconds) : _seconds(seconds) {}  // NOLINT(google-explicit-constructor)
    static auto refused(std::string why) -> duration { return duration(std::move(why)); }

    [[nodiscard]] auto has_value() const noexcept -> bool { return _seconds.has_value(); }
    [[nodiscard]] auto value() const -> std::int64_t {
        if (!_seconds) {
            throw std::logic_error("duration refused: " + _error);
        }
        return *_seconds;
    }
    [[nodiscard]] auto error() const noexcept -> const std::string& { return _error; }

private:
    explicit duration(std::string why) : _error(std::move(why)) {}
    std::optional<std::int64_t> _seconds;
    std::string _error;
};

/// INV-DUR-1..3: AssumeRole's DurationSeconds. Default 3600; otherwise
/// 900–43200 and no more than the role's MaxSessionDuration. A chained
/// session (assumed with role credentials) may not ask for more than an
/// hour: the request fails, it is not shortened.
[[nodiscard]] inline auto assume_role_duration(std::optional<std::int64_t> requested,
                                               std::int64_t role_max, bool role_chaining)
    -> duration {
    if (!requested) {
        return 3600;
    }
    const auto d = *requested;
    if (d < 900 || d > 43200) {
        return duration::refused("DurationSeconds must be between 900 and 43200");
    }
    if (d > role_max) {
        return duration::refused(
            "The requested DurationSeconds exceeds the MaxSessionDuration set "
            "for this role.");
    }
    if (role_chaining && d > 3600) {
        return duration::refused(
            "The requested DurationSeconds exceeds the 1 hour session limit "
            "for roles assumed by role chaining.");
    }
    return d;
}

/// INV-DUR-4: AssumeRoleWithWebIdentity's DurationSeconds: as AssumeRole,
/// with no chaining (the call is unsigned).
[[nodiscard]] inline auto web_identity_duration(std::optional<std::int64_t> requested,
                                                std::int64_t role_max) -> duration {
    return assume_role_duration(requested, role_max, false);
}

/// INV-DUR-5, INV-DUR-6: GetSessionToken's and GetFederationToken's
/// DurationSeconds. 900–129600, default 43200 for an IAM user. For the
/// account root the default is 3600 and a longer request is shortened to
/// 3600 rather than refused.
[[nodiscard]] inline auto long_term_duration(std::optional<std::int64_t> requested, bool root)
    -> duration {
    if (requested && (*requested < 900 || *requested > 129600)) {
        return duration::refused("DurationSeconds must be between 900 and 129600");
    }
    if (root) {
        return std::min<std::int64_t>(requested.value_or(3600), 3600);
    }
    return requested.value_or(43200);
}

// ── STS session policies and tags ───────────────────────────────────────────

/// INV-SPOL-1..4: why a request's session policies are refused, or nothing.
/// `Policy`, when given, is 1–2048 characters of the policy character set;
/// at most 10 `PolicyArns`, each in `account` (or AWS-managed); and the
/// policy's characters (white space not counted) plus the ARNs' together
/// are at most 2048. The first failure maps to `MalformedPolicyDocument`
/// or `PackedPolicyTooLarge`, given as the pair's first element.
[[nodiscard]] inline auto session_policies_error(std::optional<std::string_view> policy,
                                                 const std::vector<std::string>& arns,
                                                 std::string_view account)
    -> std::optional<std::pair<std::string, std::string>> {
    std::size_t total = 0;
    if (policy) {
        const auto cps = detail::decode_utf8(*policy);
        if (!cps || cps->empty() || cps->size() > 2048 ||
            !std::ranges::all_of(*cps, detail::policy_char)) {
            return std::pair{std::string("MalformedPolicyDocument"),
                             std::string("Policy must be 1-2048 characters of tab, LF, CR and "
                                         "U+0020-U+00FF")};
        }
        total += detail::non_whitespace_chars(*cps);
    }
    if (arns.size() > 10) {
        return std::pair{std::string("InvalidInput"),
                         std::string("At most 10 PolicyArns may be passed")};
    }
    for (const auto& a : arns) {
        // arn:<partition>:iam::<account>:policy/...
        const auto own = ":iam::" + std::string(account) + ":policy/";
        const bool in_account = a.starts_with("arn:") && a.find(own) != std::string::npos;
        const bool aws_managed =
            a.starts_with("arn:") && a.find(":iam::aws:policy/") != std::string::npos;
        if (!arn_length(a) || (!in_account && !aws_managed)) {
            return std::pair{
                std::string("InvalidInput"),
                "Policy " + a + " is not a managed policy in account " + std::string(account)};
        }
        total += a.size();
    }
    if (total > 2048) {
        return std::pair{std::string("PackedPolicyTooLarge"),
                         std::string("Session policies exceed 2048 characters")};
    }
    return std::nullopt;
}

/// INV-STAG-1..4: why a request's session tags are refused, or nothing. At
/// most 50 tags with valid keys and values, keys unique case-insensitively;
/// at most 50 TransitiveTagKeys of 1–128 characters; and no key that the
/// calling session already carries as a transitive tag (`inherited`), which
/// cannot be overridden down a chain.
[[nodiscard]] inline auto session_tags_error(const tag_list& tags,
                                             const std::vector<std::string>& transitive_keys,
                                             const tag_list& inherited)
    -> std::optional<std::string> {
    if (auto e = tags_error(tags, tag_keys::case_insensitive)) {
        return e;
    }
    if (transitive_keys.size() > 50) {
        return "At most 50 TransitiveTagKeys may be passed";
    }
    for (const auto& k : transitive_keys) {
        if (k.empty() || k.size() > 128) {
            return "TransitiveTagKeys must be 1-128 characters";
        }
    }
    for (const auto& [k, v] : tags) {
        const auto f = detail::fold(k);
        for (const auto& [ik, iv] : inherited) {
            if (detail::fold(ik) == f) {
                return "Tag " + k + " is a transitive tag of the calling session";
            }
        }
    }
    return std::nullopt;
}

/// INV-STAG-5: the session's tags: the principal's tags overridden, key by
/// key and case-insensitively, by the request's session tags (whose
/// spelling is kept), then the calling session's transitive tags.
[[nodiscard]] inline auto merge_session_tags(const tag_list& principal_tags,
                                             const tag_list& session_tags,
                                             const tag_list& inherited) -> tag_list {
    tag_list out;
    const auto put = [&](const std::string& k, const std::string& v) {
        const auto f = detail::fold(k);
        std::erase_if(out, [&](const auto& kv) { return detail::fold(kv.first) == f; });
        out.emplace_back(k, v);
    };
    for (const auto& [k, v] : principal_tags) {
        put(k, v);
    }
    for (const auto& [k, v] : session_tags) {
        put(k, v);
    }
    for (const auto& [k, v] : inherited) {
        put(k, v);
    }
    return out;
}

// ── What each kind of credential may call ───────────────────────────────────

/// The kind of credential that signed a request.
enum class credential_kind : std::uint8_t {
    root,            // the account root's access key
    user,            // an IAM user's access key
    assumed_role,    // AssumeRole / AssumeRoleWithWebIdentity
    session_token,   // GetSessionToken
    federated_user,  // GetFederationToken
};

/// INV-CRED-1..4: whether a credential may call an STS action.
/// GetSessionToken credentials may call only AssumeRole and
/// GetCallerIdentity; GetFederationToken credentials only
/// GetCallerIdentity; role credentials anything but GetFederationToken and
/// GetSessionToken; long-term keys anything. AssumeRoleWithWebIdentity is
/// unsigned, so any caller may make it.
[[nodiscard]] inline auto sts_action_allowed(credential_kind k, std::string_view action) -> bool {
    if (action == "AssumeRoleWithWebIdentity" || action == "GetCallerIdentity") {
        return true;
    }
    switch (k) {
        case credential_kind::root:
        case credential_kind::user:
            return true;
        case credential_kind::assumed_role:
            return action != "GetFederationToken" && action != "GetSessionToken";
        case credential_kind::session_token:
            return action == "AssumeRole";
        case credential_kind::federated_user:
            return false;
    }
    return false;
}

/// INV-CRED-5, INV-CRED-6: whether a credential may call the IAM API at
/// all. GetSessionToken credentials only with MFA in the session;
/// GetFederationToken credentials never.
[[nodiscard]] inline auto iam_api_allowed(credential_kind k, bool mfa_authenticated) -> bool {
    switch (k) {
        case credential_kind::session_token:
            return mfa_authenticated;
        case credential_kind::federated_user:
            return false;
        default:
            return true;
    }
}

}  // namespace kythira::iam::validate
