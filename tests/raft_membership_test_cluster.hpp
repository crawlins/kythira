// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A three-node Raft cluster over the network simulator for membership-change
// property tests, with recording loggers and metrics, created for
// raft_add_server_implementation_property_test (#515) and shared with the
// remove_server suite. Nodes 1-3 form the cluster and node 1 leads, because
// only node 1 is ever ticked for elections; `spare()` makes node 4. Include
// from one translation unit per test binary.

#include <raft/future_default.hpp>

#include <raft/config_entry.hpp>
#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::test::membership {

using namespace std::chrono_literals;

// ── Observability ──────────────────────────────────────────────────────────

/// Every message one node logged. Shared by all copies of its logger.
struct log_sink {
    struct entry {
        kythira::log_level level;
        std::string message;
        std::map<std::string, std::string> fields;
    };

    mutable std::mutex mutex;
    std::vector<entry> entries;

    [[nodiscard]] auto count(std::string_view message) const -> std::size_t {
        const std::lock_guard<std::mutex> lock(mutex);
        return static_cast<std::size_t>(std::count_if(
            entries.begin(), entries.end(), [&](const entry& e) { return e.message == message; }));
    }

    /// The `field` values of every entry logged as `message`, in order.
    [[nodiscard]] auto field_values(std::string_view message, const std::string& field) const
        -> std::vector<std::string> {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> out;
        for (const auto& e : entries) {
            if (e.message != message) {
                continue;
            }
            if (auto it = e.fields.find(field); it != e.fields.end()) {
                out.push_back(it->second);
            }
        }
        return out;
    }

    [[nodiscard]] auto level_of(std::string_view message) const
        -> std::optional<kythira::log_level> {
        const std::lock_guard<std::mutex> lock(mutex);
        for (const auto& e : entries) {
            if (e.message == message) {
                return e.level;
            }
        }
        return std::nullopt;
    }
};

class recording_logger {
public:
    using kvps = std::vector<std::pair<std::string_view, std::string_view>>;

    recording_logger() : _sink(std::make_shared<log_sink>()) {}
    explicit recording_logger(std::shared_ptr<log_sink> sink) : _sink(std::move(sink)) {}

    auto log(kythira::log_level level, std::string_view message) -> void {
        log(level, message, kvps{});
    }
    auto log(kythira::log_level level, std::string_view message, const kvps& pairs) -> void {
        log_sink::entry e{.level = level, .message = std::string(message), .fields = {}};
        for (const auto& [k, v] : pairs) {
            e.fields.emplace(std::string(k), std::string(v));
        }
        const std::lock_guard<std::mutex> lock(_sink->mutex);
        _sink->entries.push_back(std::move(e));
    }

    auto trace(std::string_view m) -> void { log(kythira::log_level::trace, m); }
    auto debug(std::string_view m) -> void { log(kythira::log_level::debug, m); }
    auto info(std::string_view m) -> void { log(kythira::log_level::info, m); }
    auto warning(std::string_view m) -> void { log(kythira::log_level::warning, m); }
    auto error(std::string_view m) -> void { log(kythira::log_level::error, m); }
    auto critical(std::string_view m) -> void { log(kythira::log_level::critical, m); }
    auto trace(std::string_view m, const kvps& p) -> void { log(kythira::log_level::trace, m, p); }
    auto debug(std::string_view m, const kvps& p) -> void { log(kythira::log_level::debug, m, p); }
    auto info(std::string_view m, const kvps& p) -> void { log(kythira::log_level::info, m, p); }
    auto warning(std::string_view m, const kvps& p) -> void {
        log(kythira::log_level::warning, m, p);
    }
    auto error(std::string_view m, const kvps& p) -> void { log(kythira::log_level::error, m, p); }
    auto critical(std::string_view m, const kvps& p) -> void {
        log(kythira::log_level::critical, m, p);
    }

private:
    std::shared_ptr<log_sink> _sink;
};

/// Every metric one node emitted: its name and dimensions.
struct metric_sink {
    struct entry {
        std::string name;
        std::map<std::string, std::string> dimensions;
    };

    mutable std::mutex mutex;
    std::vector<entry> entries;

    [[nodiscard]] auto count(std::string_view name) const -> std::size_t {
        const std::lock_guard<std::mutex> lock(mutex);
        return static_cast<std::size_t>(std::count_if(
            entries.begin(), entries.end(), [&](const entry& e) { return e.name == name; }));
    }

    [[nodiscard]] auto dimension_values(std::string_view name, const std::string& dimension) const
        -> std::vector<std::string> {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> out;
        for (const auto& e : entries) {
            if (e.name != name) {
                continue;
            }
            if (auto it = e.dimensions.find(dimension); it != e.dimensions.end()) {
                out.push_back(it->second);
            }
        }
        return out;
    }
};

/// Models `kythira::metrics`. The node builds a metric over several calls
/// (name, dimensions, emit) on one member it shares between its own thread
/// and its future continuations, so the half-built metric is kept per thread:
/// two threads emitting at once would otherwise mix their dimensions.
class recording_metrics {
public:
    recording_metrics() : _sink(std::make_shared<metric_sink>()) {}
    explicit recording_metrics(std::shared_ptr<metric_sink> sink) : _sink(std::move(sink)) {}

    auto set_metric_name(std::string_view name) -> void { pending().name = std::string(name); }
    auto add_dimension(std::string_view name, std::string_view value) -> void {
        pending().dimensions.insert_or_assign(std::string(name), std::string(value));
    }
    auto add_one() -> void {}
    auto add_count(std::int64_t) -> void {}
    auto add_duration(std::chrono::nanoseconds) -> void {}
    auto add_value(double) -> void {}
    auto emit() -> void {
        auto& p = pending();
        if (!p.name.empty()) {
            const std::lock_guard<std::mutex> lock(_sink->mutex);
            _sink->entries.push_back(p);
        }
        p = {};
    }

private:
    auto pending() -> metric_sink::entry& {
        thread_local std::unordered_map<const metric_sink*, metric_sink::entry> per_sink;
        return per_sink[_sink.get()];
    }

    std::shared_ptr<metric_sink> _sink;
};

static_assert(kythira::metrics<recording_metrics>);
static_assert(kythira::diagnostic_logger<recording_logger>);

// ── Node types ─────────────────────────────────────────────────────────────

template<typename NodeId, typename Address> class preset_peer_discovery {
public:
    using node_id_type = NodeId;
    using address_type = Address;

    auto register_node(NodeId, Address) -> kythira::future_default<void> {
        return kythira::future_factory_default::makeFuture();
    }
    [[nodiscard]] auto find_peers(std::chrono::milliseconds) const
        -> kythira::future_default<std::vector<kythira::peer_info<NodeId, Address>>> {
        return kythira::future_factory_default::makeFuture(
            std::vector<kythira::peer_info<NodeId, Address>>{});
    }
};

struct test_types {
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
    using logger_type = recording_logger;
    using metrics_type = recording_metrics;
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

    using address_type = std::string;
    using peer_discovery_type = preset_peer_discovery<node_id_type, address_type>;
    using cluster_join_request_type = kythira::cluster_join_request<node_id_type, address_type>;
    using cluster_join_response_type = kythira::cluster_join_response<node_id_type, address_type>;
};

using test_node = kythira::node<test_types>;
using sim_t = network_simulator::NetworkSimulator<test_types::raft_network_types>;
using sm_t = kythira::test_key_value_state_machine<test_types::log_index_type>;
using config_t = kythira::cluster_configuration<std::uint64_t>;

inline auto make_fast_config() -> kythira::raft_configuration {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = 80ms;
    cfg._election_timeout_max = 160ms;
    cfg._heartbeat_interval = 26ms;
    cfg._rpc_timeout = 200ms;
    cfg._bootstrap_retry_interval = 200ms;
    cfg._bootstrap_peer_find_timeout = 100ms;
    // Both the catch-up stall budget and the joint-consensus timeout are ten
    // of these. The default (5 s) would make the failure cases slow.
    cfg._append_entries_timeout = 200ms;
    return cfg;
}

template<typename Pred> auto wait_until(Pred pred, std::chrono::milliseconds deadline = 5000ms) {
    const auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

inline auto contains(const std::vector<std::uint64_t>& ids, std::uint64_t id) -> bool {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

/// How a future settled: its value's size, or its exception's message.
struct outcome {
    std::atomic<bool> resolved{false};
    std::atomic<bool> failed{false};
    std::atomic<std::size_t> value_size{0};
    std::mutex mutex;
    std::string error;

    [[nodiscard]] auto done() const -> bool { return resolved || failed; }
    [[nodiscard]] auto message() -> std::string {
        const std::lock_guard<std::mutex> lock(mutex);
        return error;
    }
};

inline auto watch(test_node::future_type future, outcome& out) -> void {
    std::move(future)
        .thenValue([&out](std::vector<std::byte> v) {
            out.value_size = v.size();
            out.resolved = true;
        })
        .thenError([&out](const std::exception_ptr& e) {
            {
                const std::lock_guard<std::mutex> lock(out.mutex);
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    out.error = ex.what();
                } catch (...) {
                    out.error = "non-standard exception";
                }
            }
            out.failed = true;
        })
        .detach();
}

inline auto mentions(const std::string& text, std::string_view needle) -> bool {
    return text.find(needle) != std::string::npos;
}

/// The configuration entries in a log, decoded, with their indices.
inline auto configuration_entries(const test_node& n)
    -> std::vector<std::pair<std::uint64_t, config_t>> {
    std::vector<std::pair<std::uint64_t, config_t>> out;
    for (const auto& e : n.debug_state().log) {
        if (e.type() == kythira::entry_type::configuration) {
            out.emplace_back(e.index(),
                             kythira::deserialize_configuration<std::uint64_t>(e.command()));
        }
    }
    return out;
}

// ── Cluster harness ────────────────────────────────────────────────────────

/// Nodes 1-3 form a cluster led by node 1. `spare()` creates node 4, reachable
/// or not, which the cases then add.
class cluster {
public:
    explicit cluster(kythira::raft_configuration cfg = make_fast_config()) : _cfg(cfg) {
        _sim.start();
        for (std::uint64_t id : {1u, 2u, 3u}) {
            create(id);
        }
        link_all({1, 2, 3});
        for (std::uint64_t id : {1u, 2u, 3u}) {
            node(id).set_cluster_configuration({1, 2, 3});
            node(id).start();
        }
        // Only node 1 is ever ticked for elections, so it is the leader. A
        // campaign can lose a vote to scheduling; it then campaigns again.
        BOOST_REQUIRE(wait_until(
            [&] {
                std::this_thread::sleep_for(_cfg._election_timeout_max + 20ms);
                node(1).check_election_timeout();
                return wait_until([&] { return node(1).is_leader(); }, 500ms);
            },
            8000ms));
    }

    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;

    ~cluster() {
        for (auto& [id, n] : std::views::reverse(_nodes)) {
            n->stop();
        }
        _sim.stop();
    }

    /// Creates node 4, a fresh server that knows only itself, and links it to
    /// the cluster unless `reachable` is false.
    auto spare(bool reachable = true) -> test_node& {
        auto& n = create(4);
        n.set_cluster_configuration({4});
        n.start();
        if (reachable) {
            link_all({1, 2, 3, 4});
        }
        return n;
    }

    auto node(std::uint64_t id) -> test_node& { return *_nodes.at(id); }
    auto leader() -> test_node& { return node(1); }
    auto logs(std::uint64_t id) -> log_sink& { return *_logs.at(id); }
    auto metrics(std::uint64_t id) -> metric_sink& { return *_metrics.at(id); }
    [[nodiscard]] auto config() const -> const kythira::raft_configuration& { return _cfg; }

    auto link_all(std::initializer_list<std::uint64_t> ids) -> void {
        for (auto from : ids) {
            for (auto to : ids) {
                if (from != to) {
                    _sim.add_edge(std::to_string(from), std::to_string(to),
                                  network_simulator::NetworkEdge{});
                }
            }
        }
    }

    /// Stops every message to `to`; its replies still arrive. Every inbound
    /// edge goes, not just the leader's: the simulator routes over multiple
    /// hops, so 1 -> 2 -> 3 would still reach node 3.
    auto isolate(std::uint64_t to) -> void {
        for (const auto& [id, n] : _nodes) {
            if (id != to) {
                _sim.remove_edge(std::to_string(id), std::to_string(to));
            }
        }
    }
    auto reconnect(std::uint64_t to) -> void {
        for (const auto& [id, n] : _nodes) {
            if (id != to) {
                _sim.add_edge(std::to_string(id), std::to_string(to),
                              network_simulator::NetworkEdge{});
            }
        }
    }

    /// A raw client on a simulator node of its own, for hand-made RPCs.
    auto raw_client() -> test_types::network_client_type {
        auto net = _sim.create_node("9");
        for (std::uint64_t id : {1u, 2u, 3u}) {
            _sim.add_edge("9", std::to_string(id), network_simulator::NetworkEdge{});
            _sim.add_edge(std::to_string(id), "9", network_simulator::NetworkEdge{});
        }
        return test_types::network_client_type{net, test_types::serializer_type{}};
    }

    /// Ticks the leader until `pred` holds or `deadline` elapses.
    template<typename Pred> auto pump(Pred pred, std::chrono::milliseconds deadline = 5000ms) {
        const auto start = std::chrono::steady_clock::now();
        while (!pred()) {
            if (std::chrono::steady_clock::now() - start > deadline) {
                return false;
            }
            leader().check_heartbeat_timeout();
            std::this_thread::sleep_for(20ms);
        }
        return true;
    }

    /// Ticks the leader for `span` regardless.
    auto pump_for(std::chrono::milliseconds span) -> void {
        const auto until = std::chrono::steady_clock::now() + span;
        while (std::chrono::steady_clock::now() < until) {
            leader().check_heartbeat_timeout();
            std::this_thread::sleep_for(20ms);
        }
    }

    auto submit(std::string key, outcome& out) -> void {
        watch(leader().submit_command(sm_t::make_put_command(key, "v"), 3000ms), out);
    }

    /**
     * Starts add_server(4) and holds the change in its joint phase.
     *
     * The promotion begins at the start of the first tick after the leader
     * has both applied the learner entry and heard that node 4 holds it, and
     * that same tick sends the joint entry. So this ticks until node 4's log
     * reaches the learner entry, lets node 4's reply land, and isolates
     * nodes 3 and 4 before ticking again. The joint entry
     * then reaches node 2 only. Returns the joint entry's index.
     */
    auto hold_in_joint_phase(outcome& added) -> std::uint64_t {
        spare();
        watch(leader().add_server(4), added);
        const auto learner_index = leader().debug_state().log.back().index();
        BOOST_REQUIRE(pump([&] {
            return node(4).debug_state().log.size() >= learner_index &&
                   leader().debug_state().last_applied >= learner_index;
        }));
        std::this_thread::sleep_for(100ms);
        BOOST_REQUIRE(!leader().current_membership().joint);
        isolate(3);
        isolate(4);
        BOOST_REQUIRE(pump([&] { return leader().current_membership().joint; }));
        const auto entries = configuration_entries(leader());
        BOOST_REQUIRE(!entries.empty());
        BOOST_REQUIRE(entries.back().second.is_joint_consensus());
        return entries.back().first;
    }

    auto release_joint_phase() -> void {
        reconnect(3);
        reconnect(4);
    }

private:
    auto create(std::uint64_t id) -> test_node& {
        auto log = std::make_shared<log_sink>();
        auto met = std::make_shared<metric_sink>();
        auto net = _sim.create_node(std::to_string(id));
        auto n = std::make_unique<test_node>(kythira::node_config<test_types>{
            .node_id = id,
            .network_client = {net, test_types::serializer_type{}},
            .network_server = {net, test_types::serializer_type{}},
            .persistence = {},
            .logger = recording_logger{log},
            .metrics = recording_metrics{met},
            .membership = {},
            .config = _cfg,
            .self_address = std::to_string(id),
            .peer_discovery = preset_peer_discovery<std::uint64_t, std::string>{},
        });
        _logs[id] = log;
        _metrics[id] = met;
        auto& ref = *n;
        _nodes[id] = std::move(n);
        return ref;
    }

    kythira::raft_configuration _cfg;
    sim_t _sim;
    std::map<std::uint64_t, std::unique_ptr<test_node>> _nodes;
    std::map<std::uint64_t, std::shared_ptr<log_sink>> _logs;
    std::map<std::uint64_t, std::shared_ptr<metric_sink>> _metrics;
};

/// Deposes node 1 with a RequestVote that names voter 2 at a higher term.
inline auto depose_leader(cluster& c) -> void {
    auto client = c.raw_client();
    const auto term = c.leader().get_current_term();
    kythira::request_vote_request<> rv{._term = term + 5,
                                       ._candidate_id = 2,
                                       ._last_log_index = c.leader().debug_state().log.size() + 10,
                                       ._last_log_term = term + 5};
    auto response = client.send_request_vote(1, rv, 1000ms).get();
    BOOST_REQUIRE(response.vote_granted());
    BOOST_REQUIRE(!c.leader().is_leader());
}

}  // namespace kythira::test::membership
