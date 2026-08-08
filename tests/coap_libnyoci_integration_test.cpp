#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_libnyoci_integration_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT (120 * KYTHIRA_TEST_TIMEOUT_SCALE)

#include <raft/json_serializer.hpp>
#include <raft/network.hpp>
#include <raft/serializer_registry.hpp>
// Deliberately NOT raft/coap_transport.hpp: <coap3/coap.h> and
// <libnyoci/libnyoci.h> cannot share a translation unit, so a test of the
// libnyoci backend can never also name the libcoap one. See the header comment
// in coap_transport_libnyoci_impl.hpp.
#include <raft/coap_transport_libnyoci_impl.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

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

using test_client = kythira::coap_libnyoci_client<test_types>;
using test_server = kythira::coap_libnyoci_server<test_types>;

constexpr std::uint64_t peer_node_id = 7;
constexpr const char* loopback = "127.0.0.1";

// Ephemeral bind (port 0) throughout, so these tests never collide with each
// other or with the libcoap suite's assigned port literals.
constexpr std::uint16_t ephemeral_port = 0;

[[nodiscard]] auto endpoint_for(std::uint16_t port) -> std::string {
    return std::string{"coap://"} + loopback + ":" + std::to_string(port);
}

[[nodiscard]] auto client_config() -> kythira::coap_client_config {
    kythira::coap_client_config config;
    config.use_confirmable_messages = true;
    return config;
}

/// Bind and immediately release a server, yielding a port nothing is listening
/// on. Better than guessing at a literal, which can collide with whatever else
/// the machine is running.
[[nodiscard]] auto reserve_dead_port() -> std::uint16_t {
    test_server probe{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
    probe.start();
    const auto port = probe.bound_port();
    probe.stop();
    return port;
}
}

BOOST_AUTO_TEST_SUITE(coap_libnyoci_integration_tests)

#ifdef LIBNYOCI_AVAILABLE

// ── Requirement 7.2: end-to-end round trip for all three RPCs ──────────────

BOOST_AUTO_TEST_CASE(test_request_vote_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    test_server server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
    server.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        // Granted only for the candidate the test actually asked about, and the
        // term echoed back, so a hard-coded response would not pass.
        return kythira::request_vote_response<>{request.term(), request.candidate_id() == 42};
    });
    server.start();
    BOOST_TEST(server.is_running());
    BOOST_TEST(server.bound_port() != 0);

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config(), test_metrics{}};

    const kythira::request_vote_request<> request{9, 42, 3, 8};
    const auto response =
        client.send_request_vote(peer_node_id, request, std::chrono::seconds{10}).get();
    BOOST_TEST(response.term() == 9U);
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
        // Carried back so the test can prove the *request body* round-tripped,
        // not merely that some response arrived.
        response._conflict_index = request.prev_log_index() + request.entries().size();
        return response;
    });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config(), test_metrics{}};

    kythira::append_entries_request<> request{};
    request._term = 4;
    request._leader_id = 1;
    request._prev_log_index = 10;
    request._prev_log_term = 3;
    request._leader_commit = 10;
    request._entries.push_back(
        kythira::log_entry<>{4, 11, std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}},
                             kythira::entry_type::normal});

    const auto response =
        client.send_append_entries(peer_node_id, request, std::chrono::seconds{10}).get();
    BOOST_TEST(response.term() == 4U);
    BOOST_TEST(response.success());
    BOOST_REQUIRE(response.conflict_index().has_value());
    BOOST_TEST(*response.conflict_index() == 11U);

    server.stop();
}

BOOST_AUTO_TEST_CASE(test_install_snapshot_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    test_server server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
    server.register_install_snapshot_handler(
        [](const kythira::install_snapshot_request<>& request) {
            // Echoing last_included_index rather than term proves the snapshot
            // metadata survived the trip.
            return kythira::install_snapshot_response<>{request.last_included_index()};
        });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config(), test_metrics{}};

    kythira::install_snapshot_request<> request{};
    request._term = 12;
    request._leader_id = 1;
    request._last_included_index = 100;
    request._last_included_term = 11;
    request._offset = 0;
    request._data = std::vector<std::byte>{std::byte{0xAB}, std::byte{0xCD}};
    request._done = true;

    const auto response =
        client.send_install_snapshot(peer_node_id, request, std::chrono::seconds{15}).get();
    BOOST_TEST(response.term() == 100U);

    server.stop();
}

// ── Requirement 7.3 / 4.5: block-wise transfer end to end ──────────────────

// libnyoci implements Block2 (block-wise *responses*) and nothing else, so
// this is where block-wise is genuinely exercisable: the server slices, and
// libnyoci's own transaction layer drives the follow-up requests whose blocks
// the adapter reassembles. Forcing a 16-byte block size makes an ordinary Raft
// response — a couple of hundred bytes of JSON — span a dozen blocks, a far
// sharper test of the reassembly than a single 1024-byte boundary would be. It
// also proves the exchange is stateless on the server: every block is served
// from the request's own Block2 option, with no per-peer transfer state.
BOOST_AUTO_TEST_CASE(test_block_wise_response_reassembly,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    kythira::coap_server_config config;
    config.enable_block_transfer = true;
    config.max_block_size = 16;  // the smallest block size RFC 7959 defines

    test_server server{loopback, ephemeral_port, config, test_metrics{}};
    server.register_append_entries_handler([](const kythira::append_entries_request<>& request) {
        kythira::append_entries_response<> response{};
        response._term = request.term();
        response._success = true;
        response._conflict_index = 1234567;
        response._conflict_term = 7654321;
        return response;
    });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config(), test_metrics{}};

    kythira::append_entries_request<> request{};
    request._term = 77;
    request._leader_id = 2;

    const auto response =
        client.send_append_entries(peer_node_id, request, std::chrono::seconds{30}).get();
    BOOST_TEST(response.term() == 77U);
    BOOST_TEST(response.success());
    BOOST_REQUIRE(response.conflict_index().has_value());
    BOOST_TEST(*response.conflict_index() == 1234567U);
    BOOST_REQUIRE(response.conflict_term().has_value());
    BOOST_TEST(*response.conflict_term() == 7654321U);
}

// The other half of the block-wise story, and the half that most needs saying
// out loud: libnyoci has no Block1 at all, so an over-large *request* cannot be
// split. The adapter must reject it with an error naming the reason, rather
// than truncating the payload or hanging until the timeout.
BOOST_AUTO_TEST_CASE(test_oversized_request_is_rejected_with_a_descriptive_error,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    test_server server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
    server.register_install_snapshot_handler([](const kythira::install_snapshot_request<>&) {
        return kythira::install_snapshot_response<>{1};
    });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config(), test_metrics{}};

    kythira::install_snapshot_request<> request{};
    request._term = 1;
    request._leader_id = 1;
    request._last_included_index = 1;
    request._last_included_term = 1;
    // Far past any packet budget libnyoci can be built with.
    request._data = std::vector<std::byte>(64 * 1024, std::byte{0x5A});
    request._done = true;

    bool rejected = false;
    std::string message;
    try {
        (void)client.send_install_snapshot(peer_node_id, request, std::chrono::seconds{20}).get();
    } catch (const kythira::coap_transport_error& e) {
        rejected = true;
        message = e.what();
    }
    BOOST_TEST(rejected, "an unsendable request must reject its future, not hang");
    BOOST_TEST(message.find("Block1") != std::string::npos,
               "the error must name the missing Block1 support, got: " << message);

    server.stop();
}

// ── Requirement 4.3: a peer that never answers rejects the future ──────────

BOOST_AUTO_TEST_CASE(test_unreachable_peer_rejects_with_timeout,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const auto dead_port = reserve_dead_port();
    BOOST_TEST(dead_port != 0);

    test_client client{{{peer_node_id, endpoint_for(dead_port)}}, client_config(), test_metrics{}};

    const kythira::request_vote_request<> request{1, 1, 0, 0};

    bool rejected = false;
    try {
        (void)client.send_request_vote(peer_node_id, request, std::chrono::seconds{3}).get();
    } catch (const kythira::coap_transport_error&) {
        rejected = true;
    }
    BOOST_TEST(rejected, "a request to a dead peer must reject rather than hang forever");
}

// A target with no configured endpoint is a caller error, and it has to reject
// the future rather than throw out of send_* — the Raft layer treats these
// calls as infallible and only ever inspects the future.
BOOST_AUTO_TEST_CASE(test_unknown_target_rejects_the_future,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    test_client client{{}, client_config(), test_metrics{}};
    const kythira::request_vote_request<> request{1, 1, 0, 0};

    bool rejected = false;
    try {
        (void)client.send_request_vote(999, request, std::chrono::seconds{5}).get();
    } catch (const kythira::coap_network_error&) {
        rejected = true;
    }
    BOOST_TEST(rejected);
}

// ── Requirement 4.2: one response resolves exactly one future ──────────────

BOOST_AUTO_TEST_CASE(test_concurrent_requests_are_correlated_independently,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    test_server server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
    // Echo the candidate id back in the term field, so a mis-correlated
    // response is detectable rather than merely plausible.
    server.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        return kythira::request_vote_response<>{request.candidate_id(), true};
    });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config(), test_metrics{}};

    constexpr int request_count = 16;
    std::vector<kythira::future_default<kythira::request_vote_response<>>> futures;
    futures.reserve(request_count);
    for (int i = 0; i < request_count; ++i) {
        const kythira::request_vote_request<> request{1, static_cast<std::uint64_t>(1000 + i), 0,
                                                      0};
        futures.push_back(
            client.send_request_vote(peer_node_id, request, std::chrono::seconds{20}));
    }
    for (int i = 0; i < request_count; ++i) {
        const auto response = std::move(futures[static_cast<std::size_t>(i)]).get();
        BOOST_TEST(response.term() == static_cast<std::uint64_t>(1000 + i),
                   "response " << i << " resolved the wrong future");
    }

    server.stop();
}

// ── Requirement 6.4: shutdown resolves in-flight futures ───────────────────

BOOST_AUTO_TEST_CASE(test_destroying_the_client_rejects_in_flight_requests,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    // Nothing is listening, so without cancellation the request would stay in
    // flight for its full two-minute timeout.
    const auto dead_port = reserve_dead_port();

    auto future = [&] {
        test_client client{
            {{peer_node_id, endpoint_for(dead_port)}}, client_config(), test_metrics{}};
        const kythira::request_vote_request<> request{1, 1, 0, 0};
        auto pending = client.send_request_vote(peer_node_id, request, std::chrono::seconds{120});
        // Let the event thread actually start the transaction, so this
        // exercises cancellation rather than the queue-drain path.
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        return pending;
    }();

    bool rejected = false;
    try {
        (void)std::move(future).get();
    } catch (const kythira::coap_transport_error&) {
        rejected = true;
    }
    BOOST_TEST(rejected, "destroying the client must reject in-flight futures, never abandon them");
}

// ── Requirement 6.2: start/stop is repeatable and releases the socket ──────

BOOST_AUTO_TEST_CASE(test_server_restarts_cleanly_on_the_same_port,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    std::uint16_t port = 0;
    {
        test_server first{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
        first.start();
        port = first.bound_port();
        BOOST_TEST(first.is_running());
        first.stop();
        BOOST_TEST(!first.is_running());
    }

    // Rebinding that very port is the observable proof the socket was closed
    // rather than leaked to a still-running thread.
    test_server second{loopback, port, kythira::coap_server_config{}, test_metrics{}};
    BOOST_REQUIRE_NO_THROW(second.start());
    BOOST_TEST(second.bound_port() == port);
    second.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        return kythira::request_vote_response<>{request.term(), true};
    });

    test_client client{{{peer_node_id, endpoint_for(port)}}, client_config(), test_metrics{}};
    const kythira::request_vote_request<> request{5, 1, 0, 0};
    const auto response =
        client.send_request_vote(peer_node_id, request, std::chrono::seconds{10}).get();
    BOOST_TEST(response.vote_granted());
    second.stop();
}

// An RPC with no registered handler must be answered, not silently dropped —
// dropping it would leave the peer retransmitting until its own timeout.
BOOST_AUTO_TEST_CASE(test_unregistered_handler_rejects_rather_than_hanging,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    test_server server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
    // Only RequestVote is registered; AppendEntries is not.
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>&) { return kythira::request_vote_response<>{}; });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config(), test_metrics{}};

    kythira::append_entries_request<> request{};
    request._term = 1;
    request._leader_id = 1;

    bool rejected = false;
    try {
        (void)client.send_append_entries(peer_node_id, request, std::chrono::seconds{10}).get();
    } catch (const kythira::coap_transport_error&) {
        rejected = true;
    }
    BOOST_TEST(rejected, "an unhandled RPC must be answered with an error, not dropped");

    server.stop();
}

#else  // LIBNYOCI_AVAILABLE

// Requirement 7.5: skipped, not failed, when libnyoci is absent.
BOOST_AUTO_TEST_CASE(test_libnyoci_backend_unavailable_is_skipped) {
    BOOST_TEST_MESSAGE(
        "libnyoci not available — the libnyoci CoAP integration tests are skipped. Rebuild with "
        "the vcpkg 'coap-libnyoci' feature to run them.");
    BOOST_TEST(!test_client::backend_available());
}

#endif  // LIBNYOCI_AVAILABLE

// ── Requirement 5: security ────────────────────────────────────────────────
// Plain CoAP here; the DTLS modes and the two refusals live in
// coap_libnyoci_dtls_test.cpp, which needs OpenSSL fixtures.

BOOST_AUTO_TEST_CASE(test_plain_coap_is_accepted,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    // The default config leaves security.mode at none and the legacy DTLS
    // fields empty, so construction must succeed (Requirement 5.3).
    BOOST_REQUIRE_NO_THROW(
        (test_server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}}));
}

BOOST_AUTO_TEST_CASE(test_legacy_dtls_fields_still_select_dtls,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    // Legacy field inference is shared verbatim with the libcoap backend
    // (translate_legacy_fields), so a populated cert_file still means dtls_pki
    // here — which this backend now provides through libnyoci's OpenSSL plugin
    // rather than refusing. Construction plans the channel and succeeds; the
    // material is only loaded by start(), and these paths do not exist, so that
    // is where it fails (Requirement 5.4).
    kythira::coap_server_config config;
    config.cert_file = "/nonexistent/server.pem";
    config.key_file = "/nonexistent/server.key";

    test_server server{loopback, ephemeral_port, config, test_metrics{}};
    BOOST_CHECK_THROW(server.start(), kythira::coap_security_error);
}

BOOST_AUTO_TEST_CASE(test_mixing_security_mode_and_legacy_fields_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    // Shared with the libcoap backend, and the reason translate_legacy_fields()
    // had to move into the neutral header: both backends must reach the same
    // verdict about the same config.
    kythira::coap_client_config config;
    config.security.mode = kythira::coap_auth_mode::dtls_psk;
    config.security.credentials =
        kythira::psk_credentials{"identity", std::vector<std::byte>{std::byte{0x01}}};
    config.cert_file = "/also/set.pem";

    BOOST_CHECK_THROW((test_client{{}, config, test_metrics{}}),
                      kythira::coap_security_config_error);
}

BOOST_AUTO_TEST_SUITE_END()
