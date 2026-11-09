// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file static_key_authorizer.hpp
/// @brief A fixed access-key table with three roles, for clusters that run
///        without IAM (`.kiro/specs/dynamodb-compatible-api/` Requirement
///        5.3, and the same for Kinesis).
///
/// Satisfies `aws_request_authorizer`, so a front end takes either this or
/// the IAM authorizer without knowing which:
///
///   static_key_authorizer auth({{"AKIDEXAMPLE", secret, "123456789012",
///                                "ci", static_role::read_write}});
///   auth.register_service(dynamodb_static_roles());   // "dynamodb" patterns
///   auth.register_service(kinesis_static_roles());    // "kinesis" patterns
///
/// Each service registers its own role table under its prefix, so the
/// patterns live beside the service's action table. A key's role then
/// decides, per action:
///
///   read_only   the action matches one of the service's read-only patterns
///   read_write  ... or one of its read-write patterns
///   admin       the action belongs to a registered service
///
/// An action whose prefix no service registered is denied for every role,
/// so `admin` on a node that serves only DynamoDB grants nothing in
/// `kinesis:` (as the IAM authorizer refuses undeclared actions). Patterns
/// are IAM-style action globs (`*` any run, `?` one character) matched
/// case-insensitively, as AWS matches action names.
///
/// Authentication is ordinary SigV4 through `sigv4_verifier`: the key must
/// be in the table and the signature must verify against its secret.
/// Temporary credentials (a session token, an `ASIA` key) are refused as an
/// unknown token, since nothing here issues them. There are no resource
/// policies, conditions or tags: a role is all a key has.
///
/// Register services before serving; registration and lookups are guarded
/// so a late registration is safe, but a request racing it may see the
/// table without the new service.

#include <raft/aws_request_authorizer.hpp>
#include <raft/sigv4_verifier.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::aws_auth {

enum class static_role : std::uint8_t {
    read_only,
    read_write,
    admin
};

[[nodiscard]] constexpr auto to_string(static_role r) noexcept -> std::string_view {
    switch (r) {
        case static_role::read_only:
            return "read-only";
        case static_role::read_write:
            return "read-write";
        case static_role::admin:
            return "admin";
    }
    return "unknown";
}

/// One configured key. `name` becomes the principal's user name and ARN.
struct static_key {
    std::string access_key_id;
    std::string secret;
    std::string account;
    std::string name;
    static_role role = static_role::read_only;
};

/// A service's role patterns, by action name without the prefix
/// (`Get*`, `DescribeTable`). `read_write` adds to `read_only`; admin needs
/// no list.
struct static_role_table {
    std::string service_prefix;
    std::vector<std::string> read_only;
    std::vector<std::string> read_write;
};

namespace detail {

[[nodiscard]] inline auto ascii_lower(std::string_view s) -> std::string {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// IAM action-glob match: `*` matches any run (including none), `?` one
/// character. Both sides already lower-cased.
[[nodiscard]] inline auto glob_match(std::string_view pattern, std::string_view text) -> bool {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t resume = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

[[nodiscard]] inline auto valid_prefix(std::string_view prefix) -> bool {
    return !prefix.empty() && std::all_of(prefix.begin(), prefix.end(), [](unsigned char c) {
        return std::islower(c) != 0 || std::isdigit(c) != 0 || c == '-';
    });
}

}  // namespace detail

class static_key_authorizer {
public:
    using clock_fn = std::function<std::chrono::system_clock::time_point()>;

    /// Throws `std::invalid_argument` on a key with an empty id, secret or
    /// account, or a repeated id.
    explicit static_key_authorizer(
        std::vector<static_key> keys, std::string partition = "aws",
        clock_fn clock = [] { return std::chrono::system_clock::now(); })
        : _partition(std::move(partition)), _clock(std::move(clock)) {
        for (auto& k : keys) {
            if (k.access_key_id.empty() || k.secret.empty() || k.account.empty()) {
                throw std::invalid_argument("static key needs an id, a secret and an account");
            }
            const auto id = k.access_key_id;
            if (!_keys.emplace(id, std::move(k)).second) {
                throw std::invalid_argument("duplicate static access key " + id);
            }
        }
    }

    /// Adds a service's role patterns. Throws `std::invalid_argument` on a
    /// malformed or already registered prefix, or an empty pattern; a
    /// refused table leaves nothing registered.
    void register_service(static_role_table table) {
        if (!detail::valid_prefix(table.service_prefix)) {
            throw std::invalid_argument("bad service prefix '" + table.service_prefix + "'");
        }
        compiled c;
        for (const auto& p : table.read_only) {
            if (p.empty()) {
                throw std::invalid_argument("empty action pattern in " + table.service_prefix);
            }
            c.read_only.push_back(detail::ascii_lower(p));
        }
        for (const auto& p : table.read_write) {
            if (p.empty()) {
                throw std::invalid_argument("empty action pattern in " + table.service_prefix);
            }
            c.read_write.push_back(detail::ascii_lower(p));
        }
        const std::unique_lock lock(_mutex);
        if (!_services.emplace(table.service_prefix, std::move(c)).second) {
            throw std::invalid_argument("service " + table.service_prefix + " already registered");
        }
    }

    [[nodiscard]] auto authenticate(const sigv4::request& req, const sigv4::service_rules& rules)
        -> authn_result {
        const auto now = std::chrono::floor<std::chrono::seconds>(_clock());
        auto parsed = sigv4::parse(req, rules, now);
        if (!parsed) {
            return parsed.error();
        }
        const auto& c = *parsed;
        if (c.security_token.has_value() || c.access_key_id.starts_with("ASIA")) {
            return sigv4::invalid_client_token_id(rules.protocol);
        }
        const auto it = _keys.find(c.access_key_id);
        if (it == _keys.end()) {
            return sigv4::invalid_client_token_id(rules.protocol);
        }
        if (!sigv4::verify(c, sigv4::derive_signing_key(it->second.secret, c.scope))) {
            return sigv4::signature_mismatch(rules.protocol);
        }
        const auto& k = it->second;
        principal_context p;
        p.type = principal_type::user;
        p.account = k.account;
        p.username = k.name.empty() ? k.access_key_id : k.name;
        p.arn = "arn:" + _partition + ":iam::" + k.account + ":user/" + p.username;
        p.userid = k.access_key_id;
        p.access_key_id = k.access_key_id;
        return p;
    }

    [[nodiscard]] auto authorize(const principal_context& who, const authz_request& r)
        -> authz_decision {
        authz_decision d;
        const auto key = _keys.find(who.access_key_id);
        if (key == _keys.end() || key->second.account != who.account) {
            d.reason = "unknown static key";
            return d;
        }
        const auto colon = r.action.find(':');
        if (colon == std::string::npos) {
            d.reason = "malformed action";
            return d;
        }
        const auto prefix = detail::ascii_lower(std::string_view(r.action).substr(0, colon));
        const auto action = detail::ascii_lower(std::string_view(r.action).substr(colon + 1));
        if (!r.resource_account.empty() && r.resource_account != who.account) {
            d.reason = "cross-account access is not available with static keys";
            return d;
        }
        const std::shared_lock lock(_mutex);
        const auto svc = _services.find(prefix);
        if (svc == _services.end()) {
            d.reason = "no service registered for " + prefix;
            return d;
        }
        const auto matches = [&](const std::vector<std::string>& patterns) {
            return std::any_of(patterns.begin(), patterns.end(),
                               [&](const std::string& p) { return detail::glob_match(p, action); });
        };
        const auto role = key->second.role;
        switch (role) {
            case static_role::admin:
                d.allowed = true;
                break;
            case static_role::read_write:
                d.allowed = matches(svc->second.read_only) || matches(svc->second.read_write);
                break;
            case static_role::read_only:
                d.allowed = matches(svc->second.read_only);
                break;
        }
        d.policy = "static-key-role";
        d.sid = std::string(to_string(role));
        d.reason = d.allowed ? "allowed by role" : "not in role";
        return d;
    }

private:
    struct compiled {
        std::vector<std::string> read_only;
        std::vector<std::string> read_write;
    };

    std::map<std::string, static_key, std::less<>> _keys;  // immutable after construction
    std::string _partition;
    clock_fn _clock;
    mutable std::shared_mutex _mutex;
    std::map<std::string, compiled, std::less<>> _services;
};

static_assert(aws_request_authorizer<static_key_authorizer>);

}  // namespace kythira::aws_auth
