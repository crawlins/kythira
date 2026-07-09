// **Feature: coap-transport-security, Requirement 9.4**
// Client/server round-trip under plain OSCORE, verifying the payload
// round-trips correctly and that a session with a mismatched master secret
// (standing in for a tampered/forged ciphertext, since the wire-level
// tampering itself would need raw packet interception rather than the
// coap_context_t-level API this test operates at) is rejected rather than
// silently accepted.
//
// This drives oscore_provider directly against real libcoap contexts, not
// through coap_client<Types>/coap_server<Types> — LIBCOAP_AVAILABLE has
// never been defined anywhere in this project's default build (the
// coap-transport spec's own tests all run libcoap's stub code path), so
// turning it on for the templated transport for the first time here would
// pull in ~40 other #ifdef LIBCOAP_AVAILABLE branches across
// coap_transport_impl.hpp that have never been compiled before — a much
// larger, unrelated risk than this spec's actual scope (the new provider
// classes). Only built when the vcpkg "edhoc"-independent, always-available
// libcoap::coap-3 target is linked (see tests/CMakeLists.txt); the whole
// file is a no-op fallback otherwise.
#define BOOST_TEST_MODULE coap_oscore_integration_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 30

#ifdef LIBCOAP_AVAILABLE

#include <raft/coap_security_impl.hpp>

#include <coap3/coap.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace kythira;

namespace {

constexpr const char* kResourcePath = "echo";

// Registers a resource that copies the request payload verbatim into the
// 2.04 Changed response.
auto register_echo_resource(coap_context_t* ctx) -> void {
    coap_str_const_t* uri = coap_new_str_const(reinterpret_cast<const uint8_t*>(kResourcePath),
                                               std::strlen(kResourcePath));
    coap_resource_t* resource = coap_resource_init(uri, 0);
    coap_register_request_handler(resource, COAP_REQUEST_POST,
                                  [](coap_resource_t*, coap_session_t*, const coap_pdu_t* request,
                                     const coap_string_t*, coap_pdu_t* response) {
                                      const uint8_t* data = nullptr;
                                      std::size_t len = 0;
                                      coap_get_data(request, &len, &data);
                                      coap_pdu_set_code(response, COAP_RESPONSE_CODE_CHANGED);
                                      if (len > 0) coap_add_data(response, len, data);
                                  });
    coap_add_resource(ctx, resource);
}

auto make_loopback_addr(std::uint16_t port) -> coap_address_t {
    coap_address_t addr;
    coap_address_init(&addr);
    addr.addr.sin.sin_family = AF_INET;
    addr.addr.sin.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.addr.sin.sin_addr);
    addr.size = sizeof(struct sockaddr_in);
    return addr;
}

// Fixed, project-convention loopback ports (see coap_dtls_handshake_
// property_test.cpp's test_bind_port) rather than OS-assigned, to avoid
// needing to introspect the bound socket for its ephemeral port.
constexpr std::uint16_t kServerPort = 18720;

struct oscore_server {
    coap_context_t* ctx{nullptr};
    oscore_provider provider;
    std::atomic<bool> running{true};
    std::thread io_thread;

    explicit oscore_server(oscore_credentials creds)
        : provider(std::move(creds), coap_security_role::server) {
        coap_startup();
        ctx = coap_new_context(nullptr);
        BOOST_REQUIRE(ctx != nullptr);
        provider.configure_session(ctx);
        register_echo_resource(ctx);
        auto addr = make_loopback_addr(kServerPort);
        coap_endpoint_t* ep = coap_new_endpoint(ctx, &addr, COAP_PROTO_UDP);
        BOOST_REQUIRE(ep != nullptr);
        io_thread = std::thread([this] {
            while (running.load()) {
                coap_io_process(ctx, 50);
            }
        });
    }

    ~oscore_server() {
        running.store(false);
        if (io_thread.joinable()) io_thread.join();
        if (ctx) coap_free_context(ctx);
    }
};

// State shared with the response handler below: the resource path/payload
// needed to build a retry request, and where the final echoed payload ends
// up once one actually arrives.
struct client_exchange_state {
    std::string payload;
    std::optional<std::string> result;
};

auto build_request_pdu(coap_session_t* session, const std::string& payload,
                       const coap_opt_t* echo_opt) -> coap_pdu_t* {
    coap_pdu_t* pdu =
        coap_pdu_init(COAP_MESSAGE_CON, COAP_REQUEST_CODE_POST, coap_new_message_id(session),
                      coap_session_max_pdu_size(session));
    BOOST_REQUIRE(pdu != nullptr);
    coap_add_option(pdu, COAP_OPTION_URI_PATH, std::strlen(kResourcePath),
                    reinterpret_cast<const uint8_t*>(kResourcePath));
    if (echo_opt != nullptr) {
        coap_add_option(pdu, COAP_OPTION_ECHO, coap_opt_length(echo_opt), coap_opt_value(echo_opt));
    }
    coap_add_data(pdu, payload.size(), reinterpret_cast<const uint8_t*>(payload.data()));
    return pdu;
}

// Sends a single confirmable POST of `payload` to the server and blocks
// (via the client context's own I/O loop) until either the payload echoes
// back or `timeout` elapses. Transparently handles the RFC 9175/RFC 8613
// Appendix B.1.2 Echo challenge a freshly-started OSCORE server issues on
// its first contact with a given Recipient ID (4.01 Unauthorized + an Echo
// option): resends the same request with that Echo option copied in, the
// same way a real OSCORE-aware CoAP client library would.
auto send_and_await_echo(oscore_credentials client_creds, const std::string& payload,
                         std::chrono::milliseconds timeout) -> std::optional<std::string> {
    oscore_provider provider(std::move(client_creds), coap_security_role::client);
    coap_context_t* ctx = coap_new_context(nullptr);
    BOOST_REQUIRE(ctx != nullptr);

    client_exchange_state state;
    state.payload = payload;
    coap_register_response_handler(
        ctx,
        [](coap_session_t* session, const coap_pdu_t*, const coap_pdu_t* received,
           coap_mid_t) -> coap_response_t {
            auto* st = static_cast<client_exchange_state*>(coap_session_get_app_data(session));
            auto code = coap_pdu_get_code(received);
            if (code == COAP_RESPONSE_CODE_UNAUTHORIZED) {
                coap_opt_iterator_t opt_iter;
                coap_opt_t* echo_opt = coap_check_option(received, COAP_OPTION_ECHO, &opt_iter);
                if (echo_opt != nullptr) {
                    coap_send(session, build_request_pdu(session, st->payload, echo_opt));
                    return COAP_RESPONSE_OK;
                }
            }
            // Anything other than a real 2.04 Changed (e.g. an OSCORE
            // decryption-failure error response, or an Echo challenge with
            // no Echo option for some reason) is not a valid echo — leave
            // st->result unset so the caller's timeout loop reports failure
            // rather than treating an error body as a successful round trip.
            if (code == COAP_RESPONSE_CODE_CHANGED) {
                const uint8_t* data = nullptr;
                std::size_t len = 0;
                coap_get_data(received, &len, &data);
                st->result = std::string(reinterpret_cast<const char*>(data), len);
            }
            return COAP_RESPONSE_OK;
        });

    auto server_addr = make_loopback_addr(kServerPort);
    coap_session_t* session =
        provider.create_client_session(ctx, nullptr, &server_addr, COAP_PROTO_UDP);
    BOOST_REQUIRE(session != nullptr);
    coap_session_set_app_data(session, &state);

    coap_send(session, build_request_pdu(session, payload, nullptr));

    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!state.result.has_value() && std::chrono::steady_clock::now() < deadline) {
        coap_io_process(ctx, 50);
    }

    coap_session_release(session);
    coap_free_context(ctx);
    return state.result;
}

auto make_oscore_credentials(std::vector<std::byte> sender_id, std::vector<std::byte> recipient_id,
                             std::vector<std::byte> master_secret) -> oscore_credentials {
    oscore_credentials creds;
    creds.sender_id = std::move(sender_id);
    creds.recipient_id = std::move(recipient_id);
    creds.master_secret = std::move(master_secret);
    creds.master_salt =
        std::vector<std::byte>{std::byte{0x9e}, std::byte{0x7c}, std::byte{0xa9}, std::byte{0x22},
                               std::byte{0x23}, std::byte{0x78}, std::byte{0x63}, std::byte{0x40}};
    return creds;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_oscore_integration_tests)

BOOST_AUTO_TEST_CASE(oscore_round_trip_echoes_payload, *boost::unit_test::timeout(25)) {
    auto secret = std::vector<std::byte>(16, std::byte{0x77});
    oscore_server server(make_oscore_credentials({std::byte{0x01}}, {std::byte{0x00}}, secret));
    // Give the server's endpoint a moment to be ready for datagrams.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto client_creds = make_oscore_credentials({std::byte{0x00}}, {std::byte{0x01}}, secret);
    auto response = send_and_await_echo(std::move(client_creds), "hello-oscore",
                                        std::chrono::milliseconds(5000));

    BOOST_REQUIRE(response.has_value());
    BOOST_CHECK_EQUAL(*response, "hello-oscore");
}

BOOST_AUTO_TEST_CASE(mismatched_master_secret_is_rejected, *boost::unit_test::timeout(25)) {
    auto server_secret = std::vector<std::byte>(16, std::byte{0x11});
    oscore_server server(
        make_oscore_credentials({std::byte{0x01}}, {std::byte{0x00}}, server_secret));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Wrong key: the server can't decrypt/verify this, so no valid echo
    // response should ever arrive.
    auto wrong_secret = std::vector<std::byte>(16, std::byte{0x22});
    auto client_creds = make_oscore_credentials({std::byte{0x00}}, {std::byte{0x01}}, wrong_secret);
    auto response = send_and_await_echo(std::move(client_creds), "hello-oscore",
                                        std::chrono::milliseconds(2000));

    BOOST_CHECK(!response.has_value());
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(oscore_integration_test_requires_libcoap) {
    BOOST_TEST_MESSAGE(
        "coap_oscore_integration_test built without LIBCOAP_AVAILABLE; skipping "
        "real-libcoap OSCORE round-trip (see tests/CMakeLists.txt for how this target "
        "opts into linking libcoap::coap-3).");
}

#endif  // LIBCOAP_AVAILABLE
