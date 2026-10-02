// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file http_client_connection_pool_test.cpp
/// @brief kythira::cpp_httplib_client honours `connection_pool_size` and
///        `keep_alive_timeout` (http-transport Requirements 11.2, 11.4, 11.5).
///
/// Both fields used to be accepted and ignored: every peer got one cached
/// connection that lived for the client's lifetime, and every RPC to that
/// peer queued for it. Each test here observes the pool through the
/// client's connection metrics, since a round trip that succeeds says nothing
/// about how many connections carried it.

#define BOOST_TEST_MODULE http_client_connection_pool_test
#include <boost/test/unit_test.hpp>

#include <raft/http_exceptions.hpp>
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>

#include "recording_metrics.hpp"

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
using server_types = kythira::http_transport_types<serializer_type, kythira::noop_metrics,
                                                   folly::CPUThreadPoolExecutor>;
using client_types =
    kythira::http_transport_types<serializer_type, kythira::testing::recording_metrics,
                                  folly::CPUThreadPoolExecutor>;

constexpr const char* bind_address = "127.0.0.1";
constexpr std::uint64_t peer_id = 1;
constexpr auto generous_timeout = std::chrono::milliseconds{5000};

// A peer whose request_vote handler takes `delay` to answer.
struct peer {
    std::unique_ptr<kythira::cpp_httplib_server<server_types>> server;

    peer(std::uint16_t port, std::chrono::milliseconds delay) {
        kythira::cpp_httplib_server_config config;
        typename server_types::metrics_type metrics;
        server = std::make_unique<kythira::cpp_httplib_server<server_types>>(bind_address, port,
                                                                             config, metrics);
        server->register_request_vote_handler([delay](const kythira::request_vote_request<>& req) {
            std::this_thread::sleep_for(delay);
            kythira::request_vote_response<> response;
            response._term = req.term();
            response._vote_granted = true;
            return response;
        });
        server->start();
    }

    ~peer() { server->stop(); }
    peer(const peer&) = delete;
    auto operator=(const peer&) -> peer& = delete;
};

struct pool_client {
    kythira::testing::recording_metrics metrics;
    std::unique_ptr<kythira::cpp_httplib_client<client_types>> client;

    pool_client(std::uint16_t port, std::size_t pool_size,
                std::chrono::milliseconds keep_alive_timeout = std::chrono::milliseconds{60000}) {
        kythira::cpp_httplib_client_config config;
        config.connection_pool_size = pool_size;
        config.keep_alive_timeout = keep_alive_timeout;
        std::unordered_map<std::uint64_t, std::string> node_map;
        node_map[peer_id] = std::format("http://{}:{}", bind_address, port);
        client = std::make_unique<kythira::cpp_httplib_client<client_types>>(std::move(node_map),
                                                                             config, metrics);
    }

    auto vote(std::chrono::milliseconds timeout = generous_timeout)
        -> kythira::request_vote_response<> {
        kythira::request_vote_request<> request;
        request._term = 3;
        request._candidate_id = 2;
        request._last_log_index = 0;
        request._last_log_term = 0;
        return client->send_request_vote(peer_id, request, timeout).get();
    }

    [[nodiscard]] auto count(std::string_view name) const -> std::size_t {
        return metrics.recorder()->count_named(name);
    }

    [[nodiscard]] auto closed_reasons() const -> std::vector<std::string> {
        return metrics.recorder()->dimension_values("http.client.connection.closed", "reason");
    }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(http_client_connection_pool_tests)

// Requirement 11.2: back-to-back RPCs to one peer ride one connection.
BOOST_AUTO_TEST_CASE(sequential_rpcs_reuse_one_connection, *boost::unit_test::timeout(30)) {
    constexpr std::uint16_t port = 18571;
    peer server(port, std::chrono::milliseconds{0});
    pool_client client(port, 4);

    for (int i = 0; i < 3; ++i) {
        BOOST_TEST(client.vote().vote_granted());
    }
    BOOST_TEST(client.count("http.client.connection.created") == 1U);
    BOOST_TEST(client.count("http.client.connection.reused") == 2U);
}

// Requirement 11.5: concurrent RPCs open connections up to the pool size and
// no further; the rest queue for a returned connection and still succeed.
// Before the pool, the four calls queued on a single connection.
BOOST_AUTO_TEST_CASE(concurrent_rpcs_are_bounded_by_pool_size, *boost::unit_test::timeout(30)) {
    constexpr std::uint16_t port = 18572;
    constexpr auto delay = std::chrono::milliseconds{400};
    peer server(port, delay);
    pool_client client(port, 2);

    std::vector<std::thread> callers;
    std::atomic<int> granted{0};
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 4; ++i) {
        callers.emplace_back([&] {
            try {
                if (client.vote().vote_granted()) {
                    ++granted;
                }
            } catch (...) {
            }
        });
    }
    for (auto& t : callers) {
        t.join();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    BOOST_TEST(granted.load() == 4);
    BOOST_TEST(client.count("http.client.connection.created") == 2U);
    // Two waves of two, not one wave of four and not four in a row.
    BOOST_TEST(elapsed >= 2 * delay);
    BOOST_TEST(elapsed < 4 * delay);
}

// Requirement 11.4: a connection idle past keep_alive_timeout is closed and
// the next RPC opens a fresh one.
BOOST_AUTO_TEST_CASE(idle_connection_past_keep_alive_is_closed, *boost::unit_test::timeout(30)) {
    constexpr std::uint16_t port = 18573;
    peer server(port, std::chrono::milliseconds{0});
    pool_client client(port, 4, std::chrono::milliseconds{100});

    BOOST_TEST(client.vote().vote_granted());
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    BOOST_TEST(client.vote().vote_granted());

    BOOST_TEST(client.count("http.client.connection.created") == 2U);
    BOOST_TEST(client.count("http.client.connection.reused") == 0U);
    BOOST_TEST(client.closed_reasons() == std::vector<std::string>{"idle_timeout"});
}

// Within keep_alive_timeout the same connection is reused.
BOOST_AUTO_TEST_CASE(connection_within_keep_alive_is_reused, *boost::unit_test::timeout(30)) {
    constexpr std::uint16_t port = 18574;
    peer server(port, std::chrono::milliseconds{0});
    pool_client client(port, 4, std::chrono::milliseconds{5000});

    BOOST_TEST(client.vote().vote_granted());
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    BOOST_TEST(client.vote().vote_granted());

    BOOST_TEST(client.count("http.client.connection.created") == 1U);
    BOOST_TEST(client.closed_reasons().empty());
}

// A connection whose exchange failed is closed rather than pooled, and the
// next RPC opens a fresh one.
BOOST_AUTO_TEST_CASE(failed_exchange_connection_is_not_pooled, *boost::unit_test::timeout(30)) {
    constexpr std::uint16_t port = 18575;
    peer server(port, std::chrono::milliseconds{600});
    pool_client client(port, 4);

    BOOST_CHECK_THROW(client.vote(std::chrono::milliseconds{100}), kythira::http_timeout_error);
    BOOST_TEST(client.vote().vote_granted());

    BOOST_TEST(client.count("http.client.connection.created") == 2U);
    BOOST_TEST(client.closed_reasons() == std::vector<std::string>{"error"});
}

// A zero pool size still allows one connection rather than deadlocking.
BOOST_AUTO_TEST_CASE(zero_pool_size_is_treated_as_one, *boost::unit_test::timeout(30)) {
    constexpr std::uint16_t port = 18576;
    peer server(port, std::chrono::milliseconds{0});
    pool_client client(port, 0);

    BOOST_TEST(client.vote().vote_granted());
    BOOST_TEST(client.vote().vote_granted());
    BOOST_TEST(client.count("http.client.connection.created") == 1U);
}

BOOST_AUTO_TEST_SUITE_END()
