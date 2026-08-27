// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file multi_raft_impl.hpp
/// @brief Out-of-line definitions for `multi_raft<Types, Key, GroupId>`.
///
/// Include this rather than `multi_raft.hpp` to use the class; the split
/// follows `raft.hpp`'s own declaration/definition pattern.

#include <raft/fault_injection.hpp>
#include <raft/multi_raft.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>
#include <condition_variable>
#include <stdexcept>
#include <utility>

namespace kythira {

// ─────────────────────────────────────────────────────────────────────────────
// Construction and lifecycle
// ─────────────────────────────────────────────────────────────────────────────

template<raft_types Types, shard_key Key, raft_group_id GroupId>
multi_raft<Types, Key, GroupId>::multi_raft(config_type cfg)
    : _cfg(std::move(cfg)),
      _client(std::move(_cfg.network_client)),
      _demux(std::move(_cfg.network_server)),
      _stripe_count(_cfg.executor_stripes == 0 ? striped_serial_executor::default_stripe_count()
                                               : _cfg.executor_stripes),
      _executor(std::make_shared<striped_serial_executor>(_stripe_count)) {
    if (!_cfg.store_factory) {
        throw std::invalid_argument(
            "multi_raft: store_factory is required — a group's store is scoped by its "
            "construction argument, so one cannot be derived from a prototype");
    }

    if (!_cfg.host_data_dir.empty()) {
        std::filesystem::create_directories(_cfg.host_data_dir);
        _tombstones = tombstone_set_type::load_from_file(_cfg.host_data_dir / "tombstones");
    }

    // A tombstoned group's messages are dropped inside the transport, before
    // the unknown-group path can create a replica. That ordering is the whole
    // protection: re-creating a merged-away group gives two shards one range.
    _demux.set_tombstone_predicate([this](const GroupId& g) { return this->is_tombstoned(g); });

    // Every inbound message is a wake signal (Requirement 5.5).
    _demux.set_message_observer([this](const GroupId& g) { this->note_activity(g); });

    _demux.set_unknown_group_handler(
        [this](const GroupId& g) { return this->handle_unknown_group(g); });

    _automatic_enabled.store(_cfg.automatic_split_merge_enabled);
    _last_policy_run = std::chrono::steady_clock::now();
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
multi_raft<Types, Key, GroupId>::~multi_raft() {
    stop();
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::start() -> void {
    if (_running.exchange(true)) {
        return;
    }
    {
        std::lock_guard lock(_executor_mutex);
        if (!_executor) {
            _executor = std::make_shared<striped_serial_executor>(_stripe_count);
        }
    }
    _demux.start();
    for (const auto& g : all_groups()) {
        g->_node->start();
        note_activity(*g);
    }
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::stop() -> void {
    if (!_running.exchange(false)) {
        return;
    }

    auto executor = current_executor();

    // Drain before stopping the nodes: a queued phase task holds a group_ptr
    // and would otherwise run against a node that stop() has already torn down.
    if (executor) {
        executor->drain_all();
    }

    for (const auto& g : all_groups()) {
        g->_node->stop();
    }
    _demux.stop();

    // Released rather than merely stopped, so `start()` can build a fresh pool.
    // `node` has no destructor, so a joinable thread surviving stop() aborts
    // the process at teardown rather than merely leaking — leaving nothing
    // joinable is the contract, and restartability is the other half of it.
    //
    // The member is cleared first and the local copy stopped after, so a
    // `tick()` that took its own copy a moment ago keeps a live pool rather
    // than a dangling pointer.
    {
        std::lock_guard lock(_executor_mutex);
        _executor.reset();
    }
    if (executor) {
        executor->stop();
    }
    persist_tombstones();
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::current_executor() const
    -> std::shared_ptr<striped_serial_executor> {
    std::lock_guard lock(_executor_mutex);
    return _executor;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::is_running() const -> bool {
    return _running.load();
}

// ─────────────────────────────────────────────────────────────────────────────
// Registry
// ─────────────────────────────────────────────────────────────────────────────

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::create_group(const descriptor_type& descriptor) -> void {
    create_group(descriptor, {});
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::create_group(
    const descriptor_type& descriptor,
    const std::function<void(typename Types::persistence_engine_type&)>& seed) -> void {
    const auto& group = descriptor._group_id;

    {
        std::shared_lock lock(_registry_mutex);
        if (_groups.contains(group)) {
            throw unknown_shard_exception<GroupId>{group, "already exists on this node"};
        }
    }

    auto state = std::make_shared<group_state>(_cfg.latency_window_count, _cfg.load_split);
    state->_group_id = group;
    state->_descriptor = descriptor;
    state->_stripe = std::hash<GroupId>{}(group) % _stripe_count;

    // Seeded BEFORE the node opens it: a split child begins at the parent's
    // apply index with an empty log and a synthetic snapshot, and
    // `node::start()` reads that snapshot during initialize_from_storage().
    auto store = _cfg.store_factory(group);
    if (seed) {
        seed(store);
    }

    node_config<group_types> node_cfg{
        .node_id = _cfg.node_id,
        .network_client = typename group_types::network_client_type{_client, group},
        .network_server = typename group_types::network_server_type{_demux, group},
        .persistence = std::move(store),
        // Each group gets its OWN logger, metrics sink and membership manager,
        // built by a factory or default-constructed. Never a copy of the host's
        // own: `console_logger` is move-only, and several metrics backends own
        // a connection that two groups must not share.
        .logger = _cfg.logger_factory ? _cfg.logger_factory(group) : typename Types::logger_type{},
        .metrics =
            _cfg.metrics_factory ? _cfg.metrics_factory(group) : typename Types::metrics_type{},
        .membership = _cfg.membership_factory ? _cfg.membership_factory(group)
                                              : typename Types::membership_manager_type{},
    };
    if (_cfg.state_machine_factory) {
        node_cfg.state_machine = _cfg.state_machine_factory(group);
    }
    node_cfg.config = _cfg.config;

    state->_node = std::make_unique<group_node_type>(std::move(node_cfg));
    if (!descriptor._voters.empty()) {
        state->_node->set_cluster_configuration(descriptor._voters);
    }
    install_admin_handler(*state);
    note_activity(*state);

    {
        std::unique_lock lock(_registry_mutex);
        auto [it, inserted] = _groups.emplace(group, state);
        if (!inserted) {
            throw unknown_shard_exception<GroupId>{group, "already exists on this node"};
        }
    }
    {
        std::unique_lock lock(_map_mutex);
        _shard_map.upsert(descriptor);
    }

    // A group created after a merge tombstoned it is a deliberate re-creation
    // (an operator, or the placement driver placing a fresh replica here), so
    // the tombstone must go — otherwise the transport would drop its messages.
    {
        std::lock_guard lock(_tombstone_mutex);
        _tombstones.erase(group);
    }

    if (_running.load()) {
        state->_node->start();
    }
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::create_group(const GroupId& group,
                                                   const std::vector<node_id_type>& voters)
    -> void {
    create_group(descriptor_type{._group_id = group,
                                 ._range = unbounded_shard_range<Key>(),
                                 ._epoch = shard_epoch{},
                                 ._voters = voters,
                                 ._learners = {},
                                 ._leader_hint = std::nullopt});
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::destroy_group(const GroupId& group, tombstone_reason reason)
    -> bool {
    group_ptr state;
    {
        std::unique_lock lock(_registry_mutex);
        auto it = _groups.find(group);
        if (it == _groups.end()) {
            return false;
        }
        state = it->second;
        _groups.erase(it);
    }

    // Order matters. Unregister first so the transport stops delivering, then
    // tombstone so a message already in flight is dropped rather than
    // re-creating the replica through the unknown-group path, and only then
    // drain and destroy.
    _demux.unregister_group(group);
    {
        std::lock_guard lock(_tombstone_mutex);
        _tombstones.insert(group, reason, std::chrono::system_clock::now());
    }
    persist_tombstones();

    // Drain the group's own stripe so nothing is executing inside the node we
    // are about to destroy. `post_and_wait` refuses to run from that stripe,
    // which is exactly the case that would deadlock.
    if (auto executor = current_executor();
        executor && !executor->stopped() &&
        striped_serial_executor::current_stripe() != state->_stripe) {
        executor->post_and_wait(state->_stripe, [] {});
    }

    state->_node->stop();
    state->_node.reset();

    {
        std::unique_lock lock(_map_mutex);
        _shard_map.erase_group(group);
    }
    return true;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::has_group(const GroupId& group) const -> bool {
    std::shared_lock lock(_registry_mutex);
    return _groups.contains(group);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::group_count() const -> std::size_t {
    std::shared_lock lock(_registry_mutex);
    return _groups.size();
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::group_ids() const -> std::vector<GroupId> {
    std::shared_lock lock(_registry_mutex);
    std::vector<GroupId> out;
    out.reserve(_groups.size());
    for (const auto& [id, _] : _groups) {
        out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::group_node(const GroupId& group) -> group_node_type* {
    auto state = find_group(group);
    return state ? state->_node.get() : nullptr;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::find_group(const GroupId& group) const -> group_ptr {
    std::shared_lock lock(_registry_mutex);
    auto it = _groups.find(group);
    return it == _groups.end() ? nullptr : it->second;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::all_groups() const -> std::vector<group_ptr> {
    std::shared_lock lock(_registry_mutex);
    std::vector<group_ptr> out;
    out.reserve(_groups.size());
    for (const auto& [_, g] : _groups) {
        out.push_back(g);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Activity and hibernation
// ─────────────────────────────────────────────────────────────────────────────

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::now_ns() -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::note_activity(group_state& g) -> void {
    g._last_activity_ns.store(now_ns(), std::memory_order_relaxed);
    g._hibernating.store(false, std::memory_order_relaxed);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::note_activity(const GroupId& group) -> void {
    if (auto g = find_group(group)) {
        note_activity(*g);
    }
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::wake(const GroupId& group) -> bool {
    auto g = find_group(group);
    if (!g) {
        return false;
    }
    const bool was = g->_hibernating.load(std::memory_order_relaxed);
    note_activity(*g);
    return was;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::is_hibernating(const GroupId& group) const -> bool {
    auto g = find_group(group);
    return g && g->_hibernating.load(std::memory_order_relaxed);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::hibernating_count() const -> std::size_t {
    std::size_t n = 0;
    for (const auto& g : all_groups()) {
        if (g->_hibernating.load(std::memory_order_relaxed)) {
            ++n;
        }
    }
    return n;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::hibernation_enabled(std::size_t group_count) const -> bool {
    switch (_cfg.hibernation) {
        case hibernation_mode::off:
            return false;
        case hibernation_mode::on:
            return true;
        case hibernation_mode::auto_above_group_count:
            return group_count > _cfg.hibernation_group_threshold;
    }
    return false;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::leader_hibernate_after() const -> std::chrono::nanoseconds {
    const auto configured =
        _cfg.hibernate_after.value_or(std::chrono::duration_cast<std::chrono::milliseconds>(
            10 * _cfg.config.heartbeat_interval()));
    return std::chrono::duration_cast<std::chrono::nanoseconds>(configured);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::follower_hibernate_after() const -> std::chrono::nanoseconds {
    // A follower must go quiet BEFORE it would campaign, or hibernating a
    // leader simply buys an election: the leader stops heartbeating, and the
    // followers' election timers fire long before the leader-side idle window
    // elapses. Half the minimum election timeout is comfortably inside that
    // bound and still several heartbeat intervals away from a live leader, so a
    // follower under an active leader never reaches it — and if a dropped
    // heartbeat does push it over, the next inbound message wakes it again at
    // no cost.
    //
    // This is the part of design §4.3's follower rule that can be implemented
    // without a wire signal. The design's own formulation — a follower that
    // "has been told the leader is hibernating" — needs a field on
    // AppendEntries that `raft.hpp` does not have and that Phase 4 is not
    // allowed to add.
    const auto election_min =
        std::chrono::duration_cast<std::chrono::nanoseconds>(_cfg.config.election_timeout_min());
    const auto half_election = election_min / 2;
    const auto leader_side = leader_hibernate_after();
    return half_election < leader_side ? half_election : leader_side;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::evaluate_hibernation(const std::vector<group_ptr>& groups)
    -> void {
    if (!hibernation_enabled(groups.size())) {
        for (const auto& g : groups) {
            g->_hibernating.store(false, std::memory_order_relaxed);
        }
        return;
    }

    const auto now = now_ns();
    const auto leader_window = leader_hibernate_after().count();
    const auto follower_window = follower_hibernate_after().count();

    for (const auto& g : groups) {
        if (g->_hibernating.load(std::memory_order_relaxed)) {
            continue;
        }
        const auto idle = now - g->_last_activity_ns.load(std::memory_order_relaxed);
        {
            std::lock_guard lock(g->_deferred_mutex);
            if (!g->_deferred.empty()) {
                continue;  // Work is queued; the group is not idle.
            }
        }
        // Eligibility, as close to design §4.3's rule as the signals allow.
        //
        // A CANDIDATE is mid-election and never eligible: hibernating it would
        // abandon the election it just started.
        //
        // A FOLLOWER that has never heard from a leader is not "quiet", it is
        // *leaderless* — the design's rule is that a follower hibernates when
        // it "heard from a hibernating leader within its election timeout", and
        // a follower with no known leader has heard from nobody. Hibernating it
        // would suppress exactly the election the group needs, and since a
        // freshly created group is in this state, it would suppress the FIRST
        // election too and the group would never start.
        //
        // A LEADER idle for ten heartbeat intervals has, by construction, had
        // nothing to replicate for that whole time: its heartbeats carried
        // everything it held. The remaining gap is an unreachable follower,
        // which the `match_index_of` check added in Phase 6 (task 15) closes.
        const auto role = g->_node->get_state();
        if (role == server_state::candidate) {
            continue;
        }
        if (role != server_state::leader && !g->_node->known_leader().has_value()) {
            continue;
        }
        const auto window = role == server_state::leader ? leader_window : follower_window;
        if (idle >= window) {
            g->_hibernating.store(true, std::memory_order_relaxed);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// The tick
// ─────────────────────────────────────────────────────────────────────────────

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::select_ready(std::vector<group_ptr>& hibernating_out)
    -> std::vector<group_ptr> {
    std::vector<group_ptr> ready;
    for (const auto& g : all_groups()) {
        bool has_deferred = false;
        {
            std::lock_guard lock(g->_deferred_mutex);
            has_deferred = !g->_deferred.empty();
        }
        if (g->_hibernating.load(std::memory_order_relaxed) && !has_deferred) {
            hibernating_out.push_back(g);
        } else {
            ready.push_back(g);
        }
    }
    return ready;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::run_phase(const std::vector<group_ptr>& ready,
                                                const std::function<void(group_state&)>& fn)
    -> void {
    if (ready.empty()) {
        return;
    }

    std::mutex m;
    std::condition_variable cv;
    std::size_t remaining = ready.size();

    // One copy for the whole phase: `stop()` may clear the member while this
    // phase is running, and the copy is what keeps the pool alive underneath it.
    auto executor = current_executor();
    if (!executor || executor->stopped()) {
        // No pool: the host is stopped, so there is nothing to serialise
        // against and running inline is equivalent.
        for (const auto& g : ready) {
            try {
                fn(*g);
            } catch (...) {
            }
        }
        return;
    }

    for (const auto& g : ready) {
        const bool queued = executor->post(g->_stripe, [&, g] {
            try {
                fn(*g);
            } catch (...) {
                // One group's failure must not stall the whole tick: the
                // barrier below would never be reached, and every other group
                // would stop being driven. The node's own error handling has
                // already logged whatever this was.
            }
            // Notify INSIDE the lock: `m`, `cv` and `remaining` live on this
            // function's stack, and a notify after the unlock lets the waiter
            // return and unwind them while this thread is still inside
            // `notify_one()`. See the same fix in `striped_serial_executor`.
            std::lock_guard lock(m);
            --remaining;
            cv.notify_one();
        });
        if (!queued) {
            std::lock_guard lock(m);
            --remaining;
            cv.notify_one();
        }
    }

    std::unique_lock lock(m);
    cv.wait(lock, [&] { return remaining == 0; });
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::tick() -> tick_report {
    using clock = std::chrono::steady_clock;

    tick_report report;
    std::vector<group_ptr> hibernating;
    auto ready = select_ready(hibernating);

    report._ready_count = ready.size();
    report._hibernating_count = hibernating.size();
    report._total_count = ready.size() + hibernating.size();

    // ── PHASE 1: persist ─────────────────────────────────────────────────────
    //
    // Strictly before send: never advertise an append that is not durably
    // taken. The barrier spans the whole phase when the caller supplied a
    // controller, and otherwise collapses each group's own appends.
    {
        const auto t0 = clock::now();
        const bool shared_batch = _cfg.batch_controller && _cfg.batch_controller->valid();
        if (shared_batch && !ready.empty()) {
            _cfg.batch_controller->_begin();
            report._batch_size = ready.size();
        }
        try {
            run_phase(ready, [](group_state& g) {
                g._node->check_election_timeout();
                g._node->check_heartbeat_timeout();
            });
        } catch (...) {
            if (shared_batch && !ready.empty()) {
                _cfg.batch_controller->_abort();
            }
            throw;
        }
        if (shared_batch && !ready.empty()) {
            _cfg.batch_controller->_commit();
        }
        report._persist_duration = clock::now() - t0;
    }

    // ── PHASE 2: send ────────────────────────────────────────────────────────
    {
        const auto t0 = clock::now();
        run_phase(ready, [this](group_state& g) {
            // Leadership is sampled here rather than pushed from `node`,
            // because `node` has no hook for it and adding one would widen the
            // consensus core's surface for a statistic.
            const bool leader = g._node->is_leader();
            const auto was = g._leader_since_ns.load(std::memory_order_relaxed);
            if (leader && was == 0) {
                g._leader_since_ns.store(now_ns(), std::memory_order_relaxed);
                publish_report(g);
            } else if (!leader && was != 0) {
                g._leader_since_ns.store(0, std::memory_order_relaxed);
                publish_report(g);
            }
            g._node->replicate_to_followers();
        });
        report._send_duration = clock::now() - t0;
    }

    // ── PHASE 3: apply ───────────────────────────────────────────────────────
    //
    // Strictly after send, so a follower's copy is on the wire before the
    // leader spends time in the state machine. `node<Types>` applies committed
    // entries inline on whichever thread drove the commit, so what this phase
    // drains is the host's own per-group work — which from Phase 6 onward is
    // where admin-entry dispatch lands.
    {
        const auto t0 = clock::now();
        run_phase(ready, [](group_state& g) {
            // Apply latency is sampled per GROUP, around that group's share of
            // the phase — not around the phase as a whole. Nothing measured it
            // before, which is why `_p99_apply_latency` has been an unpopulated
            // field since the first draft.
            const auto group_start = clock::now();
            std::vector<std::function<void()>> work;
            {
                std::lock_guard lock(g._deferred_mutex);
                work.swap(g._deferred);
            }
            for (auto& w : work) {
                w();
            }
            g._apply_latency.record(
                std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - group_start));
        });
        report._apply_duration = clock::now() - t0;
    }

    // ── PHASE 4: policy ──────────────────────────────────────────────────────
    {
        const auto t0 = clock::now();
        const auto now = clock::now();
        if (now - _last_policy_run >= _cfg.policy_interval) {
            _last_policy_run = now;
            report._policy_ran = true;

            // A merge whose target leader is unreachable leaves the source
            // frozen: unavailable but CORRECT, which is the right trade and
            // the one design §5.7 makes deliberately. What is not acceptable is
            // leaving an operator to guess, so it is surfaced by name.
            for (const auto& g : ready) {
                if (g->_operation.load(std::memory_order_relaxed) !=
                    shard_operation_state::merging_source) {
                    continue;
                }
                std::chrono::steady_clock::time_point frozen_at{};
                GroupId merge_target{};
                {
                    std::lock_guard lock(g->_merge_mutex);
                    frozen_at = g->_merge_frozen_at;
                    merge_target = g->_merge_target.value_or(GroupId{});
                }
                if (now - frozen_at >= _cfg.merge_stall_warning_after) {
                    _stalled_merges.fetch_add(1, std::memory_order_relaxed);
                    _cfg.logger.warning(
                        "Merge stalled: the source has been frozen past the warning threshold",
                        {{"source", detail::describe_value(g->_group_id)},
                         {"target", detail::describe_value(merge_target)},
                         {"frozen_ms",
                          std::to_string(
                              std::chrono::duration_cast<std::chrono::milliseconds>(now - frozen_at)
                                  .count())}});
                }
            }

            // Derive load RATES before anything reads them. `_reads` and
            // `_writes` are cumulative, and the policy tick is the only place
            // with two observations and the interval between them — so it is
            // the only place a per-second figure can be computed at all.
            for (const auto& g : ready) {
                update_load_rates(*g, now);
            }

            // Rotate the latency windows here, on the policy tick, so that the
            // percentile a policy reads covers the last
            // `latency_window_count * policy_interval` and nothing older. A
            // lifetime percentile never decays, which would make any
            // latency-derived policy permanently sticky once a shard has had
            // one bad minute.
            for (const auto& g : ready) {
                g->_read_latency.rotate();
                g->_apply_latency.rotate();
            }

            evaluate_policy(ready);
        }
        report._policy_duration = clock::now() - t0;
    }

    // ── PHASE 5: heartbeat ───────────────────────────────────────────────────
    //
    // After policy, deliberately. A shard that this tick's policy just put into
    // `splitting` should be reported as splitting rather than as stable — the
    // driver can then decline to compute an operator that would only be
    // skipped.
    maybe_heartbeat();

    evaluate_hibernation(ready);
    return report;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::update_load_rates(group_state& g,
                                                        std::chrono::steady_clock::time_point now)
    -> void {
    const auto reads = g._reads.load(std::memory_order_relaxed);
    const auto writes = g._writes.load(std::memory_order_relaxed);
    const auto read_bytes = g._read_bytes.load(std::memory_order_relaxed);
    const auto write_bytes = g._write_bytes.load(std::memory_order_relaxed);

    if (g._rate_sampled_at.time_since_epoch().count() == 0) {
        // First observation: a rate needs an interval, and there is not one
        // yet. Reporting the cumulative count here would be a figure with the
        // wrong units, which is worse than reporting zero.
        g._rate_sampled_at = now;
        g._prev_reads = reads;
        g._prev_writes = writes;
        g._prev_read_bytes = read_bytes;
        g._prev_write_bytes = write_bytes;
        return;
    }

    const auto seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(now - g._rate_sampled_at).count();
    if (seconds <= 0.0) {
        return;
    }

    const auto rate = [seconds](std::uint64_t current, std::uint64_t previous) {
        // Counters only ever advance, but a shard whose replica was destroyed
        // and re-created starts again from zero, and a negative "rate" would
        // read as a shard going quiet rather than as a shard that restarted.
        return current >= previous ? static_cast<double>(current - previous) / seconds : 0.0;
    };

    g._read_qps.store(rate(reads, g._prev_reads), std::memory_order_relaxed);
    g._write_qps.store(rate(writes, g._prev_writes), std::memory_order_relaxed);
    g._read_bytes_per_sec.store(rate(read_bytes, g._prev_read_bytes), std::memory_order_relaxed);
    g._write_bytes_per_sec.store(rate(write_bytes, g._prev_write_bytes), std::memory_order_relaxed);

    g._rate_sampled_at = now;
    g._prev_reads = reads;
    g._prev_writes = writes;
    g._prev_read_bytes = read_bytes;
    g._prev_write_bytes = write_bytes;

    // Channel (c′) advances on the same tick, off the rates just computed.
    if (_cfg.load_split._enabled) {
        std::vector<hot_key_sample<Key>> samples;
        {
            std::lock_guard lock(g._sampler_mutex);
            samples = g._load_sampler.evaluate(
                g._read_qps.load(std::memory_order_relaxed),
                g._write_qps.load(std::memory_order_relaxed),
                g._read_bytes_per_sec.load(std::memory_order_relaxed),
                g._write_bytes_per_sec.load(std::memory_order_relaxed), now);
            // Published unconditionally, empty included: a stale hot key left
            // in place would keep proposing a split at a boundary the sampler
            // has since stopped believing in.
            g._hot_keys = samples;
        }
    }
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::evaluate_policy(const std::vector<group_ptr>& ready) -> void {
    if (!_automatic_enabled.load() || (!_cfg.evaluate_split && !_cfg.evaluate_merge)) {
        return;
    }

    for (const auto& g : ready) {
        if (!g->_node || !g->_node->is_leader()) {
            // Leader-only. The policy's answer is frozen into the entry every
            // replica applies, so exactly one replica may decide — and the
            // leader is the only one that can propose.
            continue;
        }
        // The kill switch and the freeze are checked here as well as inside
        // `admit`, so a frozen shard is never even evaluated: an operator who
        // froze a shard should not see policy activity against it in the logs.
        if (g->_frozen.load(std::memory_order_relaxed) ||
            g->_operation.load(std::memory_order_relaxed) != shard_operation_state::stable) {
            continue;
        }

        auto stats = stats_for(g->_group_id);
        if (!stats.has_value()) {
            continue;
        }

        if (_cfg.evaluate_split) {
            const auto decision = _cfg.evaluate_split(*stats);
            if (decision.should_split()) {
                split_options options{};
                options._channel = signal_channel::policy;
                options._reason = decision.reason();
                options._policy = decision.policy();
                // A load split ALWAYS scatters: children whose leaders both
                // land on the machine that was already hot have accomplished
                // exactly nothing (Requirement 8.6).
                options._scatter_children = decision.reason() == split_reason::read_load ||
                                            decision.reason() == split_reason::write_load;
                // Fire and forget: the arbiter's gates decide whether it
                // happens, and a rejection is already logged with its gate.
                std::ignore = split_shard(g->_group_id, decision.at_keys(), options,
                                          std::chrono::seconds{30});
                continue;
            }
        }

        if (_cfg.evaluate_merge) {
            // Only a left-adjacent sibling can be a merge SOURCE for this
            // shard, and only if this node holds it too — colocation is a
            // precondition and checking it here saves a rejection.
            std::optional<descriptor_type> sibling;
            {
                std::shared_lock lock(_map_mutex);
                sibling = _shard_map.left_sibling(g->_group_id);
            }
            if (!sibling.has_value()) {
                continue;
            }
            auto sibling_stats = stats_for(sibling->_group_id);
            if (!sibling_stats.has_value()) {
                continue;
            }
            const auto decision = _cfg.evaluate_merge(*stats, *sibling_stats);
            if (decision.vetoed()) {
                // Counted, not merely skipped. A veto and an abstention both
                // leave the merge unproposed, and only one of them is a policy
                // actively holding the cluster back — which is the difference
                // an operator needs when merges have stopped happening.
                note_merge_veto(g->_group_id, decision.policy(), decision.reason());
                continue;
            }
            if (decision.should_merge()) {
                merge_options options{};
                options._channel = signal_channel::policy;
                options._reason = decision.reason();
                options._policy = decision.policy();
                _cfg.metrics.set_metric_name("kythira.multiraft.merge.proposed");
                _cfg.metrics.add_dimension("group", detail::describe_value(g->_group_id));
                _cfg.metrics.add_dimension("reason", to_string(decision.reason()));
                _cfg.metrics.add_dimension("channel", to_string(signal_channel::policy));
                _cfg.metrics.add_dimension("policy", policy_label(decision.policy()));
                _cfg.metrics.add_count(1);
                _cfg.metrics.emit();
                std::ignore =
                    merge_shards(sibling->_group_id, g->_group_id, std::chrono::seconds{30});
            }
        }
    }
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::note_merge_veto(const GroupId& group, const char* policy,
                                                      merge_reason reason) -> void {
    const auto label = policy_label(policy);
    {
        std::lock_guard lock(_rejection_mutex);
        ++_merge_vetoes[label];
    }
    _cfg.logger.info("merge_vetoed", {{"group", detail::describe_value(group)},
                                      {"policy", label},
                                      {"reason", to_string(reason)}});
    _cfg.metrics.set_metric_name("kythira.multiraft.merge.vetoed");
    _cfg.metrics.add_dimension("group", detail::describe_value(group));
    _cfg.metrics.add_dimension("policy", label);
    _cfg.metrics.add_count(1);
    _cfg.metrics.emit();
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::load_sampler_state_of(const GroupId& group) const
    -> std::optional<load_sampler_state> {
    auto g = find_group(group);
    if (!g) {
        return std::nullopt;
    }
    std::lock_guard lock(g->_sampler_mutex);
    return g->_load_sampler.state();
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::load_sampler_counters(const GroupId& group) const
    -> std::optional<std::array<std::uint64_t, 4>> {
    auto g = find_group(group);
    if (!g) {
        return std::nullopt;
    }
    std::lock_guard lock(g->_sampler_mutex);
    return std::array<std::uint64_t, 4>{
        g->_load_sampler.windows_started(), g->_load_sampler.abandoned_spike_count(),
        g->_load_sampler.single_hot_key_count(), g->_load_sampler.proposal_count()};
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::merge_veto_count(const std::string& policy) const
    -> std::uint64_t {
    std::lock_guard lock(_rejection_mutex);
    auto it = _merge_vetoes.find(policy);
    return it == _merge_vetoes.end() ? 0 : it->second;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::merge_veto_count() const -> std::uint64_t {
    std::lock_guard lock(_rejection_mutex);
    std::uint64_t total = 0;
    for (const auto& [_, n] : _merge_vetoes) {
        total += n;
    }
    return total;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::defer_to_apply_phase(const GroupId& group,
                                                           std::function<void()> work) -> bool {
    auto g = find_group(group);
    if (!g) {
        return false;
    }
    {
        std::lock_guard lock(g->_deferred_mutex);
        g->_deferred.push_back(std::move(work));
    }
    // Deferred work is itself activity: a hibernating group with queued work
    // would otherwise never be selected to run it.
    note_activity(*g);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// The arbiter (design §6.6)
// ─────────────────────────────────────────────────────────────────────────────

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::note_rejection(const GroupId& group, arbiter_gate gate,
                                                     signal_channel channel, const char* operation)
    -> void {
    {
        std::lock_guard lock(_rejection_mutex);
        ++_rejections[static_cast<std::uint8_t>(gate)];
    }
    // Every decision, accepted or rejected, gets a line with the reason as a
    // dimension. Thresholds are untunable without this: an operator who cannot
    // see `split.rejected{gate=cooldown}` will conclude the feature is broken.
    _cfg.logger.info(std::string{operation} + ".rejected",
                     {{"group", detail::describe_value(group)},
                      {"gate", to_string(gate)},
                      {"channel", to_string(channel)}});
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::operations_in_flight() const -> std::size_t {
    std::size_t n = 0;
    for (const auto& g : all_groups()) {
        switch (g->_operation.load(std::memory_order_relaxed)) {
            case shard_operation_state::splitting:
            case shard_operation_state::merging_source:
            case shard_operation_state::merging_target:
                ++n;
                break;
            default:
                break;
        }
    }
    return n;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::admit(group_state& g, signal_channel channel,
                                            shard_operation_state to, bool override_cooldown)
    -> arbiter_decision<GroupId> {
    using decision = arbiter_decision<GroupId>;

    // The kill switch stops the AUTOMATIC channels only. An operator who has
    // turned it off wants the cluster to stop moving on its own, not to lose
    // their own escape hatch.
    if (channel != signal_channel::admin && !_automatic_enabled.load()) {
        return decision{
            ._admitted = false, ._gate = arbiter_gate::globally_disabled, ._channel = channel};
    }

    // `freeze_shard` blocks the automatic channels and not admin, for the same
    // reason.
    if (channel != signal_channel::admin && g._frozen.load(std::memory_order_relaxed)) {
        return decision{._admitted = false, ._gate = arbiter_gate::state, ._channel = channel};
    }

    // The cooldown. Enforced HERE, by the host, so that a custom policy which
    // forgets it still cannot oscillate — the one knob whose misconfiguration
    // is unbounded gets defence in depth.
    if (!override_cooldown) {
        const auto now = now_ns();
        const auto interval =
            std::chrono::duration_cast<std::chrono::nanoseconds>(_cfg.split_merge_interval).count();
        const auto last_split = g._last_split_ns.load(std::memory_order_relaxed);
        const auto last_merge = g._last_merge_ns.load(std::memory_order_relaxed);
        if ((last_split != 0 && now - last_split < interval) ||
            (last_merge != 0 && now - last_merge < interval)) {
            return decision{
                ._admitted = false, ._gate = arbiter_gate::cooldown, ._channel = channel};
        }
    }

    // The concurrency limit, checked BEFORE the transition and never enforced
    // by aborting something already committed.
    if (operations_in_flight() >= _cfg.max_concurrent_split_merge) {
        return decision{
            ._admitted = false, ._gate = arbiter_gate::concurrency_limit, ._channel = channel};
    }

    // The state gate is a TRANSITION, not a check. Two channels racing in the
    // same interval cannot both proceed, because only one compare-exchange can
    // win — conflicting operations are impossible by construction rather than
    // by check-then-act.
    auto expected = shard_operation_state::stable;
    if (g._operation.compare_exchange_strong(expected, to)) {
        return decision{._admitted = true, ._gate = arbiter_gate::admitted, ._channel = channel};
    }

    // `frozen` admits an explicit admin command and nothing else, transitioning
    // straight into the operation. `release` puts it back to `frozen` rather
    // than to `stable`, so the operator's freeze survives their own command.
    if (channel == signal_channel::admin) {
        auto frozen = shard_operation_state::frozen;
        if (g._operation.compare_exchange_strong(frozen, to)) {
            return decision{
                ._admitted = true, ._gate = arbiter_gate::admitted, ._channel = channel};
        }
    }
    return decision{._admitted = false,
                    ._gate = arbiter_gate::state,
                    ._channel = channel,
                    ._preempted_by = std::nullopt};
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::release(group_state& g) -> void {
    // A shard an operator froze returns to FROZEN, not to stable: an admin
    // command run against a frozen shard must not quietly thaw it.
    g._operation.store(g._frozen.load(std::memory_order_relaxed) ? shard_operation_state::frozen
                                                                 : shard_operation_state::stable);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::would_admit(const GroupId& group,
                                                  signal_channel channel) const
    -> arbiter_decision<GroupId> {
    using decision = arbiter_decision<GroupId>;
    auto g = find_group(group);
    if (!g) {
        return decision{._admitted = false, ._gate = arbiter_gate::state, ._channel = channel};
    }
    if (channel != signal_channel::admin && !_automatic_enabled.load()) {
        return decision{
            ._admitted = false, ._gate = arbiter_gate::globally_disabled, ._channel = channel};
    }
    if (channel != signal_channel::admin && g->_frozen.load(std::memory_order_relaxed)) {
        return decision{._admitted = false, ._gate = arbiter_gate::state, ._channel = channel};
    }
    const auto state = g->_operation.load(std::memory_order_relaxed);
    const bool startable =
        state == shard_operation_state::stable ||
        (channel == signal_channel::admin && state == shard_operation_state::frozen);
    if (!startable) {
        return decision{._admitted = false, ._gate = arbiter_gate::state, ._channel = channel};
    }
    const auto now = now_ns();
    const auto interval =
        std::chrono::duration_cast<std::chrono::nanoseconds>(_cfg.split_merge_interval).count();
    const auto last_split = g->_last_split_ns.load(std::memory_order_relaxed);
    const auto last_merge = g->_last_merge_ns.load(std::memory_order_relaxed);
    if ((last_split != 0 && now - last_split < interval) ||
        (last_merge != 0 && now - last_merge < interval)) {
        return decision{._admitted = false, ._gate = arbiter_gate::cooldown, ._channel = channel};
    }
    if (operations_in_flight() >= _cfg.max_concurrent_split_merge) {
        return decision{
            ._admitted = false, ._gate = arbiter_gate::concurrency_limit, ._channel = channel};
    }
    return decision{._admitted = true, ._gate = arbiter_gate::admitted, ._channel = channel};
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::freeze_shard(const GroupId& group) -> bool {
    auto g = find_group(group);
    if (!g) {
        return false;
    }
    g->_frozen.store(true, std::memory_order_relaxed);
    auto expected = shard_operation_state::stable;
    // Only an idle shard changes state now; one mid-operation picks the frozen
    // state up when it releases.
    std::ignore = g->_operation.compare_exchange_strong(expected, shard_operation_state::frozen);
    _cfg.logger.info("Shard frozen: automatic channels blocked, admin commands still accepted",
                     {{"group", detail::describe_value(group)}});
    return true;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::thaw_shard(const GroupId& group) -> bool {
    auto g = find_group(group);
    if (!g) {
        return false;
    }
    g->_frozen.store(false, std::memory_order_relaxed);
    auto expected = shard_operation_state::frozen;
    std::ignore = g->_operation.compare_exchange_strong(expected, shard_operation_state::stable);
    _cfg.logger.info("Shard thawed", {{"group", detail::describe_value(group)}});
    return true;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::set_automatic_split_merge_enabled(bool enabled) -> void {
    _automatic_enabled.store(enabled);
    _cfg.logger.info(enabled ? "Automatic split and merge enabled"
                             : "Automatic split and merge disabled; admin commands unaffected",
                     {});
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::automatic_split_merge_enabled() const -> bool {
    return _automatic_enabled.load();
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::rejection_count(arbiter_gate gate) const -> std::uint64_t {
    std::lock_guard lock(_rejection_mutex);
    auto it = _rejections.find(static_cast<std::uint8_t>(gate));
    return it == _rejections.end() ? 0 : it->second;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::set_report_listener(
    std::function<void(const group_report<GroupId, node_id_type>&)> listener) -> void {
    _report_listener = std::move(listener);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::publish_report(group_state& g) -> void {
    if (!_report_listener || !g._node) {
        return;
    }
    group_report<GroupId, node_id_type> report{
        ._group_id = g._group_id,
        ._role = g._node->get_state(),
        ._term = static_cast<std::uint64_t>(g._node->get_current_term()),
        ._commit_index = static_cast<std::uint64_t>(g._node->last_applied_index()),
        ._voters = g._descriptor._voters,
        ._operation = g._operation.load(std::memory_order_relaxed)};
    _report_listener(report);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::stats_for(const GroupId& group) const
    -> std::optional<shard_stats<GroupId, Key, node_id_type>> {
    auto g = find_group(group);
    if (!g || !g->_node) {
        return std::nullopt;
    }

    shard_stats<GroupId, Key, node_id_type> stats;
    stats._descriptor = g->_descriptor;
    stats._last_applied_index = g->_node->last_applied_index();

    // Sizes come from the state machine's extension, and `_size_available`
    // stays false without it. That flag is deliberate: a silent no-op is the
    // worst failure mode a policy layer can have, so the absence is a fact the
    // policy can see rather than a zero it would misread as "small".
    if constexpr (splittable_state_machine<typename Types::state_machine_type, Key>) {
        g->_node->with_state_machine([&](typename Types::state_machine_type& sm) {
            stats._approximate_size_bytes = sm.approximate_size_bytes();
            stats._approximate_key_count = sm.approximate_key_count();
        });
        stats._size_available = true;
    }

    // Load is measured HERE, at the routing layer, which is what makes
    // load-based split work for any state machine — including one with no
    // sizing hooks at all.
    stats._read_qps = g->_read_qps.load(std::memory_order_relaxed);
    stats._write_qps = g->_write_qps.load(std::memory_order_relaxed);
    stats._read_bytes_per_sec = g->_read_bytes_per_sec.load(std::memory_order_relaxed);
    stats._write_bytes_per_sec = g->_write_bytes_per_sec.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(g->_sampler_mutex);
        stats._hot_key_samples = g->_hot_keys;
    }

    const auto now = now_ns();
    const auto since = [now](std::int64_t stamp) {
        return stamp == 0 ? std::chrono::milliseconds::max()
                          : std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::nanoseconds{now - stamp});
    };
    stats._time_since_last_split = since(g->_last_split_ns.load(std::memory_order_relaxed));
    stats._time_since_last_merge = since(g->_last_merge_ns.load(std::memory_order_relaxed));
    stats._leader_since = since(g->_leader_since_ns.load(std::memory_order_relaxed));

    stats._p99_read_latency = g->_read_latency.p99();
    stats._p99_apply_latency = g->_apply_latency.p99();

    stats._voter_count = g->_descriptor._voters.size();
    stats._learner_count = g->_descriptor._learners.size();
    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// Split (design §5.3 propose, §5.4 apply)
// ─────────────────────────────────────────────────────────────────────────────

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::handle_unknown_group(const GroupId& group)
    -> unknown_group_action {
    if (!_cfg.lazy_replica_creation) {
        return unknown_group_action::drop;
    }
    // The tombstone check already happened in the transport, before this was
    // called: a merged-away group must never be re-created here, and putting
    // that check upstream is what guarantees this function never sees one.

    // 1. The local routing map. A node that missed a split still learns the
    //    child from the first peer that tells it about one.
    std::optional<descriptor_type> desc;
    {
        std::shared_lock lock(_map_mutex);
        desc = _shard_map.find(group);
    }

    // 2. One rate-limited external lookup. An unknown group id arriving at
    //    message rate must not become a control-plane query at message rate —
    //    a partitioned peer retrying an AppendEntries would otherwise be a
    //    denial of service against the placement driver.
    if (!desc.has_value() && _cfg.lookup_descriptor) {
        bool may_query = false;
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard lock(_lookup_mutex);
            auto it = _last_lookup.find(group);
            if (it == _last_lookup.end() ||
                now - it->second >= _cfg.unknown_group_lookup_interval) {
                _last_lookup[group] = now;
                may_query = true;
            }
        }
        if (may_query) {
            _descriptor_lookups.fetch_add(1, std::memory_order_relaxed);
            desc = _cfg.lookup_descriptor(group);
        }
    }

    if (!desc.has_value()) {
        return unknown_group_action::drop;
    }

    // 3. Membership. A message naming a group this node is not a member of must
    //    never create a replica: doing so would place a replica the cluster's
    //    configuration does not know about, and no membership change would ever
    //    remove it.
    if (!desc->has_replica(_cfg.node_id)) {
        return unknown_group_action::drop;
    }

    // 4. An UNINITIALISED replica: empty store, empty log. It is populated by
    //    the InstallSnapshot the leader sends once it sees how far behind this
    //    replica is — exactly the one snapshot transfer design §9 prices this
    //    recovery at.
    try {
        create_group(*desc);
    } catch (const std::exception& e) {
        _cfg.logger.warning("Lazy replica creation failed",
                            {{"group", detail::describe_value(group)}, {"error", e.what()}});
        return unknown_group_action::drop;
    }
    _lazy_replicas.fetch_add(1, std::memory_order_relaxed);
    _cfg.logger.info("Created a replica from an inbound message",
                     {{"group", detail::describe_value(group)}});
    return unknown_group_action::created;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::lazily_created_replica_count() const -> std::uint64_t {
    return _lazy_replicas.load(std::memory_order_relaxed);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::descriptor_lookup_count() const -> std::uint64_t {
    return _descriptor_lookups.load(std::memory_order_relaxed);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::encode_key_or_default(const Key& k) const
    -> std::vector<std::byte> {
    if (_cfg.encode_key) {
        return _cfg.encode_key(k);
    }
    return default_shard_key_codec<Key>{}.encode(k);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::make_key_codec() const -> key_codec_adapter<Key> {
    return key_codec_adapter<Key>{
        [this](const Key& k) { return encode_key_or_default(k); },
        [this](const std::vector<std::byte>& b) { return decode_key_or_default(b); }};
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::decode_key_or_default(
    const std::vector<std::byte>& bytes) const -> Key {
    if (_cfg.decode_key) {
        return _cfg.decode_key(bytes);
    }
    return default_shard_key_codec<Key>{}.decode(bytes);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::operation_state(const GroupId& group) const
    -> std::optional<shard_operation_state> {
    auto g = find_group(group);
    return g ? std::optional{g->_operation.load(std::memory_order_relaxed)} : std::nullopt;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::applied_split_count() const -> std::uint64_t {
    return _applied_splits.load(std::memory_order_relaxed);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::node_id() const -> const node_id_type& {
    return _cfg.node_id;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::replay_admin_entry(const GroupId& group,
                                                         const log_entry_type& entry,
                                                         log_index_type index) -> bool {
    auto g = find_group(group);
    if (!g || !is_admin_entry_type(entry.type())) {
        return false;
    }
    // Through `with_state_machine` so the replay takes the same lock and sees
    // the same state machine reference the apply loop would hand the handler.
    g->_node->with_state_machine([&](typename Types::state_machine_type& sm) {
        if (entry.type() == entry_type::split) {
            auto cmd =
                decode_split_command<GroupId, Key, node_id_type>(entry.command(), make_key_codec());
            apply_split(*g, cmd, index, entry.term(), sm);
        }
    });
    return true;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::install_admin_handler(group_state& g) -> void {
    // Captured by raw pointer, not by `shared_ptr`: the handler lives on the
    // node, the node lives on the group_state, and a strong reference here
    // would make the pair immortal. `destroy_group` tears the node down before
    // the group_state goes, so the handler cannot outlive what it points at.
    auto* state = &g;
    g._node->set_admin_entry_handler([this, state](const log_entry_type& entry,
                                                   log_index_type index,
                                                   typename Types::state_machine_type& sm) {
        switch (entry.type()) {
            case entry_type::split: {
                auto cmd = decode_split_command<GroupId, Key, node_id_type>(entry.command(),
                                                                            make_key_codec());
                apply_split(*state, cmd, index, entry.term(), sm);
                break;
            }
            case entry_type::merge_prepare: {
                auto cmd = decode_merge_prepare_command<GroupId, Key, node_id_type>(
                    entry.command(), make_key_codec());
                apply_merge_prepare(*state, cmd, index);
                break;
            }
            case entry_type::merge_commit: {
                auto cmd = decode_merge_commit_command<GroupId, Key, node_id_type, log_entry_type>(
                    entry.command(), make_key_codec());
                apply_merge_commit(*state, cmd, sm);
                break;
            }
            case entry_type::merge_rollback: {
                auto cmd = decode_merge_rollback_command<GroupId>(entry.command());
                apply_merge_rollback(*state, cmd);
                break;
            }
            case entry_type::merge_abandoned: {
                auto cmd = decode_merge_abandoned_command<GroupId>(entry.command());
                apply_merge_abandoned(*state, cmd);
                break;
            }
            default:
                // Reaching here means a peer is running a newer binary and
                // proposed an entry type this build does not know. Worth saying
                // so: the alternative is silently diverging from the rest of
                // the cluster about what that entry did.
                _cfg.logger.warning("Administration entry type not handled by this build",
                                    {{"group", detail::describe_value(state->_group_id)},
                                     {"entry_type", std::to_string(static_cast<int>(entry.type()))},
                                     {"index", std::to_string(index)}});
                break;
        }
    });
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::split_shard(const GroupId& group, std::vector<Key> at_keys,
                                                  std::chrono::milliseconds timeout)
    -> future_type {
    return split_shard(group, std::move(at_keys), split_options{}, timeout);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::pre_split(const GroupId& group, std::vector<Key> boundaries,
                                                std::chrono::milliseconds timeout) -> future_type {
    auto g = find_group(group);
    if (!g) {
        return failed_future(
            std::make_exception_ptr(unknown_shard_exception<GroupId>{group, "no local replica"}));
    }

    // Refused on a non-empty shard, deliberately. Pre-split exists for the
    // bulk-load case — cut the range up BEFORE the writes arrive — and allowing
    // it on a populated shard would make it a second, weaker `split_shard` that
    // skips the state machine's veto entirely.
    bool empty = true;
    if constexpr (splittable_state_machine<typename Types::state_machine_type, Key>) {
        g->_node->with_state_machine([&](typename Types::state_machine_type& sm) {
            empty = sm.approximate_key_count() == 0;
        });
    }
    if (!empty) {
        note_rejection(group, arbiter_gate::state, signal_channel::admin, "pre_split");
        return failed_future(std::make_exception_ptr(shard_busy_exception<GroupId>{
            group, "not empty; pre_split is for a shard that has not been written to yet"}));
    }

    split_options options{};
    options._reason = split_reason::pre_split;
    // The cooldown guards against automatic oscillation; a pre-split happens
    // once, before the shard has ever been written to, and waiting an hour to
    // do it would defeat the point.
    options._override_cooldown = true;
    // Empty means "you choose", and a state machine with no keys has nothing to
    // suggest — so a pre-split with no boundaries is a no-op the caller should
    // hear about rather than a silent success.
    if (boundaries.empty()) {
        return failed_future(
            std::make_exception_ptr(no_valid_split_key_exception<GroupId>{group, 0}));
    }
    return split_shard(group, std::move(boundaries), options, timeout);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::split_shard(const GroupId& group, std::vector<Key> at_keys,
                                                  split_options options,
                                                  std::chrono::milliseconds timeout)
    -> future_type {
    auto g = find_group(group);
    if (!g) {
        return failed_future(
            std::make_exception_ptr(unknown_shard_exception<GroupId>{group, "no local replica"}));
    }

    // ── step 1: gate ─────────────────────────────────────────────────────────
    if (!g->_node->is_leader()) {
        note_rejection(group, arbiter_gate::not_leader, options._channel, "split");
        return failed_future(std::make_exception_ptr(
            shard_not_leader_exception<GroupId, node_id_type>{group, g->_node->known_leader()}));
    }
    const auto admitted =
        admit(*g, options._channel, shard_operation_state::splitting, options._override_cooldown);
    if (!admitted) {
        note_rejection(group, admitted._gate, options._channel, "split");
        return failed_future(std::make_exception_ptr(
            shard_busy_exception<GroupId>{group, to_string(admitted._gate)}));
    }

    // From here every failure path must put the state back, or the shard is
    // wedged in `splitting` with nothing running.
    auto release_gate = [this, g] { this->release(*g); };

    const auto parent = g->_descriptor;

    // ── step 2: keys ─────────────────────────────────────────────────────────
    //
    // Requested keys minus the state machine's vetoes; if that leaves nothing,
    // the state machine's own suggestions minus its vetoes. The veto comes
    // FIRST and the suggestion is the fallback, which is what Requirement 9.4
    // buys the application: a policy saying "split at K" cannot cut through an
    // entity the state machine says is indivisible.
    std::size_t candidates_considered = at_keys.size();
    for (const auto& k : at_keys) {
        if (!parent._range.contains(k)) {
            release_gate();
            return failed_future(std::make_exception_ptr(
                split_key_out_of_range_exception<GroupId, Key>{group, k, parent._range}));
        }
    }

    std::vector<Key> chosen;
    if constexpr (splittable_state_machine<typename Types::state_machine_type, Key>) {
        const auto limit = _cfg.batch_split_limit;
        const auto& range = parent._range;
        const bool allow_fallback = options._allow_state_machine_veto || at_keys.empty();
        g->_node->with_state_machine([&](typename Types::state_machine_type& sm) {
            for (const auto& k : at_keys) {
                if (sm.can_split_at(k)) {
                    chosen.push_back(k);
                }
            }
            // The fallback is what `_allow_state_machine_veto` controls: on by
            // default, an operator naming a forbidden key gets a nearby valid
            // one; off, they get the failure they asked for.
            if (chosen.empty() && allow_fallback) {
                auto suggested = sm.suggest_split_keys(limit);
                candidates_considered += suggested.size();
                for (const auto& k : suggested) {
                    if (range.contains(k) && sm.can_split_at(k)) {
                        chosen.push_back(k);
                    }
                }
            }
        });
    } else {
        // Without the extension there is no veto and no suggestion: the caller
        // must name the keys, and an empty request cannot be served.
        chosen = at_keys;
    }

    std::sort(chosen.begin(), chosen.end());
    chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());

    // `batch_split_limit` is a HOST gate, not only a knob of the default
    // policy's own config (Requirement 7.8). A composition unions its members'
    // split keys, so the proposal that actually reaches Raft can exceed a limit
    // every member respected individually — each of them behaving correctly.
    //
    // It truncates rather than refusing: the shard genuinely does need
    // splitting, and fewer children is a worse answer than what was asked for
    // but a far better one than no split at all. Truncation is counted and
    // logged because it overrides a decision a policy actually made.
    if (chosen.size() > _cfg.batch_split_limit) {
        const auto proposed = chosen.size();
        chosen.resize(_cfg.batch_split_limit);
        note_rejection(group, arbiter_gate::split_keys_truncated, options._channel, "split");
        _cfg.logger.warning("Split key list truncated to batch_split_limit",
                            {{"group", detail::describe_value(group)},
                             {"proposed_keys", std::to_string(proposed)},
                             {"kept_keys", std::to_string(chosen.size())},
                             {"policy", policy_label(options._policy)},
                             {"channel", to_string(options._channel)}});
        _cfg.metrics.set_metric_name("kythira.multiraft.split.truncated");
        _cfg.metrics.add_dimension("group", detail::describe_value(group));
        _cfg.metrics.add_dimension("proposed_keys", std::to_string(proposed));
        _cfg.metrics.add_dimension("kept_keys", std::to_string(chosen.size()));
        _cfg.metrics.add_count(1);
        _cfg.metrics.emit();
    }

    if (chosen.empty()) {
        release_gate();
        note_rejection(group, arbiter_gate::no_valid_split_key, options._channel, "split");
        return failed_future(std::make_exception_ptr(
            no_valid_split_key_exception<GroupId>{group, candidates_considered}));
    }

    // ── step 3: ids ──────────────────────────────────────────────────────────
    //
    // Not optional and not local. Every replica must derive identical group
    // ids, and only a cluster-scope authority can guarantee uniqueness — so an
    // unavailable authority ABANDONS the split rather than queueing it with
    // locally invented ids, which is how two partitions end up with two
    // different shards sharing one group id.
    const std::size_t child_count = chosen.size() + 1;
    const std::size_t new_ids_needed = child_count - 1;  // one child reuses the parent's id
    std::vector<GroupId> new_ids;
    if (new_ids_needed > 0) {
        if (!_cfg.allocate_group_ids) {
            release_gate();
            return failed_future(std::make_exception_ptr(std::runtime_error(
                "multi_raft: split needs allocate_group_ids; ids must come from a "
                "cluster-scope authority, never from the proposing node")));
        }
        new_ids = _cfg.allocate_group_ids(new_ids_needed);
        if (new_ids.size() < new_ids_needed) {
            release_gate();
            note_rejection(group, arbiter_gate::pd_unavailable, options._channel, "split");
            _cfg.logger.warning("Split abandoned: id authority unavailable",
                                {{"group", detail::describe_value(group)},
                                 {"needed", std::to_string(new_ids_needed)},
                                 {"got", std::to_string(new_ids.size())}});
            return failed_future(std::make_exception_ptr(std::runtime_error(
                "multi_raft: split abandoned, the shard-id authority is unavailable")));
        }
    }

    // ── step 4: derive ───────────────────────────────────────────────────────
    //
    // Children take `version = parent.version + N`, a single value rather than
    // a per-child increment: it keeps epoch ordering total, so "did I miss a
    // split" is a comparison rather than a set difference.
    const shard_epoch child_epoch{._version = parent._epoch._version + child_count,
                                  ._conf_version = parent._epoch._conf_version};

    std::vector<descriptor_type> children;
    children.reserve(child_count);
    for (std::size_t i = 0; i < child_count; ++i) {
        descriptor_type child;
        child._range._start = i == 0 ? parent._range._start : std::optional{chosen[i - 1]};
        child._range._end = i + 1 == child_count ? parent._range._end : std::optional{chosen[i]};
        child._epoch = child_epoch;
        // Members one-for-one with the parent's: a child replica is created on
        // exactly the machines that already hold a parent replica, so no data
        // moves.
        child._voters = parent._voters;
        child._learners = parent._learners;
        child._leader_hint = std::nullopt;
        children.push_back(std::move(child));
    }

    const std::size_t derived_index = _cfg.right_derive ? child_count - 1 : 0;
    children[derived_index]._group_id = parent._group_id;
    children[derived_index]._leader_hint = parent._leader_hint;
    for (std::size_t i = 0, next = 0; i < child_count; ++i) {
        if (i != derived_index) {
            children[i]._group_id = new_ids[next++];
        }
    }

    // ── step 6: propose ──────────────────────────────────────────────────────
    split_command_type cmd{._parent_group = parent._group_id,
                           ._parent_epoch = parent._epoch,
                           ._at_keys = chosen,
                           ._children = children,
                           ._right_derive = _cfg.right_derive,
                           ._reason = options._reason,
                           ._pd_operation_id = std::nullopt};

    auto payload = encode_split_command<GroupId, Key, node_id_type>(cmd, make_key_codec());

    _cfg.logger.info("Proposing split", {{"group", detail::describe_value(group)},
                                         {"children", std::to_string(child_count)},
                                         {"parent_version", std::to_string(parent._epoch._version)},
                                         {"child_version", std::to_string(child_epoch._version)},
                                         {"reason", to_string(options._reason)},
                                         {"channel", to_string(options._channel)},
                                         {"policy", policy_label(options._policy)}});

    _cfg.metrics.set_metric_name("kythira.multiraft.split.proposed");
    _cfg.metrics.add_dimension("group", detail::describe_value(group));
    _cfg.metrics.add_dimension("reason", to_string(options._reason));
    _cfg.metrics.add_dimension("channel", to_string(options._channel));
    _cfg.metrics.add_dimension("policy", policy_label(options._policy));
    _cfg.metrics.add_count(1);
    _cfg.metrics.emit();

    auto future = g->_node->propose_admin_entry(entry_type::split, std::move(payload), timeout);

    // The operation state is cleared by apply on success. A proposal that never
    // commits would otherwise wedge the shard, so the release is chained onto
    // the future's failure path too.
    auto self = this;
    const bool scatter_children = options._scatter_children;
    const auto child_ids = [&] {
        std::vector<GroupId> ids;
        ids.reserve(children.size());
        for (const auto& c : children) {
            ids.push_back(c._group_id);
        }
        return ids;
    }();
    return std::move(future).thenTry(
        [self, g, scatter_children, child_ids, timeout](auto&& result) {
            if (result.hasException()) {
                self->release(*g);
            } else if (scatter_children) {
                // Scatter is what a load split is *for*: children whose leaders
                // both land on the machine that was already hot have accomplished
                // nothing. Only the children this host actually leads are moved —
                // which, straight after apply, is the derived child, the one whose
                // leadership is concentrated by construction rather than by an
                // election. The non-derived children hold ordinary elections, and
                // an election is not concentrated to begin with.
                for (const auto& id : child_ids) {
                    auto child = self->find_group(id);
                    if (child && child->_node && child->_node->is_leader()) {
                        self->scatter(id, timeout);
                    }
                }
            }
            return std::forward<decltype(result)>(result).value();
        });
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::apply_split(group_state& parent,
                                                  const split_command_type& cmd,
                                                  log_index_type at_index, term_id_type at_term,
                                                  typename Types::state_machine_type& parent_sm)
    -> void {
    // ── A: epoch ─────────────────────────────────────────────────────────────
    //
    // An entry proposed under one epoch can commit after the epoch moved
    // (Requirement 3.7). Applying it anyway would cut a shard whose membership
    // the entry no longer describes.
    if (parent._descriptor._epoch != cmd._parent_epoch) {
        _cfg.logger.warning(
            "Skipping split entry: parent epoch moved",
            {{"group", detail::describe_value(parent._group_id)},
             {"entry_version", std::to_string(cmd._parent_epoch._version)},
             {"local_version", std::to_string(parent._descriptor._epoch._version)}});
        return;
    }

    // ── B: idempotence ───────────────────────────────────────────────────────
    //
    // Checked before ANYTHING else happens, so replaying the entry after a
    // crash is a no-op. This is what makes step E's "children first, then the
    // parent's apply index" ordering safe without a batched store.
    bool all_present = true;
    for (const auto& child : cmd._children) {
        if (child._group_id == cmd._parent_group) {
            continue;
        }
        auto existing = find_group(child._group_id);
        if (!existing || existing->_descriptor._epoch != child._epoch) {
            all_present = false;
            break;
        }
    }
    if (all_present && parent._descriptor._epoch == cmd._children.front()._epoch) {
        return;
    }

    // ── C: freeze ────────────────────────────────────────────────────────────
    parent._operation.store(shard_operation_state::splitting);

    // ── D: cut ───────────────────────────────────────────────────────────────
    //
    // Deterministic across replicas by contract (the round-trip law); every
    // replica therefore produces byte-identical children.
    std::vector<std::vector<std::byte>> blobs;
    if constexpr (splittable_state_machine<typename Types::state_machine_type, Key>) {
        blobs = parent_sm.split_state(cmd._at_keys);
    }
    if (blobs.size() != cmd._children.size()) {
        // A state machine that cannot cut cannot be split. Failing loudly beats
        // creating children with no state, which would silently lose every key
        // in their ranges.
        parent._operation.store(shard_operation_state::stable);
        throw shard_exception("split apply: state machine produced " +
                              std::to_string(blobs.size()) + " parts for " +
                              std::to_string(cmd._children.size()) + " children");
    }

    // ── E: durable state ─────────────────────────────────────────────────────
    //
    // The children's initial state and the parent's advanced descriptor must
    // land together. Without a store that spans groups they cannot, so the
    // order is children FIRST and the parent afterwards: a crash between them
    // replays the entry, and step B finds the children already present. The
    // reverse order loses a child permanently and silently — nothing notices
    // until a client asks for a key in that range.
    //
    // The synthetic snapshot is why a split moves no data. The child does not
    // copy the parent's log; it begins at the parent's apply index with an
    // EMPTY log and a snapshot that *is* its share of the parent's state.
    std::size_t derived_index = 0;
    for (std::size_t i = 0; i < cmd._children.size(); ++i) {
        if (cmd._children[i]._group_id == cmd._parent_group) {
            derived_index = i;
        }
    }

    for (std::size_t i = 0; i < cmd._children.size(); ++i) {
        const auto& child = cmd._children[i];
        if (i == derived_index) {
            continue;
        }
        // This node holds a replica of the child only where it holds one of the
        // parent — which is everywhere, since members are one-for-one.
        if (!child.has_replica(_cfg.node_id)) {
            continue;
        }
        if (find_group(child._group_id)) {
            continue;  // replayed
        }

        fiu_do_on("raft/multiraft/split/before_children",
                  throw shard_exception("chaos: split/before_children"););

        const auto blob = blobs[i];
        const auto members = child._voters;
        const auto learners = child._learners;
        create_group(child, [&](typename Types::persistence_engine_type& store) {
            snapshot_type snap{};
            snap._last_included_index = at_index;
            snap._last_included_term = at_term;
            snap._configuration =
                typename Types::cluster_configuration_type{members, false, std::nullopt, learners};
            snap._state_machine_state = blob;
            store.save_snapshot(snap);
            store.save_current_term(at_term);
        });

        fiu_do_on("raft/multiraft/split/between_children",
                  throw shard_exception("chaos: split/between_children"););
    }

    fiu_do_on("raft/multiraft/split/after_children_before_parent",
              throw shard_exception("chaos: split/after_children_before_parent"););

    // The derived child keeps the parent's group id, log and term; only its
    // range narrows and its state machine is reduced to its own share.
    if constexpr (splittable_state_machine<typename Types::state_machine_type, Key>) {
        parent_sm.restore_from_snapshot(blobs[derived_index], at_index);
    }
    parent._descriptor = cmd._children[derived_index];

    // ── F/G: publish ─────────────────────────────────────────────────────────
    {
        std::unique_lock lock(_map_mutex);
        for (const auto& child : cmd._children) {
            _shard_map.upsert(child);
        }
    }
    fiu_do_on("raft/multiraft/split/after_publish",
              throw shard_exception("chaos: split/after_publish"););

#ifndef NDEBUG
    if (auto problem = shard_map_snapshot().check_tiling(); problem.has_value()) {
        _cfg.logger.error(
            "Split broke the tiling invariant",
            {{"group", detail::describe_value(parent._group_id)}, {"problem", *problem}});
    }
#endif

    // ── H: unfreeze ──────────────────────────────────────────────────────────
    //
    // The history stamp goes here, at APPLY, not at proposal: the cooldown is
    // about how recently the shard actually changed shape, and a proposal that
    // never committed changed nothing.
    parent._last_split_ns.store(now_ns(), std::memory_order_relaxed);
    release(parent);
    _applied_splits.fetch_add(1, std::memory_order_relaxed);
    publish_report(parent);

    _cfg.logger.info("Applied split",
                     {{"group", detail::describe_value(parent._group_id)},
                      {"children", std::to_string(cmd._children.size())},
                      {"at_index", std::to_string(at_index)},
                      {"new_version", std::to_string(cmd._children.front()._epoch._version)}});

    // Told now rather than waited for on the next heartbeat: a split changes
    // the routing table for the whole cluster, and up to a heartbeat interval
    // of clients holding a descriptor for a range that no longer exists is a
    // cost with no benefit. Every replica applies, but only the leader reports
    // — N copies of the same fact would tell the driver nothing extra.
    if (_cfg.report_split && parent._node && parent._node->is_leader()) {
        _cfg.report_split(cmd._children[derived_index], cmd._children);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Routing table and tombstones
// ─────────────────────────────────────────────────────────────────────────────

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::shard_map_snapshot() const -> shard_map_type {
    std::shared_lock lock(_map_mutex);
    return _shard_map;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::learn_descriptors(
    const std::vector<descriptor_type>& descriptors) -> bool {
    std::unique_lock lock(_map_mutex);
    return _shard_map.upsert_all(descriptors);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::is_tombstoned(const GroupId& group) const -> bool {
    std::lock_guard lock(_tombstone_mutex);
    return _tombstones.contains(group);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::gc_tombstones(std::chrono::system_clock::time_point now)
    -> std::size_t {
    std::size_t removed = 0;
    {
        std::lock_guard lock(_tombstone_mutex);
        removed = _tombstones.gc(now, _cfg.tombstone_horizon);
    }
    if (removed > 0) {
        persist_tombstones();
    }
    return removed;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::persist_tombstones() -> void {
    if (_cfg.host_data_dir.empty()) {
        return;
    }
    std::lock_guard lock(_tombstone_mutex);
    _tombstones.save_to_file(_cfg.host_data_dir / "tombstones");
}

// ─────────────────────────────────────────────────────────────────────────────
// Merge (design §5.5)
// ─────────────────────────────────────────────────────────────────────────────

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::applied_merge_count() const -> std::uint64_t {
    return _applied_merges.load(std::memory_order_relaxed);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::rolled_back_merge_count() const -> std::uint64_t {
    return _rolled_back_merges.load(std::memory_order_relaxed);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::stalled_merge_report_count() const -> std::uint64_t {
    return _stalled_merges.load(std::memory_order_relaxed);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::merge_target_of(const GroupId& source) const
    -> std::optional<GroupId> {
    auto g = find_group(source);
    if (!g) {
        return std::nullopt;
    }
    std::lock_guard lock(g->_merge_mutex);
    return g->_merge_target;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::check_merge_preconditions(const descriptor_type& source,
                                                                const descriptor_type& target,
                                                                const group_ptr& source_state,
                                                                const group_ptr& target_state) const
    -> std::exception_ptr {
    // Adjacency. Either order is legal — a shard may merge into the neighbour
    // on either side, which is what `merge_direction` exists to express.
    if (!source._range.is_adjacent_left_of(target._range) &&
        !target._range.is_adjacent_left_of(source._range)) {
        return std::make_exception_ptr(shard_not_adjacent_exception<GroupId, Key>{
            source._group_id, target._group_id, source._range, target._range});
    }

    // Colocation. Each target replica absorbs state from the source replica ON
    // ITS OWN MACHINE, so a target replica with no local source peer simply
    // cannot apply `merge_commit`. This fails fast rather than shipping state
    // across the network mid-merge, which is the operation the whole design is
    // built to avoid.
    if (!is_colocated(source, target)) {
        return std::make_exception_ptr(shard_alignment_required_exception<GroupId, node_id_type>{
            source._group_id, target._group_id, source._voters, target._voters});
    }

    // Operation state. Only `stable` admits a new operation, on both sides.
    if (source_state) {
        const auto st = source_state->_operation.load(std::memory_order_relaxed);
        if (st != shard_operation_state::stable) {
            return std::make_exception_ptr(
                shard_busy_exception<GroupId>{source._group_id, to_string(st)});
        }
    }
    if (target_state) {
        const auto st = target_state->_operation.load(std::memory_order_relaxed);
        if (st != shard_operation_state::stable) {
            return std::make_exception_ptr(
                shard_busy_exception<GroupId>{target._group_id, to_string(st)});
        }
    }

    // Joint consensus. A membership change in flight on either side would move
    // the replica sets out from under the colocation check we just made.
    if (source_state && source_state->_node->get_cluster_size() != source._voters.size()) {
        return std::make_exception_ptr(
            shard_busy_exception<GroupId>{source._group_id, "configuration change in flight"});
    }
    if (target_state && target_state->_node->get_cluster_size() != target._voters.size()) {
        return std::make_exception_ptr(
            shard_busy_exception<GroupId>{target._group_id, "configuration change in flight"});
    }
    return nullptr;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::merge_shards(const GroupId& source, const GroupId& target,
                                                   std::chrono::milliseconds timeout)
    -> future_type {
    auto source_state = find_group(source);
    auto target_state = find_group(target);
    if (!source_state) {
        return failed_future(std::make_exception_ptr(
            unknown_shard_exception<GroupId>{source, "no local replica of the source"}));
    }
    if (!target_state) {
        // Colocation is a precondition, so a host that leads the source and has
        // no target replica is telling us the sets are not aligned.
        return failed_future(
            std::make_exception_ptr(shard_alignment_required_exception<GroupId, node_id_type>{
                source, target, source_state->_descriptor._voters, {}}));
    }
    if (!source_state->_node->is_leader()) {
        return failed_future(
            std::make_exception_ptr(shard_not_leader_exception<GroupId, node_id_type>{
                source, source_state->_node->known_leader()}));
    }

    const auto source_desc = source_state->_descriptor;
    const auto target_desc = target_state->_descriptor;
    if (auto problem =
            check_merge_preconditions(source_desc, target_desc, source_state, target_state)) {
        note_rejection(source, arbiter_gate::alignment_required, signal_channel::admin, "merge");
        return failed_future(problem);
    }

    // `min_index` over the source's VOTERS bounds what `merge_commit` carries:
    // every voter is known to hold everything up to it, so only the tail
    // beyond it has to travel. Without it the commit entry would carry the
    // source's whole log or its whole state.
    //
    // A down voter's stale match index drags this figure down and makes the
    // carried tail larger — correct but wasteful. Design §12 open question 3
    // covers the alternative; taking the minimum over live voters only would
    // need a rule for a source replica that is behind `min_index`, and getting
    // that wrong loses entries.
    log_index_type min_index = std::numeric_limits<log_index_type>::max();
    for (const auto& voter : source_desc._voters) {
        const auto m = source_state->_node->match_index_of(voter);
        min_index = std::min(min_index, m.value_or(log_index_type{0}));
    }
    if (min_index == std::numeric_limits<log_index_type>::max()) {
        min_index = 0;
    }

    // Claim the source through the arbiter: every gate in one place, and the
    // state gate is a transition rather than a check, so two channels racing in
    // the same interval cannot both proceed. Apply sets it again, idempotently,
    // on every replica.
    const auto admitted =
        admit(*source_state, signal_channel::admin, shard_operation_state::merging_source, false);
    if (!admitted) {
        note_rejection(source, admitted._gate, signal_channel::admin, "merge");
        return failed_future(std::make_exception_ptr(
            shard_busy_exception<GroupId>{source, to_string(admitted._gate)}));
    }

    merge_prepare_command_type cmd{._source = source_desc,
                                   ._target = target_desc,
                                   ._min_index = static_cast<std::uint64_t>(min_index),
                                   ._reason = merge_reason::admin};

    _cfg.logger.info("Proposing merge_prepare", {{"source", detail::describe_value(source)},
                                                 {"target", detail::describe_value(target)},
                                                 {"min_index", std::to_string(min_index)}});

    auto payload = encode_merge_prepare_command<GroupId, Key, node_id_type>(cmd, make_key_codec());
    auto future = source_state->_node->propose_admin_entry(entry_type::merge_prepare,
                                                           std::move(payload), timeout);

    // A proposal that never commits would leave the source wedged in
    // `merging_source` with nothing running, so the release is chained onto the
    // failure path. On success, apply owns the state.
    auto self = this;
    return std::move(future).thenTry([self, source_state](auto&& result) {
        if (result.hasException()) {
            self->release(*source_state);
        }
        return std::forward<decltype(result)>(result).value();
    });
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::apply_merge_prepare(group_state& source,
                                                          const merge_prepare_command_type& cmd,
                                                          log_index_type at_index) -> void {
    // Re-checked at apply, not merely at proposal: an entry proposed under one
    // epoch can commit after the epoch has moved.
    if (source._descriptor._epoch != cmd._source._epoch) {
        _cfg.logger.warning("Skipping merge_prepare: source epoch moved",
                            {{"source", detail::describe_value(source._group_id)}});
        return;
    }

    {
        std::lock_guard lock(source._merge_mutex);
        if (source._merge_target.has_value()) {
            return;  // replayed
        }
        source._merge_target = cmd._target._group_id;
        source._merge_prepare_index = at_index;
        source._merge_min_index = static_cast<log_index_type>(cmd._min_index);
        source._merge_frozen_at = std::chrono::steady_clock::now();
    }
    // From here the source rejects proposals and reads. It is released ONLY by
    // a `merge_rollback` in its own log — never by a timer, unless the operator
    // has explicitly taken on `merge_lease_mode`'s clock assumption.
    source._operation.store(shard_operation_state::merging_source);

    fiu_do_on("raft/multiraft/merge/after_prepare",
              throw shard_exception("chaos: merge/after_prepare"););

    _cfg.logger.info("Source frozen for merge",
                     {{"source", detail::describe_value(source._group_id)},
                      {"target", detail::describe_value(cmd._target._group_id)},
                      {"prepare_index", std::to_string(at_index)}});

    // The prepare entry IS the notification. Colocation guarantees the machine
    // leading the target holds a source replica, so it learns of the merge by
    // applying this entry — no out-of-band channel, and no second place for the
    // two sides to disagree about whether the merge started.
    //
    // DEFERRED, not called inline. This runs inside the SOURCE node's apply
    // loop with the source's mutex held, and proposing on the target takes the
    // TARGET's mutex. `apply_merge_commit` crosses the same two locks in the
    // opposite direction — target's held, source's wanted — so doing both
    // inline is a textbook ABBA deadlock. Deferring this one leaves exactly one
    // crossing direction (target then source), which cannot deadlock, and the
    // apply phase runs it on the target's own stripe with no node lock held.
    defer_to_apply_phase(cmd._target._group_id,
                         [this, cmd] { this->maybe_propose_merge_commit(cmd); });
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::maybe_propose_merge_commit(
    const merge_prepare_command_type& cmd) -> void {
    auto target = find_group(cmd._target._group_id);
    if (!target || !target->_node->is_leader()) {
        return;  // some other machine leads the target; it will do this.
    }

    {
        std::lock_guard lock(target->_merge_mutex);
        if (target->_merge_abandoned) {
            // This target already committed `merge_abandoned` for this merge.
            // Once abandoned, never committed: both decisions are made by this
            // one log, which is what makes them mutually exclusive.
            return;
        }
        if (target->_merge_commit_proposed) {
            return;  // already under way
        }
        target->_merge_commit_proposed = true;
        target->_merge_source = cmd._source._group_id;
    }
    target->_operation.store(shard_operation_state::merging_target);

    auto source = find_group(cmd._source._group_id);
    if (!source) {
        return;
    }

    // Carry the tail `(min_index, prepare_index]` so that every target replica
    // can bring its own local source replica to exactly the prepare index
    // before reading its state. That is what makes the absorb deterministic
    // across target replicas — they all read the same source.
    merge_commit_command_type commit{
        ._source = cmd._source, ._target = cmd._target, ._prepare_index = 0, ._entries = {}};
    {
        std::lock_guard lock(source->_merge_mutex);
        commit._prepare_index = static_cast<std::uint64_t>(source->_merge_prepare_index);
    }
    const auto from = static_cast<log_index_type>(cmd._min_index) + 1;
    const auto to = static_cast<log_index_type>(commit._prepare_index);
    commit._entries = source->_node->log_entries_between(from, to);

    _cfg.logger.info("Proposing merge_commit",
                     {{"source", detail::describe_value(cmd._source._group_id)},
                      {"target", detail::describe_value(cmd._target._group_id)},
                      {"tail_entries", std::to_string(commit._entries.size())}});

    auto payload = encode_merge_commit_command<GroupId, Key, node_id_type, log_entry_type>(
        commit, make_key_codec());
    std::ignore = target->_node->propose_admin_entry(entry_type::merge_commit, std::move(payload),
                                                     std::chrono::seconds{30});
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::apply_merge_commit(
    group_state& target, const merge_commit_command_type& cmd,
    typename Types::state_machine_type& target_sm) -> void {
    // Idempotence first, as in split apply: a replayed commit must be a no-op.
    if (target._descriptor._range.covers(cmd._source._range)) {
        return;
    }

    auto source = find_group(cmd._source._group_id);
    if (!source) {
        // No local source replica means colocation was violated between the
        // proposal and now. Absorbing nothing would silently lose the source's
        // whole range, so this fails loudly instead.
        throw shard_alignment_required_exception<GroupId, node_id_type>{
            cmd._source._group_id, target._group_id, cmd._source._voters,
            target._descriptor._voters};
    }

    // (a) Force-apply the carried tail to the LOCAL source replica, so it
    //     stands exactly at the prepare index. Only entries this replica has
    //     NOT applied are replayed: re-applying a command is safe only for a
    //     state machine whose commands happen to be idempotent, which the
    //     `state_machine` concept does not require.
    std::vector<std::byte> source_state_bytes;
    const auto already_applied = source->_node->last_applied_index();
    source->_node->with_state_machine([&](typename Types::state_machine_type& source_sm) {
        for (const auto& e : cmd._entries) {
            if (e.index() > already_applied && e.type() == entry_type::normal &&
                !e.command().empty()) {
                std::ignore = source_sm.apply(e.command(), e.index());
            }
        }
        fiu_do_on("raft/multiraft/merge/mid_commit_catchup",
                  throw shard_exception("chaos: merge/mid_commit_catchup"););
        source_state_bytes = source_sm.get_state();
    });

    // (b) Absorb. Deterministic by contract, and the exact inverse of
    //     `split_state` — the round-trip law is what makes this safe.
    if constexpr (splittable_state_machine<typename Types::state_machine_type, Key>) {
        target_sm.absorb(source_state_bytes, cmd._source._range);
    } else {
        throw shard_exception(
            "merge apply: the state machine cannot absorb; "
            "splittable_state_machine is required for merge");
    }

    // (c) Extend the range over the source's, and (d) take
    //     `version = max(src, tgt).version + 1`.
    auto survivor = target._descriptor;
    if (cmd._source._range.is_adjacent_left_of(survivor._range)) {
        survivor._range._start = cmd._source._range._start;
    } else {
        survivor._range._end = cmd._source._range._end;
    }
    survivor._epoch._version =
        std::max(cmd._source._epoch._version, target._descriptor._epoch._version) + 1;
    target._descriptor = survivor;

    fiu_do_on("raft/multiraft/merge/after_absorb_before_destroy",
              throw shard_exception("chaos: merge/after_absorb_before_destroy"););

    // (e) Destroy and tombstone the local source replica.
    //
    // The correctness-critical half happens NOW, synchronously: the source is
    // unregistered from the transport and tombstoned, so it can neither serve
    // nor be resurrected. The expensive half — stopping the node, which joins
    // threads — is deferred to the host's apply phase, because doing it here
    // would join threads while holding the target node's mutex, and would
    // deadlock outright if the source happened to share the target's stripe.
    const auto source_group = cmd._source._group_id;
    _demux.unregister_group(source_group);
    {
        std::lock_guard lock(_tombstone_mutex);
        _tombstones.insert(source_group, tombstone_reason::merged_away,
                           std::chrono::system_clock::now());
    }
    {
        std::unique_lock lock(_registry_mutex);
        _groups.erase(source_group);
    }
    {
        std::unique_lock lock(_map_mutex);
        _shard_map.erase_group(source_group);
        _shard_map.upsert(survivor);
    }
    // `source` is the last strong reference; handing it to the apply phase is
    // what keeps the node alive until it can be stopped off this thread.
    defer_to_apply_phase(target._group_id, [source]() mutable {
        source->_node->stop();
        source->_node.reset();
    });

    target._last_merge_ns.store(now_ns(), std::memory_order_relaxed);
    if (_cfg.report_merge && target._node && target._node->is_leader()) {
        // The command's own copy of the source descriptor, not a lookup: by
        // this point the local source replica has been torn down, and the
        // descriptor the merge was computed against is the one the driver needs
        // in order to retire its routing row.
        _cfg.report_merge(cmd._source, target._descriptor);
    }
    release(target);
    {
        std::lock_guard lock(target._merge_mutex);
        target._merge_source.reset();
        target._merge_commit_proposed = false;
    }
    _applied_merges.fetch_add(1, std::memory_order_relaxed);
    publish_report(target);
    persist_tombstones();

#ifndef NDEBUG
    if (auto problem = shard_map_snapshot().check_tiling(); problem.has_value()) {
        _cfg.logger.error(
            "Merge broke the tiling invariant",
            {{"target", detail::describe_value(target._group_id)}, {"problem", *problem}});
    }
#endif

    _cfg.logger.info("Applied merge", {{"source", detail::describe_value(source_group)},
                                       {"target", detail::describe_value(target._group_id)},
                                       {"new_version", std::to_string(survivor._epoch._version)}});
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::apply_merge_rollback(group_state& source,
                                                           const merge_rollback_command_type& cmd)
    -> void {
    {
        std::lock_guard lock(source._merge_mutex);
        if (!source._merge_target.has_value()) {
            return;  // replayed, or never frozen
        }
        source._merge_target.reset();
        source._merge_prepare_index = 0;
        source._merge_min_index = 0;
    }
    release(source);
    _rolled_back_merges.fetch_add(1, std::memory_order_relaxed);
    publish_report(source);
    _cfg.logger.info("Merge rolled back; source resumes serving",
                     {{"source", detail::describe_value(source._group_id)},
                      {"target", detail::describe_value(cmd._target_group)}});
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::apply_merge_abandoned(group_state& target,
                                                            const merge_abandoned_command_type& cmd)
    -> void {
    // A committed fact in the TARGET's own log. A target leader failover cannot
    // lose it: the new leader replays this entry and inherits the refusal to
    // commit. That is the whole reason the abandon is a log entry rather than a
    // message.
    {
        std::lock_guard lock(target._merge_mutex);
        target._merge_abandoned = true;
        target._merge_source.reset();
        target._merge_commit_proposed = false;
    }
    if (target._operation.load(std::memory_order_relaxed) ==
        shard_operation_state::merging_target) {
        target._operation.store(shard_operation_state::stable);
    }
    _cfg.logger.info("Merge abandoned by the target",
                     {{"source", detail::describe_value(cmd._source_group)},
                      {"target", detail::describe_value(target._group_id)}});
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::abandon_merge(const GroupId& source,
                                                    std::chrono::milliseconds timeout)
    -> future_type {
    auto source_state = find_group(source);
    if (!source_state) {
        return failed_future(
            std::make_exception_ptr(unknown_shard_exception<GroupId>{source, "no local replica"}));
    }

    GroupId target_group{};
    {
        std::lock_guard lock(source_state->_merge_mutex);
        if (!source_state->_merge_target.has_value()) {
            return failed_future(std::make_exception_ptr(
                shard_busy_exception<GroupId>{source, "not frozen for a merge"}));
        }
        target_group = *source_state->_merge_target;
    }

    auto target_state = find_group(target_group);
    if (!target_state || !target_state->_node->is_leader()) {
        // The abandon has to be recorded in the TARGET's log, so only the
        // target's leader can do it. A source stuck here stays frozen —
        // unavailable but correct — which is the trade design §5.7 makes.
        _cfg.logger.warning("Merge stalled: cannot reach the target's leader to abandon",
                            {{"source", detail::describe_value(source)},
                             {"target", detail::describe_value(target_group)}});
        return failed_future(
            std::make_exception_ptr(shard_merging_exception<GroupId>{source, target_group}));
    }

    {
        std::lock_guard lock(target_state->_merge_mutex);
        if (target_state->_merge_commit_proposed) {
            // Commit always wins. Once proposed, the target refuses to abandon,
            // because a source that resumed while a target replica had already
            // applied the commit would mean two shards owning one range.
            return failed_future(std::make_exception_ptr(
                shard_busy_exception<GroupId>{target_group, "merge_commit already proposed"}));
        }
    }

    merge_abandoned_command_type abandoned{._source_group = source, ._target_group = target_group};
    auto payload = encode_merge_abandoned_command<GroupId>(abandoned);
    auto future = target_state->_node->propose_admin_entry(entry_type::merge_abandoned,
                                                           std::move(payload), timeout);

    // The source proposes `merge_rollback` only after OBSERVING the committed
    // abandon record — never on the strength of having asked for it.
    auto self = this;
    const auto src = source;
    const auto tgt = target_group;
    return std::move(future).thenValue([self, src, tgt, timeout](auto&& value) {
        auto source_state = self->find_group(src);
        if (!source_state || !source_state->_node->is_leader()) {
            return std::forward<decltype(value)>(value);
        }
        fiu_do_on("raft/multiraft/merge/after_abandon_before_rollback",
                  throw shard_exception("chaos: merge/after_abandon_before_rollback"););
        merge_rollback_command_type rollback{._source_group = src, ._target_group = tgt};
        auto rollback_payload = encode_merge_rollback_command<GroupId>(rollback);
        std::ignore = source_state->_node->propose_admin_entry(
            entry_type::merge_rollback, std::move(rollback_payload), timeout);
        return std::forward<decltype(value)>(value);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Client routing
// ─────────────────────────────────────────────────────────────────────────────

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::failed_future(std::exception_ptr error) -> future_type {
    typename Types::promise_type promise;
    auto future = promise.getFuture();
    promise.setException(std::move(error));
    return future;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::resolve(const Key& key) const
    -> std::optional<descriptor_type> {
    std::shared_lock lock(_map_mutex);
    return _shard_map.lookup(key);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::update_descriptor(const GroupId& group,
                                                        const descriptor_type& descriptor) -> bool {
    auto g = find_group(group);
    if (!g) {
        return false;
    }
    g->_descriptor = descriptor;
    std::unique_lock lock(_map_mutex);
    _shard_map.upsert(descriptor);
    return true;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::set_local_descriptor(const GroupId& group,
                                                           const descriptor_type& descriptor)
    -> bool {
    auto g = find_group(group);
    if (!g) {
        return false;
    }
    g->_descriptor = descriptor;
    return true;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::publish_descriptor(const GroupId& group) -> bool {
    auto g = find_group(group);
    if (!g) {
        return false;
    }
    std::unique_lock lock(_map_mutex);
    return _shard_map.upsert(g->_descriptor);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::forget_routing_row(const GroupId& group) -> bool {
    std::unique_lock lock(_map_mutex);
    return _shard_map.erase_group(group);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::local_descriptor(const GroupId& group) const
    -> std::optional<descriptor_type> {
    auto g = find_group(group);
    return g ? std::optional{g->_descriptor} : std::nullopt;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::refresh_map_from_local_groups() -> bool {
    std::vector<descriptor_type> rows;
    for (const auto& g : all_groups()) {
        rows.push_back(g->_descriptor);
    }
    std::unique_lock lock(_map_mutex);
    return _shard_map.upsert_all(rows);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::load_counters(const GroupId& group) const
    -> std::pair<std::uint64_t, std::uint64_t> {
    auto g = find_group(group);
    if (!g) {
        return {0, 0};
    }
    return {g->_reads.load(std::memory_order_relaxed), g->_writes.load(std::memory_order_relaxed)};
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::route_and_run(std::optional<Key> key,
                                                    std::optional<GroupId> group,
                                                    std::optional<shard_epoch> expected_epoch,
                                                    const std::vector<std::byte>* command,
                                                    bool is_read, std::chrono::milliseconds timeout)
    -> future_type {
    std::exception_ptr last_error;

    for (std::size_t attempt = 0; attempt <= _cfg.max_route_retries; ++attempt) {
        // ── resolve ──────────────────────────────────────────────────────────
        std::optional<descriptor_type> desc;
        if (key.has_value()) {
            std::shared_lock lock(_map_mutex);
            desc = _shard_map.lookup(*key);
        } else {
            std::shared_lock lock(_map_mutex);
            desc = _shard_map.find(*group);
        }

        if (!desc.has_value()) {
            // A hole in the map is repairable from the local groups exactly
            // once; if it survives that, the shard genuinely is not here.
            if (attempt == 0 && refresh_map_from_local_groups()) {
                continue;
            }
            last_error = key.has_value()
                             ? std::make_exception_ptr(unrouted_key_exception<Key>{*key})
                             : std::make_exception_ptr(
                                   unknown_shard_exception<GroupId>{*group, "no routing row"});
            break;
        }

        auto local = find_group(desc->_group_id);
        if (!local) {
            // The routing row exists but the replica is elsewhere. Nothing this
            // host can do; the caller must reach one of the shard's voters.
            last_error = std::make_exception_ptr(unknown_shard_exception<GroupId>{
                desc->_group_id, "no local replica; try one of its voters"});
            break;
        }

        // ── epoch ────────────────────────────────────────────────────────────
        //
        // The local replica's own descriptor is the authority; the map is a
        // cache of it. A disagreement means the map is stale, so repair it and
        // re-resolve — that is what makes a split cost an in-flight request one
        // extra resolution instead of a placement-driver query.
        if (local->_descriptor._epoch != desc->_epoch) {
            last_error =
                std::make_exception_ptr(shard_epoch_mismatch_exception<GroupId, Key, node_id_type>{
                    desc->_group_id,
                    desc->_epoch,
                    local->_descriptor._epoch,
                    {local->_descriptor}});
            refresh_map_from_local_groups();
            continue;
        }
        if (expected_epoch.has_value() && *expected_epoch != local->_descriptor._epoch) {
            // The caller computed this request against a descriptor that has
            // since moved. Serving it would apply a command to a range the
            // caller no longer believes is there.
            last_error =
                std::make_exception_ptr(shard_epoch_mismatch_exception<GroupId, Key, node_id_type>{
                    desc->_group_id,
                    *expected_epoch,
                    local->_descriptor._epoch,
                    {local->_descriptor}});
            break;
        }

        // ── cross-shard admission ────────────────────────────────────────────
        //
        // There is no distributed transaction here, and pretending otherwise
        // would be the worst available failure: a command silently applied to a
        // shard that does not own its key produces a state no invariant catches.
        if (command != nullptr && _cfg.partitioner) {
            const auto command_key = _cfg.partitioner(*command);
            if (!local->_descriptor._range.contains(command_key)) {
                last_error = std::make_exception_ptr(cross_shard_command_exception<GroupId, Key>{
                    desc->_group_id, command_key, local->_descriptor._range});
                break;
            }
        }

        // ── the merge freeze ─────────────────────────────────────────────────
        //
        // A source that has applied `merge_prepare` no longer owns its range in
        // any useful sense: the target is about to take it. Serving a proposal
        // or a read here would produce a write the survivor never absorbs, or a
        // read of state that is about to move. The client backs off and retries;
        // the retry either succeeds against the source again (the merge rolled
        // back) or is redirected by an epoch mismatch (it committed).
        if (local->_operation.load(std::memory_order_relaxed) ==
            shard_operation_state::merging_source) {
            GroupId merge_target{};
            {
                std::lock_guard merge_lock(local->_merge_mutex);
                merge_target = local->_merge_target.value_or(GroupId{});
            }
            last_error = std::make_exception_ptr(
                shard_merging_exception<GroupId>{desc->_group_id, merge_target});
            if (attempt < _cfg.max_route_retries) {
                std::this_thread::sleep_for(_cfg.route_retry_backoff);
                refresh_map_from_local_groups();
                continue;
            }
            break;
        }

        // ── leadership ───────────────────────────────────────────────────────
        note_activity(*local);
        if (!local->_node->is_leader()) {
            last_error = std::make_exception_ptr(shard_not_leader_exception<GroupId, node_id_type>{
                desc->_group_id, local->_node->known_leader()});
            break;
        }

        // ── submit ───────────────────────────────────────────────────────────
        //
        // Load is counted here, at the routing layer, so that load-based split
        // works for ANY state machine — including one with no sizing hooks.
        // Channel (c′). The `_enabled` test is a load from an immutable config
        // member and is what Requirement 9.7's "one predictable branch when
        // off" buys: with sampling off the host never takes the lock, and the
        // sampler never runs.
        if (_cfg.load_split._enabled && key.has_value()) {
            std::lock_guard lock(local->_sampler_mutex);
            local->_load_sampler.observe(*key);
        }

        if (is_read) {
            local->_reads.fetch_add(1, std::memory_order_relaxed);
            // Latency is sampled HERE, not in `node<Types>::read_state`, for
            // three reasons in order of weight (design §6.1.4): the node cannot
            // see the shard-map lookup or the epoch validation the client
            // actually paid for; the node cannot attribute a read rejected
            // before it ran; and `node<Types>` is closed to changes beyond the
            // admin-entry hook and the group-id field.
            //
            // The clock starts at the top of the retry loop, so a read that
            // resolved twice reports what the caller waited, not what the last
            // attempt took.
            const auto started = std::chrono::steady_clock::now();
            auto g = local;
            return local->_node->read_state(timeout).thenTry([g, started, timeout](auto&& result) {
                // A completed read AND a timed-out read are both samples;
                // the timeout is clamped at its deadline. A percentile over
                // the requests that survived reports the health of the
                // survivors. Rejections before execution are excluded, and
                // they never reach here — `not_leader` and epoch mismatch
                // are refused above, before this continuation exists.
                auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started);
                if (result.hasException()) {
                    elapsed = std::min(
                        elapsed, std::chrono::duration_cast<std::chrono::nanoseconds>(timeout));
                }
                g->_read_latency.record(elapsed);
                auto value = std::forward<decltype(result)>(result).value();
                g->_read_bytes.fetch_add(value.size(), std::memory_order_relaxed);
                return value;
            });
        }
        local->_writes.fetch_add(1, std::memory_order_relaxed);
        local->_write_bytes.fetch_add(command->size(), std::memory_order_relaxed);
        return local->_node->submit_command(*command, timeout);
    }

    if (!last_error) {
        last_error = std::make_exception_ptr(
            std::runtime_error("multi_raft: routing exhausted its retries with no recorded error"));
    }
    return failed_future(std::move(last_error));
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::submit_command(const Key& key,
                                                     const std::vector<std::byte>& command,
                                                     std::chrono::milliseconds timeout)
    -> future_type {
    return route_and_run(key, std::nullopt, std::nullopt, &command, false, timeout);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::submit_command(const GroupId& group,
                                                     shard_epoch expected_epoch,
                                                     const std::vector<std::byte>& command,
                                                     std::chrono::milliseconds timeout)
    -> future_type {
    return route_and_run(std::nullopt, group, expected_epoch, &command, false, timeout);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::submit_command(const std::vector<std::byte>& command,
                                                     std::chrono::milliseconds timeout)
    -> future_type {
    if (!_cfg.partitioner) {
        return failed_future(std::make_exception_ptr(std::logic_error(
            "multi_raft: submit_command(command) needs a partitioner; name the key instead")));
    }
    return route_and_run(_cfg.partitioner(command), std::nullopt, std::nullopt, &command, false,
                         timeout);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::read_state(const Key& key, std::chrono::milliseconds timeout)
    -> future_type {
    return route_and_run(key, std::nullopt, std::nullopt, nullptr, true, timeout);
}

// ─────────────────────────────────────────────────────────────────────────────
// Observability
// ─────────────────────────────────────────────────────────────────────────────

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::stale_group_message_count() const -> std::uint64_t {
    return _demux.stale_group_message_count();
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::executor_stripe_count() const -> std::size_t {
    return _stripe_count;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::stripe_of(const GroupId& group) const -> std::size_t {
    auto g = find_group(group);
    return g ? g->_stripe : striped_serial_executor::npos;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::set_unknown_group_handler(
    std::function<unknown_group_action(const GroupId&)> handler) -> void {
    _demux.set_unknown_group_handler(std::move(handler));
}

// ─────────────────────────────────────────────────────────────────────────────
// Placement driver, channel (d) (Requirement 14, design §7)
// ─────────────────────────────────────────────────────────────────────────────

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::build_shard_reports() const
    -> std::vector<shard_report_type> {
    std::vector<shard_report_type> out;
    for (const auto& g : all_groups()) {
        if (!g->_node || !g->_node->is_leader()) {
            continue;
        }
        shard_report_type r;
        r._descriptor = g->_descriptor;
        r._leader = _cfg.node_id;
        r._term = static_cast<std::uint64_t>(g->_node->get_current_term());
        r._operation = g->_operation.load(std::memory_order_relaxed);

        if constexpr (splittable_state_machine<typename Types::state_machine_type, Key>) {
            g->_node->with_state_machine([&](typename Types::state_machine_type& sm) {
                r._approximate_size_bytes = sm.approximate_size_bytes();
                r._approximate_key_count = sm.approximate_key_count();
            });
            r._size_available = true;
        }

        r._read_qps = static_cast<double>(g->_reads.load(std::memory_order_relaxed));
        r._write_qps = static_cast<double>(g->_writes.load(std::memory_order_relaxed));

        // "Down" is the leader's own belief, formed from match indices, and it
        // is the only belief anyone holds: no other replica tracks per-peer
        // progress. A driver that waited for certainty would never act.
        for (const auto& voter : g->_descriptor._voters) {
            if (voter == _cfg.node_id) {
                continue;
            }
            if (!g->_node->match_index_of(voter).has_value()) {
                r._down_replicas.push_back(voter);
            }
        }
        r._down_replica_count = r._down_replicas.size();
        out.push_back(std::move(r));
    }
    return out;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::build_node_report() const -> node_report_type {
    node_report_type r;
    r._node_id = _cfg.node_id;
    r._labels = _cfg.node_labels;

    if (_cfg.capacity_probe) {
        const auto [capacity, available] = _cfg.capacity_probe();
        r._capacity_bytes = capacity;
        r._available_bytes = available;
        r._used_bytes = capacity >= available ? capacity - available : 0;
    }
    r._overloaded = _cfg.overload_probe ? _cfg.overload_probe() : false;

    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    for (const auto& g : all_groups()) {
        ++r._shard_count;
        if (g->_node && g->_node->is_leader()) {
            ++r._leader_count;
        }
        reads += g->_reads.load(std::memory_order_relaxed);
        writes += g->_writes.load(std::memory_order_relaxed);
    }
    r._read_qps = static_cast<double>(reads);
    r._write_qps = static_cast<double>(writes);
    return r;
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::note_skipped_operator(const shard_operation_type& op,
                                                            skipped_operator_reason reason)
    -> operator_outcome {
    _operators_skipped.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(_skip_mutex);
        ++_operator_skips[static_cast<std::uint8_t>(reason)];
    }
    _cfg.logger.info("skipped_operator", {{"group", detail::describe_value(op.group_id())},
                                          {"operation_id", std::to_string(op.operation_id())},
                                          {"operator", op.name()},
                                          {"reason", to_string(reason)}});
    _cfg.metrics.set_metric_name("shard.operator.skipped");
    _cfg.metrics.add_dimension("group", detail::describe_value(op.group_id()));
    _cfg.metrics.add_dimension("operator", op.name());
    _cfg.metrics.add_dimension("reason", to_string(reason));
    _cfg.metrics.add_count(1);
    _cfg.metrics.emit();
    return operator_outcome{
        ._operation_id = op.operation_id(), ._accepted = false, ._reason = reason};
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::apply_operator(const shard_operation_type& op)
    -> operator_outcome {
    auto g = find_group(op.group_id());
    if (!g || !g->_node) {
        return note_skipped_operator(op, skipped_operator_reason::unknown_shard);
    }

    // Requirement 14.6, and the check that makes an advisory operator safe
    // rather than merely tolerable: the driver reasoned about a range that may
    // no longer exist. Comparing epochs turns a wrong action into a discarded
    // message.
    if (op.epoch() != g->_descriptor._epoch) {
        return note_skipped_operator(op, skipped_operator_reason::stale_epoch);
    }
    if (!g->_node->is_leader()) {
        return note_skipped_operator(op, skipped_operator_reason::not_leader);
    }
    // A frozen shard, or the global kill switch, silences the *automatic*
    // channels, and the placement driver is one of them. An operator who froze
    // a shard did so to stop exactly this.
    if (!_automatic_enabled.load() || g->_frozen.load(std::memory_order_relaxed)) {
        return note_skipped_operator(op, skipped_operator_reason::driver_disabled);
    }

    const auto accept = [&] {
        _operators_accepted.fetch_add(1, std::memory_order_relaxed);
        _cfg.logger.info("accepted_operator", {{"group", detail::describe_value(op.group_id())},
                                               {"operation_id", std::to_string(op.operation_id())},
                                               {"operator", op.name()}});
        _cfg.metrics.set_metric_name("shard.operator.accepted");
        _cfg.metrics.add_dimension("group", detail::describe_value(op.group_id()));
        _cfg.metrics.add_dimension("operator", op.name());
        _cfg.metrics.add_count(1);
        _cfg.metrics.emit();
        return operator_outcome{._operation_id = op.operation_id(), ._accepted = true};
    };

    // Any of these may fail asynchronously. That is not a reason to report the
    // operator as skipped: skipped means "this host declined to try", and a
    // membership change that is refused three entries later is a different
    // event with its own reporting. Conflating them would tell an operator that
    // the driver's view was stale when in fact the cluster refused.
    const auto timeout = _cfg.config.rpc_timeout();

    return std::visit(
        [&]<typename Op>(const Op& concrete) -> operator_outcome {
            if constexpr (std::same_as<Op, add_replica_operator<node_id_type>>) {
                if (g->_descriptor.has_replica(concrete._node)) {
                    return note_skipped_operator(op, skipped_operator_reason::precondition);
                }
                if (concrete._as_learner) {
                    g->_node->add_learner(concrete._node);
                } else {
                    g->_node->add_server(concrete._node);
                }
                return accept();
            } else if constexpr (std::same_as<Op, remove_replica_operator<node_id_type>>) {
                if (!g->_descriptor.has_replica(concrete._node)) {
                    return note_skipped_operator(op, skipped_operator_reason::precondition);
                }
                g->_node->remove_server(concrete._node);
                return accept();
            } else if constexpr (std::same_as<Op, transfer_leader_operator<node_id_type>>) {
                // Naming this host, or a node that is not a voter, is a
                // precondition failure the host can see for itself — unlike a
                // transfer that is accepted and then loses its election, which
                // is an ordinary asynchronous outcome and not a skip.
                if (!g->_descriptor.has_voter(concrete._to) || concrete._to == _cfg.node_id) {
                    return note_skipped_operator(op, skipped_operator_reason::precondition);
                }
                if constexpr (!network_client_with_timeout_now<
                                  typename group_types::network_client_type>) {
                    return note_skipped_operator(op, skipped_operator_reason::unsupported);
                } else {
                    g->_node->transfer_leadership(concrete._to, timeout);
                    return accept();
                }
            } else if constexpr (std::same_as<Op, split_operator<Key>>) {
                split_options options{};
                options._channel = signal_channel::placement_driver;
                options._reason = split_reason::placement_driver;
                auto would = would_admit(op.group_id(), signal_channel::placement_driver);
                if (!would) {
                    return note_skipped_operator(op, skipped_operator_reason::shard_busy);
                }
                split_shard(op.group_id(), concrete._at_keys, options, timeout);
                return accept();
            } else if constexpr (std::same_as<Op, merge_operator<GroupId>>) {
                auto would = would_admit(op.group_id(), signal_channel::placement_driver);
                if (!would) {
                    return note_skipped_operator(op, skipped_operator_reason::shard_busy);
                }
                merge_shards(op.group_id(), concrete._into, timeout);
                return accept();
            } else {
                if constexpr (!network_client_with_timeout_now<
                                  typename group_types::network_client_type>) {
                    return note_skipped_operator(op, skipped_operator_reason::unsupported);
                } else {
                    scatter(op.group_id(), timeout);
                    return accept();
                }
            }
        },
        op.kind());
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::heartbeat() -> std::size_t {
    _heartbeats.fetch_add(1, std::memory_order_relaxed);

    if (_cfg.report_node_heartbeat) {
        _cfg.report_node_heartbeat(build_node_report());
    }
    if (!_cfg.report_shard_heartbeat) {
        return 0;
    }

    // Built once and sent once, however many shards this host leads. That is
    // the whole point of the batch: a control plane whose load grows with shard
    // count fails at exactly the scale sharding was adopted to reach.
    auto reports = build_shard_reports();
    auto operators = _cfg.report_shard_heartbeat(reports);
    _operators_received.fetch_add(operators.size(), std::memory_order_relaxed);

    for (const auto& op : operators) {
        apply_operator(op);
    }
    return operators.size();
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::maybe_heartbeat() -> void {
    if (_cfg.heartbeat_interval <= std::chrono::milliseconds::zero()) {
        // Disabled from the tick. `heartbeat()` remains available to a caller
        // driving it on its own thread, which is what a driver with a slow
        // round trip should do rather than blocking the tick.
        return;
    }
    if (!_cfg.report_shard_heartbeat && !_cfg.report_node_heartbeat) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (_last_heartbeat.time_since_epoch().count() != 0 &&
        now - _last_heartbeat < _cfg.heartbeat_interval) {
        return;
    }
    _last_heartbeat = now;
    heartbeat();
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::transfer_leadership(const GroupId& group,
                                                          const node_id_type& to,
                                                          std::chrono::milliseconds timeout)
    -> future_type {
    auto g = find_group(group);
    if (!g || !g->_node) {
        return failed_future(
            std::make_exception_ptr(unknown_shard_exception<GroupId>{group, "no local replica"}));
    }
    note_activity(*g);
    return g->_node->transfer_leadership(to, timeout);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::scatter(const GroupId& group,
                                              std::chrono::milliseconds timeout) -> future_type {
    auto g = find_group(group);
    if (!g || !g->_node) {
        return failed_future(
            std::make_exception_ptr(unknown_shard_exception<GroupId>{group, "no local replica"}));
    }
    if (!g->_node->is_leader()) {
        // Nothing to scatter: leadership is already somewhere else, which is
        // the outcome scatter exists to produce.
        return failed_future(std::make_exception_ptr(
            shard_not_leader_exception<GroupId, node_id_type>{group, g->_node->known_leader()}));
    }

    std::vector<node_id_type> candidates;
    for (const auto& v : g->_descriptor._voters) {
        if (v != _cfg.node_id) {
            candidates.push_back(v);
        }
    }
    if (candidates.empty()) {
        return failed_future(std::make_exception_ptr(
            shard_busy_exception<GroupId>{group, "no other voter to scatter leadership to"}));
    }
    // Sorted so the choice does not depend on the order membership happened to
    // be recorded in, then round-robin so two consecutive scatters on this host
    // land on different machines. Both halves matter: without the sort the
    // "round robin" would follow an arbitrary order, and without the cursor
    // every child of one split would pick the same target.
    std::sort(candidates.begin(), candidates.end());
    const auto index = _scatter_cursor.fetch_add(1, std::memory_order_relaxed) % candidates.size();
    const auto target = candidates[index];

    _cfg.logger.info("scatter", {{"group", detail::describe_value(group)},
                                 {"target", detail::describe_value(target)},
                                 {"candidates", std::to_string(candidates.size())}});
    return g->_node->transfer_leadership(target, timeout);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::heartbeat_count() const -> std::uint64_t {
    return _heartbeats.load(std::memory_order_relaxed);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::received_operator_count() const -> std::uint64_t {
    return _operators_received.load(std::memory_order_relaxed);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::accepted_operator_count() const -> std::uint64_t {
    return _operators_accepted.load(std::memory_order_relaxed);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::skipped_operator_count() const -> std::uint64_t {
    return _operators_skipped.load(std::memory_order_relaxed);
}

template<raft_types Types, shard_key Key, raft_group_id GroupId>
auto multi_raft<Types, Key, GroupId>::skipped_operator_count(skipped_operator_reason reason) const
    -> std::uint64_t {
    std::lock_guard lock(_skip_mutex);
    auto it = _operator_skips.find(static_cast<std::uint8_t>(reason));
    return it == _operator_skips.end() ? 0 : it->second;
}

}  // namespace kythira
