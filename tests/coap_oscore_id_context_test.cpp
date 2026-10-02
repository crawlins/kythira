// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_oscore_id_context_test.cpp
/// @brief oscore_credentials::id_context over the libcoap backend, whose OSCORE
///        is libcoap's own rather than raft/oscore.hpp's
///        (.kiro/specs/coap-transport-multi-raft/ task 8).
///
/// oscore_rfc8613_vectors_test pins kythira's own security_context against
/// RFC 8613 C.6. libcoap derives and frames OSCORE itself from the conf text
/// oscore_provider builds, so the field has to reach that text too, or a
/// libcoap deployment would silently derive with an empty ID Context while a
/// libnyoci or cantcoap one did not. The matched pair talks, a mismatched ID
/// Context does not, no ID Context still talks (Requirement 4.11: nothing
/// changes for a deployment that never sets it), and a plaintext client is
/// refused, without which none of the others would mean anything.

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_oscore_id_context_test
#include <boost/test/unit_test.hpp>

#include "coap_wire_probe.hpp"
#include <raft/future_default.hpp>

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/console_logger.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using data_type = std::vector<std::byte>;
using test_serializer = kythira::json_rpc_serializer<data_type>;

struct test_types {
    using serializer_type = test_serializer;
    using serializer_registry_type = kythira::single_serializer_registry<test_serializer>;
    using rpc_serializer_type = test_serializer;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = int;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

auto credentials(std::byte sender, std::byte recipient, std::vector<std::byte> id_context)
    -> kythira::coap_security_config {
    kythira::oscore_credentials creds;
    creds.sender_id = {sender};
    creds.recipient_id = {recipient};
    creds.master_secret = std::vector<std::byte>(16, std::byte{0x5C});
    creds.master_salt = std::vector<std::byte>(8, std::byte{0x9E});
    creds.id_context = std::move(id_context);
    kythira::coap_security_config config;
    config.mode = kythira::coap_auth_mode::oscore;
    config.credentials = creds;
    return config;
}

/// One RequestVote from a client holding `client_context` to a server holding
/// `server_context`. True when the vote came back granted. A client with
/// `plaintext` set sends without OSCORE at all.
auto vote_round_trip(std::vector<std::byte> client_context, std::vector<std::byte> server_context,
                     bool plaintext = false) -> bool {
    std::uint16_t port = 0;
    {
        kythira::testing::udp_probe reservation;
        port = reservation.port();
    }
    kythira::coap_server_config server_config;
    server_config.security = credentials(std::byte{0x01}, std::byte{0x00}, server_context);
    kythira::coap_server<test_types> server("127.0.0.1", port, server_config,
                                            kythira::noop_metrics{});
    server.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        return kythira::request_vote_response<>{request.term(), true};
    });
    server.start();

    kythira::coap_client_config client_config;
    if (!plaintext) {
        client_config.security = credentials(std::byte{0x00}, std::byte{0x01}, client_context);
    }
    // A mismatch is answered with an error or not at all; either way the
    // client should stop retransmitting well inside the test's budget.
    client_config.ack_timeout = std::chrono::milliseconds{500};
    client_config.max_retransmit = 1;
    kythira::coap_client<test_types> client({{2, "coap://127.0.0.1:" + std::to_string(port)}},
                                            client_config, kythira::noop_metrics{});

    bool granted = false;
    try {
        auto future = client.send_request_vote(2, kythira::request_vote_request<>{5, 1, 0, 0},
                                               kythira::testing::scaled_deadline(3000));
        if (future.wait(kythira::testing::scaled_deadline(6000))) {
            const auto response = std::move(future).get();
            granted = response.vote_granted() && response.term() == 5U;
        }
    } catch (const std::exception& error) {
        BOOST_TEST_MESSAGE("round trip failed: " << error.what());
    }
    server.stop();
    return granted;
}

const std::vector<std::byte> group_7{std::byte{0x00}, std::byte{0x07}, std::byte{0xA1},
                                     std::byte{0xB2}, std::byte{0xC3}, std::byte{0xD4},
                                     std::byte{0xE5}, std::byte{0xF6}, std::byte{0x07}};
const std::vector<std::byte> group_8{std::byte{0x00}, std::byte{0x08}, std::byte{0xA1},
                                     std::byte{0xB2}, std::byte{0xC3}, std::byte{0xD4},
                                     std::byte{0xE5}, std::byte{0xF6}, std::byte{0x07}};

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_oscore_id_context_tests)

#ifdef LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(no_id_context_still_talks,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    if (coap_oscore_is_supported() == 0) {
        BOOST_TEST_MESSAGE("linked libcoap has no OSCORE; skipping");
        return;
    }
    BOOST_TEST(vote_round_trip({}, {}));
}

BOOST_AUTO_TEST_CASE(a_matched_id_context_talks,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    if (coap_oscore_is_supported() == 0) {
        BOOST_TEST_MESSAGE("linked libcoap has no OSCORE; skipping");
        return;
    }
    BOOST_TEST(vote_round_trip(group_7, group_7));
}

// The ID Context is in every key, so a client on another group's context
// cannot be understood, and must not be answered as if it were.
BOOST_AUTO_TEST_CASE(a_mismatched_id_context_does_not,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    if (coap_oscore_is_supported() == 0) {
        BOOST_TEST_MESSAGE("linked libcoap has no OSCORE; skipping");
        return;
    }
    BOOST_TEST(!vote_round_trip(group_8, group_7));
}

// Not an ID Context property, but the one that makes the others mean
// anything: before this work the libcoap server answered a plaintext RPC even
// with OSCORE configured, so a mismatched context was never really refused.
BOOST_AUTO_TEST_CASE(an_oscore_server_refuses_plaintext,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    if (coap_oscore_is_supported() == 0) {
        BOOST_TEST_MESSAGE("linked libcoap has no OSCORE; skipping");
        return;
    }
    BOOST_TEST(!vote_round_trip({}, group_7, true));
}

#else

BOOST_AUTO_TEST_CASE(skipped_without_libcoap) {
    BOOST_TEST_MESSAGE("libcoap not available - no libcoap OSCORE to configure");
    BOOST_TEST(true);
}

#endif

BOOST_AUTO_TEST_SUITE_END()
