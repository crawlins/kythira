// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_fetch_log_entries_test.cpp
/// @brief FetchLogEntries on the libcoap backend's wire
///        (.kiro/specs/peer2peer-log-replication/ Requirements 4.1/4.2).
///
///  * **The round trip, per serializer.** A `send_fetch_log_entries` reaches
///    a real server's handler with every field intact, `group_id` included,
///    and a response of sixty-four 100-byte entries comes back whole. That
///    response is several times `max_block_size`, so it only arrives if
///    libcoap's Block2 path carries it.
///  * **5.01 without a handler**, as `/raft/timeout_now` answers: the
///    resource exists whenever the server does.
///
/// The cantcoap and libnyoci backends have the same two cases in their
/// integration tests.

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_fetch_log_entries_test
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

using kythira::testing::udp_probe;

auto endpoint(std::uint16_t port) -> std::string {
    return "coap://127.0.0.1:" + std::to_string(port);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_fetch_log_entries_tests)

#ifdef LIBCOAP_AVAILABLE

BOOST_TEST_DECORATOR(*boost::unit_test::timeout(kythira::testing::scaled_timeout(30)))
BOOST_AUTO_TEST_CASE_TEMPLATE(fetch_log_entries_round_trips_through_each_serializer, Serializer,
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
    std::optional<kythira::fetch_log_entries_request<>> seen;
    server.register_fetch_log_entries_handler(
        [&](const kythira::fetch_log_entries_request<>& request) {
            {
                const std::lock_guard lock(seen_mutex);
                seen = request;
            }
            kythira::fetch_log_entries_response<> response;
            response._responder_id = 2;
            response._available = true;
            response._prev_log_term = 4;
            response._group_id = request._group_id;
            for (auto index = request._from_index; index <= request._to_index; ++index) {
                response._entries.push_back(
                    {5, index, std::vector<std::byte>(100, static_cast<std::byte>(index))});
            }
            return response;
        });
    server.start();

    kythira::coap_client<types> client({{2, endpoint(port)}}, kythira::coap_client_config{},
                                       kythira::noop_metrics{});

    kythira::fetch_log_entries_request<> request;
    request._requester_id = 3;
    request._from_index = 11;
    request._to_index = 74;
    request._group_id = 9001;

    auto future =
        client.send_fetch_log_entries(2, request, kythira::testing::scaled_deadline(5000));
    BOOST_REQUIRE(future.wait(kythira::testing::scaled_deadline(10000)));
    const auto response = std::move(future).get();
    server.stop();

    BOOST_TEST(response.responder_id() == 2u);
    BOOST_TEST(response.available());
    BOOST_TEST(response.prev_log_term() == 4u);
    BOOST_TEST(response.group_id() == 9001u);
    BOOST_REQUIRE_EQUAL(response.entries().size(), 64u);
    BOOST_TEST(response.entries().front().index() == 11u);
    BOOST_TEST(response.entries().back().index() == 74u);
    BOOST_TEST(response.entries().back().term() == 5u);
    BOOST_TEST((response.entries().back().command() ==
                std::vector<std::byte>(100, static_cast<std::byte>(74))));

    const std::lock_guard lock(seen_mutex);
    BOOST_REQUIRE(seen.has_value());
    BOOST_TEST(seen->_requester_id == 3u);
    BOOST_TEST(seen->_from_index == 11u);
    BOOST_TEST(seen->_to_index == 74u);
    BOOST_TEST(seen->_group_id == 9001u);
}

BOOST_AUTO_TEST_CASE(fetch_log_entries_without_a_handler_is_not_implemented,
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

    auto future = client.send_fetch_log_entries(2, kythira::fetch_log_entries_request<>{},
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
    BOOST_TEST_MESSAGE("libcoap not available - no real CoAP transport to fetch log entries over");
    BOOST_TEST(true);
}

#endif

BOOST_AUTO_TEST_SUITE_END()
