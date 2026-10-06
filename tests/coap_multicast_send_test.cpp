// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * coap_client::send_multicast_message() over a real multicast group
 * (coap-transport Requirements 2.5, 13.1-13.4).
 *
 * send_multicast_message() used to return the literal bytes
 * "mock_multicast_response" without sending anything, and its return type --
 * a single response -- could not express what a multicast request gets back,
 * which is one answer per group member. discover_raft_nodes() and
 * send_multicast_heartbeat() were written against the right type, so neither
 * compiled the moment anything instantiated them.
 *
 * Here two real coap_servers join one group on one port, and a real client
 * sends one NON request to the group address. Both servers' answers have to
 * come back, in one collection.
 */
#define BOOST_TEST_MODULE coap_multicast_send_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/console_logger.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_timeout_scale.hpp"

using namespace kythira;

namespace {
// Administratively scoped (RFC 2365), so a test run cannot leak onto a real
// network's CoAP group.
constexpr const char* test_group = "239.255.83.17";
constexpr const char* any_address = "0.0.0.0";
// libcoap holds back each member's answer to a multicast request by a
// random delay of up to its default Leisure, 5s (RFC 7252 8.2), so the
// collection window has to be longer than that.
constexpr auto collection_window = kythira::testing::scaled_deadline(8000);

using test_serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;

struct test_transport_types {
    using serializer_type = test_serializer;
    using serializer_registry_type = kythira::single_serializer_registry<test_serializer>;
    using rpc_serializer_type = test_serializer;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = int;  // named, never invoked; see coap_conformance_types.hpp

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;

    using future_type = kythira::future_default<kythira::request_vote_response<>>;
};

// A UDP port nothing is bound to: bind an ephemeral one, note it, release it.
auto unused_udp_port() -> std::uint16_t {
    kythira::coap_server_config config;
    kythira::noop_metrics metrics;
    coap_server<test_transport_types> probe(any_address, 0, config, metrics);
    probe.start();
    const auto port = probe.bound_port();
    probe.stop();
    return port;
}

auto group_member_config(std::uint16_t port) -> kythira::coap_server_config {
    kythira::coap_server_config config;
    config.enable_dtls = false;
    config.enable_multicast = true;
    config.multicast_address = test_group;
    config.multicast_port = port;
    return config;
}

auto plain_client() -> coap_client<test_transport_types> {
    kythira::coap_client_config config;
    config.enable_dtls = false;
    kythira::noop_metrics metrics;
    return {{}, config, metrics};
}
}  // namespace

BOOST_AUTO_TEST_SUITE(coap_multicast_send_tests)

BOOST_AUTO_TEST_CASE(every_group_member_answers_one_request,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    const auto port = unused_udp_port();

    // Each member answers with its own term, so the two answers can be told
    // apart and a duplicate of one cannot pass for the other.
    kythira::noop_metrics metrics_a;
    coap_server<test_transport_types> member_a(any_address, port, group_member_config(port),
                                               metrics_a);
    member_a.register_request_vote_handler(
        [](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            return kythira::request_vote_response<>{req.term() + 100, true};
        });
    member_a.start();

    kythira::noop_metrics metrics_b;
    coap_server<test_transport_types> member_b(any_address, port, group_member_config(port),
                                               metrics_b);
    member_b.register_request_vote_handler(
        [](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            return kythira::request_vote_response<>{req.term() + 200, true};
        });
    member_b.start();

    auto client = plain_client();
    test_serializer serializer;
    const kythira::request_vote_request<> request{7, 42, 3, 6};

    auto future = client.send_multicast_message(test_group, port, "/raft/request_vote",
                                                serializer.serialize(request), collection_window);
    BOOST_REQUIRE(future.wait(collection_window * 2));
    const auto responses = std::move(future).get();

    std::set<std::uint64_t> terms;
    for (const auto& body : responses) {
        terms.insert(serializer.deserialize_request_vote_response(body).term());
    }
    BOOST_TEST(responses.size() == 2U);
    BOOST_TEST((terms == std::set<std::uint64_t>{107, 207}));

    member_b.stop();
    member_a.stop();
}

// Nobody in the group is a valid outcome, not an error: the request resolves
// with an empty collection once the window closes.
BOOST_AUTO_TEST_CASE(a_group_with_no_members_resolves_empty,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    auto client = plain_client();
    const std::vector<std::byte> payload{std::byte{'p'}, std::byte{'i'}, std::byte{'n'},
                                         std::byte{'g'}};
    const auto window = std::chrono::milliseconds{300};

    auto future = client.send_multicast_message(test_group, unused_udp_port(), "/raft/request_vote",
                                                payload, window);
    BOOST_REQUIRE(future.wait(kythira::testing::scaled_deadline(10000)));
    BOOST_TEST(std::move(future).get().empty());
}

// discover_raft_nodes() is the first caller of send_multicast_message() to be
// instantiated anywhere; with the old single-response type it did not
// compile. No member serves /raft/discovery, so it finds nobody.
BOOST_AUTO_TEST_CASE(discovery_compiles_and_finds_nobody_without_responders,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    auto client = plain_client();
    auto future =
        client.discover_raft_nodes(test_group, unused_udp_port(), std::chrono::milliseconds{300});
    BOOST_REQUIRE(future.wait(kythira::testing::scaled_deadline(10000)));
    BOOST_TEST(std::move(future).get().empty());
}

// A multicast request cannot go block-wise, so a body larger than one PDU is
// refused up front rather than truncated.
BOOST_AUTO_TEST_CASE(a_body_larger_than_one_pdu_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    auto client = plain_client();
    const std::vector<std::byte> payload(64 * 1024, std::byte{0x5a});

    auto future = client.send_multicast_message(test_group, unused_udp_port(), "/raft/request_vote",
                                                payload, std::chrono::milliseconds{300});
    BOOST_REQUIRE(future.wait(kythira::testing::scaled_deadline(10000)));
    BOOST_CHECK_THROW(std::move(future).get(), kythira::coap_network_error);
}

BOOST_AUTO_TEST_SUITE_END()
