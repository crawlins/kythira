#define BOOST_TEST_MODULE bootstrap_property_test
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

// ── Folly global fixture ───────────────────────────────────────────────────

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("bootstrap_property_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

// ── Shared types ───────────────────────────────────────────────────────────

namespace {

// Configurable-list peer discovery — default-constructible (empty peers = no-op)
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

static_assert(kythira::peer_discovery<preset_peer_discovery<std::uint64_t, std::string>,
                                      std::uint64_t, std::string>);

// Type bundle used for all bootstrap property tests
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
};

using test_node = kythira::node<test_types>;
using pi = kythira::peer_info<std::uint64_t, std::string>;
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
bool wait_until(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{3000}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

// Fully connect a set of simulator nodes (bidirectional, zero latency)
void connect_all(sim_t& sim, std::initializer_list<std::string> addresses) {
    network_simulator::NetworkEdge edge{};  // zero latency, 100% reliability
    for (const auto& from : addresses) {
        for (const auto& to : addresses) {
            if (from != to) {
                sim.add_edge(from, to, edge);
            }
        }
    }
}

}  // namespace

// ── Property tests ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(bootstrap_join_properties)

/**
 * Property: a fresh node with peer_discovery pointing directly at a leader
 * completes start() (ClusterJoin accepted) within a short deadline.
 *
 * Setup: node 1 is a single-node cluster and elected leader; node 4 is fresh
 * with preset_peer_discovery returning node 1's address.
 */
BOOST_AUTO_TEST_CASE(fresh_node_joins_single_node_leader, *boost::unit_test::timeout(30)) {
    sim_t sim;
    sim.start();

    auto net1 = sim.create_node("1");
    auto net4 = sim.create_node("4");
    connect_all(sim, {"1", "4"});
    auto cfg = make_fast_config();

    // Node 1: single-node cluster, no peer discovery
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
    node1.start();

    // Drive node1 to become leader
    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    // Node 4: fresh, peer discovery returns node1
    test_node node4{4,
                    {net4, test_types::serializer_type{}},
                    {net4, test_types::serializer_type{}},
                    {},
                    kythira::console_logger{},
                    {},
                    {},
                    cfg,
                    "4",
                    preset_peer_discovery<std::uint64_t, std::string>{{pi{1, "1"}}}};

    // start() blocks until bootstrap completes — run it in a background thread
    std::promise<void> done;
    auto done_fut = done.get_future();
    std::thread t([&node4, &done] {
        node4.start();
        done.set_value();
    });

    // Property: fresh node joins within a reasonable deadline
    auto status = done_fut.wait_for(std::chrono::seconds{10});
    BOOST_CHECK(status == std::future_status::ready);

    node4.stop();
    node1.stop();
    t.join();
}

/**
 * Property: a fresh node whose first peer is a follower that knows the leader
 * follows the redirect and joins via the leader.
 *
 * Setup: 3-node cluster {1,2,3} with node 1 as leader; node 4 bootstraps
 * by contacting node 2 (follower) first.
 */
BOOST_AUTO_TEST_CASE(fresh_node_follows_redirect_to_leader, *boost::unit_test::timeout(30)) {
    sim_t sim;
    sim.start();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");
    auto net4 = sim.create_node("4");
    connect_all(sim, {"1", "2", "3", "4"});
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

    // Configure 3-node cluster before starting
    node1.set_cluster_configuration({1, 2, 3});
    node2.set_cluster_configuration({1, 2, 3});
    node3.set_cluster_configuration({1, 2, 3});

    node1.start();
    node2.start();
    node3.start();

    // Drive node 1 to become leader of the 3-node cluster
    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }, std::chrono::milliseconds{3000}));

    // Send a heartbeat so node 2 learns who the leader is
    node1.check_heartbeat_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    // Node 4: fresh, peer discovery returns node 2 (follower)
    test_node node4{4,
                    {net4, test_types::serializer_type{}},
                    {net4, test_types::serializer_type{}},
                    {},
                    kythira::console_logger{},
                    {},
                    {},
                    cfg,
                    "4",
                    preset_peer_discovery<std::uint64_t, std::string>{{pi{2, "2"}}}};

    std::promise<void> done;
    auto done_fut = done.get_future();
    std::thread t([&node4, &done] {
        node4.start();
        done.set_value();
    });

    // Property: node 4 follows redirect from node 2 to node 1 and joins
    auto status = done_fut.wait_for(std::chrono::seconds{10});
    BOOST_CHECK(status == std::future_status::ready);

    node4.stop();
    node1.stop();
    node2.stop();
    node3.stop();
    t.join();
}

/**
 * Property: a fresh node with empty peer discovery founds a new single-node
 * cluster immediately (start() does not block).
 */
BOOST_AUTO_TEST_CASE(empty_peers_founds_single_node_cluster, *boost::unit_test::timeout(10)) {
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

    BOOST_CHECK_NO_THROW(node1.start());
    node1.stop();
}

/**
 * Property: a fresh node whose peer is temporarily unreachable enters the retry
 * loop, and completes bootstrap once the network partition heals.
 *
 * Setup: node 1 is a single-node leader; node 4 is fresh but its route to
 * node 1 is blocked.  After ~200 ms the partition is healed and node 4 joins.
 */
BOOST_AUTO_TEST_CASE(retry_loop_on_unreachable_peer, *boost::unit_test::timeout(30)) {
    sim_t sim;
    sim.start();

    auto net1 = sim.create_node("1");
    auto net4 = sim.create_node("4");
    // Connect node 1 to itself only — node 4 cannot reach node 1 yet.
    {
        network_simulator::NetworkEdge e{};
        sim.add_edge("1", "1", e);
    }
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
    node1.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    test_node node4{4,
                    {net4, test_types::serializer_type{}},
                    {net4, test_types::serializer_type{}},
                    {},
                    kythira::console_logger{},
                    {},
                    {},
                    cfg,
                    "4",
                    preset_peer_discovery<std::uint64_t, std::string>{{pi{1, "1"}}}};

    std::promise<void> done;
    auto done_fut = done.get_future();
    std::thread t([&node4, &done] {
        node4.start();
        done.set_value();
    });

    // While the network is partitioned start() must not complete.
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    BOOST_CHECK(done_fut.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready);

    // Heal the partition — node 4 can now reach node 1.
    network_simulator::NetworkEdge edge{};
    sim.add_edge("4", "1", edge);
    sim.add_edge("1", "4", edge);

    // Property: bootstrap completes promptly after connectivity is restored.
    auto status = done_fut.wait_for(std::chrono::seconds{8});
    BOOST_CHECK(status == std::future_status::ready);

    node4.stop();
    node1.stop();
    t.join();
}

/**
 * Property: a restarting node (non-fresh, term > 0) uses run_reconnect() and
 * NOT run_bootstrap() — it does not send a ClusterJoin request and does not
 * trigger a new add_server() on the leader.
 *
 * Verification: the cluster configuration stays at 3 nodes throughout, and
 * the leader's term does not increase (no extra election caused by a spurious
 * membership change).
 */
BOOST_AUTO_TEST_CASE(restarting_node_reconnects_without_join, *boost::unit_test::timeout(30)) {
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
    {
        auto node3_orig = make_node(3, net3);
        node1.set_cluster_configuration({1, 2, 3});
        node2.set_cluster_configuration({1, 2, 3});
        node3_orig.set_cluster_configuration({1, 2, 3});

        node1.start();
        node2.start();
        node3_orig.start();

        std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
        node1.check_election_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        BOOST_REQUIRE(
            wait_until([&] { return node1.is_leader(); }, std::chrono::milliseconds{3000}));

        node3_orig.stop();
        // node3_orig destructs here; its term (≥ 1) is lost from memory
    }

    auto leader_term = node1.get_current_term();
    BOOST_REQUIRE_GE(leader_term, 1u);

    // Simulate node 3 restarting: seed its persistence with the term it would
    // have observed while running as a cluster member (term ≥ 1 → not fresh).
    test_types::persistence_engine_type persistence3;
    persistence3.save_current_term(leader_term);

    // node3 restarts with peer discovery — goes through run_reconnect(), not run_bootstrap()
    test_node node3_restart{
        3,
        {net3, test_types::serializer_type{}},
        {net3, test_types::serializer_type{}},
        std::move(persistence3),
        kythira::console_logger{},
        {},
        {},
        cfg,
        "3",
        preset_peer_discovery<std::uint64_t, std::string>{{pi{1, "1"}, pi{2, "2"}}}};
    node3_restart.set_cluster_configuration({1, 2, 3});

    // start() returns immediately for restarting nodes (run_reconnect is background)
    BOOST_CHECK_NO_THROW(node3_restart.start());

    // Trigger heartbeat so node 3 receives AppendEntries and run_reconnect exits
    node1.check_heartbeat_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    // Property: cluster configuration did not grow (no ClusterJoin was processed)
    BOOST_CHECK_EQUAL(node1.get_cluster_size(), 3u);
    BOOST_CHECK_EQUAL(node3_restart.get_cluster_size(), 3u);

    // Property: leader term is unchanged (no disruption from restarting node)
    BOOST_CHECK_EQUAL(node1.get_current_term(), leader_term);

    node3_restart.stop();
    node1.stop();
    node2.stop();
}

/**
 * Property: an isolated restarting node with no peer discovery cannot elect
 * itself leader of a new single-node cluster.
 *
 * Even if check_election_timeout() is called, the node knows it is in a
 * 3-node cluster and cannot win an election without quorum.
 */
BOOST_AUTO_TEST_CASE(isolated_restarting_node_stays_follower, *boost::unit_test::timeout(10)) {
    sim_t sim;
    sim.start();
    auto net3 = sim.create_node("3");
    // No edges — node 3 is completely isolated
    auto cfg = make_fast_config();

    test_types::persistence_engine_type persistence3;
    persistence3.save_current_term(1);  // non-fresh: had term 1 in a prior run

    test_node node3{3,
                    {net3, test_types::serializer_type{}},
                    {net3, test_types::serializer_type{}},
                    std::move(persistence3),
                    kythira::console_logger{},
                    {},
                    {},
                    cfg,
                    "3",
                    preset_peer_discovery<std::uint64_t, std::string>{}};  // no_op effectively
    node3.set_cluster_configuration({1, 2, 3});
    node3.start();

    // Give the reconnect loop time to run at least one isolation-timeout cycle.
    // Because run_reconnect() runs in the background and no peers respond,
    // node 3 will keep waiting — but it will never call run_bootstrap() and
    // therefore never founds a new single-node cluster.
    std::this_thread::sleep_for(cfg._election_timeout_max * 3);

    // Property: node 3 is NOT a leader — it skipped run_bootstrap() entirely
    // because term=1 means is_fresh_node() == false.
    BOOST_CHECK(!node3.is_leader());

    // Property: configuration is still {1,2,3} — no new single-node cluster was created
    BOOST_CHECK_EQUAL(node3.get_cluster_size(), 3u);

    node3.stop();
}

BOOST_AUTO_TEST_SUITE_END()
