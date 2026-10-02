// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/**
 * Multi-node Raft test fixture.
 *
 * Runs N real `kythira::node` instances over the in-process network
 * simulator, each driven by its own ticker thread the way an event loop
 * drives a deployed node. Everything a multi-node scenario needs to observe
 * or perturb the cluster lives here:
 *
 * - lifecycle: start, stop (crash) and restart a node, keeping its
 *   in-memory persistence across the restart;
 * - network: partitions into arbitrary groups, single-node isolation,
 *   per-node latency and loss, healing;
 * - inspection: the current leader, terms, commit indices, committed logs,
 *   and a log-matching check across every pair of nodes;
 * - an election-safety observer that samples every node's (term, leader)
 *   pair throughout the test, so a scenario can assert that no term ever
 *   had two leaders, and that a stable cluster never had two at once.
 *
 * Node ids are 1..N. Simulator addresses are the decimal ids.
 *
 * Requirements: raft-consensus 1.1, 1.2, 1.3, 2.1
 * Tasks: raft-consensus 700, 710, 730
 */

#include <raft/future_default.hpp>
#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#include "test_timeout_scale.hpp"

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace kythira::test {

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
/// Register in each test translation unit with
/// `using kythira::test::folly_init_fixture;` and
/// `BOOST_GLOBAL_FIXTURE(folly_init_fixture);` (the macro pastes its argument
/// into an identifier, so it must be unqualified). The Folly backend needs
/// its singletons.
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("raft_multi_node_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
#endif

/// Types bundle for the fixture's nodes: simulator transport, in-memory
/// persistence and the key/value test state machine.
struct multi_node_test_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using raft_network_types = kythira::raft_simulator_network_types<std::string>;
    using network_client_type =
        kythira::simulator_network_client<raft_network_types, serializer_type,
                                          serialized_data_type>;
    using network_server_type =
        kythira::simulator_network_server<raft_network_types, serializer_type,
                                          serialized_data_type>;

    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = kythira::test_key_value_state_machine<log_index_type>;
    using configuration_type = kythira::raft_configuration;

    using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
    using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;

    using request_vote_request_type =
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type>;
    using append_entries_request_type =
        kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type>;
    using append_entries_response_type =
        kythira::append_entries_response<term_id_type, log_index_type>;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type>;
    using install_snapshot_response_type = kythira::install_snapshot_response<term_id_type>;

    // One generous placement group (see raft_multi_node_fixture::add_node), so
    // add_learner()/promote_to_voter() capacity checks never get in the way.
    using quorum_manager_type =
        kythira::no_op_quorum_manager<node_id_type, std::string, std::string>;
};

/// Cluster shape and protocol timings.
struct cluster_config {
    std::size_t node_count = 3;
    std::chrono::milliseconds election_timeout_min{150};
    std::chrono::milliseconds election_timeout_max{300};
    std::chrono::milliseconds heartbeat_interval{25};
    std::chrono::milliseconds rpc_timeout{100};
    /// How often each node's ticker calls check_election_timeout() and
    /// check_heartbeat_timeout().
    std::chrono::milliseconds tick_interval{5};
    /// Default edge latency and delivery probability for the mesh.
    std::chrono::milliseconds network_latency{0};
    double network_reliability = 1.0;
    kythira::log_level log_level = kythira::log_level::error;
};

/// Outcome of a client operation (submit_command, add_server, ...).
struct op_result {
    bool completed = false;  ///< false: still pending when the wait gave up
    bool ok = false;         ///< resolved with a value
    std::vector<std::byte> value;
    std::string error;
};

/// Waits up to `timeout` for a kythira future of bytes and reports how it
/// ended. The callbacks own their state, so a future that resolves after the
/// wait gives up is harmless.
template<typename Future>
auto await_result(Future fut, std::chrono::milliseconds timeout) -> op_result {
    struct shared_state {
        std::mutex mu;
        std::condition_variable cv;
        op_result result;
    };
    auto state = std::make_shared<shared_state>();
    std::move(fut)
        .thenValue([state](std::vector<std::byte> value) {
            std::lock_guard lock(state->mu);
            state->result = op_result{.completed = true, .ok = true, .value = std::move(value)};
            state->cv.notify_all();
        })
        .thenError([state](const std::exception_ptr& error) {
            std::string what = "unknown error";
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& e) {
                what = e.what();
            } catch (...) {
            }
            std::lock_guard lock(state->mu);
            state->result = op_result{.completed = true, .ok = false, .error = std::move(what)};
            state->cv.notify_all();
        })
        .detach();
    std::unique_lock lock(state->mu);
    state->cv.wait_for(lock, timeout, [&] { return state->result.completed; });
    return state->result;
}

/// Polls `pred` every 5ms until it holds or `deadline` passes.
template<typename Pred> auto wait_until(Pred pred, std::chrono::milliseconds deadline) -> bool {
    auto end = std::chrono::steady_clock::now() + deadline;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return true;
}

class raft_multi_node_fixture {
public:
    using types = multi_node_test_types;
    using node_type = kythira::node<types>;
    using node_id_type = types::node_id_type;
    using term_id_type = types::term_id_type;
    using log_index_type = types::log_index_type;
    using log_entry_type = types::log_entry_type;
    using state_machine_type = types::state_machine_type;
    using simulator_type = network_simulator::NetworkSimulator<types::raft_network_types>;

    explicit raft_multi_node_fixture(cluster_config config = cluster_config{}) : _config(config) {
        if (_config.node_count < 1 || _config.node_count > 9) {
            throw std::invalid_argument("node_count must be between 1 and 9");
        }
        _sim.start();
    }

    raft_multi_node_fixture(const raft_multi_node_fixture&) = delete;
    auto operator=(const raft_multi_node_fixture&) -> raft_multi_node_fixture& = delete;

    ~raft_multi_node_fixture() { cleanup(); }

    // ── Cluster setup and lifecycle ──────────────────────────────────────────

    /// Creates nodes 1..node_count, each configured with all of them as
    /// voters, on a fully connected mesh, and starts the election-safety
    /// observer. Nodes are created stopped.
    auto initialize_cluster() -> void {
        std::vector<node_id_type> voters;
        for (node_id_type id = 1; id <= _config.node_count; ++id) {
            voters.push_back(id);
        }
        for (auto id : voters) {
            add_node(id, voters);
        }
        start_observer();
    }

    /// Creates one more node (stopped) with the given initial configuration
    /// and connects it to every node. A node joining by add_server should list
    /// itself as a learner so it never campaigns before the leader's
    /// configuration entry reaches it.
    auto add_node(node_id_type id, const std::vector<node_id_type>& voters,
                  const std::vector<node_id_type>& learners = {}) -> node_type& {
        std::lock_guard lock(_mu);
        if (_nodes.contains(id)) {
            throw std::invalid_argument("node already exists: " + std::to_string(id));
        }
        auto net = _sim.create_node(address_of(id));
        kythira::raft_configuration cfg;
        cfg._election_timeout_min = _config.election_timeout_min;
        cfg._election_timeout_max = _config.election_timeout_max;
        cfg._heartbeat_interval = _config.heartbeat_interval;
        cfg._rpc_timeout = _config.rpc_timeout;
        kythira::node_config<types> ncfg{
            .node_id = id,
            .network_client = {net, types::serializer_type{}},
            .network_server = {net, types::serializer_type{}},
            .persistence = {},
            .logger = kythira::console_logger{_config.log_level},
            .metrics = {},
            .membership = {},
            .config = cfg,
            .self_address = address_of(id),
            .quorum_manager = types::quorum_manager_type{kythira::desired_topology<std::string>{
                .groups = {{.group_id = "", .target_count = 10}}}},
        };
        auto entry = std::make_unique<node_entry>();
        entry->raft = std::make_unique<node_type>(std::move(ncfg));
        if (learners.empty()) {
            entry->raft->set_cluster_configuration(voters);
        } else {
            entry->raft->set_cluster_configuration(voters, learners);
        }
        auto& ref = *entry->raft;
        _nodes.emplace(id, std::move(entry));
        rebuild_edges_locked();
        return ref;
    }

    auto start_all_nodes() -> void {
        for (auto id : get_node_ids()) {
            start_node(id);
        }
    }

    auto stop_all_nodes() -> void {
        for (auto id : get_node_ids()) {
            stop_node(id);
        }
    }

    /// Starts the node and its ticker. A no-op on a running node.
    auto start_node(node_id_type id) -> void {
        auto& e = entry(id);
        if (e.running.load()) {
            return;
        }
        e.raft->start();
        e.ticker_stop = std::make_shared<std::atomic<bool>>(false);
        e.ticker = std::thread(
            [raft = e.raft.get(), stop = e.ticker_stop, interval = _config.tick_interval] {
                while (!stop->load()) {
                    raft->check_election_timeout();
                    raft->check_heartbeat_timeout();
                    std::this_thread::sleep_for(interval);
                }
            });
        e.running.store(true);
    }

    /// Crash-stops the node: its ticker ends and it stops serving RPCs. Its
    /// persistence (term, vote, log) survives for restart_node().
    auto stop_node(node_id_type id) -> void {
        auto& e = entry(id);
        if (!e.running.load()) {
            return;
        }
        e.running.store(false);
        e.ticker_stop->store(true);
        if (e.ticker.joinable()) {
            e.ticker.join();
        }
        e.raft->stop();
    }

    auto restart_node(node_id_type id) -> void {
        stop_node(id);
        start_node(id);
    }

    /// Stops the observer and every node. Idempotent.
    auto cleanup() -> void {
        stop_observer();
        for (auto id : get_node_ids()) {
            stop_node(id);
        }
        _sim.stop();
    }

    // ── Accessors ────────────────────────────────────────────────────────────

    [[nodiscard]] auto node(node_id_type id) -> node_type& { return *entry(id).raft; }

    [[nodiscard]] auto get_node_count() const -> std::size_t {
        std::lock_guard lock(_mu);
        return _nodes.size();
    }

    [[nodiscard]] auto get_node_ids() const -> std::vector<node_id_type> {
        std::lock_guard lock(_mu);
        std::vector<node_id_type> ids;
        for (const auto& [id, _] : _nodes) {
            ids.push_back(id);
        }
        return ids;
    }

    [[nodiscard]] auto is_node_running(node_id_type id) const -> bool {
        std::lock_guard lock(_mu);
        auto it = _nodes.find(id);
        return it != _nodes.end() && it->second->running.load();
    }

    [[nodiscard]] auto running_node_ids() const -> std::vector<node_id_type> {
        std::lock_guard lock(_mu);
        std::vector<node_id_type> ids;
        for (const auto& [id, e] : _nodes) {
            if (e->running.load()) {
                ids.push_back(id);
            }
        }
        return ids;
    }

    [[nodiscard]] auto config() const -> const cluster_config& { return _config; }

    // ── Leadership ───────────────────────────────────────────────────────────

    /// The leader among running nodes in `among` (all nodes when empty): the
    /// node claiming leadership whose term is at least every other candidate
    /// node's term. A deposed leader stranded in a minority partition holds a
    /// lower term, so it is never returned while a newer leader exists.
    [[nodiscard]] auto get_leader(const std::vector<node_id_type>& among = {}) const
        -> std::optional<node_id_type> {
        auto ids = among.empty() ? running_node_ids() : among;
        std::optional<node_id_type> leader;
        term_id_type leader_term = 0;
        term_id_type max_term = 0;
        for (auto id : ids) {
            if (!is_node_running(id)) {
                continue;
            }
            auto s = entry(id).raft->debug_state();
            max_term = std::max(max_term, s.current_term);
            if (s.is_leader && (!leader || s.current_term > leader_term)) {
                leader = id;
                leader_term = s.current_term;
            }
        }
        if (leader && leader_term == max_term) {
            return leader;
        }
        return std::nullopt;
    }

    auto wait_for_leader(std::chrono::milliseconds timeout,
                         const std::vector<node_id_type>& among = {})
        -> std::optional<node_id_type> {
        std::optional<node_id_type> leader;
        wait_until(
            [&] {
                leader = get_leader(among);
                return leader.has_value();
            },
            timeout);
        return leader;
    }

    [[nodiscard]] auto term_of(node_id_type id) const -> term_id_type {
        return entry(id).raft->debug_state().current_term;
    }

    [[nodiscard]] auto commit_index_of(node_id_type id) const -> log_index_type {
        return entry(id).raft->debug_state().commit_index;
    }

    [[nodiscard]] auto last_applied_of(node_id_type id) const -> log_index_type {
        return entry(id).raft->last_applied_index();
    }

    /// One line per node (running, role, term, log end, commit, applied) for
    /// failure messages.
    [[nodiscard]] auto cluster_summary() const -> std::string {
        std::string out;
        for (auto id : get_node_ids()) {
            const auto& raft = *entry(id).raft;
            auto s = raft.debug_state();
            auto state = raft.get_state();
            auto last = raft.last_log_index();
            auto tail = raft.log_entries_between(last, last);
            out += "\n  node " + std::to_string(id) + ": " +
                   (is_node_running(id) ? "running" : "stopped") + ", " +
                   (state == kythira::server_state::leader      ? "leader"
                    : state == kythira::server_state::candidate ? "candidate"
                                                                : "follower") +
                   ", term " + std::to_string(s.current_term) + ", last log " +
                   std::to_string(last) + "/" +
                   (tail.empty() ? std::string{"-"} : std::to_string(tail.front().term())) +
                   ", commit " + std::to_string(s.commit_index) + ", applied " +
                   std::to_string(s.last_applied);
        }
        return out;
    }

    // ── Client operations ────────────────────────────────────────────────────

    /// Submits `command` to node `id`.
    auto submit_to(node_id_type id, const std::vector<std::byte>& command,
                   std::chrono::milliseconds timeout) -> op_result {
        return await_result(node(id).submit_command(command, timeout),
                            timeout + std::chrono::milliseconds{500});
    }

    /// Submits `command` to whichever node leads, retrying through leader
    /// changes until it commits or `timeout` elapses. A retried command may be
    /// committed twice; the key/value commands the fixture builds are
    /// idempotent.
    auto submit(const std::vector<std::byte>& command, std::chrono::milliseconds timeout)
        -> op_result {
        auto end = std::chrono::steady_clock::now() + timeout;
        op_result last{.error = "no leader"};
        auto no_leader = [&] { last.error = "no leader:" + cluster_summary(); };
        while (std::chrono::steady_clock::now() < end) {
            auto leader = get_leader();
            if (!leader) {
                no_leader();
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                continue;
            }
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                end - std::chrono::steady_clock::now());
            // A leader stranded in a minority looks current until the majority
            // elects past it, so no single attempt may spend the whole budget.
            auto attempt = std::clamp(remaining, std::chrono::milliseconds{50},
                                      kythira::testing::scaled_deadline(1000));
            last = submit_to(*leader, command, attempt);
            if (last.ok) {
                return last;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return last;
    }

    static auto put_command(const std::string& key, const std::string& value)
        -> std::vector<std::byte> {
        return state_machine_type::make_put_command(key, value);
    }

    static auto get_command(const std::string& key) -> std::vector<std::byte> {
        return state_machine_type::make_get_command(key);
    }

    // ── Network conditions ───────────────────────────────────────────────────

    /// Splits the cluster: nodes in different groups cannot reach each other.
    /// Nodes left out of every group form one more group of their own. The
    /// simulator routes over multiple hops, so every cross-group edge goes.
    auto partition(const std::vector<std::vector<node_id_type>>& groups) -> void {
        std::lock_guard lock(_mu);
        _group_of.clear();
        std::size_t g = 1;
        for (const auto& group : groups) {
            for (auto id : group) {
                _group_of[id] = g;
            }
            ++g;
        }
        rebuild_edges_locked();
    }

    /// Cuts `id` off from every other node.
    auto isolate(node_id_type id) -> void { partition({{id}}); }

    /// Restores full connectivity (latency/loss overrides are kept).
    auto heal() -> void {
        std::lock_guard lock(_mu);
        _group_of.clear();
        rebuild_edges_locked();
    }

    /// Sets latency and delivery probability on every edge touching `id`.
    auto set_node_network(node_id_type id, std::chrono::milliseconds latency, double reliability)
        -> void {
        std::lock_guard lock(_mu);
        _edge_override[id] = network_simulator::NetworkEdge{latency, reliability};
        rebuild_edges_locked();
    }

    auto clear_node_network(node_id_type id) -> void {
        std::lock_guard lock(_mu);
        _edge_override.erase(id);
        rebuild_edges_locked();
    }

    // ── Log inspection ───────────────────────────────────────────────────────

    /// Entries 1..commit_index of node `id`, copied under its lock.
    [[nodiscard]] auto committed_entries(node_id_type id) const -> std::vector<log_entry_type> {
        const auto& raft = *entry(id).raft;
        auto ci = raft.debug_state().commit_index;
        if (ci == 0) {
            return {};
        }
        return raft.log_entries_between(1, ci);
    }

    /// Raft's Log Matching and State Machine Safety, checked across every pair
    /// of nodes: wherever two committed prefixes overlap, they hold the same
    /// entries. Also checks that each node's committed prefix holds indices
    /// 1..commit_index exactly once. Returns the first problem found, or
    /// nullopt.
    [[nodiscard]] auto committed_logs_mismatch() const -> std::optional<std::string> {
        std::map<node_id_type, std::vector<log_entry_type>> logs;
        for (auto id : get_node_ids()) {
            logs[id] = committed_entries(id);
            // Each node's own log must hold every index exactly once.
            const auto& log = logs[id];
            for (std::size_t i = 0; i < log.size(); ++i) {
                if (log[i].index() != i + 1) {
                    return "node " + std::to_string(id) + " holds " + describe(log[i]) +
                           " at log position " + std::to_string(i + 1);
                }
            }
        }
        for (const auto& [a, la] : logs) {
            for (const auto& [b, lb] : logs) {
                if (a >= b) {
                    continue;
                }
                auto n = std::min(la.size(), lb.size());
                for (std::size_t i = 0; i < n; ++i) {
                    if (la[i].index() != lb[i].index() || la[i].term() != lb[i].term() ||
                        la[i].command() != lb[i].command() || la[i].type() != lb[i].type()) {
                        return "committed entry " + std::to_string(i + 1) +
                               " differs between node " + std::to_string(a) + " (" +
                               describe(la[i]) + ") and node " + std::to_string(b) + " (" +
                               describe(lb[i]) + ")";
                    }
                }
            }
        }
        return std::nullopt;
    }

    /// Waits until every running node in `ids` (all running nodes when empty)
    /// has applied the same index, at least `min_index`.
    auto wait_for_convergence(std::chrono::milliseconds timeout, log_index_type min_index = 0,
                              const std::vector<node_id_type>& ids = {}) -> bool {
        return wait_until(
            [&] {
                auto members = ids.empty() ? running_node_ids() : ids;
                std::optional<log_index_type> applied;
                for (auto id : members) {
                    auto a = last_applied_of(id);
                    if (a < min_index || (applied && *applied != a)) {
                        return false;
                    }
                    applied = a;
                }
                return applied.has_value();
            },
            timeout);
    }

    // ── Election-safety observer ─────────────────────────────────────────────

    /// Terms in which more than one node claimed leadership, with the nodes.
    /// Raft's Election Safety property says this is always empty.
    [[nodiscard]] auto election_safety_violations() const -> std::vector<std::string> {
        std::lock_guard lock(_observer_mu);
        std::vector<std::string> out;
        for (const auto& [term, leaders] : _leaders_by_term) {
            if (leaders.size() > 1) {
                std::string s = "term " + std::to_string(term) + ":";
                for (auto id : leaders) {
                    s += " " + std::to_string(id);
                }
                out.push_back(s);
            }
        }
        return out;
    }

    /// Every (term, leader) pair the observer has seen.
    [[nodiscard]] auto observed_leaders() const -> std::map<term_id_type, std::set<node_id_type>> {
        std::lock_guard lock(_observer_mu);
        return _leaders_by_term;
    }

    /// The most nodes the observer ever saw claiming leadership in one sample,
    /// whatever their terms, since the last reset. Two at once is legal Raft
    /// (a deposed leader that has not heard of the new term yet) but must not
    /// happen in a cluster whose network and membership are both healthy.
    [[nodiscard]] auto max_simultaneous_leaders() const -> std::size_t {
        std::lock_guard lock(_observer_mu);
        return _max_simultaneous;
    }

    auto reset_simultaneous_leaders() -> void {
        std::lock_guard lock(_observer_mu);
        _max_simultaneous = 0;
    }

private:
    struct node_entry {
        std::unique_ptr<node_type> raft;
        std::atomic<bool> running{false};
        std::shared_ptr<std::atomic<bool>> ticker_stop;
        std::thread ticker;
    };

    static auto address_of(node_id_type id) -> std::string { return std::to_string(id); }

    static auto describe(const log_entry_type& e) -> std::string {
        return "index " + std::to_string(e.index()) + ", term " + std::to_string(e.term()) +
               ", type " + std::to_string(static_cast<int>(e.type())) + ", " +
               std::to_string(e.command().size()) + " command bytes";
    }

    auto entry(node_id_type id) const -> node_entry& {
        std::lock_guard lock(_mu);
        auto it = _nodes.find(id);
        if (it == _nodes.end()) {
            throw std::invalid_argument("node not found: " + std::to_string(id));
        }
        return *it->second;
    }

    auto connected_locked(node_id_type a, node_id_type b) const -> bool {
        auto ga = _group_of.find(a);
        auto gb = _group_of.find(b);
        auto group_a = ga == _group_of.end() ? 0 : ga->second;
        auto group_b = gb == _group_of.end() ? 0 : gb->second;
        return group_a == group_b;
    }

    auto edge_locked(node_id_type a, node_id_type b) const -> network_simulator::NetworkEdge {
        network_simulator::NetworkEdge edge{_config.network_latency, _config.network_reliability};
        for (auto id : {a, b}) {
            if (auto it = _edge_override.find(id); it != _edge_override.end()) {
                edge._latency = std::max(edge._latency, it->second._latency);
                edge._reliability = std::min(edge._reliability, it->second._reliability);
            }
        }
        return edge;
    }

    auto rebuild_edges_locked() -> void {
        for (const auto& [a, _] : _nodes) {
            for (const auto& [b, __] : _nodes) {
                if (a == b) {
                    continue;
                }
                if (connected_locked(a, b)) {
                    _sim.add_edge(address_of(a), address_of(b), edge_locked(a, b));
                } else {
                    _sim.remove_edge(address_of(a), address_of(b));
                }
            }
        }
    }

    auto start_observer() -> void {
        if (_observer.joinable()) {
            return;
        }
        _observer_stop.store(false);
        _observer = std::thread([this] {
            while (!_observer_stop.load()) {
                sample_leaders();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        });
    }

    auto stop_observer() -> void {
        _observer_stop.store(true);
        if (_observer.joinable()) {
            _observer.join();
        }
    }

    auto sample_leaders() -> void {
        std::vector<std::pair<node_id_type, term_id_type>> leaders;
        for (auto id : running_node_ids()) {
            // debug_state() reads term and role under one lock, so the pair
            // is consistent even while the node is mid-election.
            auto s = entry(id).raft->debug_state();
            if (s.is_leader) {
                leaders.emplace_back(id, s.current_term);
            }
        }
        std::lock_guard lock(_observer_mu);
        for (const auto& [id, term] : leaders) {
            _leaders_by_term[term].insert(id);
        }
        _max_simultaneous = std::max(_max_simultaneous, leaders.size());
    }

    cluster_config _config;
    simulator_type _sim;

    mutable std::mutex _mu;
    std::map<node_id_type, std::unique_ptr<node_entry>> _nodes;
    std::map<node_id_type, std::size_t> _group_of;
    std::map<node_id_type, network_simulator::NetworkEdge> _edge_override;

    mutable std::mutex _observer_mu;
    std::map<term_id_type, std::set<node_id_type>> _leaders_by_term;
    std::size_t _max_simultaneous = 0;
    std::atomic<bool> _observer_stop{false};
    std::thread _observer;
};

}  // namespace kythira::test
