// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_oscore_group_libcoap_test.cpp
/// @brief Per-Raft-group OSCORE contexts on the libcoap backend
///        (.kiro/specs/coap-transport-multi-raft/ tasks 9-11).
///
/// oscore_group_contexts_test pins the registry's own properties: distinct keys
/// per group, one bootstrap for N groups, bounded recipient-side derivation,
/// LRU and TTL, zeroization, and the replay-window isolation the whole phase
/// exists for. This suite checks that the libcoap transport actually puts
/// them on the wire: with `oscore_groups` on, each group's requests travel
/// under that group's context, the server derives the matching one on first
/// sight, refuses a group it does not host without deriving anything, and
/// forgets a group's keys on request.

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_oscore_group_libcoap_test
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
#include <memory>
#include <set>
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

auto credentials(std::byte sender, std::byte recipient) -> kythira::coap_security_config {
    kythira::oscore_credentials creds;
    creds.sender_id = {sender};
    creds.recipient_id = {recipient};
    creds.master_secret = std::vector<std::byte>(16, std::byte{0x5C});
    creds.master_salt = std::vector<std::byte>(8, std::byte{0x9E});
    kythira::coap_security_config config;
    config.mode = kythira::coap_auth_mode::oscore;
    config.credentials = creds;
    return config;
}

/// One server hosting `hosted`, one client, both with per-group contexts on.
struct group_pair {
    explicit group_pair(std::set<std::uint64_t> hosted) : _hosted{std::move(hosted)} {
        {
            kythira::testing::udp_probe reservation;
            _port = reservation.port();
        }
        kythira::coap_server_config server_config;
        server_config.security = credentials(std::byte{0x01}, std::byte{0x00});
        server_config.oscore_groups.enabled = true;
        server_config.oscore_groups.hosts_group = [this](std::uint64_t group) {
            return _hosted.contains(group);
        };
        server = std::make_unique<kythira::coap_server<test_types>>(
            "127.0.0.1", _port, server_config, kythira::noop_metrics{});
        server->register_request_vote_handler([](const kythira::request_vote_request<>& request) {
            return kythira::request_vote_response<>{request.term(), true, request.group_id()};
        });
        server->start();

        kythira::coap_client_config client_config;
        client_config.security = credentials(std::byte{0x00}, std::byte{0x01});
        client_config.oscore_groups.enabled = true;
        // A refusal is answered at once, but keep a lost datagram from
        // outliving the case's budget.
        client_config.ack_timeout = std::chrono::milliseconds{1000};
        client_config.max_retransmit = 1;
        client = std::make_unique<kythira::coap_client<test_types>>(
            std::unordered_map<std::uint64_t, std::string>{
                {2, "coap://127.0.0.1:" + std::to_string(_port)}},
            client_config, kythira::noop_metrics{});
    }

    ~group_pair() { server->stop(); }

    /// One RequestVote in `group`; true when it came back granted for that group.
    auto vote(std::uint64_t group) -> bool {
        try {
            kythira::request_vote_request<> request{5, 1, 0, 0, group};
            auto future =
                client->send_request_vote(2, request, kythira::testing::scaled_deadline(3000));
            if (!future.wait(kythira::testing::scaled_deadline(6000))) {
                return false;
            }
            const auto response = std::move(future).get();
            return response.vote_granted() && response.group_id() == group;
        } catch (const std::exception& error) {
            BOOST_TEST_MESSAGE("vote in group " << group << " failed: " << error.what());
            return false;
        }
    }

    std::set<std::uint64_t> _hosted;
    std::uint16_t _port{0};
    std::unique_ptr<kythira::coap_server<test_types>> server;
    std::unique_ptr<kythira::coap_client<test_types>> client;
};

auto libcoap_has_its_own_oscore() -> bool {
    coap_startup();
    return coap_oscore_is_supported() != 0;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_oscore_group_libcoap_tests)

#ifdef LIBCOAP_AVAILABLE

// Task 9: N groups, N derivations on each side, one bootstrap. Repeated traffic
// in a group reuses its context rather than deriving again.
BOOST_AUTO_TEST_CASE(each_group_gets_its_own_context_from_one_bootstrap,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    if (libcoap_has_its_own_oscore()) {
        BOOST_TEST_MESSAGE("linked libcoap has its own OSCORE, which the backend refuses");
        return;
    }
    group_pair pair({7, 8});
    BOOST_TEST(pair.vote(7));
    BOOST_TEST(pair.vote(8));
    BOOST_TEST(pair.vote(7));
    BOOST_TEST(pair.vote(8));

    const auto client = pair.client->oscore_group_counters();
    const auto server = pair.server->oscore_group_counters();
    BOOST_REQUIRE(client.has_value());
    BOOST_REQUIRE(server.has_value());
    BOOST_TEST(client->bootstraps == 1U);
    BOOST_TEST(client->sender_derivations == 2U);
    BOOST_TEST(server->bootstraps == 1U);
    BOOST_TEST(server->recipient_derivations == 2U);
}

// Task 10: a kid context naming a group this node does not host is refused
// before any key is derived for it.
BOOST_AUTO_TEST_CASE(an_unhosted_group_is_refused_without_deriving,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    if (libcoap_has_its_own_oscore()) {
        BOOST_TEST_MESSAGE("linked libcoap has its own OSCORE, which the backend refuses");
        return;
    }
    group_pair pair({7});
    BOOST_TEST(!pair.vote(9));

    const auto server = pair.server->oscore_group_counters();
    BOOST_REQUIRE(server.has_value());
    BOOST_TEST(server->rejected_unhosted == 1U);
    BOOST_TEST(server->recipient_derivations == 0U);
    // The hosted group still works.
    BOOST_TEST(pair.vote(7));
}

// Task 11: forgetting a group wipes its contexts on both sides; the next
// request in it derives fresh ones rather than using wiped keys.
BOOST_AUTO_TEST_CASE(forgetting_a_group_wipes_and_rederives,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    if (libcoap_has_its_own_oscore()) {
        BOOST_TEST_MESSAGE("linked libcoap has its own OSCORE, which the backend refuses");
        return;
    }
    group_pair pair({7, 8});
    BOOST_TEST(pair.vote(7));
    BOOST_TEST(pair.vote(8));

    pair.client->forget_oscore_group(7);
    pair.server->forget_oscore_group(7);
    BOOST_TEST(pair.client->oscore_group_counters()->groups_forgotten == 1U);
    BOOST_TEST(pair.server->oscore_group_counters()->groups_forgotten == 1U);

    BOOST_TEST(pair.vote(7));
    BOOST_TEST(pair.vote(8));
    BOOST_TEST(pair.client->oscore_group_counters()->sender_derivations == 3U);
    BOOST_TEST(pair.server->oscore_group_counters()->recipient_derivations == 3U);
}

// Mixed configuration stays interoperable: a client without per-group
// contexts talks to a server with them over the base context, exactly as
// before the feature existed.
BOOST_AUTO_TEST_CASE(a_client_without_groups_still_talks_to_a_server_with_them,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    if (libcoap_has_its_own_oscore()) {
        BOOST_TEST_MESSAGE("linked libcoap has its own OSCORE, which the backend refuses");
        return;
    }
    group_pair pair({7});
    kythira::coap_client_config plain_groups;
    plain_groups.security = credentials(std::byte{0x00}, std::byte{0x01});
    kythira::coap_client<test_types> client({{2, "coap://127.0.0.1:" + std::to_string(pair._port)}},
                                            plain_groups, kythira::noop_metrics{});
    auto future = client.send_request_vote(2, kythira::request_vote_request<>{5, 1, 0, 0, 7},
                                           kythira::testing::scaled_deadline(3000));
    BOOST_REQUIRE(future.wait(kythira::testing::scaled_deadline(6000)));
    BOOST_TEST(std::move(future).get().vote_granted());
    BOOST_TEST(pair.server->oscore_group_counters()->recipient_derivations == 0U);
}

// The server must name hosts_group when per-group contexts are on: without it
// any kid context would be derived, which is the unbounded work Requirement
// 4.8 forbids.
BOOST_AUTO_TEST_CASE(a_server_without_hosts_group_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::coap_server_config server_config;
    server_config.security = credentials(std::byte{0x01}, std::byte{0x00});
    server_config.oscore_groups.enabled = true;
    BOOST_CHECK_THROW(
        kythira::coap_server<test_types>("127.0.0.1", 0, server_config, kythira::noop_metrics{}),
        kythira::coap_security_config_error);
}

#else

BOOST_AUTO_TEST_CASE(skipped_without_libcoap) {
    BOOST_TEST_MESSAGE("libcoap not available");
    BOOST_TEST(true);
}

#endif

BOOST_AUTO_TEST_SUITE_END()
