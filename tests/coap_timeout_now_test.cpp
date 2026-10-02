// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_timeout_now_test.cpp
/// @brief TimeoutNow on the libcoap backend's wire
///        (.kiro/specs/coap-transport-multi-raft/ task 6, Requirement 3).
///
/// Two things are checked here and nowhere else:
///
///  * **The round trip, per serializer.** A `send_timeout_now` from a real
///    client reaches a real server's handler with every field intact —
///    `group_id` included, since multi-Raft's scatter is the reason this RPC
///    exists on CoAP — and the response comes back the same way. Run once per
///    serializer this build has, because the encoding is whatever
///    `Types::serializer_type` produces (Requirement 3.3) and the serializers,
///    not the transport, are where a field goes missing.
///  * **Always confirmable.** With `use_confirmable_messages = false` a
///    RequestVote leaves as NON, and a TimeoutNow still leaves as CON
///    (Requirement 3.4). Observed on the wire, from a raw socket standing in
///    for the server, since the message type is not visible above the
///    transport.
///
/// The end-to-end leadership-transfer check lives in
/// coap_raft_node_integration_test.cpp, beside the other node-over-CoAP cases.

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_timeout_now_test
#include <boost/test/unit_test.hpp>

#include "coap_wire_probe.hpp"
#include <raft/future_default.hpp>

#include <raft/cbor_serializer.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/console_logger.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>
#if defined(KYTHIRA_COAP_TEST_HAS_PROTOBUF)
#include <raft/protobuf_serializer.hpp>
#endif
#if defined(KYTHIRA_ION_SERIALIZER_AVAILABLE)
#include <raft/ion_serializer.hpp>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace {

using data_type = std::vector<std::byte>;

template<typename Serializer> struct test_types {
    using serializer_type = Serializer;
    using serializer_registry_type = kythira::single_serializer_registry<Serializer>;
    using rpc_serializer_type = Serializer;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = int;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

using serializers =
    std::tuple<kythira::json_rpc_serializer<data_type>, kythira::cbor_rpc_serializer<data_type>
#if defined(KYTHIRA_COAP_TEST_HAS_PROTOBUF)
               ,
               kythira::protobuf_rpc_serializer<data_type>
#endif
#if defined(KYTHIRA_ION_SERIALIZER_AVAILABLE)
               ,
               kythira::ion_rpc_serializer<data_type>
#endif
               >;

using kythira::testing::coap_message_type;
using kythira::testing::coap_type_con;
using kythira::testing::coap_type_non;
using kythira::testing::udp_probe;

auto endpoint(std::uint16_t port) -> std::string {
    return "coap://127.0.0.1:" + std::to_string(port);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_timeout_now_tests)

#ifdef LIBCOAP_AVAILABLE

BOOST_TEST_DECORATOR(*boost::unit_test::timeout(kythira::testing::scaled_timeout(30)))
BOOST_AUTO_TEST_CASE_TEMPLATE(timeout_now_round_trips_through_each_serializer, Serializer,
                              serializers) {
    using types = test_types<Serializer>;
    BOOST_TEST_MESSAGE("serializer: " << Serializer{}.name());

    std::uint16_t port = 0;
    {
        udp_probe reservation;
        port = reservation.port();
    }
    kythira::coap_server<types> server("127.0.0.1", port, kythira::coap_server_config{},
                                       kythira::noop_metrics{});

    std::mutex seen_mutex;
    std::optional<kythira::timeout_now_request<>> seen;
    server.register_timeout_now_handler([&](const kythira::timeout_now_request<>& request) {
        {
            const std::lock_guard lock(seen_mutex);
            seen = request;
        }
        kythira::timeout_now_response<> response;
        response._term = request._term;
        response._success = true;
        response._group_id = request._group_id;
        return response;
    });
    server.start();

    kythira::coap_client<types> client({{2, endpoint(port)}}, kythira::coap_client_config{},
                                       kythira::noop_metrics{});

    kythira::timeout_now_request<> request;
    request._term = 7;
    request._leader_id = 1;
    request._last_log_index = 42;
    request._group_id = 9001;

    auto future = client.send_timeout_now(2, request, kythira::testing::scaled_deadline(5000));
    BOOST_REQUIRE(future.wait(kythira::testing::scaled_deadline(10000)));
    const auto response = std::move(future).get();
    server.stop();

    BOOST_TEST(response._term == 7u);
    BOOST_TEST(response._success);
    BOOST_TEST(response._group_id == 9001u);

    const std::lock_guard lock(seen_mutex);
    BOOST_REQUIRE(seen.has_value());
    BOOST_TEST(seen->_term == 7u);
    BOOST_TEST(seen->_leader_id == 1u);
    BOOST_TEST(seen->_last_log_index == 42u);
    BOOST_TEST(seen->_group_id == 9001u);
}

// Requirement 3.4. The same client, configured for NON, sends one RequestVote
// and one TimeoutNow to a raw socket; only the RequestVote follows the config.
BOOST_AUTO_TEST_CASE(timeout_now_is_confirmable_when_the_config_says_non,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    using types = test_types<kythira::json_rpc_serializer<data_type>>;
    udp_probe fake_server;

    kythira::coap_client_config config;
    config.use_confirmable_messages = false;
    kythira::coap_client<types> client({{2, endpoint(fake_server.port())}}, config,
                                       kythira::noop_metrics{});

    // Neither future is ever answered; only the outgoing bytes matter.
    auto vote = client.send_request_vote(2, kythira::request_vote_request<>{},
                                         kythira::testing::scaled_deadline(1000));
    const auto vote_datagram = fake_server.receive();
    auto transfer = client.send_timeout_now(2, kythira::timeout_now_request<>{},
                                            kythira::testing::scaled_deadline(1000));
    const auto transfer_datagram = fake_server.receive();

    BOOST_REQUIRE(vote_datagram.has_value());
    BOOST_REQUIRE(transfer_datagram.has_value());
    BOOST_TEST(coap_message_type(*vote_datagram) == coap_type_non);
    BOOST_TEST(coap_message_type(*transfer_datagram) == coap_type_con);
}

// An unregistered handler answers 5.01 Not Implemented, never 4.04: the
// resource exists whenever the server does, so a peer can tell "this node
// cannot transfer" from "this is not a Raft endpoint".
BOOST_AUTO_TEST_CASE(timeout_now_without_a_handler_is_not_implemented,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    using types = test_types<kythira::json_rpc_serializer<data_type>>;
    std::uint16_t port = 0;
    {
        udp_probe reservation;
        port = reservation.port();
    }
    kythira::coap_server<types> server("127.0.0.1", port, kythira::coap_server_config{},
                                       kythira::noop_metrics{});
    server.start();
    kythira::coap_client<types> client({{2, endpoint(port)}}, kythira::coap_client_config{},
                                       kythira::noop_metrics{});

    auto future = client.send_timeout_now(2, kythira::timeout_now_request<>{},
                                          kythira::testing::scaled_deadline(5000));
    bool not_implemented = false;
    try {
        BOOST_REQUIRE(future.wait(kythira::testing::scaled_deadline(10000)));
        std::ignore = std::move(future).get();
    } catch (const kythira::coap_server_error& error) {
        not_implemented = error.response_code() == COAP_RESPONSE_CODE_NOT_IMPLEMENTED;
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
    server.stop();
    BOOST_TEST(not_implemented);
}

#else

BOOST_AUTO_TEST_CASE(skipped_without_libcoap) {
    BOOST_TEST_MESSAGE("libcoap not available - no real CoAP transport to send TimeoutNow over");
    BOOST_TEST(true);
}

#endif

BOOST_AUTO_TEST_SUITE_END()
