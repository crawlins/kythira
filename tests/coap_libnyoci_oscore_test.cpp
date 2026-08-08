#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_libnyoci_oscore_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT (180 * KYTHIRA_TEST_TIMEOUT_SCALE)

#include <raft/json_serializer.hpp>
#include <raft/network.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/coap_transport_libnyoci_impl.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// The point of the whole exercise: OSCORE end to end over a CoAP backend that
// is not libcoap. `oscore_rfc8613_vectors_test.cpp` proves the transform is
// RFC-correct in isolation; this proves the transport actually carries it --
// that the protected message survives libnyoci's option writer, that the server
// routes on the *inner* path, and that nothing sensitive is left in the clear.

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

constexpr std::uint64_t peer_node_id = 5;
constexpr const char* loopback = "127.0.0.1";
constexpr std::uint16_t ephemeral_port = 0;

[[nodiscard]] auto endpoint_for(std::uint16_t port) -> std::string {
    return std::string{"coap://"} + loopback + ":" + std::to_string(port);
}

[[nodiscard]] auto bytes(std::initializer_list<int> values) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

/// A matched pair of OSCORE contexts. The two endpoints share a Master Secret
/// and swap Sender/Recipient IDs, exactly as RFC 8613 Appendix C.1 does.
[[nodiscard]] auto oscore_security(bool is_client) -> kythira::coap_security_config {
    kythira::oscore_credentials creds;
    creds.master_secret = bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
                                 0x0c, 0x0d, 0x0e, 0x0f, 0x10});
    creds.master_salt = bytes({0x9e, 0x7c, 0xa9, 0x22, 0x23, 0x78, 0x63, 0x40});
    creds.sender_id = is_client ? bytes({0x00}) : bytes({0x01});
    creds.recipient_id = is_client ? bytes({0x01}) : bytes({0x00});

    kythira::coap_security_config config;
    config.mode = kythira::coap_auth_mode::oscore;
    config.credentials = creds;
    return config;
}

[[nodiscard]] auto client_config() -> kythira::coap_client_config {
    kythira::coap_client_config config;
    config.security = oscore_security(true);
    return config;
}

[[nodiscard]] auto server_config() -> kythira::coap_server_config {
    kythira::coap_server_config config;
    config.security = oscore_security(false);
    return config;
}
}

BOOST_AUTO_TEST_SUITE(coap_libnyoci_oscore_tests)

#ifdef LIBNYOCI_AVAILABLE

BOOST_AUTO_TEST_CASE(test_request_vote_over_oscore,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    test_server server{loopback, ephemeral_port, server_config(), test_metrics{}};
    server.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        // Echoing fields back proves the *inner* request survived: the outer
        // message carries nothing but a ciphertext.
        return kythira::request_vote_response<>{request.term(), request.candidate_id() == 99};
    });
    server.start();
    BOOST_TEST(server.is_running());

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config(), test_metrics{}};

    const kythira::request_vote_request<> request{31, 99, 7, 30};
    const auto response =
        client.send_request_vote(peer_node_id, request, std::chrono::seconds{20}).get();
    BOOST_TEST(response.term() == 31U);
    BOOST_TEST(response.vote_granted());

    server.stop();
}

BOOST_AUTO_TEST_CASE(test_append_entries_over_oscore,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    test_server server{loopback, ephemeral_port, server_config(), test_metrics{}};
    server.register_append_entries_handler([](const kythira::append_entries_request<>& request) {
        kythira::append_entries_response<> response{};
        response._term = request.term();
        response._success = !request.entries().empty();
        response._conflict_index = request.prev_log_index() + request.entries().size();
        return response;
    });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config(), test_metrics{}};

    kythira::append_entries_request<> request{};
    request._term = 8;
    request._leader_id = 2;
    request._prev_log_index = 40;
    request._prev_log_term = 7;
    request._entries.push_back(
        kythira::log_entry<>{8, 41, bytes({0xde, 0xad, 0xbe, 0xef}), kythira::entry_type::normal});

    const auto response =
        client.send_append_entries(peer_node_id, request, std::chrono::seconds{20}).get();
    BOOST_TEST(response.term() == 8U);
    BOOST_TEST(response.success());
    BOOST_REQUIRE(response.conflict_index().has_value());
    BOOST_TEST(*response.conflict_index() == 41U);

    server.stop();
}

BOOST_AUTO_TEST_CASE(test_install_snapshot_over_oscore,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    test_server server{loopback, ephemeral_port, server_config(), test_metrics{}};
    server.register_install_snapshot_handler(
        [](const kythira::install_snapshot_request<>& request) {
            return kythira::install_snapshot_response<>{request.last_included_index()};
        });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config(), test_metrics{}};

    kythira::install_snapshot_request<> request{};
    request._term = 3;
    request._leader_id = 1;
    request._last_included_index = 512;
    request._last_included_term = 2;
    request._data = bytes({0x11, 0x22, 0x33});
    request._done = true;

    const auto response =
        client.send_install_snapshot(peer_node_id, request, std::chrono::seconds{20}).get();
    BOOST_TEST(response.term() == 512U);

    server.stop();
}

// A client with the wrong Master Secret derives different keys, so the server's
// AEAD tag check fails. It must be refused, not served.
BOOST_AUTO_TEST_CASE(test_wrong_master_secret_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    test_server server{loopback, ephemeral_port, server_config(), test_metrics{}};
    bool handler_ran = false;
    server.register_request_vote_handler(
        [&handler_ran](const kythira::request_vote_request<>& request) {
            handler_ran = true;
            return kythira::request_vote_response<>{request.term(), true};
        });
    server.start();

    auto config = client_config();
    auto creds = std::get<kythira::oscore_credentials>(config.security.credentials);
    creds.master_secret = bytes({0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88, 0x77, 0x66, 0x55,
                                 0x44, 0x33, 0x22, 0x11, 0x00});
    config.security.credentials = creds;

    test_client client{{{peer_node_id, endpoint_for(server.bound_port())}}, config, test_metrics{}};

    const kythira::request_vote_request<> request{1, 1, 0, 0};
    bool rejected = false;
    try {
        (void)client.send_request_vote(peer_node_id, request, std::chrono::seconds{8}).get();
    } catch (const kythira::coap_transport_error&) {
        rejected = true;
    }
    BOOST_TEST(rejected, "a request under the wrong key must be refused");
    BOOST_TEST(!handler_ran, "the RPC handler must never see an unverified request");

    server.stop();
}

// An OSCORE server must not answer an unprotected request, however well-formed.
BOOST_AUTO_TEST_CASE(test_unprotected_request_is_refused_by_an_oscore_server,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    test_server server{loopback, ephemeral_port, server_config(), test_metrics{}};
    bool handler_ran = false;
    server.register_request_vote_handler(
        [&handler_ran](const kythira::request_vote_request<>& request) {
            handler_ran = true;
            return kythira::request_vote_response<>{request.term(), true};
        });
    server.start();

    // A plain client: same endpoint, no security at all.
    test_client client{{{peer_node_id, endpoint_for(server.bound_port())}},
                       kythira::coap_client_config{},
                       test_metrics{}};

    const kythira::request_vote_request<> request{1, 1, 0, 0};
    bool rejected = false;
    try {
        (void)client.send_request_vote(peer_node_id, request, std::chrono::seconds{8}).get();
    } catch (const kythira::coap_transport_error&) {
        rejected = true;
    }
    BOOST_TEST(rejected, "an unprotected request must not be served by an OSCORE server");
    BOOST_TEST(!handler_ran);

    server.stop();
}

// Repeated RPCs must keep working: each burns a fresh Partial IV, and the
// server's replay window has to accept the ascending sequence.
BOOST_AUTO_TEST_CASE(test_sequential_requests_advance_the_partial_iv,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    test_server server{loopback, ephemeral_port, server_config(), test_metrics{}};
    server.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        return kythira::request_vote_response<>{request.candidate_id(), true};
    });
    server.start();

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config(), test_metrics{}};

    for (std::uint64_t i = 1; i <= 12; ++i) {
        const kythira::request_vote_request<> request{1, i, 0, 0};
        const auto response =
            client.send_request_vote(peer_node_id, request, std::chrono::seconds{20}).get();
        BOOST_TEST(response.term() == i, "request " << i << " came back mismatched");
    }

    server.stop();
}

#else  // LIBNYOCI_AVAILABLE

BOOST_AUTO_TEST_CASE(test_oscore_transport_tests_skipped_without_libnyoci) {
    BOOST_TEST_MESSAGE(
        "libnyoci not available — the libnyoci OSCORE transport tests are skipped. The OSCORE "
        "implementation itself is still covered by oscore_rfc8613_vectors_test.");
    BOOST_TEST(!test_client::backend_available());
}

#endif  // LIBNYOCI_AVAILABLE

// Compiled either way: a property of plan_security(), not of libnyoci.
BOOST_AUTO_TEST_CASE(test_edhoc_bootstrap_is_still_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    // EDHOC's handshake is transport-neutral, but running it needs a
    // .well-known/edhoc exchange this backend does not offer yet, so it must
    // be refused rather than silently treated as static provisioning.
    auto config = client_config();
    auto creds = std::get<kythira::oscore_credentials>(config.security.credentials);
    creds.bootstrap_method = kythira::oscore_bootstrap::edhoc;
    config.security.credentials = creds;

    BOOST_CHECK_THROW((test_client{{}, config, test_metrics{}}), kythira::coap_security_error);
}

BOOST_AUTO_TEST_SUITE_END()
