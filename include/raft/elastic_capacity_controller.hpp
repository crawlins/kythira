// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file elastic_capacity_controller.hpp
/// @brief The control loop of elastic shard capacity: it turns a capacity
///        policy's decisions into machines, and machines into shard placement.
///
/// See `.kiro/specs/elastic-shard-capacity/` design §4, §7, §8 and §10-§11.
///
/// ### What it owns, and what it does not
///
/// It owns the *machines*: when to add one, where, how to admit it to shards,
/// and when to drain and remove one. It does not own shard identity or
/// split/merge: those stay with the host and its split/merge policy. It reaches
/// a cloud only through something satisfying `quorum_manager` — no provider
/// header is reachable from this file (Requirement 1.1).
///
/// ### The four rules that make it safe to run
///
/// 1. **Nothing it calls can block it.** `step()` is called on a heartbeat
///    path. Every provider call is *initiated* on the controller's own
///    executor, which deposits the returned future where the next `step()`
///    finds it; `step()` only ever polls `isReady()` (Requirement 13.1). A
///    provider having a bad day costs a deadline, never a tick.
/// 2. **Record before act.** An intent is durably in the ledger before the
///    provider call that enacts it, and every provider call needs a token only
///    a committed ledger record can mint. A controller that dies between the
///    two leaves an intent that reconciliation resolves.
/// 3. **Lease every step.** No step acts without the lease, and a result
///    observed after the lease was lost is discarded (Requirement 7.3).
/// 4. **Bounds outside the policy.** Cluster ceiling, in-flight intents, the
///    provider-call interval, the rolling budget, the circuit: all enforced
///    here, each refusal counted and logged by name, so a policy bug cannot
///    buy a fleet (Requirement 12.1-12.3).
///
/// ### Threading
///
/// Every public method takes one internal mutex, so the controller may be fed
/// by several hosts' heartbeat threads at once. Provider work is handed to the
/// executor *after* the mutex is released, so an executor that runs work
/// inline cannot deadlock it — though it does put the provider call on the
/// caller's thread, which defeats rule 1; a deployment supplies a real one.

#include <raft/capacity_lease.hpp>
#include <raft/capacity_ledger.hpp>
#include <raft/capacity_policy.hpp>
#include <raft/logger.hpp>
#include <raft/metrics.hpp>
#include <raft/peer_discovery.hpp>
#include <raft/quorum_management.hpp>
#include <raft/shard_placement_driver.hpp>
#include <raft/shard_types.hpp>
#include <raft/types.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

// ─────────────────────────────────────────────────────────────────────────────
// Optional quorum-manager refinements
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A manager that can tag what it creates with the controller's
///        idempotency key, and find a machine by it.
///
/// Detected, never required (Requirement 1.3). The docker manager and the
/// instance-level cloud managers (`aws_ec2`, `azure_vm`, `gcp_compute`)
/// implement it, attaching the key in their create call. The group-capacity
/// managers cannot -- the provider launches their instances, so there is no
/// create call to attach a key to atomically. Where it is absent,
/// reconciliation matches on node id and join deadline instead, and that
/// degradation is documented per provider (design §7, §15 item 7).
template<typename Q>
concept keyed_quorum_manager =
    requires(Q& q, typename Q::placement_group_id_type group,
             std::optional<typename Q::node_id_type> replacing, const std::string& key) {
        q.provision_node_keyed(group, replacing, key);
        q.find_by_idempotency_key(key);
    };

/// @brief A group-capacity manager (an ASG, a MIG, a VMSS) whose own notion of
///        a group's size can be kept in step with the controller's.
template<typename Q>
concept resizable_quorum_manager =
    requires(Q& q, typename Q::placement_group_id_type group, std::size_t count) {
        q.set_group_target(group, count);
    };

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Every knob of design §10.
///
/// **The defaults are chosen so that enabling the feature and configuring
/// nothing else cannot exceed three times the starting size** (Requirement
/// 12.6): one machine at a time, at most four an hour, never more than 3× the
/// topology's floor, and dry run on. Their derivations are in
/// `doc/elastic_shard_capacity.md`.
struct elastic_capacity_config {
    using ms = std::chrono::milliseconds;

    /// The feature, whole. Off: `step()` returns nothing and calls nothing.
    bool enabled{false};
    /// Scale-in, independently of scale-out (Requirement 11.6).
    bool scale_in_enabled{false};
    /// Decide, log and count; call no provider, write no ledger, issue no
    /// operator (Requirement 12.5). On by default: dry run is the documented
    /// first step of adoption.
    bool dry_run{true};
    /// Stop all provisioning and decommissioning, and every operator, from
    /// the next step on (Requirement 12.4). Also settable at run time.
    bool kill_switch{false};

    /// The policy is consulted at most this often — and immediately on a
    /// split, which is a trigger edge (Requirement 5.1).
    ms evaluation_interval{std::chrono::seconds{60}};

    /// Hard bounds on cluster size. Zero means "derive": the floor from
    /// `topology().total_size()`, the ceiling as three times that floor (or
    /// three times the first observed size when the topology declares none).
    std::size_t min_cluster_size{0};
    std::size_t max_cluster_size{0};

    /// Machines being added or removed at once.
    std::size_t max_in_flight_intents{1};
    /// Minimum gap between two capacity-changing provider calls, whatever the
    /// policy says.
    ms min_provider_call_interval{std::chrono::minutes{5}};
    /// The budget guard: at most this many provisioning calls per window.
    std::size_t max_provisions_per_window{4};
    ms provision_window{std::chrono::hours{1}};

    /// Deadlines (design §4 rule 3). Expiry always has a next state.
    ms provision_deadline{std::chrono::minutes{10}};
    ms join_deadline{std::chrono::minutes{15}};
    ms admit_deadline{std::chrono::minutes{30}};
    ms drain_deadline{std::chrono::minutes{60}};
    /// How long reconciliation may hold up deciding after the lease is
    /// acquired (Requirement 8.3).
    ms reconcile_deadline{std::chrono::minutes{2}};

    /// Snapshot transfers at once, per receiving machine and cluster-wide
    /// (Requirement 10.4).
    std::size_t max_moves_per_target{2};
    std::size_t max_moves_cluster_wide{8};
    /// How long before an unacknowledged move step is sent again.
    ms operator_retry_interval{std::chrono::seconds{30}};
    /// Back-off after a host reports `shard_busy` (Requirement 13.6).
    ms operator_busy_backoff{std::chrono::minutes{2}};
    /// A shard is not moved within this long of a split or merge touching it
    /// (Requirement 10.5).
    ms move_cooldown_after_split{std::chrono::minutes{5}};

    /// A report older than this is *unknown*, not absent (Requirement 2.4).
    /// Three heartbeat intervals of the default host configuration.
    ms node_report_staleness{std::chrono::seconds{30}};
    /// The split/merge rate window the snapshot reports.
    ms split_rate_window{std::chrono::minutes{10}};
    /// How often `assess_quorum` is asked for the provider's view.
    ms assess_interval{std::chrono::seconds{60}};

    /// Terminal intents older than this are compacted away (Requirement 8.8).
    /// Must exceed the longest deadline by at least `retention_margin`×.
    ms ledger_retention{std::chrono::hours{24 * 7}};

    /// Provider-failure back-off, reusing the repository's own retry
    /// configuration and formula (Requirement 13.3). `max_attempts` is the
    /// number of consecutive failures that opens the circuit.
    retry_policy_config provider_backoff{.initial_delay = std::chrono::minutes{1},
                                         .max_delay = std::chrono::minutes{30},
                                         .backoff_multiplier = 2.0,
                                         .jitter_factor = 0.2,
                                         .max_attempts = 3};
    /// How long an open circuit suspends scale-out before one trial call.
    ms circuit_cool_off{std::chrono::minutes{30}};

    /// A placement group that refused a call is ranked down by this much,
    /// decaying with `placement_refusal_decay` (design §8).
    double placement_refusal_penalty{2.0};
    ms placement_refusal_decay{std::chrono::minutes{30}};

    /// Seed for the back-off jitter, so a test is reproducible. Unset: random.
    std::optional<std::uint32_t> jitter_seed{};

    /// The retention-over-deadline margin `validate()` enforces.
    static constexpr std::size_t retention_margin = 2;

    [[nodiscard]] auto get_validation_errors() const -> std::vector<std::string> {
        std::vector<std::string> e;
        const auto positive = [&](ms v, const char* name) {
            if (v <= ms::zero()) {
                e.emplace_back(std::string(name) + " must be positive");
            }
        };
        positive(evaluation_interval, "evaluation_interval");
        positive(provision_deadline, "provision_deadline");
        positive(join_deadline, "join_deadline");
        positive(admit_deadline, "admit_deadline");
        positive(drain_deadline, "drain_deadline");
        positive(reconcile_deadline, "reconcile_deadline");
        positive(node_report_staleness, "node_report_staleness");
        positive(split_rate_window, "split_rate_window");
        positive(assess_interval, "assess_interval");
        positive(operator_retry_interval, "operator_retry_interval");
        if (min_provider_call_interval < ms::zero()) {
            e.emplace_back("min_provider_call_interval must not be negative");
        }
        if (max_cluster_size != 0 && max_cluster_size < min_cluster_size) {
            e.emplace_back("max_cluster_size must be at least min_cluster_size");
        }
        if (max_in_flight_intents == 0) {
            e.emplace_back("max_in_flight_intents must be at least 1");
        }
        if (max_provisions_per_window == 0) {
            e.emplace_back("max_provisions_per_window must be at least 1");
        }
        positive(provision_window, "provision_window");
        if (max_moves_per_target == 0 || max_moves_cluster_wide == 0) {
            e.emplace_back("move caps must be at least 1");
        }
        if (!provider_backoff.is_valid()) {
            e.emplace_back("provider_backoff is not a valid retry_policy_config");
        }
        const auto longest = std::max({provision_deadline, join_deadline + provision_deadline,
                                       admit_deadline, drain_deadline});
        if (ledger_retention < longest * static_cast<long>(retention_margin)) {
            e.emplace_back("ledger_retention must be at least " + std::to_string(retention_margin) +
                           "x the longest deadline");
        }
        if (placement_refusal_penalty < 0.0) {
            e.emplace_back("placement_refusal_penalty must not be negative");
        }
        return e;
    }

    [[nodiscard]] auto validate() const -> bool { return get_validation_errors().empty(); }
};

/// @brief A logger that says nothing; the controller's default.
struct null_capacity_logger {
    auto log(log_level, std::string_view) -> void {}
    auto log(log_level, std::string_view,
             const std::vector<std::pair<std::string_view, std::string_view>>&) -> void {}
    auto trace(std::string_view) -> void {}
    auto debug(std::string_view) -> void {}
    auto info(std::string_view) -> void {}
    auto warning(std::string_view) -> void {}
    auto error(std::string_view) -> void {}
    auto critical(std::string_view) -> void {}
};

static_assert(diagnostic_logger<null_capacity_logger>);

namespace detail::capacity {

template<typename T> auto to_text(const T& v) -> std::string {
    if constexpr (std::convertible_to<T, std::string>) {
        return std::string(v);
    } else if constexpr (std::integral<T>) {
        return std::to_string(v);
    } else if constexpr (requires(std::ostream& os) { os << v; }) {
        std::ostringstream os;
        os << v;
        return os.str();
    } else {
        return "?";
    }
}

template<typename T> auto sorted(std::vector<T> v) -> std::vector<T> {
    std::sort(v.begin(), v.end());
    return v;
}

template<typename T> auto contains(const std::vector<T>& v, const T& x) -> bool {
    return std::find(v.begin(), v.end(), x) != v.end();
}

/// The formula of `error_handler::calculate_delay`, which is private: the
/// same initial delay, multiplier, cap and symmetric jitter, so a provider
/// back-off reads exactly like every other retry in the repository.
inline auto backoff_delay(const retry_policy_config& p, std::size_t attempt, std::mt19937& rng)
    -> std::chrono::milliseconds {
    auto base = p.initial_delay;
    for (std::size_t i = 1; i < attempt; ++i) {
        base = std::chrono::milliseconds{
            static_cast<long long>(static_cast<double>(base.count()) * p.backoff_multiplier)};
        if (base >= p.max_delay) {
            break;
        }
    }
    base = std::min(base, p.max_delay);
    if (p.jitter_factor > 0.0) {
        std::uniform_real_distribution<double> d(-p.jitter_factor, p.jitter_factor);
        base += std::chrono::milliseconds{
            static_cast<long long>(static_cast<double>(base.count()) * d(rng))};
    }
    return std::max(base, std::chrono::milliseconds{1});
}

}  // namespace detail::capacity

// ─────────────────────────────────────────────────────────────────────────────
// The controller
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The elastic capacity controller of design §4.
///
/// @tparam QuorumMgr The provider, through `quorum_manager`. Its node, address
///                   and placement-group types are the controller's.
/// @tparam Policy    A `capacity_policy` over the same types.
/// @tparam Ledger    A `capacity_ledger`; held by reference, so the caller
///                   chooses its lifetime (a replicated ledger outlives any one
///                   controller by design).
/// @tparam Lease     A `capacity_lease`.
/// @tparam GroupId   The shard group id of the hosts it drives.
/// @tparam Key       The shard routing key.
/// @tparam Metrics   A `metrics` sink, under `kythira.multiraft.capacity.*`.
/// @tparam Logger    A `diagnostic_logger`.
template<typename QuorumMgr, typename Policy, typename Ledger, typename Lease,
         typename GroupId = std::uint64_t, typename Key = std::string,
         typename Metrics = noop_metrics, typename Logger = null_capacity_logger>
requires quorum_manager<QuorumMgr, typename QuorumMgr::node_id_type,
                        typename QuorumMgr::address_type,
                        typename QuorumMgr::placement_group_id_type> &&
         capacity_policy<Policy, typename QuorumMgr::node_id_type, GroupId, Key,
                         typename QuorumMgr::placement_group_id_type> &&
         capacity_ledger<Ledger, typename QuorumMgr::node_id_type,
                         typename QuorumMgr::placement_group_id_type> &&
         capacity_lease<Lease> && metrics<Metrics> && diagnostic_logger<Logger>
class elastic_capacity_controller {
public:
    using node_id_type = typename QuorumMgr::node_id_type;
    using address_type = typename QuorumMgr::address_type;
    using placement_group_type = typename QuorumMgr::placement_group_id_type;
    using group_id_type = GroupId;
    using key_type = Key;
    using time_point = std::chrono::system_clock::time_point;
    using clock_fn = std::function<time_point()>;
    using executor_fn = std::function<void(std::function<void()>)>;
    using descriptor_type = shard_descriptor<GroupId, Key, node_id_type>;
    using shard_report_type = shard_report<GroupId, Key, node_id_type>;
    using node_report_type = node_report<node_id_type>;
    using operation_type = shard_operation<GroupId, Key, node_id_type>;
    using snapshot_type =
        cluster_capacity_snapshot<node_id_type, GroupId, Key, placement_group_type>;
    using decision_type = capacity_decision<placement_group_type, node_id_type>;
    using intent_type = capacity_intent<node_id_type, placement_group_type>;
    using patch_type = capacity_intent_patch<node_id_type, placement_group_type>;
    using health_type = quorum_health<node_id_type, placement_group_type>;
    using peer_type = peer_info<node_id_type, address_type>;
    using ms = std::chrono::milliseconds;

    /// The first operation id the controller hands out. The high bit keeps
    /// its ids disjoint from an inner driver's, so the decorator can tell its
    /// own outcomes apart (design §5).
    static constexpr std::uint64_t first_operation_id = (std::uint64_t{1} << 63U) + 1U;

    /// @throws std::invalid_argument when the configuration or the policy fails
    ///         validation, listing every error rather than the first
    ///         (Requirement 3.6).
    elastic_capacity_controller(
        QuorumMgr manager, Policy policy, Ledger& ledger, Lease lease,
        elastic_capacity_config config, executor_fn executor,
        clock_fn clock = [] { return std::chrono::system_clock::now(); },
        Metrics metrics = Metrics{}, Logger logger = Logger{})
        : _mgr(std::make_shared<QuorumMgr>(std::move(manager))),
          _policy(std::move(policy)),
          _ledger(&ledger),
          _lease(std::move(lease)),
          _cfg(std::move(config)),
          _executor(std::move(executor)),
          _clock(std::move(clock)),
          _metrics(std::move(metrics)),
          _logger(std::move(logger)),
          _rng(_cfg.jitter_seed ? *_cfg.jitter_seed : std::random_device{}()),
          _kill(_cfg.kill_switch) {
        auto errors = _cfg.get_validation_errors();
        for (auto& p : _policy.get_validation_errors()) {
            errors.push_back("policy: " + p);
        }
        if (!_executor) {
            errors.emplace_back("an executor is required: provider calls never run inline");
        }
        if (!_clock) {
            errors.emplace_back("a clock is required");
        }
        if (!errors.empty()) {
            std::string all = "elastic_capacity_controller: invalid configuration:";
            for (const auto& e : errors) {
                all += " " + e + ";";
            }
            throw std::invalid_argument(all);
        }
        if constexpr (!resizable_quorum_manager<QuorumMgr>) {
            log(log_level::info, "capacity_note",
                {{"note",
                  "manager has no set_group_target; group-capacity targets are not kept "
                  "in step"}});
        }
    }

    elastic_capacity_controller(const elastic_capacity_controller&) = delete;
    auto operator=(const elastic_capacity_controller&) -> elastic_capacity_controller& = delete;

    // ── inputs: the heartbeat channel ────────────────────────────────────────

    /// @brief One host's machine report.
    auto observe_node(const node_report_type& report) -> void {
        std::lock_guard lock(_mutex);
        const auto now = _clock();
        if (_observing_since == time_point{}) {
            _observing_since = now;
        }
        auto& e = _nodes[report._node_id];
        if (e._first_seen == time_point{}) {
            e._first_seen = now;
        }
        e._report = report;
        e._seen = now;
        if (_initial_cluster_size == 0) {
            _initial_cluster_size = _nodes.size();
        }
    }

    /// @brief One host's batch of leader shard reports.
    auto observe_shards(const std::vector<shard_report_type>& reports) -> void {
        std::lock_guard lock(_mutex);
        const auto now = _clock();
        std::size_t refusals = 0;
        for (const auto& r : reports) {
            auto& e = _shards[r.group_id()];
            // A report from a deposed leader (lower term) must not overwrite
            // its successor's.
            if (e._seen != time_point{} && r._term < e._report._term &&
                now - e._seen < _cfg.node_report_staleness) {
                continue;
            }
            e._report = r;
            e._seen = now;
            _descriptors[r.group_id()] = r.descriptor();
            refusals += r._capacity_refusals;
        }
        if (refusals > 0) {
            _refusal_events.push_back({now, refusals});
            // Out of room right now is a trigger edge too.
            _dirty = true;
        }
    }

    /// @brief A split happened: a trigger edge (Requirement 5.1).
    auto observe_split(const descriptor_type& parent, const std::vector<descriptor_type>& children)
        -> void {
        std::lock_guard lock(_mutex);
        const auto now = _clock();
        std::size_t added = 0;
        for (const auto& c : children) {
            _descriptors[c._group_id] = c;
            _cooldown[c._group_id] = now + _cfg.move_cooldown_after_split;
            added += c._voters.size() + c._learners.size();
        }
        const auto parent_replicas = parent._voters.size() + parent._learners.size();
        _rate_events.push_back(
            {now, 1, 0, added > parent_replicas ? added - parent_replicas : 0, 0});
        _cooldown[parent._group_id] = now + _cfg.move_cooldown_after_split;
        _dirty = true;
    }

    /// @brief A merge happened. Feeds the rate window; merges feed scale-in only.
    auto observe_merge(const descriptor_type& source, const descriptor_type& target) -> void {
        std::lock_guard lock(_mutex);
        const auto now = _clock();
        _descriptors[target._group_id] = target;
        _descriptors.erase(source._group_id);
        _shards.erase(source._group_id);
        _cooldown[target._group_id] = now + _cfg.move_cooldown_after_split;
        _rate_events.push_back({now, 0, 1, 0, source._voters.size() + source._learners.size()});
    }

    /// @brief What became of the operators this controller sent (Requirement
    ///        13.6). Outcomes for ids it did not issue are ignored.
    auto observe_operator_outcomes(const std::vector<operator_outcome>& outcomes) -> void {
        std::lock_guard lock(_mutex);
        const auto now = _clock();
        for (const auto& o : outcomes) {
            auto it = _op_owner.find(o._operation_id);
            if (it == _op_owner.end()) {
                continue;
            }
            const auto group = it->second;
            _op_owner.erase(it);
            if (o._accepted) {
                continue;
            }
            count("operator_skipped", {{"reason", to_string(o._reason)}});
            auto m = _moves.find(group);
            if (m == _moves.end()) {
                // A leader transfer of a drain: just allow a re-send later.
                _leader_transfer_sent.erase(group);
                continue;
            }
            switch (o._reason) {
                case skipped_operator_reason::shard_busy:
                    m->second._retry_after = now + _cfg.operator_busy_backoff;
                    m->second._last_emit = time_point{};
                    break;
                case skipped_operator_reason::stale_epoch:
                case skipped_operator_reason::not_leader:
                case skipped_operator_reason::unknown_shard:
                case skipped_operator_reason::precondition:
                    // Re-plan from the next fresh report, never resend blindly.
                    m->second._last_emit = time_point{};
                    m->second._await_report_after = now;
                    break;
                case skipped_operator_reason::unsupported:
                    if (displace_another_voter(m->second, now)) {
                        break;
                    }
                    abandon_move(m->second, "host refused: " + std::string(to_string(o._reason)));
                    break;
                case skipped_operator_reason::driver_disabled:
                default:
                    abandon_move(m->second, "host refused: " + std::string(to_string(o._reason)));
                    break;
            }
        }
    }

    /// @brief A descriptor this controller has seen, for a host's lazy replica
    ///        creation (Requirement 10.2). A group with a move in flight is
    ///        served with the move's target already listed as a learner, so the
    ///        target can materialise its replica from the very first
    ///        AppendEntries rather than one heartbeat later.
    [[nodiscard]] auto lookup_descriptor(const GroupId& group) const
        -> std::optional<descriptor_type> {
        std::lock_guard lock(_mutex);
        auto it = _descriptors.find(group);
        if (it == _descriptors.end()) {
            return std::nullopt;
        }
        auto d = it->second;
        if (auto m = _moves.find(group); m != _moves.end() && !d.has_replica(m->second._to)) {
            d._learners.push_back(m->second._to);
        }
        return d;
    }

    // ── the step ─────────────────────────────────────────────────────────────

    /// @brief One non-blocking turn of the loop (design §4 `step()`).
    ///
    /// Returns the operators to send this heartbeat. Never waits on a provider
    /// future and never calls a provider itself: provider work goes to the
    /// executor, after the internal mutex is released.
    auto step() -> std::vector<operation_type> {
        std::vector<std::function<void()>> work;
        std::vector<operation_type> ops;
        {
            std::lock_guard lock(_mutex);
            ops = step_locked(work);
        }
        for (auto& w : work) {
            _executor(std::move(w));
        }
        return ops;
    }

    /// @brief A step on behalf of one host: its leader reports are observed
    ///        first, and only operators for the groups it reported — the
    ///        groups it leads, and so can apply — are returned.
    ///
    /// What the decorator calls on each host's heartbeat. Several hosts may
    /// share one controller; each gets its own groups' operators.
    auto step_for(const std::vector<shard_report_type>& reports) -> std::vector<operation_type> {
        observe_shards(reports);
        std::vector<std::function<void()>> work;
        std::vector<operation_type> ops;
        {
            std::lock_guard lock(_mutex);
            std::set<GroupId> mine;
            for (const auto& r : reports) {
                mine.insert(r.group_id());
            }
            _addressable = std::move(mine);
            try {
                ops = step_locked(work);
            } catch (...) {
                _addressable.reset();
                throw;
            }
            _addressable.reset();
        }
        for (auto& w : work) {
            _executor(std::move(w));
        }
        return ops;
    }

    /// @brief The kill switch (Requirement 12.4).
    auto set_kill_switch(bool on) -> void { _kill.store(on); }
    [[nodiscard]] auto kill_switch() const -> bool { return _kill.load(); }

    // ── introspection ────────────────────────────────────────────────────────

    /// @brief The snapshot the policy would see now.
    [[nodiscard]] auto snapshot() const -> snapshot_type {
        std::lock_guard lock(_mutex);
        return build_snapshot(_clock());
    }

    /// @brief A counter by its rendered name, e.g. `refused{bound=max_cluster_size}`.
    [[nodiscard]] auto counter(const std::string& rendered) const -> std::uint64_t {
        std::lock_guard lock(_mutex);
        auto it = _counters.find(rendered);
        return it == _counters.end() ? 0 : it->second;
    }

    [[nodiscard]] auto counters() const -> std::map<std::string, std::uint64_t> {
        std::lock_guard lock(_mutex);
        return _counters;
    }

    /// @brief The last decision record, exactly as logged: one line.
    [[nodiscard]] auto last_decision_record() const -> std::string {
        std::lock_guard lock(_mutex);
        return _last_record;
    }

    [[nodiscard]] auto reconciling() const -> bool {
        std::lock_guard lock(_mutex);
        return _reconcile.has_value();
    }

    [[nodiscard]] auto moves_in_flight() const -> std::size_t {
        std::lock_guard lock(_mutex);
        return _moves.size();
    }

    [[nodiscard]] auto provider_calls_in_flight() const -> std::size_t {
        std::lock_guard lock(_mutex);
        return _calls.size();
    }

    [[nodiscard]] auto circuit_open() const -> bool {
        std::lock_guard lock(_mutex);
        return _circuit_until.has_value() && _clock() < *_circuit_until;
    }

    [[nodiscard]] auto config() const -> const elastic_capacity_config& { return _cfg; }

private:
    // ── state ────────────────────────────────────────────────────────────────

    struct node_entry {
        node_report_type _report{};
        time_point _seen{};
        time_point _first_seen{};
    };

    struct shard_entry {
        shard_report_type _report{};
        time_point _seen{};
    };

    struct rate_event {
        time_point _at{};
        std::size_t _splits{0};
        std::size_t _merges{0};
        std::size_t _added{0};
        std::size_t _removed{0};
    };

    enum class call_op : std::uint8_t {
        provision,
        decommission_reap,
        decommission_scale_in,
        find_by_key,
        assess,
        set_group_target,
    };

    static auto op_name(call_op op) -> const char* {
        switch (op) {
            case call_op::provision:
                return "provision";
            case call_op::decommission_reap:
            case call_op::decommission_scale_in:
                return "decommission";
            case call_op::find_by_key:
                return "find_by_key";
            case call_op::assess:
                return "assess";
            case call_op::set_group_target:
                return "set_group_target";
            default:
                return "unknown";
        }
    }

    /// What a provider call came back with, flattened.
    struct provider_outcome {
        bool _ok{false};
        std::string _error;
        std::optional<peer_type> _peer;
        std::optional<std::optional<peer_type>> _lookup;
        std::optional<health_type> _health;
    };

    struct pending_call {
        call_op _op{call_op::assess};
        std::string _key;
        std::uint64_t _generation{0};
        time_point _started{};
        placement_group_type _group{};
        std::optional<node_id_type> _node;
        bool _reconcile{false};
        std::function<std::optional<provider_outcome>()> _poll;
    };

    template<typename Fut> struct call_slot {
        std::mutex _m;
        std::optional<Fut> _future;
        std::optional<std::string> _start_error;
    };

    enum class move_stage : std::uint8_t {
        add_learner,
        promote,
        remove,
        rollback
    };

    struct move {
        GroupId _group{};
        node_id_type _from{};
        node_id_type _to{};
        std::string _intent;
        capacity_reason _reason{capacity_reason::manual};
        time_point _started{};
        time_point _retry_after{};
        time_point _last_emit{};
        time_point _await_report_after{};
        bool _drain{false};
        /// Remove `_from` and add nothing: a drained learner.
        bool _remove_only{false};
        /// Abandoned after adding `_to` as a learner: remove it again.
        bool _rollback{false};
        /// Abandoned with nothing to undo; pruned on the next step.
        bool _done{false};
    };

    struct pending_write {
        capacity_ledger_ticket _ticket{};
        std::string _key;
        std::optional<capacity_intent_state> _to;  // unset: a record
        std::uint64_t _generation{0};
    };

    struct reconcile_state {
        time_point _started{};
        time_point _deadline{};
        bool _assess_done{false};
        std::map<std::string, std::optional<std::optional<peer_type>>> _lookups;
    };

    std::shared_ptr<QuorumMgr> _mgr;
    Policy _policy;
    Ledger* _ledger;
    Lease _lease;
    elastic_capacity_config _cfg;
    executor_fn _executor;
    clock_fn _clock;
    Metrics _metrics;
    Logger _logger;
    std::mt19937 _rng;
    std::atomic<bool> _kill;

    mutable std::mutex _mutex;

    std::map<node_id_type, node_entry> _nodes;
    std::map<GroupId, shard_entry> _shards;
    std::map<GroupId, descriptor_type> _descriptors;
    std::map<GroupId, time_point> _cooldown;
    std::deque<rate_event> _rate_events;
    std::deque<std::pair<time_point, std::size_t>> _refusal_events;
    std::size_t _initial_cluster_size{0};
    time_point _observing_since{};

    std::optional<health_type> _health;
    time_point _health_at{};
    bool _assess_in_flight{false};

    std::vector<pending_call> _calls;
    std::vector<pending_write> _writes;
    std::set<std::string> _keys_in_write;
    std::vector<intent_type> _intents;
    std::map<std::string, time_point> _retry_after;  // per intent key
    std::map<std::string, std::vector<placement_group_type>> _fallback;
    std::map<std::string, node_id_type> _found_by_key;  // reaping, keyed
    std::set<std::string> _admission_saturated;

    std::map<GroupId, move> _moves;
    std::map<GroupId, time_point> _leader_transfer_sent;
    std::map<std::uint64_t, GroupId> _op_owner;
    /// Set for the duration of a `step_for`: the groups its caller leads.
    std::optional<std::set<GroupId>> _addressable;
    std::uint64_t _next_op_id{first_operation_id};

    std::map<placement_group_type, time_point> _placement_refused;

    bool _was_held{false};
    std::uint64_t _token{0};
    std::uint64_t _generation{0};
    std::optional<reconcile_state> _reconcile;

    time_point _last_eval{};
    time_point _last_non_hold{};
    bool _has_non_hold{false};
    bool _dirty{false};
    time_point _last_capacity_call{};
    bool _has_capacity_call{false};
    std::deque<time_point> _provision_calls;
    std::size_t _consecutive_failures{0};
    time_point _backoff_until{};
    std::optional<time_point> _circuit_until;
    time_point _last_compact{};
    time_point _last_kill_count{};
    time_point _last_fence_count{};

    std::map<std::string, std::uint64_t> _counters;
    std::string _last_record;

    // ── observability ────────────────────────────────────────────────────────

    using dims = std::vector<std::pair<std::string, std::string>>;

    static auto render(const std::string& name, dims d) -> std::string {
        if (d.empty()) {
            return name;
        }
        std::sort(d.begin(), d.end());
        std::string out = name + "{";
        for (std::size_t i = 0; i < d.size(); ++i) {
            out += ((i != 0u) ? "," : "") + d[i].first + "=" + d[i].second;
        }
        return out + "}";
    }

    auto count(const std::string& name, dims d = {}, std::int64_t n = 1) -> void {
        _counters[render(name, d)] += static_cast<std::uint64_t>(n);
        _metrics.set_metric_name("kythira.multiraft.capacity." + name);
        for (const auto& [k, v] : d) {
            _metrics.add_dimension(k, v);
        }
        _metrics.add_count(n);
        _metrics.emit();
    }

    auto gauge(const std::string& name, double value, dims d = {}) -> void {
        _metrics.set_metric_name("kythira.multiraft.capacity." + name);
        for (const auto& [k, v] : d) {
            _metrics.add_dimension(k, v);
        }
        _metrics.add_value(value);
        _metrics.emit();
    }

    auto latency(call_op op, bool ok, ms elapsed) -> void {
        _metrics.set_metric_name("kythira.multiraft.capacity.provider_call.latency");
        _metrics.add_dimension("op", op_name(op));
        _metrics.add_dimension("outcome", ok ? "ok" : "error");
        _metrics.add_duration(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed));
        _metrics.emit();
    }

    auto log(log_level level, std::string_view message, const dims& kvs = {}) -> void {
        std::vector<std::pair<std::string_view, std::string_view>> views;
        views.reserve(kvs.size());
        for (const auto& [k, v] : kvs) {
            views.emplace_back(k, v);
        }
        _logger.log(level, message, views);
    }

    auto refuse(const std::string& bound, const std::string& detail = {}) -> void {
        count("refused", {{"bound", bound}});
        log(log_level::info, "capacity_refused", {{"bound", bound}, {"detail", detail}});
    }

    // ── the step, under the lock ─────────────────────────────────────────────

    auto step_locked(std::vector<std::function<void()>>& work) -> std::vector<operation_type> {
        if (!_cfg.enabled) {
            return {};
        }
        const auto now = _clock();

        bool held = false;
        try {
            held = _lease.held();
        } catch (...) {
            held = false;  // unknown is not held (Requirement 7.4)
        }
        if (!held) {
            if (_was_held) {
                on_lease_lost();
            }
            return {};
        }
        std::uint64_t token = 0;
        try {
            token = _lease.fencing_token();
        } catch (...) {
            return {};
        }
        if (!_was_held || token != _token) {
            on_lease_acquired(token, now, work);
        }

        _intents = _ledger->intents();
        if (fenced_out(now)) {
            return {};
        }
        if (_kill.load()) {
            if (now - _last_kill_count >= _cfg.evaluation_interval) {
                _last_kill_count = now;
                refuse("kill_switch");
            }
            return {};
        }

        poll_writes(now, work);
        harvest(now, work);
        maybe_assess(now, work);
        if (_reconcile) {
            reconcile_step(now, work);
            if (_reconcile) {
                return {};
            }
        }
        advance_intents(now, work);
        maybe_compact(now);
        if (_dirty || _last_eval == time_point{} || now - _last_eval >= _cfg.evaluation_interval) {
            evaluate(now, work);
        }
        if (_cfg.dry_run) {
            return {};
        }
        return plan(now);
    }

    auto on_lease_lost() -> void {
        _was_held = false;
        ++_generation;
        // In-flight calls complete or time out without being acted on
        // (Requirement 7.3): dropping them here means their results are never
        // harvested. What they created, reconciliation finds.
        _calls.clear();
        _moves.clear();
        _leader_transfer_sent.clear();
        _op_owner.clear();
        _assess_in_flight = false;
        _reconcile.reset();
        count("lease", {{"event", "lost"}});
        log(log_level::warning, "capacity_lease_lost");
    }

    auto on_lease_acquired(std::uint64_t token, time_point now,
                           std::vector<std::function<void()>>& work) -> void {
        _was_held = true;
        _token = token;
        ++_generation;
        _calls.clear();
        _moves.clear();
        _leader_transfer_sent.clear();
        _op_owner.clear();
        _assess_in_flight = false;
        _fallback.clear();
        _found_by_key.clear();
        count("lease", {{"event", "acquired"}});
        log(log_level::info, "capacity_lease_acquired", {{"fencing_token", std::to_string(token)}});

        // Reconcile before any decision (design §7, Requirement 8.3).
        _reconcile = reconcile_state{._started = now, ._deadline = now + _cfg.reconcile_deadline};
        dispatch_assess(now, work, true);
        if constexpr (keyed_quorum_manager<QuorumMgr>) {
            for (const auto& i : _ledger->intents()) {
                if (i.terminal() || i._kind != capacity_intent_kind::scale_out || i._node) {
                    continue;
                }
                _reconcile->_lookups[i._key] = std::nullopt;
                dispatch_lookup(i, now, work, true);
            }
        }
    }

    /// Requirement 7.6: an intent stamped with a newer fencing token than ours
    /// means a controller holding a newer lease has acted. Refuse to act at
    /// all until the lease mechanism resolves it.
    auto fenced_out(time_point now) -> bool {
        for (const auto& i : _intents) {
            if (i._fencing_token > _token) {
                if (now - _last_fence_count >= _cfg.evaluation_interval ||
                    _last_fence_count == time_point{}) {
                    _last_fence_count = now;
                    count("fencing_conflict");
                    log(log_level::error, "capacity_second_controller",
                        {{"intent", i._key},
                         {"their_token", std::to_string(i._fencing_token)},
                         {"our_token", std::to_string(_token)}});
                }
                return true;
            }
        }
        return false;
    }

    // ── ledger writes ────────────────────────────────────────────────────────

    [[nodiscard]] auto find_intent(const std::string& key) -> intent_type* {
        for (auto& i : _intents) {
            if (i._key == key) {
                return &i;
            }
        }
        return nullptr;
    }

    auto record(intent_type intent, time_point now, std::vector<std::function<void()>>& work)
        -> void {
        const auto key = intent._key;
        const auto t = _ledger->record(intent);
        _writes.push_back(
            {._ticket = t, ._key = key, ._to = std::nullopt, ._generation = _generation});
        _keys_in_write.insert(key);
        poll_writes(now, work);
    }

    auto transition(const intent_type& i, capacity_intent_state to, patch_type patch,
                    time_point now, std::vector<std::function<void()>>& work) -> bool {
        if (_keys_in_write.contains(i._key) ||
            !capacity_intent_transition_allowed(i._kind, i._state, to)) {
            return false;
        }
        const auto t = _ledger->transition(i._key, to, patch, now);
        _writes.push_back({._ticket = t, ._key = i._key, ._to = to, ._generation = _generation});
        _keys_in_write.insert(i._key);
        poll_writes(now, work);
        return true;
    }

    auto poll_writes(time_point now, std::vector<std::function<void()>>& work) -> void {
        for (std::size_t idx = 0; idx < _writes.size();) {
            auto w = _writes[idx];
            const auto status = _ledger->status(w._ticket);
            if (status == capacity_ledger_write_status::pending) {
                ++idx;
                continue;
            }
            _writes.erase(_writes.begin() + static_cast<std::ptrdiff_t>(idx));
            _keys_in_write.erase(w._key);
            std::optional<capacity_intent_token> token;
            if (status == capacity_ledger_write_status::committed && !w._to) {
                token = _ledger->token(w._ticket);
            }
            _ledger->forget(w._ticket);
            if (status == capacity_ledger_write_status::rejected) {
                count("ledger_write", {{"outcome", "rejected"}});
                log(log_level::warning, "capacity_ledger_rejected",
                    {{"intent", w._key}, {"to", w._to ? to_string(*w._to) : "record"}});
                continue;
            }
            // Committed: refresh our copy of the intent.
            const auto fresh = _ledger->find(w._key);
            if (fresh) {
                if (auto* mine = find_intent(w._key)) {
                    *mine = *fresh;
                } else {
                    _intents.push_back(*fresh);
                }
            }
            if (w._to && is_terminal(*w._to)) {
                count("intent", {{"terminal_state", to_string(*w._to)}});
                _fallback.erase(w._key);
                _admission_saturated.erase(w._key);
                _retry_after.erase(w._key);
            }
            if (!w._to && token && fresh && w._generation == _generation) {
                on_recorded(*fresh, *token, now, work);
            }
        }
    }

    /// A record committed: the intent is durable, so its action may start.
    auto on_recorded(const intent_type& i, const capacity_intent_token& token, time_point now,
                     std::vector<std::function<void()>>& work) -> void {
        if (i._kind == capacity_intent_kind::scale_out) {
            transition(i, capacity_intent_state::provisioning,
                       patch_type{._deadline = now + _cfg.provision_deadline}, now, work);
            dispatch_provision(i, token, now, work);
        }
        // A scale-in needs no provider call until it is drained.
    }

    // ── provider calls ───────────────────────────────────────────────────────

    template<typename Initiate, typename Convert>
    auto dispatch(pending_call call, Initiate initiate, Convert convert,
                  std::vector<std::function<void()>>& work) -> void {
        using fut = std::invoke_result_t<Initiate&, QuorumMgr&>;
        auto slot = std::make_shared<call_slot<fut>>();
        work.push_back([slot, mgr = _mgr, initiate = std::move(initiate)]() mutable {
            try {
                auto f = initiate(*mgr);
                std::lock_guard lock(slot->_m);
                slot->_future.emplace(std::move(f));
            } catch (const std::exception& e) {
                std::lock_guard lock(slot->_m);
                slot->_start_error = e.what();
            } catch (...) {
                std::lock_guard lock(slot->_m);
                slot->_start_error = "unknown error";
            }
        });
        call._poll = [slot, convert = std::move(convert)]() -> std::optional<provider_outcome> {
            std::lock_guard lock(slot->_m);
            if (slot->_start_error) {
                return provider_outcome{._ok = false, ._error = *slot->_start_error};
            }
            if (!slot->_future || !slot->_future->isReady()) {
                return std::nullopt;
            }
            provider_outcome out;
            try {
                convert(std::move(*slot->_future), out);
                out._ok = true;
            } catch (const std::exception& e) {
                out._ok = false;
                out._error = e.what();
            } catch (...) {
                out._ok = false;
                out._error = "unknown error";
            }
            slot->_future.reset();
            return out;
        };
        _calls.push_back(std::move(call));
    }

    [[nodiscard]] auto call_in_flight(const std::string& key) const -> bool {
        return std::any_of(_calls.begin(), _calls.end(),
                           [&](const auto& c) { return c._key == key; });
    }

    auto dispatch_provision(const intent_type& i, const capacity_intent_token& token,
                            time_point now, std::vector<std::function<void()>>& work) -> void {
        _last_capacity_call = now;
        _has_capacity_call = true;
        _provision_calls.push_back(now);
        count("provider_call", {{"op", "provision"}, {"outcome", "started"}});
        pending_call c{._op = call_op::provision,
                       ._key = i._key,
                       ._generation = _generation,
                       ._started = now,
                       ._group = i._group};
        auto group = i._group;
        const auto& key = token.key();
        dispatch(
            std::move(c),
            [group, key](QuorumMgr& m) {
                if constexpr (keyed_quorum_manager<QuorumMgr>) {
                    return m.provision_node_keyed(group, std::nullopt, key);
                } else {
                    return m.provision_node(group, std::nullopt);
                }
            },
            [](auto&& f, provider_outcome& out) { out._peer = std::forward<decltype(f)>(f).get(); },
            work);
    }

    auto dispatch_decommission(const intent_type& i, node_id_type node, call_op op, time_point now,
                               std::vector<std::function<void()>>& work) -> void {
        count("provider_call", {{"op", "decommission"}, {"outcome", "started"}});
        pending_call c{._op = op,
                       ._key = i._key,
                       ._generation = _generation,
                       ._started = now,
                       ._group = i._group,
                       ._node = node};
        dispatch(
            std::move(c), [node](QuorumMgr& m) { return m.decommission_node(node); },
            [](auto&& f, provider_outcome&) {
                static_cast<void>(std::forward<decltype(f)>(f).get());
            },
            work);
    }

    auto dispatch_lookup(const intent_type& i, time_point now,
                         std::vector<std::function<void()>>& work, bool reconcile) -> void {
        if constexpr (keyed_quorum_manager<QuorumMgr>) {
            pending_call c{._op = call_op::find_by_key,
                           ._key = i._key,
                           ._generation = _generation,
                           ._started = now,
                           ._group = i._group,
                           ._reconcile = reconcile};
            auto key = i._key;
            dispatch(
                std::move(c), [key](QuorumMgr& m) { return m.find_by_idempotency_key(key); },
                [](auto&& f, provider_outcome& out) {
                    auto r = std::forward<decltype(f)>(f).get();
                    if (r) {
                        out._lookup = std::optional<peer_type>{peer_type{r->node_id, r->address}};
                    } else {
                        out._lookup = std::optional<peer_type>{};
                    }
                },
                work);
        } else {
            static_cast<void>(i);
            static_cast<void>(now);
            static_cast<void>(work);
            static_cast<void>(reconcile);
        }
    }

    auto assessment_cluster() const
        -> std::vector<node_placement<node_id_type, placement_group_type>> {
        std::vector<node_placement<node_id_type, placement_group_type>> out;
        for (const auto& [id, e] : _nodes) {
            if (auto g = placement_of(id)) {
                out.push_back({.node_id = id, .group_id = *g});
            }
        }
        return out;
    }

    auto maybe_assess(time_point now, std::vector<std::function<void()>>& work) -> void {
        if (!_assess_in_flight &&
            (_health_at == time_point{} || now - _health_at >= _cfg.assess_interval)) {
            dispatch_assess(now, work, false);
        }
    }

    auto dispatch_assess(time_point now, std::vector<std::function<void()>>& work, bool reconcile)
        -> void {
        _assess_in_flight = true;
        pending_call c{._op = call_op::assess,
                       ._generation = _generation,
                       ._started = now,
                       ._reconcile = reconcile};
        auto cluster = assessment_cluster();
        dispatch(
            std::move(c), [cluster](QuorumMgr& m) { return m.assess_quorum(cluster); },
            [](auto&& f, provider_outcome& out) {
                out._health = std::forward<decltype(f)>(f).get();
            },
            work);
    }

    auto dispatch_group_target(const placement_group_type& group, std::size_t n, time_point now,
                               std::vector<std::function<void()>>& work) -> void {
        if constexpr (resizable_quorum_manager<QuorumMgr>) {
            pending_call c{._op = call_op::set_group_target,
                           ._generation = _generation,
                           ._started = now,
                           ._group = group};
            dispatch(
                std::move(c), [group, n](QuorumMgr& m) { return m.set_group_target(group, n); },
                [](auto&& f, provider_outcome&) {
                    static_cast<void>(std::forward<decltype(f)>(f).get());
                },
                work);
        } else {
            static_cast<void>(group);
            static_cast<void>(n);
            static_cast<void>(now);
            static_cast<void>(work);
        }
    }

    // ── harvesting ───────────────────────────────────────────────────────────

    auto harvest(time_point now, std::vector<std::function<void()>>& work) -> void {
        for (std::size_t idx = 0; idx < _calls.size();) {
            auto& c = _calls[idx];
            // A result for an intent whose write is still pending waits a
            // step: the ledger must show the state the result moves it from.
            if (!c._key.empty() && _keys_in_write.contains(c._key)) {
                ++idx;
                continue;
            }
            auto outcome = c._poll();
            if (!outcome) {
                ++idx;
                continue;
            }
            auto call = std::move(c);
            _calls.erase(_calls.begin() + static_cast<std::ptrdiff_t>(idx));
            if (call._generation != _generation) {
                count("provider_result_discarded");
                continue;
            }
            const auto elapsed = std::chrono::duration_cast<ms>(now - call._started);
            latency(call._op, outcome->_ok, elapsed);
            count("provider_call",
                  {{"op", op_name(call._op)}, {"outcome", outcome->_ok ? "ok" : "error"}});
            on_outcome(call, std::move(*outcome), now, work);
        }
    }

    auto note_failure(time_point now) -> void {
        ++_consecutive_failures;
        _backoff_until = now + detail::capacity::backoff_delay(_cfg.provider_backoff,
                                                               _consecutive_failures, _rng);
        if (_consecutive_failures >= _cfg.provider_backoff.max_attempts) {
            if (!_circuit_until || now >= *_circuit_until) {
                count("circuit", {{"event", "opened"}});
                log(log_level::warning, "capacity_circuit_open",
                    {{"consecutive_failures", std::to_string(_consecutive_failures)}});
            }
            _circuit_until = now + _cfg.circuit_cool_off;
        }
    }

    auto note_success() -> void {
        if (_circuit_until) {
            count("circuit", {{"event", "closed"}});
        }
        _consecutive_failures = 0;
        _backoff_until = time_point{};
        _circuit_until.reset();
    }

    static auto is_refusal(const std::string& error) -> bool {
        auto lower = error;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return lower.find("stock") != std::string::npos ||
               lower.find("quota") != std::string::npos ||
               lower.find("capacity") != std::string::npos ||
               lower.find("insufficient") != std::string::npos;
    }

    auto on_outcome(const pending_call& call, provider_outcome out, time_point now,
                    std::vector<std::function<void()>>& work) -> void {
        if (call._op == call_op::assess) {
            _assess_in_flight = false;
            if (out._ok && out._health) {
                _health = std::move(out._health);
                _health_at = now;
            } else {
                log(log_level::warning, "capacity_assess_failed", {{"error", out._error}});
                _health_at = now;  // retry at the next interval, not every step
            }
            if (call._reconcile && _reconcile) {
                _reconcile->_assess_done = true;
            }
            return;
        }
        if (call._op == call_op::set_group_target) {
            if (!out._ok) {
                log(log_level::warning, "capacity_group_target_failed", {{"error", out._error}});
            }
            return;
        }
        if (call._op == call_op::find_by_key && call._reconcile) {
            if (_reconcile) {
                _reconcile->_lookups[call._key] =
                    out._ok && out._lookup ? *out._lookup : std::optional<peer_type>{};
            }
            return;
        }
        auto* i = find_intent(call._key);
        if (i == nullptr || i->terminal()) {
            return;
        }
        switch (call._op) {
            case call_op::provision:
                on_provisioned(*i, std::move(out), now, work);
                break;
            case call_op::find_by_key:
                // Reaping a keyed machine whose id we never learned.
                if (out._ok && out._lookup && *out._lookup) {
                    _found_by_key[i->_key] = (*out._lookup)->node_id;
                    dispatch_decommission(*i, (*out._lookup)->node_id, call_op::decommission_reap,
                                          now, work);
                } else if (out._ok) {
                    transition(*i, capacity_intent_state::failed,
                               patch_type{._note = "reaping: no machine carries the key"}, now,
                               work);
                    note_failure(now);
                } else {
                    _retry_after[i->_key] = now + _cfg.operator_retry_interval;
                }
                break;
            case call_op::decommission_reap:
                if (out._ok) {
                    count("orphan_reaped");
                    transition(*i, capacity_intent_state::orphaned,
                               patch_type{._node = call._node, ._note = "reaped"}, now, work);
                } else {
                    _retry_after[i->_key] =
                        now + detail::capacity::backoff_delay(_cfg.provider_backoff, 1, _rng);
                }
                break;
            case call_op::decommission_scale_in:
                if (out._ok) {
                    count("drain", {{"outcome", "completed"}});
                    transition(*i, capacity_intent_state::completed,
                               patch_type{._note = "decommissioned"}, now, work);
                    if (auto g = i->_group; true) {
                        dispatch_group_target(g, group_known_count(g, call._node), now, work);
                    }
                    _nodes.erase(*call._node);
                } else {
                    transition(*i, capacity_intent_state::failed,
                               patch_type{._note = "decommission failed: " + out._error}, now,
                               work);
                    note_failure(now);
                }
                break;
            default:
                break;
        }
    }

    auto on_provisioned(const intent_type& i, provider_outcome out, time_point now,
                        std::vector<std::function<void()>>& work) -> void {
        if (out._ok && out._peer) {
            note_success();
            const auto node = out._peer->node_id;
            transition(i, capacity_intent_state::provisioned,
                       patch_type{._node = node,
                                  ._address = detail::capacity::to_text(out._peer->address),
                                  ._deadline = now + _cfg.join_deadline,
                                  ._note = "provisioned",
                                  ._attempt =
                                      capacity_placement_attempt<placement_group_type>{
                                          ._group = i._group, ._outcome = "provisioned"}},
                       now, work);
            // The new machine counted once, whether or not it heartbeats yet.
            dispatch_group_target(i._group, group_known_count(i._group, node) + 1, now, work);
            return;
        }
        // Refused or failed: never retried in place (Requirement 13.5).
        const bool refusal = is_refusal(out._error);
        _placement_refused[i._group] = now;
        note_failure(now);
        count("placement", {{"outcome", refusal ? "refused" : "failed"}});
        auto remaining = _fallback[i._key];
        const auto attempt = capacity_placement_attempt<placement_group_type>{
            ._group = i._group, ._outcome = out._error.empty() ? "error" : out._error};
        if (!remaining.empty() && provision_budget_left(now) && !circuit_is_open(now)) {
            const auto next = remaining.front();
            remaining.erase(remaining.begin());
            transition(
                i, capacity_intent_state::failed,
                patch_type{._note = "refused; falling back to " + detail::capacity::to_text(next),
                           ._attempt = attempt},
                now, work);
            count("placement", {{"outcome", "fallback"}});
            auto successor = i;
            successor._key = new_key(now);
            successor._state = capacity_intent_state::requested;
            successor._group = next;
            successor._node.reset();
            successor._address.clear();
            successor._attempts.push_back(attempt);
            successor._created_at = now;
            successor._updated_at = now;
            successor._deadline = now + _cfg.provision_deadline;
            successor._fencing_token = _token;
            successor._note = "fallback from " + i._key;
            _fallback[successor._key] = remaining;
            log(log_level::info, "capacity_placement_fallback",
                {{"from_group", detail::capacity::to_text(i._group)},
                 {"to_group", detail::capacity::to_text(next)},
                 {"error", out._error},
                 {"intent", successor._key}});
            record(std::move(successor), now, work);
            return;
        }
        transition(i, capacity_intent_state::failed,
                   patch_type{._note = "provider: " + out._error, ._attempt = attempt}, now, work);
    }

    // ── reconciliation (design §7) ───────────────────────────────────────────

    auto reconcile_step(time_point now, std::vector<std::function<void()>>& work) -> void {
        auto& r = *_reconcile;
        const bool lookups_done = std::all_of(r._lookups.begin(), r._lookups.end(),
                                              [](const auto& kv) { return kv.second.has_value(); });
        const bool complete = r._assess_done && lookups_done;
        if (!complete && now < r._deadline) {
            return;
        }
        if (!_keys_in_write.empty()) {
            // Let our own last writes land before reading the ledger as truth.
            if (now < r._deadline) {
                return;
            }
        }
        auto state = std::move(r);
        _reconcile.reset();
        count("reconcile", {{"outcome", complete ? "completed" : "timed_out"}});
        for (const auto& i : _ledger->intents()) {
            if (i.terminal()) {
                continue;
            }
            resolve(i, state, now, work);
        }
        _intents = _ledger->intents();
    }

    [[nodiscard]] auto joined(const node_id_type& n, time_point now) const -> bool {
        auto it = _nodes.find(n);
        return it != _nodes.end() && now - it->second._seen <= _cfg.node_report_staleness;
    }

    [[nodiscard]] auto reported_unreachable(const node_id_type& n) const -> bool {
        return _health && detail::capacity::contains(_health->unreachable_nodes, n);
    }

    [[nodiscard]] auto holds_voter(const node_id_type& n) const -> bool {
        return std::any_of(_descriptors.begin(), _descriptors.end(),
                           [&](const auto& kv) { return kv.second.has_voter(n); });
    }

    [[nodiscard]] auto holds_replica(const node_id_type& n) const -> bool {
        return std::any_of(_descriptors.begin(), _descriptors.end(),
                           [&](const auto& kv) { return kv.second.has_replica(n); });
    }

    [[nodiscard]] auto attributed_nodes() const -> std::set<node_id_type> {
        std::set<node_id_type> out;
        for (const auto& i : _intents) {
            if (i._node) {
                out.insert(*i._node);
            }
        }
        return out;
    }

    auto resolve(const intent_type& i, const reconcile_state& state, time_point now,
                 std::vector<std::function<void()>>& work) -> void {
        using S = capacity_intent_state;
        if (i._kind == capacity_intent_kind::scale_in) {
            // Drains resume where they stopped; a decommission in progress is
            // re-issued, which the concept makes harmless (Requirement 8.6).
            if (i._state == S::decommissioning && i._node) {
                dispatch_decommission(i, *i._node, call_op::decommission_scale_in, now, work);
            }
            return;
        }
        if (i._state == S::admitting || i._state == S::reaping) {
            return;  // advance_intents resumes both
        }
        std::optional<node_id_type> node = i._node;
        std::optional<bool> key_found;
        if (auto it = state._lookups.find(i._key); it != state._lookups.end() && it->second) {
            key_found = it->second->has_value();
            if (*key_found && !node) {
                node = (*it->second)->node_id;
            }
        }
        const auto join_by = i._created_at + _cfg.provision_deadline + _cfg.join_deadline;
        if (node) {
            if (joined(*node, now)) {
                if (holds_voter(*node)) {
                    transition(i, S::completed,
                               patch_type{._node = node, ._note = "reconciled: joined"}, now, work);
                } else {
                    transition(i, S::admitting,
                               patch_type{._node = node,
                                          ._deadline = now + _cfg.admit_deadline,
                                          ._note = "reconciled: joined"},
                               now, work);
                }
                return;
            }
            if (key_found && !*key_found) {
                transition(i, S::failed, patch_type{._note = "reconciled: not found"}, now, work);
                note_failure(now);
                return;
            }
            if (now >= join_by) {
                transition(i, S::reaping,
                           patch_type{._node = node,
                                      ._deadline = now + _cfg.provision_deadline,
                                      ._note = "reconciled: never joined"},
                           now, work);
                return;
            }
            if (i._state != S::provisioned) {
                transition(i, S::provisioning == i._state ? S::provisioned : S::provisioning,
                           patch_type{._node = node,
                                      ._deadline = join_by,
                                      ._note = "reconciled: created, not joined"},
                           now, work);
            }
            return;
        }
        if (key_found && !*key_found) {
            // Requirement 8.5: not found is failed, and feeds the back-off.
            transition(i, S::failed, patch_type{._note = "reconciled: not found"}, now, work);
            note_failure(now);
            return;
        }
        // No key support, no node: matched by join deadline in advance_intents.
        if (i._state == S::requested) {
            transition(i, S::provisioning,
                       patch_type{._deadline = join_by, ._note = "reconciled: unattributed"}, now,
                       work);
        }
    }

    /// When a machine booted, as far as can be told: from its reported
    /// uptime, else from when this controller first heard from it — but only
    /// if that was well after this controller started listening, because
    /// everything looks new to a controller that has just started.
    [[nodiscard]] auto booted_at(const node_entry& e) const -> std::optional<time_point> {
        if (e._report._uptime > ms::zero()) {
            return e._seen - e._report._uptime;
        }
        if (e._first_seen > _observing_since + _cfg.node_report_staleness) {
            return e._first_seen;
        }
        return std::nullopt;
    }

    /// The no-metadata matching path (design §7 step 3, "else by join
    /// deadline"): a machine that booted after the intent was created, in
    /// its group, and is attributed to no other intent, is that intent's
    /// machine. A machine the provider reports but that never joined is
    /// reaped once the join deadline passes.
    auto attribute(const intent_type& i, time_point now) -> std::optional<node_id_type> {
        const auto taken = attributed_nodes();
        std::optional<node_id_type> best;
        for (const auto& [id, e] : _nodes) {
            const auto booted = booted_at(e);
            if (taken.contains(id) || !booted || *booted < i._created_at) {
                continue;
            }
            if (auto g = placement_of(id); g && *g != i._group) {
                continue;
            }
            if (!joined(id, now)) {
                continue;
            }
            if (!best || id < *best) {
                best = id;
            }
        }
        return best;
    }

    // ── advancing intents ────────────────────────────────────────────────────

    auto advance_intents(time_point now, std::vector<std::function<void()>>& work) -> void {
        using S = capacity_intent_state;
        const auto snapshot_intents = _intents;
        for (const auto& i : snapshot_intents) {
            if (i.terminal() || _keys_in_write.contains(i._key)) {
                continue;
            }
            const bool expired = i._deadline != time_point{} && now >= i._deadline;
            if (i._kind == capacity_intent_kind::scale_out) {
                switch (i._state) {
                    case S::requested:
                        if (expired) {
                            transition(i, S::abandoned, patch_type{._note = "never called"}, now,
                                       work);
                        }
                        break;
                    case S::provisioning:
                        if (!i._node && !call_in_flight(i._key)) {
                            // A predecessor's call, or an unkeyed reconcile.
                            if (auto n = attribute(i, now)) {
                                transition(i, S::admitting,
                                           patch_type{._node = n,
                                                      ._deadline = now + _cfg.admit_deadline,
                                                      ._note = "matched by join"},
                                           now, work);
                                break;
                            }
                        }
                        if (expired) {
                            erase_calls(i._key);
                            transition(i, S::reaping,
                                       patch_type{._deadline = now + _cfg.provision_deadline,
                                                  ._note = "provision deadline"},
                                       now, work);
                        }
                        break;
                    case S::provisioned:
                        if (i._node && joined(*i._node, now) && !reported_unreachable(*i._node)) {
                            transition(i, S::admitting,
                                       patch_type{._deadline = now + _cfg.admit_deadline,
                                                  ._note = "joined"},
                                       now, work);
                        } else if (expired) {
                            transition(i, S::reaping,
                                       patch_type{._deadline = now + _cfg.provision_deadline,
                                                  ._note = "join deadline"},
                                       now, work);
                        }
                        break;
                    case S::admitting:
                        advance_admission(i, expired, now, work);
                        break;
                    case S::reaping:
                        advance_reaping(i, expired, now, work);
                        break;
                    default:
                        break;
                }
            } else {
                advance_drain(i, expired, now, work);
            }
        }
    }

    auto erase_calls(const std::string& key) -> void {
        std::erase_if(_calls, [&](const auto& c) { return c._key == key; });
    }

    auto advance_admission(const intent_type& i, bool expired, time_point now,
                           std::vector<std::function<void()>>& work) -> void {
        using S = capacity_intent_state;
        if (expired) {
            cancel_moves_for(i._key, "admit deadline");
            count("admission", {{"outcome", "abandoned"}});
            transition(i, S::abandoned, patch_type{._note = "admit deadline; machine kept"}, now,
                       work);
            return;
        }
        const bool moving = std::any_of(_moves.begin(), _moves.end(), [&](const auto& kv) {
            return kv.second._intent == i._key;
        });
        // Requirement 10.8: complete only once it holds a voting replica.
        if (i._node && holds_voter(*i._node) && !moving && _admission_saturated.contains(i._key)) {
            count("admission", {{"outcome", "completed"}});
            transition(i, S::completed, patch_type{._note = "admitted"}, now, work);
        }
    }

    auto advance_reaping(const intent_type& i, bool expired, time_point now,
                         std::vector<std::function<void()>>& work) -> void {
        using S = capacity_intent_state;
        if (expired) {
            erase_calls(i._key);
            transition(i, S::failed, patch_type{._note = "reap deadline: decommission failing"},
                       now, work);
            return;
        }
        if (call_in_flight(i._key)) {
            return;
        }
        if (auto r = _retry_after.find(i._key); r != _retry_after.end() && now < r->second) {
            return;
        }
        std::optional<node_id_type> node = i._node;
        if (!node) {
            if (auto f = _found_by_key.find(i._key); f != _found_by_key.end()) {
                node = f->second;
            }
        }
        if (!node && !keyed_quorum_manager<QuorumMgr>) {
            // Unkeyed: anything the provider reports unreachable in the
            // group, unattributed, is the orphan.
            if (_health) {
                const auto taken = attributed_nodes();
                for (const auto& n : _health->unreachable_nodes) {
                    if (!taken.contains(n) && !joined(n, now)) {
                        node = n;
                        break;
                    }
                }
            }
            if (!node) {
                transition(i, S::failed,
                           patch_type{._note = "unattributable: manager carries no key"}, now,
                           work);
                return;
            }
        }
        if (node) {
            dispatch_decommission(i, *node, call_op::decommission_reap, now, work);
        } else {
            dispatch_lookup(i, now, work, false);
        }
    }

    auto advance_drain(const intent_type& i, bool expired, time_point now,
                       std::vector<std::function<void()>>& work) -> void {
        using S = capacity_intent_state;
        if (!i._node) {
            transition(i, S::abandoned, patch_type{._note = "drain names no machine"}, now, work);
            return;
        }
        const auto node = *i._node;
        switch (i._state) {
            case S::draining:
            case S::drained:
                if (expired) {
                    // Requirement 11.5: back into service, never half-drained.
                    cancel_moves_for(i._key, "drain deadline");
                    count("drain", {{"outcome", "abandoned"}});
                    transition(i, S::abandoned,
                               patch_type{._note = "drain deadline; returned to service"}, now,
                               work);
                    return;
                }
                if (i._state == S::draining) {
                    const bool moving =
                        std::any_of(_moves.begin(), _moves.end(),
                                    [&](const auto& kv) { return kv.second._intent == i._key; });
                    if (!moving && !holds_replica(node)) {
                        transition(i, S::drained, patch_type{._note = "holds nothing"}, now, work);
                    }
                } else if (!call_in_flight(i._key)) {
                    if (holds_replica(node)) {
                        // Someone placed a replica back: never decommission it.
                        transition(i, S::abandoned,
                                   patch_type{._note = "replica reappeared; returned to service"},
                                   now, work);
                        return;
                    }
                    transition(i, S::decommissioning,
                               patch_type{._deadline = now + _cfg.provision_deadline}, now, work);
                    dispatch_decommission(i, node, call_op::decommission_scale_in, now, work);
                }
                break;
            case S::decommissioning:
                if (expired) {
                    erase_calls(i._key);
                    transition(i, S::failed, patch_type{._note = "decommission deadline"}, now,
                               work);
                }
                break;
            default:
                break;
        }
    }

    auto maybe_compact(time_point now) -> void {
        if (_last_compact != time_point{} && now - _last_compact < std::chrono::hours{1}) {
            return;
        }
        _last_compact = now;
        const auto t = _ledger->compact(now - _cfg.ledger_retention);
        _ledger->forget(t);
    }

    // ── the snapshot ─────────────────────────────────────────────────────────

    [[nodiscard]] auto topology() const -> desired_topology<placement_group_type> {
        try {
            return _mgr->topology();
        } catch (...) {
            return {};
        }
    }

    [[nodiscard]] auto in_topology(const placement_group_type& g) const -> bool {
        const auto t = topology();
        return std::any_of(t.groups.begin(), t.groups.end(),
                           [&](const auto& x) { return x.group_id == g; });
    }

    /// The group a machine belongs to: the one it was provisioned into, else
    /// the first of its labels the topology names.
    [[nodiscard]] auto placement_of(const node_id_type& n) const
        -> std::optional<placement_group_type> {
        for (const auto& i : _intents) {
            if (i._node && *i._node == n && i._kind == capacity_intent_kind::scale_out) {
                return i._group;
            }
        }
        auto it = _nodes.find(n);
        if (it == _nodes.end()) {
            return std::nullopt;
        }
        const auto t = topology();
        for (const auto& label : it->second._report._labels) {
            if constexpr (std::equality_comparable_with<decltype(label), placement_group_type>) {
                for (const auto& g : t.groups) {
                    if (g.group_id == label) {
                        return g.group_id;
                    }
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto draining_nodes() const -> std::set<node_id_type> {
        std::set<node_id_type> out;
        for (const auto& i : _intents) {
            if (!i.terminal() && i._kind == capacity_intent_kind::scale_in && i._node) {
                out.insert(*i._node);
            }
        }
        return out;
    }

    [[nodiscard]] auto open_intents() const -> std::size_t {
        return static_cast<std::size_t>(std::count_if(_intents.begin(), _intents.end(),
                                                      [](const auto& i) { return !i.terminal(); }));
    }

    [[nodiscard]] auto open_scale_out() const -> std::size_t {
        return static_cast<std::size_t>(
            std::count_if(_intents.begin(), _intents.end(), [](const auto& i) {
                return !i.terminal() && i._kind == capacity_intent_kind::scale_out &&
                       i._state != capacity_intent_state::admitting;
            }));
    }

    [[nodiscard]] auto group_known_count(const placement_group_type& g,
                                         std::optional<node_id_type> excluding = std::nullopt) const
        -> std::size_t {
        std::size_t n = 0;
        for (const auto& [id, e] : _nodes) {
            if (excluding && id == *excluding) {
                continue;
            }
            if (auto pg = placement_of(id); pg && *pg == g) {
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] auto floor_size() const -> std::size_t {
        if (_cfg.min_cluster_size != 0) {
            return _cfg.min_cluster_size;
        }
        return topology().total_size();
    }

    [[nodiscard]] auto ceiling_size() const -> std::size_t {
        if (_cfg.max_cluster_size != 0) {
            return _cfg.max_cluster_size;
        }
        const auto base =
            std::max<std::size_t>({topology().total_size(), _initial_cluster_size, std::size_t{1}});
        return base * 3;
    }

    [[nodiscard]] auto build_snapshot(time_point now) const -> snapshot_type {
        snapshot_type s;
        s._taken_at = now;
        s._topology = topology();
        s._health = _health;
        const auto draining = draining_nodes();
        for (const auto& [id, e] : _nodes) {
            typename snapshot_type::node_view_type v;
            v._report = e._report;
            v._age = std::chrono::duration_cast<ms>(now - e._seen);
            // Stale is unknown, not absent (Requirement 2.4); the quorum
            // manager's "unreachable" wins over a fresh-looking heartbeat
            // (Requirement 2.5).
            v._stale = v._age > _cfg.node_report_staleness || reported_unreachable(id);
            v._placement_group = placement_of(id);
            v._draining = draining.contains(id);
            s._nodes.push_back(std::move(v));
        }
        for (const auto& [g, e] : _shards) {
            s._shards.push_back(e._report);
        }
        for (const auto& t : s._topology.groups) {
            typename snapshot_type::group_view_type gv;
            gv._group = t.group_id;
            gv._target = t.target_count;
            std::size_t fresh = 0;
            std::size_t known = 0;
            for (const auto& v : s._nodes) {
                if (v._placement_group && *v._placement_group == t.group_id) {
                    ++known;
                    if (!v._stale) {
                        ++fresh;
                    }
                }
            }
            gv._known = known;
            gv._live = fresh;
            if (_health) {
                for (const auto& h : _health->groups) {
                    if (h.group_id == t.group_id) {
                        gv._live = h.live_count;
                    }
                }
            }
            s._groups.push_back(gv);
        }
        s._rate_window = _cfg.split_rate_window;
        for (const auto& e : _rate_events) {
            if (now - e._at <= _cfg.split_rate_window) {
                s._splits_in_window += e._splits;
                s._merges_in_window += e._merges;
                s._replicas_added_in_window += e._added;
                s._replicas_removed_in_window += e._removed;
            }
        }
        for (const auto& [at, n] : _refusal_events) {
            if (now - at <= _cfg.split_rate_window) {
                s._capacity_refusals += n;
            }
        }
        s._in_flight_intents = open_intents();
        s._min_cluster_size = floor_size();
        s._max_cluster_size = ceiling_size();
        return s;
    }

    auto trim_windows(time_point now) -> void {
        while (!_rate_events.empty() && now - _rate_events.front()._at > _cfg.split_rate_window) {
            _rate_events.pop_front();
        }
        while (!_refusal_events.empty() &&
               now - _refusal_events.front().first > _cfg.split_rate_window) {
            _refusal_events.pop_front();
        }
        while (!_provision_calls.empty() &&
               now - _provision_calls.front() > _cfg.provision_window) {
            _provision_calls.pop_front();
        }
    }

    auto emit_gauges(const snapshot_type& s) -> void {
        gauge("cluster_size", static_cast<double>(s.cluster_size()));
        gauge("cluster_size_floor", static_cast<double>(s._min_cluster_size));
        gauge("cluster_size_ceiling", static_cast<double>(s._max_cluster_size));
        gauge("intents_in_flight", static_cast<double>(s._in_flight_intents));
        for (const auto& g : s._groups) {
            const auto name = detail::capacity::to_text(g._group);
            gauge("group_size", static_cast<double>(g._live), {{"group", name}});
            gauge("group_target", static_cast<double>(g._target), {{"group", name}});
        }
        const auto stats = [&](const char* metric, auto field) {
            std::optional<double> lo;
            std::optional<double> hi;
            double sum = 0.0;
            std::size_t n = 0;
            for (const auto& v : s._nodes) {
                if (v._stale || v._draining) {
                    continue;
                }
                const auto x = static_cast<double>(field(v._report));
                lo = lo ? std::min(*lo, x) : x;
                hi = hi ? std::max(*hi, x) : x;
                sum += x;
                ++n;
            }
            if (n == 0) {
                return;
            }
            gauge(metric, *hi, {{"stat", "max"}});
            gauge(metric, sum / static_cast<double>(n), {{"stat", "mean"}});
            gauge(metric, *hi - *lo, {{"stat", "spread"}});
        };
        stats("shards_per_node", [](const auto& r) { return r._shard_count; });
        stats("leaders_per_node", [](const auto& r) { return r._leader_count; });
    }

    // ── evaluation and bounds ────────────────────────────────────────────────

    [[nodiscard]] auto circuit_is_open(time_point now) const -> bool {
        return _circuit_until.has_value() && now < *_circuit_until;
    }

    [[nodiscard]] auto provision_budget_left(time_point now) const -> bool {
        const auto used = static_cast<std::size_t>(
            std::count_if(_provision_calls.begin(), _provision_calls.end(),
                          [&](const auto& t) { return now - t <= _cfg.provision_window; }));
        return used < _cfg.max_provisions_per_window;
    }

    auto new_key(time_point now) -> std::string {
        std::uniform_int_distribution<std::uint64_t> d;
        std::ostringstream os;
        os << "cap-" << _token << "-"
           << std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
           << "-" << std::hex << d(_rng);
        return os.str();
    }

    /// Placement groups ranked by design §8's score, best first.
    [[nodiscard]] auto rank_groups(const snapshot_type& s, time_point now) const
        -> std::vector<placement_group_type> {
        struct scored {
            placement_group_type _g;
            double _score;
        };
        std::vector<scored> out;
        std::size_t total_target = 0;
        std::size_t total_live = 0;
        std::map<placement_group_type, std::size_t> pending;
        for (const auto& i : _intents) {
            if (!i.terminal() && i._kind == capacity_intent_kind::scale_out &&
                i._state != capacity_intent_state::admitting) {
                ++pending[i._group];
            }
        }
        for (const auto& g : s._groups) {
            total_target += g._target;
            total_live += g._live + pending[g._group];
        }
        for (const auto& g : s._groups) {
            const auto live = g._live + pending[g._group];
            const double target_ratio = total_target == 0 ? 0.0
                                                          : static_cast<double>(g._target) /
                                                                static_cast<double>(total_target);
            const double live_ratio =
                total_live == 0 ? 0.0 : static_cast<double>(live) / static_cast<double>(total_live);
            double bonus = 0.0;
            if (live < g._target && g._target > 0) {
                bonus =
                    1.0 + static_cast<double>(g._target - live) / static_cast<double>(g._target);
            }
            double penalty = 0.0;
            if (auto it = _placement_refused.find(g._group); it != _placement_refused.end()) {
                const auto age = std::chrono::duration<double>(now - it->second).count();
                const auto decay =
                    std::chrono::duration<double>(_cfg.placement_refusal_decay).count();
                penalty = _cfg.placement_refusal_penalty * std::exp(-age / std::max(decay, 1e-9));
            }
            out.push_back({g._group, (target_ratio - live_ratio) + bonus - penalty});
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            if (a._score != b._score) {
                return a._score > b._score;
            }
            return a._g < b._g;  // deterministic tie-break: lowest id
        });
        std::vector<placement_group_type> ranked;
        for (const auto& x : out) {
            ranked.push_back(x._g);
        }
        return ranked;
    }

    struct bound_check {
        std::string _refused;  // empty: passed
        std::string _summary;
    };

    auto check_scale_out(const snapshot_type& s, time_point now) const -> bound_check {
        bound_check b;
        const auto add = [&](const std::string& name, bool ok, const std::string& detail) {
            b._summary +=
                (b._summary.empty() ? "" : ";") + name + "=" + detail + (ok ? ":ok" : ":refused");
            if (!ok && b._refused.empty()) {
                b._refused = name;
            }
        };
        const auto size = s.cluster_size() + open_scale_out();
        const auto ceiling = ceiling_size();
        add("quorum_lost", !(_health && _health->status == quorum_status::lost),
            _health ? detail::capacity::to_text(static_cast<int>(_health->status)) : "unknown");
        add("stale_inventory", s.fresh_node_count() > 0, std::to_string(s.fresh_node_count()));
        add("circuit_open", !circuit_is_open(now), std::to_string(_consecutive_failures));
        add("backoff", now >= _backoff_until, "");
        add("max_cluster_size", size + 1 <= ceiling,
            std::to_string(size) + "/" + std::to_string(ceiling));
        add("max_in_flight_intents", open_intents() < _cfg.max_in_flight_intents,
            std::to_string(open_intents()) + "/" + std::to_string(_cfg.max_in_flight_intents));
        add("min_provider_call_interval",
            !_has_capacity_call || now - _last_capacity_call >= _cfg.min_provider_call_interval,
            "");
        add("provision_budget", provision_budget_left(now),
            std::to_string(_provision_calls.size()) + "/" +
                std::to_string(_cfg.max_provisions_per_window));
        return b;
    }

    auto check_scale_in(const snapshot_type& s, const decision_type& d) const -> bound_check {
        bound_check b;
        const auto add = [&](const std::string& name, bool ok, const std::string& detail) {
            b._summary +=
                (b._summary.empty() ? "" : ";") + name + "=" + detail + (ok ? ":ok" : ":refused");
            if (!ok && b._refused.empty()) {
                b._refused = name;
            }
        };
        add("scale_in_disabled", _cfg.scale_in_enabled, "");
        add("quorum_not_healthy", _health && _health->status == quorum_status::healthy,
            _health ? detail::capacity::to_text(static_cast<int>(_health->status)) : "unknown");
        add("drain_in_progress", draining_nodes().empty(), "");
        add("intent_in_flight", open_intents() == 0, std::to_string(open_intents()));
        const auto size = s.cluster_size();
        add("min_cluster_size", size >= floor_size() + 1,
            std::to_string(size) + "/" + std::to_string(floor_size()));
        const auto node = *d.node();
        add("unknown_node", _nodes.contains(node), detail::capacity::to_text(node));
        if (auto g = placement_of(node)) {
            if (const auto* gv = s.find_group(*g)) {
                add("topology_floor", gv->_live >= gv->_target + 1,
                    std::to_string(gv->_live) + "/" + std::to_string(gv->_target));
            }
        }
        return b;
    }

    auto decision_record(const decision_type& d, const std::string& key,
                         const std::optional<placement_group_type>& group,
                         const std::string& bounds, const std::string& outcome) -> void {
        dims kv;
        kv.emplace_back("action", to_string(d.action()));
        kv.emplace_back("reason", to_string(d.reason()));
        std::string signals;
        for (const auto& sig : d.evidence()._signals) {
            std::ostringstream os;
            os << sig._name << "=" << sig._value << "/" << sig._threshold;
            signals += (signals.empty() ? "" : ",") + os.str();
        }
        kv.emplace_back("signals", signals);
        if (const auto& p = d.evidence()._projection) {
            std::ostringstream os;
            os << "current=" << p->_current_shards_per_node << ",rate=" << p->_split_rate_per_minute
               << ",horizon=" << p->_horizon_minutes << ",nodes=" << p->_live_node_count
               << ",projected=" << p->_projected_shards_per_node;
            kv.emplace_back("projection", os.str());
        } else {
            kv.emplace_back("projection", "none");
        }
        kv.emplace_back("capacity_refusals", std::to_string(d.evidence()._capacity_refusals));
        kv.emplace_back("group", group ? detail::capacity::to_text(*group) : "none");
        if (d.node()) {
            kv.emplace_back("node", detail::capacity::to_text(*d.node()));
        }
        kv.emplace_back("idempotency_key", key.empty() ? "none" : key);
        kv.emplace_back("bounds", bounds);
        kv.emplace_back("dry_run", _cfg.dry_run ? "true" : "false");
        kv.emplace_back("outcome", outcome);
        std::string line;
        for (const auto& [k, v] : kv) {
            line += (line.empty() ? "" : " ") + k + "=" + v;
        }
        _last_record = line;
        log(log_level::info, "capacity_decision", kv);
    }

    auto evaluate(time_point now, std::vector<std::function<void()>>& work) -> void {
        _last_eval = now;
        _dirty = false;
        trim_windows(now);
        const auto s = build_snapshot(now);
        emit_gauges(s);
        // Requirement 3.5: not within cooldown() of the last non-hold.
        if (_has_non_hold && now - _last_non_hold < _policy.cooldown()) {
            return;
        }
        decision_type d;
        try {
            d = _policy.evaluate(s);
        } catch (const std::exception& e) {
            count("policy_error");
            log(log_level::error, "capacity_policy_threw", {{"error", e.what()}});
            return;
        }
        if (d.is_hold()) {
            return;
        }
        _last_non_hold = now;
        _has_non_hold = true;
        count("decision", {{"action", to_string(d.action())}, {"reason", to_string(d.reason())}});
        if (d.is_scale_out()) {
            decide_scale_out(d, s, now, work);
        } else {
            decide_scale_in(d, s, now, work);
        }
    }

    auto decide_scale_out(const decision_type& d, const snapshot_type& s, time_point now,
                          std::vector<std::function<void()>>& work) -> void {
        for (std::size_t n = 0; n < d.count(); ++n) {
            const auto b = check_scale_out(s, now);
            if (!b._refused.empty()) {
                refuse(b._refused, b._summary);
                decision_record(d, {}, d.group(), b._summary, "refused:" + b._refused);
                return;
            }
            std::vector<placement_group_type> ranked;
            if (d.group()) {
                // A named group is a constraint: honoured or refused, never
                // silently re-routed.
                if (!in_topology(*d.group())) {
                    refuse("group_not_in_topology", detail::capacity::to_text(*d.group()));
                    decision_record(d, {}, d.group(), b._summary, "refused:group_not_in_topology");
                    return;
                }
                ranked.push_back(*d.group());
            } else {
                ranked = rank_groups(s, now);
            }
            if (ranked.empty()) {
                refuse("no_placement_group");
                decision_record(d, {}, std::nullopt, b._summary, "refused:no_placement_group");
                return;
            }
            const auto key = new_key(now);
            const auto group = ranked.front();
            if (_cfg.dry_run) {
                decision_record(d, key, group, b._summary, "dry_run");
                return;  // one record per decision; nothing is called
            }
            intent_type i;
            i._key = key;
            i._kind = capacity_intent_kind::scale_out;
            i._state = capacity_intent_state::requested;
            i._reason = d.reason();
            i._evidence = d.evidence();
            i._group = group;
            i._fencing_token = _token;
            i._observed_cluster_size = s.cluster_size();
            i._created_at = now;
            i._updated_at = now;
            i._deadline = now + _cfg.provision_deadline;
            i._note = "decided";
            _fallback[key] = std::vector<placement_group_type>(ranked.begin() + 1, ranked.end());
            decision_record(d, key, group, b._summary, "recorded");
            // Reserve the interval now: the second machine of a count>1
            // decision must clear it too.
            _last_capacity_call = now;
            _has_capacity_call = true;
            record(std::move(i), now, work);
        }
    }

    auto decide_scale_in(const decision_type& d, const snapshot_type& s, time_point now,
                         std::vector<std::function<void()>>& work) -> void {
        const auto b = check_scale_in(s, d);
        if (!b._refused.empty()) {
            refuse(b._refused, b._summary);
            decision_record(d, {}, d.group(), b._summary, "refused:" + b._refused);
            return;
        }
        const auto key = new_key(now);
        if (_cfg.dry_run) {
            decision_record(d, key, d.group(), b._summary, "dry_run");
            return;
        }
        intent_type i;
        i._key = key;
        i._kind = capacity_intent_kind::scale_in;
        i._state = capacity_intent_state::draining;
        i._reason = d.reason();
        i._evidence = d.evidence();
        i._group = d.group() ? *d.group() : placement_group_type{};
        i._node = d.node();
        i._fencing_token = _token;
        i._observed_cluster_size = s.cluster_size();
        i._created_at = now;
        i._updated_at = now;
        i._deadline = now + _cfg.drain_deadline;
        i._note = "drain";
        decision_record(d, key, d.group(), b._summary, "recorded");
        count("drain", {{"outcome", "started"}});
        record(std::move(i), now, work);
    }

    // ── the rebalance planner (design §4) ────────────────────────────────────

    [[nodiscard]] auto fresh_shard(const GroupId& g, time_point now) const
        -> const shard_report_type* {
        auto it = _shards.find(g);
        if (it == _shards.end() || now - it->second._seen > _cfg.node_report_staleness) {
            return nullptr;
        }
        return &it->second._report;
    }

    [[nodiscard]] auto movable(const shard_report_type& r, time_point now) const -> bool {
        if (r._operation != shard_operation_state::stable) {
            return false;
        }
        if (r._down_replica_count > 0 || !r._down_replicas.empty() ||
            !r._pending_replicas.empty()) {
            return false;
        }
        if (auto c = _cooldown.find(r.group_id()); c != _cooldown.end() && now < c->second) {
            return false;
        }
        return !_moves.contains(r.group_id());
    }

    [[nodiscard]] auto load_of(const node_id_type& n, capacity_reason reason) const -> double {
        auto it = _nodes.find(n);
        if (it == _nodes.end()) {
            return 0.0;
        }
        const auto& r = it->second._report;
        switch (reason) {
            case capacity_reason::storage:
                return r._capacity_bytes == 0 ? 0.0
                                              : static_cast<double>(r._used_bytes) /
                                                    static_cast<double>(r._capacity_bytes);
            case capacity_reason::load:
            case capacity_reason::overload:
                return r._write_bytes_per_sec;
            default:
                return static_cast<double>(r._shard_count);
        }
    }

    [[nodiscard]] auto moves_to(const node_id_type& n) const -> std::size_t {
        return static_cast<std::size_t>(std::count_if(
            _moves.begin(), _moves.end(), [&](const auto& kv) { return kv.second._to == n; }));
    }

    [[nodiscard]] auto moves_from(const node_id_type& n) const -> std::size_t {
        return static_cast<std::size_t>(std::count_if(
            _moves.begin(), _moves.end(), [&](const auto& kv) { return kv.second._from == n; }));
    }

    auto cancel_moves_for(const std::string& key, const std::string& why) -> void {
        for (auto& [g, m] : _moves) {
            if (m._intent == key) {
                abandon_move(m, why);
            }
        }
    }

    /// An admission move whose target already votes, refused the leader
    /// transfer that would let it remove its source (a transport without
    /// TimeoutNow answers `unsupported`). Abandoning it there would leave the
    /// shard one voter over and count no move, and where one host leads every
    /// shard every admission move ends that way: the new machine joins every
    /// group and displaces nothing. Instead, displace another voter the
    /// leader can remove directly, preferring the source's placement group so
    /// failure-domain spread is what the plan intended, then the most loaded.
    /// Only for admissions: a drain or an overload move must empty its own
    /// source. Returns false when no such voter exists.
    auto displace_another_voter(move& m, time_point now) -> bool {
        if (m._drain || m._remove_only || m._rollback || m._done || !is_admission(m._intent)) {
            return false;
        }
        const auto* d = descriptor_of(m._group);
        const auto* r = fresh_shard(m._group, now);
        if (d == nullptr || r == nullptr || !d->has_voter(m._to) || !d->has_replica(m._from) ||
            r->leader() != m._from) {
            return false;
        }
        const auto draining = draining_nodes();
        const auto home = placement_of(m._from);
        std::optional<node_id_type> alt;
        std::pair<int, double> alt_key{};
        for (const auto& v : d->voters()) {
            if (v == m._from || v == m._to || draining.contains(v) || !sending_capacity(v)) {
                continue;
            }
            // Lower is better: same placement group first, then most loaded.
            const std::pair<int, double> key{home && placement_of(v) == home ? 0 : 1,
                                             -load_of(v, m._reason)};
            if (!alt || key < alt_key) {
                alt = v;
                alt_key = key;
            }
        }
        if (!alt) {
            return false;
        }
        log(log_level::info, "capacity_move_retargeted",
            {{"group", detail::capacity::to_text(m._group)},
             {"from", detail::capacity::to_text(m._from)},
             {"instead", detail::capacity::to_text(*alt)},
             {"to", detail::capacity::to_text(m._to)},
             {"why", "leader transfer unsupported"}});
        count("move", {{"outcome", "retargeted"}});
        m._from = *alt;
        m._last_emit = time_point{};
        m._retry_after = time_point{};
        return true;
    }

    [[nodiscard]] auto is_admission(const std::string& key) const -> bool {
        return !key.empty() && std::any_of(_intents.begin(), _intents.end(), [&](const auto& i) {
            return i._key == key && i._kind == capacity_intent_kind::scale_out;
        });
    }

    /// Stop a move. A learner it already added is removed again, so an
    /// abandoned admission leaves the group as it found it; nothing is ever
    /// left in joint configuration because every step is one plain Raft
    /// membership change (Requirement 10.7).
    auto abandon_move(move& m, const std::string& why) -> void {
        count("move", {{"outcome", "abandoned"}});
        log(log_level::info, "capacity_move_abandoned",
            {{"group", detail::capacity::to_text(m._group)}, {"reason", why}});
        m._intent.clear();
        m._last_emit = time_point{};
        m._retry_after = time_point{};
        const auto* d = descriptor_of(m._group);
        const bool added_learner =
            !m._remove_only && d && d->has_replica(m._to) && !d->has_voter(m._to);
        if (added_learner) {
            m._rollback = true;
        } else {
            m._done = true;
        }
    }

    [[nodiscard]] auto descriptor_of(const GroupId& g) const -> const descriptor_type* {
        auto it = _descriptors.find(g);
        return it == _descriptors.end() ? nullptr : &it->second;
    }

    /// Queues one operator, unless this step was asked for another host's
    /// groups only. Returns whether it was queued: a step skipped here must
    /// not count as sent, or the leader's own heartbeat would wait out the
    /// retry interval for an operator nobody received.
    auto emit(const shard_report_type& r, shard_operator_kind<GroupId, Key, node_id_type> op,
              std::vector<operation_type>& ops) -> bool {
        if (_addressable && !_addressable->contains(r.group_id())) {
            return false;
        }
        // Never a membership or leadership change on a shard mid-split or
        // mid-merge (Requirement 10.5): checked here, at the one place every
        // operator leaves, so a move that was planned while the shard was
        // stable cannot carry a promotion or a rollback into its split.
        if (r._operation != shard_operation_state::stable) {
            return false;
        }
        operation_type o;
        o._group_id = r.group_id();
        o._operation_id = _next_op_id++;
        o._epoch = r.epoch();
        o._operator = std::move(op);
        _op_owner[o._operation_id] = r.group_id();
        ops.push_back(std::move(o));
        return true;
    }

    auto plan(time_point now) -> std::vector<operation_type> {
        std::vector<operation_type> ops;
        progress_moves(now, ops);
        plan_drains(now, ops);
        plan_admissions(now);
        return ops;
    }

    auto progress_moves(time_point now, std::vector<operation_type>& ops) -> void {
        for (auto it = _moves.begin(); it != _moves.end();) {
            auto& m = it->second;
            if (m._done) {
                it = _moves.erase(it);
                continue;
            }
            const auto* r = fresh_shard(m._group, now);
            if (r == nullptr) {
                ++it;
                continue;
            }
            const auto& d = r->descriptor();
            const bool may_emit =
                now >= m._retry_after && (m._last_emit == time_point{} ||
                                          now - m._last_emit >= _cfg.operator_retry_interval);
            if (m._rollback) {
                if (!d.has_replica(m._to) || d.has_voter(m._to)) {
                    it = _moves.erase(it);
                    continue;
                }
                if (may_emit) {
                    if (emit(*r, remove_replica_operator<node_id_type>{._node = m._to}, ops)) {
                        m._last_emit = now;
                    }
                }
                ++it;
                continue;
            }
            if (m._remove_only) {
                if (!d.has_replica(m._from)) {
                    count("shards_moved");
                    it = _moves.erase(it);
                    continue;
                }
                if (may_emit && r->leader() != m._from) {
                    if (emit(*r, remove_replica_operator<node_id_type>{._node = m._from}, ops)) {
                        m._last_emit = now;
                    }
                }
                ++it;
                continue;
            }
            const bool to_voter = d.has_voter(m._to);
            const bool to_learner = d.has_replica(m._to) && !to_voter;
            const bool from_present = d.has_replica(m._from);
            if (to_voter && !from_present) {
                count("shards_moved");
                log(log_level::info, "capacity_move_done",
                    {{"group", detail::capacity::to_text(m._group)},
                     {"from", detail::capacity::to_text(m._from)},
                     {"to", detail::capacity::to_text(m._to)}});
                it = _moves.erase(it);
                continue;
            }
            if (!may_emit) {
                ++it;
                continue;
            }
            std::optional<shard_operator_kind<GroupId, Key, node_id_type>> op;
            if (!d.has_replica(m._to)) {
                if (r->_operation != shard_operation_state::stable) {
                    ++it;
                    continue;
                }
                op = add_replica_operator<node_id_type>{._node = m._to, ._as_learner = true};
            } else if (to_learner) {
                // Promote only once caught up (Requirement 10.3).
                if (!detail::capacity::contains(r->_pending_replicas, m._to)) {
                    op = add_replica_operator<node_id_type>{._node = m._to, ._as_learner = false};
                }
            } else if (to_voter && from_present) {
                // Never remove before the replacement votes; never remove with
                // another replica down or behind (Requirement 11.3).
                const bool others_healthy =
                    r->_down_replicas.empty() &&
                    std::all_of(r->_pending_replicas.begin(), r->_pending_replicas.end(),
                                [&](const auto& p) { return p == m._from; });
                if (others_healthy && r->_operation == shard_operation_state::stable) {
                    if (r->leader() == m._from) {
                        op = transfer_leader_operator<node_id_type>{._to = m._to};
                    } else {
                        op = remove_replica_operator<node_id_type>{._node = m._from};
                    }
                }
            }
            if (op) {
                if (emit(*r, std::move(*op), ops)) {
                    m._last_emit = now;
                }
            }
            ++it;
        }
    }

    auto plan_drains(time_point now, std::vector<operation_type>& ops) -> void {
        for (const auto& i : _intents) {
            if (i.terminal() || i._kind != capacity_intent_kind::scale_in ||
                i._state != capacity_intent_state::draining || !i._node ||
                _keys_in_write.contains(i._key)) {
                continue;
            }
            const auto d = *i._node;
            // One group at a time (Requirement 11.1).
            const bool moving = std::any_of(_moves.begin(), _moves.end(), [&](const auto& kv) {
                return kv.second._intent == i._key;
            });
            if (moving || _moves.size() >= _cfg.max_moves_cluster_wide) {
                continue;
            }
            const auto draining = draining_nodes();
            for (const auto& [g, e] : _shards) {
                if (now - e._seen > _cfg.node_report_staleness) {
                    continue;
                }
                const auto& r = e._report;
                const auto& desc = r.descriptor();
                if (!desc.has_replica(d) || !movable(r, now)) {
                    continue;
                }
                // Leadership first (Requirement 11.1).
                if (r.leader() == d) {
                    auto sent = _leader_transfer_sent.find(g);
                    if (sent != _leader_transfer_sent.end() &&
                        now - sent->second < _cfg.operator_retry_interval) {
                        break;
                    }
                    std::optional<node_id_type> to;
                    for (const auto& v : desc.voters()) {
                        if (v != d && !draining.contains(v) &&
                            (!to || leaders_on(v) < leaders_on(*to))) {
                            to = v;
                        }
                    }
                    if (to) {
                        if (emit(r, transfer_leader_operator<node_id_type>{._to = *to}, ops)) {
                            _leader_transfer_sent[g] = now;
                        }
                    }
                    break;
                }
                if (!desc.has_voter(d)) {
                    // A learner only: removal costs no quorum.
                    _moves[g] = move{._group = g,
                                     ._from = d,
                                     ._to = d,
                                     ._intent = i._key,
                                     ._reason = i._reason,
                                     ._started = now,
                                     ._drain = true,
                                     ._remove_only = true};
                    break;
                }
                auto target = drain_target(desc, d, now);
                if (!target) {
                    continue;
                }
                _moves[g] = move{._group = g,
                                 ._from = d,
                                 ._to = *target,
                                 ._intent = i._key,
                                 ._reason = i._reason,
                                 ._started = now,
                                 ._drain = true};
                count("move", {{"outcome", "planned"}, {"purpose", "drain"}});
                log(log_level::info, "capacity_move_planned",
                    {{"group", detail::capacity::to_text(g)},
                     {"from", detail::capacity::to_text(d)},
                     {"to", detail::capacity::to_text(*target)},
                     {"why", "drain"}});
                break;
            }
        }
    }

    [[nodiscard]] auto leaders_on(const node_id_type& n) const -> std::size_t {
        auto it = _nodes.find(n);
        return it == _nodes.end() ? 0 : it->second._report._leader_count;
    }

    [[nodiscard]] auto receiving_capacity(const node_id_type& n) const -> std::size_t {
        auto it = _nodes.find(n);
        const auto busy = it == _nodes.end() ? 0 : it->second._report._receiving_snapshot_count;
        const auto used = busy + moves_to(n);
        return used >= _cfg.max_moves_per_target ? 0 : _cfg.max_moves_per_target - used;
    }

    [[nodiscard]] auto sending_capacity(const node_id_type& n) const -> bool {
        auto it = _nodes.find(n);
        const auto busy = it == _nodes.end() ? 0 : it->second._report._sending_snapshot_count;
        return busy + moves_from(n) < _cfg.max_moves_per_target;
    }

    /// Where a drained replica goes: the least-loaded fresh machine holding
    /// none of the shard, preferring the drained machine's own placement
    /// group so failure-domain spread is kept.
    [[nodiscard]] auto drain_target(const descriptor_type& desc, const node_id_type& from,
                                    time_point now) const -> std::optional<node_id_type> {
        const auto draining = draining_nodes();
        const auto home = placement_of(from);
        std::optional<node_id_type> best;
        std::tuple<int, std::size_t, node_id_type> best_key{};
        for (const auto& [id, e] : _nodes) {
            if (id == from || draining.contains(id) || desc.has_replica(id) || !joined(id, now) ||
                reported_unreachable(id) || receiving_capacity(id) == 0 || e._report._overloaded) {
                continue;
            }
            const int same = home && placement_of(id) == home ? 0 : 1;
            const auto key = std::tuple{same, e._report._shard_count + moves_to(id), id};
            if (!best || key < best_key) {
                best = id;
                best_key = key;
            }
        }
        return best;
    }

    auto plan_admissions(time_point now) -> void {
        // Targets: machines being admitted, in key order.
        for (const auto& i : _intents) {
            if (i.terminal() || i._kind != capacity_intent_kind::scale_out ||
                i._state != capacity_intent_state::admitting || !i._node ||
                _keys_in_write.contains(i._key)) {
                continue;
            }
            const auto t = *i._node;
            if (!joined(t, now) || reported_unreachable(t)) {
                continue;
            }
            // The mean the target is filled towards: shards per fresh,
            // non-draining machine, counting moves already planned.
            const auto draining = draining_nodes();
            double sum = 0.0;
            std::size_t n = 0;
            for (const auto& [id, e] : _nodes) {
                if (draining.contains(id) || now - e._seen > _cfg.node_report_staleness) {
                    continue;
                }
                sum += static_cast<double>(e._report._shard_count);
                ++n;
            }
            const auto mean = n == 0 ? 0.0 : sum / static_cast<double>(n);
            const auto target_count = [&] {
                std::size_t held = 0;
                for (const auto& [g, desc] : _descriptors) {
                    if (desc.has_replica(t)) {
                        ++held;
                    }
                }
                return held + moves_to(t);
            };
            bool planned_any = false;
            bool blocked = false;
            while (true) {
                // Requirement 10.8 needs at least one voter, whatever the mean.
                const bool need_one = !holds_voter(t) && moves_to(t) == 0;
                if (!need_one && static_cast<double>(target_count()) >= std::floor(mean)) {
                    break;
                }
                if (_moves.size() >= _cfg.max_moves_cluster_wide || receiving_capacity(t) == 0) {
                    blocked = true;
                    break;
                }
                auto pick = pick_shard_for(t, i._reason, now);
                if (!pick) {
                    break;
                }
                const auto [group, from] = *pick;
                _moves[group] = move{._group = group,
                                     ._from = from,
                                     ._to = t,
                                     ._intent = i._key,
                                     ._reason = i._reason,
                                     ._started = now};
                planned_any = true;
                count("move", {{"outcome", "planned"}, {"purpose", "admission"}});
                log(log_level::info, "capacity_move_planned",
                    {{"group", detail::capacity::to_text(group)},
                     {"from", detail::capacity::to_text(from)},
                     {"to", detail::capacity::to_text(t)},
                     {"why", to_string(i._reason)},
                     {"source_load", std::to_string(load_of(from, i._reason))}});
            }
            if (!planned_any && !blocked &&
                std::none_of(_moves.begin(), _moves.end(),
                             [&](const auto& kv) { return kv.second._intent == i._key; })) {
                _admission_saturated.insert(i._key);
            }
        }
    }

    /// The shard to move onto `t` (Requirement 10.6): from the most loaded
    /// source by the decision's own measure, the largest contributor first;
    /// among equals, one the source does not lead, then group id. Moving a
    /// replica off its leader costs a leadership transfer (an election, and
    /// on a transport without TimeoutNow an operator the host refuses), so
    /// it is the last choice, never a reason to skip the shard.
    [[nodiscard]] auto pick_shard_for(const node_id_type& t, capacity_reason reason,
                                      time_point now) const
        -> std::optional<std::pair<GroupId, node_id_type>> {
        std::vector<std::pair<double, node_id_type>> sources;
        const auto draining = draining_nodes();
        for (const auto& [id, e] : _nodes) {
            if (id == t || now - e._seen > _cfg.node_report_staleness || !sending_capacity(id)) {
                continue;
            }
            sources.emplace_back(load_of(id, reason), id);
        }
        std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) {
                return a.first > b.first;
            }
            return a.second < b.second;
        });
        for (const auto& [load, src] : sources) {
            struct candidate {
                double _weight;
                bool _led_by_source;
                GroupId _group;
            };
            std::vector<candidate> shards;
            for (const auto& [g, e] : _shards) {
                if (now - e._seen > _cfg.node_report_staleness) {
                    continue;
                }
                const auto& r = e._report;
                const auto& d = r.descriptor();
                if (!d.has_voter(src) || d.has_replica(t) || !movable(r, now)) {
                    continue;
                }
                double weight = 0.0;
                if (reason == capacity_reason::storage) {
                    weight = static_cast<double>(r._approximate_size_bytes);
                } else if (reason == capacity_reason::load || reason == capacity_reason::overload) {
                    weight = r._write_bytes_per_sec;
                }
                shards.push_back({weight, r.leader() == src, g});
            }
            std::sort(shards.begin(), shards.end(), [](const auto& a, const auto& b) {
                if (a._weight != b._weight) {
                    return a._weight > b._weight;
                }
                if (a._led_by_source != b._led_by_source) {
                    return !a._led_by_source;
                }
                return a._group < b._group;
            });
            if (!shards.empty()) {
                return std::pair{shards.front()._group, src};
            }
        }
        return std::nullopt;
    }
};

}  // namespace kythira
