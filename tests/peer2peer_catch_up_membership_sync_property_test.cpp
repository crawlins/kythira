// Property tests for .kiro/specs/peer2peer-log-replication/ Task 21
// (design.md Property 6, Requirements 11.1, 11.2, 11.3, 11.5).
// remove_server_revokes_catch_up_eligibility_immediately in the sibling
// peer2peer_catch_up_property_test.cpp already covers final post-completion
// exclusion of a removed node; this file covers what that one doesn't:
// immediate add-eligibility, the C_old/C_new *union* staying in effect
// while a removal is still mid-flight (not just the end state), and learner
// exclusion.
//
// Note on scope: Requirement 11.1 describes "both a C_old member being
// removed and a C_new member being added" being simultaneously eligible
// during one joint-consensus window. The real add_server()/remove_server()
// API only ever changes one side per call (classic single-server-change
// joint consensus, not arbitrary multi-node reconfiguration), and a second
// call while one is already in flight is rejected
// ("Configuration change already in progress") — so a literal simultaneous
// add+remove is not constructible through the public API. This file tests
// each half of the union separately instead: add eligibility (trivial side
// of the union, C_new already a superset) and the non-trivial removal side
// (old_nodes() keeping a departing node in the union after its own C_new
// has already dropped it).
#define BOOST_TEST_MODULE peer2peer_catch_up_membership_sync_property_test
#include <boost/test/unit_test.hpp>

#include <raft/quorum_management.hpp>
#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// ── Folly global fixture ───────────────────────────────────────────────────

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("peer2peer_catch_up_membership_sync_property_test"),
                         nullptr};
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

    using peer2peer_replicator_type =
        kythira::static_peer2peer_replicator<node_id_type, address_type, log_index_type>;

    // Generous, single-default-group topology so add_learner()'s placement-capacity
    // criterion never blocks the learner-exclusion test below — capacity itself is
    // exercised separately (learner_admission_capacity_test.cpp).
    using quorum_manager_type =
        kythira::no_op_quorum_manager<node_id_type, address_type, std::string>;
};

using test_node = kythira::node<test_types>;
using sim_t = network_simulator::NetworkSimulator<test_types::raft_network_types>;
using replicator_t = test_types::peer2peer_replicator_type;

template<typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{8000}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{15});
    }
    return true;
}

kythira::raft_configuration make_fast_config() {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{80};
    cfg._election_timeout_max = std::chrono::milliseconds{160};
    cfg._heartbeat_interval = std::chrono::milliseconds{20};
    cfg._rpc_timeout = std::chrono::milliseconds{200};
    cfg._progress_gossip_interval = std::chrono::milliseconds{25};
    cfg._catch_up_fetch_timeout = std::chrono::milliseconds{300};
    return cfg;
}

kythira::raft_configuration make_dormant_config() {
    kythira::raft_configuration cfg = make_fast_config();
    cfg._election_timeout_min = std::chrono::minutes{10};
    cfg._election_timeout_max = std::chrono::minutes{10} + std::chrono::milliseconds{1};
    return cfg;
}

test_types::quorum_manager_type generous_quorum_manager() {
    return test_types::quorum_manager_type{
        kythira::desired_topology<std::string>{.groups = {{.group_id = "", .target_count = 10}}}};
}

}  // namespace

BOOST_AUTO_TEST_SUITE(peer2peer_catch_up_membership_sync)

/**
 * Property (Requirement 11.1, 11.3): a node added via add_server() becomes
 * catch-up-eligible at the same synchronous point its configuration entry
 * takes effect on the leader — get_cluster_size() reads the same
 * _configuration that feeds cluster_members()/sync_peer2peer_membership(),
 * so this proves both update at once, not staggered.
 */
BOOST_AUTO_TEST_CASE(add_server_makes_new_node_immediately_eligible,
                     *boost::unit_test::timeout(30)) {
    auto table = std::make_shared<replicator_t::table_type>();
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    network_simulator::NetworkEdge edge{};
    sim.add_edge("1", "2", edge);
    sim.add_edge("2", "1", edge);

    auto cfg = make_fast_config();
    auto make_node = [&](std::uint64_t id, auto net) {
        return test_node{id,
                         {net, test_types::serializer_type{}},
                         {net, test_types::serializer_type{}},
                         {},
                         kythira::console_logger{},
                         {},
                         {},
                         cfg,
                         std::to_string(id),
                         preset_peer_discovery<std::uint64_t, std::string>{},
                         replicator_t{table}};
    };

    auto node1 = make_node(1, net1);
    node1.set_cluster_configuration({1});
    node1.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    BOOST_CHECK_EQUAL(node1.get_cluster_size(), 1u);
    node1.add_server(2);
    // No heartbeat pump — checked at the exact synchronous point add_server()
    // returns, proving eligibility does not wait for the change to commit.
    BOOST_CHECK_EQUAL(node1.get_cluster_size(), 2u);

    node1.stop();
}

/**
 * Property (Requirement 11.1, design.md Property 6): while a removal is
 * still mid-joint-consensus (never allowed to commit, by keeping the
 * departing C_new-required node network-unreachable), the departing node
 * remains a valid catch-up source via cluster_members()'s union with
 * old_nodes() — proven behaviorally: node2 fetches a later entry from node3
 * (the node being removed) that is only reachable *and* eligible because of
 * the union. Without the union, node3 would no longer be in node2's own
 * membership set (C_new = {1,2} excludes it) even though node3 remains the
 * only network-reachable, up-to-date peer node2 has.
 */
BOOST_AUTO_TEST_CASE(remove_server_union_keeps_departing_node_eligible_mid_flight,
                     *boost::unit_test::timeout(30)) {
    auto table = std::make_shared<replicator_t::table_type>();
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");
    network_simulator::NetworkEdge edge{};
    for (auto from : {"1", "2", "3"}) {
        for (auto to : {"1", "2", "3"}) {
            if (std::string(from) != std::string(to)) {
                sim.add_edge(from, to, edge);
            }
        }
    }

    auto cfg = make_fast_config();
    auto make_node = [&](std::uint64_t id, auto net) {
        return test_node{id,
                         {net, test_types::serializer_type{}},
                         {net, test_types::serializer_type{}},
                         {},
                         kythira::console_logger{},
                         {},
                         {},
                         cfg,
                         std::to_string(id),
                         preset_peer_discovery<std::uint64_t, std::string>{},
                         replicator_t{table}};
    };

    auto node1 = make_node(1, net1);
    auto node2 = make_node(2, net2);
    auto node3 = make_node(3, net3);

    node1.set_cluster_configuration({1, 2, 3});
    node2.set_cluster_configuration({1, 2, 3});
    node3.set_cluster_configuration({1, 2, 3});
    node1.start();
    node2.start();
    node3.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    auto pump_heartbeats = [&](int cycles) {
        for (int i = 0; i < cycles; ++i) {
            node1.check_heartbeat_timeout();
            std::this_thread::sleep_for(cfg._heartbeat_interval);
        }
    };

    // Real committed prefix, all three caught up.
    auto prefix_command =
        kythira::test_key_value_state_machine<std::uint64_t>::make_put_command("prefix", "v");
    try {
        node1.submit_command(prefix_command, std::chrono::milliseconds{500});
    } catch (...) {
    }
    pump_heartbeats(10);
    auto prefix_index = node1.debug_state().commit_index;
    BOOST_REQUIRE_GT(prefix_index, 0u);
    BOOST_REQUIRE(wait_until([&] { return node3.debug_state().last_applied >= prefix_index; }));
    BOOST_REQUIRE(wait_until([&] { return node2.debug_state().last_applied >= prefix_index; }));

    // Fully isolate node2 from *both* node1 and node3 — the simulator
    // routes multi-hop, so leaving node2<->node3 connected would let node3
    // relay node1's ordinary AppendEntries straight through, defeating the
    // isolation (and submit_command() itself replicates synchronously, not
    // just on the next heartbeat, so node2 must already be unreachable
    // before any further commands are submitted below).
    sim.remove_edge("1", "2");
    sim.remove_edge("2", "1");
    sim.remove_edge("2", "3");
    sim.remove_edge("3", "2");

    // Start removing node3 from the cluster. Node2 (a required C_new voter)
    // is unreachable, so this configuration change can never reach C_new
    // majority and commit — the joint window stays open indefinitely,
    // regardless of how many heartbeats follow.
    node1.remove_server(3);
    pump_heartbeats(10);  // node3 (still connected to node1) receives the joint config entry.
    BOOST_REQUIRE(wait_until([&] { return node3.debug_state().log.size() >= prefix_index + 1; }));

    // node1 appends one further entry (still can't commit — same blocker),
    // replicated to node3 via ordinary heartbeats while node1<->node3 is
    // still connected.
    auto later_command =
        kythira::test_key_value_state_machine<std::uint64_t>::make_put_command("later", "v");
    try {
        node1.submit_command(later_command, std::chrono::milliseconds{500});
    } catch (...) {
    }
    pump_heartbeats(10);
    auto later_index = node1.debug_state().log.size();
    BOOST_REQUIRE(wait_until([&] { return node3.debug_state().log.size() >= later_index; }));

    // Disconnect node1<->node3 too, then connect node2<->node3 — node2's
    // only possible path to anything is now node3, and node3 has no path
    // back to node1, so nothing can relay: this fetch is unambiguously
    // node2 pulling directly from node3.
    sim.remove_edge("1", "3");
    sim.remove_edge("3", "1");
    sim.add_edge("2", "3", edge);
    sim.add_edge("3", "2", edge);

    // node2 fetches both entries from node3 via peer-to-peer catch-up.
    // node2's own _configuration becomes the joint one upon receiving the
    // first (config) entry: nodes() = {1,2} (node3 already dropped from
    // C_new), old_nodes() = {1,2,3}. If cluster_members() did NOT union
    // with old_nodes(), node3 would no longer be in node2's own membership
    // set once that happens — and node2 has no other reachable node at
    // all — so the *second* fetch (for later_command) could never
    // succeed. Its success is only possible because of the union.
    BOOST_REQUIRE(wait_until(
        [&] {
            node3.check_election_timeout();
            node2.check_election_timeout();
            return node2.debug_state().log.size() >= later_index;
        },
        std::chrono::milliseconds{4000}));
    BOOST_CHECK(node2.debug_state().log.back().command() == later_command);

    node3.stop();
    node2.stop();
    node1.stop();
}

/**
 * Property (Requirement 11.2): a learner is never offered as a catch-up
 * source, regardless of how far its log has advanced — cluster_members()
 * unconditionally excludes learners(). Proven behaviorally, mirroring
 * peer2peer_catch_up_property_test.cpp's asymmetric-topology technique: a
 * lagging voting node reachable *only* through the learner does not
 * converge (proving exclusion), then does converge once given a real path
 * (proving the earlier non-convergence was exclusion, not a network fault).
 */
BOOST_AUTO_TEST_CASE(learner_never_offered_as_catch_up_source, *boost::unit_test::timeout(30)) {
    auto table = std::make_shared<replicator_t::table_type>();
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net4 = sim.create_node("4");  // the learner
    network_simulator::NetworkEdge edge{};
    sim.add_edge("1", "2", edge);
    sim.add_edge("2", "1", edge);
    sim.add_edge("1", "4", edge);
    sim.add_edge("4", "1", edge);
    sim.add_edge("2", "4", edge);
    sim.add_edge("4", "2", edge);

    auto cfg = make_fast_config();
    auto cfg_dormant = make_dormant_config();
    auto make_node = [&](std::uint64_t id, auto net, const kythira::raft_configuration& c) {
        return kythira::node_config<test_types>{
            .node_id = id,
            .network_client = {net, test_types::serializer_type{}},
            .network_server = {net, test_types::serializer_type{}},
            .persistence = {},
            .logger = kythira::console_logger{},
            .metrics = {},
            .membership = {},
            .config = c,
            .self_address = std::to_string(id),
            .peer_discovery = preset_peer_discovery<std::uint64_t, std::string>{},
            .quorum_manager = generous_quorum_manager(),
            .peer2peer_replicator = replicator_t{table},
        };
    };

    test_node node1{make_node(1, net1, cfg)};
    test_node node2{make_node(2, net2, cfg_dormant)};
    test_node node4{make_node(4, net4, cfg_dormant)};

    node1.set_cluster_configuration({1, 2});
    node2.set_cluster_configuration({1, 2});
    node4.set_cluster_configuration({4});

    node1.start();
    node2.start();
    node4.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    auto pump_heartbeats = [&](int cycles) {
        for (int i = 0; i < cycles; ++i) {
            node1.check_heartbeat_timeout();
            std::this_thread::sleep_for(cfg._heartbeat_interval);
        }
    };

    auto add_fut = node1.add_learner(4);
    pump_heartbeats(20);
    bool add_done = false;
    std::move(add_fut)
        .thenValue([&](std::vector<std::byte>) { add_done = true; })
        .thenError([&](const std::exception_ptr&) { add_done = true; });
    BOOST_REQUIRE(wait_until([&] { return add_done; }));
    // node4 is now a learner replicating from node1 like any other follower.

    // Commit a lot more, keeping node4 (the learner) fully caught up —
    // deliberately further ahead than node2 below, so if learners *were*
    // eligible sources node4 would be an obviously attractive one.
    for (int i = 0; i < 10; ++i) {
        auto command = kythira::test_key_value_state_machine<std::uint64_t>::make_put_command(
            "k" + std::to_string(i), "v");
        try {
            node1.submit_command(command, std::chrono::milliseconds{500});
        } catch (...) {
        }
        pump_heartbeats(1);
    }
    pump_heartbeats(15);
    auto leader_index = node1.debug_state().commit_index;
    BOOST_REQUIRE_GT(leader_index, 0u);
    BOOST_REQUIRE(wait_until([&] { return node4.debug_state().last_applied >= leader_index; }));

    // node2 falls behind — fully isolated (from *both* node1 and node4;
    // the simulator routes multi-hop, so leaving node2<->node4 connected
    // would let node4 relay node1's ordinary AppendEntries straight
    // through, defeating the isolation entirely since submit_command()
    // replicates synchronously).
    sim.remove_edge("1", "2");
    sim.remove_edge("2", "1");
    sim.remove_edge("2", "4");
    sim.remove_edge("4", "2");
    for (int i = 0; i < 5; ++i) {
        auto command = kythira::test_key_value_state_machine<std::uint64_t>::make_put_command(
            "later" + std::to_string(i), "v");
        try {
            node1.submit_command(command, std::chrono::milliseconds{500});
        } catch (...) {
        }
        pump_heartbeats(1);
    }
    pump_heartbeats(10);
    // log.size(), not commit_index: node2 (a required voter) is unreachable,
    // so these entries are appended to node1's own log immediately but can
    // never commit until node2 reconnects and acks, later below.
    auto final_index = node1.debug_state().log.size();
    BOOST_REQUIRE_GT(final_index, leader_index);

    // Disconnect node1<->node4 too, then reconnect node2<->node4 — node2's
    // only possible path to anything is now node4 (the learner), and node4
    // has no path back to node1, so nothing can relay. If a learner were a
    // valid catch-up source, node2 would converge here regardless.
    sim.remove_edge("1", "4");
    sim.remove_edge("4", "1");
    sim.add_edge("2", "4", edge);
    sim.add_edge("4", "2", edge);
    bool converged_via_learner = wait_until(
        [&] {
            node4.check_election_timeout();
            node2.check_election_timeout();
            return static_cast<std::size_t>(node2.debug_state().log.size()) >=
                   static_cast<std::size_t>(final_index);
        },
        std::chrono::milliseconds{1500});
    BOOST_CHECK(!converged_via_learner);

    // Now give node2 a real path (reconnect to node1) — proving the prior
    // non-convergence was genuine exclusion, not a broken test setup.
    sim.add_edge("1", "2", edge);
    sim.add_edge("2", "1", edge);
    BOOST_REQUIRE(wait_until(
        [&] {
            node1.check_heartbeat_timeout();
            node2.check_election_timeout();
            return node2.debug_state().last_applied >= final_index;
        },
        std::chrono::milliseconds{4000}));

    node4.stop();
    node2.stop();
    node1.stop();
}

BOOST_AUTO_TEST_SUITE_END()
