// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file http_client_send_lanes_test.cpp
/// @brief `cpp_httplib_client` sends each RPC on its peer's send lane, so a
///        slow peer never holds up a send to another one.
///
/// cpp-httplib's client blocks for the whole round trip, and `send_rpc` used
/// to run it on the caller's thread. The Raft leader replicates on its tick
/// thread, one follower after another, so a follower whose AppendEntries sat
/// behind a slow durable write held back every other follower's heartbeat;
/// past an election timeout those followers started elections. That was the
/// leadership churn under `multi_raft_node_s3_process_test` (see
/// `doc/TODO.md`, the S3 write-behind entry).
///
/// Two raw `httplib::Server`s stand in for followers: one answers after a
/// delay, the other at once. Raw servers, because the delay has to be in the
/// handler, and `cpp_httplib_server` has no hook for one.

#define BOOST_TEST_MODULE http_client_send_lanes_test
#include <boost/test/unit_test.hpp>

#include "negotiation_test_harness.hpp"
#include "test_ports.hpp"

#include <raft/http_exceptions.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace kythira::negotiation_test;
using namespace std::chrono_literals;

using data_type = std::vector<std::byte>;
using json_serializer = kythira::json_rpc_serializer<data_type>;
using single_json = kythira::single_serializer_registry<json_serializer>;
/// The bundle the Raft hosts use: `node<Types>` takes the client's future as
/// a `kythira::future_default`, which is also the future the lanes complete
/// from a worker. (The folly-less `http_transport_types` default is
/// `SimpleFuture`, which has no promise, so it keeps the blocking send.)
struct client_types : kythira::http_transport_types<json_serializer, recording_metrics,
                                                    folly::CPUThreadPoolExecutor> {
    template<typename T> using future_template = kythira::future_default<T>;
    using serializer_registry_type = single_json;
};
using clock_type = std::chrono::steady_clock;

constexpr std::uint64_t slow_peer = 2;
constexpr std::uint64_t fast_peer = 3;
constexpr const char* endpoint_append_entries = "/v1/raft/append_entries";

auto append_response_body() -> std::string {
    kythira::append_entries_response<> resp;
    resp._term = 1;
    resp._success = true;
    const json_serializer serializer;
    const auto bytes = serializer.serialize(resp);
    std::string body;
    body.reserve(bytes.size());
    for (auto b : bytes) {
        body.push_back(static_cast<char>(b));
    }
    return body;
}

/// A follower that answers every AppendEntries after `delay`.
struct follower {
    std::uint16_t port = kythira::testing::free_port();
    httplib::Server server;
    std::thread thread;
    std::atomic<int> served{0};

    explicit follower(std::chrono::milliseconds delay) {
        server.Post(endpoint_append_entries, [this, delay, body = append_response_body()](
                                                 const httplib::Request&, httplib::Response& res) {
            std::this_thread::sleep_for(delay);
            served.fetch_add(1);
            res.status = 200;
            res.set_content(body, "application/json");
        });
        thread = std::thread([this] { server.listen(bind_address, port); });
        server.wait_until_ready();
    }
    ~follower() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }
    follower(const follower&) = delete;
    auto operator=(const follower&) -> follower& = delete;

    [[nodiscard]] auto url() const -> std::string {
        return std::string("http://") + bind_address + ":" + std::to_string(port);
    }
};

auto make_client(const follower& slow, const follower& fast, recording_metrics& metrics,
                 std::size_t pool_size = 10)
    -> std::unique_ptr<kythira::cpp_httplib_client<client_types>> {
    std::unordered_map<std::uint64_t, std::string> urls;
    urls[slow_peer] = slow.url();
    urls[fast_peer] = fast.url();
    kythira::cpp_httplib_client_config config;
    config.connection_timeout = 2000ms;
    config.request_timeout = 5000ms;
    config.connection_pool_size = pool_size;
    return std::make_unique<kythira::cpp_httplib_client<client_types>>(std::move(urls), config,
                                                                       metrics);
}

auto heartbeat() -> kythira::append_entries_request<> {
    kythira::append_entries_request<> req;
    req._term = 1;
    req._leader_id = 1;
    return req;
}

/// Waits for a reply. A function rather than `std::move(f).get()` inside a
/// Boost.Test macro, which clang-tidy reads as a use after move.
template<typename Future> auto take(Future& reply) {
    return std::move(reply).get();
}

auto elapsed_ms(clock_type::time_point since) -> long long {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - since).count();
}

}  // namespace

/// The leader's loop in miniature: send to the slow follower, then to the fast
/// one, from one thread. The fast follower's reply must not wait for the slow
/// one's, and neither send may block the caller.
BOOST_AUTO_TEST_CASE(a_slow_peer_does_not_hold_up_a_send_to_another_peer) {
    follower slow(1500ms);
    follower fast(0ms);
    recording_metrics metrics;
    auto client = make_client(slow, fast, metrics);

    const auto start = clock_type::now();
    auto slow_reply = client->send_append_entries(slow_peer, heartbeat(), 5000ms);
    auto fast_reply = client->send_append_entries(fast_peer, heartbeat(), 5000ms);
    const auto queued_after = elapsed_ms(start);
    BOOST_TEST_MESSAGE("both sends returned after " << queued_after << " ms");
    BOOST_TEST(queued_after < 500,
               "send_append_entries blocked its caller for " << queued_after << " ms");

    const auto fast_response = std::move(fast_reply).get();
    const auto fast_done = elapsed_ms(start);
    BOOST_TEST(fast_response.success());
    BOOST_TEST(fast_done < 1000,
               "the fast peer's reply took " << fast_done << " ms, behind the slow peer");
    BOOST_TEST(slow.served.load() == 0, "the slow peer had already answered");

    const auto slow_response = std::move(slow_reply).get();
    BOOST_TEST(slow_response.success());
    BOOST_TEST(elapsed_ms(start) >= 1400);
}

/// Several sends to one peer at once each take a connection of their own, up
/// to the pool size, so they overlap instead of queueing one behind another.
BOOST_AUTO_TEST_CASE(sends_to_one_peer_overlap_up_to_the_pool_size) {
    follower slow(800ms);
    follower fast(0ms);
    recording_metrics metrics;
    auto client = make_client(slow, fast, metrics, 4);

    const auto start = clock_type::now();
    std::vector<kythira::future_default<kythira::append_entries_response<>>> replies;
    for (int i = 0; i < 4; ++i) {
        replies.push_back(client->send_append_entries(slow_peer, heartbeat(), 5000ms));
    }
    for (auto& reply : replies) {
        BOOST_TEST(take(reply).success());
    }
    const auto took = elapsed_ms(start);
    BOOST_TEST_MESSAGE("four concurrent sends took " << took << " ms");
    BOOST_TEST(took < 2000, "four sends of 800 ms each ran one after another: " << took << " ms");
    BOOST_TEST(slow.served.load() == 4);
}

/// The caller's timeout counts from the send, so time spent queued behind the
/// peer's other sends is spent from it: with one connection, the second send
/// waits out the first and then has nothing left.
BOOST_AUTO_TEST_CASE(time_queued_behind_other_sends_counts_against_the_timeout) {
    follower slow(1200ms);
    follower fast(0ms);
    recording_metrics metrics;
    auto client = make_client(slow, fast, metrics, 1);

    auto first = client->send_append_entries(slow_peer, heartbeat(), 5000ms);
    auto second = client->send_append_entries(slow_peer, heartbeat(), 600ms);
    BOOST_TEST(take(first).success());
    BOOST_CHECK_THROW(take(second), kythira::http_timeout_error);
}

/// A client destroyed with sends still queued fails them instead of leaving
/// their futures unresolved, and waits only for the one already on the wire.
BOOST_AUTO_TEST_CASE(destroying_the_client_fails_sends_it_never_started) {
    follower slow(1000ms);
    follower fast(0ms);
    recording_metrics metrics;
    auto client = make_client(slow, fast, metrics, 1);

    auto on_the_wire = client->send_append_entries(slow_peer, heartbeat(), 5000ms);
    auto queued = client->send_append_entries(slow_peer, heartbeat(), 5000ms);
    std::this_thread::sleep_for(200ms);  // the first is now being served

    const auto start = clock_type::now();
    client.reset();
    BOOST_TEST(elapsed_ms(start) < 3000);

    BOOST_TEST(take(on_the_wire).success());
    BOOST_CHECK_THROW(take(queued), std::runtime_error);
    BOOST_TEST(slow.served.load() == 1);
}

/// A continuation that sends again runs on a lane worker. That send runs
/// inline there, as every send did before lanes, so a worker never waits on a
/// job queued behind itself, even with one worker per peer.
BOOST_AUTO_TEST_CASE(a_send_from_a_continuation_completes_with_one_worker) {
    follower slow(100ms);
    follower fast(0ms);
    recording_metrics metrics;
    auto client = make_client(slow, fast, metrics, 1);

    std::atomic<bool> nested_ok{false};
    auto chained = client->send_append_entries(slow_peer, heartbeat(), 5000ms)
                       .thenValue([&](const kythira::append_entries_response<>&) {
                           auto again = client->send_append_entries(slow_peer, heartbeat(), 5000ms);
                           nested_ok = std::move(again).get().success();
                           return kythira::unit{};
                       });
    std::move(chained).get();
    BOOST_TEST(nested_ok.load());
    BOOST_TEST(slow.served.load() == 2);
}

/// A peer that is down must not grow a backlog without bound: the leader
/// sends it a heartbeat every tick. Past four times the pool size waiting,
/// a send fails at once instead of queueing.
BOOST_AUTO_TEST_CASE(a_full_lane_fails_new_sends_at_once) {
    follower slow(600ms);
    follower fast(0ms);
    recording_metrics metrics;
    auto client = make_client(slow, fast, metrics, 1);

    std::vector<kythira::future_default<kythira::append_entries_response<>>> accepted;
    accepted.push_back(client->send_append_entries(slow_peer, heartbeat(), 10000ms));
    std::this_thread::sleep_for(100ms);  // the first is on the wire, the queue empty
    for (int i = 0; i < 4; ++i) {
        accepted.push_back(client->send_append_entries(slow_peer, heartbeat(), 10000ms));
    }
    const auto start = clock_type::now();
    BOOST_CHECK_THROW(client->send_append_entries(slow_peer, heartbeat(), 10000ms).get(),
                      kythira::http_transport_error);
    BOOST_TEST(elapsed_ms(start) < 200, "the refused send waited");
    // Another peer's lane is its own.
    BOOST_TEST(client->send_append_entries(fast_peer, heartbeat(), 5000ms).get().success());
    for (auto& reply : accepted) {
        BOOST_TEST(take(reply).success());
    }
}
