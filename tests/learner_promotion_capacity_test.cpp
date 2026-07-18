// Tests for the promotion capacity criterion:
// .kiro/specs/non-voting-nodes/requirements.md, Requirement 3.
//
// promote_to_voter() must only promote a learner when its placement group's
// current VOTING-ONLY count is below the group's desired_topology target —
// deliberately distinct from add_learner()'s voting+learner count (Requirement 3.1,
// design.md Property 3). Uses an adjustable quorum manager so the same node can be
// tested first under a generous topology (to admit a learner) and then under a
// tightened one (to exercise the promotion-time capacity check independently).

#define BOOST_TEST_MODULE learner_promotion_capacity_test
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("learner_promotion_capacity_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {

// A quorum manager whose desired_topology can be changed after construction via a
// shared_ptr — lets a single test simulate an operator tightening the target voter
// count between admitting a learner and attempting to promote it.
struct adjustable_quorum_manager {
    using node_id_type = std::uint64_t;
    using address_type = std::string;
    using placement_group_id_type = std::string;

    std::shared_ptr<kythira::desired_topology<placement_group_id_type>> topo =
        std::make_shared<kythira::desired_topology<placement_group_id_type>>();

    auto assess_quorum(
        const std::vector<kythira::node_placement<node_id_type, placement_group_id_type>>& cluster)
        -> kythira::Future<kythira::quorum_health<node_id_type, placement_group_id_type>> {
        return kythira::FutureFactory::makeFuture(
            kythira::quorum_health<node_id_type, placement_group_id_type>{
                .status = kythira::quorum_status::healthy,
                .live_node_count = cluster.size(),
                .total_node_count = cluster.size(),
                .unreachable_nodes = {},
                .groups = {},
            });
    }
    auto provision_node(placement_group_id_type, std::optional<node_id_type>)
        -> kythira::Future<kythira::peer_info<node_id_type, address_type>> {
        return kythira::FutureFactory::makeExceptionalFuture<
            kythira::peer_info<node_id_type, address_type>>(std::runtime_error("not supported"));
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
    kythira::quorum_manager<adjustable_quorum_manager, std::uint64_t, std::string, std::string>);

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

    using quorum_manager_type = adjustable_quorum_manager;
};

using test_node = kythira::node<test_types>;
using sim_t = network_simulator::NetworkSimulator<test_types::raft_network_types>;

kythira::raft_configuration make_fast_config() {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{80};
    cfg._election_timeout_max = std::chrono::milliseconds{160};
    cfg._heartbeat_interval = std::chrono::milliseconds{26};
    cfg._rpc_timeout = std::chrono::milliseconds{200};
    return cfg;
}

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

// Admits `learner_id` on `leader`, driving heartbeats until the future resolves.
// Requires the leader's current topology to have room (voting+learner < target).
void admit_learner(test_node& leader, std::uint64_t learner_id) {
    auto fut = leader.add_learner(learner_id);
    for (int i = 0; i < 15; ++i) {
        leader.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
    bool done = false, threw = false;
    std::move(fut)
        .thenValue([&](std::vector<std::byte>) { done = true; })
        .thenError([&](const std::exception_ptr&) { threw = true; });
    BOOST_REQUIRE(wait_until([&] { return done || threw; }));
    BOOST_REQUIRE_MESSAGE(!threw, "learner admission unexpectedly failed");
}

}  // namespace

BOOST_AUTO_TEST_SUITE(learner_promotion_capacity)

// Requirement 3.2: promotion fails when voting-only count already meets/exceeds
// target, even though the learner was admitted while there was room (Property 3).
BOOST_AUTO_TEST_CASE(blocked_at_voting_target_despite_admitted_learner,
                     *boost::unit_test::timeout(20)) {
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    network_simulator::NetworkEdge edge{};
    sim.add_edge("1", "2", edge);
    sim.add_edge("2", "1", edge);
    auto cfg = make_fast_config();

    test_types::quorum_manager_type qm;
    *qm.topo =
        kythira::desired_topology<std::string>{.groups = {{.group_id = "", .target_count = 3}}};

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
        .quorum_manager = qm,
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
        .quorum_manager = adjustable_quorum_manager{},
    };

    test_node node1{std::move(ncfg1)};
    test_node node2{std::move(ncfg2)};
    node1.set_cluster_configuration({1});
    node2.set_cluster_configuration({2});
    node1.start();
    node2.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    // Generous topology (target 3, 1 voter) — admission succeeds.
    admit_learner(node1, 2);

    // Operator tightens the target to 1 — the single existing voter now already
    // meets it, so promotion must be blocked despite the learner being ready.
    *qm.topo =
        kythira::desired_topology<std::string>{.groups = {{.group_id = "", .target_count = 1}}};

    auto promote_fut = node1.promote_to_voter(2);
    bool threw = false;
    std::exception_ptr caught;
    std::move(promote_fut)
        .thenValue([&](std::vector<std::byte>) {})
        .thenError([&](const std::exception_ptr& ex) {
            threw = true;
            caught = ex;
        });
    BOOST_REQUIRE(wait_until([&] { return threw; }, std::chrono::milliseconds{2000}));
    BOOST_CHECK_THROW(std::rethrow_exception(caught), kythira::voting_capacity_exceeded_exception);
    BOOST_CHECK_EQUAL(node1.get_cluster_size(), 1u);  // still just the original voter

    node2.stop();
    node1.stop();
}

// Requirement 3.3: promotion succeeds when voting-only count is below target.
BOOST_AUTO_TEST_CASE(allowed_below_voting_target, *boost::unit_test::timeout(20)) {
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    network_simulator::NetworkEdge edge{};
    sim.add_edge("1", "2", edge);
    sim.add_edge("2", "1", edge);
    auto cfg = make_fast_config();

    test_types::quorum_manager_type qm;
    *qm.topo =
        kythira::desired_topology<std::string>{.groups = {{.group_id = "", .target_count = 3}}};

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
        .quorum_manager = qm,
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
        .quorum_manager = adjustable_quorum_manager{},
    };

    test_node node1{std::move(ncfg1)};
    test_node node2{std::move(ncfg2)};
    node1.set_cluster_configuration({1});
    node2.set_cluster_configuration({2});
    node1.start();
    node2.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    admit_learner(node1, 2);

    auto promote_fut = node1.promote_to_voter(2);
    for (int i = 0; i < 25; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
    bool done = false, threw = false;
    std::move(promote_fut)
        .thenValue([&](std::vector<std::byte>) { done = true; })
        .thenError([&](const std::exception_ptr&) { threw = true; });
    BOOST_REQUIRE(wait_until([&] { return done || threw; }));
    BOOST_CHECK(!threw);
    BOOST_CHECK_EQUAL(node1.get_cluster_size(), 2u);  // grew: learner promoted to voter

    node2.stop();
    node1.stop();
}

// Requirement 1.2: a group removed from desired_topology after admission fails
// closed at promotion time.
BOOST_AUTO_TEST_CASE(fails_closed_when_group_removed, *boost::unit_test::timeout(20)) {
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    network_simulator::NetworkEdge edge{};
    sim.add_edge("1", "2", edge);
    sim.add_edge("2", "1", edge);
    auto cfg = make_fast_config();

    test_types::quorum_manager_type qm;
    *qm.topo =
        kythira::desired_topology<std::string>{.groups = {{.group_id = "", .target_count = 3}}};

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
        .quorum_manager = qm,
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
        .quorum_manager = adjustable_quorum_manager{},
    };

    test_node node1{std::move(ncfg1)};
    test_node node2{std::move(ncfg2)};
    node1.set_cluster_configuration({1});
    node2.set_cluster_configuration({2});
    node1.start();
    node2.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    admit_learner(node1, 2);

    // The group is now entirely undeclared.
    *qm.topo = kythira::desired_topology<std::string>{.groups = {}};

    auto promote_fut = node1.promote_to_voter(2);
    bool threw = false;
    std::exception_ptr caught;
    std::move(promote_fut)
        .thenValue([&](std::vector<std::byte>) {})
        .thenError([&](const std::exception_ptr& ex) {
            threw = true;
            caught = ex;
        });
    BOOST_REQUIRE(wait_until([&] { return threw; }, std::chrono::milliseconds{2000}));
    BOOST_CHECK_THROW(std::rethrow_exception(caught), kythira::voting_capacity_exceeded_exception);

    node2.stop();
    node1.stop();
}

BOOST_AUTO_TEST_SUITE_END()
