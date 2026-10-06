// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Unit tests for tcp_detail::connection_tracker and run_accept_loop()
// (.kiro/specs/tcp-rpc-server-hardening/, task 2.4): the connection caps,
// source-address folding, the reaper's deadlines, draining, and an accept
// loop whose thread start fails.
#define BOOST_TEST_MODULE tcp_connection_tracker_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/tcp_connection_tracker.hpp>

#include "tcp_server_hardening_cases.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using kythira::tcp_server_limits;
using kythira::tcp_detail::accept_error_class;
using kythira::tcp_detail::classify_accept_error;
using kythira::tcp_detail::connection_source_key;
using kythira::tcp_detail::connection_tracker;
using phase = connection_tracker::phase;
namespace hardening = tcp_server_hardening;

namespace {

auto v4(const char* ip) -> sockaddr_storage {
    sockaddr_storage ss{};
    auto& in = reinterpret_cast<sockaddr_in&>(ss);
    in.sin_family = AF_INET;
    in.sin_port = htons(4242);
    ::inet_pton(AF_INET, ip, &in.sin_addr);
    return ss;
}

auto v6(const char* ip) -> sockaddr_storage {
    sockaddr_storage ss{};
    auto& in6 = reinterpret_cast<sockaddr_in6&>(ss);
    in6.sin6_family = AF_INET6;
    in6.sin6_port = htons(4242);
    ::inet_pton(AF_INET6, ip, &in6.sin6_addr);
    return ss;
}

// A connected socket pair: `server` is handed to the tracker (which closes
// it), `peer` stays with the test.
struct pair {
    int server{-1};
    int peer{-1};
    pair() {
        int fds[2];
        BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        server = fds[0];
        peer = fds[1];
    }
    ~pair() { ::close(peer); }
    pair(const pair&) = delete;
    auto operator=(const pair&) -> pair& = delete;
};

auto limits(std::size_t total, std::size_t per_source, std::chrono::milliseconds timeout = 30s)
    -> tcp_server_limits {
    tcp_server_limits l;
    l.request_timeout = timeout;
    l.reply_timeout = timeout;
    l.max_connections = total;
    l.max_connections_per_source = per_source;
    return l;
}

}  // namespace

BOOST_AUTO_TEST_CASE(zero_limits_are_rejected) {
    auto with = [](auto mutate) {
        tcp_server_limits l;
        mutate(l);
        return l;
    };
    BOOST_CHECK_NO_THROW(kythira::validate(tcp_server_limits{}, "t"));
    BOOST_CHECK_THROW(kythira::validate(with([](auto& l) { l.request_timeout = 0ms; }), "t"),
                      std::invalid_argument);
    BOOST_CHECK_THROW(kythira::validate(with([](auto& l) { l.reply_timeout = 0ms; }), "t"),
                      std::invalid_argument);
    BOOST_CHECK_THROW(kythira::validate(with([](auto& l) { l.max_connections = 0; }), "t"),
                      std::invalid_argument);
    BOOST_CHECK_THROW(
        kythira::validate(with([](auto& l) { l.max_connections_per_source = 0; }), "t"),
        std::invalid_argument);
    BOOST_CHECK_THROW(connection_tracker::create(with([](auto& l) { l.max_connections = 0; }), "t"),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(accept_errors_are_classified) {
    for (int e : {EBADF, EINVAL, ENOTSOCK}) {
        BOOST_TEST((classify_accept_error(e) == accept_error_class::fatal));
    }
    for (int e : {EMFILE, ENFILE, ENOBUFS, ENOMEM}) {
        BOOST_TEST((classify_accept_error(e) == accept_error_class::backoff));
    }
    for (int e : {EINTR, EAGAIN, ECONNABORTED, EPROTO, EPERM, ENETDOWN, EHOSTUNREACH}) {
        BOOST_TEST((classify_accept_error(e) == accept_error_class::retry));
    }
}

BOOST_AUTO_TEST_CASE(source_key_folds_ipv4_mapped_addresses) {
    BOOST_TEST(connection_source_key(v4("10.1.2.3")) == "10.1.2.3");
    BOOST_TEST(connection_source_key(v6("::ffff:10.1.2.3")) == "10.1.2.3");
    BOOST_TEST(connection_source_key(v6("2001:db8::1")) == "2001:db8::1");
}

BOOST_AUTO_TEST_CASE(caps_refuse_and_free_slots) {
    auto tracker = connection_tracker::create(limits(3, 2), "t");
    pair a1, a2, a3, b1, c1;

    auto t1 = tracker->admit(a1.server, v4("10.0.0.1"));
    auto t2 = tracker->admit(a2.server, v6("::ffff:10.0.0.1"));
    BOOST_REQUIRE(t1 && t2);
    // A third from the same source, even as a mapped address: refused.
    BOOST_TEST(!tracker->admit(a3.server, v4("10.0.0.1")).has_value());
    ::close(a3.server);
    auto t3 = tracker->admit(b1.server, v4("10.0.0.2"));
    BOOST_REQUIRE(t3);
    // The total is reached.
    BOOST_TEST(!tracker->admit(c1.server, v4("10.0.0.3")).has_value());

    auto s = tracker->stats();
    BOOST_TEST(s.active_connections == 3u);
    BOOST_TEST(s.refused_per_source_limit == 1u);
    BOOST_TEST(s.refused_global_limit == 1u);

    // Releasing a ticket closes its socket and frees both its slots.
    t1.reset();
    char b = 0;
    BOOST_TEST(::read(a1.peer, &b, 1) == 0);
    BOOST_TEST(tracker->stats().active_connections == 2u);
    auto t4 = tracker->admit(c1.server, v4("10.0.0.1"));
    BOOST_TEST(t4.has_value());
}

BOOST_AUTO_TEST_CASE(reaper_shuts_down_overdue_request_phase, *boost::unit_test::timeout(10)) {
    auto tracker = connection_tracker::create(limits(8, 8, 150ms), "t");
    tracker->start_reaper();
    pair p;
    auto t = tracker->admit(p.server, v4("10.0.0.1"));
    BOOST_REQUIRE(t);
    auto start = std::chrono::steady_clock::now();
    BOOST_TEST(hardening::closed_by_server(p.peer, 2000ms));
    auto took = std::chrono::steady_clock::now() - start;
    BOOST_TEST((took >= 100ms));
    BOOST_TEST((took < 1000ms));
    BOOST_TEST(tracker->stats().timed_out == 1u);
    // Shut down, not closed: the ticket still owns the descriptor.
    BOOST_TEST(tracker->stats().active_connections == 1u);
    t.reset();
    tracker->shutdown_and_drain();
}

BOOST_AUTO_TEST_CASE(handler_phase_has_no_deadline, *boost::unit_test::timeout(10)) {
    auto tracker = connection_tracker::create(limits(8, 8, 100ms), "t");
    tracker->start_reaper();
    pair p;
    auto t = tracker->admit(p.server, v4("10.0.0.1"));
    BOOST_REQUIRE(t);
    t->enter(phase::handler);
    BOOST_TEST(!hardening::closed_by_server(p.peer, 400ms));
    BOOST_TEST(tracker->stats().timed_out == 0u);
    // The reply phase has a deadline again.
    t->enter(phase::reply);
    BOOST_TEST(hardening::closed_by_server(p.peer, 2000ms));
    BOOST_TEST(tracker->stats().timed_out == 1u);
    t.reset();
    tracker->shutdown_and_drain();
}

BOOST_AUTO_TEST_CASE(drain_waits_for_tickets_and_refuses_new_ones, *boost::unit_test::timeout(10)) {
    auto tracker = connection_tracker::create(limits(8, 8), "t");
    tracker->start_reaper();
    pair p, q;
    auto t = tracker->admit(p.server, v4("10.0.0.1"));
    BOOST_REQUIRE(t);
    t->enter(phase::handler);

    auto drained = std::async(std::launch::async, [&] { tracker->shutdown_and_drain(); });
    BOOST_TEST((drained.wait_for(200ms) == std::future_status::timeout));
    // Draining shuts every socket down, even in the handler phase.
    BOOST_TEST(hardening::closed_by_server(p.peer, 1000ms));
    BOOST_TEST(!tracker->admit(q.server, v4("10.0.0.2")).has_value());
    ::close(q.server);
    t.reset();
    drained.get();
    BOOST_TEST(tracker->stats().active_connections == 0u);

    // A drained tracker accepts again once restarted.
    tracker->start_reaper();
    pair r;
    BOOST_TEST(tracker->admit(r.server, v4("10.0.0.1")).has_value());
    tracker->shutdown_and_drain();
}

BOOST_AUTO_TEST_CASE(accept_loop_survives_failing_thread_start, *boost::unit_test::timeout(20)) {
    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(lfd >= 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOST_REQUIRE(::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    BOOST_REQUIRE(::listen(lfd, 16) == 0);
    socklen_t len = sizeof(a);
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &len);
    auto port = ntohs(a.sin_port);

    auto tracker = connection_tracker::create(limits(8, 8), "t");
    tracker->start_reaper();
    kythira::tcp_detail::accept_backoff backoff;
    std::atomic<bool> running{true};
    std::atomic<int> spawned{0};
    std::thread loop([&] {
        kythira::tcp_detail::run_accept_loop(
            lfd, running, *tracker, backoff, [&](connection_tracker::ticket t) {
                ++spawned;
                (void)t;
                throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again));
            });
    });

    for (int i = 0; i < 3; ++i) {
        int fd = hardening::dial(port);
        BOOST_REQUIRE(fd >= 0);
        // The connection is closed, not leaked, and its slot is free again.
        BOOST_TEST(hardening::closed_by_server(fd, 2000ms));
        ::close(fd);
        BOOST_TEST(hardening::wait_until([&] { return spawned.load() == i + 1; }));
        BOOST_TEST(tracker->stats().active_connections == 0u);
    }

    running = false;
    backoff.wake();
    ::shutdown(lfd, SHUT_RDWR);
    loop.join();
    ::close(lfd);
    tracker->shutdown_and_drain();
}
