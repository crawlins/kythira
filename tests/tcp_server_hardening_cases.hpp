// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Test cases shared by tcp_rpc_server, tls_tcp_rpc_server and the
// tcp_gossip listener (.kiro/specs/tcp-rpc-server-hardening/, Testing
// Strategy cases 2-6). Each case takes a running server through anything
// with connection_stats(), and drives it with raw sockets, so the same code
// checks all three. Include after <boost/test/unit_test.hpp>.

#include <raft/tcp_connection_tracker.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace tcp_server_hardening {

using namespace std::chrono_literals;

// Small limits so each case finishes in a few seconds.
inline auto small_limits() -> kythira::tcp_server_limits {
    kythira::tcp_server_limits l;
    l.request_timeout = 300ms;
    l.reply_timeout = 300ms;
    l.max_connections = 4;
    l.max_connections_per_source = 2;
    return l;
}

template<typename Pred>
auto wait_until(Pred pred, std::chrono::milliseconds limit = 5000ms) -> bool {
    auto end = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// A blocking TCP connection to 127.0.0.1:port, from `source` (any address
// in 127.0.0.0/8, which Linux routes to lo). Returns -1 on failure.
inline auto dial(std::uint16_t port, const char* source = "127.0.0.1") -> int {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in src{};
    src.sin_family = AF_INET;
    ::inet_pton(AF_INET, source, &src.sin_addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&src), sizeof(src)) != 0) {
        ::close(fd);
        return -1;
    }
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Whether the server closed or reset `fd` within `limit`: a read returns 0
// or fails with anything other than a timeout.
inline auto closed_by_server(int fd, std::chrono::milliseconds limit) -> bool {
    auto end = std::chrono::steady_clock::now() + limit;
    char buf[256];
    while (true) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            return false;
        }
        pollfd p{fd, POLLIN, 0};
        int pr = ::poll(&p, 1, static_cast<int>(left.count()));
        if (pr <= 0) {
            return false;
        }
        ssize_t r = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
            return true;
        }
        // Data (a TLS alert, say): keep waiting for the close itself.
    }
}

// Case 2: a peer that connects and sends nothing is closed at
// request_timeout, without a handler call, and frees its slot.
template<typename Server>
void silent_peer_is_closed(Server& server, std::uint16_t port,
                           const kythira::tcp_server_limits& limits) {
    auto before = server.connection_stats().timed_out;
    int fd = dial(port);
    BOOST_REQUIRE(fd >= 0);
    auto start = std::chrono::steady_clock::now();
    BOOST_TEST(closed_by_server(fd, limits.request_timeout + 2000ms));
    auto took = std::chrono::steady_clock::now() - start;
    BOOST_TEST((took >= limits.request_timeout - 50ms));
    BOOST_TEST((took <= limits.request_timeout + 1000ms));
    ::close(fd);
    BOOST_TEST(wait_until([&] { return server.connection_stats().active_connections == 0; }));
    BOOST_TEST(server.connection_stats().timed_out == before + 1);
}

// Case 3: a peer that sends `prefix` and then one byte every 50 ms (never
// silent for long) is still closed when request_timeout passes.
template<typename Server>
void trickling_peer_is_closed(Server& server, std::uint16_t port,
                              const kythira::tcp_server_limits& limits, const std::string& prefix) {
    int fd = dial(port);
    BOOST_REQUIRE(fd >= 0);
    BOOST_REQUIRE(::send(fd, prefix.data(), prefix.size(), MSG_NOSIGNAL) ==
                  static_cast<ssize_t>(prefix.size()));
    auto start = std::chrono::steady_clock::now();
    bool closed = false;
    while (std::chrono::steady_clock::now() - start < limits.request_timeout + 2000ms) {
        char b = 0;
        if (::send(fd, &b, 1, MSG_NOSIGNAL) != 1 || closed_by_server(fd, 50ms)) {
            closed = true;
            break;
        }
    }
    auto took = std::chrono::steady_clock::now() - start;
    BOOST_TEST(closed);
    BOOST_TEST((took <= limits.request_timeout + 1000ms));
    ::close(fd);
    BOOST_TEST(wait_until([&] { return server.connection_stats().active_connections == 0; }));
}

// Case 4: idle connections beyond max_connections_per_source from one
// address, or beyond max_connections in total, are closed on arrival; once
// the idle ones time out, `round_trip` (a normal RPC) succeeds again.
// Expects max_connections_per_source == 2 and max_connections == 4.
template<typename Server, typename RoundTrip>
void connection_limits_hold(Server& server, std::uint16_t port, RoundTrip round_trip) {
    auto s0 = server.connection_stats();
    std::vector<int> held;
    auto admitted = [&](std::size_t n) {
        return wait_until([&] { return server.connection_stats().active_connections == n; });
    };

    for (int i = 0; i < 2; ++i) {
        held.push_back(dial(port, "127.0.0.1"));
        BOOST_REQUIRE(held.back() >= 0);
        BOOST_REQUIRE(admitted(held.size()));
    }
    int over_source = dial(port, "127.0.0.1");
    BOOST_REQUIRE(over_source >= 0);
    BOOST_TEST(closed_by_server(over_source, 1000ms));
    ::close(over_source);
    BOOST_TEST(server.connection_stats().refused_per_source_limit ==
               s0.refused_per_source_limit + 1);

    for (int i = 0; i < 2; ++i) {
        held.push_back(dial(port, "127.0.0.2"));
        BOOST_REQUIRE(held.back() >= 0);
        BOOST_REQUIRE(admitted(held.size()));
    }
    int over_total = dial(port, "127.0.0.3");
    BOOST_REQUIRE(over_total >= 0);
    BOOST_TEST(closed_by_server(over_total, 1000ms));
    ::close(over_total);
    BOOST_TEST(server.connection_stats().refused_global_limit == s0.refused_global_limit + 1);
    BOOST_TEST(server.connection_stats().active_connections <= 4u);

    // The held connections reach their request deadline and free the slots.
    BOOST_TEST(admitted(0));
    for (int fd : held) {
        ::close(fd);
    }
    BOOST_TEST(round_trip());
}

// Case 6b: stop() returns promptly while a connection sits in its request
// phase, without waiting for request_timeout.
template<typename Server>
void stop_is_prompt_with_idle_connection(Server& server, std::uint16_t port) {
    int fd = dial(port);
    BOOST_REQUIRE(fd >= 0);
    BOOST_REQUIRE(wait_until([&] { return server.connection_stats().active_connections == 1; }));
    auto start = std::chrono::steady_clock::now();
    server.stop();
    BOOST_TEST((std::chrono::steady_clock::now() - start < 1000ms));
    BOOST_TEST(server.connection_stats().active_connections == 0u);
    BOOST_TEST(closed_by_server(fd, 1000ms));
    ::close(fd);
}

}  // namespace tcp_server_hardening
