// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE BoundedResolverTest
#include <boost/test/unit_test.hpp>

#include <raft/bounded_resolver.hpp>
#include <raft/net_bind.hpp>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace nr = kythira::net_resolve;
using namespace std::chrono_literals;

namespace {

using clock_type = std::chrono::steady_clock;

auto elapsed_since(clock_type::time_point start) -> std::chrono::milliseconds {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - start);
}

// A lookup that blocks for `delay`, the way getaddrinfo() does for a stopped
// container's name under rootless Podman, and counts how often it ran.
auto slow_lookup(std::chrono::milliseconds delay, std::atomic<int>& runs) {
    return [delay, &runs]() -> nr::lookup_outcome {
        ++runs;
        std::this_thread::sleep_for(delay);
        return {std::nullopt, false};
    };
}

auto found(nr::resolved_address a) -> nr::lookup_outcome {
    return {std::vector<nr::resolved_address>{a}, false};
}

auto address(int port) -> nr::resolved_address {
    nr::resolved_address a;
    auto& in = reinterpret_cast<sockaddr_in&>(a.addr);
    in.sin_family = AF_INET;
    in.sin_port = htons(static_cast<std::uint16_t>(port));
    a.family = AF_INET;
    a.len = sizeof(sockaddr_in);
    return a;
}

auto port_of(const nr::resolved_address& a) -> int {
    return ntohs(reinterpret_cast<const sockaddr_in&>(a.addr).sin_port);
}

}  // namespace

BOOST_AUTO_TEST_CASE(an_address_literal_resolves_without_a_lookup) {
    auto v4 = nr::resolve("127.0.0.1", "7000", SOCK_STREAM, 1ms);
    BOOST_REQUIRE(v4.has_value());
    BOOST_REQUIRE_EQUAL(v4->size(), 1u);
    BOOST_CHECK_EQUAL(v4->front().family, AF_INET);
    BOOST_CHECK_EQUAL(ntohs(reinterpret_cast<const sockaddr_in&>(v4->front().addr).sin_port), 7000);

    auto v6 = nr::resolve("::1", "7000", SOCK_STREAM, 1ms);
    BOOST_REQUIRE(v6.has_value());
    BOOST_CHECK_EQUAL(v6->front().family, AF_INET6);
}

BOOST_AUTO_TEST_CASE(localhost_resolves) {
    auto r = nr::resolve("localhost", "80", SOCK_STREAM, 5s);
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(!r->empty());
}

// The bug this header exists for: the caller used to wait as long as the
// lookup took. Now it waits `timeout`, and the lookup carries on without it.
BOOST_AUTO_TEST_CASE(a_slow_lookup_is_abandoned_at_the_deadline) {
    std::atomic<int> runs{0};
    auto start = clock_type::now();
    auto r = nr::resolve_using("slow-deadline", 100ms, slow_lookup(1500ms, runs));
    auto waited = elapsed_since(start);
    BOOST_CHECK(!r.has_value());
    BOOST_CHECK_GE(waited.count(), 100);
    BOOST_CHECK_LT(waited.count(), 1000);
    BOOST_CHECK_EQUAL(runs.load(), 1);
}

// A dead peer is dialled on every heartbeat. Each RPC must not start its
// own lookup: they share the one in flight, and only the one that started
// it waits, so one thread stays blocked however many RPCs ask.
BOOST_AUTO_TEST_CASE(concurrent_callers_share_one_lookup) {
    std::atomic<int> runs{0};
    std::atomic<int> answered{0};
    std::atomic<int> waited{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < 8; ++i) {
        waiters.emplace_back([&] {
            auto start = clock_type::now();
            if (nr::resolve_using("slow-shared", 200ms, slow_lookup(800ms, runs)).has_value()) {
                ++answered;
            }
            if (elapsed_since(start) >= 150ms) {
                ++waited;
            }
        });
    }
    for (auto& w : waiters) {
        w.join();
    }
    BOOST_CHECK_EQUAL(answered.load(), 0);
    BOOST_CHECK_EQUAL(runs.load(), 1);
    BOOST_CHECK_EQUAL(waited.load(), 1);
}

// A peer that is up answers from the last lookup: the RPC never waits on
// DNS once the name has resolved.
BOOST_AUTO_TEST_CASE(an_answer_is_reused_while_it_is_fresh) {
    std::atomic<int> runs{0};
    auto lookup = [&runs] {
        ++runs;
        return found(address(1));
    };
    BOOST_REQUIRE(nr::resolve_using("fresh", 2s, lookup, 10s).has_value());
    BOOST_REQUIRE(nr::resolve_using("fresh", 2s, lookup, 10s).has_value());
    BOOST_CHECK_EQUAL(runs.load(), 1);
}

// A dead peer is dialled on every heartbeat. Once its name has failed to
// resolve, later RPCs fail at once instead of each waiting out a lookup.
BOOST_AUTO_TEST_CASE(a_failure_is_remembered) {
    std::atomic<int> runs{0};
    BOOST_CHECK(!nr::resolve_using("failed", 0ms, slow_lookup(10ms, runs), 10s).has_value());
    auto start = clock_type::now();
    BOOST_CHECK(!nr::resolve_using("failed", 5s, slow_lookup(1500ms, runs), 10s).has_value());
    BOOST_CHECK_LT(elapsed_since(start).count(), 500);
    BOOST_CHECK_EQUAL(runs.load(), 1);
}

// An answer past its age is still handed out at once, and the lookup that
// replaces it runs in the background: a peer that moved is seen after that
// lookup, and nobody waits on it.
BOOST_AUTO_TEST_CASE(an_aged_answer_is_served_while_it_is_refreshed) {
    auto answer = [](int port, std::chrono::milliseconds delay) {
        return [port, delay] {
            std::this_thread::sleep_for(delay);
            return found(address(port));
        };
    };
    auto first = nr::resolve_using("aged", 2s, answer(1, 0ms), 0ms);
    BOOST_REQUIRE(first.has_value());
    BOOST_CHECK_EQUAL(port_of(first->front()), 1);

    auto start = clock_type::now();
    auto stale = nr::resolve_using("aged", 2s, answer(2, 300ms), 0ms);
    BOOST_CHECK_LT(elapsed_since(start).count(), 200);
    BOOST_REQUIRE(stale.has_value());
    BOOST_CHECK_EQUAL(port_of(stale->front()), 1);

    std::this_thread::sleep_for(600ms);
    auto fresh = nr::resolve_using("aged", 2s, answer(3, 0ms), 10s);
    BOOST_REQUIRE(fresh.has_value());
    BOOST_CHECK_EQUAL(port_of(fresh->front()), 2);
}

// aardvark-dns answers nothing while it forwards a dead name upstream, so a
// running peer's lookup can fail for a while. That must not cost the
// address it had; only a definite "no such name" does.
BOOST_AUTO_TEST_CASE(a_transient_failure_keeps_the_last_address) {
    auto failing = [](bool transient) {
        return [transient] { return nr::lookup_outcome{std::nullopt, transient}; };
    };
    BOOST_REQUIRE(nr::resolve_using("kept", 2s, [] { return found(address(4)); }, 0ms));

    auto after_transient = nr::resolve_using("kept", 2s, failing(true), 0ms);  // stale, served
    BOOST_REQUIRE(after_transient.has_value());
    BOOST_CHECK_EQUAL(port_of(after_transient->front()), 4);
    std::this_thread::sleep_for(50ms);  // the transient failure has landed
    auto still_kept = nr::resolve_using("kept", 2s, failing(false), 0ms);
    BOOST_REQUIRE(still_kept.has_value());
    BOOST_CHECK_EQUAL(port_of(still_kept->front()), 4);

    std::this_thread::sleep_for(50ms);  // the definite failure has landed
    BOOST_CHECK(!nr::resolve_using("kept", 2s, failing(false), 10s).has_value());
}

BOOST_AUTO_TEST_CASE(a_waiter_that_outlasts_the_lookup_gets_its_answer) {
    auto a = address(1);
    auto r = nr::resolve_using("slow-answer", 2s, [a] {
        std::this_thread::sleep_for(50ms);
        return found(a);
    });
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK_EQUAL(r->size(), 1u);
}

// connect_to() spends its whole budget, resolution included, on one call.
BOOST_AUTO_TEST_CASE(connect_to_a_literal_still_connects) {
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE_GE(srv, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOST_REQUIRE_EQUAL(::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
    BOOST_REQUIRE_EQUAL(::listen(srv, 1), 0);
    socklen_t len = sizeof addr;
    ::getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &len);

    int fd = kythira::net_bind::connect_to("127.0.0.1", ntohs(addr.sin_port), 1s);
    BOOST_CHECK_GE(fd, 0);
    if (fd >= 0) {
        ::close(fd);
    }
    ::close(srv);
}

// A cold name is worth waiting for, but not for the whole RPC timeout: the
// lookup keeps running and the next RPC collects its answer, while this
// thread goes back to peers that do answer.
BOOST_AUTO_TEST_CASE(a_cold_name_waits_no_longer_than_the_cap) {
    std::atomic<int> runs{0};
    auto start = clock_type::now();
    auto r = nr::resolve_using("cold-cap", 10s, slow_lookup(1500ms, runs), 10s, 100ms);
    auto waited = elapsed_since(start);
    BOOST_CHECK(!r.has_value());
    BOOST_CHECK_GE(waited.count(), 100);
    BOOST_CHECK_LT(waited.count(), 1000);
    BOOST_CHECK_EQUAL(runs.load(), 1);
}

// Podman gives a restarted container a new address. Once the dialler has
// reported the old one unreachable, the next caller waits for the lookup
// that replaces it instead of dialling the old one again.
BOOST_AUTO_TEST_CASE(an_unreachable_address_is_not_handed_out_again) {
    std::atomic<int> runs{0};
    auto answer = [&runs](int port, std::chrono::milliseconds delay) {
        return [&runs, port, delay] {
            ++runs;
            std::this_thread::sleep_for(delay);
            return found(address(port));
        };
    };
    auto old = nr::resolve_using("moved", 2s, answer(1, 0ms), 10s);
    BOOST_REQUIRE(old.has_value());
    BOOST_CHECK_EQUAL(port_of(old->front()), 1);

    nr::mark_unreachable("moved");
    auto fresh = nr::resolve_using("moved", 2s, answer(2, 50ms), 10s);
    BOOST_REQUIRE(fresh.has_value());
    BOOST_CHECK_EQUAL(port_of(fresh->front()), 2);
    BOOST_CHECK_EQUAL(runs.load(), 2);

    // Confirmed again: served from the answer, no lookup, no suspicion.
    auto again = nr::resolve_using("moved", 2s, answer(3, 0ms), 10s);
    BOOST_REQUIRE(again.has_value());
    BOOST_CHECK_EQUAL(port_of(again->front()), 2);
    BOOST_CHECK_EQUAL(runs.load(), 2);
}

// A transient failure keeps a running peer's address, but not one the
// dialler could not reach: nothing confirms it, so it goes.
BOOST_AUTO_TEST_CASE(an_unreachable_address_is_dropped_on_a_transient_failure) {
    std::atomic<int> runs{0};
    BOOST_REQUIRE(nr::resolve_using("gone", 2s, [] { return found(address(1)); }, 10s));
    nr::mark_unreachable("gone");
    BOOST_CHECK(!nr::resolve_using(
                     "gone", 2s, [] { return nr::lookup_outcome{std::nullopt, true}; }, 10s)
                     .has_value());
    auto start = clock_type::now();
    BOOST_CHECK(!nr::resolve_using("gone", 2s, slow_lookup(1500ms, runs), 10s).has_value());
    BOOST_CHECK_LT(elapsed_since(start).count(), 500);
    BOOST_CHECK_EQUAL(runs.load(), 0);  // the failure is remembered, not retried yet
}

// While the lookup confirming a suspect address is stuck, callers wait the
// cold cap and then fail, rather than dialling the suspect address or
// waiting out the RPC timeout.
BOOST_AUTO_TEST_CASE(a_suspect_address_waits_no_longer_than_the_cap) {
    std::atomic<int> runs{0};
    BOOST_REQUIRE(nr::resolve_using("stuck", 2s, [] { return found(address(1)); }, 10s));
    nr::mark_unreachable("stuck");
    auto start = clock_type::now();
    auto r = nr::resolve_using("stuck", 10s, slow_lookup(1500ms, runs), 10s, 100ms);
    auto waited = elapsed_since(start);
    BOOST_CHECK(!r.has_value());
    BOOST_CHECK_GE(waited.count(), 100);
    BOOST_CHECK_LT(waited.count(), 1000);
    BOOST_CHECK_EQUAL(runs.load(), 1);
}

// connect_to() reports a name unreachable when every address it resolved
// to refused, so the next call looks the name up again.
BOOST_AUTO_TEST_CASE(connect_to_marks_a_refused_name_unreachable) {
    // A port nothing listens on: bind, read the port back, close.
    int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE_GE(probe, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOST_REQUIRE_EQUAL(::bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
    socklen_t len = sizeof addr;
    ::getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(probe);
    const int port = ntohs(addr.sin_port);

    auto loopback = [port] {
        auto a = address(port);
        reinterpret_cast<sockaddr_in&>(a.addr).sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.socktype = SOCK_STREAM;
        return found(a);
    };
    const auto key = nr::detail::key_for("localhost", std::to_string(port), SOCK_STREAM);
    BOOST_REQUIRE(nr::resolve_using(key, 2s, loopback, 10s).has_value());

    BOOST_CHECK_LT(kythira::net_bind::connect_to("localhost", static_cast<std::uint16_t>(port), 1s),
                   0);
    BOOST_CHECK_EQUAL(errno, ECONNREFUSED);

    std::atomic<int> runs{0};
    auto relooked = nr::resolve_using(
        key, 2s,
        [&runs] {
            ++runs;
            return found(address(2));
        },
        10s);
    BOOST_REQUIRE(relooked.has_value());
    BOOST_CHECK_EQUAL(port_of(relooked->front()), 2);
    BOOST_CHECK_EQUAL(runs.load(), 1);
}

// A peer that sends us an RPC has told us its address. The resolver uses
// it only while DNS has nothing usable: a suspect name, or one whose last
// lookup failed. A resolved address always wins.
BOOST_AUTO_TEST_CASE(a_learned_address_stands_in_while_the_name_is_suspect) {
    BOOST_REQUIRE(nr::resolve_using("spoke", 2s, [] { return found(address(1)); }, 10s));
    nr::learn("spoke", {address(9)});
    auto resolved = nr::resolve_using("spoke", 2s, [] { return found(address(1)); }, 10s);
    BOOST_REQUIRE(resolved.has_value());
    BOOST_CHECK_EQUAL(port_of(resolved->front()), 1);  // DNS wins while usable

    nr::mark_unreachable("spoke");
    nr::learn("spoke", {address(9)});  // newer than the report
    std::atomic<int> runs{0};
    auto start = clock_type::now();
    auto hinted = nr::resolve_using("spoke", 10s, slow_lookup(1500ms, runs), 10s);
    BOOST_CHECK_LT(elapsed_since(start).count(), 500);  // no wait: the hint answers
    BOOST_REQUIRE(hinted.has_value());
    BOOST_CHECK_EQUAL(port_of(hinted->front()), 9);
    std::this_thread::sleep_for(50ms);  // the lookup still runs, on its own thread
    BOOST_CHECK_EQUAL(runs.load(), 1);
}

BOOST_AUTO_TEST_CASE(a_learned_address_stands_in_after_a_failed_lookup) {
    BOOST_CHECK(!nr::resolve_using(
        "silent", 2s, [] { return nr::lookup_outcome{std::nullopt, true}; }, 10s));
    nr::learn("silent", {address(5)});
    auto hinted = nr::resolve_using("silent", 2s, [] { return found(address(1)); }, 10s);
    BOOST_REQUIRE(hinted.has_value());
    BOOST_CHECK_EQUAL(port_of(hinted->front()), 5);
}

// A hint older than the last unreachable report was already tried: it is
// not dialled again until the peer speaks again.
BOOST_AUTO_TEST_CASE(a_learned_address_is_retired_by_an_unreachable_report) {
    nr::learn("retired", {address(5)});
    BOOST_REQUIRE(nr::resolve_using(
        "retired", 2s, [] { return nr::lookup_outcome{std::nullopt, true}; }, 10s));
    nr::mark_unreachable("retired");
    std::this_thread::sleep_for(20ms);
    BOOST_CHECK(!nr::resolve_using(
                     "retired", 2s, [] { return nr::lookup_outcome{std::nullopt, true}; }, 10s)
                     .has_value());
    nr::learn("retired", {address(6)});
    auto again = nr::resolve_using(
        "retired", 2s, [] { return nr::lookup_outcome{std::nullopt, true}; }, 10s);
    BOOST_REQUIRE(again.has_value());
    BOOST_CHECK_EQUAL(port_of(again->front()), 6);
}

BOOST_AUTO_TEST_CASE(address_from_keeps_the_host_and_takes_the_peers_port) {
    sockaddr_storage from{};
    auto& sin = reinterpret_cast<sockaddr_in&>(from);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(40000);
    sin.sin_addr.s_addr = htonl(0x0A000001);
    auto a = nr::address_from(from, sizeof(sockaddr_in), 7000, SOCK_STREAM);
    BOOST_REQUIRE(a.has_value());
    BOOST_CHECK_EQUAL(port_of(*a), 7000);
    BOOST_CHECK_EQUAL(ntohl(reinterpret_cast<const sockaddr_in&>(a->addr).sin_addr.s_addr),
                      0x0A000001u);
    BOOST_CHECK_EQUAL(a->family, AF_INET);
    BOOST_CHECK(!nr::address_from(sockaddr_storage{}, 0, 7000, SOCK_STREAM).has_value());
}

// A dial that fails retires only the hints that were there when it began:
// one that arrived while it was failing was never tried.
BOOST_AUTO_TEST_CASE(an_unreachable_report_keeps_hints_newer_than_the_dial) {
    auto transient = [] { return nr::lookup_outcome{std::nullopt, true}; };
    nr::learn("during", {address(5)});
    BOOST_REQUIRE(nr::resolve_using("during", 2s, transient, 10s).has_value());
    auto dialled_at = clock_type::now();
    std::this_thread::sleep_for(20ms);
    nr::learn("during", {address(6)});  // the peer spoke while the dial was failing
    nr::mark_unreachable("during", dialled_at);
    auto kept = nr::resolve_using("during", 2s, transient, 10s);
    BOOST_REQUIRE(kept.has_value());
    BOOST_CHECK_EQUAL(port_of(kept->front()), 6);
}

// A SYN nobody answers costs at most max_connect_attempt, whatever the RPC
// timeout: 192.0.2.1 (TEST-NET-1) is never routable to a host.
BOOST_AUTO_TEST_CASE(connect_to_gives_up_on_a_silent_address_before_the_rpc_timeout) {
    auto start = clock_type::now();
    int fd = kythira::net_bind::connect_to("192.0.2.1", 7000, 10s);
    auto waited = elapsed_since(start);
    BOOST_CHECK_LT(fd, 0);
    BOOST_CHECK_LT(waited.count(), kythira::net_bind::max_connect_attempt.count() + 1000);
    if (fd >= 0) {
        ::close(fd);
    }
}
