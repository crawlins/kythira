// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file http_client_per_call_timeout_test.cpp
/// @brief kythira::cpp_httplib_client bounds each RPC by the caller's timeout
///        (http-transport Requirement 12), not only by the per-peer
///        `request_timeout` the client was configured with.
///
/// Raft passes its election and heartbeat timeouts into every send_* call. If
/// the transport ignores them, a stalled peer holds an RPC for the full
/// configured request timeout (10 s by default), far past the point where Raft
/// has moved on. Each test pairs a slow handler with a configured timeout much
/// longer than the per-call one, so only an enforced per-call deadline can make
/// the RPC give up in time.

#define BOOST_TEST_MODULE http_client_per_call_timeout_test
#include <boost/test/unit_test.hpp>

#include <raft/http_exceptions.hpp>
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

using test_types =
    kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                  kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

constexpr const char* bind_address = "127.0.0.1";
constexpr std::uint64_t peer_id = 1;
constexpr auto handler_delay = std::chrono::milliseconds{1500};

// A peer whose request_vote handler takes `handler_delay` to answer.
struct slow_peer {
    std::unique_ptr<kythira::cpp_httplib_server<test_types>> server;

    explicit slow_peer(std::uint16_t port) {
        kythira::cpp_httplib_server_config config;
        typename test_types::metrics_type metrics;
        server = std::make_unique<kythira::cpp_httplib_server<test_types>>(bind_address, port,
                                                                           config, metrics);
        server->register_request_vote_handler([](const kythira::request_vote_request<>& req) {
            std::this_thread::sleep_for(handler_delay);
            kythira::request_vote_response<> response;
            response._term = req.term();
            response._vote_granted = true;
            return response;
        });
        server->start();
    }

    ~slow_peer() { server->stop(); }
    slow_peer(const slow_peer&) = delete;
    auto operator=(const slow_peer&) -> slow_peer& = delete;
};

auto make_client(std::uint16_t port, std::chrono::milliseconds request_timeout)
    -> kythira::cpp_httplib_client<test_types> {
    kythira::cpp_httplib_client_config config;
    config.request_timeout = request_timeout;
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[peer_id] = std::format("http://{}:{}", bind_address, port);
    typename test_types::metrics_type metrics;
    return kythira::cpp_httplib_client<test_types>(std::move(node_map), config, metrics);
}

auto make_vote_request() -> kythira::request_vote_request<> {
    kythira::request_vote_request<> request;
    request._term = 3;
    request._candidate_id = 2;
    request._last_log_index = 0;
    request._last_log_term = 0;
    return request;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(http_client_per_call_timeout_tests)

// The per-call timeout is shorter than the handler, the configured
// request_timeout is far longer: the RPC must fail with http_timeout_error
// near the per-call deadline. Before the fix it waited out the handler and
// succeeded, because only request_timeout reached the socket.
BOOST_AUTO_TEST_CASE(per_call_timeout_bounds_the_rpc, *boost::unit_test::timeout(30)) {
    constexpr std::uint16_t port = 18561;
    slow_peer peer(port);
    auto client = make_client(port, std::chrono::milliseconds{10000});

    const auto start = std::chrono::steady_clock::now();
    BOOST_CHECK_THROW(
        client.send_request_vote(peer_id, make_vote_request(), std::chrono::milliseconds{300})
            .get(),
        kythira::http_timeout_error);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // Well under the handler's delay, so the deadline (not the reply) ended it.
    BOOST_TEST(elapsed < std::chrono::milliseconds{1200});
}

// A per-call timeout that covers the handler still succeeds: the deadline
// bounds the RPC, it does not shorten an RPC that fits inside it.
BOOST_AUTO_TEST_CASE(rpc_within_per_call_timeout_succeeds, *boost::unit_test::timeout(30)) {
    constexpr std::uint16_t port = 18562;
    slow_peer peer(port);
    auto client = make_client(port, std::chrono::milliseconds{10000});

    auto response =
        client.send_request_vote(peer_id, make_vote_request(), std::chrono::milliseconds{5000})
            .get();
    BOOST_TEST(response.term() == 3U);
    BOOST_TEST(response.vote_granted());
}

// A caller with no deadline of its own (a zero timeout) falls back to the
// configured request_timeout instead of being treated as "expire at once".
BOOST_AUTO_TEST_CASE(zero_timeout_falls_back_to_request_timeout, *boost::unit_test::timeout(30)) {
    constexpr std::uint16_t port = 18563;
    slow_peer peer(port);
    auto client = make_client(port, std::chrono::milliseconds{300});

    const auto start = std::chrono::steady_clock::now();
    BOOST_CHECK_THROW(
        client.send_request_vote(peer_id, make_vote_request(), std::chrono::milliseconds{0}).get(),
        kythira::http_timeout_error);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    BOOST_TEST(elapsed >= std::chrono::milliseconds{250});
    BOOST_TEST(elapsed < std::chrono::milliseconds{1200});
}

// RPCs to one peer go out one at a time over its cached connection. A call
// queued behind a slow one must still give up at its own deadline rather than
// wait for the slow call to finish first.
BOOST_AUTO_TEST_CASE(queued_call_keeps_its_own_deadline, *boost::unit_test::timeout(30)) {
    constexpr std::uint16_t port = 18564;
    slow_peer peer(port);
    auto client = make_client(port, std::chrono::milliseconds{10000});

    std::thread slow_call([&client] {
        try {
            client.send_request_vote(peer_id, make_vote_request(), std::chrono::milliseconds{5000})
                .get();
        } catch (...) {
        }
    });
    // Let the slow call take the connection first.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    const auto start = std::chrono::steady_clock::now();
    BOOST_CHECK_THROW(
        client.send_request_vote(peer_id, make_vote_request(), std::chrono::milliseconds{300})
            .get(),
        kythira::http_timeout_error);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    BOOST_TEST(elapsed < std::chrono::milliseconds{1000});

    slow_call.join();
}

BOOST_AUTO_TEST_SUITE_END()
