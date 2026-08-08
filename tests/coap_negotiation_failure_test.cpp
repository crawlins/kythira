/// @file coap_negotiation_failure_test.cpp
/// @brief CoAP content-negotiation branches driven by a raw libcoap peer
///        (`.kiro/specs/transport-multi-serializer/`, Tasks 15 and 16, CoAP half).
///
/// The HTTP half of Tasks 15/16 needed a foreign client to reach 415 and 406,
/// because our own client never sends the headers that trigger them. CoAP is
/// the same situation with different spelling: `coap_client` always sets a
/// `Content-Format` its own registry produced and an `Accept` list drawn from
/// that same registry, so 4.15 and 4.06 are unreachable from it by
/// construction. This suite therefore drives a real `coap_server` with a raw
/// libcoap client, hand-building the PDUs.
///
/// **Options are written with `coap_encode_var_safe`, not by pointing
/// `coap_add_option` at a `uint16_t`.** That second form puts host-order bytes
/// on the wire — it is the bug Task 11 found and fixed at every call site, where
/// CBOR's 60 went out as 15360. A test that reintroduced it here would be
/// testing the server against a malformed peer and calling the result a CoAP
/// interop check.
///
/// Relatedly: CoAP strips leading zeros, so Content-Format 0 (`text/plain`) is a
/// legitimately **zero-length** option value. Nothing here asserts a non-zero
/// option length.

#define BOOST_TEST_MODULE coap_negotiation_failure_test
#include <boost/test/unit_test.hpp>

#include <raft/future_default.hpp>

#include <raft/cbor_serializer.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/coap_utils.hpp>
#include <raft/console_logger.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <arpa/inet.h>
#include <coap3/coap.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "test_timeout_scale.hpp"

namespace {

using data_type = std::vector<std::byte>;
using json_serializer = kythira::json_rpc_serializer<data_type>;
using cbor_serializer = kythira::cbor_rpc_serializer<data_type>;

constexpr const char* bind_address = "127.0.0.1";
constexpr std::chrono::milliseconds exchange_timeout{5000};

// Response codes as libcoap encodes them (class << 5 | detail).
constexpr int code_unsupported_content_format = 0x8F;  // 4.15
constexpr int code_not_acceptable = 0x86;              // 4.06
constexpr int code_content = 0x45;                     // 2.05

/// A `Types` bundle parameterised on the registry, mirroring the other CoAP
/// suites' shape. Spelled out rather than derived because the CoAP bundles are
/// not a template over one serializer the way `http_transport_types` is.
template<typename Default_Serializer, typename Registry> struct coap_test_types {
    using serializer_type = Default_Serializer;
    using serializer_registry_type = Registry;
    using rpc_serializer_type = Default_Serializer;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = folly::Executor;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;

    using future_type = kythira::future_default<kythira::request_vote_response<>>;
};

using json_only_types =
    coap_test_types<json_serializer, kythira::single_serializer_registry<json_serializer>>;
using json_first_types =
    coap_test_types<json_serializer,
                    kythira::multi_serializer_registry<json_serializer, cbor_serializer>>;

/// What the raw client observed coming back.
struct raw_response {
    bool received{false};
    int code{0};
    /// The response's `Content-Format`, if it carried one.
    std::optional<std::uint16_t> content_format;
    std::size_t payload_size{0};
};

auto make_loopback_addr(std::uint16_t port) -> coap_address_t {
    coap_address_t addr;
    coap_address_init(&addr);
    addr.addr.sin.sin_family = AF_INET;
    addr.addr.sin.sin_port = htons(port);
    inet_pton(AF_INET, bind_address, &addr.addr.sin.sin_addr);
    addr.size = sizeof(struct sockaddr_in);
    return addr;
}

/// @brief Encode @p value the way CoAP requires, then attach it as @p option.
///
/// `coap_encode_var_safe` is the whole point: it writes network-order bytes with
/// leading zeros stripped. Passing `sizeof(uint16_t)` and a pointer to the value
/// instead — the shape this transport used everywhere before Task 11 — puts host
/// -order bytes on the wire and is wrong on every little-endian machine.
auto add_uint_option(coap_pdu_t* pdu, std::uint16_t option, std::uint16_t value) -> void {
    std::uint8_t buf[4];
    const std::size_t len = coap_encode_var_safe(buf, sizeof(buf), value);
    BOOST_REQUIRE(coap_add_option(pdu, option, len, buf) != 0U ||
                  len == 0U);  // len 0 is legitimate (value 0)
}

/// A well-formed `request_vote_request` body in @p Serializer's format, so any
/// rejection is attributable to the options rather than to the payload.
template<typename Serializer> auto request_body() -> std::vector<std::uint8_t> {
    kythira::request_vote_request<> req{7, 42, 3, 6};
    const Serializer serializer;
    const auto bytes = serializer.serialize(req);
    std::vector<std::uint8_t> out;
    out.reserve(bytes.size());
    for (auto b : bytes) {
        out.push_back(static_cast<std::uint8_t>(b));
    }
    return out;
}

/// @brief POST a hand-built PDU to @p port and return what came back.
///
/// @param content_format  Content-Format to declare, or nullopt to omit the
///                        option entirely (the "peer predating negotiation" case).
/// @param accept          Accept options to attach, in order. CoAP repeats the
///                        option rather than comma-joining one header, which is
///                        why this is a list and not a string.
auto post_and_await(std::uint16_t port, std::optional<std::uint16_t> content_format,
                    const std::vector<std::uint16_t>& accept,
                    const std::vector<std::uint8_t>& payload) -> raw_response {
    raw_response observed;

    coap_context_t* ctx = coap_new_context(nullptr);
    BOOST_REQUIRE(ctx != nullptr);

    coap_register_response_handler(
        ctx,
        [](coap_session_t* session, const coap_pdu_t*, const coap_pdu_t* received,
           coap_mid_t) -> coap_response_t {
            auto* out = static_cast<raw_response*>(coap_session_get_app_data(session));
            out->received = true;
            out->code = static_cast<int>(coap_pdu_get_code(received));

            coap_opt_iterator_t iter;
            if (coap_opt_t* fmt = coap_check_option(received, COAP_OPTION_CONTENT_FORMAT, &iter)) {
                out->content_format = static_cast<std::uint16_t>(
                    coap_decode_var_bytes(coap_opt_value(fmt), coap_opt_length(fmt)));
            }
            const std::uint8_t* data = nullptr;
            std::size_t len = 0;
            coap_get_data(received, &len, &data);
            out->payload_size = len;
            return COAP_RESPONSE_OK;
        });

    auto server_addr = make_loopback_addr(port);
    coap_session_t* session = coap_new_client_session(ctx, nullptr, &server_addr, COAP_PROTO_UDP);
    BOOST_REQUIRE(session != nullptr);
    coap_session_set_app_data(session, &observed);

    coap_pdu_t* pdu =
        coap_pdu_init(COAP_MESSAGE_CON, COAP_REQUEST_CODE_POST, coap_new_message_id(session),
                      coap_session_max_pdu_size(session));
    BOOST_REQUIRE(pdu != nullptr);

    // "/raft/request_vote" is two URI-Path options, not one containing a slash:
    // a single option would be percent-encoded to "raft%2Frequest_vote" and
    // would not match the registered resource.
    coap_add_option(pdu, COAP_OPTION_URI_PATH, 4, reinterpret_cast<const std::uint8_t*>("raft"));
    coap_add_option(pdu, COAP_OPTION_URI_PATH, 12,
                    reinterpret_cast<const std::uint8_t*>("request_vote"));

    // Option numbers must be added in ascending order; Content-Format (12)
    // precedes Accept (17), and both follow Uri-Path (11).
    if (content_format) {
        add_uint_option(pdu, COAP_OPTION_CONTENT_FORMAT, *content_format);
    }
    for (auto a : accept) {
        add_uint_option(pdu, COAP_OPTION_ACCEPT, a);
    }

    coap_add_data(pdu, payload.size(), payload.data());
    BOOST_REQUIRE(coap_send(session, pdu) != COAP_INVALID_MID);

    const auto deadline = std::chrono::steady_clock::now() + exchange_timeout;
    while (!observed.received && std::chrono::steady_clock::now() < deadline) {
        coap_io_process(ctx, 50);
    }

    coap_session_release(session);
    coap_free_context(ctx);
    return observed;
}

/// Starts a `coap_server` on an OS-assigned port and counts handler entries.
template<typename Types> struct server_fixture {
    kythira::noop_metrics metrics;
    kythira::coap_server<Types> server;
    std::atomic<int> handler_invocations{0};

    server_fixture()
        : server(
              bind_address, 0,
              [] {
                  kythira::coap_server_config c;
                  c.enable_dtls = false;
                  return c;
              }(),
              metrics) {
        server.register_request_vote_handler(
            [this](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
                handler_invocations.fetch_add(1);
                return kythira::request_vote_response<>{req.term(), true};
            });
        server.start();
    }

    ~server_fixture() { server.stop(); }

    server_fixture(const server_fixture&) = delete;
    auto operator=(const server_fixture&) -> server_fixture& = delete;

    [[nodiscard]] auto port() const -> std::uint16_t { return server.bound_port(); }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_negotiation_failure_tests)

/// @brief Requirement 4.5: an unsupported request `Content-Format` is answered
///        4.15 **before** the handler runs.
///
/// The load-bearing assertion is `handler_invocations == 0`, not the response
/// code. Answering 4.15 after running the handler would look identical from
/// outside on a handler without side effects, and be silently wrong on one with
/// them — which is the whole reason the ordering is specified.
///
/// Content-Format 41 (`application/xml`) is a real registered number that no
/// shipped serializer claims, so this exercises "recognised by CoAP, not by our
/// registry" rather than "malformed option".
BOOST_AUTO_TEST_CASE(an_unsupported_content_format_is_4_15_before_the_handler,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    server_fixture<json_only_types> fixture;

    const auto observed = post_and_await(
        fixture.port(),
        static_cast<std::uint16_t>(kythira::coap_utils::coap_content_format::application_xml), {},
        request_body<json_serializer>());

    BOOST_REQUIRE(observed.received);
    BOOST_TEST(observed.code == code_unsupported_content_format);
    BOOST_TEST(fixture.handler_invocations.load() == 0);
}

/// @brief The same rejection when the format is one *another* shipped serializer
///        uses but this server was not built with.
///
/// Distinct from the case above: 60 (`application/cbor`) is a format the
/// codebase knows and maps, so this proves the server rejects on *its own
/// registry's* contents rather than on whether the number is in the table at
/// all. A server that consulted the global table instead would accept this and
/// then fail to decode.
BOOST_AUTO_TEST_CASE(a_known_format_the_registry_lacks_is_also_4_15,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    server_fixture<json_only_types> fixture;

    const auto observed = post_and_await(
        fixture.port(),
        static_cast<std::uint16_t>(kythira::coap_utils::coap_content_format::application_cbor), {},
        request_body<cbor_serializer>());

    BOOST_REQUIRE(observed.received);
    BOOST_TEST(observed.code == code_unsupported_content_format);
    BOOST_TEST(fixture.handler_invocations.load() == 0);
}

/// @brief Requirement 4.6: an **absent** `Content-Format` falls back to the
///        registry default rather than being rejected.
///
/// A peer predating negotiation sends no Content-Format at all, and must keep
/// working. This is the CoAP twin of the HTTP suite's absent-`Content-Type` case.
BOOST_AUTO_TEST_CASE(an_absent_content_format_falls_back_to_the_default,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    server_fixture<json_only_types> fixture;

    const auto observed =
        post_and_await(fixture.port(), std::nullopt, {}, request_body<json_serializer>());

    BOOST_REQUIRE(observed.received);
    BOOST_TEST(observed.code == code_content);
    BOOST_TEST(fixture.handler_invocations.load() == 1);
    BOOST_REQUIRE(observed.content_format.has_value());
    BOOST_TEST(
        *observed.content_format ==
        static_cast<std::uint16_t>(kythira::coap_utils::coap_content_format::application_json));
}

/// @brief Task 15, CoAP: a multi-serializer server honours the client's `Accept`
///        rather than its own preference order.
///
/// The server prefers JSON (declaration order) and the client asks for CBOR
/// only. "The peer's order wins" means the answer comes back as CBOR. This is
/// the CoAP evidence for the same rule the HTTP suite pins, and it is the cell
/// that would fail if the CoAP server ignored `Accept` and always answered with
/// its default.
BOOST_AUTO_TEST_CASE(a_multi_serializer_server_answers_in_the_requested_accept,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    server_fixture<json_first_types> fixture;

    const auto observed = post_and_await(
        fixture.port(),
        static_cast<std::uint16_t>(kythira::coap_utils::coap_content_format::application_json),
        {static_cast<std::uint16_t>(kythira::coap_utils::coap_content_format::application_cbor)},
        request_body<json_serializer>());

    BOOST_REQUIRE(observed.received);
    BOOST_TEST(observed.code == code_content);
    BOOST_TEST(fixture.handler_invocations.load() == 1);
    BOOST_REQUIRE(observed.content_format.has_value());
    BOOST_TEST(
        *observed.content_format ==
        static_cast<std::uint16_t>(kythira::coap_utils::coap_content_format::application_cbor));
}

/// @brief Requirement 5.5: an unsatisfiable `Accept` is answered 4.06, with no
///        payload and before the handler runs.
///
/// The client declares it can read **only** CBOR, against a JSON-only server.
/// There is no overlap, so the server must refuse rather than answer in a format
/// the peer just said it cannot read.
///
/// **This case was written to the requirement and failed, which is how the
/// defect below was found.** Before the fix in this change the server replied
/// `2.05 Content` with a 61-byte JSON body and *ran the handler*. The 4.06
/// branch was unreachable: the `Accept` collection loop drops options it cannot
/// resolve through the registry, so an `Accept` naming only unsupported formats
/// collapsed to an empty list — and an empty list means "no preference stated",
/// which correctly yields the default. The two situations are opposites and had
/// become indistinguishable by the time the decision was made.
///
/// The consequence was worse than a wrong status code: a *success* carrying a
/// body the peer had explicitly said it could not decode. The client would then
/// report a deserialization failure, pointing at the payload rather than at the
/// negotiation that produced it.
BOOST_AUTO_TEST_CASE(an_unsatisfiable_accept_is_4_06_before_the_handler,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    server_fixture<json_only_types> fixture;

    const auto observed = post_and_await(
        fixture.port(),
        static_cast<std::uint16_t>(kythira::coap_utils::coap_content_format::application_json),
        {static_cast<std::uint16_t>(kythira::coap_utils::coap_content_format::application_cbor)},
        request_body<json_serializer>());

    BOOST_REQUIRE(observed.received);
    BOOST_TEST(observed.code == code_not_acceptable);
    BOOST_TEST(fixture.handler_invocations.load() == 0);
    BOOST_TEST(observed.payload_size == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
