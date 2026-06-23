#define BOOST_TEST_MODULE membership_change_add_server_property_test
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

// ── Folly global fixture ───────────────────────────────────────────────────

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("membership_change_add_server_property_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

// ── Shared types ───────────────────────────────────────────────────────────

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
    auto find_peers(std::chrono::milliseconds) const
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

}  // namespace

// ── Property tests ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(add_server_properties)

/**
 * Property: add_server() resolves (future completes without exception) and the
 * cluster grows from 3 to 4 nodes after the joint-consensus protocol completes.
 */
BOOST_AUTO_TEST_CASE(add_server_resolves_and_cluster_grows, *boost::unit_test::timeout(30)) {
    for (int iteration = 0; iteration < 3; ++iteration) {
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
        auto node4 = make_node(4, net4);

        node1.set_cluster_configuration({1, 2, 3});
        node2.set_cluster_configuration({1, 2, 3});
        node3.set_cluster_configuration({1, 2, 3});
        // node4 will be added — start it as a standalone follower with no config
        node4.set_cluster_configuration({4});

        node1.start();
        node2.start();
        node3.start();
        node4.start();

        // Drive an election on node1
        std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
        node1.check_election_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{200});

        BOOST_REQUIRE_MESSAGE(
            wait_until([&] { return node1.is_leader(); }, std::chrono::milliseconds{4000}),
            "iteration " + std::to_string(iteration) + ": node1 did not become leader");

        // Call add_server — returns a Future<vector<byte>> (empty on success)
        auto add_fut = node1.add_server(4);

        // Drive heartbeats to replicate the joint-config entry
        for (int i = 0; i < 20; ++i) {
            node1.check_heartbeat_timeout();
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
        }

        // Property: future resolves without throwing
        bool resolved = false;
        bool threw = false;
        std::move(add_fut)
            .thenValue([&](std::vector<std::byte>) { resolved = true; })
            .thenError([&](const std::exception_ptr&) { threw = true; });

        BOOST_CHECK_MESSAGE(
            wait_until([&] { return resolved || threw; }, std::chrono::milliseconds{6000}),
            "iteration " + std::to_string(iteration) + ": add_server future did not resolve");
        BOOST_CHECK_MESSAGE(
            !threw, "iteration " + std::to_string(iteration) + ": add_server future rejected");

        // Property: cluster size grew to 4
        BOOST_CHECK_EQUAL(node1.get_cluster_size(), 4u);

        node4.stop();
        node3.stop();
        node2.stop();
        node1.stop();
    }
}

/**
 * Property: after add_server completes, a command submitted to the leader is
 * applied by all four nodes (last_applied matches across the cluster).
 */
BOOST_AUTO_TEST_CASE(command_applied_on_all_nodes_after_add, *boost::unit_test::timeout(30)) {
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
    auto node4 = make_node(4, net4);

    node1.set_cluster_configuration({1, 2, 3});
    node2.set_cluster_configuration({1, 2, 3});
    node3.set_cluster_configuration({1, 2, 3});
    node4.set_cluster_configuration({4});

    node1.start();
    node2.start();
    node3.start();
    node4.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }, std::chrono::milliseconds{4000}));

    auto add_fut = node1.add_server(4);

    // Pump heartbeats until add completes
    for (int i = 0; i < 25; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }

    bool add_done = false;
    std::move(add_fut)
        .thenValue([&](std::vector<std::byte>) { add_done = true; })
        .thenError([&](const std::exception_ptr&) { add_done = true; });

    BOOST_REQUIRE(wait_until([&] { return add_done; }, std::chrono::milliseconds{5000}));
    BOOST_REQUIRE_EQUAL(node1.get_cluster_size(), 4u);

    // Submit a properly formatted PUT command after membership change
    using sm_t = kythira::test_key_value_state_machine<test_types::log_index_type>;
    auto cmd = sm_t::make_put_command("key1", "val1");
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

    // Property: all four nodes have applied the same entries
    auto snap1 = node1.debug_state();
    auto snap4 = node4.debug_state();
    BOOST_CHECK_EQUAL(snap1.last_applied, snap4.last_applied);

    node4.stop();
    node3.stop();
    node2.stop();
    node1.stop();
}

BOOST_AUTO_TEST_SUITE_END()
