// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file capacity_policy.hpp
/// @brief The decision layer of elastic shard capacity: the value types a
///        capacity decision is made from and expressed in, the
///        `capacity_policy` concept, and the default
///        `threshold_capacity_policy`.
///
/// See `.kiro/specs/elastic-shard-capacity/` design §3. Capacity is *machines*
/// — distinct from split/merge, which is *shards* — and it is expressed in the
/// placement-group vocabulary of `quorum_management.hpp`, never in provider
/// terms (Requirement 1.4). Nothing in this file knows what a cloud is.
///
/// ### Why the snapshot is built only from data already on the wire
///
/// Every input below is assembled from `node_report`, `shard_report`,
/// `quorum_health` and `desired_topology` — types that already exist and are
/// already sent. A capacity policy that needed its own telemetry path would
/// need its own failure story for it, and Requirement 2.2 forbids inventing one
/// unless a decision genuinely cannot be made without it.

#include <raft/quorum_management.hpp>
#include <raft/shard_placement_driver.hpp>
#include <raft/shard_types.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kythira {

// ─────────────────────────────────────────────────────────────────────────────
// Reasons and evidence
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Why a policy asked for a machine to be added or removed.
///
/// A metric dimension (`capacity.decision{reason}`) and the first field of the
/// decision's log record. "Why did this cluster grow?" starts here.
enum class capacity_reason : std::uint8_t {
    density = 0,         ///< Shards or leaders per node crossed the watermark.
    storage = 1,         ///< used/capacity crossed the watermark.
    load = 2,            ///< Write rate per node crossed the watermark.
    overload = 3,        ///< Too many hosts asserting `_overloaded`.
    split_pressure = 4,  ///< Split rate, projected density, or gate=capacity refusals.
    topology_floor = 5,  ///< Below `topology()`'s declared target.
    manual = 6,          ///< An operator asked.
};

[[nodiscard]] inline auto to_string(capacity_reason r) -> const char* {
    switch (r) {
        case capacity_reason::density:
            return "density";
        case capacity_reason::storage:
            return "storage";
        case capacity_reason::load:
            return "load";
        case capacity_reason::overload:
            return "overload";
        case capacity_reason::split_pressure:
            return "split_pressure";
        case capacity_reason::topology_floor:
            return "topology_floor";
        case capacity_reason::manual:
            return "manual";
        default:
            return "unknown";
    }
}

/// @brief One signal a policy looked at, with the threshold it compared against.
struct capacity_signal {
    std::string _name;
    double _value{0.0};
    double _threshold{0.0};

    [[nodiscard]] auto operator==(const capacity_signal&) const -> bool = default;
};

/// @brief The inputs and output of a split-pressure projection (design §3).
///
/// Recorded verbatim in the decision so an operator can see why a machine was
/// provisioned before any watermark was visibly crossed (Requirement 5.3).
struct capacity_projection {
    double _current_shards_per_node{0.0};
    /// Shard replicas added by splits per minute, over `_rate_window`.
    double _split_rate_per_minute{0.0};
    std::chrono::milliseconds _rate_window{};
    double _horizon_minutes{0.0};
    std::size_t _live_node_count{0};
    double _projected_shards_per_node{0.0};

    [[nodiscard]] auto operator==(const capacity_projection&) const -> bool = default;
};

/// @brief The subset of the snapshot a policy actually used.
///
/// Carried into the ledger and the log record, so a decision is explainable
/// from one line without joining it against a metrics store (Requirement 14.3).
struct capacity_evidence {
    std::vector<capacity_signal> _signals;
    std::optional<capacity_projection> _projection;
    /// `gate=capacity` split refusals seen in the window. Non-zero means the
    /// cluster is already out of room, which is not a forecast.
    std::size_t _capacity_refusals{0};
    /// Free text, for a custom policy's own explanation.
    std::string _note;

    [[nodiscard]] auto operator==(const capacity_evidence&) const -> bool = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// The decision
// ─────────────────────────────────────────────────────────────────────────────

enum class capacity_action : std::uint8_t {
    hold = 0,
    scale_out = 1,
    scale_in = 2,
};

[[nodiscard]] inline auto to_string(capacity_action a) -> const char* {
    switch (a) {
        case capacity_action::hold:
            return "hold";
        case capacity_action::scale_out:
            return "scale_out";
        case capacity_action::scale_in:
            return "scale_in";
        default:
            return "unknown";
    }
}

/// @brief Hold, add `N` machines (optionally in a named placement group), or
///        remove one named machine.
///
/// **Factory-only construction.** There is no public constructor beyond the
/// default (`hold`) and no public data, so an inconsistent decision — a
/// scale-out of zero machines, a scale-in naming nobody — cannot be built
/// rather than merely being documented as wrong (Requirement 3.4). The checks
/// that cannot be expressed in the type throw `std::invalid_argument` at the
/// factory, which is the one place a bad decision is created.
///
/// A scale-out may leave the placement group unnamed: choosing it is the
/// controller's job (Requirement 9.1, design §8), because only the controller
/// holds the refusal history that decides between equally under-filled
/// groups. A policy that names one is stating a constraint, which the
/// controller honours or refuses — never silently re-routes.
///
/// @tparam PlacementGroupId The quorum manager's placement-group id type.
/// @tparam NodeId           Node identifier type.
template<typename PlacementGroupId, typename NodeId> class capacity_decision {
public:
    /// @brief The default decision: do nothing.
    capacity_decision() = default;

    [[nodiscard]] static auto hold() -> capacity_decision { return capacity_decision{}; }

    /// @brief Add `count` machines; the controller chooses where.
    [[nodiscard]] static auto scale_out(std::size_t count, capacity_reason reason,
                                        capacity_evidence evidence) -> capacity_decision {
        if (count == 0) {
            throw std::invalid_argument("capacity_decision::scale_out: count must be at least 1");
        }
        capacity_decision d;
        d._action = capacity_action::scale_out;
        d._count = count;
        d._reason = reason;
        d._evidence = std::move(evidence);
        return d;
    }

    /// @brief Add `count` machines in `group`.
    [[nodiscard]] static auto scale_out_in(PlacementGroupId group, std::size_t count,
                                           capacity_reason reason, capacity_evidence evidence)
        -> capacity_decision {
        auto d = scale_out(count, reason, std::move(evidence));
        d._group = std::move(group);
        return d;
    }

    /// @brief Remove `node`, which lives in `group`.
    ///
    /// Names a machine, not a count: which machine leaves decides how much data
    /// moves, and that is a policy decision the controller should not guess at.
    [[nodiscard]] static auto scale_in(PlacementGroupId group, NodeId node, capacity_reason reason,
                                       capacity_evidence evidence) -> capacity_decision {
        capacity_decision d;
        d._action = capacity_action::scale_in;
        d._count = 1;
        d._group = std::move(group);
        d._node = std::move(node);
        d._reason = reason;
        d._evidence = std::move(evidence);
        return d;
    }

    [[nodiscard]] auto action() const -> capacity_action { return _action; }
    [[nodiscard]] auto is_hold() const -> bool { return _action == capacity_action::hold; }
    [[nodiscard]] auto is_scale_out() const -> bool {
        return _action == capacity_action::scale_out;
    }
    [[nodiscard]] auto is_scale_in() const -> bool { return _action == capacity_action::scale_in; }
    /// Machines to add or remove; zero only for `hold`.
    [[nodiscard]] auto count() const -> std::size_t { return _count; }
    [[nodiscard]] auto reason() const -> capacity_reason { return _reason; }
    /// The named placement group: always set for scale-in, optional for scale-out.
    [[nodiscard]] auto group() const -> const std::optional<PlacementGroupId>& { return _group; }
    /// The machine to remove; set exactly for scale-in.
    [[nodiscard]] auto node() const -> const std::optional<NodeId>& { return _node; }
    [[nodiscard]] auto evidence() const -> const capacity_evidence& { return _evidence; }

    [[nodiscard]] auto operator==(const capacity_decision&) const -> bool = default;

private:
    capacity_action _action{capacity_action::hold};
    std::size_t _count{0};
    capacity_reason _reason{capacity_reason::manual};
    std::optional<PlacementGroupId> _group{};
    std::optional<NodeId> _node{};
    capacity_evidence _evidence{};
};

// ─────────────────────────────────────────────────────────────────────────────
// The snapshot
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One machine as the controller last heard of it.
///
/// `_stale` is set when the report is older than `node_report_staleness`. A
/// stale machine is **unknown, not absent** (Requirement 2.4): it still counts
/// towards cluster size — it may well be running — but its numbers are not a
/// reason to act on.
template<typename NodeId, typename PlacementGroupId> struct node_capacity_view {
    node_report<NodeId> _report{};
    std::chrono::milliseconds _age{};
    bool _stale{false};
    /// The placement group the controller knows this machine by: the one it
    /// was provisioned into, else the first of its labels naming a group in
    /// `topology()`. Unset when neither says.
    std::optional<PlacementGroupId> _placement_group{};
    /// Being drained by a scale-in. Not a candidate for anything.
    bool _draining{false};

    [[nodiscard]] auto node_id() const -> const NodeId& { return _report._node_id; }
};

/// @brief One placement group: what `topology()` asks for and what is there.
template<typename PlacementGroupId> struct placement_group_view {
    PlacementGroupId _group{};
    /// The floor (`desired_topology`'s `target_count`).
    std::size_t _target{0};
    /// Live per the quorum manager when it reported per-group health, else the
    /// controller's count of fresh heartbeats in the group. Requirement 2.5:
    /// the manager's answer about liveness wins when the two disagree.
    std::size_t _live{0};
    /// Machines known in the group, fresh or stale.
    std::size_t _known{0};
};

/// @brief Everything a capacity policy may look at, assembled by the controller.
///
/// A value: the policy cannot reach anything through it, which is what makes
/// "no I/O, no mutation" enforceable by construction rather than by review.
///
/// @tparam NodeId           Node identifier type.
/// @tparam GroupId          The *shard* group id (`shard_report`'s).
/// @tparam Key              The routing key.
/// @tparam PlacementGroupId The quorum manager's placement-group id type.
template<typename NodeId, typename GroupId, typename Key, typename PlacementGroupId = std::string>
struct cluster_capacity_snapshot {
    using node_view_type = node_capacity_view<NodeId, PlacementGroupId>;
    using group_view_type = placement_group_view<PlacementGroupId>;
    using shard_report_type = shard_report<GroupId, Key, NodeId>;

    /// The controller's clock when the snapshot was built. Policies measure
    /// `sustained_for` against this rather than reading a clock of their own,
    /// so that a test's injected clock reaches them.
    std::chrono::system_clock::time_point _taken_at{};

    std::vector<node_view_type> _nodes;
    std::vector<shard_report_type> _shards;
    std::vector<group_view_type> _groups;
    desired_topology<PlacementGroupId> _topology{};
    /// The quorum manager's last assessment, if it has answered yet.
    std::optional<quorum_health<NodeId, PlacementGroupId>> _health{};

    /// Split/merge rate window (design §3).
    std::chrono::milliseconds _rate_window{};
    std::size_t _splits_in_window{0};
    std::size_t _merges_in_window{0};
    /// Shard replicas added by splits / removed by merges in the window. A
    /// split of a three-replica shard into two adds three replicas, which is
    /// the quantity "shards per node" is measured in.
    std::size_t _replicas_added_in_window{0};
    std::size_t _replicas_removed_in_window{0};
    /// `gate=capacity` refusals in the window, summed over shard reports.
    std::size_t _capacity_refusals{0};

    std::size_t _in_flight_intents{0};
    std::size_t _min_cluster_size{0};
    std::size_t _max_cluster_size{0};

    // ── derived views ────────────────────────────────────────────────────────

    /// Machines the cluster has, fresh or stale, excluding any being drained.
    [[nodiscard]] auto cluster_size() const -> std::size_t {
        return static_cast<std::size_t>(std::count_if(_nodes.begin(), _nodes.end(),
                                                      [](const auto& n) { return !n._draining; }));
    }

    /// Machines with a report fresher than `node_report_staleness`.
    [[nodiscard]] auto fresh_node_count() const -> std::size_t {
        return static_cast<std::size_t>(std::count_if(
            _nodes.begin(), _nodes.end(), [](const auto& n) { return !n._stale && !n._draining; }));
    }

    /// Mean of `f` over fresh, non-draining machines; `nullopt` when there are none.
    template<typename F> [[nodiscard]] auto fresh_mean(F f) const -> std::optional<double> {
        double sum = 0.0;
        std::size_t n = 0;
        for (const auto& v : _nodes) {
            if (v._stale || v._draining) {
                continue;
            }
            sum += static_cast<double>(f(v._report));
            ++n;
        }
        if (n == 0) {
            return std::nullopt;
        }
        return sum / static_cast<double>(n);
    }

    /// Maximum of `f` over fresh, non-draining machines.
    template<typename F> [[nodiscard]] auto fresh_max(F f) const -> std::optional<double> {
        std::optional<double> best;
        for (const auto& v : _nodes) {
            if (v._stale || v._draining) {
                continue;
            }
            const auto x = static_cast<double>(f(v._report));
            if (!best || x > *best) {
                best = x;
            }
        }
        return best;
    }

    [[nodiscard]] auto mean_shards_per_node() const -> std::optional<double> {
        return fresh_mean([](const auto& r) { return r._shard_count; });
    }
    [[nodiscard]] auto mean_leaders_per_node() const -> std::optional<double> {
        return fresh_mean([](const auto& r) { return r._leader_count; });
    }
    [[nodiscard]] auto mean_write_bytes_per_node() const -> std::optional<double> {
        return fresh_mean([](const auto& r) { return r._write_bytes_per_sec; });
    }

    /// Σused / Σcapacity over fresh machines that report a capacity at all.
    /// A machine whose probe reports nothing is not "empty" and is left out.
    [[nodiscard]] auto storage_utilisation() const -> std::optional<double> {
        std::uint64_t used = 0;
        std::uint64_t capacity = 0;
        for (const auto& v : _nodes) {
            if (v._stale || v._draining || v._report._capacity_bytes == 0) {
                continue;
            }
            used += v._report._used_bytes;
            capacity += v._report._capacity_bytes;
        }
        if (capacity == 0) {
            return std::nullopt;
        }
        return static_cast<double>(used) / static_cast<double>(capacity);
    }

    [[nodiscard]] auto overloaded_fraction() const -> std::optional<double> {
        return fresh_mean([](const auto& r) { return r._overloaded ? 1.0 : 0.0; });
    }

    [[nodiscard]] auto find_group(const PlacementGroupId& g) const -> const group_view_type* {
        for (const auto& v : _groups) {
            if (v._group == g) {
                return &v;
            }
        }
        return nullptr;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// The concept
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Decides whether the cluster needs a machine more, or one fewer.
///
/// The shape of `split_merge_policy`, because the constraints are the same:
///
/// **1. Consulted on one machine, and NOT required to be deterministic.**
/// A capacity policy runs inside the single controller that holds the lease,
/// and its answer is recorded in the ledger *before* anything acts on it. So a
/// policy may keep its own history (it must, to implement `sustained_for`),
/// sample, or change its mind — none of it can produce two different actions,
/// because only one party ever asks and the answer is written down first
/// (Requirement 3.3).
///
/// **2. No I/O, no mutation of cluster state, no blocking.** A policy receives
/// a value and returns a value (Requirement 3.2). Everything it needs is in the
/// snapshot; a policy that wanted to call a cloud API would be doing the
/// controller's job without the controller's bounds, deadlines or ledger.
///
/// **3. Validated once, at construction.** A controller refuses to start on a
/// policy whose `validate()` fails, and reports every error rather than the
/// first (Requirement 3.6) — a misconfigured pair of watermarks is the kind of
/// mistake whose failure mode is a fleet.
///
/// The controller never consults a policy more often than its
/// `evaluation_interval`, nor while it would refuse to act (lease lost, kill
/// switch, reconciliation pending), nor within `cooldown()` of the policy's
/// last non-hold decision (Requirement 3.5).
///
/// @tparam P                The policy type.
/// @tparam NodeId           Node identifier type.
/// @tparam GroupId          The shard group id type.
/// @tparam Key              The routing key.
/// @tparam PlacementGroupId The quorum manager's placement-group id type.
template<typename P, typename NodeId, typename GroupId, typename Key, typename PlacementGroupId>
concept capacity_policy = requires(
    P& p, const P& cp, const cluster_capacity_snapshot<NodeId, GroupId, Key, PlacementGroupId>& s) {
    { p.evaluate(s) } -> std::same_as<capacity_decision<PlacementGroupId, NodeId>>;
    { cp.cooldown() } -> std::same_as<std::chrono::milliseconds>;
    { cp.validate() } -> std::same_as<bool>;
    { cp.get_validation_errors() } -> std::same_as<std::vector<std::string>>;
};

// ─────────────────────────────────────────────────────────────────────────────
// The default policy
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A pair of watermarks for one signal.
///
/// Two thresholds, never one: a single threshold oscillates around itself,
/// adding a machine at 201 shards per node and removing it at 199. The gap
/// between `_high` and `_low` is what stops that, and `validate()` refuses a
/// gap too small to.
struct capacity_watermark {
    double _high{0.0};  ///< Scale out while the signal is above this.
    double _low{0.0};   ///< Scale in only while the signal is at or below this.
    bool _enabled{true};

    [[nodiscard]] auto operator==(const capacity_watermark&) const -> bool = default;
};

/// @brief Knobs for `threshold_capacity_policy`.
///
/// **The defaults describe what each knob means, not what an unconfigured
/// cluster does**: `_enabled` is false (Requirement 4.6). They are chosen so a
/// first adopter's failure mode is "did not scale out soon enough", never
/// "provisioned a fleet" (Requirement 4.7). Each one's derivation is in
/// `doc/elastic_shard_capacity.md`.
struct threshold_capacity_policy_config {
    /// The policy as a whole. Off: `evaluate` always holds.
    bool _enabled{false};

    /// Shards (replicas) per node. 200 out / 80 in: comfortably inside what a
    /// single host's tick budget handles, with the in-mark low enough that a
    /// cluster which just grew by a third does not immediately shrink.
    capacity_watermark _shards_per_node{._high = 200.0, ._low = 80.0};
    /// Leaders per node: the share of the cluster's write path one machine owns.
    capacity_watermark _leaders_per_node{._high = 80.0, ._low = 30.0};
    /// Σused / Σcapacity. 75% out leaves room for the snapshots a rebalance
    /// writes before it frees anything; 35% in.
    capacity_watermark _storage{._high = 0.75, ._low = 0.35};
    /// Write bytes per second per node. Disabled: no figure is right for every
    /// workload, and a wrong default here provisions machines for a benchmark.
    capacity_watermark _write_bytes_per_sec{._high = 0.0, ._low = 0.0, ._enabled = false};
    /// Fraction of machines asserting `_overloaded`. Over a third out; scale in
    /// only when nobody is.
    capacity_watermark _overloaded{._high = 0.34, ._low = 0.0};

    /// @brief The low watermark must be at most this fraction of the high.
    ///
    /// 0.6 by default. The same guard `threshold_split_merge_policy::validate()`
    /// applies to split and merge thresholds, adopted for the same reason: the
    /// cost of a misconfigured pair is unbounded.
    double _min_hysteresis_margin{0.6};

    /// @brief How long a signal must stay above its high watermark before a
    ///        machine is added. One heartbeat's spike must not provision.
    std::chrono::milliseconds _sustained_for{std::chrono::minutes{5}};
    /// @brief How long every signal must stay at or below its low watermark
    ///        before a machine is removed.
    ///
    /// Six times `_sustained_for`, and the asymmetry is the policy's central
    /// trade: a machine added late costs latency, a machine removed early
    /// costs a re-provision plus two snapshot transfers per shard moved twice.
    std::chrono::milliseconds _sustained_for_scale_in{std::chrono::minutes{30}};

    /// @brief Scale out on *projected* density (Requirement 5.2).
    bool _split_pressure_enabled{true};
    /// The projection horizon. 30 minutes comfortably exceeds the p99
    /// provisioning time of every shipped manager, which is the only thing the
    /// horizon has to beat.
    double _horizon_minutes{30.0};

    /// @brief Bring a placement group up to `topology()`'s floor.
    ///
    /// Off by default even when the policy is on: a deployment that already
    /// runs the single-group quorum loop, or a cloud autoscaler, repairs the
    /// floor itself, and two repairers fight.
    bool _repair_topology_floor{false};

    /// Machines one scale-out asks for. The controller's own bounds still
    /// apply on top.
    std::size_t _scale_out_step{1};

    /// Minimum time between this policy's own non-hold decisions.
    std::chrono::milliseconds _cooldown{std::chrono::minutes{10}};
};

/// @brief The default policy: five signals with paired watermarks, sustained
///        crossings, and split-pressure projection.
///
/// ### What "sustained" means here
///
/// Each signal remembers when it was first seen continuously above its high
/// watermark (and, separately, at or below its low one). A crossing becomes a
/// decision only once the remembered instant is `_sustained_for` behind the
/// snapshot's `_taken_at`. A single sample on the other side resets it. So a
/// spike that lasts one heartbeat changes nothing, and a plateau that lasts
/// five minutes provisions exactly one machine (the cooldown and the
/// controller's in-flight bound see to "exactly one").
///
/// ### What it will not do
///
/// It does not decide on stale data: only machines with a fresh report count
/// towards the averages, and with none fresh it holds. It does not scale in
/// below `_min_cluster_size` or the topology floor, nor while any split is
/// producing more replicas than merges are removing (Requirement 5.6: merges
/// feed scale-in only). It does not scale in if removing a machine would push
/// mean shards per node over the high watermark — which would be a scale-in
/// that schedules its own scale-out.
template<typename NodeId, typename GroupId, typename Key, typename PlacementGroupId = std::string>
class threshold_capacity_policy {
public:
    using config_type = threshold_capacity_policy_config;
    using snapshot_type = cluster_capacity_snapshot<NodeId, GroupId, Key, PlacementGroupId>;
    using decision_type = capacity_decision<PlacementGroupId, NodeId>;
    using time_point = std::chrono::system_clock::time_point;

    threshold_capacity_policy() = default;
    explicit threshold_capacity_policy(config_type cfg) : _cfg(std::move(cfg)) {}

    [[nodiscard]] auto config() const -> const config_type& { return _cfg; }

    [[nodiscard]] auto evaluate(const snapshot_type& s) -> decision_type {
        if (!_cfg._enabled) {
            return decision_type::hold();
        }
        const auto now = s._taken_at;

        // Every sample updates the crossing clocks, inside the cooldown too:
        // "sustained" means continuously observed, not observed since the last
        // decision.
        const auto signals = measure(s);
        for (const auto& m : signals) {
            track(m, now);
        }

        if (_last_decision.has_value() && now - *_last_decision < _cfg._cooldown) {
            return decision_type::hold();
        }

        auto decision = decide(s, signals, now);
        if (!decision.is_hold()) {
            _last_decision = now;
            // A decision consumes the crossing that produced it. Without this
            // the next evaluation after the cooldown would act on the same
            // five minutes again, before the machine it asked for has had any
            // chance to change the numbers.
            _crossing.clear();
        }
        return decision;
    }

    [[nodiscard]] auto cooldown() const -> std::chrono::milliseconds { return _cfg._cooldown; }

    [[nodiscard]] auto validate() const -> bool { return get_validation_errors().empty(); }

    /// @brief Every error, not the first (Requirement 3.6).
    [[nodiscard]] auto get_validation_errors() const -> std::vector<std::string> {
        std::vector<std::string> errors;
        if (!(_cfg._min_hysteresis_margin > 0.0 && _cfg._min_hysteresis_margin < 1.0)) {
            errors.push_back("_min_hysteresis_margin (" +
                             std::to_string(_cfg._min_hysteresis_margin) +
                             ") must be in (0, 1): at 1 the two watermarks may coincide, which "
                             "is a single threshold that oscillates around itself");
        }
        const auto check = [&](const char* name, const capacity_watermark& w) {
            if (!w._enabled) {
                return;
            }
            if (w._high <= 0.0) {
                errors.push_back(std::string{name} + "._high (" + std::to_string(w._high) +
                                 ") must be positive");
            }
            if (w._low < 0.0) {
                errors.push_back(std::string{name} + "._low (" + std::to_string(w._low) +
                                 ") must not be negative");
            }
            if (w._low >= w._high) {
                errors.push_back(std::string{name} + " is inverted: _low (" +
                                 std::to_string(w._low) + ") is not below _high (" +
                                 std::to_string(w._high) +
                                 "), so a cluster can be asked to grow and shrink on the same "
                                 "reading");
            } else if (w._low > w._high * _cfg._min_hysteresis_margin) {
                errors.push_back(
                    std::string{name} + " is not hysteretic: _low (" + std::to_string(w._low) +
                    ") must be at most _min_hysteresis_margin * _high (" +
                    std::to_string(w._high * _cfg._min_hysteresis_margin) +
                    "): adding one machine moves the signal by about 1/N, and a gap smaller "
                    "than that scales out, finds itself under the in-mark, scales back in, "
                    "and is over the out-mark again — oscillating forever, two snapshot "
                    "transfers per shard per cycle");
            }
        };
        check("_shards_per_node", _cfg._shards_per_node);
        check("_leaders_per_node", _cfg._leaders_per_node);
        check("_storage", _cfg._storage);
        check("_write_bytes_per_sec", _cfg._write_bytes_per_sec);
        // The overloaded fraction is the one signal whose natural low is zero
        // ("scale in only when nobody is overloaded"), so it is allowed to be.
        if (_cfg._overloaded._enabled) {
            if (_cfg._overloaded._high <= 0.0 || _cfg._overloaded._high > 1.0) {
                errors.push_back("_overloaded._high must be in (0, 1]: it is a fraction of nodes");
            }
            if (_cfg._overloaded._low < 0.0 ||
                _cfg._overloaded._low > _cfg._overloaded._high * _cfg._min_hysteresis_margin) {
                errors.push_back(
                    "_overloaded is not hysteretic: _low must be in [0, "
                    "_min_hysteresis_margin * _high]");
            }
        }
        if (_cfg._storage._enabled && _cfg._storage._high > 1.0) {
            errors.push_back("_storage._high must be at most 1: it is a utilisation ratio");
        }
        if (_cfg._sustained_for <= std::chrono::milliseconds::zero()) {
            errors.push_back(
                "_sustained_for must be positive: zero means a single heartbeat's spike "
                "provisions a machine");
        }
        if (_cfg._sustained_for_scale_in < _cfg._sustained_for) {
            errors.push_back(
                "_sustained_for_scale_in must be at least _sustained_for: removing a machine "
                "early costs a re-provision and two snapshot transfers per moved shard, adding "
                "one late costs only latency");
        }
        if (_cfg._split_pressure_enabled && !(_cfg._horizon_minutes > 0.0)) {
            errors.push_back("_horizon_minutes must be positive when split pressure is enabled");
        }
        if (_cfg._split_pressure_enabled && !_cfg._shards_per_node._enabled) {
            errors.push_back(
                "split pressure projects shards per node, so it needs _shards_per_node "
                "enabled to have a watermark to project against");
        }
        if (_cfg._scale_out_step == 0) {
            errors.push_back("_scale_out_step must be at least 1");
        }
        return errors;
    }

    /// @brief The projection of design §3, exposed for its own tests.
    ///
    /// `current + rate_per_minute * horizon / live_nodes`, where the rate is
    /// in shard *replicas* (the unit shards-per-node is measured in) added by
    /// splits over the snapshot's rate window. Merges are deliberately not
    /// netted off: Requirement 5.6 makes them a scale-in input only.
    [[nodiscard]] auto project(const snapshot_type& s) const -> std::optional<capacity_projection> {
        const auto current = s.mean_shards_per_node();
        const auto live = s.fresh_node_count();
        if (!current || live == 0 || s._rate_window <= std::chrono::milliseconds::zero()) {
            return std::nullopt;
        }
        const double window_minutes =
            std::chrono::duration<double, std::ratio<60>>(s._rate_window).count();
        const double rate = static_cast<double>(s._replicas_added_in_window) / window_minutes;
        capacity_projection p;
        p._current_shards_per_node = *current;
        p._split_rate_per_minute = rate;
        p._rate_window = s._rate_window;
        p._horizon_minutes = _cfg._horizon_minutes;
        p._live_node_count = live;
        p._projected_shards_per_node =
            *current + rate * _cfg._horizon_minutes / static_cast<double>(live);
        return p;
    }

private:
    /// One measured signal for this evaluation.
    struct measurement {
        const char* _name;
        capacity_reason _reason;
        std::optional<double> _value;
        const capacity_watermark* _mark;
    };

    /// When a signal was first seen continuously across a watermark.
    struct crossing {
        std::optional<time_point> _above_since;
        std::optional<time_point> _below_since;
    };

    [[nodiscard]] auto measure(const snapshot_type& s) const -> std::vector<measurement> {
        return {
            {"shards_per_node", capacity_reason::density, s.mean_shards_per_node(),
             &_cfg._shards_per_node},
            {"leaders_per_node", capacity_reason::density, s.mean_leaders_per_node(),
             &_cfg._leaders_per_node},
            {"storage_utilisation", capacity_reason::storage, s.storage_utilisation(),
             &_cfg._storage},
            {"write_bytes_per_sec_per_node", capacity_reason::load, s.mean_write_bytes_per_node(),
             &_cfg._write_bytes_per_sec},
            {"overloaded_fraction", capacity_reason::overload, s.overloaded_fraction(),
             &_cfg._overloaded},
        };
    }

    auto track(const measurement& m, time_point now) -> void {
        auto& c = _crossing[m._name];
        if (!m._mark->_enabled || !m._value.has_value()) {
            // No reading is not a reading on either side: forget both. A signal
            // that cannot be measured must not keep an old crossing alive.
            c = {};
            return;
        }
        const double v = *m._value;
        if (v > m._mark->_high) {
            if (!c._above_since) {
                c._above_since = now;
            }
        } else {
            c._above_since.reset();
        }
        if (v <= m._mark->_low) {
            if (!c._below_since) {
                c._below_since = now;
            }
        } else {
            c._below_since.reset();
        }
    }

    [[nodiscard]] auto sustained_above(const char* name, time_point now) const -> bool {
        auto it = _crossing.find(name);
        return it != _crossing.end() && it->second._above_since.has_value() &&
               now - *it->second._above_since >= _cfg._sustained_for;
    }

    [[nodiscard]] auto sustained_below(const char* name, time_point now) const -> bool {
        auto it = _crossing.find(name);
        return it != _crossing.end() && it->second._below_since.has_value() &&
               now - *it->second._below_since >= _cfg._sustained_for_scale_in;
    }

    [[nodiscard]] static auto signals_of(const std::vector<measurement>& ms)
        -> std::vector<capacity_signal> {
        std::vector<capacity_signal> out;
        for (const auto& m : ms) {
            if (m._mark->_enabled && m._value.has_value()) {
                out.push_back(capacity_signal{
                    ._name = m._name, ._value = *m._value, ._threshold = m._mark->_high});
            }
        }
        return out;
    }

    [[nodiscard]] auto decide(const snapshot_type& s, const std::vector<measurement>& ms,
                              time_point now) -> decision_type {
        // Nothing fresh: the policy knows nothing, and "nothing" is not a
        // reason to provision (Requirement 2.4).
        if (s.fresh_node_count() == 0) {
            return decision_type::hold();
        }

        // ── the floor, if asked to repair it ────────────────────────────────
        if (_cfg._repair_topology_floor) {
            const typename snapshot_type::group_view_type* worst = nullptr;
            for (const auto& g : s._groups) {
                if (g._live < g._target &&
                    (worst == nullptr || g._target - g._live > worst->_target - worst->_live)) {
                    worst = &g;
                }
            }
            if (worst != nullptr) {
                capacity_evidence ev;
                ev._signals.push_back(
                    capacity_signal{._name = "group_live",
                                    ._value = static_cast<double>(worst->_live),
                                    ._threshold = static_cast<double>(worst->_target)});
                return decision_type::scale_out_in(worst->_group, 1,
                                                   capacity_reason::topology_floor, std::move(ev));
            }
        }

        const auto at_ceiling = s._max_cluster_size != 0 && s.cluster_size() >= s._max_cluster_size;

        // ── split pressure: refusals first, then the projection ──────────────
        if (_cfg._split_pressure_enabled && !at_ceiling) {
            if (s._capacity_refusals > 0) {
                // Already out of room: not a forecast, so not held to
                // `sustained_for` either (design §3).
                capacity_evidence ev;
                ev._signals = signals_of(ms);
                ev._capacity_refusals = s._capacity_refusals;
                ev._projection = project(s);
                return decision_type::scale_out(_cfg._scale_out_step,
                                                capacity_reason::split_pressure, std::move(ev));
            }
            if (const auto p = project(s); p.has_value()) {
                const bool already_over = p->_current_shards_per_node > _cfg._shards_per_node._high;
                if (p->_projected_shards_per_node > _cfg._shards_per_node._high && !already_over) {
                    // The projection still has to be sustained: one burst of
                    // splits in an otherwise quiet window is a spike, too.
                    auto& c = _projection_crossing;
                    if (!c) {
                        c = now;
                    }
                    if (now - *c >= _cfg._sustained_for) {
                        capacity_evidence ev;
                        ev._signals = signals_of(ms);
                        ev._projection = p;
                        return decision_type::scale_out(
                            _cfg._scale_out_step, capacity_reason::split_pressure, std::move(ev));
                    }
                } else {
                    _projection_crossing.reset();
                }
            }
        }

        // ── observed watermarks ─────────────────────────────────────────────
        if (!at_ceiling) {
            for (const auto& m : ms) {
                if (m._mark->_enabled && sustained_above(m._name, now)) {
                    capacity_evidence ev;
                    ev._signals = signals_of(ms);
                    return decision_type::scale_out(_cfg._scale_out_step, m._reason, std::move(ev));
                }
            }
        }

        return maybe_scale_in(s, ms, now);
    }

    [[nodiscard]] auto maybe_scale_in(const snapshot_type& s, const std::vector<measurement>& ms,
                                      time_point now) const -> decision_type {
        // Every enabled, measurable signal must have been low for the long
        // window. One signal still high is a reason to keep the machine.
        bool any = false;
        for (const auto& m : ms) {
            if (!m._mark->_enabled || !m._value.has_value()) {
                continue;
            }
            any = true;
            if (!sustained_below(m._name, now)) {
                return decision_type::hold();
            }
        }
        if (!any) {
            return decision_type::hold();
        }
        // Splits outpacing merges means shards are still arriving.
        if (s._replicas_added_in_window > s._replicas_removed_in_window) {
            return decision_type::hold();
        }
        // Every stale machine is a machine whose numbers are unknown; a
        // scale-in that is wrong about one of them is the expensive kind.
        if (s.fresh_node_count() != s.cluster_size()) {
            return decision_type::hold();
        }
        const auto size = s.cluster_size();
        const auto floor = std::max(s._min_cluster_size, s._topology.total_size());
        if (size <= floor || size <= 1) {
            return decision_type::hold();
        }
        // Removing a machine raises density by size/(size-1). If that lands
        // over the out-mark, this scale-in schedules its own scale-out.
        if (_cfg._shards_per_node._enabled) {
            if (const auto mean = s.mean_shards_per_node(); mean.has_value()) {
                const auto after =
                    *mean * static_cast<double>(size) / static_cast<double>(size - 1);
                if (after > _cfg._shards_per_node._high) {
                    return decision_type::hold();
                }
            }
        }

        // Which machine: one in a group above its floor, holding the fewest
        // shards (the least data moves), tie-broken on node id.
        const node_capacity_view<NodeId, PlacementGroupId>* pick = nullptr;
        for (const auto& v : s._nodes) {
            if (v._draining || v._stale || !v._placement_group.has_value()) {
                continue;
            }
            const auto* g = s.find_group(*v._placement_group);
            if (g == nullptr || g->_known <= g->_target) {
                continue;
            }
            if (pick == nullptr || v._report._shard_count < pick->_report._shard_count ||
                (v._report._shard_count == pick->_report._shard_count &&
                 v.node_id() < pick->node_id())) {
                pick = &v;
            }
        }
        if (pick == nullptr) {
            return decision_type::hold();
        }
        capacity_evidence ev;
        ev._signals = signals_of(ms);
        // The reason is the signal that is lowest relative to its in-mark: the
        // one that best explains "this cluster is too big".
        auto reason = capacity_reason::density;
        double lowest = 2.0;
        for (const auto& m : ms) {
            if (m._mark->_enabled && m._value.has_value() && m._mark->_low > 0.0) {
                const auto ratio = *m._value / m._mark->_low;
                if (ratio < lowest) {
                    lowest = ratio;
                    reason = m._reason;
                }
            }
        }
        return decision_type::scale_in(*pick->_placement_group, pick->node_id(), reason,
                                       std::move(ev));
    }

    /// Insertion-ordered and tiny (five signals): a vector beats a map here,
    /// and keeps evaluation order — and so reason precedence — stable.
    struct crossing_map {
        std::vector<std::pair<std::string, crossing>> _v;
        auto operator[](const std::string& k) -> crossing& {
            for (auto& [name, c] : _v) {
                if (name == k) {
                    return c;
                }
            }
            _v.emplace_back(k, crossing{});
            return _v.back().second;
        }
        [[nodiscard]] auto find(const std::string& k) const ->
            typename std::vector<std::pair<std::string, crossing>>::const_iterator {
            return std::find_if(_v.begin(), _v.end(), [&](const auto& p) { return p.first == k; });
        }
        [[nodiscard]] auto end() const ->
            typename std::vector<std::pair<std::string, crossing>>::const_iterator {
            return _v.end();
        }
        auto clear() -> void { _v.clear(); }
    };
    config_type _cfg{};
    crossing_map _crossing;
    std::optional<time_point> _projection_crossing;
    std::optional<time_point> _last_decision;
};

static_assert(capacity_policy<threshold_capacity_policy<std::uint64_t, std::uint64_t, std::string>,
                              std::uint64_t, std::uint64_t, std::string, std::string>);

}  // namespace kythira
