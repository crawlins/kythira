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

    auto state = std::make_shared<group_state>();
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
            auto cmd = decode_split_command<GroupId, Key, node_id_type>(
                entry.command(),
                key_codec_adapter{
                    [this](const Key& k) { return encode_key_or_default(k); },
                    [this](const std::vector<std::byte>& b) { return decode_key_or_default(b); }});
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
                auto cmd = decode_split_command<GroupId, Key, node_id_type>(
                    entry.command(),
                    key_codec_adapter{[this](const Key& k) { return encode_key_or_default(k); },
                                      [this](const std::vector<std::byte>& b) {
                                          return decode_key_or_default(b);
                                      }});
                apply_split(*state, cmd, index, entry.term(), sm);
                break;
            }
            default:
                // The merge entry types land in Phase 8 (tasks 20-22). Until
                // then nothing proposes them, so reaching here means a peer is
                // running a newer binary — worth saying so rather than
                // silently ignoring.
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
    auto g = find_group(group);
    if (!g) {
        return failed_future(
            std::make_exception_ptr(unknown_shard_exception<GroupId>{group, "no local replica"}));
    }

    // ── step 1: gate ─────────────────────────────────────────────────────────
    //
    // Only `stable` admits a new operation. Compare-exchange rather than
    // check-then-set: two channels proposing in the same interval is exactly
    // the case the operation state exists to make impossible.
    if (!g->_node->is_leader()) {
        return failed_future(std::make_exception_ptr(
            shard_not_leader_exception<GroupId, node_id_type>{group, g->_node->known_leader()}));
    }
    auto expected = shard_operation_state::stable;
    if (!g->_operation.compare_exchange_strong(expected, shard_operation_state::splitting)) {
        return failed_future(
            std::make_exception_ptr(shard_busy_exception<GroupId>{group, to_string(expected)}));
    }

    // From here every failure path must put the state back, or the shard is
    // wedged in `splitting` with nothing running.
    auto release = [g] { g->_operation.store(shard_operation_state::stable); };

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
            release();
            return failed_future(std::make_exception_ptr(
                split_key_out_of_range_exception<GroupId, Key>{group, k, parent._range}));
        }
    }

    std::vector<Key> chosen;
    if constexpr (splittable_state_machine<typename Types::state_machine_type, Key>) {
        const auto limit = _cfg.batch_split_limit;
        const auto& range = parent._range;
        g->_node->with_state_machine([&](typename Types::state_machine_type& sm) {
            for (const auto& k : at_keys) {
                if (sm.can_split_at(k)) {
                    chosen.push_back(k);
                }
            }
            if (chosen.empty()) {
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
    if (chosen.size() > _cfg.batch_split_limit) {
        chosen.resize(_cfg.batch_split_limit);
    }

    if (chosen.empty()) {
        release();
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
            release();
            return failed_future(std::make_exception_ptr(std::runtime_error(
                "multi_raft: split needs allocate_group_ids; ids must come from a "
                "cluster-scope authority, never from the proposing node")));
        }
        new_ids = _cfg.allocate_group_ids(new_ids_needed);
        if (new_ids.size() < new_ids_needed) {
            release();
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
                           ._reason = split_reason::admin,
                           ._pd_operation_id = std::nullopt};

    auto payload = encode_split_command<GroupId, Key, node_id_type>(
        cmd, key_codec_adapter{
                 [this](const Key& k) { return encode_key_or_default(k); },
                 [this](const std::vector<std::byte>& b) { return decode_key_or_default(b); }});

    _cfg.logger.info("Proposing split", {{"group", detail::describe_value(group)},
                                         {"children", std::to_string(child_count)},
                                         {"parent_version", std::to_string(parent._epoch._version)},
                                         {"child_version", std::to_string(child_epoch._version)}});

    auto future = g->_node->propose_admin_entry(entry_type::split, std::move(payload), timeout);

    // The operation state is cleared by apply on success. A proposal that never
    // commits would otherwise wedge the shard, so the release is chained onto
    // the future's failure path too.
    return std::move(future).thenTry([g](auto&& result) {
        if (result.hasException()) {
            g->_operation.store(shard_operation_state::stable);
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
    parent._operation.store(shard_operation_state::stable);
    _applied_splits.fetch_add(1, std::memory_order_relaxed);

    _cfg.logger.info("Applied split",
                     {{"group", detail::describe_value(parent._group_id)},
                      {"children", std::to_string(cmd._children.size())},
                      {"at_index", std::to_string(at_index)},
                      {"new_version", std::to_string(cmd._children.front()._epoch._version)}});
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
        if (is_read) {
            local->_reads.fetch_add(1, std::memory_order_relaxed);
            return local->_node->read_state(timeout);
        }
        local->_writes.fetch_add(1, std::memory_order_relaxed);
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

}  // namespace kythira
