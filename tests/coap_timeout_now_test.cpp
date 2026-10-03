// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_timeout_now_test.cpp
/// @brief TimeoutNow and PreVote on the libcoap backend's wire
///        (.kiro/specs/coap-transport-multi-raft/ task 6, Requirement 3;
///        .kiro/specs/http-coap-pre-vote-timeout-now/ tasks 8-9).
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
/// PreVote gets the same round trip, the opposite reliability rule (it follows
/// the config, like RequestVote), and the two "not implemented" outcomes a
/// mixed-version cluster depends on: 5.01 from a server with no handler and
/// 4.04 from a peer with no resource both reach the caller as
/// `rpc_not_implemented_exception`, while a 4.04 on RequestVote does not.
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
#include <raft/exceptions.hpp>
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

/// The exception a settled future failed with, or nullptr if it succeeded or
/// never settled.
template<typename Future> auto failure_of(Future future) -> std::exception_ptr {
    if (!future.wait(kythira::testing::scaled_deadline(10000))) {
        return nullptr;
    }
    try {
        std::ignore = std::move(future).get();
    } catch (...) {
        return std::current_exception();
    }
    return nullptr;
}

template<typename T> auto is_a(const std::exception_ptr& e) -> bool {
    if (!e) {
        return false;
    }
    try {
        std::rethrow_exception(e);
    } catch (const T&) {
        return true;
    } catch (...) {
        return false;
    }
}

// RFC 7252 Section 12.1.2, as the class << 5 | detail byte on the wire.
constexpr std::uint8_t coap_code_not_found = (4U << 5U) | 4U;

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
// cannot transfer" from "this is not a Raft endpoint". The client turns that
// 5.01 into rpc_not_implemented_exception, which transfer_leadership()
// reports as unsupported.
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

    const auto error = failure_of(client.send_timeout_now(2, kythira::timeout_now_request<>{},
                                                          kythira::testing::scaled_deadline(5000)));
    server.stop();
    BOOST_TEST(is_a<kythira::rpc_not_implemented_exception>(error));
}

// ── PreVote ──────────────────────────────────────────────────────────────────

BOOST_TEST_DECORATOR(*boost::unit_test::timeout(kythira::testing::scaled_timeout(30)))
BOOST_AUTO_TEST_CASE_TEMPLATE(pre_vote_round_trips_through_each_serializer, Serializer,
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
    std::optional<kythira::request_pre_vote_request<>> seen;
    server.register_request_pre_vote_handler(
        [&](const kythira::request_pre_vote_request<>& request) {
            {
                const std::lock_guard lock(seen_mutex);
                seen = request;
            }
            kythira::request_pre_vote_response<> response;
            response._term = request._term;
            response._vote_granted = true;
            response._group_id = request._group_id;
            return response;
        });
    server.start();

    kythira::coap_client<types> client({{2, endpoint(port)}}, kythira::coap_client_config{},
                                       kythira::noop_metrics{});

    kythira::request_pre_vote_request<> request;
    request._term = 8;
    request._candidate_id = 3;
    request._last_log_index = 41;
    request._last_log_term = 6;
    request._group_id = 9002;

    auto future = client.send_request_pre_vote(2, request, kythira::testing::scaled_deadline(5000));
    BOOST_REQUIRE(future.wait(kythira::testing::scaled_deadline(10000)));
    const auto response = std::move(future).get();
    server.stop();

    BOOST_TEST(response._term == 8u);
    BOOST_TEST(response._vote_granted);
    BOOST_TEST(response._group_id == 9002u);

    const std::lock_guard lock(seen_mutex);
    BOOST_REQUIRE(seen.has_value());
    BOOST_TEST(seen->_term == 8u);
    BOOST_TEST(seen->_candidate_id == 3u);
    BOOST_TEST(seen->_last_log_index == 41u);
    BOOST_TEST(seen->_last_log_term == 6u);
    BOOST_TEST(seen->_group_id == 9002u);
}

// Requirement 5.3: a pre-vote takes RequestVote's reliability, so with the
// config saying NON it leaves as NON, unlike TimeoutNow above.
BOOST_AUTO_TEST_CASE(pre_vote_follows_the_configured_reliability,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    using types = test_types<kythira::json_rpc_serializer<data_type>>;
    udp_probe fake_server;

    kythira::coap_client_config config;
    config.use_confirmable_messages = false;
    kythira::coap_client<types> client({{2, endpoint(fake_server.port())}}, config,
                                       kythira::noop_metrics{});

    auto pre_vote = client.send_request_pre_vote(2, kythira::request_pre_vote_request<>{},
                                                 kythira::testing::scaled_deadline(1000));
    const auto datagram = fake_server.receive();
    BOOST_REQUIRE(datagram.has_value());
    BOOST_TEST(coap_message_type(*datagram) == coap_type_non);
}

// Requirement 3.2 and 6.4: no handler means 5.01, which the client reports as
// rpc_not_implemented_exception.
BOOST_AUTO_TEST_CASE(pre_vote_without_a_handler_is_not_implemented,
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

    const auto error = failure_of(client.send_request_pre_vote(
        2, kythira::request_pre_vote_request<>{}, kythira::testing::scaled_deadline(5000)));
    server.stop();
    BOOST_TEST(is_a<kythira::rpc_not_implemented_exception>(error));
}

// Requirement 6.3: a peer on a build without the resource answers 4.04. On an
// extension RPC that is "older peer", reported as rpc_not_implemented_exception
// after one exchange; on RequestVote it stays the ordinary 4.04 client error,
// since a peer without RequestVote is misconfigured, not older (Requirement
// 3.4).
BOOST_AUTO_TEST_CASE(not_found_means_not_implemented_only_on_an_extension,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    using types = test_types<kythira::json_rpc_serializer<data_type>>;
    udp_probe older_peer;
    kythira::coap_client<types> client({{2, endpoint(older_peer.port())}},
                                       kythira::coap_client_config{}, kythira::noop_metrics{});

    auto pre_vote = client.send_request_pre_vote(2, kythira::request_pre_vote_request<>{},
                                                 kythira::testing::scaled_deadline(5000));
    BOOST_REQUIRE(older_peer.answer(coap_code_not_found).has_value());
    const auto pre_vote_error = failure_of(std::move(pre_vote));
    BOOST_TEST(is_a<kythira::rpc_not_implemented_exception>(pre_vote_error));

    auto transfer = client.send_timeout_now(2, kythira::timeout_now_request<>{},
                                            kythira::testing::scaled_deadline(5000));
    BOOST_REQUIRE(older_peer.answer(coap_code_not_found).has_value());
    BOOST_TEST(is_a<kythira::rpc_not_implemented_exception>(failure_of(std::move(transfer))));

    auto vote = client.send_request_vote(2, kythira::request_vote_request<>{},
                                         kythira::testing::scaled_deadline(5000));
    BOOST_REQUIRE(older_peer.answer(coap_code_not_found).has_value());
    const auto vote_error = failure_of(std::move(vote));
    BOOST_TEST(!is_a<kythira::rpc_not_implemented_exception>(vote_error));
    BOOST_TEST(is_a<kythira::coap_client_error>(vote_error));
}

#else

BOOST_AUTO_TEST_CASE(skipped_without_libcoap) {
    BOOST_TEST_MESSAGE("libcoap not available - no real CoAP transport to send TimeoutNow over");
    BOOST_TEST(true);
}

#endif

BOOST_AUTO_TEST_SUITE_END()
