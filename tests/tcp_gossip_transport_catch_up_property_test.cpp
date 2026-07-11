// End-to-end property tests wiring real tcp_gossip_peer2peer_replicator
// instances as peer2peer_replicator_type for a multi-node node<Types>
// cluster whose Raft RPC transport remains the existing in-process network
// simulator — deliberately keeping Raft consensus itself on its
// already-deterministic simulated transport while only the gossip layer runs
// over real sockets (.kiro/specs/peer2peer-gossip-transport/, Requirement
// 10.3/10.4).
#define BOOST_TEST_MODULE tcp_gossip_transport_catch_up_property_test
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/tcp_gossip_transport.hpp>
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
        char* argv0[] = {const_cast<char*>("tcp_gossip_transport_catch_up_property_test"), nullptr};
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

    using peer2peer_replicator_type =
        kythira::tcp_gossip_peer2peer_replicator<node_id_type, address_type, log_index_type>;
};

using test_node = kythira::node<test_types>;
using sim_t = network_simulator::NetworkSimulator<test_types::raft_network_types>;

template<typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{10000}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
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

auto make_gossip_config(std::uint64_t self_id, std::uint16_t self_port,
                        const std::vector<std::pair<std::uint64_t, std::uint16_t>>& all)
    -> kythira::tcp_gossip_config<std::uint64_t, std::string> {
    kythira::tcp_gossip_config<std::uint64_t, std::string> cfg;
    cfg.listen_port = self_port;
    cfg.gossip_round_interval = std::chrono::milliseconds{40};
    cfg.freshness_interval = std::chrono::seconds{3};
    for (const auto& [id, port] : all) {
        cfg.address_book.push_back({id, "127.0.0.1:" + std::to_string(port)});
    }
    (void)self_id;
    return cfg;
}

}  // namespace

/**
 * Property (Requirement 10.3/10.4): a lagging/joining node's digest table
 * converges to reflect a peer's advertised progress via the REAL TCP gossip
 * transport, and find_catch_up_source returns a usable peer once it does —
 * while Raft RPCs themselves stay on the deterministic simulator transport.
 */
BOOST_AUTO_TEST_CASE(joining_node_catches_up_via_real_gossip_transport,
                     *boost::unit_test::timeout(30)) {
    constexpr std::uint16_t port1 = 19711;
    constexpr std::uint16_t port2 = 19712;
    constexpr std::uint16_t port3 = 19713;
    std::vector<std::pair<std::uint64_t, std::uint16_t>> all_ports{
        {1, port1}, {2, port2}, {3, port3}};

    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");

    // Real Raft cluster is only {1, 2}; no edge to node 3 at the Raft-RPC
    // (simulator) layer — any log convergence node 3 achieves must come via
    // the real TCP gossip transport's peer-to-peer fetch, routed through node 2.
    network_simulator::NetworkEdge edge{};
    sim.add_edge("1", "2", edge);
    sim.add_edge("2", "1", edge);
    sim.add_edge("2", "3", edge);
    sim.add_edge("3", "2", edge);

    auto cfg = make_fast_config();
    auto cfg3 = make_dormant_config();

    auto make_node = [&](std::uint64_t id, auto net, const kythira::raft_configuration& c,
                         std::uint16_t port) {
        return test_node{
            id,
            {net, test_types::serializer_type{}},
            {net, test_types::serializer_type{}},
            {},
            kythira::console_logger{},
            {},
            {},
            c,
            std::to_string(id),
            preset_peer_discovery<std::uint64_t, std::string>{},
            test_types::peer2peer_replicator_type{make_gossip_config(id, port, all_ports)}};
    };

    auto node1 = make_node(1, net1, cfg, port1);
    auto node2 = make_node(2, net2, cfg, port2);
    auto node3 = make_node(3, net3, cfg3, port3);

    node1.set_cluster_configuration({1, 2});
    node2.set_cluster_configuration({1, 2});
    // Node 3's own gossip membership view excludes the leader — it can only
    // ever select node 2 as a catch-up source.
    node3.set_cluster_configuration({2, 3});

    node1.start();
    node2.start();
    node3.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    auto pump_heartbeats = [&](int cycles) {
        for (int i = 0; i < cycles; ++i) {
            node1.check_heartbeat_timeout();
            std::this_thread::sleep_for(cfg._heartbeat_interval);
        }
    };

    constexpr int num_commands = 12;
    for (int i = 0; i < num_commands; ++i) {
        auto command = kythira::test_key_value_state_machine<std::uint64_t>::make_put_command(
            "key" + std::to_string(i), "value" + std::to_string(i));
        try {
            node1.submit_command(command, std::chrono::milliseconds{500});
        } catch (...) {
        }
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{15});
    }
    pump_heartbeats(15);

    auto leader_last_index = node1.debug_state().commit_index;
    BOOST_REQUIRE_GT(leader_last_index, 0u);
    BOOST_REQUIRE(
        wait_until([&] { return node2.debug_state().last_applied >= leader_last_index; }));

    BOOST_REQUIRE(wait_until(
        [&] {
            node1.check_election_timeout();
            node2.check_election_timeout();
            node3.check_election_timeout();
            return static_cast<std::size_t>(node3.debug_state().log.size()) >=
                   static_cast<std::size_t>(leader_last_index);
        },
        std::chrono::milliseconds{10000}));

    auto node1_log = node1.debug_state().log;
    auto node3_log = node3.debug_state().log;
    BOOST_REQUIRE_EQUAL(node3_log.size(), node1_log.size());
    for (std::size_t i = 0; i < node1_log.size(); ++i) {
        BOOST_CHECK_EQUAL(node3_log[i].index(), node1_log[i].index());
        BOOST_CHECK_EQUAL(node3_log[i].term(), node1_log[i].term());
    }
    BOOST_CHECK(!node3.is_leader());

    node3.stop();
    node2.stop();
    node1.stop();
}

/**
 * Property (Requirement 6.3/10.4): a node that stops advertising (simulating
 * a crash) has its digest pruned from other nodes' tables after
 * freshness_interval elapses.
 */
BOOST_AUTO_TEST_CASE(silent_node_digest_expires_after_freshness_interval,
                     *boost::unit_test::timeout(30)) {
    constexpr std::uint16_t port1 = 19721;
    constexpr std::uint16_t port2 = 19722;
    std::vector<std::pair<std::uint64_t, std::uint16_t>> all_ports{{1, port1}, {2, port2}};

    auto cfg1 = make_gossip_config(1, port1, all_ports);
    cfg1.freshness_interval = std::chrono::seconds{1};
    cfg1.gossip_round_interval = std::chrono::milliseconds{40};
    auto cfg2 = make_gossip_config(2, port2, all_ports);
    cfg2.freshness_interval = std::chrono::seconds{1};
    cfg2.gossip_round_interval = std::chrono::milliseconds{40};

    test_types::peer2peer_replicator_type node1{cfg1};
    test_types::peer2peer_replicator_type node2{cfg2};
    node1.start();
    node2.start();

    std::move(node1.update_membership({1, 2})).get();
    std::move(node2.update_membership({1, 2})).get();

    std::move(node1.advertise_progress(1, "127.0.0.1:" + std::to_string(port1), 3, 500)).get();
    std::move(node2.advertise_progress(2, "127.0.0.1:" + std::to_string(port2), 1, 0)).get();

    // node2 sees node1's digest while node1 keeps advertising.
    BOOST_REQUIRE(wait_until([&] {
        auto s = std::move(node2.find_catch_up_source(1, 1, std::chrono::milliseconds{100})).get();
        return s.has_value() && s->node_id == 1u;
    }));

    // node1 goes silent (simulating a crash) — stop advertising, but the
    // instance (and its gossip thread) keeps running so node2 can still try
    // to reach it; the digest should simply age out.
    std::this_thread::sleep_for(std::chrono::milliseconds{1300});

    bool expired = wait_until(
        [&] {
            auto s =
                std::move(node2.find_catch_up_source(1, 1, std::chrono::milliseconds{100})).get();
            return !s.has_value();
        },
        std::chrono::milliseconds{3000});
    BOOST_CHECK(expired);
}

/**
 * Property (design.md Property 4 / Requirements 1.4, 2.2, 2.3): a removed
 * member is never offered as a catch-up source, immediately upon the next
 * update_membership() call — even though its last-advertised digest may
 * still be present in the table until freshness_interval separately elapses.
 */
BOOST_AUTO_TEST_CASE(removed_member_immediately_ineligible_despite_lingering_digest,
                     *boost::unit_test::timeout(30)) {
    constexpr std::uint16_t port1 = 19731;
    constexpr std::uint16_t port2 = 19732;
    std::vector<std::pair<std::uint64_t, std::uint16_t>> all_ports{{1, port1}, {2, port2}};

    auto cfg1 = make_gossip_config(1, port1, all_ports);
    auto cfg2 = make_gossip_config(2, port2, all_ports);

    test_types::peer2peer_replicator_type node1{cfg1};
    test_types::peer2peer_replicator_type node2{cfg2};
    node1.start();
    node2.start();

    std::move(node1.update_membership({1, 2})).get();
    std::move(node2.update_membership({1, 2})).get();

    std::move(node1.advertise_progress(1, "127.0.0.1:" + std::to_string(port1), 1, 100)).get();

    // node2 currently sees node1 as an eligible catch-up source.
    BOOST_REQUIRE(wait_until([&] {
        auto s = std::move(node2.find_catch_up_source(1, 1, std::chrono::milliseconds{100})).get();
        return s.has_value() && s->node_id == 1u;
    }));

    // node2 is told node1 was removed from the cluster — its own digest may
    // still linger in node2's table, but it must stop being offered
    // immediately, synchronously with this call (no gossip round or
    // freshness-interval wait required).
    std::move(node2.update_membership({2})).get();
    auto after = std::move(node2.find_catch_up_source(1, 1, std::chrono::milliseconds{100})).get();
    BOOST_CHECK(!after.has_value());

    // And re-adding it makes it eligible again immediately too.
    std::move(node2.update_membership({1, 2})).get();
    auto readded =
        std::move(node2.find_catch_up_source(1, 1, std::chrono::milliseconds{100})).get();
    BOOST_CHECK(readded.has_value());
}
