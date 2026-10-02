// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_cantcoap_integration_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT (240 * KYTHIRA_TEST_TIMEOUT_SCALE)

#include <raft/json_serializer.hpp>
#include <raft/network.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/coap_transport_cantcoap_impl.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// This backend carries far more hand-written logic than the other two --
// cantcoap supplies only the codec, so the socket, retransmission, duplicate
// suppression and block-wise sequencing are all ours. The tests are weighted
// accordingly: the round trips matter, but so do the failure paths that only
// exist because we own the stack.

namespace {
using test_serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;
using test_metrics = kythira::noop_metrics;

struct test_types {
    using serializer_type = test_serializer;
    using serializer_registry_type = kythira::single_serializer_registry<test_serializer>;
    using rpc_serializer_type = test_serializer;
    using metrics_type = test_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = folly::Executor;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

using test_client = kythira::coap_cantcoap_client<test_types>;
using test_server = kythira::coap_cantcoap_server<test_types>;

constexpr std::uint64_t peer_node_id = 9;
constexpr const char* loopback = "127.0.0.1";
constexpr std::uint16_t ephemeral_port = 0;

[[nodiscard]] auto endpoint_for(std::uint16_t port) -> std::string {
    return std::string{"coap://"} + loopback + ":" + std::to_string(port);
}

[[nodiscard]] auto fast_client_config() -> kythira::coap_client_config {
    kythira::coap_client_config config;
    config.use_confirmable_messages = true;
    // Keep the retransmission schedule short so the exhaustion test finishes in
    // seconds rather than the RFC 7252 default's minute-plus.
    config.ack_timeout = std::chrono::milliseconds{200};
    config.ack_random_factor_ms = std::chrono::milliseconds{50};
    config.max_retransmit = 2;
    return config;
}

/// A port nothing is listening on, obtained by binding and releasing.
///
/// IPv4 on purpose, like `loopback`: the backend's dual-stack socket receives
/// v4 either way, and a host with no IPv6 at all (where the backend falls back
/// to AF_INET) cannot create an AF_INET6 socket for these helpers to use.
[[nodiscard]] auto reserve_dead_port() -> std::uint16_t {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t length = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length);
    const auto port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

/// Fire raw bytes at a port. Used to prove the server survives garbage.
auto send_raw(std::uint16_t port, const std::vector<std::uint8_t>& bytes) -> void {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, loopback, &addr.sin_addr);
    ::sendto(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::close(fd);
}

[[nodiscard]] auto hex(const std::string& s) -> std::vector<std::byte> {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return c - 'A' + 10;
    };
    std::vector<std::byte> out;
    out.reserve(s.size() / 2);
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<std::byte>((nibble(s[i]) << 4) | nibble(s[i + 1])));
    }
    return out;
}

// The RFC 9529 test credentials coap_edhoc_oscore_bootstrap_test and the
// libnyoci backend's EDHOC tests use. Known-good against lakers, so a failure
// here is this backend's plumbing rather than the credentials.
[[nodiscard]] auto cred_i() -> std::vector<std::byte> {
    return hex(
        "A2027734322D35302D33312D46462D45462D33372D33322D333908A101A5010202412B2001215820AC75E9EC"
        "E3E50BFC8ED60399889522405C47BF16DF96660A41298CB4307F7EB62258206E5DE611388A4B8A8211334AC7D"
        "37ECB52A387D257E6DB3C2A93DF21FF3AFFC8");
}
[[nodiscard]] auto cred_r() -> std::vector<std::byte> {
    return hex(
        "A2026008A101A5010202410A2001215820BBC34960526EA4D32E940CAD2A234148DDC21791A12AFBCBAC93622"
        "046DD44F02258204519E257236B2A0CE2023F0931F1F386CA7AFDA64FCDE0108C224C51EABF6072");
}
[[nodiscard]] auto i_key() -> std::vector<std::byte> {
    return hex("fb13adeb6518cee5f88417660841142e830a81fe334380a953406a1305e8706b");
}
[[nodiscard]] auto r_key() -> std::vector<std::byte> {
    return hex("72cc4761dbd4c78f758931aa589d348d1ef874a7e303ede2f140dcf3e6aa4aac");
}

/// OSCORE credentials whose context is to be *derived* by EDHOC rather than
/// supplied: no master secret here at all, which is the point.
[[nodiscard, maybe_unused]] auto edhoc_security(bool is_client) -> kythira::coap_security_config {
    kythira::oscore_credentials creds;
    creds.bootstrap_method = kythira::oscore_bootstrap::edhoc;
    creds.edhoc.is_initiator = is_client;
    creds.edhoc.identity_credential = is_client ? cred_i() : cred_r();
    creds.edhoc.identity_private_key = is_client ? i_key() : r_key();
    creds.edhoc.peer_credential = is_client ? cred_r() : cred_i();

    kythira::coap_security_config config;
    config.mode = kythira::coap_auth_mode::oscore;
    config.credentials = creds;
    return config;
}
}

BOOST_AUTO_TEST_SUITE(coap_cantcoap_integration_tests)

#ifdef CANTCOAP_AVAILABLE

// ── Requirement 8.2: end-to-end round trip for all three RPCs ──────────────

BOOST_AUTO_TEST_CASE(test_request_vote_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    test_server server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
    server.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        return kythira::request_vote_response<>{request.term(), request.candidate_id() == 17};
    });
    server.start();
    BOOST_TEST(server.is_running());
    BOOST_TEST(server.bound_port() != 0);

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, fast_client_config(), test_metrics{}};

    const kythira::request_vote_request<> request{6, 17, 4, 5};
    const auto response =
        client.send_request_vote(peer_node_id, request, std::chrono::seconds{15}).get();
    BOOST_TEST(response.term() == 6U);
    BOOST_TEST(response.vote_granted());

    server.stop();
    BOOST_TEST(!server.is_running());
}

BOOST_AUTO_TEST_CASE(test_append_entries_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    test_server server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
    server.register_append_entries_handler([](const kythira::append_entries_request<>& request) {
        kythira::append_entries_response<> response{};
        response._term = request.term();
        response._success = !request.entries().empty();
        response._conflict_index = request.prev_log_index() + request.entries().size();
        return response;
    });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, fast_client_config(), test_metrics{}};

    kythira::append_entries_request<> request{};
    request._term = 3;
    request._leader_id = 1;
    request._prev_log_index = 20;
    request._prev_log_term = 2;
    request._entries.push_back(
        kythira::log_entry<>{3, 21, std::vector<std::byte>{std::byte{0xAA}, std::byte{0xBB}},
                             kythira::entry_type::normal});

    const auto response =
        client.send_append_entries(peer_node_id, request, std::chrono::seconds{15}).get();
    BOOST_TEST(response.term() == 3U);
    BOOST_TEST(response.success());
    BOOST_REQUIRE(response.conflict_index().has_value());
    BOOST_TEST(*response.conflict_index() == 21U);

    server.stop();
}

BOOST_AUTO_TEST_CASE(test_install_snapshot_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    test_server server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
    server.register_install_snapshot_handler(
        [](const kythira::install_snapshot_request<>& request) {
            return kythira::install_snapshot_response<>{request.last_included_index()};
        });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, fast_client_config(), test_metrics{}};

    kythira::install_snapshot_request<> request{};
    request._term = 2;
    request._leader_id = 1;
    request._last_included_index = 250;
    request._last_included_term = 1;
    request._data = std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}};
    request._done = true;

    const auto response =
        client.send_install_snapshot(peer_node_id, request, std::chrono::seconds{15}).get();
    BOOST_TEST(response.term() == 250U);

    server.stop();
}

// ── Requirement 8.4 / 5.1: block-wise, in both directions ──────────────────

// cantcoap encodes one message; the sequencing is entirely ours. A 12 KiB
// snapshot against a 256-byte block size walks ~48 Block1 rounds, which is a
// real exercise of the state machine rather than a single boundary.
BOOST_AUTO_TEST_CASE(test_large_request_over_block1,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    kythira::coap_server_config server_config;
    server_config.enable_block_transfer = true;
    server_config.max_block_size = 256;

    test_server server{loopback, ephemeral_port, server_config, test_metrics{}};
    std::size_t received = 0;
    server.register_install_snapshot_handler(
        [&received](const kythira::install_snapshot_request<>& request) {
            received = request.data().size();
            return kythira::install_snapshot_response<>{request.last_included_index()};
        });
    server.start();

    auto config = fast_client_config();
    config.enable_block_transfer = true;
    config.max_block_size = 256;
    test_client client{{{peer_node_id, endpoint_for(server.bound_port())}}, config, test_metrics{}};

    constexpr std::size_t snapshot_size = 12 * 1024;
    std::vector<std::byte> snapshot(snapshot_size);
    for (std::size_t i = 0; i < snapshot_size; ++i) {
        snapshot[i] = static_cast<std::byte>(i & 0xFF);
    }

    kythira::install_snapshot_request<> request{};
    request._term = 1;
    request._leader_id = 1;
    request._last_included_index = 4242;
    request._data = snapshot;
    request._done = true;

    const auto response =
        client.send_install_snapshot(peer_node_id, request, std::chrono::seconds{90}).get();
    BOOST_TEST(response.term() == 4242U);
    BOOST_TEST(received == snapshot_size, "server reassembled " << received << " bytes");

    server.stop();
}

// ── Requirement 8.3: reliability, which cantcoap provides none of ──────────

// Nothing is listening, so every retransmission goes unanswered and the
// exchange must fail after MAX_RETRANSMIT rather than hanging. With the fast
// schedule above that is ~0.2 + 0.4 + 0.8s, so a comfortable margin still
// proves the schedule terminates.
BOOST_AUTO_TEST_CASE(test_retransmission_exhaustion_rejects,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const auto dead_port = reserve_dead_port();
    BOOST_TEST(dead_port != 0);

    test_client client{
        {{peer_node_id, endpoint_for(dead_port)}}, fast_client_config(), test_metrics{}};

    const kythira::request_vote_request<> request{1, 1, 0, 0};
    const auto started = std::chrono::steady_clock::now();
    bool rejected = false;
    try {
        (void)client.send_request_vote(peer_node_id, request, std::chrono::seconds{30}).get();
    } catch (const kythira::coap_timeout_error&) {
        rejected = true;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    BOOST_TEST(rejected, "an unanswered confirmable request must fail, not hang");
    // It must not have given up instantly either -- that would mean the
    // retransmission schedule never ran.
    BOOST_TEST(elapsed > std::chrono::milliseconds{300},
               "gave up before retransmitting; the backoff schedule did not run");
    BOOST_TEST(elapsed < std::chrono::seconds{25}, "the schedule did not terminate promptly");
}

// A response duplicated on the wire must resolve the future exactly once. The
// second copy carries the same Message ID and has to be dropped by
// received_message_info, not delivered again.
BOOST_AUTO_TEST_CASE(test_duplicate_requests_are_suppressed,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    test_server server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
    std::atomic<int> invocations{0};
    server.register_request_vote_handler(
        [&invocations](const kythira::request_vote_request<>& request) {
            ++invocations;
            return kythira::request_vote_response<>{request.term(), true};
        });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, fast_client_config(), test_metrics{}};
    const kythira::request_vote_request<> request{1, 1, 0, 0};
    const auto response =
        client.send_request_vote(peer_node_id, request, std::chrono::seconds{15}).get();
    BOOST_TEST(response.vote_granted());

    // The handler must have run exactly once: no spurious retransmission
    // reached it, and no duplicate was processed twice.
    BOOST_TEST(invocations.load() == 1, "handler ran " << invocations.load() << " times");

    server.stop();
}

// ── Requirement 8.5: garbage on the socket must not take the server down ───

BOOST_AUTO_TEST_CASE(test_malformed_datagrams_keep_the_server_alive,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    test_server server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
    server.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        return kythira::request_vote_response<>{request.term(), true};
    });
    server.start();
    const auto port = server.bound_port();

    // A spread of things a CoAP parser should reject rather than trust:
    // empty, truncated header, a version-0 header, an absurd token length, and
    // random bytes.
    send_raw(port, {});
    send_raw(port, {0x40});
    send_raw(port, {0x40, 0x01});
    send_raw(port, {0x00, 0x01, 0x00, 0x01});
    send_raw(port, {0x4F, 0x01, 0x00, 0x01, 0xFF, 0xFF});
    send_raw(port, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    std::vector<std::uint8_t> noise(600);
    for (std::size_t i = 0; i < noise.size(); ++i) {
        noise[i] = static_cast<std::uint8_t>((i * 37) & 0xFF);
    }
    send_raw(port, noise);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    // Still serving.
    BOOST_TEST(server.is_running());
    test_client client{{{peer_node_id, endpoint_for(port)}}, fast_client_config(), test_metrics{}};
    const kythira::request_vote_request<> request{99, 1, 0, 0};
    const auto response =
        client.send_request_vote(peer_node_id, request, std::chrono::seconds{15}).get();
    BOOST_TEST(response.term() == 99U, "the server stopped answering after garbage input");

    server.stop();
}

// ── Requirement 7.3: shutdown resolves in-flight futures ───────────────────

BOOST_AUTO_TEST_CASE(test_destroying_the_client_rejects_in_flight_requests,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const auto dead_port = reserve_dead_port();

    auto future = [&] {
        auto config = fast_client_config();
        // Long schedule, so the request is still in flight when the client dies.
        config.ack_timeout = std::chrono::milliseconds{5000};
        config.max_retransmit = 8;
        test_client client{{{peer_node_id, endpoint_for(dead_port)}}, config, test_metrics{}};
        const kythira::request_vote_request<> request{1, 1, 0, 0};
        auto pending = client.send_request_vote(peer_node_id, request, std::chrono::seconds{120});
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        return pending;
    }();

    bool rejected = false;
    try {
        (void)std::move(future).get();
    } catch (const kythira::coap_transport_error&) {
        rejected = true;
    }
    BOOST_TEST(rejected, "destroying the client must reject in-flight futures");
}

BOOST_AUTO_TEST_CASE(test_unknown_target_rejects_the_future,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    test_client client{{}, fast_client_config(), test_metrics{}};
    const kythira::request_vote_request<> request{1, 1, 0, 0};
    bool rejected = false;
    try {
        (void)client.send_request_vote(404, request, std::chrono::seconds{5}).get();
    } catch (const kythira::coap_network_error&) {
        rejected = true;
    }
    BOOST_TEST(rejected);
}

BOOST_AUTO_TEST_CASE(test_unregistered_handler_is_answered_not_dropped,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    test_server server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>&) { return kythira::request_vote_response<>{}; });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, fast_client_config(), test_metrics{}};
    kythira::append_entries_request<> request{};
    request._term = 1;
    request._leader_id = 1;

    bool rejected = false;
    try {
        (void)client.send_append_entries(peer_node_id, request, std::chrono::seconds{15}).get();
    } catch (const kythira::coap_transport_error&) {
        rejected = true;
    }
    BOOST_TEST(rejected, "an unhandled RPC must be answered with an error, not dropped");

    server.stop();
}

BOOST_AUTO_TEST_CASE(test_server_restarts_cleanly_on_the_same_port,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    std::uint16_t port = 0;
    {
        test_server first{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
        first.start();
        port = first.bound_port();
        first.stop();
        BOOST_TEST(!first.is_running());
    }

    // Rebinding the same port proves the socket was really closed.
    test_server second{loopback, port, kythira::coap_server_config{}, test_metrics{}};
    BOOST_REQUIRE_NO_THROW(second.start());
    BOOST_TEST(second.bound_port() == port);
    second.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        return kythira::request_vote_response<>{request.term(), true};
    });

    test_client client{{{peer_node_id, endpoint_for(port)}}, fast_client_config(), test_metrics{}};
    const kythira::request_vote_request<> request{8, 1, 0, 0};
    const auto response =
        client.send_request_vote(peer_node_id, request, std::chrono::seconds{15}).get();
    BOOST_TEST(response.term() == 8U);
    second.stop();
}

// A name resolves to every address it has, not only the first, and repeats
// (AI_ALL can list one address twice) are dropped.
BOOST_AUTO_TEST_CASE(test_endpoint_resolves_every_address) {
    const auto as_v6 =
        [](const kythira::cantcoap_detail::peer_address& peer) -> const sockaddr_in6& {
        return reinterpret_cast<const sockaddr_in6&>(peer.storage);
    };

    const auto v6 = kythira::cantcoap_detail::resolve_endpoint("coap://[::1]:5683");
    BOOST_REQUIRE_EQUAL(v6.size(), 1U);
    BOOST_TEST(IN6_IS_ADDR_LOOPBACK(&as_v6(v6.front()).sin6_addr));
    BOOST_TEST(ntohs(as_v6(v6.front()).sin6_port) == 5683U);

    const auto v4 = kythira::cantcoap_detail::resolve_endpoint("127.0.0.1:9");
    BOOST_REQUIRE_EQUAL(v4.size(), 1U);
    BOOST_TEST(IN6_IS_ADDR_V4MAPPED(&as_v6(v4.front()).sin6_addr));

    const auto local = kythira::cantcoap_detail::resolve_endpoint("coap://localhost:5683");
    BOOST_TEST(!local.empty());
    for (std::size_t i = 0; i < local.size(); ++i) {
        for (std::size_t j = i + 1; j < local.size(); ++j) {
            BOOST_TEST(local[i].key() != local[j].key());
        }
    }
}

// A peer named "localhost" is reached whichever of its addresses answers.
BOOST_AUTO_TEST_CASE(test_localhost_endpoint_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    test_server server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
    server.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        return kythira::request_vote_response<>{request.term(), true};
    });
    server.start();

    test_client client{{{peer_node_id, "coap://localhost:" + std::to_string(server.bound_port())}},
                       fast_client_config(),
                       test_metrics{}};
    const kythira::request_vote_request<> request{11, 1, 0, 0};
    const auto response =
        client.send_request_vote(peer_node_id, request, std::chrono::seconds{15}).get();
    BOOST_TEST(response.term() == 11U);
    server.stop();
}

// ── Requirement 6: OSCORE, inherited from the transport-neutral RFC 8613 ───

BOOST_AUTO_TEST_CASE(test_rpc_over_oscore,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const auto security = [](bool is_client) {
        kythira::oscore_credentials creds;
        creds.master_secret = std::vector<std::byte>(16, std::byte{0x2a});
        creds.master_salt = std::vector<std::byte>(8, std::byte{0x77});
        creds.sender_id = is_client ? std::vector<std::byte>{std::byte{0x00}}
                                    : std::vector<std::byte>{std::byte{0x01}};
        creds.recipient_id = is_client ? std::vector<std::byte>{std::byte{0x01}}
                                       : std::vector<std::byte>{std::byte{0x00}};
        kythira::coap_security_config config;
        config.mode = kythira::coap_auth_mode::oscore;
        config.credentials = creds;
        return config;
    };

    kythira::coap_server_config server_config;
    server_config.security = security(false);
    test_server server{loopback, ephemeral_port, server_config, test_metrics{}};
    server.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        return kythira::request_vote_response<>{request.term(), request.candidate_id() == 3};
    });
    server.start();

    auto client_config = fast_client_config();
    client_config.security = security(true);
    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config, test_metrics{}};

    const kythira::request_vote_request<> request{14, 3, 2, 13};
    const auto response =
        client.send_request_vote(peer_node_id, request, std::chrono::seconds{20}).get();
    BOOST_TEST(response.term() == 14U);
    BOOST_TEST(response.vote_granted());

    server.stop();
}

// A negative control for the OSCORE test above: if the transport were somehow
// falling back to plaintext, a client with the *wrong* Master Secret would
// still be served. It must not be.
BOOST_AUTO_TEST_CASE(test_oscore_wrong_key_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const auto security = [](bool is_client, std::byte secret) {
        kythira::oscore_credentials creds;
        creds.master_secret = std::vector<std::byte>(16, secret);
        creds.master_salt = std::vector<std::byte>(8, std::byte{0x77});
        creds.sender_id = is_client ? std::vector<std::byte>{std::byte{0x00}}
                                    : std::vector<std::byte>{std::byte{0x01}};
        creds.recipient_id = is_client ? std::vector<std::byte>{std::byte{0x01}}
                                       : std::vector<std::byte>{std::byte{0x00}};
        kythira::coap_security_config config;
        config.mode = kythira::coap_auth_mode::oscore;
        config.credentials = creds;
        return config;
    };

    kythira::coap_server_config server_config;
    server_config.security = security(false, std::byte{0x2a});
    test_server server{loopback, ephemeral_port, server_config, test_metrics{}};
    bool handler_ran = false;
    server.register_request_vote_handler(
        [&handler_ran](const kythira::request_vote_request<>& request) {
            handler_ran = true;
            return kythira::request_vote_response<>{request.term(), true};
        });
    server.start();

    auto client_config = fast_client_config();
    client_config.security = security(true, std::byte{0x5b});  // different secret
    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config, test_metrics{}};

    const kythira::request_vote_request<> request{1, 1, 0, 0};
    bool rejected = false;
    try {
        (void)client.send_request_vote(peer_node_id, request, std::chrono::seconds{6}).get();
    } catch (const kythira::coap_transport_error&) {
        rejected = true;
    }
    BOOST_TEST(rejected, "a request under the wrong OSCORE key must not be served");
    BOOST_TEST(!handler_ran, "the RPC handler must never see an unverified request");

    server.stop();
}

// Likewise: an OSCORE server must not answer a plaintext client at all.
BOOST_AUTO_TEST_CASE(test_oscore_server_refuses_plaintext,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    kythira::oscore_credentials creds;
    creds.master_secret = std::vector<std::byte>(16, std::byte{0x2a});
    creds.master_salt = std::vector<std::byte>(8, std::byte{0x77});
    creds.sender_id = std::vector<std::byte>{std::byte{0x01}};
    creds.recipient_id = std::vector<std::byte>{std::byte{0x00}};
    kythira::coap_server_config server_config;
    server_config.security.mode = kythira::coap_auth_mode::oscore;
    server_config.security.credentials = creds;

    test_server server{loopback, ephemeral_port, server_config, test_metrics{}};
    bool handler_ran = false;
    server.register_request_vote_handler(
        [&handler_ran](const kythira::request_vote_request<>& request) {
            handler_ran = true;
            return kythira::request_vote_response<>{request.term(), true};
        });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, fast_client_config(), test_metrics{}};
    const kythira::request_vote_request<> request{1, 1, 0, 0};
    bool rejected = false;
    try {
        (void)client.send_request_vote(peer_node_id, request, std::chrono::seconds{6}).get();
    } catch (const kythira::coap_transport_error&) {
        rejected = true;
    }
    BOOST_TEST(rejected);
    BOOST_TEST(!handler_ran);

    server.stop();
}

#ifdef LAKERS_AVAILABLE

// ── Requirement 6.2: the EDHOC bootstrap over /.well-known/edhoc ───────────

// Neither side is given a Master Secret. The OSCORE context is derived by a
// real EDHOC handshake carried over this backend's own CoAP, and only then can
// an RPC succeed -- so a passing round trip proves the whole chain:
// unprotected handshake on /.well-known/edhoc, mirrored contexts, then
// object-secured RPCs under keys that were never configured.
BOOST_AUTO_TEST_CASE(test_rpc_after_an_edhoc_bootstrap,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    kythira::coap_server_config server_config;
    server_config.security = edhoc_security(false);
    test_server server{loopback, ephemeral_port, server_config, test_metrics{}};
    server.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        return kythira::request_vote_response<>{request.term(), request.candidate_id() == 7};
    });
    server.start();

    auto client_config = fast_client_config();
    client_config.security = edhoc_security(true);
    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config, test_metrics{}};

    const kythira::request_vote_request<> request{42, 7, 1, 41};
    const auto response =
        client.send_request_vote(peer_node_id, request, std::chrono::seconds{60}).get();
    BOOST_TEST(response.term() == 42U);
    BOOST_TEST(response.vote_granted());

    server.stop();
}

// The handshake runs once; later RPCs reuse the derived context. If they ran
// it again, each would derive a fresh context and the OSCORE sequence numbers
// would restart -- and a large request also has to cross under the derived
// keys, block by block.
BOOST_AUTO_TEST_CASE(test_edhoc_bootstrap_happens_once,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    kythira::coap_server_config server_config;
    server_config.security = edhoc_security(false);
    server_config.enable_block_transfer = true;
    server_config.max_block_size = 256;
    test_server server{loopback, ephemeral_port, server_config, test_metrics{}};
    server.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        return kythira::request_vote_response<>{request.candidate_id(), true};
    });
    // Atomic: written on the server's loop thread, read here.
    std::atomic<std::size_t> received{0};
    server.register_install_snapshot_handler(
        [&received](const kythira::install_snapshot_request<>& request) {
            received = request.data().size();
            return kythira::install_snapshot_response<>{request.last_included_index()};
        });
    server.start();

    auto client_config = fast_client_config();
    client_config.security = edhoc_security(true);
    client_config.enable_block_transfer = true;
    client_config.max_block_size = 256;
    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config, test_metrics{}};

    for (std::uint64_t i = 1; i <= 5; ++i) {
        const kythira::request_vote_request<> request{1, i, 0, 0};
        const auto response =
            client.send_request_vote(peer_node_id, request, std::chrono::seconds{60}).get();
        BOOST_TEST(response.term() == i, "RPC " << i << " after bootstrap came back wrong");
    }

    kythira::install_snapshot_request<> snapshot{};
    snapshot._term = 1;
    snapshot._leader_id = 1;
    snapshot._last_included_index = 77;
    snapshot._data = std::vector<std::byte>(4 * 1024, std::byte{0x5a});
    snapshot._done = true;
    const auto response =
        client.send_install_snapshot(peer_node_id, snapshot, std::chrono::seconds{60}).get();
    BOOST_TEST(response.term() == 77U);
    BOOST_TEST(received.load() == 4U * 1024U);

    server.stop();
}

// A peer presenting the wrong credential must fail the handshake, the failure
// must surface rather than hang, the handler must never run -- and the server
// must not be left wedged: a correctly configured client bootstraps after it.
BOOST_AUTO_TEST_CASE(test_mismatched_edhoc_credentials_fail_the_bootstrap,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    kythira::coap_server_config server_config;
    server_config.security = edhoc_security(false);
    test_server server{loopback, ephemeral_port, server_config, test_metrics{}};
    std::atomic<bool> handler_ran{false};
    server.register_request_vote_handler(
        [&handler_ran](const kythira::request_vote_request<>& request) {
            handler_ran = true;
            return kythira::request_vote_response<>{request.term(), true};
        });
    server.start();

    {
        // The client expects the *initiator's* own credential back from the
        // peer, which the server will not present.
        auto client_config = fast_client_config();
        auto security = edhoc_security(true);
        auto creds = std::get<kythira::oscore_credentials>(security.credentials);
        creds.edhoc.peer_credential = cred_i();
        security.credentials = creds;
        client_config.security = security;
        test_client client{
            {{peer_node_id, endpoint_for(server.bound_port())}}, client_config, test_metrics{}};

        const kythira::request_vote_request<> request{1, 1, 0, 0};
        bool rejected = false;
        try {
            (void)client.send_request_vote(peer_node_id, request, std::chrono::seconds{40}).get();
        } catch (const kythira::coap_security_error&) {
            rejected = true;
        } catch (const kythira::coap_transport_error&) {
            rejected = true;
        }
        BOOST_TEST(rejected, "a failed EDHOC handshake must surface, not hang");
        BOOST_TEST(!handler_ran.load());
    }

    handler_ran = false;
    auto client_config = fast_client_config();
    client_config.security = edhoc_security(true);
    test_client good{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config, test_metrics{}};
    const kythira::request_vote_request<> request{3, 1, 0, 0};
    const auto response =
        good.send_request_vote(peer_node_id, request, std::chrono::seconds{60}).get();
    BOOST_TEST(response.term() == 3U);
    BOOST_TEST(handler_ran.load());

    server.stop();
}

// Before any handshake, an EDHOC server serves /.well-known/edhoc and nothing
// else: a plaintext RPC is refused with 4.01 and never reaches the handler.
BOOST_AUTO_TEST_CASE(test_edhoc_server_refuses_plaintext_before_bootstrap,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    kythira::coap_server_config server_config;
    server_config.security = edhoc_security(false);
    test_server server{loopback, ephemeral_port, server_config, test_metrics{}};
    std::atomic<bool> handler_ran{false};
    server.register_request_vote_handler(
        [&handler_ran](const kythira::request_vote_request<>& request) {
            handler_ran = true;
            return kythira::request_vote_response<>{request.term(), true};
        });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, fast_client_config(), test_metrics{}};
    const kythira::request_vote_request<> request{1, 1, 0, 0};
    std::optional<std::uint8_t> code;
    try {
        (void)client.send_request_vote(peer_node_id, request, std::chrono::seconds{10}).get();
    } catch (const kythira::coap_client_error& e) {
        code = e.response_code();
    }
    BOOST_REQUIRE(code.has_value());
    BOOST_TEST(*code == 0x81U, "expected 4.01 Unauthorized");
    BOOST_TEST(!handler_ran.load());

    server.stop();
}

// A restarted server has no context, so the client's next protected request is
// answered with an unprotected 4.01. That must fail promptly *and* drop the
// client's stale context, so the request after it bootstraps again instead of
// failing forever.
BOOST_AUTO_TEST_CASE(test_edhoc_client_bootstraps_again_after_a_server_restart,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(180))) {
    kythira::coap_server_config server_config;
    server_config.security = edhoc_security(false);
    std::uint16_t port = 0;
    auto client_config = fast_client_config();
    client_config.security = edhoc_security(true);
    std::unique_ptr<test_client> client;
    const auto handler = [](const kythira::request_vote_request<>& request) {
        return kythira::request_vote_response<>{request.term(), true};
    };

    {
        test_server first{loopback, ephemeral_port, server_config, test_metrics{}};
        first.register_request_vote_handler(handler);
        first.start();
        port = first.bound_port();
        client = std::make_unique<test_client>(
            std::unordered_map<std::uint64_t, std::string>{{peer_node_id, endpoint_for(port)}},
            client_config, test_metrics{});
        const kythira::request_vote_request<> request{1, 1, 0, 0};
        BOOST_TEST(client->send_request_vote(peer_node_id, request, std::chrono::seconds{60})
                       .get()
                       .term() == 1U);
        first.stop();
    }

    test_server second{loopback, port, server_config, test_metrics{}};
    second.register_request_vote_handler(handler);
    second.start();

    const kythira::request_vote_request<> stale{2, 1, 0, 0};
    const auto started = std::chrono::steady_clock::now();
    BOOST_CHECK_THROW(
        (void)client->send_request_vote(peer_node_id, stale, std::chrono::seconds{30}).get(),
        kythira::coap_client_error);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    BOOST_TEST((elapsed < std::chrono::seconds{10}),
               "the stale context must be reported by the 4.01, not by a timeout");

    const kythira::request_vote_request<> fresh{3, 1, 0, 0};
    BOOST_TEST(
        client->send_request_vote(peer_node_id, fresh, std::chrono::seconds{60}).get().term() ==
        3U);

    client.reset();
    second.stop();
}

#endif  // LAKERS_AVAILABLE

#else  // CANTCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(test_cantcoap_backend_unavailable_is_skipped) {
    BOOST_TEST_MESSAGE(
        "cantcoap not available — the cantcoap integration tests are skipped. Rebuild with the "
        "vcpkg 'coap-cantcoap' feature to run them.");
    BOOST_TEST(!test_client::backend_available());
}

#endif  // CANTCOAP_AVAILABLE

// ── Requirement 6: what this backend refuses, and why ──────────────────────
// Compiled either way: a property of plan_security(), not of cantcoap.

BOOST_AUTO_TEST_CASE(test_plain_coap_is_accepted,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    BOOST_REQUIRE_NO_THROW(
        (test_server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}}));
}

BOOST_AUTO_TEST_CASE(test_dtls_is_planned_not_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    // DTLS used to be refused here; it is now provided over this backend's own
    // socket (coap_cantcoap_dtls.hpp, and coap_cantcoap_dtls_test.cpp for the
    // handshakes themselves). Planning it must select the DTLS channel, and
    // a server must construct, since key material is loaded only in start().
    kythira::coap_server_config config;
    config.security.mode = kythira::coap_auth_mode::dtls_psk;
    config.security.credentials =
        kythira::psk_credentials{"identity", std::vector<std::byte>{std::byte{0x01}}};
    const auto [selected, effective] = kythira::cantcoap_detail::plan_security(config, "server");
    BOOST_TEST((selected == kythira::cantcoap_detail::channel::dtls));
    BOOST_TEST((effective.mode == kythira::coap_auth_mode::dtls_psk));
    BOOST_REQUIRE_NO_THROW((test_server{loopback, ephemeral_port, config, test_metrics{}}));
}

BOOST_AUTO_TEST_CASE(test_legacy_dtls_fields_select_dtls_pki,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    // translate_legacy_fields() is shared with every other backend, so a
    // populated cert_file still means dtls_pki here.
    kythira::coap_server_config config;
    config.cert_file = "/nonexistent/server.pem";
    config.key_file = "/nonexistent/server.key";
    const auto [selected, effective] = kythira::cantcoap_detail::plan_security(config, "server");
    BOOST_TEST((selected == kythira::cantcoap_detail::channel::dtls));
    BOOST_TEST((effective.mode == kythira::coap_auth_mode::dtls_pki));
}

#if !defined(LAKERS_AVAILABLE)
// Without lakers the bootstrap cannot run, and asking for it must say so
// rather than quietly behaving as though static credentials were supplied.
BOOST_AUTO_TEST_CASE(test_edhoc_is_refused_without_lakers,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::coap_client_config config;
    config.security = edhoc_security(true);
    BOOST_CHECK_THROW((test_client{{}, config, test_metrics{}}), kythira::coap_security_error);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
