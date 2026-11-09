// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_replica.hpp
/// @brief A read replica of the IAM Raft group, embeddable in any front end
///        (`.kiro/specs/iam-compatible-auth/` Requirements 7.3-7.4, design
///        Component 6 "Replica").
///
/// A front end (Kinesis, DynamoDB, the Redis adapter) builds the Raft
/// transport and persistence it already uses, hands them to `iam_replica`,
/// and gets back a learner that applies the IAM log locally:
///
///   iam::iam_replica<types> replica(std::move(node_cfg), voters, 60s);
///   replica.start();          // then `kythira-iam replica-add` admits it
///   iam::iam_authorizer authz(replica.reader(), kek, registry);
///   authz.set_freshness([&] { return replica.freshness().fresh(); });
///
/// Authentication and authorization then make no network call. The
/// replica's staleness is how long ago it last held, and had applied,
/// every change its leader had committed (`node::last_caught_up()`). On a
/// healthy replica that is at most a heartbeat interval; one cut off from
/// the voters stops moving, and past the bound the authorizer fails
/// closed with `ServiceUnavailable`.

#include <kythira/iam/iam_authorizer.hpp>
#include <kythira/iam/iam_state_machine.hpp>

#include <raft/raft.hpp>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace kythira::iam {

/// How stale a replica's state can be, from when it was last caught up.
class replica_freshness {
public:
    using time_point = std::chrono::steady_clock::time_point;
    using caught_up_fn = std::function<std::optional<time_point>()>;
    using clock_fn = std::function<time_point()>;

    static constexpr std::chrono::milliseconds k_default_max_staleness{60000};

    explicit replica_freshness(caught_up_fn caught_up,
                               std::chrono::milliseconds max_staleness = k_default_max_staleness,
                               clock_fn clock = nullptr)
        : _caught_up(std::move(caught_up)),
          _max(max_staleness),
          _clock(clock ? std::move(clock)
                       : clock_fn([] { return std::chrono::steady_clock::now(); })) {}

    /// Seconds since the replica was last caught up; empty when it never
    /// has been, which counts as infinitely stale.
    [[nodiscard]] auto lag() const -> std::optional<std::chrono::duration<double>> {
        const auto at = _caught_up();
        if (!at) {
            return std::nullopt;
        }
        const auto d = _clock() - *at;
        return std::chrono::duration<double>(
            d < time_point::duration::zero() ? time_point::duration::zero() : d);
    }

    /// Whether the replica may answer: caught up within the bound.
    [[nodiscard]] auto fresh() const -> bool {
        const auto l = lag();
        return l.has_value() && *l <= _max;
    }

    [[nodiscard]] auto max_staleness() const -> std::chrono::milliseconds { return _max; }

    /// The lag in Prometheus' text exposition format, `+Inf` before the
    /// replica has first caught up.
    [[nodiscard]] auto metrics_text() const -> std::string {
        const auto l = lag();
        std::string value = "+Inf";
        if (l) {
            char buf[32];
            (void)std::snprintf(buf, sizeof buf, "%.3f", l->count());
            value = buf;
        }
        return "# HELP kythira_iam_replica_lag_seconds Seconds since this IAM replica last held "
               "every committed change.\n"
               "# TYPE kythira_iam_replica_lag_seconds gauge\n"
               "kythira_iam_replica_lag_seconds " +
               value + "\n";
    }

private:
    caught_up_fn _caught_up;
    std::chrono::milliseconds _max;
    clock_fn _clock;
};

/// A learner of the IAM Raft group, owning its `kythira::node`.
///
/// The caller builds `node_config` (transport, TLS, persistence, logger)
/// as its own Raft groups do; `voters` are the IAM group's voting members.
/// The voters' leader admits this node once it is registered
/// (`kythira-iam replica-add`, `POST /v1/admin/replicas`), and every later
/// leader re-admits it from the registered replicas in the IAM state.
template<raft_types Types> class iam_replica {
public:
    using node_type = kythira::node<Types>;
    using node_id_type = typename Types::node_id_type;
    static_assert(std::is_same_v<typename Types::state_machine_type, iam_state_machine>,
                  "an IAM replica applies the IAM state machine");

    iam_replica(
        node_config<Types> cfg, std::vector<node_id_type> voters,
        std::chrono::milliseconds max_staleness = replica_freshness::k_default_max_staleness,
        replica_freshness::clock_fn clock = nullptr)
        : _self(cfg.node_id),
          _voters(std::move(voters)),
          _node(std::make_unique<node_type>(std::move(cfg))),
          _freshness([n = _node.get()] { return n->last_caught_up(); }, max_staleness,
                     std::move(clock)) {}

    iam_replica(const iam_replica&) = delete;
    auto operator=(const iam_replica&) -> iam_replica& = delete;
    iam_replica(iam_replica&&) = delete;
    auto operator=(iam_replica&&) -> iam_replica& = delete;
    ~iam_replica() { stop(); }

    /// Joins as a learner: a learner never stands for election, so no
    /// election or heartbeat timer is needed, only the RPC listener the
    /// leader's AppendEntries arrive on.
    void start() {
        _node->set_cluster_configuration(_voters, {_self});
        _node->start();
        _started = true;
    }

    void stop() {
        if (_started) {
            _node->stop();
            _started = false;
        }
    }

    /// For `iam_authorizer`: runs `f` on the applied IAM state, under the
    /// node's lock, so `f` sees one consistent state and must not block.
    [[nodiscard]] auto reader() -> state_reader {
        return [n = _node.get()](const std::function<void(const iam_state_machine&)>& f) {
            n->with_state_machine([&](iam_state_machine& sm) { f(sm); });
        };
    }

    [[nodiscard]] auto freshness() const -> const replica_freshness& { return _freshness; }
    [[nodiscard]] auto node() -> node_type& { return *_node; }

private:
    node_id_type _self;
    std::vector<node_id_type> _voters;
    std::unique_ptr<node_type> _node;
    replica_freshness _freshness;
    bool _started = false;
};

}  // namespace kythira::iam
