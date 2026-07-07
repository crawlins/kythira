// Tests for decoupling learner capacity from the voting target:
// .kiro/specs/non-voting-nodes/requirements.md, Requirement 2.4-2.6, 3.4.
//
// A group's voting target (target_count) and its learner population
// (learner_capacity) are sized independently: a group whose voting target is
// already fully met (e.g. 3-of-3) must still admit learners up to a
// separately-declared learner_capacity, and promotion must continue to
// enforce target_count regardless of how large learner_capacity is.
//
// Admission of a "learner" here only requires a majority of the REAL voters
// to acknowledge the (non-joint) configuration entry — the learner itself
// need not be a reachable node for the entry to commit, since learners are
// never counted toward quorum. This lets the test exercise the capacity
// mechanics with plain integer learner IDs, without spinning up thousands of
// node processes to demonstrate the "3 voters + many learners" scenario.

#define BOOST_TEST_MODULE learner_capacity_decoupling_test
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
        char* argv0[] = {const_cast<char*>("learner_capacity_decoupling_test"), nullptr};
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
    cfg._rpc_timeout = std::chrono::milliseconds{150};
    cfg._append_entries_retry_policy.max_attempts = 1;  // fail fast against phantom learner IDs
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

void connect_all(sim_t& sim, std::initializer_list<std::string> addresses) {
    network_simulator::NetworkEdge edge{};
    for (const auto& from : addresses) {
        for (const auto& to : addresses) {
            if (from != to) sim.add_edge(from, to, edge);
        }
    }
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

// Drives heartbeats on `leader` until `fut` resolves, reporting the outcome.
template<typename Fut>
void drive_to_completion(test_node& leader, Fut fut, bool& done, bool& threw,
                         std::exception_ptr* caught = nullptr) {
    std::move(fut)
        .thenValue([&](std::vector<std::byte>) { done = true; })
        .thenError([&, caught](const std::exception_ptr& ex) {
            threw = true;
            if (caught) *caught = ex;
        });
    for (int i = 0; i < 25 && !(done || threw); ++i) {
        leader.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(learner_capacity_decoupling)

// Requirement 2.4-2.6, 3.4: target_count = 3 already met by 3 real voters;
// learner_capacity set independently to 5. Learners admit up to
// learner_capacity despite the voting group being full; the 6th admission
// fails; promotion still enforces target_count, unaffected by learner_capacity.
BOOST_AUTO_TEST_CASE(learners_admit_past_full_voting_target_up_to_learner_capacity,
                     *boost::unit_test::timeout(30)) {
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");
    connect_all(sim, {"1", "2", "3"});
    auto cfg = make_fast_config();

    auto make_qm = [] {
        kythira::desired_topology<std::string> topo{
            .groups = {{.group_id = "", .target_count = 3, .learner_capacity = 5}}};
        return test_types::quorum_manager_type{topo};
    };

    test_node node1{make_node_config(1, net1, cfg, make_qm())};
    test_node node2{make_node_config(2, net2, cfg, make_qm())};
    test_node node3{make_node_config(3, net3, cfg, make_qm())};
    node1.set_cluster_configuration({1, 2, 3});
    node2.set_cluster_configuration({1, 2, 3});
    node3.set_cluster_configuration({1, 2, 3});
    node1.start();
    node2.start();
    node3.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));
    BOOST_REQUIRE_EQUAL(node1.get_cluster_size(), 3u);  // voting target already fully met

    // Admit 5 learners (IDs 10-14) — the voting group being full must not
    // block them, since learner_capacity (5) is declared independently.
    for (std::uint64_t learner_id = 10; learner_id < 15; ++learner_id) {
        auto fut = node1.add_learner(learner_id);
        bool done = false, threw = false;
        drive_to_completion(node1, std::move(fut), done, threw);
        BOOST_REQUIRE_MESSAGE(done,
                              "admission of learner " << learner_id << " unexpectedly failed");
    }

    // A 6th learner exceeds learner_capacity (5) and must be rejected.
    {
        auto fut = node1.add_learner(15);
        bool done = false, threw = false;
        std::exception_ptr caught;
        drive_to_completion(node1, std::move(fut), done, threw, &caught);
        BOOST_REQUIRE(threw);
        BOOST_CHECK_THROW(std::rethrow_exception(caught),
                          kythira::learner_capacity_exceeded_exception);
    }

    // Promotion still enforces target_count (3, already met) — unaffected by
    // learner_capacity being set to a much larger number.
    {
        auto fut = node1.promote_to_voter(10);
        bool done = false, threw = false;
        std::exception_ptr caught;
        drive_to_completion(node1, std::move(fut), done, threw, &caught);
        BOOST_REQUIRE(threw);
        BOOST_CHECK_THROW(std::rethrow_exception(caught),
                          kythira::voting_capacity_exceeded_exception);
    }
    BOOST_CHECK_EQUAL(node1.get_cluster_size(), 3u);  // still exactly 3 voters

    node3.stop();
    node2.stop();
    node1.stop();
}

BOOST_AUTO_TEST_SUITE_END()
