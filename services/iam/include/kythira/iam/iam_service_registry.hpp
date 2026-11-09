// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_service_registry.hpp
/// @brief The actions each Kythira-served AWS API declares
///        (`.kiro/specs/iam-compatible-auth/` Requirement 7.2).
///
/// A service hands its `aws_action_table` (core `aws_request_authorizer.hpp`)
/// to the registry through its composition root. The authorizer denies any
/// action no table names, before a policy is looked at: a policy allowing
/// `kinesis:*` grants nothing on a node that serves no Kinesis.

#include <raft/aws_request_authorizer.hpp>

#include <kythira/iam/iam_policy.hpp>

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace kythira::iam {

class iam_service_registry {
public:
    /// Adds a service's table. A prefix may be added once, and its action
    /// names must be distinct; both are programming errors, so they throw.
    void add(const aws_auth::aws_action_table& table) {
        const auto prefix = detail::ascii_lower(table.service_prefix);
        if (prefix.empty() || prefix.find(':') != std::string::npos) {
            throw std::invalid_argument("iam_service_registry: bad service prefix \"" +
                                        table.service_prefix + "\"");
        }
        if (_services.contains(prefix)) {
            throw std::invalid_argument("iam_service_registry: service \"" + prefix +
                                        "\" added twice");
        }
        std::map<std::string, aws_auth::action_entry> actions;
        for (const auto& a : table.actions) {
            if (!actions.emplace(detail::ascii_lower(a.name), a).second) {
                throw std::invalid_argument("iam_service_registry: action \"" + prefix + ":" +
                                            a.name + "\" listed twice");
            }
        }
        _services.emplace(prefix, std::move(actions));
    }

    /// The entry for `prefix:Name` (any case), or null when no service
    /// declares it.
    [[nodiscard]] auto find(std::string_view action) const -> const aws_auth::action_entry* {
        const auto colon = action.find(':');
        if (colon == std::string_view::npos) {
            return nullptr;
        }
        const auto svc = _services.find(detail::ascii_lower(action.substr(0, colon)));
        if (svc == _services.end()) {
            return nullptr;
        }
        const auto it = svc->second.find(detail::ascii_lower(action.substr(colon + 1)));
        return it == svc->second.end() ? nullptr : &it->second;
    }

    [[nodiscard]] auto serves(std::string_view prefix) const -> bool {
        return _services.contains(detail::ascii_lower(prefix));
    }

private:
    std::map<std::string, std::map<std::string, aws_auth::action_entry>> _services;
};

}  // namespace kythira::iam
