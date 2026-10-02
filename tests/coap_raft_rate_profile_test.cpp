// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_raft_rate_profile_test.cpp
/// @brief The opt-in Raft-rate timer profile, and the defaults it must not
///        disturb (.kiro/specs/coap-transport-multi-raft/ task 12,
///        Requirement 6).
///
/// The defaults are asserted field by field against the literal RFC 7252
/// values, not against a default-constructed config: comparing a default to
/// itself would pass after any edit to it, and catching that edit is the
/// point. InstallSnapshot is then read off the wire under the profile, since
/// "stays confirmable" is a property of the bytes sent and nothing above the
/// transport can see it.

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_raft_rate_profile_test
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

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_raft_rate_profile_tests)

// Requirement 6.1. If this fails, a default changed: that is a behaviour
// change for every existing CoAP deployment, not a test to update.
BOOST_AUTO_TEST_CASE(the_rfc_7252_defaults_are_unchanged) {
    const kythira::coap_client_config defaults;
    BOOST_TEST(defaults.ack_timeout.count() == 2000);
    BOOST_TEST(defaults.ack_random_factor_ms.count() == 1000);
    BOOST_TEST(defaults.max_retransmit == 4u);
    BOOST_TEST(defaults.use_confirmable_messages);
    BOOST_TEST(defaults.retransmission_timeout.count() == 2000);
    BOOST_TEST(defaults.max_retransmissions == 4u);
}

// Requirement 6.2: shorter timers and NON heartbeats, and nothing else moved.
BOOST_AUTO_TEST_CASE(the_profile_shortens_the_timers_and_sends_non) {
    const kythira::coap_client_config defaults;
    const auto profile = kythira::raft_rate_profile();

    BOOST_TEST(!profile.use_confirmable_messages);
    BOOST_TEST(profile.ack_timeout < defaults.ack_timeout);
    BOOST_TEST(profile.ack_random_factor_ms < defaults.ack_random_factor_ms);
    BOOST_TEST(profile.max_retransmit < defaults.max_retransmit);
    BOOST_TEST(profile.retransmission_timeout < defaults.retransmission_timeout);
    BOOST_TEST(profile.max_retransmissions < defaults.max_retransmissions);

    // libcoap's floor: coap_session_set_ack_timeout() silently ignores a
    // timeout under 1 s and keeps its 2 s default, so a profile below it
    // would shorten nothing while appearing to.
    BOOST_TEST(profile.ack_timeout.count() >= 1000);

    // A timer profile, not a security or block-wise one.
    BOOST_TEST(profile.enable_block_transfer == defaults.enable_block_transfer);
    BOOST_TEST(profile.max_block_size == defaults.max_block_size);
    BOOST_TEST(profile.enable_dtls == defaults.enable_dtls);
    BOOST_TEST((profile.security.mode == defaults.security.mode));
}

#ifdef LIBCOAP_AVAILABLE

// Requirement 6.4. Under the profile an AppendEntries leaves as NON, and an
// InstallSnapshot from the same client still leaves as CON.
BOOST_AUTO_TEST_CASE(install_snapshot_stays_confirmable_under_the_profile,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::testing::udp_probe fake_server;
    kythira::coap_client<test_types> client({{2, fake_server.endpoint()}},
                                            kythira::raft_rate_profile(), kythira::noop_metrics{});

    // Neither future is ever answered; only the outgoing bytes matter.
    auto heartbeat = client.send_append_entries(2, kythira::append_entries_request<>{},
                                                kythira::testing::scaled_deadline(1000));
    const auto heartbeat_datagram = fake_server.receive();

    kythira::install_snapshot_request<> snapshot{};
    snapshot._term = 1;
    snapshot._leader_id = 1;
    snapshot._data = std::vector<std::byte>{std::byte{0x01}};
    snapshot._done = true;
    auto install =
        client.send_install_snapshot(2, snapshot, kythira::testing::scaled_deadline(1000));
    const auto install_datagram = fake_server.receive();

    BOOST_REQUIRE(heartbeat_datagram.has_value());
    BOOST_REQUIRE(install_datagram.has_value());
    BOOST_TEST(kythira::testing::coap_message_type(*heartbeat_datagram) ==
               kythira::testing::coap_type_non);
    BOOST_TEST(kythira::testing::coap_message_type(*install_datagram) ==
               kythira::testing::coap_type_con);
}

#endif

BOOST_AUTO_TEST_SUITE_END()
