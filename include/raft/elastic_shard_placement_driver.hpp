// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file elastic_shard_placement_driver.hpp
/// @brief The decorator that puts an elastic capacity controller on a host's
///        placement-driver channel, in front of whatever driver the deployment
///        already has (design §5 of `.kiro/specs/elastic-shard-capacity/`).
///
/// A decorator rather than a replacement, so that a deployment with its own
/// driver keeps it: ids stay the inner driver's business, the inner driver's
/// operators go first and win any conflict, and a controller that fails
/// degrades the channel to exactly what the inner driver alone would have done
/// (Requirement 13.7). The capacity controller only ever *adds*.
///
/// ### Wiring
///
/// The adapter is a plain object, so it serves both deployment shapes the
/// repository has. In-process (embedded deployments, the Docker chaos suite):
/// construct one per host over one shared controller and point the host's
/// `multi_raft_config` hooks at it —
///
/// ```cpp
/// cfg.allocate_shard_ids     = [&](auto n) { return adapter.allocate_shard_ids(n).get(); };
/// cfg.report_shard_heartbeat = [&](const auto& r) { return
/// adapter.report_shard_heartbeat(r).get(); }; cfg.report_node_heartbeat  = [&](const auto& r) {
/// adapter.report_node_heartbeat(r).get(); }; cfg.report_operator_outcomes = [&](const auto& o) {
/// adapter.report_operator_outcomes(o); }; cfg.lookup_descriptor      = [&](const auto& g) { return
/// adapter.lookup_descriptor(g); };
/// ```
///
/// Out-of-process, a control-plane binary owns the adapter and each host's
/// hooks are the application's own RPC to it — the hooks are `std::function`
/// precisely so Kythira never picks that RPC.
///
/// **`lookup_descriptor` is a deployment requirement, not an option**
/// (Requirement 10.2): without it a newly provisioned machine cannot
/// materialise a replica of a group whose descriptor it has never seen, and an
/// admission stalls at its first learner.

#include <raft/elastic_capacity_controller.hpp>
#include <raft/future_default.hpp>
#include <raft/shard_placement_driver.hpp>
#include <raft/shard_types.hpp>

#include <exception>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace kythira {

/// @brief A placement driver that is `Inner` plus elastic capacity.
///
/// @tparam Inner      The deployment's own driver (`no_op_shard_placement_driver`
///                    when it has none).
/// @tparam Controller An `elastic_capacity_controller` over the same group,
///                    key and node types. Held by reference: one controller
///                    serves every host's adapter.
template<typename Inner, typename Controller, typename GroupId, typename Key, typename NodeId>
requires shard_placement_driver<Inner, GroupId, Key, NodeId>
class elastic_shard_placement_driver {
public:
    using descriptor_type = shard_descriptor<GroupId, Key, NodeId>;
    using report_type = shard_report<GroupId, Key, NodeId>;
    using operation_type = shard_operation<GroupId, Key, NodeId>;

    /// @param controller The shared controller; must outlive the adapter.
    /// @param inner_args Constructor arguments for the inner driver, built in
    ///                   place because a driver holding a mutex (the no-op
    ///                   driver does) cannot be moved in.
    template<typename... Args>
    explicit elastic_shard_placement_driver(Controller& controller, Args&&... inner_args)
        : _inner(std::forward<Args>(inner_args)...), _ctl(&controller) {}

    /// @brief Straight through: ids are the inner driver's, and a capacity
    ///        controller has no opinion about them.
    auto allocate_shard_ids(std::size_t n) { return _inner.allocate_shard_ids(n); }

    /// @brief The inner driver's operators, then the controller's for groups
    ///        the inner driver did not name.
    ///
    /// The controller is fed and stepped *after* the inner driver answers, on
    /// whatever thread completes that answer; a throw from it is logged by
    /// the controller's own sink and costs only its operators.
    auto report_shard_heartbeat(const std::vector<report_type>& reports) {
        auto* ctl = _ctl;
        return _inner.report_shard_heartbeat(reports).thenValue(
            [ctl, reports](std::vector<operation_type> inner) {
                std::vector<operation_type> mine;
                try {
                    mine = ctl->step_for(reports);
                } catch (...) {
                    // Requirement 13.7: a failed controller is static capacity.
                    mine.clear();
                }
                return merge(std::move(inner), std::move(mine));
            });
    }

    auto report_node_heartbeat(const node_report<NodeId>& report) {
        try {
            _ctl->observe_node(report);
        } catch (...) {
        }
        return _inner.report_node_heartbeat(report);
    }

    /// @brief A split is a trigger edge for the controller (Requirement 5.1).
    auto report_split(const descriptor_type& parent, const std::vector<descriptor_type>& children) {
        try {
            _ctl->observe_split(parent, children);
        } catch (...) {
        }
        return _inner.report_split(parent, children);
    }

    auto report_merge(const descriptor_type& source, const descriptor_type& target) {
        try {
            _ctl->observe_merge(source, target);
        } catch (...) {
        }
        return _inner.report_merge(source, target);
    }

    /// @brief Outcomes of operators the host applied. The controller ignores
    ///        ids it did not issue.
    auto report_operator_outcomes(const std::vector<operator_outcome>& outcomes) -> void {
        try {
            _ctl->observe_operator_outcomes(outcomes);
        } catch (...) {
        }
    }

    /// @brief Beyond the concept: what the host's lazy replica creation asks.
    ///
    /// The controller's view first (it knows the groups it is moving), then
    /// the inner driver's, where it has one.
    auto lookup_descriptor(const GroupId& group) -> std::optional<descriptor_type> {
        try {
            if (auto d = _ctl->lookup_descriptor(group)) {
                return d;
            }
        } catch (...) {
        }
        if constexpr (requires(Inner& i) {
                          {
                              i.lookup_descriptor(group)
                          } -> std::same_as<std::optional<descriptor_type>>;
                      }) {
            return _inner.lookup_descriptor(group);
        } else {
            return std::nullopt;
        }
    }

    [[nodiscard]] auto inner() -> Inner& { return _inner; }

    /// @brief Inner first; a controller operator on a group the inner driver
    ///        already named is dropped, never sent twice (design §5).
    static auto merge(std::vector<operation_type> inner, std::vector<operation_type> mine)
        -> std::vector<operation_type> {
        std::set<GroupId> named;
        for (const auto& op : inner) {
            named.insert(op.group_id());
        }
        for (auto& op : mine) {
            if (!named.contains(op.group_id())) {
                inner.push_back(std::move(op));
            }
        }
        return inner;
    }

private:
    Inner _inner;
    Controller* _ctl;
};

}  // namespace kythira
