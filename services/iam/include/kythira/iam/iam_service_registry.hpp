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
///
/// In kythira_node, co-located modules add their tables at start, while
/// the IAM module already serves (`merge`), so the registry locks: tables
/// are added under an exclusive lock and looked up under a shared one. An
/// entry, once added, is never moved or removed, so the pointer `find`
/// returns stays valid.

#include <raft/aws_request_authorizer.hpp>

#include <kythira/iam/iam_policy.hpp>

#include <map>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace kythira::iam {

class iam_service_registry {
public:
    iam_service_registry() = default;
    iam_service_registry(const iam_service_registry& other) : _services(other.snapshot()) {}
    iam_service_registry(iam_service_registry&& other) noexcept
        : _services(std::move(other._services)) {}
    auto operator=(const iam_service_registry& other) -> iam_service_registry& {
        if (this != &other) {
            auto copy = other.snapshot();
            const std::unique_lock lock(_mu);
            _services = std::move(copy);
        }
        return *this;
    }
    auto operator=(iam_service_registry&& other) noexcept -> iam_service_registry& {
        const std::unique_lock lock(_mu);
        _services = std::move(other._services);
        return *this;
    }
    ~iam_service_registry() = default;

    /// Adds a service's table. A prefix may be added once, and its action
    /// names must be distinct; both are programming errors, so they throw.
    void add(const aws_auth::aws_action_table& table) {
        const auto prefix = checked_prefix(table);
        const std::unique_lock lock(_mu);
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

    /// Adds the table's actions that are not there yet, under a prefix
    /// that may already be: two co-located modules may each authorize
    /// some of one service's actions (Kinesis's Lease API and DynamoDB
    /// both declare DynamoDB actions). An action already present keeps
    /// its first entry.
    void merge(const aws_auth::aws_action_table& table) {
        const auto prefix = checked_prefix(table);
        const std::unique_lock lock(_mu);
        auto& actions = _services[prefix];
        for (const auto& a : table.actions) {
            actions.emplace(detail::ascii_lower(a.name), a);
        }
    }

    /// The entry for `prefix:Name` (any case), or null when no service
    /// declares it.
    [[nodiscard]] auto find(std::string_view action) const -> const aws_auth::action_entry* {
        const auto colon = action.find(':');
        if (colon == std::string_view::npos) {
            return nullptr;
        }
        const std::shared_lock lock(_mu);
        const auto svc = _services.find(detail::ascii_lower(action.substr(0, colon)));
        if (svc == _services.end()) {
            return nullptr;
        }
        const auto it = svc->second.find(detail::ascii_lower(action.substr(colon + 1)));
        return it == svc->second.end() ? nullptr : &it->second;
    }

    [[nodiscard]] auto serves(std::string_view prefix) const -> bool {
        const std::shared_lock lock(_mu);
        return _services.contains(detail::ascii_lower(prefix));
    }

private:
    using services_map = std::map<std::string, std::map<std::string, aws_auth::action_entry>>;

    [[nodiscard]] static auto checked_prefix(const aws_auth::aws_action_table& table)
        -> std::string {
        auto prefix = detail::ascii_lower(table.service_prefix);
        if (prefix.empty() || prefix.find(':') != std::string::npos) {
            throw std::invalid_argument("iam_service_registry: bad service prefix \"" +
                                        table.service_prefix + "\"");
        }
        return prefix;
    }

    [[nodiscard]] auto snapshot() const -> services_map {
        const std::shared_lock lock(_mu);
        return _services;
    }

    mutable std::shared_mutex _mu;
    services_map _services;
};

}  // namespace kythira::iam
