// Tests for the learner admission capacity criterion:
// .kiro/specs/non-voting-nodes/requirements.md, Requirement 2.
//
// add_learner() must only admit a joining node when its placement group's
// current voting+learner count is below the group's desired_topology target;
// a group absent from desired_topology fails closed (no capacity).

#define BOOST_TEST_MODULE learner_admission_capacity_test
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("learner_admission_capacity_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {

template<typename NodeId, typename Address> class preset_peer_discovery {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    preset_peer_discovery() = default;
    auto register_node(NodeId, Address) -> kythira::Future<void> {
        return kythira::FutureFactory::makeFuture();
    }
    auto find_peers(std::chrono::milliseconds) const
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

    using quorum_manager_type =
        kythira::no_op_quorum_manager<node_id_type, address_type, std::string>;
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
        if (std::chrono::steady_clock::now() - start > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

kythira::node_config<test_types> make_node_config(std::uint64_t id, auto net,
                                                  const kythira::raft_configuration& cfg,
                                                  test_types::quorum_manager_type qm) {
    return kythira::node_config<test_types>{
        .node_id = id,
        .network_client = {net, test_types::serializer_type{}},
        .network_server = {net, test_types::serializer_type{}},
        .persistence = {},
        .logger = kythira::console_logger{},
        .metrics = {},
        .membership = {},
        .config = cfg,
        .self_address = std::to_string(id),
        .peer_discovery = preset_peer_discovery<std::uint64_t, std::string>{},
        .quorum_manager = std::move(qm),
    };
}

}  // namespace

BOOST_AUTO_TEST_SUITE(learner_admission_capacity)

// Requirement 2.2: admission fails when the group's voting+learner count already
// meets or exceeds target_count.
BOOST_AUTO_TEST_CASE(blocked_at_target, *boost::unit_test::timeout(15)) {
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto cfg = make_fast_config();

    // Single voter already occupies the only slot the desired topology declares.
    test_types::quorum_manager_type qm{
        kythira::desired_topology<std::string>{.groups = {{.group_id = "", .target_count = 1}}}};
    test_node node1{make_node_config(1, net1, cfg, std::move(qm))};
    node1.set_cluster_configuration({1});
    node1.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    auto fut = node1.add_learner(2);
    bool threw = false;
    std::exception_ptr caught;
    std::move(fut)
        .thenValue([&](std::vector<std::byte>) {})
        .thenError([&](const std::exception_ptr& ex) {
            threw = true;
            caught = ex;
        });
    BOOST_REQUIRE(wait_until([&] { return threw; }, std::chrono::milliseconds{2000}));
    BOOST_CHECK_THROW(std::rethrow_exception(caught), kythira::learner_capacity_exceeded_exception);

    node1.stop();
}

// Requirement 2.3: admission succeeds when voting+learner count is below target_count.
BOOST_AUTO_TEST_CASE(allowed_below_target, *boost::unit_test::timeout(15)) {
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    network_simulator::NetworkEdge edge{};
    sim.add_edge("1", "2", edge);
    sim.add_edge("2", "1", edge);
    auto cfg = make_fast_config();

    test_types::quorum_manager_type qm{
        kythira::desired_topology<std::string>{.groups = {{.group_id = "", .target_count = 5}}}};
    test_node node1{make_node_config(1, net1, cfg, std::move(qm))};
    test_node node2{make_node_config(2, net2, cfg, test_types::quorum_manager_type{})};
    node1.set_cluster_configuration({1});
    node2.set_cluster_configuration({2});
    node1.start();
    node2.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    auto fut = node1.add_learner(2);
    for (int i = 0; i < 15; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
    bool done = false, threw = false;
    std::move(fut)
        .thenValue([&](std::vector<std::byte>) { done = true; })
        .thenError([&](const std::exception_ptr&) { threw = true; });
    BOOST_REQUIRE(wait_until([&] { return done || threw; }));
    BOOST_CHECK(!threw);

    node2.stop();
    node1.stop();
}

// Requirement 1.2 / 2: a placement group absent from desired_topology fails closed.
BOOST_AUTO_TEST_CASE(fails_closed_on_undeclared_group, *boost::unit_test::timeout(15)) {
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto cfg = make_fast_config();

    // Empty topology: no group has a declared target at all.
    test_types::quorum_manager_type qm{kythira::desired_topology<std::string>{.groups = {}}};
    test_node node1{make_node_config(1, net1, cfg, std::move(qm))};
    node1.set_cluster_configuration({1});
    node1.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    auto fut = node1.add_learner(2);
    bool threw = false;
    std::exception_ptr caught;
    std::move(fut)
        .thenValue([&](std::vector<std::byte>) {})
        .thenError([&](const std::exception_ptr& ex) {
            threw = true;
            caught = ex;
        });
    BOOST_REQUIRE(wait_until([&] { return threw; }, std::chrono::milliseconds{2000}));
    BOOST_CHECK_THROW(std::rethrow_exception(caught), kythira::learner_capacity_exceeded_exception);

    node1.stop();
}

BOOST_AUTO_TEST_SUITE_END()
