// Property test for .kiro/specs/peer2peer-log-replication/ Task 18
// (Requirement 9.3): a follower that falls behind after a real network
// partition converges via peer-to-peer catch-up once reconnected, rather
// than depending on however many AppendEntries round trips the leader alone
// would need. Structured like peer2peer_catch_up_property_test.cpp's
// joining_node_catches_up_via_peer_not_leader — the healed follower is
// reconnected only to a peer, never to the leader, so convergence can only
// be attributed to the peer-to-peer fetch path.
#define BOOST_TEST_MODULE peer2peer_catch_up_partition_reconnect_property_test
#include <boost/test/unit_test.hpp>

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
        char* argv0[] = {const_cast<char*>("peer2peer_catch_up_partition_reconnect_property_test"),
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

// Long enough that the isolated node never spontaneously campaigns during
// its time cut off from everyone, so post-heal convergence is unambiguously
// attributable to the peer-to-peer fetch rather than a self-won election.
kythira::raft_configuration make_dormant_config() {
    kythira::raft_configuration cfg = make_fast_config();
    cfg._election_timeout_min = std::chrono::minutes{10};
    cfg._election_timeout_max = std::chrono::minutes{10} + std::chrono::milliseconds{1};
    return cfg;
}

}  // namespace

/**
 * Property (Requirement 9.3): a follower isolated by a real network
 * partition, reconnected only to a peer (never the leader), converges its
 * log via the peer-to-peer catch-up path once healed.
 */
BOOST_AUTO_TEST_CASE(reconnected_follower_catches_up_via_peer, *boost::unit_test::timeout(30)) {
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
    auto cfg3 = make_dormant_config();

    auto make_node = [&](std::uint64_t id, auto net, const kythira::raft_configuration& c) {
        return test_node{id,
                         {net, test_types::serializer_type{}},
                         {net, test_types::serializer_type{}},
                         {},
                         kythira::console_logger{},
                         {},
                         {},
                         c,
                         std::to_string(id),
                         preset_peer_discovery<std::uint64_t, std::string>{},
                         replicator_t{table}};
    };

    auto node1 = make_node(1, net1, cfg);
    auto node2 = make_node(2, net2, cfg);
    auto node3 = make_node(3, net3, cfg3);

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

    // Real prefix, replicated to all three while fully connected.
    constexpr int prefix_commands = 3;
    for (int i = 0; i < prefix_commands; ++i) {
        auto command = kythira::test_key_value_state_machine<std::uint64_t>::make_put_command(
            "prefix" + std::to_string(i), "v");
        try {
            node1.submit_command(command, std::chrono::milliseconds{500});
        } catch (...) {
        }
        pump_heartbeats(1);
    }
    pump_heartbeats(15);
    auto prefix_index = node1.debug_state().commit_index;
    BOOST_REQUIRE_GT(prefix_index, 0u);
    BOOST_REQUIRE(wait_until([&] { return node3.debug_state().last_applied >= prefix_index; }));

    // Genuine partition: cut node3 off from everyone.
    sim.remove_edge("1", "3");
    sim.remove_edge("3", "1");
    sim.remove_edge("2", "3");
    sim.remove_edge("3", "2");

    // Cluster continues without node3 (majority = 2 of 3, still healthy).
    constexpr int post_partition_commands = 20;
    for (int i = 0; i < post_partition_commands; ++i) {
        auto command = kythira::test_key_value_state_machine<std::uint64_t>::make_put_command(
            "post" + std::to_string(i), "v");
        try {
            node1.submit_command(command, std::chrono::milliseconds{500});
        } catch (...) {
        }
        pump_heartbeats(1);
    }
    pump_heartbeats(15);

    auto leader_last_index = node1.debug_state().commit_index;
    BOOST_REQUIRE_GT(leader_last_index, prefix_index);
    BOOST_REQUIRE(
        wait_until([&] { return node2.debug_state().last_applied >= leader_last_index; }));

    // Heal — but only node3<->node2, deliberately never restoring node3<->node1.
    // Any convergence from here must flow through node2 via peer-to-peer fetch.
    sim.add_edge("2", "3", edge);
    sim.add_edge("3", "2", edge);

    BOOST_REQUIRE(wait_until(
        [&] {
            node1.check_election_timeout();
            node2.check_election_timeout();
            node3.check_election_timeout();
            return static_cast<std::size_t>(node3.debug_state().log.size()) >=
                   static_cast<std::size_t>(leader_last_index);
        },
        std::chrono::milliseconds{6000}));

    auto node1_log = node1.debug_state().log;
    auto node3_log = node3.debug_state().log;
    BOOST_REQUIRE_EQUAL(node3_log.size(), node1_log.size());
    for (std::size_t i = 0; i < node1_log.size(); ++i) {
        BOOST_CHECK_EQUAL(node3_log[i].index(), node1_log[i].index());
        BOOST_CHECK_EQUAL(node3_log[i].term(), node1_log[i].term());
        BOOST_CHECK(node3_log[i].command() == node1_log[i].command());
    }

    node3.stop();
    node2.stop();
    node1.stop();
}
