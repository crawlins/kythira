// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Background connection maintenance in the network simulator: keep-alive
// probing (Req 18.5), idle timeouts, and the timer-driven pool cleanup
// (tasks 19.1, 20.2, 22.2).

#define BOOST_TEST_MODULE NetworkSimulatorConnectionMaintenanceTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/network_simulator.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv[] = {const_cast<char*>("test"), nullptr};
        char** argv_ptr = argv;
        init_obj = std::make_unique<folly::Init>(&argc, &argv_ptr);
    }
    std::unique_ptr<folly::Init> init_obj;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

using namespace network_simulator;
using namespace std::chrono_literals;

using TestNetworkSimulator = NetworkSimulator<DefaultNetworkTypes>;
using TestEndpoint = Endpoint<DefaultNetworkTypes>;
using TestConfig = TestNetworkSimulator::ConnectionConfig;

namespace {

constexpr auto client_addr = "client";
constexpr auto server_addr = "server";
constexpr unsigned short server_port = 9000;
constexpr unsigned short client_port = 40000;

// Generous ceiling for anything the maintenance thread should do within a
// few cleanup intervals; only reached when something is broken.
constexpr auto eventually_limit = 5s;

auto eventually(const std::function<bool()>& condition) -> bool {
    auto deadline = std::chrono::steady_clock::now() + eventually_limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

struct Fixture {
    TestNetworkSimulator sim;
    std::shared_ptr<DefaultNetworkTypes::connection_type> client_conn;
    std::shared_ptr<DefaultNetworkTypes::connection_type> server_conn;

    explicit Fixture(const TestConfig& config) {
        sim.configure_connection_management(config);
        sim.seed_rng(7);
        sim.start();
        sim.add_node(client_addr);
        sim.add_node(server_addr);
        connect_edges();

        auto client = sim.create_node(client_addr);
        auto server = sim.create_node(server_addr);
        auto listener = server->bind(server_port).get();
        client_conn = client->connect(server_addr, server_port, client_port).get();
        server_conn = listener->accept(1000ms).get();
        BOOST_REQUIRE(client_conn && client_conn->is_open());
        BOOST_REQUIRE(server_conn && server_conn->is_open());
    }

    void connect_edges() {
        sim.add_edge(client_addr, server_addr, NetworkEdge(1ms, 1.0));
        sim.add_edge(server_addr, client_addr, NetworkEdge(1ms, 1.0));
    }

    void partition() {
        sim.remove_edge(client_addr, server_addr);
        sim.remove_edge(server_addr, client_addr);
    }

    [[nodiscard]] auto client_info() {
        auto info = sim.get_connection_tracker().get_connection_info(
            TestEndpoint{client_addr, client_port});
        BOOST_REQUIRE(info.has_value());
        return *info;
    }

    [[nodiscard]] auto server_info() {
        auto info = sim.get_connection_tracker().get_connection_info(
            TestEndpoint{server_addr, server_port});
        BOOST_REQUIRE(info.has_value());
        return *info;
    }
};

auto keep_alive_config() -> TestConfig {
    TestConfig config;
    config.enable_keep_alive = true;
    config.keep_alive_interval = 30ms;
    config.keep_alive_max_missed = 2;
    config.cleanup_interval = 10ms;
    return config;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(keep_alive)

BOOST_AUTO_TEST_CASE(probes_a_healthy_quiet_connection_and_keeps_it_open,
                     *boost::unit_test::timeout(30)) {
    Fixture f(keep_alive_config());
    auto established = f.client_info().stats.last_activity;

    BOOST_REQUIRE(eventually([&] { return f.client_info().stats.keep_alive_probes_sent >= 3; }));

    auto info = f.client_info();
    BOOST_CHECK(info.state == ConnectionState::CONNECTED);
    BOOST_CHECK_EQUAL(info.stats.keep_alive_probes_missed, 0U);
    BOOST_CHECK(!info.stats.last_error.has_value());
    // A probe is not application activity.
    BOOST_CHECK(info.stats.last_activity == established);
    BOOST_CHECK(f.client_conn->is_open());
}

BOOST_AUTO_TEST_CASE(closes_a_connection_whose_peer_became_unreachable,
                     *boost::unit_test::timeout(30)) {
    Fixture f(keep_alive_config());
    std::atomic<bool> saw_error{false};
    f.sim.get_connection_tracker().set_state_change_callback(
        TestEndpoint{client_addr, client_port}, [&](ConnectionState, ConnectionState to) {
            if (to == ConnectionState::ERROR) {
                saw_error = true;
            }
        });

    f.partition();

    BOOST_REQUIRE(eventually([&] { return !f.client_conn->is_open(); }));
    auto info = f.client_info();
    BOOST_CHECK(info.state == ConnectionState::CLOSED);
    BOOST_CHECK(saw_error.load());
    BOOST_CHECK_GE(info.stats.keep_alive_probes_sent, 2U);
    BOOST_REQUIRE(info.stats.last_error.has_value());
    BOOST_CHECK(info.stats.last_error->find("keep-alive") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(closes_a_connection_whose_peer_closed, *boost::unit_test::timeout(30)) {
    Fixture f(keep_alive_config());

    f.server_conn->close();

    BOOST_REQUIRE(eventually([&] { return !f.client_conn->is_open(); }));
    BOOST_CHECK(f.client_info().state == ConnectionState::CLOSED);
}

BOOST_AUTO_TEST_CASE(a_single_missed_probe_below_the_limit_is_forgiven,
                     *boost::unit_test::timeout(30)) {
    auto config = keep_alive_config();
    config.keep_alive_max_missed = 1000;  // never reached in this test
    Fixture f(config);

    f.partition();
    BOOST_REQUIRE(eventually([&] { return f.client_info().stats.keep_alive_probes_missed >= 2; }));
    f.connect_edges();
    BOOST_REQUIRE(eventually([&] { return f.client_info().stats.keep_alive_probes_missed == 0; }));

    BOOST_CHECK(f.client_info().state == ConnectionState::CONNECTED);
    BOOST_CHECK(f.client_conn->is_open());
}

BOOST_AUTO_TEST_CASE(disabled_by_default, *boost::unit_test::timeout(30)) {
    auto config = keep_alive_config();
    config.enable_keep_alive = false;
    Fixture f(config);

    f.partition();
    std::this_thread::sleep_for(300ms);

    BOOST_CHECK(f.client_conn->is_open());
    BOOST_CHECK(f.client_info().state == ConnectionState::CONNECTED);
    BOOST_CHECK_EQUAL(f.client_info().stats.keep_alive_probes_sent, 0U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(idle_timeout)

BOOST_AUTO_TEST_CASE(closes_idle_connections_in_the_background, *boost::unit_test::timeout(30)) {
    TestConfig config;
    config.enable_idle_timeout = true;
    config.idle_timeout = 100ms;
    config.cleanup_interval = 10ms;
    Fixture f(config);

    BOOST_REQUIRE(eventually([&] { return !f.client_conn->is_open(); }));
    BOOST_REQUIRE(eventually([&] { return !f.server_conn->is_open(); }));
    auto info = f.client_info();
    BOOST_CHECK(info.state == ConnectionState::CLOSED);
    BOOST_REQUIRE(info.stats.last_error.has_value());
    BOOST_CHECK(info.stats.last_error->find("idle") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(receiving_counts_as_activity, *boost::unit_test::timeout(30)) {
    TestConfig config;
    config.enable_idle_timeout = true;
    config.idle_timeout = 150ms;
    config.cleanup_interval = 10ms;
    Fixture f(config);

    // Only the client writes, for several idle timeouts. The server end only
    // receives, and must not be mistaken for idle.
    auto until = std::chrono::steady_clock::now() + 600ms;
    while (std::chrono::steady_clock::now() < until) {
        BOOST_REQUIRE(f.client_conn->write(std::vector<std::byte>{std::byte{1}}).get());
        std::this_thread::sleep_for(20ms);
    }

    BOOST_CHECK(f.client_conn->is_open());
    BOOST_CHECK(f.server_conn->is_open());
    BOOST_CHECK_GT(f.server_info().stats.messages_received, 0U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(maintenance)

BOOST_AUTO_TEST_CASE(stale_pooled_connections_are_dropped_without_an_explicit_call,
                     *boost::unit_test::timeout(30)) {
    TestConfig config;
    config.cleanup_interval = 10ms;
    config.pool_config.max_idle_time = 50ms;
    Fixture f(config);

    auto& pool = f.sim.get_connection_pool();
    pool.return_connection(f.client_conn);
    BOOST_REQUIRE_EQUAL(pool.get_pool_size(TestEndpoint{server_addr, server_port}), 1U);

    BOOST_CHECK(eventually(
        [&] { return pool.get_pool_size(TestEndpoint{server_addr, server_port}) == 0; }));
}

BOOST_AUTO_TEST_CASE(run_maintenance_drives_cleanup_deterministically,
                     *boost::unit_test::timeout(30)) {
    auto config = keep_alive_config();
    config.enable_background_cleanup = false;
    config.keep_alive_interval = 1ms;
    config.keep_alive_max_missed = 1;
    Fixture f(config);

    f.partition();
    std::this_thread::sleep_for(50ms);
    // The thread is running but told not to clean up.
    BOOST_CHECK(f.client_conn->is_open());
    BOOST_CHECK_EQUAL(f.client_info().stats.keep_alive_probes_sent, 0U);

    f.sim.run_maintenance();
    BOOST_CHECK(!f.client_conn->is_open());
    BOOST_CHECK(f.client_info().state == ConnectionState::CLOSED);
}

BOOST_AUTO_TEST_CASE(stop_from_an_observer_on_the_maintenance_thread_does_not_deadlock,
                     *boost::unit_test::timeout(30)) {
    auto f = std::make_unique<Fixture>(keep_alive_config());
    std::atomic<bool> stopped{false};
    f->sim.get_connection_tracker().set_state_change_callback(
        TestEndpoint{client_addr, client_port}, [&](ConnectionState, ConnectionState to) {
            if (to == ConnectionState::ERROR && !stopped.exchange(true)) {
                f->sim.stop();
            }
        });

    f->partition();
    BOOST_REQUIRE(eventually([&] { return stopped.load(); }));

    // Restarting after a self-stop joins the old thread and starts afresh.
    f->sim.start();
    f->sim.stop();
    f.reset();  // destructor joins whatever is left
}

BOOST_AUTO_TEST_CASE(many_short_lived_simulators_start_and_stop_cleanly,
                     *boost::unit_test::timeout(60)) {
    for (int i = 0; i < 200; ++i) {
        TestNetworkSimulator sim;
        sim.start();
        if (i % 2 == 0) {
            sim.stop();  // the rest rely on the destructor
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
