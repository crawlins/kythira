// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file multi_raft_impl.hpp
/// @brief Out-of-line definitions for `multi_raft<Types, Key, GroupId>`.
///
/// Include this rather than `multi_raft.hpp` to use the class; the split
/// follows `raft.hpp`'s own declaration/definition pattern.

#include <raft/multi_raft.hpp>

#include <algorithm>
#include <chrono>
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
      _executor(std::make_unique<striped_serial_executor>(_stripe_count)) {
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

    // Until task 19 installs lazy replica creation, dropping is the safe answer.
    _demux.set_unknown_group_handler([](const GroupId&) { return unknown_group_action::drop; });

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
    if (!_executor) {
        _executor = std::make_unique<striped_serial_executor>(_stripe_count);
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

    // Drain before stopping the nodes: a queued phase task holds a group_ptr
    // and would otherwise run against a node that stop() has already torn down.
    if (_executor) {
        _executor->drain_all();
    }

    for (const auto& g : all_groups()) {
        g->_node->stop();
    }
    _demux.stop();

    // Released rather than merely stopped, so `start()` can build a fresh pool.
    // `node` has no destructor, so a joinable thread surviving stop() aborts
    // the process at teardown rather than merely leaking — leaving nothing
    // joinable is the contract, and restartability is the other half of it.
    if (_executor) {
        _executor->stop();
        _executor.reset();
    }
    persist_tombstones();
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
    const auto& group = descriptor._group_id;

    {
        std::shared_lock lock(_registry_mutex);
        if (_groups.contains(group)) {
            throw unknown_shard_exception<GroupId>{group, "already exists on this node"};
        }
    }

    auto state = std::make_shared<group_state>();
    state->_group_id = group;
    state->_descriptor = descriptor;
    state->_stripe = std::hash<GroupId>{}(group) % _stripe_count;

    node_config<group_types> node_cfg{
        .node_id = _cfg.node_id,
        .network_client = typename group_types::network_client_type{_client, group},
        .network_server = typename group_types::network_server_type{_demux, group},
        .persistence = _cfg.store_factory(group),
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
    if (_executor && !_executor->stopped() &&
        striped_serial_executor::current_stripe() != state->_stripe) {
        _executor->post_and_wait(state->_stripe, [] {});
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

    if (!_executor || _executor->stopped()) {
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
        const bool queued = _executor->post(g->_stripe, [&, g] {
            try {
                fn(*g);
            } catch (...) {
                // One group's failure must not stall the whole tick: the
                // barrier below would never be reached, and every other group
                // would stop being driven. The node's own error handling has
                // already logged whatever this was.
            }
            {
                std::lock_guard lock(m);
                --remaining;
            }
            cv.notify_one();
        });
        if (!queued) {
            std::lock_guard lock(m);
            --remaining;
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
        run_phase(ready, [](group_state& g) { g._node->replicate_to_followers(); });
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
            std::vector<std::function<void()>> work;
            {
                std::lock_guard lock(g._deferred_mutex);
                work.swap(g._deferred);
            }
            for (auto& w : work) {
                w();
            }
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
            // The arbiter and the policy channels land in Phase 9 (tasks 23-25).
            // The phase exists now so that when they do, they are already
            // ordered after apply and running on each group's own stripe.
        }
        report._policy_duration = clock::now() - t0;
    }

    evaluate_hibernation(ready);
    return report;
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

}  // namespace kythira
