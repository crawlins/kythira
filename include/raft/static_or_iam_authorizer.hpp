// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file static_or_iam_authorizer.hpp
/// @brief A gateway's authorizer that signs requests in by static keys,
///        Kythira IAM, or both, as the Redis gateway's
///        `auth=password|iam|both` (`.kiro/specs/dynamodb-compatible-api/`
///        task 28; the DynamoDB and Kinesis gateways both use it).
///
/// Nothing here includes IAM. A daemon's composition root hands IAM in as
/// `iam_hooks`, three functions over an embedded IAM replica's authorizer,
/// when it is built with IAM; a build without it can only run on static
/// keys, and `set_mode` refuses the other two.
///
/// With `both`, a request signed with a static key's id is the static
/// authorizer's and any other is IAM's. An access key id that both know is
/// refused, whichever way it arrives: the gateway checks its static keys
/// against IAM at start (`conflicting_keys`), and a key made in IAM later
/// with a static key's id is refused at each request rather than letting
/// one secret stand in for the other.

#include <raft/aws_request_authorizer.hpp>
#include <raft/sigv4_verifier.hpp>
#include <raft/static_key_authorizer.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::aws_auth {

/// `KYTHIRA_DYNAMODB_AUTH`, `KINESIS_AUTH`.
enum class auth_mode : std::uint8_t {
    static_keys,
    iam,
    both
};

/// IAM's side of the gateway's authorizer, type-erased.
struct iam_hooks {
    std::function<authn_result(const sigv4::request&, const sigv4::service_rules&)> authenticate;
    std::function<authz_decision(const principal_context&, const authz_request&)> authorize;
    /// Whether IAM has an access key with this id (for the `both` check).
    std::function<bool(std::string_view)> has_key;
};

class static_or_iam_authorizer {
public:
    using clock_fn = static_key_authorizer::clock_fn;

    /// Throws `std::invalid_argument` on a bad key table (see
    /// `static_key_authorizer`).
    static_or_iam_authorizer(const std::vector<static_key>& keys, std::string partition,
                             clock_fn clock = {})
        : _clock(clock ? std::move(clock) : [] { return std::chrono::system_clock::now(); }),
          _static(keys, std::move(partition), _clock) {
        for (const auto& k : keys) {
            _static_ids.insert(k.access_key_id);
        }
    }

    /// Chooses the mode. `iam` and `both` need `hooks`; throws
    /// `std::invalid_argument` without them.
    void set_mode(auth_mode mode, std::optional<iam_hooks> hooks) {
        if (mode != auth_mode::static_keys && !hooks) {
            throw std::invalid_argument("IAM authentication needs IAM, which this build lacks");
        }
        _mode = mode;
        _iam = std::move(hooks);
    }

    [[nodiscard]] auto mode() const -> auth_mode { return _mode; }

    /// The static keys IAM also has, which `both` refuses to start with.
    [[nodiscard]] auto conflicting_keys() const -> std::vector<std::string> {
        std::vector<std::string> out;
        if (_mode == auth_mode::both && _iam && _iam->has_key) {
            for (const auto& id : _static_ids) {
                if (_iam->has_key(id)) {
                    out.push_back(id);
                }
            }
        }
        return out;
    }

    void register_service(static_role_table table) { _static.register_service(std::move(table)); }

    [[nodiscard]] auto authenticate(const sigv4::request& req, const sigv4::service_rules& rules)
        -> authn_result {
        switch (_mode) {
            case auth_mode::static_keys:
                return _static.authenticate(req, rules);
            case auth_mode::iam:
                return _iam->authenticate(req, rules);
            case auth_mode::both:
                break;
        }
        const auto now = std::chrono::floor<std::chrono::seconds>(_clock());
        const auto parsed = sigv4::parse(req, rules, now);
        if (!parsed) {
            return parsed.error();
        }
        if (!_static_ids.contains(parsed->access_key_id)) {
            return _iam->authenticate(req, rules);
        }
        if (_iam->has_key && _iam->has_key(parsed->access_key_id)) {
            return sigv4::invalid_client_token_id(rules.protocol);
        }
        return _static.authenticate(req, rules);
    }

    [[nodiscard]] auto authorize(const principal_context& who, const authz_request& r)
        -> authz_decision {
        if (_mode == auth_mode::static_keys ||
            (_mode == auth_mode::both && _static_ids.contains(who.access_key_id) &&
             who.token_issue_time == std::nullopt)) {
            return _static.authorize(who, r);
        }
        return _iam->authorize(who, r);
    }

private:
    // Before `_static`, which is built with a copy of it: both judge a
    // signature's date by the same clock.
    clock_fn _clock;
    static_key_authorizer _static;
    std::set<std::string, std::less<>> _static_ids;
    auth_mode _mode = auth_mode::static_keys;
    std::optional<iam_hooks> _iam;
};

static_assert(aws_request_authorizer<static_or_iam_authorizer>);

}  // namespace kythira::aws_auth
