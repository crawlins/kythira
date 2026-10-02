// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// A fresh node joins a running cluster over tcp_rpc, the way a replacement
// that docker_quorum_manager provisions joins (quorum-management Req 19).
// Real sockets on loopback, no simulator: the point is that a leader can
// replicate to a node that is in no member's static peer table.

#define BOOST_TEST_MODULE tcp_cluster_join_test
#include <boost/test/unit_test.hpp>

#include <raft/console_logger.hpp>
#include <raft/membership.hpp>
#include <raft/metrics.hpp>
#include <raft/peer_discovery.hpp>
#include <raft/persistence.hpp>
#include <raft/raft.hpp>
#include <raft/tcp_raft_types.hpp>
#include <raft/tcp_rpc.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include "test_timeout_scale.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using kythira::testing::scaled_deadline;
using kythira::testing::scaled_timeout;

struct join_types : kythira::tcp_raft_types {
    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using address_type = std::string;
    using peer_discovery_type = kythira::seed_peer_discovery<node_id_type, address_type>;
};
using join_node = kythira::node<join_types>;
using seeds_t = std::vector<kythira::peer_info<std::uint64_t, std::string>>;

auto free_port() -> std::uint16_t {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(fd);
    return ntohs(addr.sin_port);
}

auto fast_config() -> kythira::raft_configuration {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = scaled_deadline(300);
    cfg._election_timeout_max = scaled_deadline(600);
    cfg._heartbeat_interval = std::chrono::milliseconds{30};
    cfg._rpc_timeout = scaled_deadline(500);
    cfg._bootstrap_peer_find_timeout = scaled_deadline(500);
    cfg._bootstrap_retry_interval = std::chrono::milliseconds{200};
    return cfg;
}

template<typename Pred>
auto wait_until(Pred pred, std::chrono::milliseconds deadline = scaled_deadline(10000)) -> bool {
    auto end = std::chrono::steady_clock::now() + deadline;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return true;
}

// Nodes 1..3 are the founding members, each with a static peer table of the
// others.  Joiners are added later and appear in nobody's table.
class tcp_cluster {
public:
    tcp_cluster() {
        for (std::uint64_t id = 1; id <= 3; ++id) {
            _ports[id] = free_port();
        }
    }

    ~tcp_cluster() {
        for (auto& [id, stop] : _stops) {
            stop->store(true);
        }
        for (auto& t : _tickers) {
            t.join();
        }
        for (auto& [id, n] : _nodes) {
            n->stop();
        }
    }

    auto address(std::uint64_t id) const -> std::string {
        return "127.0.0.1:" + std::to_string(_ports.at(id));
    }

    auto start_founders() -> void {
        for (std::uint64_t id = 1; id <= 3; ++id) {
            kythira::tcp_rpc_client client;
            for (std::uint64_t peer = 1; peer <= 3; ++peer) {
                if (peer != id) {
                    client.add_peer(peer, "127.0.0.1", _ports.at(peer));
                }
            }
            auto& n = emplace(id, std::move(client), seeds_t{});
            n.set_cluster_configuration({1, 2, 3});
            n.start();
        }
    }

    // Starts a fresh node that knows the cluster only through `seeds`.
    // start() returns once a leader has accepted its ClusterJoin.
    auto start_joiner(std::uint64_t id, seeds_t seeds) -> join_node& {
        _ports[id] = free_port();
        kythira::tcp_rpc_client client;
        for (const auto& s : seeds) {
            client.update_peer_address(s.node_id, s.address);
        }
        auto& n = emplace(id, std::move(client), std::move(seeds));
        n.start();
        start_ticker(id);
        return n;
    }

    auto elect_node1() -> bool {
        auto& leader = node(1);
        auto deadline = std::chrono::steady_clock::now() + scaled_deadline(10000);
        while (!leader.is_leader()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(fast_config()._election_timeout_max);
            leader.check_election_timeout();
            wait_until([&] { return leader.is_leader(); }, std::chrono::milliseconds{500});
        }
        for (std::uint64_t id = 1; id <= 3; ++id) {
            start_ticker(id);
        }
        return true;
    }

    auto node(std::uint64_t id) -> join_node& { return *_nodes.at(id); }

    auto ever_led(std::uint64_t id) const -> bool { return _ever_led.at(id)->load(); }

private:
    auto emplace(std::uint64_t id, kythira::tcp_rpc_client client, seeds_t seeds) -> join_node& {
        kythira::node_config<join_types> cfg{
            .node_id = id,
            .network_client = std::move(client),
            .network_server = kythira::tcp_rpc_server{_ports.at(id), "127.0.0.1"},
            .persistence = {},
            .logger = kythira::console_logger{kythira::log_level::info},
            .metrics = {},
            .membership = {},
            .config = fast_config(),
            .self_address = address(id),
            .peer_discovery = kythira::seed_peer_discovery<std::uint64_t, std::string>{seeds},
            // add_learner() fails closed without a declared target for the
            // joiner's placement group, so declare room for all five nodes.
            .quorum_manager =
                kythira::node<join_types>::quorum_manager_type{
                    kythira::desired_topology<std::string>{
                        .groups = {{.group_id = "", .target_count = 5}}}},
        };
        auto n = std::make_unique<join_node>(std::move(cfg));
        auto& ref = *n;
        _nodes.emplace(id, std::move(n));
        _ever_led.emplace(id, std::make_unique<std::atomic<bool>>(false));
        return ref;
    }

    auto start_ticker(std::uint64_t id) -> void {
        auto* n = _nodes.at(id).get();
        auto* led = _ever_led.at(id).get();
        auto stop = std::make_shared<std::atomic<bool>>(false);
        _stops[id] = stop;
        _tickers.emplace_back([n, led, stop] {
            while (!stop->load()) {
                n->check_election_timeout();
                n->check_heartbeat_timeout();
                if (n->is_leader()) {
                    led->store(true);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        });
    }

    std::map<std::uint64_t, std::uint16_t> _ports;
    std::map<std::uint64_t, std::unique_ptr<join_node>> _nodes;
    std::map<std::uint64_t, std::unique_ptr<std::atomic<bool>>> _ever_led;
    std::map<std::uint64_t, std::shared_ptr<std::atomic<bool>>> _stops;
    std::vector<std::thread> _tickers;
};

auto put(std::string_view key, std::string_view value) -> std::vector<std::byte> {
    std::vector<std::byte> cmd{std::byte{1}};
    auto append = [&](std::string_view s) {
        auto n = static_cast<std::uint32_t>(s.size());
        for (int i = 0; i < 4; ++i) {
            cmd.push_back(static_cast<std::byte>((n >> (8 * i)) & 0xFF));
        }
        for (char c : s) {
            cmd.push_back(static_cast<std::byte>(c));
        }
    };
    append(key);
    append(value);
    return cmd;
}

}  // namespace

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInit {
    FollyInit() {
        int argc = 1;
        char arg0[] = "tcp_cluster_join_test";
        char* args[] = {arg0, nullptr};
        char** argv = args;
        _init = std::make_unique<folly::Init>(&argc, &argv, false);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInit);
#endif

BOOST_AUTO_TEST_SUITE(tcp_cluster_join)

// The joiner's only seed is a follower.  The follower redirects it to the
// leader by node ID, the leader admits it as a learner at the address it
// advertised, replicates to it, and promotes it.  Before it is a voter it
// never campaigns, even though its own configuration starts empty.
BOOST_AUTO_TEST_CASE(fresh_node_joins_through_a_follower,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    tcp_cluster c;
    c.start_founders();
    BOOST_REQUIRE(c.elect_node1());

    auto first = c.node(1).submit_command(put("a", "1"), scaled_deadline(5000));
    BOOST_REQUIRE_NO_THROW(std::move(first).get());

    auto& joiner = c.start_joiner(4, seeds_t{{2, c.address(2)}, {1, c.address(1)}});

    // Replicated to: the leader reached the joiner at its advertised address.
    auto leader_commit = c.node(1).debug_state().commit_index;
    BOOST_REQUIRE(wait_until([&] { return joiner.debug_state().last_applied >= leader_commit; }));
    BOOST_CHECK(!c.ever_led(4));

    BOOST_REQUIRE_NO_THROW(c.node(1).promote_to_voter(4).get());
    BOOST_REQUIRE(wait_until([&] { return joiner.get_cluster_size() == 4; }));

    auto second = c.node(1).submit_command(put("b", "2"), scaled_deadline(5000));
    BOOST_REQUIRE_NO_THROW(std::move(second).get());
    auto commit = c.node(1).debug_state().commit_index;
    BOOST_CHECK(wait_until([&] { return joiner.debug_state().last_applied >= commit; }));
    BOOST_CHECK(c.node(1).is_leader());
    BOOST_CHECK(!c.ever_led(4));
}

// A joiner whose admission never arrives waits instead of electing itself
// leader of the empty configuration it started with.  Its seed accepts the
// ClusterJoin and then replicates nothing, as a leader that is deposed right
// after accepting would.
BOOST_AUTO_TEST_CASE(joiner_never_campaigns_before_admission,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto seed_port = free_port();
    kythira::tcp_rpc_server seed{seed_port, "127.0.0.1"};
    std::atomic<int> joins{0};
    seed.register_cluster_join_handler([&joins](const kythira::cluster_join_request<>&) {
        ++joins;
        return kythira::cluster_join_response<>{true, std::nullopt};
    });
    seed.start();

    tcp_cluster c;
    auto& joiner = c.start_joiner(5, seeds_t{{1, "127.0.0.1:" + std::to_string(seed_port)}});
    BOOST_REQUIRE_EQUAL(joins.load(), 1);

    std::this_thread::sleep_for(fast_config()._election_timeout_max * 4);
    BOOST_CHECK(!c.ever_led(5));
    BOOST_CHECK(!joiner.is_leader());
    BOOST_CHECK_EQUAL(joiner.get_current_term(), 0u);
    seed.stop();
}

BOOST_AUTO_TEST_SUITE_END()
