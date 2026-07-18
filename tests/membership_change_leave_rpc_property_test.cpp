#define BOOST_TEST_MODULE membership_change_leave_rpc_property_test
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
        char* argv0[] = {const_cast<char*>("membership_change_leave_rpc_property_test"), nullptr};
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
    explicit preset_peer_discovery(std::vector<kythira::peer_info<NodeId, Address>> peers)
        : _peers(std::move(peers)) {}
    auto register_node(NodeId, Address) -> kythira::Future<void> {
        return kythira::FutureFactory::makeFuture();
    }
    [[nodiscard]] auto find_peers(std::chrono::milliseconds) const
        -> kythira::Future<std::vector<kythira::peer_info<NodeId, Address>>> {
        return kythira::FutureFactory::makeFuture(
            std::vector<kythira::peer_info<NodeId, Address>>(_peers));
    }

private:
    std::vector<kythira::peer_info<NodeId, Address>> _peers;
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
    using cluster_leave_request_type = kythira::cluster_leave_request<node_id_type, address_type>;
    using cluster_leave_response_type = kythira::cluster_leave_response<node_id_type, address_type>;
};

using test_node = kythira::node<test_types>;
using sim_t = network_simulator::NetworkSimulator<test_types::raft_network_types>;

kythira::raft_configuration make_fast_config() {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{80};
    cfg._election_timeout_max = std::chrono::milliseconds{160};
    cfg._heartbeat_interval = std::chrono::milliseconds{26};
    cfg._rpc_timeout = std::chrono::milliseconds{200};
    cfg._bootstrap_retry_interval = std::chrono::milliseconds{200};
    cfg._bootstrap_peer_find_timeout = std::chrono::milliseconds{100};
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

}  // namespace

BOOST_AUTO_TEST_SUITE(leave_rpc_properties)

/**
 * Property: a node calling leave_cluster() sends a ClusterLeave RPC to the leader,
 * the leader calls remove_server, and the cluster shrinks from 3 to 2.
 */
BOOST_AUTO_TEST_CASE(leave_cluster_via_rpc_shrinks_cluster, *boost::unit_test::timeout(30)) {
    for (int iteration = 0; iteration < 3; ++iteration) {
        sim_t sim;
        sim.start();

        auto net1 = sim.create_node("1");
        auto net2 = sim.create_node("2");
        auto net3 = sim.create_node("3");
        connect_all(sim, {"1", "2", "3"});

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
                             preset_peer_discovery<std::uint64_t, std::string>{}};
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

        // Elect node1 and let heartbeats propagate so followers learn the leader
        std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
        node1.check_election_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{300});

        BOOST_REQUIRE_MESSAGE(
            wait_until([&] { return node1.is_leader(); }, std::chrono::milliseconds{4000}),
            "iteration " + std::to_string(iteration) + ": node1 did not become leader");

        // Drive a few heartbeats so followers learn the leader address before leave_cluster()
        for (int i = 0; i < 5; ++i) {
            node1.check_heartbeat_timeout();
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
        }

        // node3 self-removes via the ClusterLeave RPC path
        node3.leave_cluster(std::chrono::milliseconds{2000});

        // Drive heartbeats to commit C_joint then C_new
        for (int i = 0; i < 20; ++i) {
            node1.check_heartbeat_timeout();
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
        }

        // Property: cluster shrank to 2
        BOOST_CHECK_MESSAGE(
            wait_until([&] { return node1.get_cluster_size() == 2u; },
                       std::chrono::milliseconds{4000}),
            "iteration " + std::to_string(iteration) + ": cluster did not shrink to 2");

        node3.stop();
        node2.stop();
        node1.stop();
    }
}

/**
 * Property: after a node leaves via the ClusterLeave RPC, the remaining 2-node cluster
 * can still commit a command.
 */
BOOST_AUTO_TEST_CASE(cluster_operable_after_leave_rpc, *boost::unit_test::timeout(30)) {
    sim_t sim;
    sim.start();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");
    connect_all(sim, {"1", "2", "3"});

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
                         preset_peer_discovery<std::uint64_t, std::string>{}};
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
    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }, std::chrono::milliseconds{4000}));

    // Drive a few heartbeats so followers learn the leader address before leave_cluster()
    for (int i = 0; i < 5; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }

    // node3 self-removes via the ClusterLeave RPC path
    node3.leave_cluster(std::chrono::milliseconds{2000});

    // Drive heartbeats to commit C_new
    for (int i = 0; i < 25; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }

    BOOST_REQUIRE(wait_until([&] { return node1.get_cluster_size() == 2u; },
                             std::chrono::milliseconds{5000}));

    // Submit a command to the 2-node cluster
    using sm_t = kythira::test_key_value_state_machine<test_types::log_index_type>;
    auto cmd = sm_t::make_put_command("after_leave", "ok");
    auto cmd_fut = node1.submit_command(cmd, std::chrono::milliseconds{3000});

    bool cmd_applied = false;
    std::move(cmd_fut)
        .thenValue([&](std::vector<std::byte>) { cmd_applied = true; })
        .thenError([&](const std::exception_ptr&) {});

    for (int i = 0; i < 15; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }

    BOOST_CHECK(wait_until([&] { return cmd_applied; }, std::chrono::milliseconds{3000}));

    // Both remaining nodes applied the same entries
    BOOST_CHECK_EQUAL(node1.debug_state().last_applied, node2.debug_state().last_applied);

    node3.stop();
    node2.stop();
    node1.stop();
}

/**
 * Property: leave_cluster() is a silent no-op when no leader is known.
 * Exercises the early-return path inside leave_cluster() that fires when
 * _known_leader has not yet been set (e.g. a brand-new follower that has
 * never received an AppendEntries from a leader).
 */
BOOST_AUTO_TEST_CASE(leave_cluster_noop_when_no_leader_known, *boost::unit_test::timeout(10)) {
    sim_t sim;
    sim.start();

    auto net1 = sim.create_node("1");
    auto cfg = make_fast_config();

    test_node node1{1,
                    {net1, test_types::serializer_type{}},
                    {net1, test_types::serializer_type{}},
                    {},
                    kythira::console_logger{},
                    {},
                    {},
                    cfg,
                    "1",
                    preset_peer_discovery<std::uint64_t, std::string>{}};

    node1.set_cluster_configuration({1});
    node1.start();

    // No heartbeats driven — _known_leader is not set.
    // leave_cluster() must return immediately without throwing or blocking.
    BOOST_CHECK_NO_THROW(node1.leave_cluster(std::chrono::milliseconds{200}));

    node1.stop();
}

/**
 * Property: leave_cluster() follows a redirect from a stale non-leader to the real leader.
 *
 * Setup: node2 becomes leader first, so node3._known_leader = 2.  Then node1 wins a new
 * election with the node1→node3 edge removed so that neither the post-election heartbeat
 * nor node1's become_leader() touch node3._known_leader (there is no background heartbeat
 * timer — only explicit check_heartbeat_timeout() calls send heartbeats).
 *
 * After one manual heartbeat from node1 to node2 only (confirming node2._known_leader = 1)
 * all edges are restored and node3.leave_cluster() runs: it sends to "2" (stale), receives
 * a redirect to "1", follows it, and the cluster shrinks.  This exercises the redirect block
 * inside leave_cluster() and the non-leader branch of handle_cluster_leave.
 */
BOOST_AUTO_TEST_CASE(leave_cluster_via_stale_leader_uses_redirect, *boost::unit_test::timeout(30)) {
    sim_t sim;
    sim.start();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");
    connect_all(sim, {"1", "2", "3"});

    auto cfg = make_fast_config();  // 26ms heartbeat interval — check_heartbeat_timeout fires

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
                         preset_peer_discovery<std::uint64_t, std::string>{}};
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

    // Phase 1: make node2 the initial leader
    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node2.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node2.is_leader(); }, std::chrono::milliseconds{4000}));

    // Phase 2: drive heartbeats from node2 → node3._known_leader = 2
    // (elapsed > 26ms between each call, so check_heartbeat_timeout() actually fires)
    for (int i = 0; i < 3; ++i) {
        node2.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{60});
    }

    // Phase 3: remove edges so that:
    //   • node2 can no longer heartbeat node1/node3 (no stale leader-change)
    //   • node3 cannot start its own election (would win with only itself on node2's term)
    //   • node1→node3 is gone so node1's post-election heartbeat won't touch node3._known_leader
    // There is NO auto-heartbeat thread; only explicit check_heartbeat_timeout() calls fire.
    network_simulator::NetworkEdge edge{};
    sim.remove_edge("2", "1");
    sim.remove_edge("2", "3");
    sim.remove_edge("3", "1");
    sim.remove_edge("3", "2");
    sim.remove_edge("1", "3");

    // Phase 4: wait for node1's election timer to expire, then re-add node2→node1 so that
    // node2 can return its vote response to node1.
    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    sim.add_edge("2", "1", edge);

    // node1 wins with itself + node2 (2/3 majority); node3 never receives RequestVote.
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }, std::chrono::milliseconds{4000}));

    // Phase 5: drive one heartbeat from node1 → node2 only (node1→node3 still absent).
    // Sleep first so that elapsed > 26ms since become_leader() reset _last_heartbeat.
    std::this_thread::sleep_for(std::chrono::milliseconds{60});
    node1.check_heartbeat_timeout();  // node2._known_leader = 1, node3 untouched
    std::this_thread::sleep_for(std::chrono::milliseconds{100});  // let node2 process

    // Phase 6: restore all edges.  node3._known_leader is still 2 (stale) because no
    // AppendEntries from node1 ever reached node3.
    sim.add_edge("1", "3", edge);
    sim.add_edge("3", "1", edge);
    sim.add_edge("2", "3", edge);
    sim.add_edge("3", "2", edge);

    // leave_cluster() reads _known_leader = 2 → sends to "2" → redirect to "1" → accepted.
    // No auto-heartbeats can race here because the raft node has no background timer thread.
    node3.leave_cluster(std::chrono::milliseconds{3000});

    // Drive heartbeats to commit C_joint then C_new
    for (int i = 0; i < 20; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{60});
    }

    BOOST_CHECK(wait_until([&] { return node1.get_cluster_size() == 2u; },
                           std::chrono::milliseconds{5000}));

    node3.stop();
    node2.stop();
    node1.stop();
}

/**
 * Property: leave_cluster() catches RPC failures and returns without throwing.
 * Exercises the catch(std::exception) block inside leave_cluster() by stopping
 * the leader after the follower has set _known_leader, then calling leave_cluster()
 * so the RPC times out and the exception is silently swallowed.
 */
BOOST_AUTO_TEST_CASE(leave_cluster_rpc_failure_is_handled, *boost::unit_test::timeout(20)) {
    sim_t sim;
    sim.start();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");
    connect_all(sim, {"1", "2", "3"});

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
                         preset_peer_discovery<std::uint64_t, std::string>{}};
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
    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }, std::chrono::milliseconds{4000}));

    // Drive heartbeats so node3 learns node1 is the leader (_known_leader = 1)
    for (int i = 0; i < 5; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }

    // Stop node1 — its server no longer processes messages
    node1.stop();

    // node3 still thinks node1 is the leader; the RPC will time out and be caught
    BOOST_CHECK_NO_THROW(node3.leave_cluster(std::chrono::milliseconds{300}));

    node3.stop();
    node2.stop();
}

/**
 * Property: handle_cluster_leave() on a non-leader returns a redirect to the
 * known leader.  Exercises the non-leader branch (is_leader_now == false, redirect set)
 * of handle_cluster_leave() by sending a ClusterLeave RPC directly to node2 (a
 * follower that has received at least one heartbeat from node1 and knows the leader).
 */
BOOST_AUTO_TEST_CASE(leave_rpc_to_follower_returns_redirect, *boost::unit_test::timeout(20)) {
    sim_t sim;
    sim.start();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");
    connect_all(sim, {"1", "2", "3"});

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
                         preset_peer_discovery<std::uint64_t, std::string>{}};
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
    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }, std::chrono::milliseconds{4000}));

    // Drive heartbeats so node2 learns node1 is the leader (_known_leader = 1)
    for (int i = 0; i < 5; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }

    // Create a standalone client connected to the simulator — no raft node, just an RPC sender
    auto standalone_net = sim.create_node("standalone");
    network_simulator::NetworkEdge standalone_edge{};
    sim.add_edge("standalone", "2", standalone_edge);
    sim.add_edge("2", "standalone", standalone_edge);

    test_types::network_client_type standalone_client{standalone_net,
                                                      test_types::serializer_type{}};

    // Send ClusterLeave to node2 (a follower) — it must redirect to node1
    kythira::cluster_leave_request<std::uint64_t, std::string> req;
    req.node_id = 99;  // arbitrary departing node_id

    auto resp =
        standalone_client.send_cluster_leave_request("2", req, std::chrono::milliseconds{2000})
            .get();

    BOOST_CHECK(!resp.is_accepted());
    BOOST_REQUIRE(resp.redirect_peer().has_value());
    BOOST_CHECK_EQUAL(resp.redirect_peer()->node_id, 1u);

    node3.stop();
    node2.stop();
    node1.stop();
}

BOOST_AUTO_TEST_SUITE_END()
