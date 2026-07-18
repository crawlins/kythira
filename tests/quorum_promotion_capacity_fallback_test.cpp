// Tests for the leader's automatic promotion-vs-provisioning policy:
// .kiro/specs/non-voting-nodes/requirements.md, Requirement 4.
//
// When the leader's quorum-maintenance loop finds a placement group below its
// voting target, it must prefer promoting an eligible, capacity-permitted learner
// over calling provision_node() — and fall back to provision_node() when the
// promotion capacity criterion blocks every eligible learner in that group.

#define BOOST_TEST_MODULE quorum_promotion_capacity_fallback_test
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#include <folly/init/Init.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("quorum_promotion_capacity_fallback_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {

// Shared, pointer-stable state so the test can inspect provision_node() call
// counts after the mock has been moved into node_config (mocks are copied/moved
// by value into the node, so any non-shared state would be inaccessible afterward).
struct call_recorder {
    std::atomic<std::size_t> provision_calls{0};
};

struct mock_quorum_manager {
    using node_id_type = std::uint64_t;
    using address_type = std::string;
    using placement_group_id_type = std::string;

    std::shared_ptr<call_recorder> recorder = std::make_shared<call_recorder>();
    // Template for status/target_count per group; live_count is recomputed dynamically
    // below from the real cluster passed to assess_quorum (minus `unreachable`), so a
    // deficit this test injects (e.g. via `unreachable`) automatically clears once the
    // real membership genuinely resolves it — matching how a real quorum_manager
    // would behave, and avoiding a stale, permanently-perceived deficit after a
    // promotion actually succeeds.
    std::shared_ptr<kythira::quorum_health<node_id_type, placement_group_id_type>> next_health =
        std::make_shared<kythira::quorum_health<node_id_type, placement_group_id_type>>();
    std::shared_ptr<kythira::desired_topology<placement_group_id_type>> topo =
        std::make_shared<kythira::desired_topology<placement_group_id_type>>();
    // Node IDs to treat as unreachable regardless of configuration — models a real,
    // still-configured voter that has died (distinct from a learner never having
    // joined at all).
    std::shared_ptr<std::vector<node_id_type>> unreachable =
        std::make_shared<std::vector<node_id_type>>();

    auto assess_quorum(
        const std::vector<kythira::node_placement<node_id_type, placement_group_id_type>>& cluster)
        -> kythira::Future<kythira::quorum_health<node_id_type, placement_group_id_type>> {
        auto health = *next_health;
        health.total_node_count = cluster.size();
        for (auto& g : health.groups) {
            g.live_count = static_cast<std::size_t>(
                std::count_if(cluster.begin(), cluster.end(), [&](const auto& np) {
                    return np.group_id == g.group_id &&
                           std::find(unreachable->begin(), unreachable->end(), np.node_id) ==
                               unreachable->end();
                }));
        }
        health.live_node_count = static_cast<std::size_t>(
            std::count_if(cluster.begin(), cluster.end(), [&](const auto& np) {
                return std::find(unreachable->begin(), unreachable->end(), np.node_id) ==
                       unreachable->end();
            }));
        bool all_met = std::all_of(health.groups.begin(), health.groups.end(),
                                   [](const auto& g) { return g.live_count >= g.target_count; });
        health.status = all_met ? kythira::quorum_status::healthy : next_health->status;
        return kythira::FutureFactory::makeFuture(health);
    }
    auto provision_node(placement_group_id_type, std::optional<node_id_type>)
        -> kythira::Future<kythira::peer_info<node_id_type, address_type>> {
        recorder->provision_calls.fetch_add(1, std::memory_order_relaxed);
        // No infrastructure to actually provision in this test; fail so the
        // leader's pending-provision bookkeeping clears itself.
        return kythira::FutureFactory::makeExceptionalFuture<
            kythira::peer_info<node_id_type, address_type>>(
            std::runtime_error("mock: provisioning not configured"));
    }
    auto decommission_node(const node_id_type&) -> kythira::Future<void> {
        return kythira::FutureFactory::makeFuture();
    }
    auto maintain_quorum(
        const std::vector<kythira::node_placement<node_id_type, placement_group_id_type>>& cluster)
        -> kythira::Future<kythira::quorum_health<node_id_type, placement_group_id_type>> {
        return assess_quorum(cluster);
    }
    [[nodiscard]] auto topology() const -> kythira::desired_topology<placement_group_id_type> {
        return *topo;
    }
};

static_assert(
    kythira::quorum_manager<mock_quorum_manager, std::uint64_t, std::string, std::string>);

template<typename NodeId, typename Address> class preset_peer_discovery {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    auto register_node(NodeId, Address) -> kythira::Future<void> {
        return kythira::FutureFactory::makeFuture();
    }
    [[nodiscard]] auto find_peers(std::chrono::milliseconds) const
        -> kythira::Future<std::vector<kythira::peer_info<NodeId, Address>>> {
        return kythira::FutureFactory::makeFuture(
            std::vector<kythira::peer_info<NodeId, Address>>{});
    }
};

struct test_types {
    using future_type = kythira::Future<std::vector<std::byte>>;
    using promise_type = kythira::Promise<std::vector<std::byte>>;
    using try_type = kythira::Try<std::vector<std::byte>>;

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

    using address_type = std::string;
    using peer_discovery_type = preset_peer_discovery<node_id_type, address_type>;
    using cluster_join_request_type = kythira::cluster_join_request<node_id_type, address_type>;
    using cluster_join_response_type = kythira::cluster_join_response<node_id_type, address_type>;

    using quorum_manager_type = mock_quorum_manager;
};

using test_node = kythira::node<test_types>;
using sim_t = network_simulator::NetworkSimulator<test_types::raft_network_types>;

template<typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{5000}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(quorum_promotion_capacity_fallback)

// Requirement 4.1, 4.2: an eligible, capacity-permitted learner is promoted
// instead of the leader calling provision_node().
BOOST_AUTO_TEST_CASE(prefers_promotion_over_provisioning, *boost::unit_test::timeout(20)) {
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    network_simulator::NetworkEdge edge{};
    sim.add_edge("1", "2", edge);
    sim.add_edge("2", "1", edge);

    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{50};
    cfg._election_timeout_max = std::chrono::milliseconds{100};
    cfg._heartbeat_interval = std::chrono::milliseconds{15};
    cfg._quorum_check_interval = std::chrono::milliseconds{10};

    mock_quorum_manager qm1;
    // Deficit of 1: target 2, only 1 live (the leader itself).
    *qm1.next_health = {
        .status = kythira::quorum_status::degraded,
        .live_node_count = 1,
        .total_node_count = 1,
        .unreachable_nodes = {},
        .groups = {{.group_id = "", .live_count = 1, .target_count = 2, .unreachable_nodes = {}}},
    };
    // Capacity check: 1 voter < target 2 — room to promote.
    *qm1.topo =
        kythira::desired_topology<std::string>{.groups = {{.group_id = "", .target_count = 2}}};
    auto recorder = qm1.recorder;

    kythira::node_config<test_types> ncfg1{
        .node_id = 1,
        .network_client = {net1, test_types::serializer_type{}},
        .network_server = {net1, test_types::serializer_type{}},
        .persistence = {},
        .logger = kythira::console_logger{},
        .metrics = {},
        .membership = {},
        .config = cfg,
        .self_address = "1",
        .peer_discovery = {},
        .quorum_manager = qm1,
    };
    kythira::node_config<test_types> ncfg2{
        .node_id = 2,
        .network_client = {net2, test_types::serializer_type{}},
        .network_server = {net2, test_types::serializer_type{}},
        .persistence = {},
        .logger = kythira::console_logger{},
        .metrics = {},
        .membership = {},
        .config = cfg,
        .self_address = "2",
        .peer_discovery = {},
        .quorum_manager = mock_quorum_manager{},
    };

    test_node node1{std::move(ncfg1)};
    test_node node2{std::move(ncfg2)};
    node1.set_cluster_configuration({1});
    node2.set_cluster_configuration({2});
    node1.start();
    node2.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    // Admit node2 as a learner first (mirrors ClusterJoin's admit-as-learner path).
    auto add_fut = node1.add_learner(2);
    for (int i = 0; i < 15; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    bool add_done = false;
    std::move(add_fut)
        .thenValue([&](std::vector<std::byte>) { add_done = true; })
        .thenError([&](const std::exception_ptr&) { add_done = true; });
    BOOST_REQUIRE(wait_until([&] { return add_done; }));

    // Drive assessment cycles: the leader should promote node2 rather than provision.
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{15});
        node1.check_heartbeat_timeout();
    }

    BOOST_CHECK(wait_until([&] { return node1.get_cluster_size() == 2u; },
                           std::chrono::milliseconds{3000}));
    BOOST_CHECK_EQUAL(recorder->provision_calls.load(), 0u);

    node2.stop();
    node1.stop();
}

// Requirement 4.2: when the promotion capacity criterion blocks every eligible
// learner, the leader falls back to provision_node().
BOOST_AUTO_TEST_CASE(falls_back_to_provisioning_when_blocked, *boost::unit_test::timeout(20)) {
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");
    network_simulator::NetworkEdge edge{};
    sim.add_edge("1", "2", edge);
    sim.add_edge("2", "1", edge);
    sim.add_edge("1", "3", edge);
    sim.add_edge("3", "1", edge);

    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{50};
    cfg._election_timeout_max = std::chrono::milliseconds{100};
    cfg._heartbeat_interval = std::chrono::milliseconds{15};
    // Deliberately long: become_leader() resets _last_quorum_check to the epoch, so
    // the very first post-election heartbeat tick fires an assessment immediately
    // regardless of this interval. That first, unavoidable cycle is deliberately
    // consumed below before any learner exists (harmless). Keeping the interval
    // long after that prevents a second, unwanted cycle from firing while node2
    // is admitted and node3 is added — both of which must complete before the
    // group is actually at capacity.
    cfg._quorum_check_interval = std::chrono::milliseconds{5000};

    mock_quorum_manager qm1;
    // Target of 2; live_count is recomputed dynamically each call from the real
    // cluster (minus `unreachable`) — see mock_quorum_manager::assess_quorum.
    *qm1.next_health = {
        .status = kythira::quorum_status::degraded,
        .live_node_count = 0,
        .total_node_count = 0,
        .unreachable_nodes = {},
        .groups = {{.group_id = "", .live_count = 0, .target_count = 2, .unreachable_nodes = {}}},
    };
    *qm1.topo =
        kythira::desired_topology<std::string>{.groups = {{.group_id = "", .target_count = 2}}};
    auto recorder = qm1.recorder;

    kythira::node_config<test_types> ncfg1{
        .node_id = 1,
        .network_client = {net1, test_types::serializer_type{}},
        .network_server = {net1, test_types::serializer_type{}},
        .persistence = {},
        .logger = kythira::console_logger{},
        .metrics = {},
        .membership = {},
        .config = cfg,
        .self_address = "1",
        .peer_discovery = {},
        .quorum_manager = qm1,
    };
    kythira::node_config<test_types> ncfg2{
        .node_id = 2,
        .network_client = {net2, test_types::serializer_type{}},
        .network_server = {net2, test_types::serializer_type{}},
        .persistence = {},
        .logger = kythira::console_logger{},
        .metrics = {},
        .membership = {},
        .config = cfg,
        .self_address = "2",
        .peer_discovery = {},
        .quorum_manager = mock_quorum_manager{},
    };
    kythira::node_config<test_types> ncfg3{
        .node_id = 3,
        .network_client = {net3, test_types::serializer_type{}},
        .network_server = {net3, test_types::serializer_type{}},
        .persistence = {},
        .logger = kythira::console_logger{},
        .metrics = {},
        .membership = {},
        .config = cfg,
        .self_address = "3",
        .peer_discovery = {},
        .quorum_manager = mock_quorum_manager{},
    };

    test_node node1{std::move(ncfg1)};
    test_node node2{std::move(ncfg2)};
    test_node node3{std::move(ncfg3)};
    node1.set_cluster_configuration({1});
    node2.set_cluster_configuration({2});
    node3.set_cluster_configuration({3});
    node1.start();
    node2.start();
    node3.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    // Consume the automatic, epoch-triggered first assessment cycle before any
    // learner exists, so it cannot race with the admission below (harmless: no
    // eligible learner yet, so it just attempts — and fails — provision_node()).
    node1.check_heartbeat_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{30});

    // Admit node2 as a learner while there's still room (1 voter < target 2).
    // Well within the 5s quorum_check_interval, so no further automatic cycle
    // fires during this window.
    auto add_fut = node1.add_learner(2);
    for (int i = 0; i < 15; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    bool add_done = false;
    std::move(add_fut)
        .thenValue([&](std::vector<std::byte>) { add_done = true; })
        .thenError([&](const std::exception_ptr&) { add_done = true; });
    BOOST_REQUIRE(wait_until([&] { return add_done; }));

    // Fill the group's remaining slot with a real voter via add_server() (no
    // capacity criterion applies to it) — this is what blocks node2's promotion.
    // Still well within the 5s interval.
    auto add3_fut = node1.add_server(3);
    for (int i = 0; i < 15; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    bool add3_done = false;
    std::move(add3_fut)
        .thenValue([&](std::vector<std::byte>) { add3_done = true; })
        .thenError([&](const std::exception_ptr&) { add3_done = true; });
    BOOST_REQUIRE(wait_until([&] { return add3_done; }));
    BOOST_REQUIRE_EQUAL(node1.get_cluster_size(), 2u);  // voters {1,3}, target now met

    // node3 has died (still configured, but unreachable) — assess_quorum keeps
    // reporting a deficit even though the group's real, CONFIGURED voting count
    // (which the capacity criterion uses) already meets target. This is the
    // realistic scenario for this test: a live replacement is needed, but node2
    // cannot fill it because the group has no remaining voting capacity.
    qm1.unreachable->push_back(3);

    // Wait past the remaining interval so the next assessment cycle fires, now
    // with node2 present as a learner AND the group already at its voting
    // target — the leader must fall back to provision_node() rather than
    // promoting node2.
    std::this_thread::sleep_for(cfg._quorum_check_interval + std::chrono::milliseconds{300});
    for (int i = 0; i < 10; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    BOOST_CHECK(wait_until([&] { return recorder->provision_calls.load() > 0; },
                           std::chrono::milliseconds{3000}));
    // node2 was never promoted — voting membership is unchanged.
    BOOST_CHECK_EQUAL(node1.get_cluster_size(), 2u);

    node3.stop();
    node2.stop();
    node1.stop();
}

BOOST_AUTO_TEST_SUITE_END()
