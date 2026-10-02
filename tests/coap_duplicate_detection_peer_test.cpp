// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_duplicate_detection_peer_test.cpp
/// @brief Wire-level regression for duplicate detection keyed by peer
///        (.kiro/specs/coap-transport-multi-raft/ Requirement 5, task 3).
///
/// Sends hand-built CoAP requests from raw UDP sockets to a real, started
/// coap_server, because the Message ID is the one field a coap_client gives a
/// test no control over: libcoap chooses it per session. Raw sockets let the
/// test put the *same* Message ID on the wire from two peers, which is the
/// collision the old bare-Message-ID key turned into a 2.03 with no payload.
///
/// Every request carries a JSON request_vote body produced by the same
/// serializer the server decodes with, and every assertion is about whether
/// the server's handler ran — the only observable that distinguishes a
/// delivered request from a suppressed one.
///
/// The server binds port 0 and is addressed through bound_port(), so this file
/// adds no fixed port to the CoAP suite (Requirement 8.3); endpoints are
/// numeric (Requirement 8.2).

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_duplicate_detection_peer_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;

struct test_types {
    using serializer_type = serializer;
    using serializer_registry_type = kythira::single_serializer_registry<serializer>;
    using rpc_serializer_type = serializer;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = int;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

constexpr const char* loopback = "127.0.0.1";

// CoAP response classes this test distinguishes (RFC 7252 Section 12.1.2).
constexpr std::uint8_t code_valid = 0x43;    // 2.03, what a suppressed duplicate gets
constexpr std::uint8_t code_content = 0x45;  // 2.05, a delivered request's answer
constexpr std::uint8_t code_changed = 0x44;  // 2.04, also a delivered request's answer

/// One UDP socket is one CoAP peer: its own source port, hence its own
/// endpoint in the server's duplicate-detection key.
class raw_peer {
public:
    raw_peer() : _fd{::socket(AF_INET, SOCK_DGRAM, 0)} {
        BOOST_REQUIRE(_fd >= 0);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        ::inet_pton(AF_INET, loopback, &local.sin_addr);
        BOOST_REQUIRE(::bind(_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
    }
    raw_peer(const raw_peer&) = delete;
    auto operator=(const raw_peer&) -> raw_peer& = delete;
    ~raw_peer() { ::close(_fd); }

    /// Sends one confirmable POST /raft/request_vote and waits for the
    /// piggybacked response; returns its code, or nullopt on no answer.
    auto request_vote(std::uint16_t port, std::uint16_t message_id, const std::string& token,
                      const std::vector<std::byte>& body) -> std::optional<std::uint8_t> {
        auto datagram = build_request(message_id, token, body);
        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(port);
        ::inet_pton(AF_INET, loopback, &server.sin_addr);
        if (::sendto(_fd, datagram.data(), datagram.size(), 0, reinterpret_cast<sockaddr*>(&server),
                     sizeof(server)) < 0) {
            return std::nullopt;
        }

        std::vector<std::uint8_t> reply(1500);
        const auto deadline =
            std::chrono::steady_clock::now() + kythira::testing::scaled_deadline(5000);
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd fds{_fd, POLLIN, 0};
            if (::poll(&fds, 1, 100) <= 0) {
                continue;
            }
            const auto n = ::recv(_fd, reply.data(), reply.size(), 0);
            // An ACK carries the request's Message ID (RFC 7252 Section 4.2);
            // ignore anything that answers some other exchange.
            if (n >= 4 && ((reply[2] << 8) | reply[3]) == message_id) {
                return reply[1];
            }
        }
        return std::nullopt;
    }

private:
    static auto build_request(std::uint16_t message_id, const std::string& token,
                              const std::vector<std::byte>& body) -> std::vector<std::uint8_t> {
        std::vector<std::uint8_t> out;
        // Version 1, type CON, token length; code 0.02 POST; Message ID.
        out.push_back(static_cast<std::uint8_t>(0x40 | token.size()));
        out.push_back(0x02);
        out.push_back(static_cast<std::uint8_t>(message_id >> 8));
        out.push_back(static_cast<std::uint8_t>(message_id & 0xFF));
        out.insert(out.end(), token.begin(), token.end());
        // Uri-Path (11) "raft", Uri-Path (delta 0) "request_vote".
        out.push_back(0xB4);
        out.insert(out.end(), {'r', 'a', 'f', 't'});
        const std::string leaf = "request_vote";
        out.push_back(static_cast<std::uint8_t>(leaf.size()));
        out.insert(out.end(), leaf.begin(), leaf.end());
        // Content-Format (12, delta 1) = 50, application/json.
        out.push_back(0x11);
        out.push_back(50);
        out.push_back(0xFF);
        for (const auto b : body) {
            out.push_back(static_cast<std::uint8_t>(b));
        }
        return out;
    }

    int _fd;
};

/// A started server that counts how many requests actually reached Raft.
struct counting_server {
    counting_server()
        : server{loopback, 0, kythira::coap_server_config{}, kythira::noop_metrics{}} {
        server.register_request_vote_handler([this](const kythira::request_vote_request<>& req) {
            handled.fetch_add(1);
            kythira::request_vote_response<> response;
            response._term = req._term;
            response._vote_granted = false;
            return response;
        });
        server.start();
    }
    ~counting_server() { server.stop(); }

    [[nodiscard]] auto port() const -> std::uint16_t { return server.bound_port(); }

    kythira::coap_server<test_types> server;
    std::atomic<int> handled{0};
};

auto vote_body(std::uint64_t candidate) -> std::vector<std::byte> {
    kythira::request_vote_request<> request;
    request._term = 1;
    request._candidate_id = candidate;
    return serializer{}.serialize(request);
}

auto delivered(std::optional<std::uint8_t> code) -> bool {
    return code && (*code == code_content || *code == code_changed);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_duplicate_detection_peer_tests)

#ifdef LIBCOAP_AVAILABLE

// Requirement 5.5: two peers put the same Message ID on the wire. Both are new
// requests and both must reach the handler.
BOOST_AUTO_TEST_CASE(two_peers_sending_the_same_message_id_are_both_delivered,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    counting_server fixture;
    raw_peer first;
    raw_peer second;

    const auto a = first.request_vote(fixture.port(), 1, "tokA", vote_body(2));
    const auto b = second.request_vote(fixture.port(), 1, "tokB", vote_body(3));

    BOOST_TEST(delivered(a));
    BOOST_TEST(delivered(b));
    BOOST_TEST(fixture.handled.load() == 2);
}

// Requirement 5.6: one peer reuses a Message ID with a new token, which is what
// a counter that wraps inside EXCHANGE_LIFETIME puts on the wire. The new
// request must be delivered.
BOOST_AUTO_TEST_CASE(a_wrapped_message_id_with_a_new_token_is_delivered,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    counting_server fixture;
    raw_peer peer;

    const auto first = peer.request_vote(fixture.port(), 7, "lap1", vote_body(2));
    const auto second = peer.request_vote(fixture.port(), 7, "lap2", vote_body(2));

    BOOST_TEST(delivered(first));
    BOOST_TEST(delivered(second));
    BOOST_TEST(fixture.handled.load() == 2);
}

// The narrowing must not cost the check its purpose: a genuine retransmission
// (same peer, same Message ID, same token) still never reaches the handler a
// second time.
BOOST_AUTO_TEST_CASE(a_genuine_retransmission_is_still_suppressed,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    counting_server fixture;
    raw_peer peer;

    const auto first = peer.request_vote(fixture.port(), 9, "same", vote_body(2));
    const auto again = peer.request_vote(fixture.port(), 9, "same", vote_body(2));

    BOOST_TEST(delivered(first));
    BOOST_TEST(again.has_value());
    BOOST_TEST(fixture.handled.load() == 1);
    if (again) {
        BOOST_TEST(*again == code_valid);
    }
}

#else

BOOST_AUTO_TEST_CASE(skipped_without_libcoap) {
    BOOST_TEST_MESSAGE("libcoap not available - no real coap_server to send to");
    BOOST_TEST(true);
}

#endif

BOOST_AUTO_TEST_SUITE_END()
