// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * Property 92: Complete Add Server Implementation
 *
 * add_server() admits the new server as a learner, promotes it with joint
 * consensus once it has caught up (C_old -> C_old,new -> C_new), validates
 * its inputs, and settles the returned future on every outcome. Every case
 * below drives real nodes over the network simulator.
 *
 * Several cases hold the joint phase open on purpose: once the new server
 * has caught up, node 3 and the new server stop receiving messages, so the
 * joint entry reaches a majority of C_old ({1, 2} of {1, 2, 3}) but
 * not of C_new ({1, 2} of {1, 2, 3, 4}). Raft must not commit it, and the
 * test can look at the cluster mid-change before reconnecting.
 *
 * Validates: Requirements 9.2, 9.3, 9.4, 13.7, 17.1, 17.3, 18.2, 23.2, 23.4,
 * 24.3, 25.1, 29.1, 29.2, 29.3
 */

#define BOOST_TEST_MODULE raft_add_server_implementation_property_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/config_entry.hpp>
#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

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

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("raft_add_server_implementation_property_test"),
                         nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

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

auto make_fast_config() -> kythira::raft_configuration {
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

auto contains(const std::vector<std::uint64_t>& ids, std::uint64_t id) -> bool {
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

auto watch(test_node::future_type future, outcome& out) -> void {
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

auto mentions(const std::string& text, std::string_view needle) -> bool {
    return text.find(needle) != std::string::npos;
}

/// The configuration entries in a log, decoded, with their indices.
auto configuration_entries(const test_node& n) -> std::vector<std::pair<std::uint64_t, config_t>> {
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
auto depose_leader(cluster& c) -> void {
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

}  // namespace

BOOST_AUTO_TEST_SUITE(add_server_implementation)

BOOST_AUTO_TEST_CASE(property_add_server_requires_leadership, *boost::unit_test::timeout(60)) {
    // Requirement 17.3: only the leader can start a configuration change.
    BOOST_TEST_MESSAGE("Property 92.1: Add server requires leadership");
    cluster c;
    c.spare();

    for (std::uint64_t follower : {2u, 3u}) {
        const auto log_size = c.node(follower).debug_state().log.size();
        outcome out;
        watch(c.node(follower).add_server(4), out);
        BOOST_REQUIRE(wait_until([&] { return out.done(); }));
        BOOST_CHECK(out.failed);
        BOOST_CHECK(mentions(out.message(), "Not leader"));
        // Refused outright: nothing was appended and node 4 is nowhere.
        BOOST_CHECK_EQUAL(c.node(follower).debug_state().log.size(), log_size);
        const auto view = c.node(follower).current_membership();
        BOOST_CHECK(!contains(view.voters, 4));
        BOOST_CHECK(!contains(view.learners, 4));
    }

    // The leader accepts the same request.
    outcome out;
    watch(c.leader().add_server(4), out);
    BOOST_REQUIRE(c.pump([&] { return out.done(); }, 10000ms));
    BOOST_CHECK(out.resolved);
}

BOOST_AUTO_TEST_CASE(property_add_server_validates_duplicate_nodes,
                     *boost::unit_test::timeout(60)) {
    // Requirement 23.4: adding a server that is already a member is refused.
    BOOST_TEST_MESSAGE("Property 92.2: Add server validates duplicate nodes");
    cluster c;
    const auto log_size = c.leader().debug_state().log.size();

    for (std::uint64_t existing : {1u, 2u, 3u}) {
        outcome out;
        watch(c.leader().add_server(existing), out);
        BOOST_REQUIRE(wait_until([&] { return out.done(); }));
        BOOST_CHECK(out.failed);
        BOOST_CHECK(mentions(out.message(), "already in configuration"));
    }
    BOOST_CHECK_EQUAL(c.leader().debug_state().log.size(), log_size);
    BOOST_CHECK_EQUAL(c.leader().get_cluster_size(), 3u);

    // A server added once is a duplicate the second time.
    c.spare();
    outcome first;
    watch(c.leader().add_server(4), first);
    BOOST_REQUIRE(c.pump([&] { return first.done(); }, 10000ms));
    BOOST_REQUIRE(first.resolved);
    outcome again;
    watch(c.leader().add_server(4), again);
    BOOST_REQUIRE(wait_until([&] { return again.done(); }));
    BOOST_CHECK(again.failed);
    BOOST_CHECK(mentions(again.message(), "already in configuration"));
}

BOOST_AUTO_TEST_CASE(property_add_server_prevents_concurrent_changes,
                     *boost::unit_test::timeout(60)) {
    // Requirement 17.3: one configuration change at a time, whether the first
    // is still catching its learner up or already in joint consensus.
    BOOST_TEST_MESSAGE("Property 92.3: Add server prevents concurrent changes");
    cluster c;
    c.spare(/*reachable=*/false);

    outcome pending;
    watch(c.leader().add_server(4), pending);

    auto expect_refused = [&](test_node::future_type f) {
        outcome out;
        watch(std::move(f), out);
        BOOST_REQUIRE(wait_until([&] { return out.done(); }));
        BOOST_CHECK(out.failed);
        BOOST_CHECK(mentions(out.message(), "already in progress"));
    };

    // Catch-up phase.
    BOOST_CHECK(!pending.done());
    expect_refused(c.leader().add_server(5));
    expect_refused(c.leader().remove_server(3));
    expect_refused(c.leader().promote_to_voter(4));
    const auto view = c.leader().current_membership();
    BOOST_CHECK(!contains(view.learners, 5));
    BOOST_CHECK(contains(view.voters, 3));

    // Once the first change has settled, the next one is accepted.
    c.link_all({1, 2, 3, 4});
    BOOST_REQUIRE(c.pump([&] { return pending.done(); }, 10000ms));
    BOOST_CHECK(pending.resolved);
    outcome next;
    watch(c.leader().remove_server(4), next);
    BOOST_REQUIRE(c.pump([&] { return next.done(); }, 10000ms));
    BOOST_CHECK(next.resolved);
}

BOOST_AUTO_TEST_CASE(property_add_server_enters_joint_consensus, *boost::unit_test::timeout(90)) {
    // Requirement 29.1: the promotion runs under C_old,new, which names both
    // voting sets.
    BOOST_TEST_MESSAGE("Property 92.4: Add server enters joint consensus");
    cluster c;
    outcome added;
    const auto joint_index = c.hold_in_joint_phase(added);

    const auto view = c.leader().current_membership();
    BOOST_CHECK(view.joint);
    BOOST_CHECK(contains(view.voters, 4));
    BOOST_CHECK(!contains(view.learners, 4));

    const auto entries = configuration_entries(c.leader());
    const auto& joint = entries.back().second;
    BOOST_CHECK_EQUAL(entries.back().first, joint_index);
    BOOST_REQUIRE(joint.old_nodes().has_value());
    BOOST_CHECK((joint.nodes() == std::vector<std::uint64_t>{1, 2, 3, 4}));
    BOOST_CHECK((*joint.old_nodes() == std::vector<std::uint64_t>{1, 2, 3}));

    // The follower that received it holds the same entry.
    BOOST_CHECK(c.pump([&] {
        const auto on_2 = configuration_entries(c.node(2));
        return !on_2.empty() && on_2.back().first == joint_index &&
               on_2.back().second.is_joint_consensus();
    }));

    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_CHECK(added.resolved);
}

BOOST_AUTO_TEST_CASE(property_add_server_waits_for_joint_consensus_commit,
                     *boost::unit_test::timeout(90)) {
    // Requirement 29.2: C_new is appended only after C_old,new commits.
    BOOST_TEST_MESSAGE("Property 92.5: Add server waits for joint consensus commit");
    cluster c;
    outcome added;
    const auto joint_index = c.hold_in_joint_phase(added);

    c.pump_for(600ms);
    BOOST_CHECK_LT(c.leader().debug_state().commit_index, joint_index);
    // Nothing after the joint entry: C_new waits for it to commit.
    BOOST_CHECK_EQUAL(c.leader().debug_state().log.back().index(), joint_index);
    BOOST_CHECK(!added.done());

    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_CHECK(added.resolved);
    const auto entries = configuration_entries(c.leader());
    BOOST_REQUIRE_GE(entries.size(), 2u);
    BOOST_CHECK_GT(entries.back().first, joint_index);
    BOOST_CHECK(!entries.back().second.is_joint_consensus());
}

BOOST_AUTO_TEST_CASE(property_add_server_commits_final_configuration,
                     *boost::unit_test::timeout(90)) {
    // Requirement 29.3: once C_old,new commits, C_new is committed and the
    // caller's future resolves only after that.
    BOOST_TEST_MESSAGE("Property 92.6: Add server commits final configuration");
    cluster c;
    outcome added;
    const auto joint_index = c.hold_in_joint_phase(added);
    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_REQUIRE(added.resolved);

    const auto entries = configuration_entries(c.leader());
    const auto& [final_index, final_config] = entries.back();
    BOOST_CHECK_GT(final_index, joint_index);
    BOOST_CHECK(!final_config.is_joint_consensus());
    BOOST_CHECK((final_config.nodes() == std::vector<std::uint64_t>{1, 2, 3, 4}));
    BOOST_CHECK(!contains(final_config.learners(), 4));
    BOOST_CHECK_GE(c.leader().debug_state().commit_index, final_index);
    BOOST_CHECK_EQUAL(c.leader().get_cluster_size(), 4u);

    // Every replica, the new one included, ends on C_new.
    for (std::uint64_t id : {1u, 2u, 3u, 4u}) {
        BOOST_CHECK(c.pump([&] {
            const auto v = c.node(id).current_membership();
            return !v.joint && v.voters.size() == 4 && contains(v.voters, 4);
        }));
    }
}

BOOST_AUTO_TEST_CASE(property_add_server_requires_majority_in_both_configs,
                     *boost::unit_test::timeout(90)) {
    // Requirement 9.3: under C_old,new an entry commits only with a majority of
    // each set. {1, 2} is a majority of C_old but not of C_new, so neither the
    // joint entry nor a client command behind it may commit. (The converse,
    // a C_new majority without a C_old one, cannot arise when one server is
    // added.)
    BOOST_TEST_MESSAGE("Property 92.7: Add server requires majority in both configs");
    cluster c;
    outcome added;
    const auto joint_index = c.hold_in_joint_phase(added);

    outcome cmd;
    c.submit("during-joint", cmd);
    c.pump_for(600ms);
    // Node 2 holds both entries: C_old's majority alone is not enough.
    BOOST_CHECK(wait_until([&] { return c.node(2).debug_state().log.size() > joint_index; }));
    BOOST_CHECK_LT(c.leader().debug_state().commit_index, joint_index);
    BOOST_CHECK(!cmd.done());
    BOOST_CHECK(!added.done());

    // With node 4 back, {1, 2, 4} is a majority of C_new as well.
    c.reconnect(4);
    BOOST_REQUIRE(c.pump([&] { return cmd.done() && added.done(); }, 10000ms));
    BOOST_CHECK(cmd.resolved);
    BOOST_CHECK(added.resolved);
    c.reconnect(3);
}

BOOST_AUTO_TEST_CASE(property_add_server_uses_joint_consensus_protocol,
                     *boost::unit_test::timeout(90)) {
    // Requirement 9.2: the log records the whole change, in order: the learner
    // entry, C_old,new, then C_new.
    BOOST_TEST_MESSAGE("Property 92.8: Add server uses joint consensus protocol");
    cluster c;
    const auto before = configuration_entries(c.leader()).size();
    c.spare();
    outcome added;
    watch(c.leader().add_server(4), added);
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_REQUIRE(added.resolved);

    auto entries = configuration_entries(c.leader());
    BOOST_REQUIRE_EQUAL(entries.size(), before + 3);
    entries.erase(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(before));
    const auto& learner = entries[0].second;
    const auto& joint = entries[1].second;
    const auto& final_config = entries[2].second;

    BOOST_CHECK(!learner.is_joint_consensus());
    BOOST_CHECK((learner.nodes() == std::vector<std::uint64_t>{1, 2, 3}));
    BOOST_CHECK(contains(learner.learners(), 4));

    BOOST_CHECK(joint.is_joint_consensus());
    BOOST_CHECK((joint.nodes() == std::vector<std::uint64_t>{1, 2, 3, 4}));
    BOOST_REQUIRE(joint.old_nodes().has_value());
    BOOST_CHECK((*joint.old_nodes() == std::vector<std::uint64_t>{1, 2, 3}));

    BOOST_CHECK(!final_config.is_joint_consensus());
    BOOST_CHECK((final_config.nodes() == std::vector<std::uint64_t>{1, 2, 3, 4}));
    BOOST_CHECK(!contains(final_config.learners(), 4));

    BOOST_CHECK_LT(entries[0].first, entries[1].first);
    BOOST_CHECK_LT(entries[1].first, entries[2].first);
}

BOOST_AUTO_TEST_CASE(property_add_server_adds_as_non_voting_initially,
                     *boost::unit_test::timeout(120)) {
    // Requirement 9.4: until it has caught up the new server is a learner,
    // outside the quorum. With node 3 cut off as well, {1, 2} still commits:
    // a majority of three voters, which it would not be if node 4 counted.
    BOOST_TEST_MESSAGE("Property 92.9: Add server adds as non-voting initially");
    cluster c;
    c.spare(/*reachable=*/false);
    outcome added;
    watch(c.leader().add_server(4), added);

    auto view = c.leader().current_membership();
    BOOST_CHECK(contains(view.learners, 4));
    BOOST_CHECK(!contains(view.voters, 4));
    BOOST_CHECK(!view.joint);
    BOOST_CHECK_EQUAL(c.leader().get_cluster_size(), 3u);

    c.isolate(3);
    outcome cmd;
    c.submit("without-the-learner", cmd);
    BOOST_REQUIRE(c.pump([&] { return cmd.done(); }));
    BOOST_CHECK(cmd.resolved);
    BOOST_CHECK(wait_until([&] { return contains(c.node(2).current_membership().learners, 4); }));
    BOOST_CHECK(!added.done());
    c.reconnect(3);

    // Reachable now: it catches up, then votes.
    c.link_all({1, 2, 3, 4});
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_CHECK(added.resolved);
    view = c.leader().current_membership();
    BOOST_CHECK(contains(view.voters, 4));
    BOOST_CHECK(!contains(view.learners, 4));
}

BOOST_AUTO_TEST_CASE(property_add_server_uses_configurable_retry_policy,
                     *boost::unit_test::timeout(90)) {
    // Requirement 23.2: how long add_server() keeps retrying a server that
    // makes no progress is ten of the configured append_entries_timeout, not
    // a hard-coded constant.
    BOOST_TEST_MESSAGE("Property 92.10: Add server uses configurable retry policy");
    for (auto aet : {100ms, 300ms}) {
        auto cfg = make_fast_config();
        cfg._append_entries_timeout = aet;
        cluster c{cfg};
        c.spare(/*reachable=*/false);

        outcome added;
        const auto start = std::chrono::steady_clock::now();
        watch(c.leader().add_server(4), added);
        BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
        const auto took = std::chrono::steady_clock::now() - start;

        BOOST_CHECK(added.failed);
        BOOST_CHECK(mentions(added.message(), "no replication progress"));
        BOOST_CHECK(took >= aet * 10);
        // Generous upper bound: the budget plus scheduling slack.
        BOOST_CHECK(took < aet * 10 + 2000ms);
    }
}

BOOST_AUTO_TEST_CASE(property_add_server_validates_configuration, *boost::unit_test::timeout(60)) {
    // Requirement 23.4: each refusal says why, and is logged as a warning.
    BOOST_TEST_MESSAGE("Property 92.11: Add server validates configuration");
    cluster c;

    outcome not_leader;
    watch(c.node(2).add_server(4), not_leader);
    outcome duplicate;
    watch(c.leader().add_server(2), duplicate);
    c.spare(/*reachable=*/false);
    outcome first;
    watch(c.leader().add_server(4), first);
    outcome busy;
    watch(c.leader().add_server(5), busy);

    BOOST_REQUIRE(wait_until([&] { return not_leader.done() && duplicate.done() && busy.done(); }));
    BOOST_CHECK(not_leader.failed);
    BOOST_CHECK_EQUAL(not_leader.message(), "Not leader - cannot add server");
    BOOST_CHECK(duplicate.failed);
    BOOST_CHECK_EQUAL(duplicate.message(), "Node already in configuration");
    BOOST_CHECK(busy.failed);
    BOOST_CHECK_EQUAL(busy.message(), "Configuration change already in progress");

    BOOST_CHECK(c.logs(2).level_of("Cannot add server: not leader") == kythira::log_level::warning);
    BOOST_CHECK(c.logs(1).level_of("Cannot add server: already in configuration") ==
                kythira::log_level::warning);
    BOOST_CHECK(c.logs(1).level_of("Cannot add server: configuration change already in progress") ==
                kythira::log_level::warning);
    // The warnings name the server that was refused.
    const auto dup_nodes =
        c.logs(1).field_values("Cannot add server: already in configuration", "new_node");
    BOOST_CHECK((dup_nodes == std::vector<std::string>{"2"}));
}

BOOST_AUTO_TEST_CASE(property_add_server_uses_two_phase_protocol, *boost::unit_test::timeout(90)) {
    // Requirement 17.1: the phases are observable in order on a follower too,
    // learner first, then joint, then final, and the caller hears of success
    // only after the last.
    BOOST_TEST_MESSAGE("Property 92.12: Add server uses two-phase protocol");
    cluster c;
    outcome added;
    const auto joint_index = c.hold_in_joint_phase(added);
    // Phase one is under way on node 2: it holds the joint entry, and C_new
    // does not exist anywhere yet.
    BOOST_REQUIRE(c.pump([&] { return c.node(2).debug_state().log.size() >= joint_index; }));
    BOOST_CHECK_EQUAL(c.leader().debug_state().log.back().index(), joint_index);
    BOOST_CHECK(!added.done());

    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_CHECK(added.resolved);
    const auto final_index = c.leader().debug_state().commit_index;
    BOOST_REQUIRE(c.pump([&] { return c.node(2).debug_state().last_applied >= final_index; }));
    const auto v = c.node(2).current_membership();
    BOOST_CHECK(!v.joint);
    BOOST_CHECK(contains(v.voters, 4));

    // Node 2 applied the three entries in order.
    const auto applied = c.logs(2).field_values("Applied configuration entry", "is_joint");
    BOOST_REQUIRE_GE(applied.size(), 3u);
    BOOST_CHECK_EQUAL(applied[applied.size() - 3], "false");
    BOOST_CHECK_EQUAL(applied[applied.size() - 2], "true");
    BOOST_CHECK_EQUAL(applied[applied.size() - 1], "false");
}

BOOST_AUTO_TEST_CASE(property_add_server_handles_leadership_loss, *boost::unit_test::timeout(90)) {
    // Requirement 17.3: a leader that is deposed while its new server is
    // catching up fails the caller on its next tick rather than leaving the
    // future pending, and the server never becomes a voter.
    BOOST_TEST_MESSAGE("Property 92.13: Add server handles leadership loss");
    cluster c;
    c.spare(/*reachable=*/false);
    outcome added;
    watch(c.leader().add_server(4), added);
    c.pump_for(100ms);
    BOOST_REQUIRE(!added.done());

    depose_leader(c);
    c.leader().check_heartbeat_timeout();
    BOOST_REQUIRE(wait_until([&] { return added.done(); }, 2000ms));
    BOOST_CHECK(added.failed);
    BOOST_CHECK(mentions(added.message(), "leadership lost"));
    BOOST_CHECK(!contains(c.leader().current_membership().voters, 4));
    BOOST_CHECK_EQUAL(c.metrics(1).count("raft_add_server_failed"), 1u);
}

BOOST_AUTO_TEST_CASE(property_add_server_emits_metrics, *boost::unit_test::timeout(60)) {
    // Requirement 13.7: started, then success or failed, each naming the
    // server; the promotion's own pair in between.
    BOOST_TEST_MESSAGE("Property 92.14: Add server emits metrics");
    cluster c;
    auto& m = c.metrics(1);

    c.spare();
    outcome added;
    watch(c.leader().add_server(4), added);
    BOOST_CHECK_EQUAL(m.count("raft_add_server_started"), 1u);
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_REQUIRE(added.resolved);
    BOOST_CHECK_EQUAL(m.count("raft_promote_to_voter_started"), 1u);
    BOOST_CHECK_EQUAL(m.count("raft_promote_to_voter_success"), 1u);
    BOOST_CHECK_EQUAL(m.count("raft_add_server_success"), 1u);
    BOOST_CHECK_EQUAL(m.count("raft_add_server_failed"), 0u);
    BOOST_CHECK((m.dimension_values("raft_add_server_started", "new_node") ==
                 std::vector<std::string>{"4"}));
    BOOST_CHECK((m.dimension_values("raft_add_server_success", "new_node") ==
                 std::vector<std::string>{"4"}));
    BOOST_CHECK((m.dimension_values("raft_add_server_success", "node_id") ==
                 std::vector<std::string>{"1"}));

    // A server that never answers: started, then failed.
    outcome lost;
    watch(c.leader().add_server(5), lost);
    BOOST_REQUIRE(c.pump([&] { return lost.done(); }, 10000ms));
    BOOST_CHECK(lost.failed);
    BOOST_CHECK_EQUAL(m.count("raft_add_server_started"), 2u);
    BOOST_CHECK((m.dimension_values("raft_add_server_failed", "new_node") ==
                 std::vector<std::string>{"5"}));
    BOOST_CHECK_EQUAL(m.count("raft_add_server_success"), 1u);
}

BOOST_AUTO_TEST_CASE(property_add_server_logs_progress, *boost::unit_test::timeout(60)) {
    // Requirement 24.3: each step is logged, and a failure says why.
    BOOST_TEST_MESSAGE("Property 92.15: Add server logs progress");
    cluster c;
    auto& log = c.logs(1);

    c.spare();
    outcome added;
    watch(c.leader().add_server(4), added);
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_REQUIRE(added.resolved);
    for (const auto* step :
         {"Add server requested", "Starting server addition: catching up as a learner first",
          "New server caught up; promoting it with joint consensus",
          "Starting learner promotion with joint consensus",
          "Appended C_new entry after joint commit", "Add server completed successfully"}) {
        BOOST_CHECK_MESSAGE(log.count(step) == 1, "expected one \"" << step << "\"");
    }
    BOOST_CHECK((log.field_values("Add server completed successfully", "new_node") ==
                 std::vector<std::string>{"4"}));

    outcome lost;
    watch(c.leader().add_server(5), lost);
    BOOST_REQUIRE(c.pump([&] { return lost.done(); }, 10000ms));
    BOOST_CHECK(lost.failed);
    BOOST_CHECK(log.level_of("Add server abandoned") == kythira::log_level::warning);
    const auto reasons = log.field_values("Add server abandoned", "reason");
    BOOST_REQUIRE_EQUAL(reasons.size(), 1u);
    BOOST_CHECK(mentions(reasons[0], "no replication progress"));
}

BOOST_AUTO_TEST_CASE(property_add_server_handles_replication_failures,
                     *boost::unit_test::timeout(90)) {
    // Requirement 18.2: replication to the new server is retried while it is
    // unreachable; an outage shorter than the stall budget only delays the
    // change.
    BOOST_TEST_MESSAGE("Property 92.16: Add server handles replication failures");
    cluster c;  // stall budget: 10 x 200 ms
    c.spare(/*reachable=*/false);
    outcome added;
    watch(c.leader().add_server(4), added);

    c.pump_for(800ms);
    BOOST_CHECK(!added.done());
    BOOST_CHECK_EQUAL(c.node(4).debug_state().log.size(), 0u);

    c.link_all({1, 2, 3, 4});
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_CHECK(added.resolved);
    BOOST_CHECK(contains(c.leader().current_membership().voters, 4));
    BOOST_CHECK(c.pump([&] {
        return c.node(4).debug_state().last_applied >= c.leader().debug_state().last_applied;
    }));
}

BOOST_AUTO_TEST_CASE(property_add_server_synchronizes_with_config_synchronizer,
                     *boost::unit_test::timeout(90)) {
    // Requirement 17.1: while the synchronizer holds the joint phase, every
    // other membership change is refused; once C_new commits it lets go.
    BOOST_TEST_MESSAGE("Property 92.17: Add server synchronizes with ConfigurationSynchronizer");
    cluster c;
    outcome added;
    c.hold_in_joint_phase(added);

    auto expect_refused = [&](test_node::future_type f) {
        outcome out;
        watch(std::move(f), out);
        BOOST_REQUIRE(wait_until([&] { return out.done(); }));
        BOOST_CHECK(out.failed);
        BOOST_CHECK_EQUAL(out.message(), "Configuration change already in progress");
    };
    expect_refused(c.leader().add_server(5));
    expect_refused(c.leader().remove_server(2));
    expect_refused(c.leader().promote_to_voter(4));

    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_REQUIRE(added.resolved);

    outcome removed;
    watch(c.leader().remove_server(4), removed);
    BOOST_REQUIRE(c.pump([&] { return removed.done(); }, 10000ms));
    BOOST_CHECK(removed.resolved);
    BOOST_CHECK(!contains(c.leader().current_membership().voters, 4));
}

BOOST_AUTO_TEST_CASE(property_add_server_returns_future_on_completion,
                     *boost::unit_test::timeout(90)) {
    // Requirement 25.1: the future is pending until the change completes,
    // then holds an empty value; a failure arrives as its exception.
    BOOST_TEST_MESSAGE("Property 92.18: Add server returns future on completion");
    cluster c;
    outcome added;
    c.hold_in_joint_phase(added);
    c.pump_for(200ms);
    BOOST_CHECK(!added.done());

    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_CHECK(added.resolved);
    BOOST_CHECK_EQUAL(added.value_size.load(), 0u);
    BOOST_CHECK_GE(c.leader().get_cluster_size(), 4u);

    outcome lost;
    watch(c.leader().add_server(5), lost);
    BOOST_REQUIRE(c.pump([&] { return lost.done(); }, 10000ms));
    BOOST_CHECK(lost.failed);
    BOOST_CHECK(!lost.message().empty());
}

BOOST_AUTO_TEST_CASE(property_add_server_handles_timeout, *boost::unit_test::timeout(90)) {
    // Requirement 23.2: a joint phase that cannot commit fails the caller once
    // the change's timeout (10 x append_entries_timeout) elapses. The joint
    // entry stays in the log, so new changes are still refused until it
    // commits; when it does, the leader completes the change anyway.
    BOOST_TEST_MESSAGE("Property 92.19: Add server handles timeout");
    cluster c;  // timeout: 10 x 200 ms
    outcome added;
    const auto start = std::chrono::steady_clock::now();
    c.hold_in_joint_phase(added);

    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 6000ms));
    BOOST_CHECK(added.failed);
    BOOST_CHECK(mentions(added.message(), "timed out"));
    BOOST_CHECK(std::chrono::steady_clock::now() - start >=
                c.config()._append_entries_timeout * 10);

    outcome busy;
    watch(c.leader().add_server(5), busy);
    BOOST_REQUIRE(wait_until([&] { return busy.done(); }));
    BOOST_CHECK(busy.failed);
    BOOST_CHECK(mentions(busy.message(), "in progress"));

    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] {
        const auto v = c.leader().current_membership();
        return !v.joint && contains(v.voters, 4);
    }));

    outcome next;
    watch(c.leader().remove_server(4), next);
    BOOST_REQUIRE(c.pump([&] { return next.done(); }, 10000ms));
    BOOST_CHECK(next.resolved);
}

BOOST_AUTO_TEST_CASE(property_add_server_maintains_safety_during_transition,
                     *boost::unit_test::timeout(120)) {
    // Requirement 9.3: across the whole change, with commands committed
    // before, during and after it, there is one leader and every replica
    // agrees on every committed entry.
    BOOST_TEST_MESSAGE("Property 92.20: Add server maintains safety during transition");
    cluster c;
    outcome before;
    c.submit("before", before);
    BOOST_REQUIRE(c.pump([&] { return before.done(); }));

    outcome added;
    c.hold_in_joint_phase(added);
    outcome during;
    c.submit("during", during);
    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] { return added.done() && during.done(); }, 10000ms));
    outcome after;
    c.submit("after", after);
    BOOST_REQUIRE(c.pump([&] { return after.done(); }));
    BOOST_CHECK(before.resolved);
    BOOST_CHECK(during.resolved);
    BOOST_CHECK(added.resolved);
    BOOST_CHECK(after.resolved);

    const auto commit = c.leader().debug_state().commit_index;
    BOOST_REQUIRE(c.pump([&] {
        for (std::uint64_t id : {2u, 3u, 4u}) {
            if (c.node(id).debug_state().last_applied < commit) {
                return false;
            }
        }
        return true;
    }));
    // Quiesce before reading the logs.
    std::this_thread::sleep_for(100ms);

    const auto leader_log = c.leader().debug_state().log;
    for (std::uint64_t id : {1u, 2u, 3u, 4u}) {
        BOOST_CHECK_EQUAL(c.node(id).is_leader(), id == 1);
        const auto log = c.node(id).debug_state().log;
        BOOST_REQUIRE_GE(log.size(), commit);
        for (std::size_t i = 0; i < commit; ++i) {
            BOOST_CHECK_EQUAL(log[i].index(), leader_log[i].index());
            BOOST_CHECK_EQUAL(log[i].term(), leader_log[i].term());
            BOOST_CHECK(log[i].type() == leader_log[i].type());
            BOOST_CHECK(log[i].command() == leader_log[i].command());
        }
        const auto v = c.node(id).current_membership();
        BOOST_CHECK(!v.joint);
        BOOST_CHECK((v.voters == std::vector<std::uint64_t>{1, 2, 3, 4}));
    }
}

BOOST_AUTO_TEST_SUITE_END()
