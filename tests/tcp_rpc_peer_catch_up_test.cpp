// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// End-to-end peer-to-peer catch-up with every hop on a real socket: Raft RPCs
// on tcp_rpc and progress digests on tcp_gossip_peer2peer_replicator, the
// combination a production TCP node runs. Until tcp_rpc carried
// fetch_log_entries this test could not pass: catch-up compiled down to a
// no-op (.kiro/specs/peer2peer-log-replication/ Requirement 5.3).
#define BOOST_TEST_MODULE tcp_rpc_peer_catch_up_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/raft.hpp>
#include <raft/tcp_gossip_transport.hpp>
#include <raft/tcp_rpc.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("tcp_rpc_peer_catch_up_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

template<typename NodeId, typename Address> class preset_peer_discovery {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    auto register_node(NodeId, Address) -> kythira::future_default<void> {
        return kythira::future_factory_default::makeFuture();
    }
    [[nodiscard]] auto find_peers(std::chrono::milliseconds) const
        -> kythira::future_default<std::vector<kythira::peer_info<NodeId, Address>>> {
        return kythira::future_factory_default::makeFuture(
            std::vector<kythira::peer_info<NodeId, Address>>{});
    }
};

struct test_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type = kythira::tcp_rpc_client;
    using network_server_type = kythira::tcp_rpc_server;

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

auto find_free_port() -> std::uint16_t {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE_GE(fd, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    (void)::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    auto port = ntohs(a.sin_port);
    ::close(fd);
    return port;
}

template<typename Pred>
auto wait_until(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{10000})
    -> bool {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

auto make_fast_config() -> kythira::raft_configuration {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{150};
    cfg._election_timeout_max = std::chrono::milliseconds{300};
    cfg._heartbeat_interval = std::chrono::milliseconds{30};
    cfg._rpc_timeout = std::chrono::milliseconds{500};
    cfg._progress_gossip_interval = std::chrono::milliseconds{25};
    cfg._catch_up_fetch_timeout = std::chrono::milliseconds{1000};
    cfg._catch_up_gap_threshold = 0;
    return cfg;
}

}  // namespace

/**
 * Raft cluster {1, 2}; node 3 is outside it, its tcp_rpc client knows only
 * node 2, and its gossip membership is {2, 3}. The leader never sends node 3
 * anything, so a node 3 log equal to the leader's can only have arrived as a
 * fetch_log_entries reply from node 2 over tcp_rpc.
 */
BOOST_AUTO_TEST_CASE(follower_fetches_missing_entries_from_peer_over_tcp_rpc,
                     *boost::unit_test::timeout(60)) {
    const std::uint64_t ids[] = {1, 2, 3};
    std::unordered_map<std::uint64_t, std::uint16_t> rpc_port;
    std::unordered_map<std::uint64_t, std::uint16_t> gossip_port;
    for (auto id : ids) {
        rpc_port[id] = find_free_port();
        gossip_port[id] = find_free_port();
    }

    auto gossip_config = [&](std::uint64_t self) {
        kythira::tcp_gossip_config<std::uint64_t, std::string> cfg;
        cfg.listen_port = gossip_port[self];
        cfg.gossip_round_interval = std::chrono::milliseconds{40};
        cfg.freshness_interval = std::chrono::seconds{3};
        for (auto id : ids) {
            cfg.address_book.push_back({id, "127.0.0.1:" + std::to_string(gossip_port[id])});
        }
        return cfg;
    };

    auto make_node = [&](std::uint64_t id, std::vector<std::uint64_t> rpc_peers,
                         const kythira::raft_configuration& c) {
        kythira::tcp_rpc_client client;
        for (auto peer : rpc_peers) {
            client.add_peer(peer, "127.0.0.1", rpc_port[peer]);
        }
        return std::make_unique<test_node>(
            id, std::move(client), kythira::tcp_rpc_server{rpc_port[id]},
            test_types::persistence_engine_type{}, kythira::console_logger{},
            kythira::noop_metrics{}, test_types::membership_manager_type{}, c,
            "127.0.0.1:" + std::to_string(rpc_port[id]),
            preset_peer_discovery<std::uint64_t, std::string>{},
            test_types::peer2peer_replicator_type{gossip_config(id)});
    };

    auto cfg = make_fast_config();
    auto node1 = make_node(1, {2}, cfg);
    auto node2 = make_node(2, {1}, cfg);
    auto node3 = make_node(3, {2}, make_fast_config());

    node1->set_cluster_configuration({1, 2});
    node2->set_cluster_configuration({1, 2});
    node3->set_cluster_configuration({2, 3});

    node1->start();
    node2->start();
    node3->start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    BOOST_REQUIRE(wait_until([&] {
        node1->check_election_timeout();
        return node1->is_leader();
    }));

    constexpr int num_commands = 12;
    for (int i = 0; i < num_commands; ++i) {
        auto command = kythira::test_key_value_state_machine<std::uint64_t>::make_put_command(
            "key" + std::to_string(i), "value" + std::to_string(i));
        try {
            node1->submit_command(command, std::chrono::milliseconds{1000});
        } catch (...) {
        }
        node1->check_heartbeat_timeout();
    }
    for (int i = 0; i < 15; ++i) {
        node1->check_heartbeat_timeout();
        std::this_thread::sleep_for(cfg._heartbeat_interval);
    }

    auto leader_commit = node1->debug_state().commit_index;
    BOOST_REQUIRE_GT(leader_commit, 0u);
    BOOST_REQUIRE(wait_until([&] {
        node1->check_heartbeat_timeout();
        return node2->debug_state().last_applied >= leader_commit;
    }));

    BOOST_REQUIRE(wait_until(
        [&] {
            node1->check_heartbeat_timeout();
            node2->check_election_timeout();
            // Gossip and catch-up only: node 3 never campaigns, so it can
            // only gain entries by fetching them.
            node3->check_peer_catch_up();
            return static_cast<std::uint64_t>(node3->debug_state().log.size()) >= leader_commit;
        },
        std::chrono::milliseconds{15000}));

    auto leader_log = node1->debug_state().log;
    auto fetched_log = node3->debug_state().log;
    BOOST_REQUIRE_GE(leader_log.size(), fetched_log.size());
    for (std::size_t i = 0; i < fetched_log.size(); ++i) {
        BOOST_CHECK_EQUAL(fetched_log[i].index(), leader_log[i].index());
        BOOST_CHECK_EQUAL(fetched_log[i].term(), leader_log[i].term());
        BOOST_CHECK(fetched_log[i].command() == leader_log[i].command());
    }
    BOOST_CHECK(!node3->is_leader());

    node3->stop();
    node2->stop();
    node1->stop();
}
