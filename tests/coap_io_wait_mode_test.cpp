// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_io_wait_mode_test.cpp
/// @brief The libcoap client's I/O thread in both wait modes
/// (`.kiro/specs/coap-client-event-driven-io/`, design §8).
///
/// The I/O thread used to make one non-blocking libcoap step and sleep 5 ms,
/// which dispatched about one reply per peer per 5 ms however many were
/// waiting. It now waits on libcoap's epoll descriptor (readiness mode) and
/// drains every ready reply on each pass, or drains and then sleeps where
/// there is no descriptor (paced mode). These cases pin what that buys and
/// what it must not break:
///
///  - a burst of replies is dispatched in a few passes, not one per reply;
///  - timers the old 5 ms cadence serviced by accident -- retransmission,
///    multicast windows -- still fire when the wait cap is far longer than
///    they are, so a missing deadline fails here instead of hiding behind
///    the cap;
///  - a send wakes a waiting thread, an idle thread does not spin, shutdown
///    does not wait out the cap, and the lock is free while the thread waits
///    (the PR #227 starvation).
///
/// The peer is a plain UDP socket owned by the test, so it decides exactly
/// when replies are sent. Every timing check is a budget with slack, never a
/// latency target: this suite has blocked unrelated merges before.

#define BOOST_TEST_MODULE coap_io_wait_mode_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/coap_config_validation.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/console_logger.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "test_timeout_scale.hpp"

using namespace kythira;
using namespace std::chrono_literals;

namespace {

using test_serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;

// A real future_template, unlike default_transport_types', because the
// multicast case needs a future of a different type than the RPCs'.
struct test_transport_types {
    using serializer_type = test_serializer;
    using serializer_registry_type = kythira::single_serializer_registry<test_serializer>;
    using rpc_serializer_type = test_serializer;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using executor_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
    using future_type = kythira::future_default<kythira::request_vote_response<>>;
};
using client_type = coap_client<test_transport_types>;

/// A UDP peer on 127.0.0.1 that reads requests and answers them only when
/// told to: a piggybacked ACK 2.04 carrying the request's token, no payload.
/// The client fails to decode the empty body, which is fine -- these tests
/// count dispatches, not results.
class held_reply_peer {
public:
    held_reply_peer() {
        _fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(_fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        socklen_t len = sizeof a;
        ::getsockname(_fd, reinterpret_cast<sockaddr*>(&a), &len);
        _port = ntohs(a.sin_port);
        int big = 4 << 20;
        ::setsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
        ::setsockopt(_fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    }
    ~held_reply_peer() { ::close(_fd); }
    held_reply_peer(const held_reply_peer&) = delete;
    auto operator=(const held_reply_peer&) -> held_reply_peer& = delete;

    [[nodiscard]] auto endpoint() const -> std::string {
        return "coap://127.0.0.1:" + std::to_string(_port);
    }

    /// Reads requests until @p n distinct ones are held (retransmissions of a
    /// held one are dropped) or @p budget passes. Returns how many are held.
    auto hold(std::size_t n, std::chrono::milliseconds budget) -> std::size_t {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (_held.size() < n && std::chrono::steady_clock::now() < deadline) {
            pollfd p{_fd, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) {
                continue;
            }
            std::uint8_t buf[1500];
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const auto got =
                ::recvfrom(_fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len);
            if (got < 4) {
                continue;
            }
            const std::uint8_t tkl = buf[0] & 0xF;
            const std::uint16_t mid = static_cast<std::uint16_t>((buf[2] << 8) | buf[3]);
            if (!_seen.insert(mid).second) {
                continue;
            }
            std::vector<std::uint8_t> r{static_cast<std::uint8_t>(0x60 | tkl), 0x44, buf[2],
                                        buf[3]};
            r.insert(r.end(), buf + 4, buf + 4 + tkl);
            _held.push_back({std::move(r), from});
        }
        return _held.size();
    }

    /// Sends every held reply back to back.
    auto release() -> void {
        for (auto& h : _held) {
            ::sendto(_fd, h.reply.data(), h.reply.size(), 0, reinterpret_cast<sockaddr*>(&h.to),
                     sizeof h.to);
        }
        _held.clear();
    }

private:
    struct held {
        std::vector<std::uint8_t> reply;
        sockaddr_in to;
    };
    int _fd{-1};
    std::uint16_t _port{0};
    std::vector<held> _held;
    std::set<std::uint16_t> _seen;
};

auto config_for(coap_io_wait_mode mode) -> coap_client_config {
    coap_client_config config;
    config.io_wait_mode = mode;
    return config;
}

auto make_client(const std::string& endpoint,
                 coap_client_config config) -> std::unique_ptr<client_type> {
    std::unordered_map<std::uint64_t, std::string> endpoints{{1, endpoint}};
    return std::make_unique<client_type>(std::move(endpoints), std::move(config), noop_metrics{});
}

auto vote_request(std::uint64_t term) -> request_vote_request<> {
    request_vote_request<> r;
    r._term = term;
    r._candidate_id = 2;
    r._last_log_index = 0;
    r._last_log_term = 0;
    return r;
}

/// Waits until @p pred holds or @p budget passes; returns whether it held.
template<typename Pred> auto eventually(Pred pred, std::chrono::milliseconds budget) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

/// What `automatic` resolves to here: readiness on every libcoap build the
/// project uses (design §1 L1), unless the paced twin of this test set
/// KYTHIRA_COAP_IO_WAIT_MODE.
auto expected_automatic_mode() -> coap_io_wait_mode {
    const char* env = std::getenv("KYTHIRA_COAP_IO_WAIT_MODE");
    if (env != nullptr && std::string(env) == "paced") {
        return coap_io_wait_mode::paced;
    }
    return coap_io_wait_mode::readiness;
}

constexpr coap_io_wait_mode both_modes[] = {coap_io_wait_mode::readiness, coap_io_wait_mode::paced};

auto mode_name(coap_io_wait_mode mode) -> const char* {
    return mode == coap_io_wait_mode::readiness ? "readiness" : "paced";
}

}  // namespace

BOOST_AUTO_TEST_CASE(mode_selection_follows_config,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    held_reply_peer peer;
    BOOST_TEST((make_client(peer.endpoint(), config_for(coap_io_wait_mode::automatic))
                    ->io_loop_stats()
                    .mode == expected_automatic_mode()));
    BOOST_TEST(
        (make_client(peer.endpoint(), config_for(coap_io_wait_mode::paced))->io_loop_stats().mode ==
         coap_io_wait_mode::paced));
    // `readiness` throws only where libcoap has no epoll descriptor, which
    // no build this project uses lacks (design §1 L1); so here it must
    // construct, and the throwing branch is covered by inspection.
    BOOST_TEST((make_client(peer.endpoint(), config_for(coap_io_wait_mode::readiness))
                    ->io_loop_stats()
                    .mode == coap_io_wait_mode::readiness));
}

BOOST_AUTO_TEST_CASE(invalid_io_settings_are_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    auto zero_budget = coap_client_config{};
    zero_budget.io_drain_budget = 0;
    auto zero_wait = coap_client_config{};
    zero_wait.io_max_wait = 0ms;
    auto negative_interval = coap_client_config{};
    negative_interval.io_paced_interval = -1ms;

    for (const auto& bad : {zero_budget, zero_wait, negative_interval}) {
        BOOST_CHECK_THROW(coap_utils::validate_client_config(bad), coap_transport_error);
        BOOST_CHECK_THROW(make_client("coap://127.0.0.1:5683", bad), coap_transport_error);
    }
    BOOST_CHECK_NO_THROW(coap_utils::validate_client_config(coap_client_config{}));
}

BOOST_AUTO_TEST_CASE(a_burst_of_replies_drains_in_a_few_passes,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    // 32 replies queued on the client's one socket for this peer. The pre-spec
    // loop needed 32 passes for them, one per 5 ms. A pass now drains what is
    // ready, so they take ceil(32 / budget) + 1 = 2 passes when they are all
    // ready together; on loopback the client can start draining before the
    // last ones land, so the budget below allows a few more passes, not 32.
    constexpr std::size_t burst = 32;
    for (const auto mode : both_modes) {
        BOOST_TEST_CONTEXT("mode " << mode_name(mode)) {
            held_reply_peer peer;
            auto config = config_for(mode);
            config.max_concurrent_requests = burst;  // NSTART: all on the wire at once
            auto client = make_client(peer.endpoint(), config);

            std::vector<future_default<request_vote_response<>>> pending;
            for (std::size_t i = 0; i < burst; ++i) {
                pending.push_back(client->send_request_vote(1, vote_request(i + 1), 30s));
            }
            BOOST_REQUIRE_EQUAL(peer.hold(burst, kythira::testing::scaled_deadline(5000)), burst);
            std::this_thread::sleep_for(50ms);  // let the sends' own wakes settle

            const auto before = client->io_loop_stats();
            peer.release();
            BOOST_REQUIRE(eventually(
                [&] { return client->io_loop_stats().dispatches >= before.dispatches + burst; },
                kythira::testing::scaled_deadline(5000)));
            const auto after = client->io_loop_stats();
            BOOST_TEST_MESSAGE(mode_name(mode)
                               << ": " << burst << " replies in " << after.passes - before.passes
                               << " passes, " << after.drain_steps - before.drain_steps
                               << " steps");
            BOOST_TEST(after.passes - before.passes <= burst / 4);
            BOOST_TEST(after.drain_steps - before.drain_steps >= burst);
        }
    }
}

BOOST_AUTO_TEST_CASE(a_reply_wakes_a_long_wait,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    // With a 10 s cap the only thing that can dispatch this reply promptly
    // is readiness on libcoap's descriptor.
    held_reply_peer peer;
    auto config = config_for(coap_io_wait_mode::readiness);
    config.io_max_wait = 10s;
    auto client = make_client(peer.endpoint(), config);

    auto pending = client->send_request_vote(1, vote_request(1), 30s);
    BOOST_REQUIRE_EQUAL(peer.hold(1, kythira::testing::scaled_deadline(5000)), 1u);
    std::this_thread::sleep_for(200ms);  // the I/O thread is now in its long wait
    const auto before = client->io_loop_stats();
    peer.release();
    BOOST_TEST(eventually([&] { return client->io_loop_stats().dispatches > before.dispatches; },
                          kythira::testing::scaled_deadline(2000)));
}

BOOST_AUTO_TEST_CASE(retransmission_still_gives_up_on_schedule,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    // A CON to a peer that never answers: libcoap retransmits once after
    // 1-1.5 s and NACKs "too many retries" 2-3 s after that. Both are libcoap
    // timers the wait has to honour; with a 60 s cap, a missed deadline
    // fails the budget instead of hiding behind the cap. (1 s is libcoap's
    // floor: coap_session_set_ack_timeout() ignores anything under a second.)
    for (const auto mode : both_modes) {
        BOOST_TEST_CONTEXT("mode " << mode_name(mode)) {
            held_reply_peer silent;
            auto config = config_for(mode);
            config.io_max_wait = 60s;
            config.ack_timeout = 1s;
            config.max_retransmit = 1;
            auto client = make_client(silent.endpoint(), config);

            const auto before = client->io_loop_stats();
            const auto start = std::chrono::steady_clock::now();
            auto pending = client->send_request_vote(1, vote_request(1), 120s);
            BOOST_TEST(
                eventually([&] { return client->io_loop_stats().dispatches > before.dispatches; },
                           kythira::testing::scaled_deadline(8000)));
            BOOST_TEST_MESSAGE(mode_name(mode)
                               << ": NACK after "
                               << std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count()
                               << " ms");
            // The original and its one retransmission reached the peer
            // (one held request; the peer drops the duplicate by Message ID).
            BOOST_TEST(silent.hold(1, 100ms) == 1u);
        }
    }
}

BOOST_AUTO_TEST_CASE(a_multicast_window_closes_on_time,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    // No group members, so the collection resolves empty when its window
    // closes -- which only the I/O thread notices. The pre-spec 5 ms cadence
    // did that by accident; now the window is in the wait's timeout.
    for (const auto mode : both_modes) {
        BOOST_TEST_CONTEXT("mode " << mode_name(mode)) {
            auto config = config_for(mode);
            config.io_max_wait = 10s;
            auto client = make_client("coap://127.0.0.1:5683", config);

            constexpr auto window = 300ms;
            const auto start = std::chrono::steady_clock::now();
            auto collected = client->send_multicast_message(
                "239.255.83.18", 5683, "/raft/discovery", std::vector<std::byte>{}, window);
            const std::vector<std::vector<std::byte>> responses = std::move(collected).get();
            const auto took = std::chrono::steady_clock::now() - start;
            BOOST_TEST(responses.empty());
            BOOST_TEST((took >= window));
            BOOST_TEST((took < window + kythira::testing::scaled_deadline(2000)));
        }
    }
}

BOOST_AUTO_TEST_CASE(sends_wake_a_waiting_thread,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    // Several threads send while the I/O thread sits in a 10 s wait; each
    // send signals the wake descriptor, so a pass follows promptly, and a
    // burst of signals costs passes, not one per signal forever.
    held_reply_peer silent;
    auto config = config_for(coap_io_wait_mode::readiness);
    config.io_max_wait = 10s;
    config.ack_timeout = 5s;  // no retransmission inside this test
    auto client = make_client(silent.endpoint(), config);
    std::this_thread::sleep_for(200ms);

    for (int round = 0; round < 3; ++round) {
        const auto before = client->io_loop_stats();
        std::vector<std::optional<future_default<request_vote_response<>>>> pending(4);
        {
            std::vector<std::jthread> senders;
            for (std::size_t t = 0; t < pending.size(); ++t) {
                senders.emplace_back([&, t] {
                    pending[t] =
                        client->send_request_vote(1, vote_request(round * 10 + t + 1), 60s);
                });
            }
        }
        BOOST_TEST(eventually([&] { return client->io_loop_stats().passes > before.passes; },
                              kythira::testing::scaled_deadline(2000)));
        std::this_thread::sleep_for(200ms);  // back into the long wait
        const auto settled = client->io_loop_stats().passes;
        std::this_thread::sleep_for(300ms);
        // Coalesced and drained: the signals do not keep the thread awake.
        BOOST_TEST(client->io_loop_stats().passes - settled <= 1u);
    }
}

BOOST_AUTO_TEST_CASE(an_idle_client_does_not_spin,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    auto client = make_client("coap://127.0.0.1:5683", config_for(coap_io_wait_mode::readiness));
    std::this_thread::sleep_for(200ms);
    const auto before = client->io_loop_stats();
    std::this_thread::sleep_for(1s);
    const auto passes = client->io_loop_stats().passes - before.passes;
    BOOST_TEST_MESSAGE("idle passes in 1 s: " << passes);
    // ceil(1 s / io_max_wait) + 2, against 200 for the pre-spec loop.
    BOOST_TEST(passes <= 12u);
}

BOOST_AUTO_TEST_CASE(the_lock_is_free_while_the_thread_waits,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    // PR #227: the loop once held _mutex across a blocking libcoap wait and
    // starved send_rpc() for seconds. get_joined_multicast_groups() takes the
    // same lock, so its worst case here is the I/O thread's longest hold.
    for (const auto mode : both_modes) {
        BOOST_TEST_CONTEXT("mode " << mode_name(mode)) {
            auto config = config_for(mode);
            config.io_max_wait = 10s;
            auto client = make_client("coap://127.0.0.1:5683", config);
            std::this_thread::sleep_for(100ms);
            auto worst = std::chrono::steady_clock::duration::zero();
            for (int i = 0; i < 200; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                (void)client->get_joined_multicast_groups();
                worst = std::max(worst, std::chrono::steady_clock::now() - t0);
                std::this_thread::sleep_for(1ms);
            }
            BOOST_TEST((worst < kythira::testing::scaled_deadline(100)));
        }
    }
}

BOOST_AUTO_TEST_CASE(shutdown_does_not_wait_out_the_cap,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    auto config = config_for(coap_io_wait_mode::readiness);
    config.io_max_wait = 10s;
    auto client = make_client("coap://127.0.0.1:5683", config);
    std::this_thread::sleep_for(200ms);  // in the 10 s wait
    const auto start = std::chrono::steady_clock::now();
    client.reset();
    BOOST_TEST(
        (std::chrono::steady_clock::now() - start < kythira::testing::scaled_deadline(2000)));
}
