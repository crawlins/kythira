/// @file beast_negotiation_retry_test.cpp
/// @brief The 415 media-type retry over `boost_beast_client`
///        (`.kiro/specs/transport-multi-serializer/`, Requirement 7.3).
///
/// The retry is implemented once per transport and must behave identically in
/// all four, so each needs its own runtime evidence — compiling is not the same
/// as working, and Beast's chain is the one where it is least obvious that it
/// does. Beast composes `.thenValue(...).thenError(teardown)` and the retry is
/// attached *outside* the teardown, so a retried attempt has to survive the
/// connection being removed and re-established underneath it. That ordering is
/// deliberate — it gives a retry the same fresh-connection treatment as a first
/// attempt — but it is exactly the kind of thing that reads fine and deadlocks,
/// so it is pinned here rather than argued for.
///
/// `handler_invocations == 1` is the load-bearing assertion in both cases: it
/// separates "recovered" from "never got through" and from "got through twice",
/// and only the first is correct.

#define BOOST_TEST_MODULE beast_negotiation_retry_test
#include <boost/test/unit_test.hpp>

#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/cbor_serializer.hpp>
#include <raft/executor_default.hpp>
#include <raft/http_exceptions.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#include "beast_test_thread_pool.hpp"
#include "recording_metrics.hpp"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

constexpr const char* bind_address = "127.0.0.1";
constexpr std::uint16_t json_server_port = 18295;
constexpr std::uint16_t cbor_server_port = 18296;
constexpr std::uint64_t peer_node_id = 1;
constexpr std::chrono::milliseconds rpc_timeout{4000};

/// A Beast bundle whose registry is a parameter, derived from the shipped one
/// so it differs in exactly that and nothing else — same future backend, same
/// executor. Spelling the members out by hand would pin a future backend and
/// make this suite's result depend on a choice unrelated to negotiation.
template<typename Default_Serializer, typename Registry>
struct beast_negotiating_types
    : kythira::future_default_http_transport_types<Default_Serializer, kythira::noop_metrics,
                                                   kythira::executor_default> {
    using serializer_registry_type = Registry;
};

using single_json =
    beast_negotiating_types<kythira::json_serializer,
                            kythira::single_serializer_registry<kythira::json_serializer>>;
using single_cbor =
    beast_negotiating_types<kythira::cbor_serializer,
                            kythira::single_serializer_registry<kythira::cbor_serializer>>;
using multi_cbor_first = beast_negotiating_types<
    kythira::cbor_serializer,
    kythira::multi_serializer_registry<kythira::cbor_serializer, kythira::json_serializer>>;

/// A serializer byte-identical to JSON that announces a different media type.
///
/// The `Accept-Post` case needs a **third** registered type before the peer's
/// choice can differ from the client's own next preference — with two, the
/// second is the only candidate and an informed jump is indistinguishable from
/// a blind walk. A third label is enough, since the retry never gets as far as
/// encoding in this one, and a third real codec would tie this suite to
/// whichever optional serializer happened to be installed.
struct alt_json_serializer : kythira::json_serializer {
    [[nodiscard]] auto media_type() const -> std::string { return "application/x-alt-json"; }
    [[nodiscard]] auto name() const -> std::string { return "alt-json"; }
};

/// The same bundle shape with a metrics backend that remembers what it was told.
/// The retry *count* is not visible from the RPC's return value — both policies
/// return the same response — so the `media_type_retry` emission is the only
/// place to observe it.
template<typename Default_Serializer, typename Registry>
struct beast_recording_types
    : kythira::future_default_http_transport_types<
          Default_Serializer, kythira::testing::recording_metrics, kythira::executor_default> {
    using serializer_registry_type = Registry;
};

/// Preference order `{cbor, alt-json, json}`: cbor is the first guess, alt-json
/// is what a blind walk would try next, json is what the peer will name.
using multi_three_recording = beast_recording_types<
    kythira::cbor_serializer,
    kythira::multi_serializer_registry<kythira::cbor_serializer, alt_json_serializer,
                                       kythira::json_serializer>>;

constexpr std::uint16_t accept_post_server_port = 18297;
constexpr std::uint16_t advertise_server_port = 18298;

}  // namespace

BOOST_AUTO_TEST_SUITE(beast_negotiation_retry_tests)

/// @brief Requirement 7.3 over Beast: a client whose preferred encoding the peer
///        refuses retries in one the peer speaks.
///
/// Client prefers CBOR and also speaks JSON; server is an unmodified JSON-only
/// node. Before the retry this pairing failed permanently with
/// `415: Unsupported Content-Type: application/cbor`.
BOOST_AUTO_TEST_CASE(a_multi_serializer_beast_client_retries_after_415) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    kythira::boost_beast_server<single_json> server(ioc, bind_address, json_server_port, {},
                                                    kythira::noop_metrics{});
    std::atomic<int> handler_invocations{0};
    server.register_request_vote_handler(
        [&](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            handler_invocations.fetch_add(1);
            kythira::request_vote_response<> resp{};
            resp._term = req.term() + 1;
            resp._vote_granted = true;
            return resp;
        });
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    std::unordered_map<std::uint64_t, std::string> node_map{
        {peer_node_id,
         std::string("http://") + bind_address + ":" + std::to_string(json_server_port)}};
    kythira::boost_beast_client<multi_cbor_first> client(ioc, node_map, {},
                                                         kythira::noop_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 5;
    req._candidate_id = 42;
    req._last_log_index = 10;
    req._last_log_term = 4;

    bool succeeded = false;
    try {
        auto resp = client.send_request_vote(peer_node_id, req, rpc_timeout).get();
        succeeded = resp.term() == 6 && resp.vote_granted();
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("retry did not recover: " << e.what());
    }

    BOOST_TEST(succeeded);
    // Once, on the retry -- not zero (never got through) and not twice (the
    // refused attempt must never have reached the handler, which is the
    // before-the-handler ordering that makes retrying safe at all).
    BOOST_TEST(handler_invocations.load() == 1);

    server.stop();
}

/// @brief Exhaustion: a client with nothing else to offer fails cleanly rather
///        than looping.
///
/// A single-serializer CBOR client against a JSON-only server has no second
/// format, so the retry must stop immediately. This is also every
/// single-serializer deployment shipping today, which is why it has to cost one
/// attempt rather than two — and why the failure must still be the typed 415 it
/// always was, not something the retry path invented.
BOOST_AUTO_TEST_CASE(a_beast_client_with_no_alternative_fails_cleanly) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    kythira::boost_beast_server<single_json> server(ioc, bind_address, cbor_server_port, {},
                                                    kythira::noop_metrics{});
    std::atomic<int> handler_invocations{0};
    server.register_request_vote_handler(
        [&](const kythira::request_vote_request<>&) -> kythira::request_vote_response<> {
            handler_invocations.fetch_add(1);
            return {};
        });
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    std::unordered_map<std::uint64_t, std::string> node_map{
        {peer_node_id,
         std::string("http://") + bind_address + ":" + std::to_string(cbor_server_port)}};
    kythira::boost_beast_client<single_cbor> client(ioc, node_map, {}, kythira::noop_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 5;

    int status = 0;
    try {
        client.send_request_vote(peer_node_id, req, rpc_timeout).get();
    } catch (const kythira::http_client_error& e) {
        status = e.status_code();
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("unexpected exception type: " << e.what());
    }

    BOOST_TEST(status == 415);
    BOOST_TEST(handler_invocations.load() == 0);

    server.stop();
}

/// @brief `Accept-Post` (W3C LDP 1.0 §7.1) on Beast's 415: the server names what
///        it *would* have taken.
///
/// Read off the wire with a raw `httplib::Client` rather than through
/// `boost_beast_client`, which is the only way to tell "the header went out"
/// from "our own two ends agree with each other". Beast is also where an emitted
/// header is easiest to get wrong: the response object is built in the session
/// from `dispatch`'s out-parameters, so a header added on the wrong side of that
/// split would compile and simply never appear.
BOOST_AUTO_TEST_CASE(a_beast_415_names_what_the_server_would_have_taken) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    kythira::boost_beast_server<single_json> server(ioc, bind_address, advertise_server_port, {},
                                                    kythira::noop_metrics{});
    std::atomic<int> handler_invocations{0};
    server.register_request_vote_handler(
        [&](const kythira::request_vote_request<>&) -> kythira::request_vote_response<> {
            handler_invocations.fetch_add(1);
            return {};
        });
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    httplib::Client raw(bind_address, advertise_server_port);
    raw.set_connection_timeout(5, 0);
    auto res = raw.Post("/v1/raft/request_vote", "{}", "application/x-not-a-thing");

    BOOST_REQUIRE_MESSAGE(res, "the request itself failed");
    BOOST_REQUIRE_EQUAL(res->status, 415);
    BOOST_CHECK(res->has_header(kythira::header_accept_post));
    BOOST_CHECK_EQUAL(res->get_header_value(kythira::header_accept_post), "application/json");
    // Advertising must not have moved the rejection past the handler, which is
    // what makes retrying safe in the first place.
    BOOST_TEST(handler_invocations.load() == 0);

    server.stop();
}

/// @brief The client half over Beast, and the case that can tell `Accept-Post`
///        from the blind walk at all.
///
/// Three serializers, `{cbor, alt-json, json}`, against a JSON-only server. cbor
/// is refused; the 415 names `application/json`; the retry goes straight there.
/// **One retry, naming json** is the assertion — the blind walk would record
/// two, the first of them `alt-json`, and would otherwise finish with the
/// identical response and the identical handler count.
///
/// Beast is the transport where this is least obviously correct: the retry lives
/// in a `thenError` continuation attached *outside* the connection teardown, so
/// the peer's header has to survive being carried on the exception across that
/// boundary rather than being read from a response object that no longer exists.
BOOST_AUTO_TEST_CASE(a_beast_client_retries_straight_to_the_type_the_peer_named) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    kythira::boost_beast_server<single_json> server(ioc, bind_address, accept_post_server_port, {},
                                                    kythira::noop_metrics{});
    std::atomic<int> handler_invocations{0};
    server.register_request_vote_handler(
        [&](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            handler_invocations.fetch_add(1);
            kythira::request_vote_response<> resp{};
            resp._term = req.term() + 1;
            resp._vote_granted = true;
            return resp;
        });
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    std::unordered_map<std::uint64_t, std::string> node_map{
        {peer_node_id,
         std::string("http://") + bind_address + ":" + std::to_string(accept_post_server_port)}};
    kythira::testing::recording_metrics client_metrics;
    kythira::boost_beast_client<multi_three_recording> client(ioc, node_map, {}, client_metrics);

    kythira::request_vote_request<> req{};
    req._term = 5;
    req._candidate_id = 42;

    bool succeeded = false;
    try {
        auto resp = client.send_request_vote(peer_node_id, req, rpc_timeout).get();
        succeeded = resp.term() == 6 && resp.vote_granted();
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("informed retry did not recover: " << e.what());
    }

    BOOST_TEST(succeeded);
    BOOST_TEST(handler_invocations.load() == 1);

    const auto retries =
        client_metrics.recorder()->media_types_of("beast_http.client.media_type_retry");
    BOOST_REQUIRE_EQUAL(retries.size(), 1U);
    BOOST_CHECK_EQUAL(retries.front(), "application/json");

    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
