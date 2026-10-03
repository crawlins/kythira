// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "test_timeout_scale.hpp"
// **Feature: coap-transport-security, Requirement 9.4**
// Client/server round trip under OSCORE over real libcoap sockets, checking
// that the payload comes back and that a client holding a different master
// secret is refused rather than answered.
//
// OSCORE on the libcoap backend is Kythira's own (raft/oscore.hpp): libcoap
// carries option 9 as an ordinary registered option and oscore_provider owns
// the Security Contexts (see its class comment). This test drives that
// contract directly, without coap_client<Types>/coap_server<Types>, so it
// checks the provider against libcoap alone: configure_session() makes
// libcoap pass a protected request through to the handler intact, and the
// provider's contexts protect and verify both directions.
// coap_oscore_id_context_test covers the same path through the transport.
//
// The linked libcoap must be built without its own OSCORE
// (vcpkg-overlays/libcoap). Against one built with it, the provider refuses,
// and that refusal is what this test checks instead.
#define BOOST_TEST_MODULE coap_oscore_integration_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT (30 * KYTHIRA_TEST_TIMEOUT_SCALE)

#ifdef LIBCOAP_AVAILABLE

#include <raft/coap_security_impl.hpp>

#include <coap3/coap.h>

#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace kythira;

namespace {

constexpr const char* kResourcePath = "echo";

auto options_of(const coap_pdu_t* pdu) -> std::vector<oscore::coap_option> {
    std::vector<oscore::coap_option> options;
    coap_opt_iterator_t iter;
    coap_option_iterator_init(pdu, &iter, COAP_OPT_ALL);
    while (coap_opt_t* option = coap_option_next(&iter)) {
        const auto* value = reinterpret_cast<const std::byte*>(coap_opt_value(option));
        options.push_back(oscore::coap_option{static_cast<std::uint16_t>(iter.number),
                                              {value, value + coap_opt_length(option)}});
    }
    return options;
}

/// The protected message as it arrived: code, token, OSCORE option, payload.
auto outer_of(const coap_pdu_t* pdu) -> oscore::coap_message {
    oscore::coap_message message;
    message.type = static_cast<std::uint8_t>(coap_pdu_get_type(pdu));
    message.code = static_cast<std::uint8_t>(coap_pdu_get_code(pdu));
    message.message_id = static_cast<std::uint16_t>(coap_pdu_get_mid(pdu));
    const auto token = coap_pdu_get_token(pdu);
    const auto* token_bytes = reinterpret_cast<const std::byte*>(token.s);
    message.token.assign(token_bytes, token_bytes + token.length);
    for (auto& option : options_of(pdu)) {
        if (oscore::is_outer_option(option.number)) {
            message.options.push_back(std::move(option));
        }
    }
    std::size_t length = 0;
    const std::uint8_t* data = nullptr;
    if (coap_get_data(pdu, &length, &data) != 0) {
        const auto* bytes = reinterpret_cast<const std::byte*>(data);
        message.payload.assign(bytes, bytes + length);
        message.has_payload = true;
    }
    return message;
}

auto write_into(coap_pdu_t* pdu, oscore::coap_message message) -> void {
    std::stable_sort(message.options.begin(), message.options.end(),
                     [](const auto& a, const auto& b) { return a.number < b.number; });
    for (const auto& option : message.options) {
        BOOST_REQUIRE(coap_add_option(pdu, option.number, option.value.size(),
                                      reinterpret_cast<const uint8_t*>(option.value.data())) != 0);
    }
    if (message.has_payload) {
        BOOST_REQUIRE(coap_add_data(pdu, message.payload.size(),
                                    reinterpret_cast<const uint8_t*>(message.payload.data())) != 0);
    }
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

/// A server whose only resource is the root, as the libcoap backend's is under
/// OSCORE: the real path is inside the ciphertext. It verifies, checks the
/// inner path, and echoes the inner payload back protected.
struct oscore_server {
    coap_context_t* ctx{nullptr};
    oscore_provider provider;
    std::atomic<bool> running{true};
    std::thread io_thread;

    explicit oscore_server(oscore_credentials creds)
        : ctx(coap_new_context(nullptr)), provider(std::move(creds), coap_security_role::server) {
        coap_startup();
        BOOST_REQUIRE(ctx != nullptr);
        coap_context_set_block_mode(ctx, COAP_BLOCK_USE_LIBCOAP | COAP_BLOCK_SINGLE_BODY);
        provider.configure_session(ctx);

        coap_resource_t* root = coap_resource_init(nullptr, 0);
        coap_resource_set_userdata(root, this);
        coap_register_request_handler(
            root, COAP_REQUEST_POST,
            [](coap_resource_t* resource, coap_session_t*, const coap_pdu_t* request,
               const coap_string_t*, coap_pdu_t* response) {
                auto* self = static_cast<oscore_server*>(coap_resource_get_userdata(resource));
                try {
                    const auto outer = outer_of(request);
                    auto [context, group] = self->provider.verifying_context(outer);
                    oscore::request_binding binding;
                    const auto inner = context->unprotect_request(outer, binding);

                    oscore::coap_message reply;
                    reply.type = static_cast<std::uint8_t>(COAP_MESSAGE_ACK);
                    reply.code = static_cast<std::uint8_t>(COAP_RESPONSE_CODE_CHANGED);
                    reply.token = inner.token;
                    std::string path;
                    for (const auto& option : inner.options) {
                        if (option.number == COAP_OPTION_URI_PATH) {
                            path.append(reinterpret_cast<const char*>(option.value.data()),
                                        option.value.size());
                        }
                    }
                    if (path != kResourcePath) {
                        reply.code = static_cast<std::uint8_t>(COAP_RESPONSE_CODE_NOT_FOUND);
                    } else {
                        reply.payload = inner.payload;
                        reply.has_payload = inner.has_payload;
                    }
                    const auto protected_reply = context->protect_response(reply, binding);
                    coap_pdu_set_code(response, static_cast<coap_pdu_code_t>(protected_reply.code));
                    write_into(response, protected_reply);
                } catch (const oscore::verification_error&) {
                    coap_pdu_set_code(response, COAP_RESPONSE_CODE_UNAUTHORIZED);
                }
            });
        coap_add_resource(ctx, root);

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
        if (io_thread.joinable()) {
            io_thread.join();
        }
        if (ctx != nullptr) {
            coap_free_context(ctx);
        }
    }
};

struct client_exchange_state {
    std::shared_ptr<oscore::security_context> context;
    oscore::request_binding binding;
    std::optional<coap_pdu_code_t> code;
    std::optional<std::string> echoed;
};

/// Sends one protected POST of `payload` to the echo path and waits for the
/// answer. Returns the echoed payload when a verified 2.04 came back.
auto send_and_await_echo(oscore_credentials client_creds, const std::string& payload,
                         std::chrono::milliseconds timeout) -> client_exchange_state {
    oscore_provider provider(std::move(client_creds), coap_security_role::client);
    coap_context_t* ctx = coap_new_context(nullptr);
    BOOST_REQUIRE(ctx != nullptr);
    provider.configure_session(ctx);

    client_exchange_state state;
    coap_register_response_handler(
        ctx,
        [](coap_session_t* session, const coap_pdu_t*, const coap_pdu_t* received,
           coap_mid_t) -> coap_response_t {
            auto* st = static_cast<client_exchange_state*>(coap_session_get_app_data(session));
            st->code = coap_pdu_get_code(received);
            coap_opt_iterator_t iter;
            if (coap_check_option(received, oscore::coap_option_oscore, &iter) == nullptr) {
                // An unprotected answer: the server could not verify us.
                return COAP_RESPONSE_OK;
            }
            const auto inner = st->context->unprotect_response(outer_of(received), st->binding);
            if (inner.code == COAP_RESPONSE_CODE_CHANGED) {
                st->echoed = std::string(reinterpret_cast<const char*>(inner.payload.data()),
                                         inner.payload.size());
            }
            return COAP_RESPONSE_OK;
        });

    auto server_addr = make_loopback_addr(kServerPort);
    coap_session_t* session =
        provider.create_client_session(ctx, nullptr, &server_addr, COAP_PROTO_UDP);
    BOOST_REQUIRE(session != nullptr);
    coap_session_set_app_data(session, &state);

    const std::vector<std::byte> token{std::byte{0x2A}, std::byte{0x17}};
    oscore::coap_message inner;
    inner.code = static_cast<std::uint8_t>(COAP_REQUEST_CODE_POST);
    inner.token = token;
    const auto* path = reinterpret_cast<const std::byte*>(kResourcePath);
    inner.options.push_back(
        oscore::coap_option{COAP_OPTION_URI_PATH, {path, path + std::strlen(kResourcePath)}});
    const auto* body = reinterpret_cast<const std::byte*>(payload.data());
    inner.payload.assign(body, body + payload.size());
    inner.has_payload = true;

    state.context = provider.request_context(std::nullopt);
    const auto outer = state.context->protect_request(inner, state.binding);

    coap_pdu_t* pdu =
        coap_pdu_init(COAP_MESSAGE_CON, static_cast<coap_pdu_code_t>(outer.code),
                      coap_new_message_id(session), coap_session_max_pdu_size(session));
    BOOST_REQUIRE(pdu != nullptr);
    coap_add_token(pdu, token.size(), reinterpret_cast<const uint8_t*>(token.data()));
    write_into(pdu, outer);
    coap_send(session, pdu);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!state.code.has_value() && std::chrono::steady_clock::now() < deadline) {
        coap_io_process(ctx, 50);
    }

    coap_session_release(session);
    coap_free_context(ctx);
    return state;
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

auto libcoap_has_its_own_oscore() -> bool {
    coap_startup();
    return coap_oscore_is_supported() != 0;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_oscore_integration_tests)

BOOST_AUTO_TEST_CASE(oscore_round_trip_echoes_payload,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(25))) {
    if (libcoap_has_its_own_oscore()) {
        BOOST_TEST_MESSAGE("linked libcoap has its own OSCORE; see the refusal case");
        return;
    }
    auto secret = std::vector<std::byte>(16, std::byte{0x77});
    oscore_server server(make_oscore_credentials({std::byte{0x01}}, {std::byte{0x00}}, secret));
    // Give the server's endpoint a moment to be ready for datagrams.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto client_creds = make_oscore_credentials({std::byte{0x00}}, {std::byte{0x01}}, secret);
    const auto state = send_and_await_echo(std::move(client_creds), "hello-oscore",
                                           std::chrono::milliseconds(5000));

    BOOST_REQUIRE(state.echoed.has_value());
    BOOST_CHECK_EQUAL(*state.echoed, "hello-oscore");
    // The outer code of a protected response is 2.04 whatever the inner one.
    BOOST_CHECK(state.code == COAP_RESPONSE_CODE_CHANGED);
}

BOOST_AUTO_TEST_CASE(mismatched_master_secret_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(25))) {
    if (libcoap_has_its_own_oscore()) {
        BOOST_TEST_MESSAGE("linked libcoap has its own OSCORE; see the refusal case");
        return;
    }
    auto server_secret = std::vector<std::byte>(16, std::byte{0x11});
    oscore_server server(
        make_oscore_credentials({std::byte{0x01}}, {std::byte{0x00}}, server_secret));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Wrong key: the server can't verify this, so it answers 4.01 unprotected
    // and nothing is echoed.
    auto wrong_secret = std::vector<std::byte>(16, std::byte{0x22});
    auto client_creds = make_oscore_credentials({std::byte{0x00}}, {std::byte{0x01}}, wrong_secret);
    const auto state = send_and_await_echo(std::move(client_creds), "hello-oscore",
                                           std::chrono::milliseconds(2000));

    BOOST_CHECK(!state.echoed.has_value());
    BOOST_CHECK(state.code == COAP_RESPONSE_CODE_UNAUTHORIZED);
}

// A libcoap with its own OSCORE drops every protected request before any
// handler sees it, so the provider must refuse up front rather than start a
// server that silently answers nothing.
BOOST_AUTO_TEST_CASE(a_libcoap_with_its_own_oscore_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(25))) {
    if (!libcoap_has_its_own_oscore()) {
        BOOST_TEST_MESSAGE(
            "linked libcoap is built without OSCORE, as required; nothing to refuse");
        return;
    }
    oscore_provider provider(make_oscore_credentials({std::byte{0x01}}, {std::byte{0x00}},
                                                     std::vector<std::byte>(16, std::byte{0x77})),
                             coap_security_role::server);
    coap_context_t* ctx = coap_new_context(nullptr);
    BOOST_CHECK_THROW(provider.configure_session(ctx), coap_unsupported_security_mode_error);
    coap_free_context(ctx);
}

// Each client session builds its own Security Context. They used to all start
// at Sender Sequence Number 0, so a second session under the same key reused
// the first one's nonces, and the server's replay window refused it. Now each
// context starts above every number the key has already used.
BOOST_AUTO_TEST_CASE(a_second_session_on_the_same_key_reuses_no_sequence_number,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(25))) {
    if (libcoap_has_its_own_oscore()) {
        BOOST_TEST_MESSAGE("linked libcoap has its own OSCORE, which the provider refuses");
        return;
    }
    auto secret = std::vector<std::byte>(16, std::byte{0x55});
    oscore_server server(make_oscore_credentials({std::byte{0x01}}, {std::byte{0x00}}, secret));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto client_creds = make_oscore_credentials({std::byte{0x00}}, {std::byte{0x01}}, secret);
    const auto first = send_and_await_echo(client_creds, "first", std::chrono::milliseconds(5000));
    BOOST_REQUIRE(first.echoed.has_value());
    BOOST_CHECK_EQUAL(*first.echoed, "first");
    const auto second =
        send_and_await_echo(client_creds, "second", std::chrono::milliseconds(5000));
    BOOST_REQUIRE(second.echoed.has_value());
    BOOST_CHECK_EQUAL(*second.echoed, "second");
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
