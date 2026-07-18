// Tests for the "blocked learner remains active and is reconsidered later"
// property: .kiro/specs/non-voting-nodes/requirements.md, Requirement 5.
//
// A learner blocked by the promotion capacity criterion is never removed or
// otherwise altered — it becomes eligible again the moment its OWN placement
// group's voting count drops below target, with no re-admission step, and a
// vacancy opening in a DIFFERENT group must not affect it.

#define BOOST_TEST_MODULE learner_promotion_retry_test
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
        char* argv0[] = {const_cast<char*>("learner_promotion_retry_test"), nullptr};
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
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

void connect_all(sim_t& sim, std::initializer_list<std::string> addresses) {
    network_simulator::NetworkEdge edge{};
    for (const auto& from : addresses) {
        for (const auto& to : addresses) {
            if (from != to) {
                sim.add_edge(from, to, edge);
            }
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

// Drives heartbeats on `leader` until `fut` resolves (success or exception),
// recording the outcome in `done`/`threw` (and the exception in `*caught`, if given).
template<typename Fut>
void drive_to_completion(test_node& leader, Fut fut, bool& done, bool& threw,
                         std::exception_ptr* caught = nullptr) {
    std::move(fut)
        .thenValue([&](std::vector<std::byte>) { done = true; })
        .thenError([&, caught](const std::exception_ptr& ex) {
            threw = true;
            if (caught) {
                *caught = ex;
            }
        });
    for (int i = 0; i < 25 && !(done || threw); ++i) {
        leader.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(learner_promotion_retry)

// Requirements 5.1-5.4: a learner blocked by capacity stays put and becomes
// eligible the moment its own group's voting count drops below target.
//
// node1: leader. node2: the learner under test. node3: a "filler" voter added
// via add_server() (uncapacitated) purely to consume the group's remaining
// target slot, then removed again to open the vacancy node2 should benefit from.
BOOST_AUTO_TEST_CASE(blocked_learner_promotes_after_vacancy_in_same_group,
                     *boost::unit_test::timeout(30)) {
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");
    connect_all(sim, {"1", "2", "3"});
    auto cfg = make_fast_config();

    auto make_qm = [] {
        return test_types::quorum_manager_type{kythira::desired_topology<std::string>{
            .groups = {{.group_id = "", .target_count = 2}}}};
    };

    test_node node1{make_node_config(1, net1, cfg, make_qm())};
    test_node node2{make_node_config(2, net2, cfg, make_qm())};
    test_node node3{make_node_config(3, net3, cfg, make_qm())};
    node1.set_cluster_configuration({1});
    node2.set_cluster_configuration({2});
    node3.set_cluster_configuration({3});
    node1.start();
    node2.start();
    node3.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    // Target 2, 1 voter (node1) — room for exactly one more. Admit node2 as a
    // learner (voting+learner = 1 < 2).
    auto add2_fut = node1.add_learner(2);
    bool add2_done = false, add2_threw = false;
    drive_to_completion(node1, std::move(add2_fut), add2_done, add2_threw);
    BOOST_REQUIRE(add2_done);
    BOOST_REQUIRE_EQUAL(node1.get_cluster_size(), 1u);

    // Fill the group's remaining slot with a real voter via add_server() (no
    // capacity criterion applies to it) — this is what should block node2.
    auto add3_fut = node1.add_server(3);
    bool add3_done = false, add3_threw = false;
    drive_to_completion(node1, std::move(add3_fut), add3_done, add3_threw);
    BOOST_REQUIRE(add3_done);
    BOOST_REQUIRE_EQUAL(node1.get_cluster_size(), 2u);  // voters {1,3}, target now met

    // node2 is blocked: group already at target (2 voters).
    {
        auto promote2_fut = node1.promote_to_voter(2);
        bool done = false, threw = false;
        std::exception_ptr caught;
        drive_to_completion(node1, std::move(promote2_fut), done, threw, &caught);
        BOOST_REQUIRE(threw);
        BOOST_CHECK_THROW(std::rethrow_exception(caught),
                          kythira::voting_capacity_exceeded_exception);
    }
    BOOST_CHECK_EQUAL(node1.get_cluster_size(), 2u);  // node2 untouched — still just a learner

    // Open a vacancy in the SAME group by removing the filler voter.
    auto remove3_fut = node1.remove_server(3);
    bool remove3_done = false, remove3_threw = false;
    drive_to_completion(node1, std::move(remove3_fut), remove3_done, remove3_threw);
    BOOST_REQUIRE(remove3_done);
    BOOST_REQUIRE_EQUAL(node1.get_cluster_size(), 1u);

    // node2 — the same, previously-blocked learner — is now promotable with no
    // re-admission step.
    auto promote2_fut = node1.promote_to_voter(2);
    bool done = false, threw = false;
    drive_to_completion(node1, std::move(promote2_fut), done, threw);
    BOOST_CHECK(done);
    BOOST_CHECK(!threw);
    BOOST_CHECK_EQUAL(node1.get_cluster_size(), 2u);

    node3.stop();
    node2.stop();
    node1.stop();
}

// Requirement 5.5: a vacancy in a DIFFERENT placement group does not make a
// blocked learner eligible.
//
// Group A: node1 (leader) + node4 (filler voter, fills A's remaining slot).
// Group B: node3 (filler voter, later removed to open a vacancy in B only).
// node2: the learner under test, placed in group A.
BOOST_AUTO_TEST_CASE(vacancy_in_different_group_does_not_help, *boost::unit_test::timeout(30)) {
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");
    auto net4 = sim.create_node("4");
    connect_all(sim, {"1", "2", "3", "4"});
    auto cfg = make_fast_config();

    auto make_qm = [] {
        return test_types::quorum_manager_type{kythira::desired_topology<std::string>{
            .groups = {{.group_id = "A", .target_count = 2},
                       {.group_id = "B", .target_count = 1}}}};
    };

    test_node node1{make_node_config(1, net1, cfg, make_qm())};
    test_node node2{make_node_config(2, net2, cfg, make_qm())};
    test_node node3{make_node_config(3, net3, cfg, make_qm())};
    test_node node4{make_node_config(4, net4, cfg, make_qm())};
    node1.set_cluster_configuration({1});
    node2.set_cluster_configuration({2});
    node3.set_cluster_configuration({3});
    node4.set_cluster_configuration({4});
    node1.set_placement(1, "A");
    node1.set_placement(2, "A");  // learner-to-be, group A
    node1.set_placement(4, "A");  // filler voter, group A
    node1.set_placement(3, "B");  // filler voter, group B
    node1.start();
    node2.start();
    node3.start();
    node4.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    // Group A target 2, 1 voter (node1) — room for node2 to join as a learner.
    auto add2_fut = node1.add_learner(2);
    bool add2_done = false, add2_threw = false;
    drive_to_completion(node1, std::move(add2_fut), add2_done, add2_threw);
    BOOST_REQUIRE(add2_done);

    // Fill group A's remaining slot with node4 (a real voter) — this blocks node2.
    auto add4_fut = node1.add_server(4);
    bool add4_done = false, add4_threw = false;
    drive_to_completion(node1, std::move(add4_fut), add4_done, add4_threw);
    BOOST_REQUIRE(add4_done);

    // Fill group B's only slot with node3.
    auto add3_fut = node1.add_server(3);
    bool add3_done = false, add3_threw = false;
    drive_to_completion(node1, std::move(add3_fut), add3_done, add3_threw);
    BOOST_REQUIRE(add3_done);
    BOOST_REQUIRE_EQUAL(node1.get_cluster_size(), 3u);  // voters {1,4,3}: A={1,4}, B={3}

    // node2 (group A) is blocked: group A already at target (2 voters: 1, 4).
    {
        auto promote2_fut = node1.promote_to_voter(2);
        bool done = false, threw = false;
        std::exception_ptr caught;
        drive_to_completion(node1, std::move(promote2_fut), done, threw, &caught);
        BOOST_REQUIRE(threw);
        BOOST_CHECK_THROW(std::rethrow_exception(caught),
                          kythira::voting_capacity_exceeded_exception);
    }

    // Open a vacancy in group B (remove node3) — a DIFFERENT group from node2's (A).
    auto remove3_fut = node1.remove_server(3);
    bool remove3_done = false, remove3_threw = false;
    drive_to_completion(node1, std::move(remove3_fut), remove3_done, remove3_threw);
    BOOST_REQUIRE(remove3_done);
    BOOST_REQUIRE_EQUAL(node1.get_cluster_size(),
                        2u);  // A={1,4} unaffected, B={} — vacancy in B only

    // node2 (group A) must STILL be blocked — the vacancy is in group B, not A.
    auto promote2_fut = node1.promote_to_voter(2);
    bool done = false, threw = false;
    std::exception_ptr caught;
    drive_to_completion(node1, std::move(promote2_fut), done, threw, &caught);
    BOOST_CHECK(threw);
    BOOST_CHECK_THROW(std::rethrow_exception(caught), kythira::voting_capacity_exceeded_exception);
    BOOST_CHECK_EQUAL(node1.get_cluster_size(), 2u);

    node4.stop();
    node3.stop();
    node2.stop();
    node1.stop();
}

BOOST_AUTO_TEST_SUITE_END()
