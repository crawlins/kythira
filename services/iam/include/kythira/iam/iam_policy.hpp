// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The IAM policy language: parser, validator and matchers
// (`.kiro/specs/iam-compatible-auth/` Requirement 5.1-5.3, design
// Component 5, task 10).
//
// A policy document goes in as the JSON an administrator would hand AWS and
// comes out as a `policy_document`: an AST of statements, each with its
// effect, principal set, compiled action and resource matchers, and its
// condition entries. Anything AWS would refuse is refused here with a
// `policy_error` whose `code()` is `MalformedPolicyDocument` (or
// `LimitExceeded` for the size quota), and whose message names the problem in
// AWS's own words where AWS has them, so a client that greps for them still
// works.
//
//     auto doc = kythira::iam::parse_policy(json, kythira::iam::policy_type::identity);
//     for (const auto& st : doc.statements) {
//         bool hit = st.action.matches("s3:GetObject") &&
//                    st.resource->matches("arn:aws:s3:::bucket/key");
//         ...
//     }
//
// What lives where:
//   - parsing, the AST, per-type validation and size limits: here (task 10);
//   - the condition operator table, policy variables and the request
//     context: task 11. Until then condition entries are kept as parsed
//     (operator, key, values as strings), with only their shape checked;
//   - `evaluate()`: task 12.
//
// Matching follows AWS:
//   - `*` matches any run of characters (including none) and `?` exactly one
//     character, both counted in UTF-8 code points. Nothing else is special:
//     no regex, no character classes, no escapes.
//   - Actions match case-insensitively (`s3:getobject` == `S3:GetObject`).
//     Patterns are lower-cased once at parse time.
//   - Resources match case-sensitively. A pattern with the ARN shape
//     (`arn:partition:service:region:account:resource`, at least five
//     colons) matches segment by segment, so `?` and an inner `*` stay
//     inside their segment. AWS lets a `*` that ends a segment "expand to
//     match beyond the colon boundaries" (IAM User Guide, "IAM JSON policy
//     elements: Resource"), so from such a segment on the rest of the
//     pattern matches the rest of the ARN as one string. The resource
//     segment, the sixth, is everything after the fifth colon and may itself
//     contain colons and slashes. Any other pattern (`*`, `arn:aws:s3:*`)
//     matches the whole string.
//
// Policy variables (`${aws:username}`) are left in the stored pattern for
// task 11 to substitute at evaluation time; a resource pattern that holds
// one reports `has_variables()` and is not precompiled.

#include <boost/json.hpp>
#include <boost/json/basic_parser_impl.hpp>  // basic_parser's template bodies

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::iam {

// ── Errors ──────────────────────────────────────────────────────────────────

// Thrown for any document AWS would refuse. `code()` is the AWS error code
// the Query layer puts in the response.
class policy_error : public std::runtime_error {
public:
    static constexpr std::string_view malformed = "MalformedPolicyDocument";
    static constexpr std::string_view limit_exceeded = "LimitExceeded";

    explicit policy_error(const std::string& message, std::string_view code = malformed)
        : std::runtime_error(message), _code(code) {}

    [[nodiscard]] auto code() const noexcept -> std::string_view { return _code; }

private:
    std::string_view _code;  // always one of the constants above
};

// ── Policy types ────────────────────────────────────────────────────────────

// Where a document is attached decides which elements it may hold.
//   identity  attached to a user, group or role (managed or inline)
//   trust     a role's AssumeRolePolicyDocument
//   resource  attached to a resource (a stream, a table, a bucket)
//   boundary  a permissions boundary (a managed policy used as a ceiling)
//   session   passed to AssumeRole* as Policy
enum class policy_type : std::uint8_t {
    identity,
    trust,
    resource,
    boundary,
    session
};

[[nodiscard]] constexpr auto to_string(policy_type t) noexcept -> std::string_view {
    switch (t) {
        case policy_type::identity:
            return "identity";
        case policy_type::trust:
            return "trust";
        case policy_type::resource:
            return "resource";
        case policy_type::boundary:
            return "boundary";
        case policy_type::session:
            return "session";
    }
    return "unknown";
}

// AWS's per-document size quotas (Requirement 4.4), counted the way IAM
// counts them: whitespace outside string literals is free. A managed policy
// (which is what identity and boundary documents are when they stand alone)
// is 6144; a role trust policy and a session policy 2048. The aggregate
// inline quotas (2048 per user, 5120 per group, 10240 per role) are summed
// across an entity's policies, so `iam_state_machine` enforces them, not the
// parser. AWS sets no IAM quota for resource policies; 20480 is S3's bucket
// policy limit, the largest a Kythira front end is likely to want.
[[nodiscard]] constexpr auto default_max_size(policy_type t) noexcept -> std::size_t {
    switch (t) {
        case policy_type::identity:
        case policy_type::boundary:
            return 6144;
        case policy_type::trust:
        case policy_type::session:
            return 2048;
        case policy_type::resource:
            return 20480;
    }
    return 6144;
}

struct parse_options {
    // 0 means `default_max_size(type)`. Quotas are configurable upward
    // (Requirement 4.4); the state machine passes its configured value.
    std::size_t max_size = 0;
};

// ── Glob matching ───────────────────────────────────────────────────────────

namespace detail {

// Length of the UTF-8 sequence starting with `lead`. A stray continuation
// byte counts as one, so malformed input still terminates.
[[nodiscard]] constexpr auto utf8_length(unsigned char lead) noexcept -> std::size_t {
    if (lead < 0xC0) {
        return 1;
    }
    if (lead < 0xE0) {
        return 2;
    }
    if (lead < 0xF0) {
        return 3;
    }
    return 4;
}

[[nodiscard]] constexpr auto ascii_lower(char c) noexcept -> char {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] inline auto ascii_lower(std::string_view s) -> std::string {
    std::string out(s);
    for (auto& c : out) {
        c = ascii_lower(c);
    }
    return out;
}

}  // namespace detail

// A compiled `*`/`?` pattern. The program is a list of steps: a literal run,
// "one code point", or "any run". Consecutive stars collapse into one.
// Matching walks the steps once, backtracking only to the most recent star,
// which is linear in practice and O(pattern × input) at worst, with no
// recursion and no allocation.
class glob {
public:
    glob() = default;

    // `fold_case` lower-cases the pattern now and each input at match time
    // (ASCII only: IAM action names are ASCII).
    explicit glob(std::string_view pattern, bool fold_case = false)
        : _source(pattern), _fold(fold_case) {
        std::string lit;
        auto flush = [&] {
            if (!lit.empty()) {
                _steps.push_back({step_kind::literal, std::move(lit)});
                lit.clear();
            }
        };
        for (char c : pattern) {
            if (c == '*') {
                flush();
                if (_steps.empty() || _steps.back().kind != step_kind::star) {
                    _steps.push_back({step_kind::star, {}});
                }
            } else if (c == '?') {
                flush();
                _steps.push_back({step_kind::one, {}});
            } else {
                lit.push_back(fold_case ? detail::ascii_lower(c) : c);
            }
        }
        flush();
    }

    [[nodiscard]] auto matches(std::string_view input) const -> bool {
        if (matches_everything()) {
            return true;
        }
        std::string folded;
        if (_fold) {
            folded = detail::ascii_lower(input);
            input = folded;
        }
        std::size_t step = 0;
        std::size_t pos = 0;
        // Where to resume after a failed step: the step after the last star,
        // and the input position that star currently stops at.
        std::optional<std::size_t> star_step;
        std::size_t star_pos = 0;
        while (true) {
            bool advanced = false;
            if (step < _steps.size()) {
                const auto& s = _steps[step];
                switch (s.kind) {
                    case step_kind::star:
                        star_step = step + 1;
                        star_pos = pos;
                        ++step;
                        advanced = true;
                        break;
                    case step_kind::one:
                        if (pos < input.size()) {
                            pos += std::min(
                                detail::utf8_length(static_cast<unsigned char>(input[pos])),
                                input.size() - pos);
                            ++step;
                            advanced = true;
                        }
                        break;
                    case step_kind::literal:
                        if (input.substr(pos).starts_with(s.text)) {
                            pos += s.text.size();
                            ++step;
                            advanced = true;
                        }
                        break;
                }
            } else if (pos == input.size()) {
                return true;
            }
            if (advanced) {
                continue;
            }
            // Mismatch: let the last star swallow one more code point.
            if (!star_step || star_pos >= input.size()) {
                return false;
            }
            star_pos += std::min(detail::utf8_length(static_cast<unsigned char>(input[star_pos])),
                                 input.size() - star_pos);
            pos = star_pos;
            step = *star_step;
        }
    }

    [[nodiscard]] auto source() const noexcept -> const std::string& { return _source; }
    [[nodiscard]] auto matches_everything() const noexcept -> bool {
        return _steps.size() == 1 && _steps.front().kind == step_kind::star;
    }

private:
    enum class step_kind : std::uint8_t {
        literal,
        one,
        star
    };
    struct step {
        step_kind kind;
        std::string text;  // literal only
    };

    std::string _source;
    std::vector<step> _steps;
    bool _fold = false;
};

// ── ARNs ────────────────────────────────────────────────────────────────────

// The six fields of an ARN: "arn", partition, service, region, account,
// resource. The resource is everything after the fifth colon.
using arn_fields = std::array<std::string_view, 6>;

// Splits `s` if it has the ARN shape: it starts with "arn:" and has at least
// five colons. Empty fields are allowed (`arn:aws:s3:::bucket`).
[[nodiscard]] inline auto split_arn(std::string_view s) -> std::optional<arn_fields> {
    if (!s.starts_with("arn:")) {
        return std::nullopt;
    }
    arn_fields out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        const auto colon = s.find(':', start);
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        out[i] = s.substr(start, colon - start);
        start = colon + 1;
    }
    out[5] = s.substr(start);
    return out;
}

// One resource pattern (an element of `Resource`/`NotResource`, or the value
// of an `Arn*` condition in task 11), compiled for matching.
class arn_pattern {
public:
    arn_pattern() = default;

    explicit arn_pattern(std::string_view pattern)
        : _source(pattern), _has_variables(pattern.find("${") != std::string_view::npos) {
        if (_has_variables) {
            return;  // task 11 substitutes, then compiles
        }
        compile(pattern);
    }

    // True when `arn` matches. A pattern holding a policy variable needs the
    // request context and never matches here.
    [[nodiscard]] auto matches(std::string_view arn) const -> bool {
        if (_has_variables) {
            return false;
        }
        if (_segments.empty()) {
            return _whole.matches(arn);
        }
        const auto value = split_arn(arn);
        if (!value) {
            return false;
        }
        for (std::size_t i = 0; i < _segments.size(); ++i) {
            const auto& seg = _segments[i];
            const auto field = (*value)[i + 1];
            if (seg.tail) {
                // A segment ending in `*`: the rest of the pattern against
                // the rest of the ARN, colons included.
                const auto rest =
                    std::string_view(field.data(), arn.data() + arn.size() - field.data());
                return seg.tail->matches(rest);
            }
            if (!seg.field.matches(field)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] auto source() const noexcept -> const std::string& { return _source; }
    [[nodiscard]] auto has_variables() const noexcept -> bool { return _has_variables; }
    [[nodiscard]] auto is_arn_shaped() const noexcept -> bool { return !_segments.empty(); }

private:
    struct segment {
        glob field;                // this segment alone
        std::optional<glob> tail;  // set when it ends in `*` (not the last segment)
    };

    void compile(std::string_view pattern) {
        const auto f = split_arn(pattern);
        if (!f) {
            _whole = glob(pattern);
            return;
        }
        // The "arn" segment is literal by construction; compile the rest.
        _segments.resize(5);
        for (std::size_t i = 1; i < 6; ++i) {
            auto& seg = _segments[i - 1];
            seg.field = glob((*f)[i]);
            if (i < 5 && (*f)[i].ends_with('*')) {
                const auto rest = std::string_view(
                    (*f)[i].data(), pattern.data() + pattern.size() - (*f)[i].data());
                seg.tail = glob(rest);
            }
        }
    }

    std::string _source;
    bool _has_variables = false;
    std::vector<segment> _segments;  // partition .. resource, when ARN-shaped
    glob _whole;
};

// ── AST ─────────────────────────────────────────────────────────────────────

enum class effect : std::uint8_t {
    allow,
    deny
};

// `Action`/`NotAction`: the patterns, lower-cased and compiled.
class action_matcher {
public:
    action_matcher() = default;
    explicit action_matcher(const std::vector<std::string>& patterns) {
        _globs.reserve(patterns.size());
        for (const auto& p : patterns) {
            _globs.emplace_back(p, /*fold_case=*/true);
        }
    }

    [[nodiscard]] auto matches(std::string_view action) const -> bool {
        return std::ranges::any_of(_globs, [&](const glob& g) { return g.matches(action); });
    }

    [[nodiscard]] auto patterns() const noexcept -> const std::vector<glob>& { return _globs; }

private:
    std::vector<glob> _globs;
};

// `Resource`/`NotResource`.
class resource_matcher {
public:
    resource_matcher() = default;
    explicit resource_matcher(const std::vector<std::string>& patterns) {
        _patterns.reserve(patterns.size());
        for (const auto& p : patterns) {
            _patterns.emplace_back(p);
        }
    }

    [[nodiscard]] auto matches(std::string_view arn) const -> bool {
        return std::ranges::any_of(_patterns, [&](const arn_pattern& p) { return p.matches(arn); });
    }

    [[nodiscard]] auto patterns() const noexcept -> const std::vector<arn_pattern>& {
        return _patterns;
    }

private:
    std::vector<arn_pattern> _patterns;
};

// `Principal`/`NotPrincipal`. Either the bare `"*"` (anyone, including
// anonymous callers) or a map from principal type (`AWS`, `Service`,
// `Federated`, `CanonicalUser`) to its values, kept as written: the
// evaluator (task 12) resolves an account id to its root ARN against the
// configured partition.
struct principal_set {
    bool anyone = false;
    std::vector<std::pair<std::string, std::vector<std::string>>> by_type;

    // The values given for `type`, or nullptr if the type is absent.
    [[nodiscard]] auto values(std::string_view type) const -> const std::vector<std::string>* {
        for (const auto& [t, v] : by_type) {
            if (t == type) {
                return &v;
            }
        }
        return nullptr;
    }
};

// One key of one operator block: `"StringEquals": {"aws:username": ["a", "b"]}`
// gives one entry with two values. Booleans and numbers are kept as their
// JSON text (`true`, `42`), the way AWS compares them. Task 11 resolves the
// operator name against its table.
struct condition_entry {
    std::string op;
    std::string key;
    std::vector<std::string> values;
};

struct statement {
    std::string sid;
    effect eff = effect::deny;

    std::optional<principal_set> principal;  // Principal or NotPrincipal
    bool not_principal = false;

    action_matcher action;  // Action or NotAction (always present)
    bool not_action = false;

    std::optional<resource_matcher> resource;  // absent only in trust policies
    bool not_resource = false;

    // Keys are ANDed, values of one key ORed (task 11).
    std::vector<condition_entry> conditions;
};

struct policy_document {
    // "2012-10-17" or "2008-10-17" (the default when Version is absent).
    // Policy variables are substituted only under 2012-10-17; under the old
    // version `${...}` is literal text, as on AWS.
    std::string version;
    std::string id;
    std::vector<statement> statements;

    [[nodiscard]] auto supports_variables() const noexcept -> bool {
        return version == "2012-10-17";
    }
};

// ── Parsing ─────────────────────────────────────────────────────────────────

namespace detail {

// A Boost.JSON SAX handler that only looks for duplicate object keys, which
// boost::json::parse silently collapses and AWS refuses. One set of keys per
// open object; arrays push an empty slot so the stack lines up.
struct duplicate_key_finder {
    static constexpr std::size_t max_object_size = static_cast<std::size_t>(-1);
    static constexpr std::size_t max_array_size = static_cast<std::size_t>(-1);
    static constexpr std::size_t max_key_size = static_cast<std::size_t>(-1);
    static constexpr std::size_t max_string_size = static_cast<std::size_t>(-1);

    std::vector<std::optional<std::set<std::string, std::less<>>>> open;
    std::string key;
    std::optional<std::string> duplicate;

    auto on_document_begin(boost::system::error_code& /*ec*/) -> bool { return true; }
    auto on_document_end(boost::system::error_code& /*ec*/) -> bool { return true; }
    auto on_object_begin(boost::system::error_code& /*ec*/) -> bool {
        open.emplace_back(std::in_place);
        return true;
    }
    auto on_object_end(std::size_t /*n*/, boost::system::error_code& /*ec*/) -> bool {
        open.pop_back();
        return true;
    }
    auto on_array_begin(boost::system::error_code& /*ec*/) -> bool {
        open.emplace_back(std::nullopt);
        return true;
    }
    auto on_array_end(std::size_t /*n*/, boost::system::error_code& /*ec*/) -> bool {
        open.pop_back();
        return true;
    }
    auto on_key_part(boost::json::string_view s, std::size_t /*n*/,
                     boost::system::error_code& /*ec*/) -> bool {
        key.append(s.data(), s.size());
        return true;
    }
    auto on_key(boost::json::string_view s, std::size_t /*n*/, boost::system::error_code& ec)
        -> bool {
        key.append(s.data(), s.size());
        if (!open.empty() && open.back() && !open.back()->insert(key).second) {
            duplicate = key;
            ec = boost::json::error::syntax;  // stop the parse
            key.clear();
            return false;
        }
        key.clear();
        return true;
    }
    auto on_string_part(boost::json::string_view /*s*/, std::size_t /*n*/,
                        boost::system::error_code& /*ec*/) -> bool {
        return true;
    }
    auto on_string(boost::json::string_view /*s*/, std::size_t /*n*/,
                   boost::system::error_code& /*ec*/) -> bool {
        return true;
    }
    auto on_number_part(boost::json::string_view /*s*/, boost::system::error_code& /*ec*/) -> bool {
        return true;
    }
    auto on_int64(std::int64_t /*v*/, boost::json::string_view /*s*/,
                  boost::system::error_code& /*ec*/) -> bool {
        return true;
    }
    auto on_uint64(std::uint64_t /*v*/, boost::json::string_view /*s*/,
                   boost::system::error_code& /*ec*/) -> bool {
        return true;
    }
    auto on_double(double /*v*/, boost::json::string_view /*s*/, boost::system::error_code& /*ec*/)
        -> bool {
        return true;
    }
    auto on_bool(bool /*v*/, boost::system::error_code& /*ec*/) -> bool { return true; }
    auto on_null(boost::system::error_code& /*ec*/) -> bool { return true; }
    auto on_comment_part(boost::json::string_view /*s*/, boost::system::error_code& /*ec*/)
        -> bool {
        return true;
    }
    auto on_comment(boost::json::string_view /*s*/, boost::system::error_code& /*ec*/) -> bool {
        return true;
    }
};

// The document's size as IAM counts it: every character except whitespace
// outside string literals. Counted in code points, since the quota is in
// characters.
[[nodiscard]] inline auto policy_size(std::string_view json) -> std::size_t {
    std::size_t n = 0;
    bool in_string = false;
    bool escaped = false;
    for (char c : json) {
        const auto u = static_cast<unsigned char>(c);
        if ((u & 0xC0) == 0x80) {
            continue;  // a continuation byte: its code point was counted already
        }
        if (in_string) {
            ++n;
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        ++n;
        if (c == '"') {
            in_string = true;
        }
    }
    return n;
}

[[noreturn]] inline void malformed(const std::string& message) {
    throw policy_error(message);
}

// A string, or a non-empty array of strings, as AWS allows for Action,
// Resource and principal values.
[[nodiscard]] inline auto string_or_array(const boost::json::value& v, std::string_view element)
    -> std::vector<std::string> {
    std::vector<std::string> out;
    if (const auto* s = v.if_string()) {
        out.emplace_back(*s);
        return out;
    }
    if (const auto* a = v.if_array()) {
        for (const auto& e : *a) {
            const auto* s = e.if_string();
            if (s == nullptr) {
                malformed("Syntax errors in policy: " + std::string(element) +
                          " must be a string or an array of strings.");
            }
            out.emplace_back(*s);
        }
        return out;
    }
    malformed("Syntax errors in policy: " + std::string(element) +
              " must be a string or an array of strings.");
}

[[nodiscard]] inline auto is_alnum(std::string_view s) -> bool {
    return std::ranges::all_of(s, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    });
}

[[nodiscard]] inline auto is_account_id(std::string_view s) -> bool {
    return s.size() == 12 && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

// An action is `*` or `vendor:name`, with wildcards allowed in either part.
inline void validate_action(const std::string& a) {
    if (a == "*") {
        return;
    }
    const auto colon = a.find(':');
    if (colon == std::string::npos || colon == 0) {
        malformed("Actions/Conditions must be prefaced by a vendor, e.g., iam, sdb, ec2, etc.");
    }
}

inline void validate_resource(const std::string& r) {
    if (r == "*" || split_arn(r)) {
        return;
    }
    malformed("Resource " + r + " must be in ARN format or \"*\".");
}

inline auto parse_principal(const boost::json::value& v, std::string_view element)
    -> principal_set {
    principal_set out;
    if (const auto* s = v.if_string()) {
        if (*s != "*") {
            malformed("Invalid principal in policy: " + std::string(element) +
                      " must be \"*\" or an object.");
        }
        out.anyone = true;
        return out;
    }
    const auto* obj = v.if_object();
    if (obj == nullptr || obj->empty()) {
        malformed("Invalid principal in policy: " + std::string(element) +
                  " must be \"*\" or a non-empty object.");
    }
    for (const auto& [k, val] : *obj) {
        const std::string type(k);
        if (type != "AWS" && type != "Service" && type != "Federated" && type != "CanonicalUser") {
            malformed("Invalid principal in policy: unknown principal type \"" + type + "\".");
        }
        auto values = string_or_array(val, std::string(element) + "." + type);
        if (values.empty()) {
            malformed("Invalid principal in policy: \"" + type + "\" has no values.");
        }
        for (const auto& p : values) {
            if (p.empty()) {
                malformed("Invalid principal in policy: \"" + type + "\":\"\"");
            }
            // An AWS principal is "*", an account id, or an ARN. The other
            // types name things outside the account (a service, an identity
            // provider), checked when the evaluator resolves them.
            if (type == "AWS" && p != "*" && !is_account_id(p) && !split_arn(p)) {
                malformed("Invalid principal in policy: \"AWS\":\"" + p + "\"");
            }
        }
        out.by_type.emplace_back(type, std::move(values));
    }
    return out;
}

inline auto condition_scalar(const boost::json::value& v, const std::string& op,
                             const std::string& key) -> std::string {
    if (const auto* s = v.if_string()) {
        return std::string(*s);
    }
    if (v.is_bool()) {
        return v.get_bool() ? "true" : "false";
    }
    if (v.is_number()) {
        return boost::json::serialize(v);
    }
    malformed("Syntax errors in policy: condition " + op + " " + key +
              " values must be strings, numbers or booleans.");
}

inline auto parse_conditions(const boost::json::value& v) -> std::vector<condition_entry> {
    const auto* ops = v.if_object();
    if (ops == nullptr) {
        malformed("Syntax errors in policy: Condition must be an object.");
    }
    std::vector<condition_entry> out;
    for (const auto& [op_name, block] : *ops) {
        const std::string op(op_name);
        if (op.empty()) {
            malformed("Syntax errors in policy: empty condition operator.");
        }
        const auto* keys = block.if_object();
        if (keys == nullptr) {
            malformed("Syntax errors in policy: condition operator " + op +
                      " must map to an object of condition keys.");
        }
        for (const auto& [key_name, values] : *keys) {
            condition_entry e{.op = op, .key = std::string(key_name), .values = {}};
            if (e.key.find(':') == std::string::npos) {
                malformed(
                    "Actions/Conditions must be prefaced by a vendor, e.g., iam, sdb, ec2, etc.");
            }
            if (const auto* a = values.if_array()) {
                if (a->empty()) {
                    malformed("Syntax errors in policy: condition " + op + " " + e.key +
                              " has no values.");
                }
                for (const auto& x : *a) {
                    e.values.push_back(condition_scalar(x, op, e.key));
                }
            } else {
                e.values.push_back(condition_scalar(values, op, e.key));
            }
            out.push_back(std::move(e));
        }
    }
    return out;
}

// Which elements a statement may and must have, by policy type.
struct type_rules {
    bool principal_allowed;
    bool principal_required;
    bool not_principal_allowed;
    bool resource_allowed;
    bool resource_required;
    bool sid_alnum;  // IAM-hosted documents; resource policies allow any Sid
};

[[nodiscard]] constexpr auto rules_for(policy_type t) noexcept -> type_rules {
    switch (t) {
        case policy_type::trust:
            return {.principal_allowed = true,
                    .principal_required = true,
                    .not_principal_allowed = false,
                    .resource_allowed = false,
                    .resource_required = false,
                    .sid_alnum = true};
        case policy_type::resource:
            return {.principal_allowed = true,
                    .principal_required = true,
                    .not_principal_allowed = true,
                    .resource_allowed = true,
                    .resource_required = true,
                    .sid_alnum = false};
        case policy_type::identity:
        case policy_type::boundary:
        case policy_type::session:
            break;
    }
    return {.principal_allowed = false,
            .principal_required = false,
            .not_principal_allowed = false,
            .resource_allowed = true,
            .resource_required = true,
            .sid_alnum = true};
}

inline auto parse_statement(const boost::json::value& v, const type_rules& rules) -> statement {
    const auto* obj = v.if_object();
    if (obj == nullptr) {
        malformed("Syntax errors in policy: each Statement must be an object.");
    }
    statement st;
    const boost::json::value* action = nullptr;
    const boost::json::value* resource = nullptr;
    const boost::json::value* principal = nullptr;
    bool saw_effect = false;
    for (const auto& [k, val] : *obj) {
        const std::string_view key(k.data(), k.size());
        if (key == "Sid") {
            const auto* s = val.if_string();
            if (s == nullptr) {
                malformed("Syntax errors in policy: Sid must be a string.");
            }
            st.sid = std::string(*s);
            if (rules.sid_alnum && !is_alnum(st.sid)) {
                malformed(
                    "Statement IDs (SID) must be alpha-numeric. Check that your input satisfies "
                    "the regular expression [0-9A-Za-z]*");
            }
        } else if (key == "Effect") {
            const auto* s = val.if_string();
            if (s != nullptr && *s == "Allow") {
                st.eff = effect::allow;
            } else if (s != nullptr && *s == "Deny") {
                st.eff = effect::deny;
            } else {
                malformed("Invalid effect: " +
                          (s != nullptr ? std::string(*s) : boost::json::serialize(val)));
            }
            saw_effect = true;
        } else if (key == "Action" || key == "NotAction") {
            if (action != nullptr) {
                malformed(
                    "Syntax errors in policy: a statement may not have both Action and "
                    "NotAction.");
            }
            action = &val;
            st.not_action = key == "NotAction";
        } else if (key == "Resource" || key == "NotResource") {
            if (!rules.resource_allowed) {
                malformed("Has prohibited field " + std::string(key));
            }
            if (resource != nullptr) {
                malformed(
                    "Syntax errors in policy: a statement may not have both Resource and "
                    "NotResource.");
            }
            resource = &val;
            st.not_resource = key == "NotResource";
        } else if (key == "Principal" || key == "NotPrincipal") {
            if (!rules.principal_allowed) {
                malformed("Policy document should not specify a principal.");
            }
            if (key == "NotPrincipal" && !rules.not_principal_allowed) {
                malformed("Has prohibited field NotPrincipal");
            }
            if (principal != nullptr) {
                malformed(
                    "Syntax errors in policy: a statement may not have both Principal and "
                    "NotPrincipal.");
            }
            principal = &val;
            st.not_principal = key == "NotPrincipal";
        } else if (key == "Condition") {
            st.conditions = parse_conditions(val);
        } else {
            malformed("Syntax errors in policy: unknown field " + std::string(key));
        }
    }

    if (!saw_effect) {
        malformed("Missing required field Effect");
    }
    if (principal != nullptr) {
        st.principal = parse_principal(*principal, st.not_principal ? "NotPrincipal" : "Principal");
    } else if (rules.principal_required) {
        malformed("Missing required field Principal");
    }
    if (action == nullptr) {
        malformed("Policy statement must contain actions.");
    }
    const auto actions = string_or_array(*action, st.not_action ? "NotAction" : "Action");
    if (actions.empty()) {
        malformed("Policy statement must contain actions.");
    }
    for (const auto& a : actions) {
        validate_action(a);
    }
    st.action = action_matcher(actions);
    if (resource != nullptr) {
        const auto resources =
            string_or_array(*resource, st.not_resource ? "NotResource" : "Resource");
        if (resources.empty()) {
            malformed("Policy statement must contain resources.");
        }
        for (const auto& r : resources) {
            validate_resource(r);
        }
        st.resource = resource_matcher(resources);
    } else if (rules.resource_required) {
        malformed("Policy statement must contain resources.");
    }
    return st;
}

}  // namespace detail

// Parses and validates `json` as a policy of type `type`. Throws
// `policy_error` for anything AWS would refuse.
[[nodiscard]] inline auto parse_policy(std::string_view json, policy_type type,
                                       const parse_options& options = {}) -> policy_document {
    const auto limit = options.max_size != 0 ? options.max_size : default_max_size(type);
    if (const auto size = detail::policy_size(json); size > limit) {
        throw policy_error("Cannot exceed quota for PolicySize: " + std::to_string(limit),
                           policy_error::limit_exceeded);
    }

    // Duplicate keys first: boost::json::parse would keep one silently.
    {
        boost::json::basic_parser<detail::duplicate_key_finder> scan(boost::json::parse_options{});
        boost::system::error_code ec;
        scan.write_some(false, json.data(), json.size(), ec);
        if (const auto& dup = scan.handler().duplicate) {
            detail::malformed("Syntax errors in policy: duplicate key \"" + *dup + "\".");
        }
        if (ec) {
            detail::malformed("Syntax errors in policy.");
        }
    }
    boost::system::error_code ec;
    const auto root = boost::json::parse(json, ec);
    if (ec) {
        detail::malformed("Syntax errors in policy.");
    }
    const auto* obj = root.if_object();
    if (obj == nullptr) {
        detail::malformed("Syntax errors in policy: the document must be a JSON object.");
    }

    policy_document doc;
    doc.version = "2008-10-17";
    const boost::json::value* statements = nullptr;
    for (const auto& [k, val] : *obj) {
        const std::string_view key(k.data(), k.size());
        if (key == "Version") {
            const auto* s = val.if_string();
            if (s == nullptr || (*s != "2012-10-17" && *s != "2008-10-17")) {
                detail::malformed("The policy failed legacy parsing");
            }
            doc.version = std::string(*s);
        } else if (key == "Id") {
            const auto* s = val.if_string();
            if (s == nullptr) {
                detail::malformed("Syntax errors in policy: Id must be a string.");
            }
            doc.id = std::string(*s);
        } else if (key == "Statement") {
            statements = &val;
        } else {
            detail::malformed("Syntax errors in policy: unknown field " + std::string(key));
        }
    }
    if (statements == nullptr) {
        detail::malformed("Syntax errors in policy: missing required field Statement");
    }

    const auto rules = detail::rules_for(type);
    if (statements->is_object()) {
        doc.statements.push_back(detail::parse_statement(*statements, rules));
    } else if (const auto* arr = statements->if_array()) {
        if (arr->empty()) {
            detail::malformed("Syntax errors in policy: Statement must not be empty.");
        }
        doc.statements.reserve(arr->size());
        for (const auto& s : *arr) {
            doc.statements.push_back(detail::parse_statement(s, rules));
        }
    } else {
        detail::malformed("Syntax errors in policy: Statement must be an object or an array.");
    }

    std::set<std::string_view> sids;
    for (const auto& st : doc.statements) {
        if (!st.sid.empty() && !sids.insert(st.sid).second) {
            detail::malformed("Statement IDs (SID) in a single policy must be unique.");
        }
    }
    return doc;
}

// Convenience matchers for one-off checks and the corpus: AWS action and
// resource matching without building a policy.
[[nodiscard]] inline auto action_matches(std::string_view pattern, std::string_view action)
    -> bool {
    return glob(pattern, /*fold_case=*/true).matches(action);
}

[[nodiscard]] inline auto arn_matches(std::string_view pattern, std::string_view arn) -> bool {
    return arn_pattern(pattern).matches(arn);
}

}  // namespace kythira::iam
