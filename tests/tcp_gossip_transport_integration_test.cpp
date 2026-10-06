// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Integration test: real TCP sockets, real background gossip thread, multiple
// tcp_gossip_peer2peer_replicator instances constructed in a single test
// process on distinct loopback ports. Deliberately NOT spawning subprocesses
// (see .kiro/specs/peer2peer-gossip-transport/tasks.md's anti-flakiness note
// — ca_cluster_node_test.cpp's posix_spawn-based pattern was this project's
// dominant CI flake source under ctest -j$(nproc) CPU contention).
#define BOOST_TEST_MODULE tcp_gossip_transport_integration_test
#include <boost/test/unit_test.hpp>

#include <raft/tcp_gossip_transport.hpp>

#include "tcp_server_hardening_cases.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using gossip_t =
    kythira::tcp_gossip_peer2peer_replicator<std::uint64_t, std::string, std::uint64_t>;

constexpr std::uint16_t k_port1 = 19701;
constexpr std::uint16_t k_port2 = 19702;
constexpr std::uint16_t k_port3 = 19703;
constexpr std::uint16_t k_port_localhost1 = 19704;
constexpr std::uint16_t k_port_localhost2 = 19705;
constexpr std::uint16_t k_port_v6_1 = 19706;
constexpr std::uint16_t k_port_v6_2 = 19707;
constexpr std::uint16_t k_port_refused = 19708;
constexpr std::uint16_t k_port_hardening = 19709;

auto host_has_ipv6() -> bool {
    int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    ::close(fd);
    return true;
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

// Two nodes that listen on `listen_address` and know each other by
// `peer1`/`peer2`; returns whether node 2 learns node 1's progress.
auto two_nodes_converge(const std::string& listen_address, std::uint16_t port1, std::uint16_t port2,
                        const std::string& peer1, const std::string& peer2) -> bool {
    kythira::tcp_gossip_config<std::uint64_t, std::string> cfg1;
    cfg1.listen_address = listen_address;
    cfg1.listen_port = port1;
    cfg1.gossip_round_interval = std::chrono::milliseconds{50};
    cfg1.address_book = {{1, peer1}, {2, peer2}};
    auto cfg2 = cfg1;
    cfg2.listen_port = port2;

    gossip_t node1{cfg1};
    gossip_t node2{cfg2};
    node1.start();
    node2.start();
    std::move(node1.update_membership({1, 2})).get();
    std::move(node2.update_membership({1, 2})).get();
    std::move(node1.advertise_progress(1, peer1, 5, 777)).get();
    std::move(node2.advertise_progress(2, peer2, 1, 0)).get();

    return wait_until([&] {
        auto source =
            std::move(node2.find_catch_up_source(777, 777, std::chrono::milliseconds{100})).get();
        return source.has_value() && source->node_id == 1u;
    });
}

}  // namespace

BOOST_AUTO_TEST_CASE(gossip_round_trip_propagates_advertised_progress,
                     *boost::unit_test::timeout(30)) {
    kythira::tcp_gossip_config<std::uint64_t, std::string> cfg1;
    cfg1.listen_port = k_port1;
    cfg1.fanout = 3;
    cfg1.gossip_round_interval = std::chrono::milliseconds{50};
    cfg1.freshness_interval = std::chrono::seconds{5};
    cfg1.address_book = {{1, "127.0.0.1:" + std::to_string(k_port1)},
                         {2, "127.0.0.1:" + std::to_string(k_port2)},
                         {3, "127.0.0.1:" + std::to_string(k_port3)}};

    auto cfg2 = cfg1;
    cfg2.listen_port = k_port2;
    auto cfg3 = cfg1;
    cfg3.listen_port = k_port3;

    gossip_t node1{cfg1};
    gossip_t node2{cfg2};
    gossip_t node3{cfg3};

    // Construction is cheap and starts no thread/socket (so this object can
    // be freely moved into node_config<Types>/node<Types>); start() is what
    // actually opens the listener and begins gossiping.
    node1.start();
    node2.start();
    node3.start();

    std::move(node1.update_membership({1, 2, 3})).get();
    std::move(node2.update_membership({1, 2, 3})).get();
    std::move(node3.update_membership({1, 2, 3})).get();

    // node1 advertises far-ahead progress; confirm it becomes visible on
    // node3 (which never talks to node1 directly except via gossip fanout)
    // within a small, bounded number of rounds.
    std::move(node1.advertise_progress(1, "127.0.0.1:" + std::to_string(k_port1), 5, 12345)).get();
    std::move(node2.advertise_progress(2, "127.0.0.1:" + std::to_string(k_port2), 1, 0)).get();
    std::move(node3.advertise_progress(3, "127.0.0.1:" + std::to_string(k_port3), 1, 0)).get();

    bool converged = wait_until([&] {
        auto source =
            std::move(node3.find_catch_up_source(12345, 12345, std::chrono::milliseconds{100}))
                .get();
        return source.has_value() && source->node_id == 1u;
    });
    BOOST_CHECK(converged);
}

// The listener binds every address "localhost" names, and peers addressed
// as "localhost:port" are reached.
BOOST_AUTO_TEST_CASE(localhost_bind_and_peer_names, *boost::unit_test::timeout(30)) {
    BOOST_CHECK(two_nodes_converge("localhost", k_port_localhost1, k_port_localhost2,
                                   "localhost:" + std::to_string(k_port_localhost1),
                                   "localhost:" + std::to_string(k_port_localhost2)));
}

// "*" listens on IPv6 as well, and a bracketed IPv6 peer address dials it.
BOOST_AUTO_TEST_CASE(star_bind_with_ipv6_peers, *boost::unit_test::timeout(30)) {
    if (!host_has_ipv6()) {
        BOOST_TEST_MESSAGE("no IPv6 on this host; skipping");
        return;
    }
    BOOST_CHECK(two_nodes_converge("*", k_port_v6_1, k_port_v6_2,
                                   "[::1]:" + std::to_string(k_port_v6_1),
                                   "[::1]:" + std::to_string(k_port_v6_2)));
}

// A bind name that is not in /etc/hosts is refused rather than looked up in
// DNS.
BOOST_AUTO_TEST_CASE(unlisted_bind_name_is_refused, *boost::unit_test::timeout(30)) {
    kythira::tcp_gossip_config<std::uint64_t, std::string> cfg;
    cfg.listen_address = "kythira-unlisted-bind-name.invalid";
    cfg.listen_port = k_port_refused;
    gossip_t node{cfg};
    BOOST_CHECK_THROW(node.start(), std::exception);
}

BOOST_AUTO_TEST_CASE(split_host_port_accepts_bracketed_ipv6) {
    using kythira::gossip_detail::split_host_port;
    BOOST_CHECK(split_host_port(std::string("[::1]:7000")) ==
                (std::pair<std::string, std::uint16_t>{"::1", 7000}));
    BOOST_CHECK(split_host_port(std::string("[fe80::1%eth0]:80")) ==
                (std::pair<std::string, std::uint16_t>{"fe80::1%eth0", 80}));
    BOOST_CHECK(split_host_port(std::string("10.0.0.1:9")) ==
                (std::pair<std::string, std::uint16_t>{"10.0.0.1", 9}));
}

// ── Listener hardening (.kiro/specs/tcp-rpc-server-hardening/) ───────────────

namespace {

auto hardened_listener(kythira::tcp_server_limits limits) -> std::unique_ptr<gossip_t> {
    kythira::tcp_gossip_config<std::uint64_t, std::string> cfg;
    cfg.listen_address = "127.0.0.1";
    cfg.listen_port = k_port_hardening;
    cfg.gossip_round_interval = std::chrono::milliseconds{50};
    cfg.listener_limits = limits;
    auto node = std::make_unique<gossip_t>(cfg);
    node->start();
    std::move(node->advertise_progress(1, "127.0.0.1:" + std::to_string(k_port_hardening), 1, 1))
        .get();
    return node;
}

// One push-pull exchange against the listener, as a peer would make it.
auto exchange_succeeds() -> bool {
    int fd = tcp_server_hardening::dial(k_port_hardening);
    if (fd < 0) {
        return false;
    }
    kythira::gossip_exchange_message<std::uint64_t, std::string, std::uint64_t> req{9, {}};
    bool ok = kythira::tcp_detail::frame_send(fd, kythira::encode_gossip_message(req)) &&
              kythira::tcp_detail::frame_recv(fd).has_value();
    ::close(fd);
    return ok;
}

}  // namespace

BOOST_AUTO_TEST_CASE(listener_rejects_zero_limits) {
    kythira::tcp_gossip_config<std::uint64_t, std::string> cfg;
    cfg.listener_limits.max_connections = 0;
    BOOST_CHECK_THROW(gossip_t{cfg}, std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(listener_closes_silent_peer, *boost::unit_test::timeout(30)) {
    auto limits = tcp_server_hardening::small_limits();
    auto node = hardened_listener(limits);
    tcp_server_hardening::silent_peer_is_closed(*node, k_port_hardening, limits);
    BOOST_TEST(exchange_succeeds());
    node->stop();
}

BOOST_AUTO_TEST_CASE(listener_closes_trickling_peer, *boost::unit_test::timeout(30)) {
    auto limits = tcp_server_hardening::small_limits();
    auto node = hardened_listener(limits);
    auto header = htonl(1000u);
    tcp_server_hardening::trickling_peer_is_closed(
        *node, k_port_hardening, limits, std::string(reinterpret_cast<const char*>(&header), 4));
    node->stop();
}

BOOST_AUTO_TEST_CASE(listener_connection_limits, *boost::unit_test::timeout(30)) {
    auto limits = tcp_server_hardening::small_limits();
    limits.request_timeout = std::chrono::milliseconds{2000};
    auto node = hardened_listener(limits);
    tcp_server_hardening::connection_limits_hold(*node, k_port_hardening, exchange_succeeds);
    node->stop();
}

BOOST_AUTO_TEST_CASE(listener_stop_is_prompt_with_idle_connection, *boost::unit_test::timeout(30)) {
    auto limits = tcp_server_hardening::small_limits();
    limits.request_timeout = std::chrono::seconds{30};
    auto node = hardened_listener(limits);
    tcp_server_hardening::stop_is_prompt_with_idle_connection(*node, k_port_hardening);
}

// A peer that sends its digests and leaves at once: the listener's reply
// lands on a closed socket, which used to raise SIGPIPE and end the process.
BOOST_AUTO_TEST_CASE(listener_reply_to_departed_peer_raises_no_sigpipe,
                     *boost::unit_test::timeout(30)) {
    auto previous = std::signal(SIGPIPE, SIG_DFL);
    {
        // Room for every peer at once. All 20 dial from 127.0.0.1 faster than
        // the listener drains them, so under small_limits()'s two per source
        // some were refused on arrival: the send below then failed, or the
        // final exchange was the refused one (seen under ThreadSanitizer in
        // CI, and in roughly one local run in four). This case is about the
        // reply to a departed peer, not about limits.
        auto limits = tcp_server_hardening::small_limits();
        limits.max_connections = 64;
        limits.max_connections_per_source = 64;
        auto node = hardened_listener(limits);
        for (int i = 0; i < 20; ++i) {
            int fd = tcp_server_hardening::dial(k_port_hardening);
            BOOST_REQUIRE(fd >= 0);
            kythira::gossip_exchange_message<std::uint64_t, std::string, std::uint64_t> req{9, {}};
            BOOST_REQUIRE(kythira::tcp_detail::frame_send(fd, kythira::encode_gossip_message(req)));
            ::close(fd);
        }
        BOOST_TEST(tcp_server_hardening::wait_until(
            [&] { return node->connection_stats().active_connections == 0; }));
        BOOST_TEST(exchange_succeeds());
        node->stop();
    }
    std::signal(SIGPIPE, previous);
}
