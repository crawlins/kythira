// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * coap_server's in-flight request count (coap-transport Requirement 8).
 *
 * handle_rpc_resource() counts each request against
 * max_concurrent_sessions while it runs. The decrement used to sit in the
 * deleter of a std::unique_ptr that held nullptr, which unique_ptr never
 * calls, so the count only ever went up: after max_concurrent_sessions
 * requests in total -- not at once -- the server rejected every request it
 * received for the rest of its life. Sequential requests are the sharpest
 * test of that, because there is never more than one in flight.
 *
 * The leak never showed, because handle_resource_exhaustion() ran at the top
 * of every request and "closed" connections by subtracting from the same
 * counter whenever it passed three quarters of the limit. That hid the leak,
 * and in exchange every request logged an exhaustion warning, reset the
 * memory pool and switched exhaustion mode on. The second case pins that
 * down by counting the handler's own log line.
 */
#define BOOST_TEST_MODULE coap_server_connection_limit_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/coap_transport.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>  // test_transport_types::executor_type below is folly::Executor directly
#include <raft/coap_transport_impl.hpp>
#include <raft/cbor_serializer.hpp>
#include <raft/console_logger.hpp>
#include <raft/serializer_registry.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "test_timeout_scale.hpp"

using namespace kythira;

namespace {
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint64_t test_node_id = 1;
constexpr auto test_timeout = kythira::testing::scaled_deadline(5000);

using test_serializer = kythira::cbor_rpc_serializer<std::vector<std::byte>>;

// Counts how often coap_server's exhaustion handler runs, by the warning it
// opens with. Global because the server default-constructs its own logger.
std::atomic<std::size_t> exhaustion_handler_runs{0};
constexpr std::string_view exhaustion_message = "Handling server resource exhaustion";

struct exhaustion_counting_logger : kythira::console_logger {
    auto warning(std::string_view message) -> void {
        count(message);
        console_logger::warning(message);
    }
    auto warning(std::string_view message,
                 const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs)
        -> void {
        count(message);
        console_logger::warning(message, key_value_pairs);
    }

private:
    static auto count(std::string_view message) -> void {
        if (message == exhaustion_message) {
            ++exhaustion_handler_runs;
        }
    }
};

struct test_transport_types {
    using serializer_type = test_serializer;
    using serializer_registry_type = kythira::single_serializer_registry<test_serializer>;
    using rpc_serializer_type = test_serializer;
    using metrics_type = kythira::noop_metrics;
    using logger_type = exhaustion_counting_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = folly::Executor;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;

    using future_type = kythira::future_default<kythira::request_vote_response<>>;
};
}  // namespace

BOOST_AUTO_TEST_SUITE(coap_server_connection_limit_tests)

// Far more sequential requests than the limit allows at once. With the leak,
// request number max_concurrent_sessions + 1 came back as an error.
BOOST_AUTO_TEST_CASE(sequential_requests_beyond_the_limit_all_succeed,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    constexpr std::size_t limit = 2;
    constexpr std::uint64_t requests = 10;

    kythira::coap_server_config server_config;
    server_config.enable_dtls = false;
    server_config.max_concurrent_sessions = limit;
    kythira::noop_metrics server_metrics;
    coap_server<test_transport_types> server(test_bind_address, 0, server_config, server_metrics);

    std::atomic<std::uint64_t> handled{0};
    server.register_request_vote_handler(
        [&](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            ++handled;
            return kythira::request_vote_response<>{req.term(), true};
        });
    server.start();

    kythira::coap_client_config client_config;
    client_config.enable_dtls = false;
    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[test_node_id] = std::format("coap://{}:{}", test_bind_address, server.bound_port());
    kythira::noop_metrics client_metrics;
    coap_client<test_transport_types> client(std::move(endpoints), client_config, client_metrics);

    for (std::uint64_t term = 1; term <= requests; ++term) {
        BOOST_TEST_CONTEXT("request " << term << " of " << requests) {
            kythira::request_vote_request<> request{term, 42, 3, 6};
            auto future = client.send_request_vote(test_node_id, request, test_timeout);
            BOOST_REQUIRE(future.wait(test_timeout));
            auto response = std::move(future).get();
            BOOST_TEST(response.term() == term);
            BOOST_TEST(response.vote_granted());
        }
    }
    BOOST_TEST(handled.load() == requests);

    server.stop();
}

// Ordinary traffic, far below the limit, must not run the exhaustion
// handler. It used to run on every request, resetting the memory pool and
// turning exhaustion mode on each time.
BOOST_AUTO_TEST_CASE(requests_below_the_limit_do_not_run_the_exhaustion_handler,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
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

    exhaustion_handler_runs = 0;
    for (std::uint64_t term = 1; term <= 5; ++term) {
        kythira::request_vote_request<> request{term, 42, 3, 6};
        auto future = client.send_request_vote(test_node_id, request, test_timeout);
        BOOST_REQUIRE(future.wait(test_timeout));
        BOOST_TEST(std::move(future).get().vote_granted());
    }
    BOOST_TEST(exhaustion_handler_runs.load() == 0U);

    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
