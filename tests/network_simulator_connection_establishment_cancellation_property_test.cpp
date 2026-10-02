// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE NetworkSimulatorConnectionEstablishmentCancellationPropertyTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/network_simulator.hpp>

#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <atomic>
#include <optional>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>

#endif
using namespace network_simulator;

// Type alias for the correct NetworkSimulator template instantiation
using TestNetworkSimulator = NetworkSimulator<DefaultNetworkTypes>;

// Global fixture to initialize folly
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv[] = {const_cast<char*>("test"), nullptr};
        char** argv_ptr = argv;
        init_obj = std::make_unique<folly::Init>(&argc, &argv_ptr);
    }
    ~FollyInitFixture() = default;

    std::unique_ptr<folly::Init> init_obj;
};
#endif

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {
constexpr std::size_t property_test_iterations = 5;
// Far longer than any test waits, so an attempt that finishes did so because
// it was cancelled, not because the handshake latency ran out.
constexpr std::chrono::milliseconds slow_latency{3000};
constexpr std::chrono::milliseconds long_timeout{10000};
// How long a cancelled attempt may take to give up. Generous for loaded CI
// runners, and still a fraction of `slow_latency`.
constexpr std::chrono::milliseconds prompt_cancel{1000};
constexpr double perfect_reliability = 1.0;

struct Fixture {
    TestNetworkSimulator sim;
    std::string client_addr;
    std::string server_addr;
    std::shared_ptr<DefaultNetworkTypes::node_type> client;
    std::shared_ptr<DefaultNetworkTypes::node_type> server;
    std::shared_ptr<DefaultNetworkTypes::listener_type> listener;
    unsigned short server_port;

    Fixture(std::size_t id, std::chrono::milliseconds latency)
        : client_addr("client_" + std::to_string(id)),
          server_addr("server_" + std::to_string(id)),
          server_port(static_cast<unsigned short>(10000 + id)) {
        sim.seed_rng(static_cast<std::uint32_t>(id));
        sim.start();
        sim.add_node(client_addr);
        sim.add_node(server_addr);
        NetworkEdge edge(latency, perfect_reliability);
        sim.add_edge(client_addr, server_addr, edge);
        sim.add_edge(server_addr, client_addr, edge);
        client = sim.create_node(client_addr);
        server = sim.create_node(server_addr);
        listener = server->bind(server_port).get();
        BOOST_REQUIRE(listener && listener->is_listening());
    }

    [[nodiscard]] auto destination() const -> Endpoint<DefaultNetworkTypes> {
        return {server_addr, server_port};
    }
};

// What a connect attempt running on another thread ended with.
struct Attempt {
    std::atomic<bool> connected{false};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> timed_out{false};
    std::atomic<bool> other_error{false};
    std::chrono::steady_clock::duration elapsed{};
    std::thread thread;

    template<typename Connect> void run(Connect connect) {
        thread = std::thread([this, connect] {
            auto start = std::chrono::steady_clock::now();
            try {
                auto conn = connect().get();
                connected = conn && conn->is_open();
            } catch (const ConnectionCancelledException&) {
                cancelled = true;
            } catch (const TimeoutException&) {
                timed_out = true;
            } catch (const std::exception&) {
                other_error = true;
            }
            elapsed = std::chrono::steady_clock::now() - start;
        });
    }
};

auto wait_for_pending(const TestNetworkSimulator& sim, std::size_t count) -> bool {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (sim.pending_connection_count() < count) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

// Nothing reached the server: accepting finds no queued connection.
void check_listener_empty(Fixture& f) {
    BOOST_CHECK_THROW(f.listener->accept(std::chrono::milliseconds{50}).get(), TimeoutException);
}
}  // namespace

/**
 * Feature: network-simulator, Property 26: Connection Establishment Cancellation
 * Validates: Requirements 15.5
 *
 * Property: For any pending connection establishment operation, when cancellation is
 * requested, the operation SHALL be cancelled and any associated resources SHALL be
 * cleaned up immediately.
 */
BOOST_AUTO_TEST_CASE(property_explicit_cancellation_by_destination,
                     *boost::unit_test::timeout(120)) {
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        Fixture f(i, slow_latency);

        Attempt attempt;
        attempt.run([&] { return f.client->connect(f.server_addr, f.server_port, long_timeout); });

        BOOST_REQUIRE(wait_for_pending(f.sim, 1));
        BOOST_CHECK_EQUAL(f.sim.cancel_pending_connections(f.destination()), 1U);
        attempt.thread.join();

        BOOST_CHECK(attempt.cancelled);
        BOOST_CHECK(!attempt.connected);
        BOOST_CHECK(attempt.elapsed < prompt_cancel);

        // Resources: no request left, no tracked connection, nothing queued.
        BOOST_CHECK_EQUAL(f.sim.pending_connection_count(), 0U);
        BOOST_CHECK(f.sim.get_connection_tracker().get_all_connections().empty());
        check_listener_empty(f);

        // Nothing left to cancel.
        BOOST_CHECK_EQUAL(f.sim.cancel_pending_connections(f.destination()), 0U);
    }
}

BOOST_AUTO_TEST_CASE(cancellation_by_source_and_destination_is_selective,
                     *boost::unit_test::timeout(60)) {
    Fixture f(100, slow_latency);
    const unsigned short src_a = 40001;
    const unsigned short src_b = 40002;

    Attempt a;
    Attempt b;
    a.run([&] { return f.client->connect(f.server_addr, f.server_port, src_a); });
    b.run([&] { return f.client->connect(f.server_addr, f.server_port, src_b); });
    BOOST_REQUIRE(wait_for_pending(f.sim, 2));

    // A source that is not connecting matches nothing.
    BOOST_CHECK_EQUAL(f.sim.cancel_pending_connections({f.client_addr, 40003}, f.destination()),
                      0U);
    BOOST_CHECK_EQUAL(f.sim.cancel_pending_connections({f.client_addr, src_a}, f.destination()),
                      1U);
    a.thread.join();
    BOOST_CHECK(a.cancelled);
    BOOST_CHECK_EQUAL(f.sim.pending_connection_count(), 1U);

    BOOST_CHECK_EQUAL(f.sim.cancel_pending_connections({f.client_addr, src_b}, f.destination()),
                      1U);
    b.thread.join();
    BOOST_CHECK(b.cancelled);
    BOOST_CHECK_EQUAL(f.sim.pending_connection_count(), 0U);
}

BOOST_AUTO_TEST_CASE(cancel_all_cancels_every_concurrent_attempt, *boost::unit_test::timeout(60)) {
    Fixture f(200, slow_latency);
    constexpr std::size_t attempts = 5;

    std::vector<Attempt> running(attempts);
    for (auto& attempt : running) {
        attempt.run([&] { return f.client->connect(f.server_addr, f.server_port, long_timeout); });
    }
    BOOST_REQUIRE(wait_for_pending(f.sim, attempts));

    BOOST_CHECK_EQUAL(f.sim.cancel_all_pending_connections(), attempts);
    for (auto& attempt : running) {
        attempt.thread.join();
        BOOST_CHECK(attempt.cancelled);
        BOOST_CHECK(attempt.elapsed < prompt_cancel);
    }
    BOOST_CHECK_EQUAL(f.sim.pending_connection_count(), 0U);
    BOOST_CHECK(f.sim.get_connection_tracker().get_all_connections().empty());
    check_listener_empty(f);
}

BOOST_AUTO_TEST_CASE(stop_and_reset_abort_in_flight_attempts_promptly,
                     *boost::unit_test::timeout(60)) {
    for (bool use_reset : {false, true}) {
        Fixture f(use_reset ? 301 : 300, slow_latency);

        Attempt attempt;
        attempt.run([&] { return f.client->connect(f.server_addr, f.server_port, long_timeout); });
        BOOST_REQUIRE(wait_for_pending(f.sim, 1));

        if (use_reset) {
            f.sim.reset();
        } else {
            f.sim.stop();
        }
        attempt.thread.join();

        BOOST_CHECK(!attempt.connected);
        BOOST_CHECK(attempt.other_error);  // "Simulator stopped during connection establishment"
        BOOST_CHECK(attempt.elapsed < prompt_cancel);
        BOOST_CHECK_EQUAL(f.sim.pending_connection_count(), 0U);
    }
}

BOOST_AUTO_TEST_CASE(timeout_shorter_than_handshake_fails_with_timeout,
                     *boost::unit_test::timeout(60)) {
    Fixture f(400, slow_latency);
    constexpr std::chrono::milliseconds timeout{100};

    Attempt attempt;
    attempt.run([&] { return f.client->connect(f.server_addr, f.server_port, timeout); });
    attempt.thread.join();

    BOOST_CHECK(attempt.timed_out);
    BOOST_CHECK(attempt.elapsed >= timeout);
    BOOST_CHECK(attempt.elapsed < prompt_cancel);
    BOOST_CHECK_EQUAL(f.sim.pending_connection_count(), 0U);
    check_listener_empty(f);
}

BOOST_AUTO_TEST_CASE(cancellation_does_not_affect_later_attempts, *boost::unit_test::timeout(60)) {
    Fixture f(500, std::chrono::milliseconds{20});

    // Nothing in flight: cancelling is a no-op.
    BOOST_CHECK_EQUAL(f.sim.cancel_all_pending_connections(), 0U);

    auto conn = f.client->connect(f.server_addr, f.server_port, long_timeout).get();
    BOOST_REQUIRE(conn);
    BOOST_CHECK(conn->is_open());
    auto accepted = f.listener->accept(std::chrono::milliseconds{1000}).get();
    BOOST_CHECK(accepted && accepted->is_open());

    // A finished attempt is past cancellation.
    BOOST_CHECK_EQUAL(f.sim.cancel_pending_connections(f.destination()), 0U);
    BOOST_CHECK(conn->is_open());
}
