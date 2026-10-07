// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// **Feature: coap-transport-security, Requirement 4**
// The Raft resources of a coap_server in OSCORE mode accept only
// OSCORE-protected requests. libcoap's OSCORE server context verifies the
// requests that carry OSCORE, but on its own still hands a plain request to
// the handler.
#define BOOST_TEST_MODULE coap_oscore_only_resources_test
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
        char* argv0[] = {const_cast<char*>("coap_oscore_only_resources_test"), nullptr};
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
// A refused request may never answer; this bounds how long the case waits.
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
    creds.volatile_sequence_state = true;  // test-only keys
    creds.sender_id = {sender};
    creds.recipient_id = {recipient};
    creds.master_secret = std::vector<std::byte>(16, secret_fill);
    creds.master_salt = {std::byte{0x9e}, std::byte{0x7c}, std::byte{0xa9}, std::byte{0x22}};
    return creds;
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
    // within() rather than wait(): see coap_oscore_over_dtls_transport_test.cpp.
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

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_oscore_only_resources_tests)

#ifdef LIBCOAP_AVAILABLE

// A request with no OSCORE option at all used to reach the handler.
BOOST_AUTO_TEST_CASE(plain_oscore_server_refuses_unprotected_request,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    coap_server_config server_config;
    server_config.security.mode = coap_auth_mode::oscore;
    server_config.security.credentials =
        make_oscore(std::byte{0x01}, std::byte{0x00}, std::byte{0x77});
    counting_server server(std::move(server_config));

    BOOST_CHECK(
        !request_vote(coap_client_config{}, "coap", server.server.bound_port(), reject_timeout));
    BOOST_CHECK_EQUAL(server.handled.load(), 0);
}

#endif  // LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_SUITE_END()
