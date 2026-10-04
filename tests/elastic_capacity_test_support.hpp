// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file elastic_capacity_test_support.hpp
/// @brief The pieces every elastic-capacity controller suite shares: a
///        scripted policy, a lease the test can flip, and a simulated cluster
///        that answers operators the way `multi_raft::apply_operator` does.
///
/// The simulated cluster is deliberately *not* the real host. The controller's
/// unit and property suites need thousands of deterministic steps under a
/// manual clock, which a real multi-Raft fabric cannot give; the fabric suite
/// (`elastic_capacity_fabric_test.cpp`) is where the real host is exercised.
/// What the simulator must get right is the host's *contract*: epoch and
/// leader checks, the same preconditions, learners that need time to catch up,
/// and a skipped operator reported with the host's reason.

#include "mock_capacity_quorum_manager.hpp"

#include <raft/capacity_policy.hpp>
#include <raft/elastic_capacity_controller.hpp>
#include <raft/capacity_ledger.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace kythira::testing {

using cap_node = std::uint64_t;
using cap_group = std::uint64_t;
using cap_key = std::string;
using cap_pg = std::string;

using cap_snapshot = cluster_capacity_snapshot<cap_node, cap_group, cap_key, cap_pg>;
using cap_decision = capacity_decision<cap_pg, cap_node>;
using cap_descriptor = shard_descriptor<cap_group, cap_key, cap_node>;
using cap_shard_report = shard_report<cap_group, cap_key, cap_node>;
using cap_operation = shard_operation<cap_group, cap_key, cap_node>;

/// @brief A policy that returns whatever the test queued, and remembers every
///        snapshot it was shown.
class scripted_capacity_policy {
public:
    struct state {
        std::deque<cap_decision> _queue;
        std::vector<cap_snapshot> _seen;
        std::chrono::milliseconds _cooldown{0};
        std::vector<std::string> _errors;
        /// Returned when the queue is empty instead of hold, if set.
        std::optional<cap_decision> _repeat;
    };

    scripted_capacity_policy() : _s(std::make_shared<state>()) {}

    auto push(cap_decision d) -> void { _s->_queue.push_back(std::move(d)); }
    auto repeat(std::optional<cap_decision> d) -> void { _s->_repeat = std::move(d); }
    auto set_cooldown(std::chrono::milliseconds c) -> void { _s->_cooldown = c; }
    auto set_errors(std::vector<std::string> e) -> void { _s->_errors = std::move(e); }
    [[nodiscard]] auto seen() const -> const std::vector<cap_snapshot>& { return _s->_seen; }
    [[nodiscard]] auto evaluations() const -> std::size_t { return _s->_seen.size(); }

    auto evaluate(const cap_snapshot& s) -> cap_decision {
        _s->_seen.push_back(s);
        if (!_s->_queue.empty()) {
            auto d = _s->_queue.front();
            _s->_queue.pop_front();
            return d;
        }
        return _s->_repeat ? *_s->_repeat : cap_decision::hold();
    }
    [[nodiscard]] auto cooldown() const -> std::chrono::milliseconds { return _s->_cooldown; }
    [[nodiscard]] auto validate() const -> bool { return _s->_errors.empty(); }
    [[nodiscard]] auto get_validation_errors() const -> std::vector<std::string> {
        return _s->_errors;
    }

private:
    std::shared_ptr<state> _s;
};

static_assert(capacity_policy<scripted_capacity_policy, cap_node, cap_group, cap_key, cap_pg>);

/// @brief A lease the test holds the other end of.
class toggle_lease {
public:
    struct state {
        bool _held{true};
        std::uint64_t _token{1};
        bool _throws{false};
        std::size_t _checks{0};
    };

    toggle_lease() : _s(std::make_shared<state>()) {}

    auto set(bool held, std::uint64_t token) -> void {
        _s->_held = held;
        _s->_token = token;
    }
    auto set_held(bool held) -> void { _s->_held = held; }
    auto set_throws(bool t) -> void { _s->_throws = t; }
    [[nodiscard]] auto checks() const -> std::size_t { return _s->_checks; }

    [[nodiscard]] auto held() -> bool {
        ++_s->_checks;
        if (_s->_throws) {
            throw std::runtime_error("lease probe failed");
        }
        return _s->_held;
    }
    [[nodiscard]] auto fencing_token() -> std::uint64_t { return _s->_token; }

private:
    std::shared_ptr<state> _s;
};

static_assert(capacity_lease<toggle_lease>);

/// @brief A cluster of hosts and shards that answers operators like
///        `multi_raft::apply_operator`.
class simulated_cluster {
public:
    struct shard {
        cap_descriptor _d;
        cap_node _leader{};
        shard_operation_state _op{shard_operation_state::stable};
        std::map<cap_node, int> _catching_up;
        std::size_t _bytes{0};
        double _write_bps{0.0};
        std::size_t _initial_voters{0};
        std::size_t _min_voters_seen{std::numeric_limits<std::size_t>::max()};
    };

    struct host {
        node_report<cap_node> _report;
        bool _alive{true};
    };

    /// Steps a newly added learner needs before it stops being pending.
    int _catch_up_steps{2};
    /// Operators the hosts accepted, in order, for assertions.
    std::vector<cap_operation> _accepted;
    std::vector<std::pair<cap_operation, skipped_operator_reason>> _skipped;
    /// When set, every operator on these groups is skipped with this reason.
    std::map<cap_group, skipped_operator_reason> _force_skip;
    /// Refuse every leader transfer as `unsupported`, as a host on a
    /// transport without TimeoutNow (cpp-httplib, Beast, Proxygen) does.
    bool _transfer_unsupported{false};
    /// Operators that arrived for a shard mid-split or mid-merge. The host
    /// refuses them as busy; a controller should never have sent one.
    std::vector<cap_operation> _sent_while_busy;

    /// `uptime` is what lets a controller tell a machine that booted after an
    /// intent from one that was always there; hosts present from the start
    /// have been up a day.
    auto add_host(cap_node id, cap_pg label, std::uint64_t capacity = 1'000'000,
                  std::chrono::milliseconds uptime = std::chrono::hours{24}) -> void {
        host h;
        h._report._node_id = id;
        h._report._uptime = uptime;
        h._report._labels = {std::move(label)};
        h._report._capacity_bytes = capacity;
        h._report._available_bytes = capacity;
        _hosts[id] = h;
    }

    auto kill_host(cap_node id) -> void { _hosts.at(id)._alive = false; }
    auto remove_host(cap_node id) -> void { _hosts.erase(id); }
    [[nodiscard]] auto has_host(cap_node id) const -> bool { return _hosts.contains(id); }

    auto add_shard(cap_group g, std::vector<cap_node> voters, std::size_t bytes = 1000,
                   double write_bps = 0.0) -> void {
        shard s;
        s._d._group_id = g;
        s._d._voters = std::move(voters);
        s._d._epoch = shard_epoch{._version = 1, ._conf_version = 1};
        s._leader = s._d._voters.front();
        s._bytes = bytes;
        s._write_bps = write_bps;
        s._initial_voters = s._d._voters.size();
        s._min_voters_seen = s._initial_voters;
        _shards[g] = s;
    }

    auto set_operation(cap_group g, shard_operation_state op) -> void { _shards.at(g)._op = op; }
    auto set_leader(cap_group g, cap_node n) -> void { _shards.at(g)._leader = n; }
    [[nodiscard]] auto shard_of(cap_group g) const -> const shard& { return _shards.at(g); }
    [[nodiscard]] auto shards() const -> const std::map<cap_group, shard>& { return _shards; }

    [[nodiscard]] auto node_reports() const -> std::vector<node_report<cap_node>> {
        std::vector<node_report<cap_node>> out;
        for (const auto& [id, h] : _hosts) {
            if (!h._alive) {
                continue;
            }
            auto r = h._report;
            r._shard_count = 0;
            r._leader_count = 0;
            for (const auto& [g, s] : _shards) {
                if (s._d.has_replica(id)) {
                    ++r._shard_count;
                    r._used_bytes += 0;
                }
                if (s._leader == id) {
                    ++r._leader_count;
                }
            }
            out.push_back(r);
        }
        return out;
    }

    [[nodiscard]] auto shard_reports() const -> std::vector<cap_shard_report> {
        std::vector<cap_shard_report> out;
        for (const auto& [g, s] : _shards) {
            auto lh = _hosts.find(s._leader);
            if (lh == _hosts.end() || !lh->second._alive) {
                continue;
            }
            cap_shard_report r;
            r._descriptor = s._d;
            r._leader = s._leader;
            r._operation = s._op;
            r._approximate_size_bytes = s._bytes;
            r._size_available = true;
            r._write_bytes_per_sec = s._write_bps;
            r._term = 1;
            for (const auto& v : s._d._voters) {
                auto h = _hosts.find(v);
                if (h == _hosts.end() || !h->second._alive) {
                    r._down_replicas.push_back(v);
                }
            }
            r._down_replica_count = r._down_replicas.size();
            for (const auto& [n, left] : s._catching_up) {
                if (left > 0) {
                    r._pending_replicas.push_back(n);
                }
            }
            out.push_back(std::move(r));
        }
        return out;
    }

    /// The host's `apply_operator`, for every operator, in order.
    auto apply(const std::vector<cap_operation>& ops) -> std::vector<operator_outcome> {
        std::vector<operator_outcome> out;
        for (const auto& op : ops) {
            out.push_back(apply_one(op));
        }
        return out;
    }

    /// Time passes: learners catch up.
    auto tick() -> void {
        for (auto& [g, s] : _shards) {
            for (auto& [n, left] : s._catching_up) {
                if (left > 0) {
                    --left;
                }
            }
        }
    }

    /// Fewest voters any shard ever had, relative to where it started.
    [[nodiscard]] auto quorum_ever_reduced() const -> bool {
        return std::any_of(_shards.begin(), _shards.end(), [](const auto& kv) {
            return kv.second._min_voters_seen < kv.second._initial_voters;
        });
    }

private:
    std::map<cap_node, host> _hosts;
    std::map<cap_group, shard> _shards;

    auto skip(const cap_operation& op, skipped_operator_reason r) -> operator_outcome {
        _skipped.emplace_back(op, r);
        return operator_outcome{
            ._operation_id = op.operation_id(), ._accepted = false, ._reason = r};
    }

    auto accept(const cap_operation& op) -> operator_outcome {
        _accepted.push_back(op);
        return operator_outcome{._operation_id = op.operation_id(), ._accepted = true};
    }

    auto apply_one(const cap_operation& op) -> operator_outcome {
        auto it = _shards.find(op.group_id());
        if (it == _shards.end()) {
            return skip(op, skipped_operator_reason::unknown_shard);
        }
        auto& s = it->second;
        if (auto f = _force_skip.find(op.group_id()); f != _force_skip.end()) {
            return skip(op, f->second);
        }
        if (s._op != shard_operation_state::stable) {
            _sent_while_busy.push_back(op);
            return skip(op, skipped_operator_reason::shard_busy);
        }
        if (op.epoch() != s._d._epoch) {
            return skip(op, skipped_operator_reason::stale_epoch);
        }
        auto& d = s._d;
        // Membership moves, the epoch does not: the real host copies the
        // leader's Raft membership into its descriptor without touching the
        // epoch (`multi_raft::sync_leader_membership`).
        const auto bump = [] {};
        const auto note = [&] {
            s._min_voters_seen = std::min(s._min_voters_seen, d._voters.size());
        };
        return std::visit(
            [&]<typename Op>(const Op& o) -> operator_outcome {
                if constexpr (std::same_as<Op, add_replica_operator<cap_node>>) {
                    const bool is_learner = std::find(d._learners.begin(), d._learners.end(),
                                                      o._node) != d._learners.end();
                    if (!o._as_learner && is_learner && !d.has_voter(o._node)) {
                        d._learners.erase(
                            std::find(d._learners.begin(), d._learners.end(), o._node));
                        d._voters.push_back(o._node);
                        bump();
                        note();
                        return accept(op);
                    }
                    if (d.has_replica(o._node)) {
                        return skip(op, skipped_operator_reason::precondition);
                    }
                    if (o._as_learner) {
                        d._learners.push_back(o._node);
                        s._catching_up[o._node] = _catch_up_steps;
                    } else {
                        d._voters.push_back(o._node);
                    }
                    bump();
                    note();
                    return accept(op);
                } else if constexpr (std::same_as<Op, remove_replica_operator<cap_node>>) {
                    if (!d.has_replica(o._node) || o._node == s._leader) {
                        return skip(op, skipped_operator_reason::precondition);
                    }
                    std::erase(d._voters, o._node);
                    std::erase(d._learners, o._node);
                    s._catching_up.erase(o._node);
                    bump();
                    note();
                    return accept(op);
                } else if constexpr (std::same_as<Op, transfer_leader_operator<cap_node>>) {
                    if (_transfer_unsupported) {
                        return skip(op, skipped_operator_reason::unsupported);
                    }
                    if (!d.has_voter(o._to) || o._to == s._leader) {
                        return skip(op, skipped_operator_reason::precondition);
                    }
                    s._leader = o._to;
                    return accept(op);
                } else {
                    return skip(op, skipped_operator_reason::unsupported);
                }
            },
            op.kind());
    }
};

}  // namespace kythira::testing
