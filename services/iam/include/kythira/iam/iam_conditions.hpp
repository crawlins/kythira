// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_conditions.hpp
/// @brief Condition operators, policy variables and the request context
///        (`.kiro/specs/iam-compatible-auth/` task 11, Requirements
///        5.4-5.6, invariants INV-COND-* and INV-VAR-*).
///
/// What `evaluate()` (task 12) asks of a statement beyond its action:
///
///     context_keys ctx = global_condition_keys(facts);
///     bool hit = conditions_match(st, ctx, doc.supports_variables()) &&
///                resource_matches(*st.resource, arn, ctx, doc.supports_variables());
///
/// Semantics, from the IAM User Guide pages "Condition operators", "Single-
/// valued vs. multivalued context keys" and "Policy variables":
///
///   - Operator blocks, and the keys within a block, are ANDed; the values
///     of one key are ORed.
///   - A positive operator is true when some context value matches some
///     policy value. A negated operator is its complement: true when no
///     context value matches any policy value (NOR over the values).
///   - A missing key makes a positive operator false and a negated one
///     true; `...IfExists` makes either true. `Null: true` holds exactly
///     when the key is missing, `Null: false` when it is present.
///   - `ForAllValues:` is true when every context value matches some policy
///     value (or, negated, matches none), and true for a missing or empty
///     key. `ForAnyValue:` is true when at least one context value matches
///     (or, negated, matches none), and false for a missing or empty key.
///   - Without a set prefix, a multivalued key behaves as `ForAnyValue:`
///     for a positive operator, and as the complement of that for a
///     negated one. AWS tells authors to use a prefix with such keys and
///     documents no other behaviour.
///   - A value that cannot be read as the operator's type (a word given to
///     a Numeric operator, a bad CIDR) matches nothing.
///
/// Variables (`${aws:username}`, `${aws:PrincipalTag/team, 'none'}`,
/// `${*}`, `${?}`, `${$}`) are substituted only under Version 2012-10-17,
/// only in Resource/NotResource and in string, Bool and ARN condition
/// values. A variable with no value and no default makes that one value or
/// pattern match nothing, so a positive operator on it is false and a
/// negated one true, and a resource pattern holding it matches no resource.
/// A substituted value is always literal: a `*` in a tag value does not
/// become a wildcard.
///
/// Key names are case-insensitive. The part after the `/` in a tag key
/// (`aws:RequestTag/Environment`) is matched exactly, except for
/// `aws:PrincipalTag/...`, because tag keys on users and roles (the only
/// principals with tags) are themselves case-insensitive (INV-TAG-5).

#include <kythira/iam/iam_policy.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>

namespace kythira::iam {

// ── The request context ─────────────────────────────────────────────────────

/// The condition keys of one request. A key is single-valued or
/// multivalued (`aws:TagKeys`); a multivalued key may be present with no
/// values, which `ForAllValues:` and `ForAnyValue:` tell apart from absent
/// only through `Null`.
class context_keys {
public:
    /// The form a key is stored and looked up under.
    [[nodiscard]] static auto normalize(std::string_view key) -> std::string {
        const auto slash = key.find('/');
        if (slash == std::string_view::npos) {
            return detail::ascii_lower(key);
        }
        auto head = detail::ascii_lower(key.substr(0, slash));
        if (head == "aws:principaltag") {
            return detail::ascii_lower(key);
        }
        return head + std::string(key.substr(slash));
    }

    void set(std::string_view key, std::string value) {
        _keys[normalize(key)] = entry{{std::move(value)}, false};
    }
    void set_multi(std::string_view key, std::vector<std::string> values) {
        _keys[normalize(key)] = entry{std::move(values), true};
    }
    void erase(std::string_view key) { _keys.erase(normalize(key)); }

    /// The key's values, or nullptr when the request does not carry it.
    [[nodiscard]] auto find(std::string_view key) const -> const std::vector<std::string>* {
        const auto it = _keys.find(normalize(key));
        return it == _keys.end() ? nullptr : &it->second.values;
    }

    /// The key's one value, or nothing when it is absent or multivalued
    /// (multivalued keys cannot be policy variables).
    [[nodiscard]] auto single(std::string_view key) const -> std::optional<std::string> {
        const auto it = _keys.find(normalize(key));
        if (it == _keys.end() || it->second.multi || it->second.values.size() != 1) {
            return std::nullopt;
        }
        return it->second.values.front();
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t { return _keys.size(); }

    /// Calls `f(key, values)` for every key, in key order.
    template<typename F> void for_each(F&& f) const {
        for (const auto& [k, e] : _keys) {
            f(k, e.values);
        }
    }

private:
    struct entry {
        std::vector<std::string> values;
        bool multi = false;
    };
    std::map<std::string, entry> _keys;
};

// ── Policy variables ────────────────────────────────────────────────────────

/// How a substituted value is used: as a plain string (StringEquals), or as
/// a `*`/`?` pattern (StringLike, Arn*, Resource), where every literal `*`,
/// `?` and `pattern_escape` is marked with `pattern_escape`.
enum class substitution : std::uint8_t {
    plain,
    pattern
};

namespace detail {

inline void append_literal(std::string& out, std::string_view text, substitution mode) {
    for (char c : text) {
        if (mode == substitution::pattern && (c == '*' || c == '?' || c == pattern_escape)) {
            out.push_back(pattern_escape);
        }
        out.push_back(c);
    }
}

inline void append_written(std::string& out, std::string_view text, substitution mode) {
    for (char c : text) {
        if (mode == substitution::pattern && c == pattern_escape) {
            out.push_back(pattern_escape);  // cannot occur in policy text; be safe
        }
        out.push_back(c);
    }
}

[[nodiscard]] inline auto trim(std::string_view s) -> std::string_view {
    while (!s.empty() && s.front() == ' ') {
        s.remove_prefix(1);
    }
    while (!s.empty() && s.back() == ' ') {
        s.remove_suffix(1);
    }
    return s;
}

}  // namespace detail

/// `text` with its policy variables replaced from `ctx`, or nothing when a
/// variable has no value and no default. In `pattern` mode the result is
/// for `glob(..., escapes = true)` or `arn_pattern(..., true)`. A `${` with
/// no closing `}` is literal text.
[[nodiscard]] inline auto substitute(std::string_view text, const context_keys& ctx,
                                     substitution mode) -> std::optional<std::string> {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto open = text.find("${", i);
        if (open == std::string_view::npos) {
            detail::append_written(out, text.substr(i), mode);
            break;
        }
        const auto close = text.find('}', open + 2);
        if (close == std::string_view::npos) {
            detail::append_written(out, text.substr(i), mode);
            break;
        }
        detail::append_written(out, text.substr(i, open - i), mode);
        const auto inner = text.substr(open + 2, close - open - 2);
        if (inner == "*" || inner == "?" || inner == "$") {
            detail::append_literal(out, inner, mode);
        } else {
            std::string_view key = inner;
            std::optional<std::string_view> fallback;
            if (const auto comma = inner.find(','); comma != std::string_view::npos) {
                key = inner.substr(0, comma);
                const auto d = detail::trim(inner.substr(comma + 1));
                if (d.size() >= 2 && d.front() == '\'' && d.back() == '\'') {
                    fallback = d.substr(1, d.size() - 2);
                } else {
                    return std::nullopt;  // a malformed default resolves to nothing
                }
            }
            if (auto v = ctx.single(detail::trim(key))) {
                detail::append_literal(out, *v, mode);
            } else if (fallback) {
                detail::append_literal(out, *fallback, mode);
            } else {
                return std::nullopt;
            }
        }
        i = close + 1;
    }
    return out;
}

// ── Typed values ────────────────────────────────────────────────────────────

namespace detail {

/// An integer or decimal, or nothing.
[[nodiscard]] inline auto parse_number(std::string_view s) -> std::optional<long double> {
    if (s.empty() || s.size() > 64) {
        return std::nullopt;
    }
    std::size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    bool digits = false;
    bool dot = false;
    for (std::size_t k = i; k < s.size(); ++k) {
        if (s[k] >= '0' && s[k] <= '9') {
            digits = true;
        } else if (s[k] == '.' && !dot) {
            dot = true;
        } else {
            return std::nullopt;
        }
    }
    if (!digits) {
        return std::nullopt;
    }
    const std::string copy(s);
    return std::strtold(copy.c_str(), nullptr);
}

[[nodiscard]] inline auto digits_at(std::string_view s, std::size_t pos, std::size_t n)
    -> std::optional<int> {
    if (pos + n > s.size()) {
        return std::nullopt;
    }
    int v = 0;
    for (std::size_t k = pos; k < pos + n; ++k) {
        if (s[k] < '0' || s[k] > '9') {
            return std::nullopt;
        }
        v = v * 10 + (s[k] - '0');
    }
    return v;
}

/// A date as milliseconds since the epoch: epoch seconds (an integer or
/// decimal), or one of the W3C profiles of ISO 8601 that AWS accepts:
/// `YYYY-MM-DD`, `YYYY-MM-DDThh:mmTZD`, `YYYY-MM-DDThh:mm:ssTZD` and
/// `YYYY-MM-DDThh:mm:ss.sTZD`, where TZD is `Z` or `±hh:mm`.
[[nodiscard]] inline auto parse_date(std::string_view s) -> std::optional<long double> {
    if (s.find('-', 1) == std::string_view::npos) {
        if (auto n = parse_number(s)) {
            return *n * 1000;
        }
        return std::nullopt;
    }
    const auto y = digits_at(s, 0, 4);
    const auto mo = digits_at(s, 5, 2);
    const auto d = digits_at(s, 8, 2);
    if (!y || !mo || !d || s.size() < 10 || s[4] != '-' || s[7] != '-') {
        return std::nullopt;
    }
    const std::chrono::year_month_day ymd{std::chrono::year{*y},
                                          std::chrono::month{static_cast<unsigned>(*mo)},
                                          std::chrono::day{static_cast<unsigned>(*d)}};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    long double ms =
        static_cast<long double>(std::chrono::sys_days{ymd}.time_since_epoch().count()) * 86400000;
    if (s.size() == 10) {
        return ms;
    }
    if (s[10] != 'T') {
        return std::nullopt;
    }
    const auto hh = digits_at(s, 11, 2);
    const auto mm = digits_at(s, 14, 2);
    if (!hh || !mm || s.size() < 16 || s[13] != ':' || *hh > 23 || *mm > 59) {
        return std::nullopt;
    }
    ms += (static_cast<long double>(*hh) * 3600 + static_cast<long double>(*mm) * 60) * 1000;
    std::size_t pos = 16;
    if (pos < s.size() && s[pos] == ':') {
        const auto ss = digits_at(s, pos + 1, 2);
        if (!ss || *ss > 60) {
            return std::nullopt;
        }
        ms += static_cast<long double>(*ss) * 1000;
        pos += 3;
        if (pos < s.size() && s[pos] == '.') {
            const auto start = ++pos;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
                ++pos;
            }
            if (pos == start) {
                return std::nullopt;
            }
            const auto frac = parse_number("0." + std::string(s.substr(start, pos - start)));
            ms += *frac * 1000;
        }
    }
    if (pos >= s.size()) {
        return std::nullopt;  // a time needs a zone designator
    }
    if (s[pos] == 'Z' && pos + 1 == s.size()) {
        return ms;
    }
    if ((s[pos] == '+' || s[pos] == '-') && pos + 6 == s.size() && s[pos + 3] == ':') {
        const auto oh = digits_at(s, pos + 1, 2);
        const auto om = digits_at(s, pos + 4, 2);
        if (!oh || !om || *oh > 23 || *om > 59) {
            return std::nullopt;
        }
        const long double offset =
            (static_cast<long double>(*oh) * 3600 + static_cast<long double>(*om) * 60) * 1000;
        return s[pos] == '+' ? ms - offset : ms + offset;
    }
    return std::nullopt;
}

[[nodiscard]] inline auto parse_bool(std::string_view s) -> std::optional<bool> {
    const auto l = ascii_lower(s);
    if (l == "true") {
        return true;
    }
    if (l == "false") {
        return false;
    }
    return std::nullopt;
}

/// Standard base64, padding required, or nothing.
[[nodiscard]] inline auto base64_decode(std::string_view s) -> std::optional<std::string> {
    if (s.size() % 4 != 0) {
        return std::nullopt;
    }
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < s.size(); i += 4) {
        std::array<int, 4> v{};
        std::size_t pad = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            if (s[i + k] == '=' && i + 4 == s.size() && k >= 2) {
                v[k] = 0;
                ++pad;
                continue;
            }
            v[k] = val(s[i + k]);
            if (pad != 0 || v[k] < 0) {
                return std::nullopt;  // a digit after padding, or not base64
            }
        }
        const auto n = (static_cast<unsigned>(v[0]) << 18U) | (static_cast<unsigned>(v[1]) << 12U) |
                       (static_cast<unsigned>(v[2]) << 6U) | static_cast<unsigned>(v[3]);
        out.push_back(static_cast<char>((n >> 16U) & 0xFFU));
        if (pad < 2) {
            out.push_back(static_cast<char>((n >> 8U) & 0xFFU));
        }
        if (pad < 1) {
            out.push_back(static_cast<char>(n & 0xFFU));
        }
    }
    return out;
}

/// An IPv4 or IPv6 address as 16 bytes plus its family, or nothing.
struct ip_address {
    std::array<unsigned char, 16> bytes{};
    bool v6 = false;
};

[[nodiscard]] inline auto parse_ip(std::string_view s) -> std::optional<ip_address> {
    const std::string copy(s);
    ip_address a;
    if (inet_pton(AF_INET, copy.c_str(), a.bytes.data()) == 1) {
        return a;
    }
    if (inet_pton(AF_INET6, copy.c_str(), a.bytes.data()) == 1) {
        a.v6 = true;
        return a;
    }
    return std::nullopt;
}

/// Whether `addr` is inside `cidr` (`203.0.113.0/24`, `2001:db8::/32`, or a
/// bare address, which is a /32 or /128). Families never mix.
[[nodiscard]] inline auto in_cidr(std::string_view cidr, std::string_view addr) -> bool {
    const auto slash = cidr.find('/');
    const auto net = parse_ip(cidr.substr(0, slash));
    const auto ip = parse_ip(addr);
    if (!net || !ip || net->v6 != ip->v6) {
        return false;
    }
    const int max_bits = net->v6 ? 128 : 32;
    int bits = max_bits;
    if (slash != std::string_view::npos) {
        const auto n = parse_number(cidr.substr(slash + 1));
        if (!n || cidr.substr(slash + 1).find('.') != std::string_view::npos || *n < 0 ||
            *n > max_bits) {
            return false;
        }
        bits = static_cast<int>(*n);
    }
    for (int i = 0; i < bits; ++i) {
        const auto byte = static_cast<std::size_t>(i / 8);
        const auto mask = static_cast<unsigned char>(0x80U >> static_cast<unsigned>(i % 8));
        if ((net->bytes.at(byte) & mask) != (ip->bytes.at(byte) & mask)) {
            return false;
        }
    }
    return true;
}

template<typename T>
[[nodiscard]] auto compare(const T& context, const T& policy, condition_operator::comparison c)
    -> bool {
    switch (c) {
        case condition_operator::comparison::eq:
            return context == policy;
        case condition_operator::comparison::lt:
            return context < policy;
        case condition_operator::comparison::le:
            return context <= policy;
        case condition_operator::comparison::gt:
            return context > policy;
        case condition_operator::comparison::ge:
            return context >= policy;
    }
    return false;
}

/// Whether one context value matches one policy value under the positive
/// form of `op` (negation is applied by the caller, over all values). An
/// unresolvable variable in the policy value matches nothing.
[[nodiscard]] inline auto value_matches(const condition_operator& op, std::string_view context,
                                        std::string_view policy, const context_keys& ctx,
                                        bool variables) -> bool {
    using fam = condition_operator::family;
    const bool subst = variables && op.takes_variables();
    switch (op.fam) {
        case fam::string:
        case fam::string_ignore_case: {
            std::string p(policy);
            if (subst) {
                auto s = substitute(policy, ctx, substitution::plain);
                if (!s) {
                    return false;
                }
                p = std::move(*s);
            }
            return op.fam == fam::string ? context == p : ascii_lower(context) == ascii_lower(p);
        }
        case fam::string_like: {
            if (!subst) {
                return glob(policy).matches(context);
            }
            auto s = substitute(policy, ctx, substitution::pattern);
            return s && glob(*s, false, true).matches(context);
        }
        case fam::arn: {
            if (!split_arn(context)) {
                return false;
            }
            if (!subst) {
                return arn_pattern(policy).matches_literally(context);
            }
            auto s = substitute(policy, ctx, substitution::pattern);
            return s && arn_pattern(*s, true).matches(context);
        }
        case fam::boolean: {
            std::string p(policy);
            if (subst) {
                auto s = substitute(policy, ctx, substitution::plain);
                if (!s) {
                    return false;
                }
                p = std::move(*s);
            }
            const auto a = parse_bool(context);
            const auto b = parse_bool(p);
            return a && b && *a == *b;
        }
        case fam::numeric: {
            const auto a = parse_number(context);
            const auto b = parse_number(policy);
            return a && b && compare(*a, *b, op.cmp);
        }
        case fam::date: {
            const auto a = parse_date(context);
            const auto b = parse_date(policy);
            return a && b && compare(*a, *b, op.cmp);
        }
        case fam::binary: {
            const auto b = base64_decode(policy);
            return b && context == *b;
        }
        case fam::ip:
            return in_cidr(policy, context);
        case fam::null:
            break;  // handled by the caller
    }
    return false;
}

}  // namespace detail

// ── Conditions ──────────────────────────────────────────────────────────────

/// Whether one condition entry (one operator, one key, its values) holds.
[[nodiscard]] inline auto condition_matches(const condition_entry& e, const context_keys& ctx,
                                            bool variables) -> bool {
    using fam = condition_operator::family;
    using set = condition_operator::set_prefix;
    const auto& op = e.oper;
    const auto* values = ctx.find(e.key);

    if (op.fam == fam::null) {
        const bool absent = values == nullptr;
        return std::ranges::any_of(e.values, [&](const std::string& v) {
            const auto want = detail::parse_bool(v);
            return want && *want == absent;
        });
    }

    // A context value matches when it matches any policy value.
    const auto hit = [&](const std::string& context) {
        return std::ranges::any_of(e.values, [&](const std::string& p) {
            return detail::value_matches(op, context, p, ctx, variables);
        });
    };

    if (values == nullptr || (values->empty() && op.set != set::none)) {
        if (op.if_exists) {
            return true;
        }
        switch (op.set) {
            case set::for_all_values:
                return true;
            case set::for_any_value:
                return false;
            case set::none:
                return op.negated;
        }
    }

    switch (op.set) {
        case set::for_all_values:
            // Every context value matches some policy value, or, negated,
            // matches none.
            return std::ranges::all_of(
                *values, [&](const std::string& v) { return op.negated ? !hit(v) : hit(v); });
        case set::for_any_value:
            return std::ranges::any_of(
                *values, [&](const std::string& v) { return op.negated ? !hit(v) : hit(v); });
        case set::none:
            break;
    }
    const bool any = std::ranges::any_of(*values, hit);
    return op.negated ? !any : any;
}

/// Whether every condition of `st` holds (an empty Condition always does).
[[nodiscard]] inline auto conditions_match(const statement& st, const context_keys& ctx,
                                           bool variables) -> bool {
    return std::ranges::all_of(st.conditions, [&](const condition_entry& e) {
        return condition_matches(e, ctx, variables);
    });
}

/// Whether `arn` matches any of the matcher's patterns, substituting policy
/// variables when `variables` (the document is Version 2012-10-17). A
/// pattern whose variable has no value matches no resource.
[[nodiscard]] inline auto resource_matches(const resource_matcher& m, std::string_view arn,
                                           const context_keys& ctx, bool variables) -> bool {
    return std::ranges::any_of(m.patterns(), [&](const arn_pattern& p) {
        if (!p.has_variables()) {
            return p.matches(arn);
        }
        if (!variables) {
            return p.matches_literally(arn);
        }
        const auto s = substitute(p.source(), ctx, substitution::pattern);
        return s && arn_pattern(*s, true).matches(arn);
    });
}

// ── Global condition keys ───────────────────────────────────────────────────

/// What the front end and the authorizer know about a request, for the AWS
/// global condition keys of Requirement 5.6. An empty optional (or empty
/// string) leaves its key absent: keys Kythira cannot provide are never
/// invented.
struct request_facts {
    std::chrono::system_clock::time_point now{};
    bool secure_transport = false;
    std::string source_ip;
    std::string user_agent;

    std::string principal_arn;
    std::string principal_account;
    std::string principal_type;  // Account, User, AssumedRole, FederatedUser, ...
    std::vector<std::pair<std::string, std::string>> principal_tags;
    std::string userid;
    std::string username;  // IAM users only

    std::optional<std::vector<std::pair<std::string, std::string>>> request_tags;
    std::vector<std::pair<std::string, std::string>> resource_tags;
    std::string resource_account;

    std::optional<std::chrono::system_clock::time_point> token_issue_time;  // temporary credentials
    std::string source_identity;
    std::string requested_region;
    std::vector<std::string> called_via;  // services, first to last
};

namespace detail {

[[nodiscard]] inline auto iso8601(std::chrono::system_clock::time_point t) -> std::string {
    const auto secs = std::chrono::floor<std::chrono::seconds>(t);
    const auto days = std::chrono::floor<std::chrono::days>(secs);
    const std::chrono::year_month_day ymd{days};
    const std::chrono::hh_mm_ss hms{secs - days};
    std::array<char, 32> buf{};
    const auto n = std::snprintf(
        buf.data(), buf.size(), "%04d-%02u-%02uT%02d:%02d:%02dZ", static_cast<int>(ymd.year()),
        static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
        static_cast<int>(hms.hours().count()), static_cast<int>(hms.minutes().count()),
        static_cast<int>(hms.seconds().count()));
    return {buf.data(), static_cast<std::size_t>(n)};
}

}  // namespace detail

/// The AWS global condition keys for a request (Requirement 5.6). Service
/// keys are added by each service on top.
[[nodiscard]] inline auto global_condition_keys(const request_facts& f) -> context_keys {
    context_keys k;
    k.set("aws:CurrentTime", detail::iso8601(f.now));
    k.set("aws:EpochTime",
          std::to_string(
              std::chrono::duration_cast<std::chrono::seconds>(f.now.time_since_epoch()).count()));
    k.set("aws:SecureTransport", f.secure_transport ? "true" : "false");
    // Kythira has no MFA, so the key is always false (Requirement 5.6),
    // as AWS reports for credentials obtained without MFA.
    k.set("aws:MultiFactorAuthPresent", "false");
    const auto put = [&](std::string_view key, const std::string& v) {
        if (!v.empty()) {
            k.set(key, v);
        }
    };
    put("aws:SourceIp", f.source_ip);
    put("aws:UserAgent", f.user_agent);
    put("aws:PrincipalArn", f.principal_arn);
    put("aws:PrincipalAccount", f.principal_account);
    put("aws:PrincipalType", f.principal_type);
    put("aws:userid", f.userid);
    put("aws:username", f.username);
    put("aws:ResourceAccount", f.resource_account);
    put("aws:SourceIdentity", f.source_identity);
    put("aws:RequestedRegion", f.requested_region);
    for (const auto& [key, v] : f.principal_tags) {
        k.set("aws:PrincipalTag/" + key, v);
    }
    for (const auto& [key, v] : f.resource_tags) {
        k.set("aws:ResourceTag/" + key, v);
    }
    if (f.request_tags) {
        std::vector<std::string> keys;
        for (const auto& [key, v] : *f.request_tags) {
            k.set("aws:RequestTag/" + key, v);
            keys.push_back(key);
        }
        k.set_multi("aws:TagKeys", std::move(keys));
    }
    if (f.token_issue_time) {
        k.set("aws:TokenIssueTime", detail::iso8601(*f.token_issue_time));
    }
    if (!f.called_via.empty()) {
        k.set_multi("aws:CalledVia", f.called_via);
        k.set("aws:CalledViaFirst", f.called_via.front());
        k.set("aws:CalledViaLast", f.called_via.back());
    }
    return k;
}

}  // namespace kythira::iam
