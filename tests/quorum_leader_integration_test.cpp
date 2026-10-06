// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Integration tests for the leader quorum-management loop (Req 16)
//
// Uses a mock_quorum_manager that records calls to assess_quorum /
// provision_node / decommission_node so that test assertions can verify
// the exact interactions the leader makes with the quorum manager without
// touching any real infrastructure.

#define BOOST_TEST_MODULE quorum_leader_integration_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/quorum_management.hpp>
#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>

#endif
#include "test_timeout_scale.hpp"
#include <network_simulator/network_simulator.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ── Folly global fixture ──────────────────────────────────────────────────────

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("quorum_leader_integration_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
#endif

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

// ── Mock quorum manager ───────────────────────────────────────────────────────

namespace {

using placement_vector = std::vector<kythira::node_placement<std::uint64_t, std::string>>;

// State shared by every copy of mock_quorum_manager.  The node takes its
// quorum manager by value, so the test keeps a shared_ptr to this to observe
// calls and steer results after the mock has been moved into node_config.
struct qm_state {
    struct assess_call {
        std::uint64_t caller;
        placement_vector cluster;
    };

    mutable std::mutex mu;
    std::vector<assess_call> assess_calls;
    std::vector<std::pair<std::string, std::optional<std::uint64_t>>> provisions;
    std::vector<std::uint64_t> decommissions;

    // Inputs.
    kythira::desired_topology<std::string> topo{};
    std::set<std::uint64_t> unreachable;
    std::optional<kythira::quorum_status> forced_status;
    bool assess_fails{false};
    bool decommission_fails{false};
    // Node IDs handed out by successive provision_node calls; empty → fail.
    std::deque<std::uint64_t> provision_ids;
    // Called (without the mock's lock) from inside decommission_node, on the
    // leader's tick thread, before the call returns.
    std::function<void(std::uint64_t)> on_decommission;
    // Called (without the mock's lock) from inside a successful
    // provision_node, before the call returns: lets a test hold the leader
    // mid-provision, as a slow cloud API would.
    std::function<void(std::uint64_t)> on_provision;

    [[nodiscard]] auto assess_count(std::uint64_t caller) const -> std::size_t {
        std::lock_guard lock(mu);
        return static_cast<std::size_t>(
            std::count_if(assess_calls.begin(), assess_calls.end(),
                          [&](const assess_call& c) { return c.caller == caller; }));
    }
    [[nodiscard]] auto last_assess(std::uint64_t caller) const -> std::optional<placement_vector> {
        std::lock_guard lock(mu);
        for (const auto& call : std::views::reverse(assess_calls)) {
            if (call.caller == caller) {
                return call.cluster;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] auto provision_count() const -> std::size_t {
        std::lock_guard lock(mu);
        return provisions.size();
    }
    [[nodiscard]] auto decommissioned() const -> std::vector<std::uint64_t> {
        std::lock_guard lock(mu);
        return decommissions;
    }
};

struct mock_quorum_manager {
    using node_id_type = std::uint64_t;
    using address_type = std::string;
    using placement_group_id_type = std::string;

    std::shared_ptr<qm_state> state = std::make_shared<qm_state>();
    std::uint64_t owner{0};  // ID of the node holding this copy

    // live_count per group is computed from the supplied cluster minus
    // `unreachable`, so the reported health follows real membership changes.
    auto assess_quorum(const placement_vector& cluster)
        -> kythira::future_default<kythira::quorum_health<node_id_type, placement_group_id_type>> {
        std::lock_guard lock(state->mu);
        state->assess_calls.push_back({owner, cluster});
        if (state->assess_fails) {
            return kythira::future_factory_default::makeExceptionalFuture<
                kythira::quorum_health<node_id_type, placement_group_id_type>>(
                std::make_exception_ptr(std::runtime_error("mock: assess failed")));
        }

        kythira::quorum_health<node_id_type, placement_group_id_type> health{};
        health.total_node_count = cluster.size();
        for (const auto& gt : state->topo.groups) {
            health.groups.push_back({.group_id = gt.group_id,
                                     .live_count = 0,
                                     .target_count = gt.target_count,
                                     .unreachable_nodes = {}});
        }
        for (const auto& np : cluster) {
            auto git = std::find_if(health.groups.begin(), health.groups.end(),
                                    [&](const auto& g) { return g.group_id == np.group_id; });
            if (git == health.groups.end()) {
                health.groups.push_back({.group_id = np.group_id,
                                         .live_count = 0,
                                         .target_count = 0,
                                         .unreachable_nodes = {}});
                git = std::prev(health.groups.end());
            }
            if (state->unreachable.contains(np.node_id)) {
                git->unreachable_nodes.push_back(np.node_id);
                health.unreachable_nodes.push_back(np.node_id);
            } else {
                ++git->live_count;
                ++health.live_node_count;
            }
        }

        const std::size_t majority = cluster.size() / 2 + 1;
        bool below_target =
            std::any_of(health.groups.begin(), health.groups.end(),
                        [](const auto& g) { return g.live_count < g.target_count; });
        if (health.live_node_count < majority) {
            health.status = kythira::quorum_status::lost;
        } else if (below_target) {
            health.status = health.live_node_count == majority ? kythira::quorum_status::critical
                                                               : kythira::quorum_status::degraded;
        } else {
            health.status = kythira::quorum_status::healthy;
        }
        if (state->forced_status) {
            health.status = *state->forced_status;
        }
        return kythira::future_factory_default::makeFuture(health);
    }

    auto provision_node(placement_group_id_type group, std::optional<node_id_type> replacing)
        -> kythira::future_default<kythira::peer_info<node_id_type, address_type>> {
        std::uint64_t id = 0;
        std::function<void(std::uint64_t)> hook;
        {
            std::lock_guard lock(state->mu);
            state->provisions.emplace_back(group, replacing);
            if (state->provision_ids.empty()) {
                return kythira::future_factory_default::makeExceptionalFuture<
                    kythira::peer_info<node_id_type, address_type>>(std::make_exception_ptr(
                    std::runtime_error("mock: provisioning not configured")));
            }
            id = state->provision_ids.front();
            state->provision_ids.pop_front();
            hook = state->on_provision;
        }
        if (hook) {
            hook(id);
        }
        return kythira::future_factory_default::makeFuture(
            kythira::peer_info<node_id_type, address_type>{id, std::to_string(id)});
    }

    auto decommission_node(const node_id_type& id) -> kythira::future_default<void> {
        std::function<void(std::uint64_t)> hook;
        bool fail = false;
        {
            std::lock_guard lock(state->mu);
            state->decommissions.push_back(id);
            hook = state->on_decommission;
            fail = state->decommission_fails;
        }
        if (hook) {
            hook(id);
        }
        if (fail) {
            return kythira::future_factory_default::makeExceptionalFuture<void>(
                std::make_exception_ptr(std::runtime_error("mock: decommission failed")));
        }
        return kythira::future_factory_default::makeFuture();
    }

    auto maintain_quorum(const placement_vector& cluster)
        -> kythira::future_default<kythira::quorum_health<node_id_type, placement_group_id_type>> {
        return assess_quorum(cluster);
    }

    [[nodiscard]] auto topology() const -> kythira::desired_topology<placement_group_id_type> {
        std::lock_guard lock(state->mu);
        return state->topo;
    }
};

static_assert(kythira::quorum_manager<mock_quorum_manager, std::uint64_t, std::string, std::string>,
              "mock_quorum_manager must satisfy quorum_manager");

// ── Recording network client ─────────────────────────────────────────────────

// Every update_peer_address() a node makes, as (peer, address).  The
// simulator routes by node id, so the binding itself changes nothing here;
// recording it is how a test sees which address a leader would replicate to.
struct rebind_log {
    mutable std::mutex mu;
    std::vector<std::pair<std::uint64_t, std::string>> calls;

    [[nodiscard]] auto for_peer(std::uint64_t id) const -> std::vector<std::string> {
        std::lock_guard lock(mu);
        std::vector<std::string> out;
        for (const auto& [peer, address] : calls) {
            if (peer == id) {
                out.push_back(address);
            }
        }
        return out;
    }
};

using sim_network_types = kythira::raft_simulator_network_types<std::string>;
using sim_serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;
using sim_client_base =
    kythira::simulator_network_client<sim_network_types, sim_serializer, std::vector<std::byte>>;

struct recording_network_client : sim_client_base {
    recording_network_client(sim_client_base::node_type node, sim_serializer serializer,
                             std::shared_ptr<rebind_log> log = nullptr)
        : sim_client_base(std::move(node), std::move(serializer)), _log(std::move(log)) {}

    auto update_peer_address(std::uint64_t id, const std::string& address) -> void {
        if (_log) {
            std::lock_guard lock(_log->mu);
            _log->calls.emplace_back(id, address);
        }
    }

    std::shared_ptr<rebind_log> _log;
};

// ── Test types bundle ─────────────────────────────────────────────────────────

struct test_raft_types_with_qm {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using raft_network_types = kythira::raft_simulator_network_types<std::string>;
    using network_client_type = recording_network_client;
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

    using quorum_manager_type = mock_quorum_manager;
};

using test_node_type = kythira::node<test_raft_types_with_qm>;
using sim_type = network_simulator::NetworkSimulator<test_raft_types_with_qm::raft_network_types>;

using kythira::testing::scaled_deadline;
using kythira::testing::scaled_timeout;

template<typename Pred>
auto wait_until(Pred pred, std::chrono::milliseconds deadline = scaled_deadline(5000)) -> bool {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return true;
}

// Fast protocol timings for every test; individual tests override the quorum
// knobs.  Election timeouts are long relative to the heartbeat so a follower
// never campaigns while the leader's tick is busy in a blocking quorum call.
auto fast_config() -> kythira::raft_configuration {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{150};
    cfg._election_timeout_max = std::chrono::milliseconds{300};
    cfg._heartbeat_interval = std::chrono::milliseconds{10};
    cfg._rpc_timeout = std::chrono::milliseconds{100};
    cfg._append_entries_timeout = std::chrono::milliseconds{200};
    cfg._heartbeat_retry_policy = {.initial_delay = std::chrono::milliseconds{10},
                                   .max_delay = std::chrono::milliseconds{20},
                                   .backoff_multiplier = 1.5,
                                   .jitter_factor = 0.1,
                                   .max_attempts = 2};
    cfg._quorum_check_interval = std::chrono::milliseconds{20};
    cfg._quorum_heartbeat_failure_threshold = 2;
    return cfg;
}

// A simulated cluster whose nodes all share one qm_state.  Node 1 is made
// leader deterministically by ticking only its election timer; afterwards a
// ticker thread per running node drives both timers, as an event loop would.
class test_cluster {
public:
    explicit test_cluster(kythira::raft_configuration cfg) : _cfg(cfg) { _sim.start(); }

    test_cluster(const test_cluster&) = delete;
    auto operator=(const test_cluster&) -> test_cluster& = delete;

    ~test_cluster() {
        stop_tickers();
        for (auto& [id, n] : _nodes) {
            n->stop();
        }
    }

    // Creates and starts node `id` with the given initial configuration;
    // every node is fully meshed with the live ones created before it.  A
    // killed node stays disconnected: the simulator routes over multiple
    // hops, so one edge to it would make it reachable from every node again.
    auto add_node(std::uint64_t id, std::vector<std::uint64_t> configuration,
                  std::unordered_map<std::uint64_t, std::string> placement = {})
        -> test_node_type& {
        auto net = _sim.create_node(std::to_string(id));
        for (const auto& [other, _] : _nodes) {
            if (_killed.contains(other)) {
                continue;
            }
            _sim.add_edge(std::to_string(id), std::to_string(other), {});
            _sim.add_edge(std::to_string(other), std::to_string(id), {});
        }
        kythira::node_config<test_raft_types_with_qm> ncfg{
            .node_id = id,
            .network_client = {net, test_raft_types_with_qm::serializer_type{}, rebinds},
            .network_server = {net, test_raft_types_with_qm::serializer_type{}},
            .persistence = {},
            .logger = kythira::console_logger{kythira::log_level::info},
            .metrics = {},
            .membership = {},
            .config = _cfg,
            .quorum_manager = mock_quorum_manager{.state = state, .owner = id},
            .initial_placement = std::move(placement),
        };
        auto n = std::make_unique<test_node_type>(std::move(ncfg));
        n->set_cluster_configuration(configuration);
        n->start();
        auto& ref = *n;
        _nodes.emplace(id, std::move(n));
        return ref;
    }

    auto node(std::uint64_t id) -> test_node_type& { return *_nodes.at(id); }

    // Elect node 1, then start a ticker for every running node.
    auto elect_node1_and_run() -> bool {
        auto& leader = node(1);
        auto deadline = std::chrono::steady_clock::now() + scaled_deadline(5000);
        while (!leader.is_leader()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(_cfg._election_timeout_max);
            leader.check_election_timeout();
            wait_until([&] { return leader.is_leader(); }, std::chrono::milliseconds{200});
        }
        for (auto& [id, n] : _nodes) {
            start_ticker(id);
        }
        return true;
    }

    // Ticks a node created after elect_node1_and_run().
    auto start_ticker(std::uint64_t id) -> void {
        auto* n = _nodes.at(id).get();
        auto stop = std::make_shared<std::atomic<bool>>(false);
        _ticker_stops[id] = stop;
        _tickers.emplace_back([n, stop] {
            while (!stop->load()) {
                n->check_election_timeout();
                n->check_heartbeat_timeout();
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        });
    }

    // Permanent failure: the node stops, its ticker stops, the network drops
    // every message to and from it, and the mock reports it unreachable.
    auto kill(std::uint64_t id) -> void {
        kill_process(id);
        std::lock_guard lock(state->mu);
        state->unreachable.insert(id);
    }

    // The kythira process dies but its VM keeps running: everything kill()
    // does except that the mock, like a cloud quorum manager reading
    // instance state, still reports the node live.
    auto kill_process(std::uint64_t id) -> void {
        if (auto it = _ticker_stops.find(id); it != _ticker_stops.end()) {
            it->second->store(true);
        }
        for (const auto& [other, _] : _nodes) {
            if (other != id) {
                _sim.remove_edge(std::to_string(id), std::to_string(other));
                _sim.remove_edge(std::to_string(other), std::to_string(id));
            }
        }
        _killed.insert(id);
        _nodes.at(id)->stop();
    }

    // Sends a ClusterJoin for `id` (reachable at its own simulator address)
    // to `target`, as a provisioned node's bootstrap does, from a dedicated
    // client endpoint so it never consumes a node's own responses.
    // `address` overrides the advertised contact address.
    auto send_join(std::uint64_t id, std::uint64_t target,
                   std::optional<std::string> address = std::nullopt) -> bool {
        if (!_join_client) {
            auto net = _sim.create_node("join-client");
            for (const auto& [other, _] : _nodes) {
                if (_killed.contains(other)) {
                    continue;
                }
                _sim.add_edge("join-client", std::to_string(other), {});
                _sim.add_edge(std::to_string(other), "join-client", {});
            }
            _join_client.emplace(net, test_raft_types_with_qm::serializer_type{});
        }
        kythira::cluster_join_request<> req;
        req.node_id = id;
        req.contact_address = address.value_or(std::to_string(id));
        return _join_client
            ->send_cluster_join_request(std::to_string(target), req, _cfg._rpc_timeout * 5)
            .get()
            .is_accepted();
    }

    std::shared_ptr<qm_state> state = std::make_shared<qm_state>();
    std::shared_ptr<rebind_log> rebinds = std::make_shared<rebind_log>();

private:
    auto stop_tickers() -> void {
        for (auto& [id, stop] : _ticker_stops) {
            stop->store(true);
        }
        for (auto& t : _tickers) {
            t.join();
        }
        _tickers.clear();
    }

    kythira::raft_configuration _cfg;
    sim_type _sim;
    std::map<std::uint64_t, std::unique_ptr<test_node_type>> _nodes;
    std::map<std::uint64_t, std::shared_ptr<std::atomic<bool>>> _ticker_stops;
    std::set<std::uint64_t> _killed;
    std::vector<std::thread> _tickers;
    std::optional<test_raft_types_with_qm::network_client_type> _join_client;
};

auto group_of(const placement_vector& cluster, std::uint64_t id) -> std::optional<std::string> {
    for (const auto& np : cluster) {
        if (np.node_id == id) {
            return np.group_id;
        }
    }
    return std::nullopt;
}

}  // namespace

// ── Tests ─────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(quorum_leader_integration)

// Req 16.1 — become_leader() starts the assessment loop and stepping down
// stops it.  Leadership moves from node 1 to node 2; node 1's assessments
// stop and node 2's begin.
BOOST_AUTO_TEST_CASE(loop_runs_only_while_leader, *boost::unit_test::timeout(scaled_timeout(20))) {
    test_cluster c{fast_config()};
    c.add_node(1, {1, 2});
    c.add_node(2, {1, 2});
    BOOST_REQUIRE(c.elect_node1_and_run());

    BOOST_REQUIRE(wait_until([&] { return c.state->assess_count(1) >= 2; }));
    BOOST_CHECK_EQUAL(c.state->assess_count(2), 0u);

    c.node(1).transfer_leadership(2, scaled_deadline(3000)).detach();
    BOOST_REQUIRE(wait_until([&] { return c.node(2).is_leader() && !c.node(1).is_leader(); }));

    auto calls_after_step_down = c.state->assess_count(1);
    BOOST_REQUIRE(wait_until([&] { return c.state->assess_count(2) >= 3; }));
    BOOST_CHECK_EQUAL(c.state->assess_count(1), calls_after_step_down);
}

// Req 16.2 — after quorum_check_interval the leader calls assess_quorum with
// every configuration member paired with its placement group.
BOOST_AUTO_TEST_CASE(assess_vector_carries_every_member_and_group,
                     *boost::unit_test::timeout(scaled_timeout(20))) {
    test_cluster c{fast_config()};
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "az-a", .target_count = 1},
                                    {.group_id = "az-b", .target_count = 1},
                                    {.group_id = "az-c", .target_count = 1}}};
    }
    std::unordered_map<std::uint64_t, std::string> placement{
        {1, "az-a"}, {2, "az-b"}, {3, "az-c"}, {99, "az-z"}};
    c.add_node(1, {1, 2, 3}, placement);
    c.add_node(2, {1, 2, 3}, placement);
    c.add_node(3, {1, 2, 3}, placement);
    BOOST_REQUIRE(c.elect_node1_and_run());

    // More than one call: the second can only come from the interval timer.
    BOOST_REQUIRE(wait_until([&] { return c.state->assess_count(1) >= 2; }));
    auto cluster = c.state->last_assess(1);
    BOOST_REQUIRE(cluster.has_value());
    BOOST_CHECK_EQUAL(cluster->size(), 3u);
    BOOST_CHECK(group_of(*cluster, 1) == std::optional<std::string>{"az-a"});
    BOOST_CHECK(group_of(*cluster, 2) == std::optional<std::string>{"az-b"});
    BOOST_CHECK(group_of(*cluster, 3) == std::optional<std::string>{"az-c"});
    // Req 12.5 — a placement entry for a non-member is ignored.
    BOOST_CHECK(!group_of(*cluster, 99).has_value());
}

// Req 16.3 — reaching quorum_heartbeat_failure_threshold for one peer triggers
// an assessment immediately.  The interval is far longer than the test, so
// any call after the leader's first (which become_leader() schedules
// immediately) must be failure-triggered.
BOOST_AUTO_TEST_CASE(heartbeat_failures_trigger_immediate_assessment,
                     *boost::unit_test::timeout(scaled_timeout(20))) {
    auto cfg = fast_config();
    cfg._quorum_check_interval = std::chrono::minutes{10};
    test_cluster c{cfg};
    c.add_node(1, {1, 2, 3});
    c.add_node(2, {1, 2, 3});
    c.add_node(3, {1, 2, 3});
    BOOST_REQUIRE(c.elect_node1_and_run());
    BOOST_REQUIRE(wait_until([&] { return c.state->assess_count(1) >= 1; }));
    // Let the leader's first assessment settle, then confirm the timer alone
    // does not produce a second call.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    auto before = c.state->assess_count(1);
    BOOST_REQUIRE_EQUAL(before, 1u);

    c.kill(3);
    BOOST_CHECK(wait_until([&] { return c.state->assess_count(1) > before; }));
}

// Req 16.4 / 14.3 — a degraded group gets exactly one provision_node call,
// and none while that provision is pending (provisioned but not yet joined).
BOOST_AUTO_TEST_CASE(provisions_once_while_pending,
                     *boost::unit_test::timeout(scaled_timeout(20))) {
    test_cluster c{fast_config()};
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "g", .target_count = 2}}};
        c.state->provision_ids = {9};  // node 9 never joins
    }
    c.add_node(1, {1}, {{1, "g"}});
    BOOST_REQUIRE(c.elect_node1_and_run());

    BOOST_REQUIRE(wait_until([&] { return c.state->assess_count(1) >= 10; }));
    std::lock_guard lock(c.state->mu);
    BOOST_REQUIRE_EQUAL(c.state->provisions.size(), 1u);
    BOOST_CHECK_EQUAL(c.state->provisions[0].first, "g");
    BOOST_CHECK(!c.state->provisions[0].second.has_value());
}

// Req 14.6 — a failed provision_node clears its pending slot, so the next
// assessment retries.
BOOST_AUTO_TEST_CASE(failed_provision_is_retried, *boost::unit_test::timeout(scaled_timeout(20))) {
    test_cluster c{fast_config()};
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "g", .target_count = 2}}};
    }
    c.add_node(1, {1}, {{1, "g"}});
    BOOST_REQUIRE(c.elect_node1_and_run());
    BOOST_CHECK(wait_until([&] { return c.state->provision_count() >= 3; }));
}

// Req 16.5 — when assess_quorum reports `lost` the leader never provisions.
BOOST_AUTO_TEST_CASE(no_provision_on_quorum_lost, *boost::unit_test::timeout(scaled_timeout(20))) {
    test_cluster c{fast_config()};
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "g", .target_count = 3}}};
        c.state->forced_status = kythira::quorum_status::lost;
        c.state->provision_ids = {7, 8};
    }
    c.add_node(1, {1}, {{1, "g"}});
    BOOST_REQUIRE(c.elect_node1_and_run());

    BOOST_REQUIRE(wait_until([&] { return c.state->assess_count(1) >= 10; }));
    BOOST_CHECK_EQUAL(c.state->provision_count(), 0u);
}

// Req 13.5 — a failed assessment is retried after quorum_check_interval, not
// on every heartbeat tick.
BOOST_AUTO_TEST_CASE(failed_assessment_waits_for_interval,
                     *boost::unit_test::timeout(scaled_timeout(20))) {
    auto cfg = fast_config();
    cfg._quorum_check_interval = std::chrono::milliseconds{250};
    test_cluster c{cfg};
    {
        std::lock_guard lock(c.state->mu);
        c.state->assess_fails = true;
    }
    c.add_node(1, {1});
    BOOST_REQUIRE(c.elect_node1_and_run());
    BOOST_REQUIRE(wait_until([&] { return c.state->assess_count(1) >= 1; }));

    // ~100 heartbeat ticks elapse; only the interval may schedule retries.
    std::this_thread::sleep_for(std::chrono::milliseconds{1000});
    auto calls = c.state->assess_count(1);
    BOOST_CHECK_GE(calls, 2u);
    BOOST_CHECK_LE(calls, 7u);
}

// Req 16.6, 16.7, 16.9 — a three-node cluster with one permanently failed node
// heals back to three live voters with no operator action: the leader
// provisions a replacement, records its placement, promotes it once it has
// joined, removes the failed node and only then decommissions it.  A second
// failure heals the same way, which fails if the pending-provision counter
// is never released after the first heal (Req 14.5).
BOOST_AUTO_TEST_CASE(self_heals_failed_nodes_repeatedly,
                     *boost::unit_test::timeout(scaled_timeout(45))) {
    test_cluster c{fast_config()};
    std::vector<std::size_t> size_at_decommission;
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "g", .target_count = 3}}};
        c.state->provision_ids = {4, 5};
        c.state->on_decommission = [&c, &size_at_decommission](std::uint64_t) {
            // Runs on node 1's tick thread before decommission_node returns.
            size_at_decommission.push_back(c.node(1).get_cluster_size());
        };
    }
    std::unordered_map<std::uint64_t, std::string> placement{{1, "g"}, {2, "g"}, {3, "g"}};
    c.add_node(1, {1, 2, 3}, placement);
    c.add_node(2, {1, 2, 3}, placement);
    c.add_node(3, {1, 2, 3}, placement);
    BOOST_REQUIRE(c.elect_node1_and_run());

    // One heal round: `failed` dies, `spare` is provisioned and joins through
    // the same add_learner() path a ClusterJoin takes.
    auto heal = [&](std::uint64_t failed, std::uint64_t spare, std::size_t round) {
        c.kill(failed);
        BOOST_REQUIRE(wait_until([&] { return c.state->provision_count() >= round; }));
        {
            std::lock_guard lock(c.state->mu);
            BOOST_REQUIRE_EQUAL(c.state->provisions.size(), round);
            BOOST_CHECK_EQUAL(c.state->provisions.back().first, "g");
            BOOST_CHECK(c.state->provisions.back().second == std::optional<std::uint64_t>{failed});
        }

        c.add_node(spare, {spare});
        c.start_ticker(spare);
        c.node(1).add_learner(spare).detach();

        BOOST_REQUIRE(wait_until([&] { return c.state->decommissioned().size() >= round; },
                                 scaled_deadline(10000)));
        BOOST_CHECK_EQUAL(c.state->decommissioned().back(), failed);
        // Req 15.3 — the failed node had already left the configuration when
        // its infrastructure was decommissioned.
        BOOST_CHECK_EQUAL(size_at_decommission.back(), 3u);

        // Req 16.6 — later assessments carry the new node in its group and no
        // longer carry the failed one.
        auto calls = c.state->assess_count(1);
        BOOST_REQUIRE(wait_until([&] { return c.state->assess_count(1) > calls; }));
        auto cluster = c.state->last_assess(1);
        BOOST_REQUIRE(cluster.has_value());
        BOOST_CHECK_EQUAL(cluster->size(), 3u);
        BOOST_CHECK(group_of(*cluster, spare) == std::optional<std::string>{"g"});
        BOOST_CHECK(!group_of(*cluster, failed).has_value());
    };

    heal(3, 4, 1);
    heal(2, 5, 2);

    BOOST_CHECK(c.node(1).is_leader());
    BOOST_CHECK_EQUAL(c.node(1).get_cluster_size(), 3u);
    // Healthy again: no further provisioning.
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    BOOST_CHECK_EQUAL(c.state->provision_count(), 2u);
}

// A provisioned node can start and send its ClusterJoin before the leader's
// provision_node() call returns, i.e. before the leader has recorded its
// placement and the voter it replaces. Under rootless Podman the container
// starts that fast every time. The leader must not answer "accepted" and then
// have add_learner() refuse the node for capacity, which left it waiting
// forever and the failed node never decommissioned; it defers the join, and
// the node's retry is admitted once the provision is recorded.
BOOST_AUTO_TEST_CASE(join_racing_its_own_provision_is_admitted_on_retry,
                     *boost::unit_test::timeout(scaled_timeout(45))) {
    // The gate below holds the leader's quorum loop mid-provision; long
    // election timeouts keep a follower from campaigning meanwhile.
    auto cfg = fast_config();
    cfg._election_timeout_min = std::chrono::milliseconds{1000};
    cfg._election_timeout_max = std::chrono::milliseconds{2000};
    test_cluster c{cfg};
    std::mutex gate_mu;
    std::condition_variable gate_cv;
    bool in_provision = false;
    bool released = false;
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "g", .target_count = 3}}};
        c.state->provision_ids = {4};
        c.state->on_provision = [&](std::uint64_t) {
            std::unique_lock lk(gate_mu);
            in_provision = true;
            gate_cv.notify_all();
            gate_cv.wait(lk, [&] { return released; });
        };
    }
    std::unordered_map<std::uint64_t, std::string> placement{{1, "g"}, {2, "g"}, {3, "g"}};
    c.add_node(1, {1, 2, 3}, placement);
    c.add_node(2, {1, 2, 3}, placement);
    c.add_node(3, {1, 2, 3}, placement);
    BOOST_REQUIRE(c.elect_node1_and_run());

    c.kill(3);
    {
        std::unique_lock lk(gate_mu);
        BOOST_REQUIRE(gate_cv.wait_for(lk, scaled_deadline(5000), [&] { return in_provision; }));
    }

    // The replacement is up and joins while the leader is still inside
    // provision_node().
    c.add_node(4, {4});
    c.start_ticker(4);
    BOOST_CHECK(!c.send_join(4, 1));

    {
        std::lock_guard lk(gate_mu);
        released = true;
    }
    gate_cv.notify_all();

    // The joiner's bootstrap retries; once the provision is recorded the
    // leader admits it.
    BOOST_CHECK(wait_until([&] { return c.send_join(4, 1); }, scaled_deadline(5000)));

    // And the heal completes: 4 is promoted, 3 removed and decommissioned.
    BOOST_REQUIRE(
        wait_until([&] { return !c.state->decommissioned().empty(); }, scaled_deadline(10000)));
    BOOST_CHECK_EQUAL(c.state->decommissioned().front(), 3u);
    BOOST_CHECK_EQUAL(c.node(1).get_cluster_size(), 3u);
}

// Req 13.7 / 16.11 — a voter whose kythira process has died while its VM
// keeps running is not live: once it has been silent for
// quorum_peer_dead_after the leader replaces it, removes it and decommissions
// it, although the quorum manager never reports it unreachable.
BOOST_AUTO_TEST_CASE(crashed_process_on_running_vm_is_replaced,
                     *boost::unit_test::timeout(scaled_timeout(30))) {
    auto cfg = fast_config();
    cfg._quorum_peer_dead_after = std::chrono::milliseconds{300};
    test_cluster c{cfg};
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "g", .target_count = 3}}};
        c.state->provision_ids = {4};
    }
    std::unordered_map<std::uint64_t, std::string> placement{{1, "g"}, {2, "g"}, {3, "g"}};
    c.add_node(1, {1, 2, 3}, placement);
    c.add_node(2, {1, 2, 3}, placement);
    c.add_node(3, {1, 2, 3}, placement);
    BOOST_REQUIRE(c.elect_node1_and_run());

    c.kill_process(3);
    BOOST_REQUIRE(wait_until([&] { return c.state->provision_count() >= 1; }));
    {
        std::lock_guard lock(c.state->mu);
        BOOST_REQUIRE_EQUAL(c.state->provisions.size(), 1u);
        BOOST_CHECK_EQUAL(c.state->provisions[0].first, "g");
        BOOST_CHECK(c.state->provisions[0].second == std::optional<std::uint64_t>{3});
        // The quorum manager itself never saw the failure.
        BOOST_CHECK(c.state->unreachable.empty());
    }

    c.add_node(4, {4});
    c.start_ticker(4);
    c.node(1).add_learner(4).detach();
    BOOST_REQUIRE(
        wait_until([&] { return !c.state->decommissioned().empty(); }, scaled_deadline(10000)));
    BOOST_CHECK_EQUAL(c.state->decommissioned().front(), 3u);
    BOOST_CHECK(c.node(1).is_leader());
    BOOST_CHECK_EQUAL(c.node(1).get_cluster_size(), 3u);
}

// A quorum manager that derives ids from the resources it can still list
// can hand out the id of an existing member once that member's resource has
// vanished.  The leader must refuse such a replacement: recording it let
// reconcile_pending_replacements() take the existing voter for the
// replacement, remove the voter it was replacing and decommission it.  The
// new resource is decommissioned instead, the slot is released so the next
// assessment provisions again, and a ClusterJoin from the new node under the
// voter's id does not rebind the voter's address.
BOOST_AUTO_TEST_CASE(replacement_with_a_member_id_is_refused,
                     *boost::unit_test::timeout(scaled_timeout(45))) {
    test_cluster c{fast_config()};
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "g", .target_count = 3}}};
        // 2 is a live voter; 4 is a genuinely new id.
        c.state->provision_ids = {2};
    }
    std::unordered_map<std::uint64_t, std::string> placement{{1, "g"}, {2, "g"}, {3, "g"}};
    c.add_node(1, {1, 2, 3}, placement);
    c.add_node(2, {1, 2, 3}, placement);
    c.add_node(3, {1, 2, 3}, placement);
    BOOST_REQUIRE(c.elect_node1_and_run());

    c.kill(3);
    BOOST_REQUIRE(wait_until([&] { return !c.state->decommissioned().empty(); }));
    BOOST_CHECK(c.state->decommissioned() == std::vector<std::uint64_t>{2});

    // The empty node behind the reused id joins before it is torn down.
    BOOST_CHECK(c.send_join(2, 1, std::string{"reused-2"}));
    BOOST_CHECK(c.rebinds->for_peer(2).empty());

    // The slot was released: the leader provisions again (and fails, with
    // no ids left), and meanwhile neither 2 nor 3 is removed.
    BOOST_REQUIRE(wait_until([&] { return c.state->provision_count() >= 3; }));
    BOOST_CHECK(c.state->decommissioned() == std::vector<std::uint64_t>{2});
    BOOST_CHECK_EQUAL(c.node(1).get_cluster_size(), 3u);
    auto cluster = c.state->last_assess(1);
    BOOST_REQUIRE(cluster.has_value());
    BOOST_CHECK(group_of(*cluster, 2) == std::optional<std::string>{"g"});
    BOOST_CHECK(group_of(*cluster, 3) == std::optional<std::string>{"g"});

    // Voter 2 still counts: a command commits with 3 down.
    auto commit = c.node(1).submit_command(
        kythira::test_key_value_state_machine<std::uint64_t>::make_put_command("k", "v"),
        std::chrono::milliseconds{2000});
    BOOST_CHECK_NO_THROW(std::move(commit).get());

    // A fresh id heals the group normally, replacing 3.
    {
        std::lock_guard lock(c.state->mu);
        c.state->provision_ids = {4};
    }
    BOOST_REQUIRE(wait_until(
        [&] {
            std::lock_guard lock(c.state->mu);
            return c.state->provision_ids.empty();
        },
        scaled_deadline(5000)));
    c.add_node(4, {4});
    c.start_ticker(4);
    BOOST_CHECK(wait_until([&] { return c.send_join(4, 1); }, scaled_deadline(5000)));
    BOOST_REQUIRE(
        wait_until([&] { return c.state->decommissioned().size() >= 2; }, scaled_deadline(10000)));
    BOOST_CHECK(c.state->decommissioned() == (std::vector<std::uint64_t>{2, 3}));
    BOOST_CHECK(c.rebinds->for_peer(4) == std::vector<std::string>{"4"});
}

// Only a fresh node sends ClusterJoin, so one under a voter's id and a new
// address is a different, empty node: the leader must keep replicating to
// (and counting) the voter at its existing address.  A learner, which
// neither votes nor counts towards commit, is rebound: a provisioned learner
// that restarts empty has no other way to reach the current leader.
BOOST_AUTO_TEST_CASE(cluster_join_does_not_rebind_a_voter,
                     *boost::unit_test::timeout(scaled_timeout(30))) {
    test_cluster c{fast_config()};
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "g", .target_count = 3, .learner_capacity = 1}}};
    }
    std::unordered_map<std::uint64_t, std::string> placement{{1, "g"}, {2, "g"}, {3, "g"}};
    c.add_node(1, {1, 2, 3}, placement);
    c.add_node(2, {1, 2, 3}, placement);
    c.add_node(3, {1, 2, 3}, placement);
    BOOST_REQUIRE(c.elect_node1_and_run());

    // A duplicate join still answers "accepted", but leaves 3's address alone.
    BOOST_CHECK(c.send_join(3, 1, std::string{"elsewhere"}));
    BOOST_CHECK(c.rebinds->for_peer(3).empty());
    BOOST_CHECK_EQUAL(c.node(1).get_cluster_size(), 3u);

    // The leader itself is a voter too.
    BOOST_CHECK(c.send_join(1, 1, std::string{"elsewhere"}));
    BOOST_CHECK(c.rebinds->for_peer(1).empty());

    // A learner is rebound to the address it now advertises.
    c.node(1).set_placement(4, "g");
    BOOST_CHECK_NO_THROW(c.node(1).add_learner(4).get());
    BOOST_CHECK(c.send_join(4, 1, std::string{"4-restarted"}));
    BOOST_CHECK(c.rebinds->for_peer(4) == std::vector<std::string>{"4-restarted"});

    // And a new id is bound as before.
    c.node(1).set_placement(5, "g");
    BOOST_CHECK(c.send_join(5, 1));
    BOOST_CHECK(c.rebinds->for_peer(5) == std::vector<std::string>{"5"});
}

// Req 13.7 — a voter silent for less than quorum_peer_dead_after is still
// live, so a process restart or a long pause does not cost an instance.
BOOST_AUTO_TEST_CASE(silent_peer_inside_window_is_not_replaced,
                     *boost::unit_test::timeout(scaled_timeout(20))) {
    auto cfg = fast_config();
    cfg._quorum_peer_dead_after = std::chrono::minutes{10};
    test_cluster c{cfg};
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "g", .target_count = 3}}};
        c.state->provision_ids = {4};
    }
    std::unordered_map<std::uint64_t, std::string> placement{{1, "g"}, {2, "g"}, {3, "g"}};
    c.add_node(1, {1, 2, 3}, placement);
    c.add_node(2, {1, 2, 3}, placement);
    c.add_node(3, {1, 2, 3}, placement);
    BOOST_REQUIRE(c.elect_node1_and_run());

    c.kill_process(3);
    auto before = c.state->assess_count(1);
    BOOST_REQUIRE(wait_until([&] { return c.state->assess_count(1) >= before + 20; }));
    BOOST_CHECK_EQUAL(c.state->provision_count(), 0u);
}

// Req 13.7 — quorum_peer_dead_after = 0 turns the rule off: the quorum
// manager's infrastructure view is the only liveness signal.
BOOST_AUTO_TEST_CASE(zero_peer_dead_after_disables_process_liveness,
                     *boost::unit_test::timeout(scaled_timeout(20))) {
    auto cfg = fast_config();
    cfg._quorum_peer_dead_after = std::chrono::milliseconds{0};
    test_cluster c{cfg};
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "g", .target_count = 3}}};
        c.state->provision_ids = {4};
    }
    std::unordered_map<std::uint64_t, std::string> placement{{1, "g"}, {2, "g"}, {3, "g"}};
    c.add_node(1, {1, 2, 3}, placement);
    c.add_node(2, {1, 2, 3}, placement);
    c.add_node(3, {1, 2, 3}, placement);
    BOOST_REQUIRE(c.elect_node1_and_run());

    c.kill_process(3);
    std::this_thread::sleep_for(std::chrono::milliseconds{1000});
    BOOST_CHECK_EQUAL(c.state->provision_count(), 0u);
}

// Req 16.7 / 15.4 — a decommission failure is logged only; the removal of
// the failed node from the configuration stands.
BOOST_AUTO_TEST_CASE(decommission_failure_keeps_removal,
                     *boost::unit_test::timeout(scaled_timeout(30))) {
    test_cluster c{fast_config()};
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "g", .target_count = 3}}};
        c.state->provision_ids = {4};
        c.state->decommission_fails = true;
    }
    std::unordered_map<std::uint64_t, std::string> placement{{1, "g"}, {2, "g"}, {3, "g"}};
    c.add_node(1, {1, 2, 3}, placement);
    c.add_node(2, {1, 2, 3}, placement);
    c.add_node(3, {1, 2, 3}, placement);
    BOOST_REQUIRE(c.elect_node1_and_run());

    c.kill(3);
    BOOST_REQUIRE(wait_until([&] { return c.state->provision_count() >= 1; }));
    c.add_node(4, {4});
    c.start_ticker(4);
    c.node(1).add_learner(4).detach();

    BOOST_REQUIRE(
        wait_until([&] { return !c.state->decommissioned().empty(); }, scaled_deadline(10000)));
    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    BOOST_CHECK_EQUAL(c.state->decommissioned().size(), 1u);
    BOOST_CHECK_EQUAL(c.node(1).get_cluster_size(), 3u);
    auto cluster = c.state->last_assess(1);
    BOOST_REQUIRE(cluster.has_value());
    BOOST_CHECK(!group_of(*cluster, 3).has_value());
    BOOST_CHECK(group_of(*cluster, 4).has_value());
}

// Req 16.8 / 15.5 — an operator-initiated remove_server() never triggers
// decommission_node.
BOOST_AUTO_TEST_CASE(operator_remove_does_not_decommission,
                     *boost::unit_test::timeout(scaled_timeout(20))) {
    test_cluster c{fast_config()};
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "g", .target_count = 2}}};
    }
    std::unordered_map<std::uint64_t, std::string> placement{{1, "g"}, {2, "g"}, {3, "g"}};
    c.add_node(1, {1, 2, 3}, placement);
    c.add_node(2, {1, 2, 3}, placement);
    c.add_node(3, {1, 2, 3}, placement);
    BOOST_REQUIRE(c.elect_node1_and_run());

    c.node(1).remove_server(3).detach();
    BOOST_REQUIRE(wait_until([&] { return c.node(1).get_cluster_size() == 2u; }));
    auto calls = c.state->assess_count(1);
    BOOST_REQUIRE(wait_until([&] { return c.state->assess_count(1) >= calls + 5; }));

    BOOST_CHECK(c.state->decommissioned().empty());
    BOOST_CHECK_EQUAL(c.state->provision_count(), 0u);
}

// Req 9.2 — when every node of a placement group is unreachable and the
// rest still hold a majority, the replacements go to the surviving groups,
// spread across them, and never to the lost group.  Once the failed nodes
// are removed the lost group is empty but still short of its target; the
// surplus in the surviving groups stands in for it, so nothing more is
// provisioned into the failed zone.
BOOST_AUTO_TEST_CASE(whole_group_loss_provisions_into_surviving_groups,
                     *boost::unit_test::timeout(scaled_timeout(45))) {
    test_cluster c{fast_config()};
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "az-a", .target_count = 2},
                                    {.group_id = "az-b", .target_count = 2},
                                    {.group_id = "az-c", .target_count = 1}}};
        c.state->provision_ids = {6, 7, 8, 9};
    }
    std::unordered_map<std::uint64_t, std::string> placement{
        {1, "az-a"}, {2, "az-a"}, {3, "az-b"}, {4, "az-b"}, {5, "az-c"}};
    for (std::uint64_t id = 1; id <= 5; ++id) {
        c.add_node(id, {1, 2, 3, 4, 5}, placement);
    }
    BOOST_REQUIRE(c.elect_node1_and_run());

    // Both at once: an assessment between the two kills would see az-b only
    // degraded (replacement_in_group_that_then_fails_is_abandoned covers that).
    {
        std::lock_guard lock(c.state->mu);
        c.state->unreachable.insert({3, 4});
    }
    c.kill(3);
    c.kill(4);
    BOOST_REQUIRE(wait_until([&] { return c.state->provision_count() >= 2; }));
    {
        std::lock_guard lock(c.state->mu);
        BOOST_REQUIRE_EQUAL(c.state->provisions.size(), 2u);
        // The emptier surviving group first, then the other.
        BOOST_CHECK_EQUAL(c.state->provisions[0].first, "az-c");
        BOOST_CHECK(c.state->provisions[0].second == std::optional<std::uint64_t>{3});
        BOOST_CHECK_EQUAL(c.state->provisions[1].first, "az-a");
        BOOST_CHECK(c.state->provisions[1].second == std::optional<std::uint64_t>{4});
    }

    // Both join as learners even though their groups are at target: each
    // replaces a voter of the lost group.
    for (std::uint64_t spare : {6u, 7u}) {
        c.add_node(spare, {spare});
        c.start_ticker(spare);
        c.node(1).add_learner(spare).detach();
    }

    BOOST_REQUIRE(
        wait_until([&] { return c.state->decommissioned().size() >= 2; }, scaled_deadline(15000)));
    auto decommissioned = c.state->decommissioned();
    std::sort(decommissioned.begin(), decommissioned.end());
    BOOST_CHECK(decommissioned == (std::vector<std::uint64_t>{3, 4}));
    BOOST_CHECK_EQUAL(c.node(1).get_cluster_size(), 5u);

    auto calls = c.state->assess_count(1);
    BOOST_REQUIRE(wait_until([&] { return c.state->assess_count(1) > calls + 5; }));
    auto cluster = c.state->last_assess(1);
    BOOST_REQUIRE(cluster.has_value());
    BOOST_CHECK(group_of(*cluster, 6) == std::optional<std::string>{"az-c"});
    BOOST_CHECK(group_of(*cluster, 7) == std::optional<std::string>{"az-a"});
    BOOST_CHECK(!group_of(*cluster, 3).has_value());
    BOOST_CHECK(!group_of(*cluster, 4).has_value());
    // az-b is empty and below target, but nothing is provisioned into it.
    BOOST_CHECK_EQUAL(c.state->provision_count(), 2u);
}

// Req 9.2 — a replacement provisioned into a group that then loses its last
// live node is abandoned and decommissioned: it cannot join from a failed
// zone.  Both of the group's failed nodes are then replaced elsewhere.
BOOST_AUTO_TEST_CASE(replacement_in_group_that_then_fails_is_abandoned,
                     *boost::unit_test::timeout(scaled_timeout(30))) {
    test_cluster c{fast_config()};
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "az-a", .target_count = 2},
                                    {.group_id = "az-b", .target_count = 2},
                                    {.group_id = "az-c", .target_count = 1}}};
        c.state->provision_ids = {6, 7, 8, 9};
    }
    std::unordered_map<std::uint64_t, std::string> placement{
        {1, "az-a"}, {2, "az-a"}, {3, "az-b"}, {4, "az-b"}, {5, "az-c"}};
    for (std::uint64_t id = 1; id <= 5; ++id) {
        c.add_node(id, {1, 2, 3, 4, 5}, placement);
    }
    BOOST_REQUIRE(c.elect_node1_and_run());

    c.kill(3);
    BOOST_REQUIRE(wait_until([&] { return c.state->provision_count() >= 1; }));
    {
        std::lock_guard lock(c.state->mu);
        BOOST_REQUIRE_EQUAL(c.state->provisions[0].first, "az-b");
    }

    c.kill(4);
    BOOST_REQUIRE(wait_until([&] { return c.state->provision_count() >= 3; }));
    BOOST_REQUIRE(wait_until([&] { return !c.state->decommissioned().empty(); }));
    BOOST_CHECK_EQUAL(c.state->decommissioned().front(), 6u);
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    std::lock_guard lock(c.state->mu);
    BOOST_REQUIRE_EQUAL(c.state->provisions.size(), 3u);
    std::set<std::uint64_t> replaced;
    for (std::size_t i = 1; i < 3; ++i) {
        BOOST_CHECK_NE(c.state->provisions[i].first, "az-b");
        BOOST_REQUIRE(c.state->provisions[i].second.has_value());
        replaced.insert(*c.state->provisions[i].second);
    }
    BOOST_CHECK(replaced == (std::set<std::uint64_t>{3, 4}));
}

// Placement travels in configuration entries, so a leader elected after a
// replacement was provisioned still knows that node's group.  Here a
// one-node group is lost, its replacement goes to az-a, and leadership then
// moves to node 2, whose own initial placement never named node 4.
BOOST_AUTO_TEST_CASE(placement_survives_leader_change,
                     *boost::unit_test::timeout(scaled_timeout(45))) {
    test_cluster c{fast_config()};
    {
        std::lock_guard lock(c.state->mu);
        c.state->topo = {.groups = {{.group_id = "az-a", .target_count = 1},
                                    {.group_id = "az-b", .target_count = 1},
                                    {.group_id = "az-c", .target_count = 1}}};
        c.state->provision_ids = {4, 5};
    }
    std::unordered_map<std::uint64_t, std::string> placement{{1, "az-a"}, {2, "az-b"}, {3, "az-c"}};
    c.add_node(1, {1, 2, 3}, placement);
    c.add_node(2, {1, 2, 3}, placement);
    c.add_node(3, {1, 2, 3}, placement);
    BOOST_REQUIRE(c.elect_node1_and_run());

    c.kill(3);
    BOOST_REQUIRE(wait_until([&] { return c.state->provision_count() >= 1; }));
    {
        std::lock_guard lock(c.state->mu);
        BOOST_CHECK_EQUAL(c.state->provisions[0].first, "az-a");
    }
    c.add_node(4, {4});
    c.start_ticker(4);
    c.node(1).add_learner(4).detach();
    BOOST_REQUIRE(
        wait_until([&] { return !c.state->decommissioned().empty(); }, scaled_deadline(10000)));

    c.node(1).transfer_leadership(2, scaled_deadline(3000)).detach();
    BOOST_REQUIRE(wait_until([&] { return c.node(2).is_leader(); }, scaled_deadline(10000)));
    BOOST_REQUIRE(wait_until([&] { return c.state->assess_count(2) >= 3; }));

    auto cluster = c.state->last_assess(2);
    BOOST_REQUIRE(cluster.has_value());
    BOOST_CHECK_EQUAL(cluster->size(), 3u);
    BOOST_CHECK(group_of(*cluster, 4) == std::optional<std::string>{"az-a"});
    // The new leader sees az-c's deficit covered by az-a's surplus.
    BOOST_CHECK_EQUAL(c.state->provision_count(), 1u);
}

// Req 12.2 — set_placement accepts entries for members and non-members.
BOOST_AUTO_TEST_CASE(set_placement_does_not_crash, *boost::unit_test::timeout(5)) {
    test_cluster c{fast_config()};
    auto& n = c.add_node(1, {1});
    BOOST_CHECK_NO_THROW(n.set_placement(42u, std::string{"az-a"}));
    BOOST_CHECK_NO_THROW(n.set_placement(1u, std::string{"az-b"}));
}

// Req 16.10 — existing node_config constructor populates initial_placement
BOOST_AUTO_TEST_CASE(node_config_with_initial_placement, *boost::unit_test::timeout(5)) {
    network_simulator::NetworkSimulator<test_raft_types_with_qm::raft_network_types> sim;
    sim.start();
    auto net = sim.create_node("1");
    test_raft_types_with_qm::serializer_type ser;

    kythira::raft_configuration cfg;
    cfg._quorum_check_interval = std::chrono::milliseconds{1000};

    std::unordered_map<std::uint64_t, std::string> placement{{1, "az-a"}, {2, "az-b"}};

    kythira::node_config<test_raft_types_with_qm> ncfg{
        .node_id = 1,
        .network_client = test_raft_types_with_qm::network_client_type{net, ser},
        .network_server = test_raft_types_with_qm::network_server_type{net, ser},
        .persistence = test_raft_types_with_qm::persistence_engine_type{},
        .logger = test_raft_types_with_qm::logger_type{},
        .metrics = test_raft_types_with_qm::metrics_type{},
        .membership = test_raft_types_with_qm::membership_manager_type{},
        .config = cfg,
        .initial_placement = std::move(placement),
    };

    auto n = std::make_unique<test_node_type>(std::move(ncfg));
    // Just verify construction succeeds with initial_placement populated
    BOOST_CHECK_EQUAL(n->get_node_id(), 1u);
}

// Req 16 — quorum check interval config round-trips correctly
BOOST_AUTO_TEST_CASE(quorum_config_roundtrip, *boost::unit_test::timeout(5)) {
    kythira::raft_configuration cfg;
    cfg._quorum_check_interval = std::chrono::milliseconds{42000};
    cfg._quorum_heartbeat_failure_threshold = 5;
    cfg._quorum_peer_dead_after = std::chrono::milliseconds{12000};

    BOOST_CHECK_EQUAL(cfg.quorum_check_interval().count(), 42000);
    BOOST_CHECK_EQUAL(cfg.quorum_heartbeat_failure_threshold(), 5u);
    BOOST_CHECK_EQUAL(cfg.quorum_peer_dead_after().count(), 12000);
    BOOST_CHECK_EQUAL(kythira::raft_configuration{}.quorum_peer_dead_after().count(), 30000);

    auto errors = cfg.get_validation_errors();
    BOOST_CHECK(errors.empty());
}

// Req 11.5 — quorum config validation rejects zero interval
BOOST_AUTO_TEST_CASE(quorum_config_validation_rejects_zero_interval,
                     *boost::unit_test::timeout(5)) {
    kythira::raft_configuration cfg;
    cfg._quorum_check_interval = std::chrono::milliseconds{0};

    auto errors = cfg.get_validation_errors();
    BOOST_CHECK(!errors.empty());
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("quorum_check_interval") != std::string::npos) {
            found = true;
            break;
        }
    }
    BOOST_CHECK(found);
}

// Req 11.5 — quorum config validation rejects zero threshold
BOOST_AUTO_TEST_CASE(quorum_config_validation_rejects_zero_threshold,
                     *boost::unit_test::timeout(5)) {
    kythira::raft_configuration cfg;
    cfg._quorum_heartbeat_failure_threshold = 0;

    auto errors = cfg.get_validation_errors();
    BOOST_CHECK(!errors.empty());
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("quorum_heartbeat_failure_threshold") != std::string::npos) {
            found = true;
            break;
        }
    }
    BOOST_CHECK(found);
}

// Req 11.5 — a negative quorum_peer_dead_after is rejected; zero is allowed
// (it disables the rule).
BOOST_AUTO_TEST_CASE(quorum_config_validation_rejects_negative_peer_dead_after,
                     *boost::unit_test::timeout(5)) {
    kythira::raft_configuration cfg;
    cfg._quorum_peer_dead_after = std::chrono::milliseconds{0};
    BOOST_CHECK(cfg.get_validation_errors().empty());

    cfg._quorum_peer_dead_after = std::chrono::milliseconds{-1};
    auto errors = cfg.get_validation_errors();
    BOOST_CHECK(std::any_of(errors.begin(), errors.end(), [](const std::string& e) {
        return e.find("quorum_peer_dead_after") != std::string::npos;
    }));
}

BOOST_AUTO_TEST_SUITE_END()
