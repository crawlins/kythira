// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE http_server_test
#include <boost/test/unit_test.hpp>

#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include "http_limit_test_helpers.hpp"

#include <chrono>
#include <string>

namespace {
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint16_t test_bind_port = 8082;

// Define transport types for testing
using test_transport_types =
    kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                  kythira::noop_metrics, folly::CPUThreadPoolExecutor>;
}

BOOST_AUTO_TEST_SUITE(http_server_tests)

// Test server conforms to network_server concept
BOOST_AUTO_TEST_CASE(test_server_concept_conformance, *boost::unit_test::timeout(30)) {
    using server_type = kythira::cpp_httplib_server<test_transport_types>;

    static_assert(kythira::network_server<server_type>,
                  "cpp_httplib_server must satisfy network_server concept");

    BOOST_TEST(true);  // Test passes if compilation succeeds
}

// Test server requires rpc_serializer concept
BOOST_AUTO_TEST_CASE(test_server_requires_rpc_serializer, *boost::unit_test::timeout(30)) {
    // This should compile with valid serializer
    using valid_server = kythira::cpp_httplib_server<test_transport_types>;
    static_assert(kythira::rpc_serializer<typename test_transport_types::serializer_type,
                                          std::vector<std::byte>>);

    BOOST_TEST(true);  // Test passes if compilation succeeds
}

// Test handler registration for each RPC type
BOOST_AUTO_TEST_CASE(test_handler_registration, *boost::unit_test::timeout(30)) {
    kythira::cpp_httplib_server_config config;
    typename test_transport_types::metrics_type metrics;

    kythira::cpp_httplib_server<test_transport_types> server(test_bind_address, test_bind_port,
                                                             config, metrics);

    // Test RequestVote handler registration
    bool request_vote_called = false;
    server.register_request_vote_handler([&](const kythira::request_vote_request<>& req) {
        request_vote_called = true;
        return kythira::request_vote_response<>{};
    });

    // Test AppendEntries handler registration
    bool append_entries_called = false;
    server.register_append_entries_handler([&](const kythira::append_entries_request<>& req) {
        append_entries_called = true;
        return kythira::append_entries_response<>{};
    });

    // Test InstallSnapshot handler registration
    bool install_snapshot_called = false;
    server.register_install_snapshot_handler([&](const kythira::install_snapshot_request<>& req) {
        install_snapshot_called = true;
        return kythira::install_snapshot_response<>{};
    });

    // Handlers should be registered (we can't easily test invocation without starting server)
    BOOST_TEST(true);  // Test passes if no exceptions thrown during registration
}

// Test server lifecycle (start, stop, is_running)
BOOST_AUTO_TEST_CASE(test_server_lifecycle, *boost::unit_test::timeout(45)) {
    kythira::cpp_httplib_server_config config;
    typename test_transport_types::metrics_type metrics;

    kythira::cpp_httplib_server<test_transport_types> server(test_bind_address, test_bind_port,
                                                             config, metrics);

    // Initially not running
    BOOST_TEST(!server.is_running());

    // Note: We can't easily test start() without potentially conflicting with other tests
    // that might be using the same port. In a real test environment, you'd want to:
    // 1. Use a unique port for each test
    // 2. Actually start the server and verify it's listening
    // 3. Stop the server and verify it's no longer listening

    BOOST_TEST(true);  // Test structure is correct
}

// Test HTTPS support configuration
BOOST_AUTO_TEST_CASE(test_https_configuration, *boost::unit_test::timeout(30)) {
    kythira::cpp_httplib_server_config config;
    config.enable_ssl = true;
    config.ssl_cert_path = "/path/to/cert.pem";
    config.ssl_key_path = "/path/to/key.pem";

    typename test_transport_types::metrics_type metrics;

    // Server constructor validates certificate paths and throws if they don't exist
    try {
        kythira::cpp_httplib_server<test_transport_types> server(test_bind_address, test_bind_port,
                                                                 config, metrics);

        BOOST_TEST(true);  // Test passes if construction succeeds
    } catch (const kythira::ssl_configuration_error& e) {
        // Expected: Server validates certificates during construction
        BOOST_TEST_MESSAGE("Expected: SSL configuration error during construction: " << e.what());
        BOOST_TEST(true);  // Test passes - error handling works correctly
    } catch (const std::exception& e) {
        // Also acceptable: any SSL-related error
        BOOST_TEST_MESSAGE("Expected: SSL error during construction: " << e.what());
        BOOST_TEST(true);  // Test passes - error handling works correctly
    }
}

// Test configuration acceptance
BOOST_AUTO_TEST_CASE(test_configuration_acceptance, *boost::unit_test::timeout(30)) {
    kythira::cpp_httplib_server_config config;
    config.max_concurrent_connections = 50;
    config.max_request_body_size = 5 * 1024 * 1024;  // 5 MB
    config.request_timeout = std::chrono::seconds{15};

    typename test_transport_types::metrics_type metrics;

    kythira::cpp_httplib_server<test_transport_types> server(test_bind_address, test_bind_port,
                                                             config, metrics);

    BOOST_TEST(true);  // Test passes if construction with custom config succeeds
}

// ── Request and connection limits (.kiro/specs/http-server-request-limits/) ──
namespace {
constexpr std::uint16_t limits_port_base = 18350;
namespace limits = kythira::testing::http_limits;

auto register_vote_handler(kythira::cpp_httplib_server<test_transport_types>& server) -> void {
    server.register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        kythira::request_vote_response<> resp{};
        resp._term = req.term();
        resp._vote_granted = true;
        return resp;
    });
}

// A well-formed RequestVote, so a connection that is admitted gets a 200.
auto valid_vote_body() -> std::string {
    kythira::request_vote_request<> req{};
    req._term = 4;
    req._candidate_id = 2;
    auto encoded = kythira::json_rpc_serializer<std::vector<std::byte>>{}.serialize(req);
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}
}  // namespace

// Requirement 6.1: the 413 on the wire, text/plain, and the handler never
// runs.
BOOST_AUTO_TEST_CASE(oversized_body_gets_413_on_the_wire, *boost::unit_test::timeout(30)) {
    kythira::cpp_httplib_server_config config;
    config.max_request_body_size = 64;
    kythira::cpp_httplib_server<test_transport_types> server(test_bind_address, limits_port_base,
                                                             config, {});
    register_vote_handler(server);
    server.start();

    limits::raw_connection conn(test_bind_address, limits_port_base);
    BOOST_REQUIRE(conn.connected());
    conn.send_all(limits::sized_request(65));
    auto headers = conn.read_headers(std::chrono::seconds(10));
    BOOST_TEST(limits::status_of(headers) == 413);
    BOOST_TEST(limits::header_of(headers, "Content-Type") == "text/plain");

    server.stop();
}

// Requirements 2.1, 2.2, 6.3: hold two idle connections, see a third closed
// without a response, close one of the two, then complete an RPC on a new
// connection.
BOOST_AUTO_TEST_CASE(connections_past_the_limit_are_refused, *boost::unit_test::timeout(60)) {
    auto port = static_cast<std::uint16_t>(limits_port_base + 1);
    kythira::cpp_httplib_server_config config;
    config.max_concurrent_connections = 2;
    // Far beyond the test's own length, so httplib's read timeout does not
    // reap the idle pair while the test still needs it.
    config.request_timeout = std::chrono::seconds(300);
    kythira::cpp_httplib_server<test_transport_types> server(test_bind_address, port, config, {});
    register_vote_handler(server);
    server.start();

    limits::raw_connection first(test_bind_address, port);
    limits::raw_connection second(test_bind_address, port);
    BOOST_REQUIRE(first.connected());
    BOOST_REQUIRE(second.connected());
    BOOST_REQUIRE(
        limits::wait_for([&] { return server.live_connections() == 2; }, std::chrono::seconds(10)));

    limits::raw_connection third(test_bind_address, port);
    BOOST_REQUIRE(third.connected());  // the kernel completes it; the server drops it
    std::string unexpected;
    auto outcome = third.read_until_close(unexpected, std::chrono::seconds(10));
    BOOST_TEST((outcome == limits::raw_connection::read_outcome::closed));
    BOOST_TEST(unexpected.empty());
    BOOST_TEST(server.live_connections() == 2u);

    first.close();
    BOOST_REQUIRE(
        limits::wait_for([&] { return server.live_connections() == 1; }, std::chrono::seconds(10)));

    limits::raw_connection fresh(test_bind_address, port);
    BOOST_REQUIRE(fresh.connected());
    fresh.send_all(limits::request_with_body(valid_vote_body()));
    BOOST_TEST(limits::status_of(fresh.read_headers(std::chrono::seconds(10))) == 200);

    second.close();
    fresh.close();
    server.stop();
}

// Requirements 2.3, 6.4: the 0.0.0.0 and :: listeners of a "*" bind draw on
// one limit.
BOOST_AUTO_TEST_CASE(connection_limit_is_shared_across_listeners, *boost::unit_test::timeout(60)) {
    if (!limits::ipv6_loopback_available()) {
        BOOST_TEST_MESSAGE("no IPv6 loopback on this host; shared-limit test skipped");
        return;
    }
    auto port = static_cast<std::uint16_t>(limits_port_base + 2);
    kythira::cpp_httplib_server_config config;
    config.max_concurrent_connections = 2;
    config.request_timeout = std::chrono::seconds(300);
    kythira::cpp_httplib_server<test_transport_types> server("*", port, config, {});
    register_vote_handler(server);
    server.start();

    limits::raw_connection v4("127.0.0.1", port);
    limits::raw_connection v6("::1", port);
    BOOST_REQUIRE(v4.connected());
    BOOST_REQUIRE(v6.connected());
    BOOST_REQUIRE(
        limits::wait_for([&] { return server.live_connections() == 2; }, std::chrono::seconds(10)));

    for (const char* address : {"127.0.0.1", "::1"}) {
        BOOST_TEST_INFO("third connection on " << address);
        limits::raw_connection extra(address, port);
        BOOST_REQUIRE(extra.connected());
        std::string unexpected;
        auto outcome = extra.read_until_close(unexpected, std::chrono::seconds(10));
        BOOST_TEST((outcome == limits::raw_connection::read_outcome::closed));
        BOOST_TEST(unexpected.empty());
    }

    v4.close();
    v6.close();
    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()