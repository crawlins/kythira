// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// **Feature: coap-transport-security, Requirements 4.2, 9.4**
// OSCORE over DTLS through the transport itself: a coap_client and a
// coap_server configured with security.mode == oscore and security.oscore_dtls
// complete a RequestVote over coaps://, and the server refuses a peer that
// has only one of the two layers.
//
// coap_oscore_over_dtls_test.cpp covers the provider on its own; this file
// covers what the transport adds: the session paths that must hand coaps://
// sessions to the provider, the enable_dtls requirement, and (with
// coap_oscore_only_resources_test.cpp) the Raft resources being OSCORE-only.
#define BOOST_TEST_MODULE coap_oscore_over_dtls_transport_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/console_logger.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_timeout_scale.hpp"

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>

#include <memory>

// within() on the Folly backend uses folly's Timekeeper singleton, which
// aborts unless folly::Init has run.
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("coap_oscore_over_dtls_transport_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

using namespace kythira;

namespace {

constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint64_t test_node_id = 1;
// Handed into the code under test as the RPC deadline, so it is scaled (see
// coap_cbor_end_to_end_test.cpp).
constexpr auto test_timeout = kythira::testing::scaled_deadline(5000);
// A refused request never answers; this bounds how long a negative case waits.
constexpr auto reject_timeout = std::chrono::milliseconds(2000);

struct test_transport_types {
    using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
    using serializer_registry_type = kythira::single_serializer_registry<serializer_type>;
    using rpc_serializer_type = serializer_type;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using executor_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;

    using future_type = kythira::future_default<kythira::request_vote_response<>>;
};

auto make_oscore(std::byte sender, std::byte recipient, std::byte secret_fill)
    -> oscore_credentials {
    oscore_credentials creds;
    creds.sender_id = {sender};
    creds.recipient_id = {recipient};
    creds.master_secret = std::vector<std::byte>(16, secret_fill);
    creds.master_salt = {std::byte{0x9e}, std::byte{0x7c}, std::byte{0xa9}, std::byte{0x22}};
    return creds;
}

auto make_psk(std::byte fill) -> psk_credentials {
    return psk_credentials{"kythira-node", std::vector<std::byte>(16, fill)};
}

// OSCORE over DTLS-PSK, as the server or the client side of one pairing.
auto combined(bool server_side, std::byte psk_fill = std::byte{0x42},
              std::byte secret_fill = std::byte{0x77}) -> coap_security_config {
    coap_security_config security;
    security.mode = coap_auth_mode::oscore;
    security.credentials = server_side ? make_oscore(std::byte{0x01}, std::byte{0x00}, secret_fill)
                                       : make_oscore(std::byte{0x00}, std::byte{0x01}, secret_fill);
    security.oscore_dtls = make_psk(psk_fill);
    return security;
}

struct counting_server {
    std::atomic<int> handled{0};
    noop_metrics metrics;
    coap_server<test_transport_types> server;

    counting_server(coap_server_config config)
        : server(test_bind_address, 0, std::move(config), metrics) {
        server.register_request_vote_handler(
            [this](const request_vote_request<>& req) -> request_vote_response<> {
                handled.fetch_add(1);
                return request_vote_response<>{req.term(), true};
            });
        server.start();
    }

    counting_server(const counting_server&) = delete;
    auto operator=(const counting_server&) -> counting_server& = delete;

    ~counting_server() { server.stop(); }
};

// Sends one RequestVote; true when it was granted within `timeout`.
auto request_vote(coap_client_config config, const std::string& scheme, std::uint16_t port,
                  std::chrono::milliseconds timeout) -> bool {
    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[test_node_id] = std::format("{}://{}:{}", scheme, test_bind_address, port);
    noop_metrics metrics;
    coap_client<test_transport_types> client(std::move(endpoints), std::move(config), metrics);
    // within() rather than wait(): the client does not enforce the RPC
    // deadline itself, and on the stdexec backend the future send_rpc returns
    // is a composed sender whose wait() reports ready at once, so get() would
    // block until libcoap gives up on a handshake that never completes.
    auto future =
        client
            .send_request_vote(test_node_id, request_vote_request<>{7, 42, 3, 6},
                               std::chrono::duration_cast<std::chrono::milliseconds>(timeout))
            .within(timeout + std::chrono::milliseconds(500));
    try {
        return std::move(future).get().vote_granted();
    } catch (const std::exception&) {
        return false;
    }
}

auto combined_server_config() -> coap_server_config {
    coap_server_config config;
    config.enable_dtls = true;
    config.security = combined(true);
    return config;
}

auto combined_client_config(coap_security_config security) -> coap_client_config {
    coap_client_config config;
    config.enable_dtls = true;
    config.security = std::move(security);
    return config;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_oscore_over_dtls_transport_tests)

#ifdef LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(request_vote_round_trips_over_oscore_inside_dtls,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    counting_server server(combined_server_config());
    BOOST_CHECK(request_vote(combined_client_config(combined(false)), "coaps",
                             server.server.bound_port(), test_timeout));
    BOOST_CHECK_EQUAL(server.handled.load(), 1);
}

// The OSCORE layer is required: a peer holding the DTLS key but no OSCORE
// context completes the handshake and is then refused.
BOOST_AUTO_TEST_CASE(dtls_only_peer_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    counting_server server(combined_server_config());
    coap_security_config dtls_only;
    dtls_only.mode = coap_auth_mode::dtls_psk;
    dtls_only.credentials = make_psk(std::byte{0x42});
    BOOST_CHECK(!request_vote(combined_client_config(dtls_only), "coaps",
                              server.server.bound_port(), reject_timeout));
    BOOST_CHECK_EQUAL(server.handled.load(), 0);
}

// The DTLS layer is required: a valid OSCORE context with the wrong DTLS key
// never gets a request through.
BOOST_AUTO_TEST_CASE(wrong_dtls_key_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    counting_server server(combined_server_config());
    BOOST_CHECK(!request_vote(combined_client_config(combined(false, std::byte{0x43})), "coaps",
                              server.server.bound_port(), reject_timeout));
    BOOST_CHECK_EQUAL(server.handled.load(), 0);
}

BOOST_AUTO_TEST_CASE(wrong_oscore_secret_is_refused_over_valid_dtls,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    counting_server server(combined_server_config());
    BOOST_CHECK(
        !request_vote(combined_client_config(combined(false, std::byte{0x42}, std::byte{0x22})),
                      "coaps", server.server.bound_port(), reject_timeout));
    BOOST_CHECK_EQUAL(server.handled.load(), 0);
}

#endif  // LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(client_with_oscore_dtls_requires_enable_dtls,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    coap_client_config config;
    config.security = combined(false);
    std::unordered_map<std::uint64_t, std::string> endpoints{
        {test_node_id, "coaps://127.0.0.1:5684"}};
    noop_metrics metrics;
    BOOST_CHECK_THROW(coap_client<test_transport_types>(endpoints, config, metrics),
                      coap_security_config_error);
}

BOOST_AUTO_TEST_CASE(server_with_oscore_dtls_requires_enable_dtls,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    coap_server_config config;
    config.security = combined(true);
    noop_metrics metrics;
    BOOST_CHECK_THROW(coap_server<test_transport_types>(test_bind_address, 0, config, metrics),
                      coap_security_config_error);
}

BOOST_AUTO_TEST_SUITE_END()
