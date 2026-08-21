// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * End-to-end sanity check for cbor_rpc_serializer over the real CoAP
 * transport (.kiro/specs/cbor-rpc-serializer/tasks.md, task 10.3).
 *
 * Unlike the other coap_*_test.cpp files (which mostly exercise the client
 * or server in isolation, or check interfaces without a real network round
 * trip), this test starts a real coap_server bound to localhost, points a
 * real coap_client at it, and drives a full RequestVote/AppendEntries/
 * InstallSnapshot cycle over an actual UDP socket -- both sides configured
 * with cbor_rpc_serializer as Types::serializer_type.
 */
#define BOOST_TEST_MODULE coap_cbor_end_to_end_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/coap_transport.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>  // test_transport_types::executor_type below is folly::Executor directly
#include <raft/coap_transport_impl.hpp>
#include <raft/cbor_serializer.hpp>
#include <raft/console_logger.hpp>
#include <raft/coap_utils.hpp>
#include <raft/serializer_registry.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "test_timeout_scale.hpp"

using namespace kythira;

namespace {
constexpr const char* test_bind_address = "127.0.0.1";
// Servers here bind port 0 and the client endpoint is built from
// server.bound_port() after start(), so nothing in this file depends on a
// fixed port being free. A literal would work today -- no other binding coap
// test uses 57831 -- but it silently assumes both that the port is unoccupied
// on the runner and that no future test picks the same number.
constexpr std::uint64_t test_node_id = 1;
// Scaled, because this is handed *into* the code under test -- it is both the
// RPC's own deadline and the budget of the BOOST_REQUIRE(future.wait(...)) that
// follows it. Scaling each case's timeout() never reaches it, so at CI's
// KYTHIRA_TEST_TIMEOUT_SCALE=4 the case gets 120s while the exchange inside it
// would keep 5s. Same shape as the coap_negotiation_failure_test deadline that
// failed the boost leg 3/3 attempts on two consecutive runs.
constexpr auto test_timeout = kythira::testing::scaled_deadline(5000);

using test_serializer = kythira::cbor_rpc_serializer<std::vector<std::byte>>;

// Matches the pattern used by every other real (non-stub) coap_*_test.cpp:
// future_template<T> must stay genuinely parameterized over T because
// coap_client has three RPCs with three distinct Response types.
struct test_transport_types {
    using serializer_type = test_serializer;
    using serializer_registry_type = kythira::single_serializer_registry<test_serializer>;
    using rpc_serializer_type = test_serializer;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = folly::Executor;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;

    using future_type = kythira::future_default<kythira::request_vote_response<>>;
};
}  // namespace

BOOST_AUTO_TEST_SUITE(coap_cbor_end_to_end_tests)

// **Feature: cbor-rpc-serializer, task 10.3: End-to-end sanity check over CoAP transport**
// Property: a node pair configured with cbor_serializer as Types::serializer_type
// completes a full RequestVote/AppendEntries/InstallSnapshot cycle, with
// application/cbor observed as the CoAP Content-Format.
BOOST_AUTO_TEST_CASE(test_cbor_content_format_mapping) {
    test_serializer serializer;
    BOOST_TEST(serializer.name() == "cbor");

    auto format = coap_utils::get_content_format_for_serializer(serializer.name());
    BOOST_TEST(static_cast<int>(format) ==
               static_cast<int>(coap_utils::coap_content_format::application_cbor));
    BOOST_TEST(coap_utils::content_format_to_string(format) == "application/cbor");
}

BOOST_AUTO_TEST_CASE(test_cbor_request_vote_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::coap_server_config server_config;
    server_config.enable_dtls = false;
    kythira::noop_metrics server_metrics;
    coap_server<test_transport_types> server(test_bind_address, 0, server_config, server_metrics);

    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            return kythira::request_vote_response<>{req.term(), true};
        });
    server.start();

    kythira::coap_client_config client_config;
    client_config.enable_dtls = false;
    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[test_node_id] = std::format("coap://{}:{}", test_bind_address, server.bound_port());
    kythira::noop_metrics client_metrics;
    coap_client<test_transport_types> client(std::move(endpoints), client_config, client_metrics);

    kythira::request_vote_request<> request{7, 42, 3, 6};
    auto future = client.send_request_vote(test_node_id, request, test_timeout);

    BOOST_REQUIRE(future.wait(test_timeout));
    auto response = std::move(future).get();
    BOOST_TEST(response.term() == 7);
    BOOST_TEST(response.vote_granted());

    server.stop();
}

BOOST_AUTO_TEST_CASE(test_cbor_append_entries_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::coap_server_config server_config;
    server_config.enable_dtls = false;
    kythira::noop_metrics server_metrics;
    coap_server<test_transport_types> server(test_bind_address, 0, server_config, server_metrics);

    server.register_append_entries_handler(
        [](const kythira::append_entries_request<>& req) -> kythira::append_entries_response<> {
            return kythira::append_entries_response<>{req.term(), true, std::nullopt, std::nullopt};
        });
    server.start();

    kythira::coap_client_config client_config;
    client_config.enable_dtls = false;
    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[test_node_id] = std::format("coap://{}:{}", test_bind_address, server.bound_port());
    kythira::noop_metrics client_metrics;
    coap_client<test_transport_types> client(std::move(endpoints), client_config, client_metrics);

    kythira::append_entries_request<> request{7, 1, 2, 6, {}, 1};
    auto future = client.send_append_entries(test_node_id, request, test_timeout);

    BOOST_REQUIRE(future.wait(test_timeout));
    auto response = std::move(future).get();
    BOOST_TEST(response.term() == 7);
    BOOST_TEST(response.success());

    server.stop();
}

BOOST_AUTO_TEST_CASE(test_cbor_install_snapshot_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::coap_server_config server_config;
    server_config.enable_dtls = false;
    kythira::noop_metrics server_metrics;
    coap_server<test_transport_types> server(test_bind_address, 0, server_config, server_metrics);

    server.register_install_snapshot_handler(
        [](const kythira::install_snapshot_request<>& req) -> kythira::install_snapshot_response<> {
            return kythira::install_snapshot_response<>{req.term()};
        });
    server.start();

    kythira::coap_client_config client_config;
    client_config.enable_dtls = false;
    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[test_node_id] = std::format("coap://{}:{}", test_bind_address, server.bound_port());
    kythira::noop_metrics client_metrics;
    coap_client<test_transport_types> client(std::move(endpoints), client_config, client_metrics);

    std::vector<std::byte> snapshot_data{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    kythira::install_snapshot_request<> request{7, 1, 5, 6, 0, snapshot_data, true};
    auto future = client.send_install_snapshot(test_node_id, request, test_timeout);

    BOOST_REQUIRE(future.wait(test_timeout));
    auto response = std::move(future).get();
    BOOST_TEST(response.term() == 7);

    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
