// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * initiate_dtls_handshake() / complete_dtls_handshake() (coap-transport
 * Requirements 6.1, 6.3, 11.4).
 *
 * These were stubs that logged "(stub)" and returned true whenever DTLS was
 * enabled, and this file -- then coap_dtls_handshake_stub_test -- asserted
 * exactly that, against an endpoint nothing listened on. A handshake that
 * "succeeds" with no peer is the behaviour that test locked in.
 *
 * The client cases now run a real DTLS-PSK handshake against a real
 * coap_server on localhost, and check the three outcomes that matter: the
 * right key succeeds, the wrong key fails, and no peer fails.
 */
#define BOOST_TEST_MODULE coap_dtls_handshake_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_timeout_scale.hpp"

using namespace kythira;

using test_transport_types =
    kythira::default_transport_types<kythira::future_default<kythira::request_vote_response<>>,
                                     kythira::json_rpc_serializer<std::vector<std::byte>>,
                                     kythira::noop_metrics, kythira::console_logger>;

namespace {
constexpr const char* test_bind_address = "127.0.0.1";
const std::string psk_identity = "raft-node-handshake";
// 16 bytes: the minimum DTLS-PSK key length (coap_min_psk_key_length).
const std::vector<std::byte> psk_key = {
    std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
    std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88},
    std::byte{0x99}, std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC},
    std::byte{0xDD}, std::byte{0xEE}, std::byte{0xFF}, std::byte{0x10}};

auto psk_server_config() -> coap_server_config {
    coap_server_config config;
    config.enable_dtls = true;
    config.psk_identity = psk_identity;
    config.psk_key = psk_key;
    return config;
}

auto psk_client_config(std::vector<std::byte> key = psk_key) -> coap_client_config {
    coap_client_config config;
    config.enable_dtls = true;
    config.psk_identity = psk_identity;
    config.psk_key = std::move(key);
    return config;
}

// A localhost UDP port with nothing bound to it: bind an ephemeral one, note
// it, and let it go.
auto unused_udp_port() -> std::uint16_t {
    coap_server_config config;
    config.enable_dtls = false;
    test_transport_types::metrics_type metrics;
    coap_server<test_transport_types> probe(test_bind_address, 0, config, metrics);
    probe.start();
    const auto port = probe.bound_port();
    probe.stop();
    return port;
}
}  // namespace

#ifdef LIBCOAP_AVAILABLE
BOOST_AUTO_TEST_CASE(client_handshake_with_matching_psk_succeeds,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    test_transport_types::metrics_type server_metrics;
    coap_server<test_transport_types> server(test_bind_address, 0, psk_server_config(),
                                             server_metrics);
    server.start();
    const auto endpoint = std::format("coaps://{}:{}", test_bind_address, server.bound_port());

    test_transport_types::metrics_type client_metrics;
    coap_client<test_transport_types> client({{1, endpoint}}, psk_client_config(), client_metrics);

    BOOST_TEST(client.initiate_dtls_handshake(endpoint));
    BOOST_TEST(client.complete_dtls_handshake(endpoint));
    // Completing an already-established handshake is a no-op success.
    BOOST_TEST(client.complete_dtls_handshake(endpoint));

    server.stop();
}

BOOST_AUTO_TEST_CASE(client_handshake_with_wrong_psk_fails,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    test_transport_types::metrics_type server_metrics;
    coap_server<test_transport_types> server(test_bind_address, 0, psk_server_config(),
                                             server_metrics);
    server.start();
    const auto endpoint = std::format("coaps://{}:{}", test_bind_address, server.bound_port());

    auto wrong_key = psk_key;
    wrong_key.back() = std::byte{0x00};
    test_transport_types::metrics_type client_metrics;
    coap_client<test_transport_types> client({{1, endpoint}}, psk_client_config(wrong_key),
                                             client_metrics);

    BOOST_TEST(client.initiate_dtls_handshake(endpoint));
    BOOST_TEST(!client.complete_dtls_handshake(endpoint));

    server.stop();
}

BOOST_AUTO_TEST_CASE(client_handshake_with_no_peer_fails,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const auto endpoint = std::format("coaps://{}:{}", test_bind_address, unused_udp_port());
    test_transport_types::metrics_type client_metrics;
    coap_client<test_transport_types> client({{1, endpoint}}, psk_client_config(), client_metrics);

    // Starting is local -- a session exists and the ClientHello is queued --
    // so it succeeds; finishing needs a peer, so it does not.
    BOOST_TEST(client.initiate_dtls_handshake(endpoint));
    BOOST_TEST(!client.complete_dtls_handshake(endpoint));
}

BOOST_AUTO_TEST_CASE(client_handshake_rejects_plain_coap_endpoint,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    const std::string endpoint = "coap://127.0.0.1:5683";
    test_transport_types::metrics_type client_metrics;
    coap_client<test_transport_types> client({{1, endpoint}}, psk_client_config(), client_metrics);

    BOOST_TEST(!client.initiate_dtls_handshake(endpoint));
    BOOST_TEST(!client.complete_dtls_handshake(endpoint));
}
#endif  // LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(client_handshake_without_dtls_returns_false,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    coap_client_config client_config;
    client_config.enable_dtls = false;
    const std::string endpoint = "coaps://127.0.0.1:5684";
    test_transport_types::metrics_type metrics;
    coap_client<test_transport_types> client({{1, endpoint}}, client_config, metrics);

    BOOST_TEST(!client.initiate_dtls_handshake(endpoint));
    BOOST_TEST(!client.complete_dtls_handshake(endpoint));
}

BOOST_AUTO_TEST_CASE(server_handshake_with_null_session_returns_false,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    test_transport_types::metrics_type metrics;
    coap_server<test_transport_types> server(test_bind_address, 0, psk_server_config(), metrics);

#ifdef LIBCOAP_AVAILABLE
    BOOST_TEST(!server.initiate_dtls_handshake(nullptr));
    BOOST_TEST(!server.complete_dtls_handshake(nullptr));
#else
    // Without libcoap there is no session type to inspect; the stub build
    // keeps its historical answer.
    BOOST_TEST(server.initiate_dtls_handshake(nullptr));
    BOOST_TEST(server.complete_dtls_handshake(nullptr));
#endif
}

BOOST_AUTO_TEST_CASE(server_handshake_without_dtls_returns_false,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    coap_server_config server_config;
    server_config.enable_dtls = false;
    test_transport_types::metrics_type metrics;
    coap_server<test_transport_types> server(test_bind_address, 0, server_config, metrics);

    BOOST_TEST(!server.initiate_dtls_handshake(nullptr));
    BOOST_TEST(!server.complete_dtls_handshake(nullptr));
}
