// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file proxygen_negotiation_integration_test.cpp
/// @brief End-to-end content negotiation against a live `proxygen_server`
///        (`.kiro/specs/transport-multi-serializer/`, Task 10a.3-10a.4).
///
/// The Proxygen twin of `http_negotiation_integration_test.cpp`, and driven the
/// same way: with a raw `httplib::Client` rather than `proxygen_client`, because
/// the whole point is to send headers our own client would never send — an
/// unsupported `Content-Type`, an unsatisfiable `Accept`, a `charset` parameter.
/// A test that could only speak through our own client would be unable to reach
/// the branches this covers, and 415/406 would be dead code that nothing ever
/// exercised.
///
/// It doubles as the first test anywhere that pairs one HTTP implementation's
/// client with a *different* implementation's server. Every existing
/// "equivalence" test runs each transport against its own pair; this one puts
/// cpp-httplib on the wire in front of Proxygen, which is exactly the cell that
/// the hardcoded `"application/json"` response label survived in for as long as
/// it did.
///
/// The load-bearing assertion in the rejection cases is not the status code —
/// it is `handler_invocations == 0`. Returning 415 *after* running the handler
/// would still look correct from the outside on a handler with no side effects,
/// and would be silently wrong on one with them.

#define BOOST_TEST_MODULE proxygen_negotiation_integration_test
#include <boost/test/unit_test.hpp>

#include <raft/proxygen_http_transport.hpp>
#include <raft/proxygen_http_transport_impl.hpp>
#include <raft/cbor_serializer.hpp>
#include <raft/http_exceptions.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/executor_default.hpp>

#include <folly/executors/IOThreadPoolExecutor.h>
#include <httplib.h>

#include "recording_metrics.hpp"
#include "test_timeout_scale.hpp"

#include <atomic>
#include <unordered_map>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* bind_address = "127.0.0.1";
// Distinct from every other proxygen_*_test's port; these tests bind for real.
constexpr std::uint16_t bind_port = 18240;
constexpr const char* endpoint_request_vote = "/v1/raft/request_vote";

using data_type = std::vector<std::byte>;
using serializer_type = kythira::json_rpc_serializer<data_type>;
using test_types =
    kythira::future_default_proxygen_transport_types<serializer_type, kythira::noop_metrics,
                                                     kythira::executor_default>;

/// A bundle whose registry is a parameter, derived from the shipped one so it
/// differs in exactly that. Used only by the retry case at the bottom of this
/// file, which needs a *client* that prefers a format this JSON-only server
/// refuses.
template<typename Default_Serializer, typename Registry>
struct proxygen_negotiating_types
    : kythira::future_default_proxygen_transport_types<Default_Serializer, kythira::noop_metrics,
                                                       kythira::executor_default> {
    using serializer_registry_type = Registry;
};

using cbor_serializer_type = kythira::cbor_rpc_serializer<data_type>;
using multi_cbor_first_types = proxygen_negotiating_types<
    cbor_serializer_type,
    kythira::multi_serializer_registry<cbor_serializer_type, serializer_type>>;
using cbor_only_types =
    proxygen_negotiating_types<cbor_serializer_type,
                               kythira::single_serializer_registry<cbor_serializer_type>>;

/// A serializer byte-identical to JSON that announces a different media type.
///
/// The `Accept-Post` case needs a **third** registered type before the peer's
/// choice can differ from the client's own next preference — with two, the
/// second is the only candidate and an informed jump is indistinguishable from
/// a blind walk. A third label is enough, since the retry never gets as far as
/// encoding in this one, and a third real codec would tie this suite to whichever
/// optional serializer happened to be installed.
struct alt_json_serializer : serializer_type {
    [[nodiscard]] auto media_type() const -> std::string { return "application/x-alt-json"; }
    [[nodiscard]] auto name() const -> std::string { return "alt-json"; }
};

/// The same bundle shape, but with a metrics backend that remembers what it was
/// told. The retry count is not visible from the RPC's return value — both
/// policies return the same response — so the only place to observe it is the
/// `media_type_retry` emission.
template<typename Default_Serializer, typename Registry>
struct proxygen_recording_types
    : kythira::future_default_proxygen_transport_types<
          Default_Serializer, kythira::testing::recording_metrics, kythira::executor_default> {
    using serializer_registry_type = Registry;
};

/// Preference order `{cbor, alt-json, json}`: cbor is the first guess, alt-json
/// is what a blind walk would try next, json is what the peer will name.
using multi_three_recording_types = proxygen_recording_types<
    cbor_serializer_type,
    kythira::multi_serializer_registry<cbor_serializer_type, alt_json_serializer, serializer_type>>;

/// A well-formed request body in the server's own default media type, so any
/// rejection observed below is attributable to the *headers* rather than to the
/// payload. Built with the same serializer the server holds.
auto valid_request_body() -> std::string {
    kythira::request_vote_request<> request;
    request._term = 5;
    request._candidate_id = 42;
    request._last_log_index = 10;
    request._last_log_term = 4;

    const serializer_type serializer;
    const auto bytes = serializer.serialize(request);
    std::string body;
    body.reserve(bytes.size());
    for (auto b : bytes) {
        body.push_back(static_cast<char>(b));
    }
    return body;
}

/// Starts one server for the whole module and counts handler invocations.
struct negotiation_fixture {
    std::shared_ptr<folly::IOThreadPoolExecutorBase> io_executor{
        std::make_shared<folly::IOThreadPoolExecutor>(2)};
    kythira::proxygen_server<test_types> server;
    std::atomic<int> handler_invocations{0};

    negotiation_fixture()
        : server(bind_address, bind_port, {}, kythira::noop_metrics{}, io_executor) {
        server.register_request_vote_handler([this](const kythira::request_vote_request<>& req) {
            handler_invocations.fetch_add(1);
            kythira::request_vote_response<> resp;
            resp._term = req.term() + 1;
            resp._vote_granted = true;
            return resp;
        });
        server.start();
        // The server binds on its own thread; give it a moment before the first
        // client connects, matching the idiom in proxygen_transport_test.cpp.
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    ~negotiation_fixture() { server.stop(); }

    /// POSTs `body` with exactly `headers` — no defaults added.
    auto post(const httplib::Headers& headers, const std::string& body = valid_request_body())
        -> httplib::Result {
        httplib::Client client(bind_address, bind_port);
        client.set_connection_timeout(std::chrono::seconds{2});
        client.set_read_timeout(std::chrono::seconds{5});
        return client.Post(endpoint_request_vote, headers, body, "");
    }
};

}  // namespace

BOOST_FIXTURE_TEST_SUITE(proxygen_negotiation_integration, negotiation_fixture)

/// Baseline, and the regression test for the defect this task exists to fix:
/// the response `Content-Type` used to be the literal `"application/json"` for
/// every 200 regardless of which serializer produced the body. It now comes
/// from what `dispatch` actually encoded in. This asserts the value rather than
/// merely its presence — with a JSON serializer configured the two are the same
/// string, and only the *source* changed, which is precisely why nothing caught
/// this earlier.
BOOST_AUTO_TEST_CASE(a_supported_content_type_is_served_and_labelled,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    const auto before = handler_invocations.load();
    auto res = post({{"Content-Type", "application/json"}, {"Accept", "application/json"}});

    BOOST_REQUIRE(res);
    BOOST_CHECK_EQUAL(res->status, 200);
    BOOST_CHECK_EQUAL(res->get_header_value("Content-Type"), "application/json");
    BOOST_CHECK_EQUAL(handler_invocations.load(), before + 1);
}

/// Requirement 4.3/4.4: 415, and the handler must not have run.
BOOST_AUTO_TEST_CASE(an_unsupported_content_type_is_rejected_before_the_handler,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    const auto before = handler_invocations.load();
    auto res = post({{"Content-Type", "application/x-not-a-thing"}});

    BOOST_REQUIRE(res);
    BOOST_CHECK_EQUAL(res->status, 415);
    // The reason phrase, not just the code. Proxygen builds the status line from
    // its own `proxygen_status_reason` switch, whose `default` returns "Internal
    // Server Error" -- so a status it does not name goes out as
    // "415 Internal Server Error", a correct code contradicted by its own phrase.
    // Asserting only the code passes straight through that.
    BOOST_CHECK_EQUAL(res->reason, "Unsupported Media Type");
    BOOST_CHECK_EQUAL(handler_invocations.load(), before);
}

/// `Accept-Post` (W3C LDP 1.0 §7.1) on the 415: the server names what it *would*
/// have taken, so the peer's retry can converge in one round trip instead of
/// walking its own preference list.
///
/// Read off the wire with the raw client rather than through `proxygen_client`,
/// which is the only way to tell "the header went out" from "our own two ends
/// agree with each other". Proxygen is also the transport where an emitted
/// header is least obvious: the response is assembled by `ResponseBuilder`, and
/// this one is conditional on the status rather than part of the fluent chain.
BOOST_AUTO_TEST_CASE(a_415_names_what_the_server_would_have_taken,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    auto res = post({{"Content-Type", "application/x-not-a-thing"}});

    BOOST_REQUIRE(res);
    BOOST_REQUIRE_EQUAL(res->status, 415);
    BOOST_CHECK(res->has_header(kythira::header_accept_post));
    BOOST_CHECK_EQUAL(res->get_header_value(kythira::header_accept_post), "application/json");
}

/// And not on a status that says nothing about media types. A client reading it
/// there would be acting on an answer to a different question.
BOOST_AUTO_TEST_CASE(an_unrelated_error_carries_no_accept_post,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    httplib::Client client(bind_address, bind_port);
    client.set_connection_timeout(std::chrono::seconds{2});
    client.set_read_timeout(std::chrono::seconds{5});
    auto res = client.Post("/v1/raft/no_such_endpoint", valid_request_body(), "application/json");

    BOOST_REQUIRE(res);
    BOOST_REQUIRE_EQUAL(res->status, 404);
    BOOST_CHECK(!res->has_header(kythira::header_accept_post));
}

/// Requirement 5.3: 406, and again the handler must not have run. Ordering the
/// `Accept` check before the handler is what makes this true; checking after
/// would produce the same status while still having done the work.
BOOST_AUTO_TEST_CASE(an_unsatisfiable_accept_is_rejected_before_the_handler,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    const auto before = handler_invocations.load();
    auto res = post({{"Content-Type", "application/json"}, {"Accept", "application/x-nothing"}});

    BOOST_REQUIRE(res);
    BOOST_CHECK_EQUAL(res->status, 406);
    BOOST_CHECK_EQUAL(res->reason, "Not Acceptable");  // see the 415 case above
    BOOST_CHECK_EQUAL(handler_invocations.load(), before);
    // No body: anything we wrote would be unreadable to this peer by its own
    // account.
    BOOST_CHECK(res->body.empty());
}

/// Requirement 4.1/4.2: an *empty* `Content-Type` is treated as "the peer told
/// us nothing" and gets the default rather than a 415. Same mechanics as the
/// httplib suite's own version of this case — supplying the header explicitly
/// with an empty value is what suppresses httplib's `text/plain` injection.
BOOST_AUTO_TEST_CASE(an_empty_content_type_falls_back_to_the_default,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    const auto before = handler_invocations.load();
    auto res = post({{"Content-Type", ""}});

    BOOST_REQUIRE(res);
    BOOST_CHECK_EQUAL(res->status, 200);
    BOOST_CHECK_EQUAL(handler_invocations.load(), before + 1);
}

/// The same deliberate behaviour change `http_negotiation_integration_test`
/// pins for cpp-httplib, asserted here too so the two servers cannot drift
/// apart on it. A peer that POSTs without a `Content-Type` reaches this server
/// as `text/plain` and is rejected like any other unknown type.
BOOST_AUTO_TEST_CASE(httplibs_default_text_plain_is_rejected_like_any_other_unknown_type,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    const auto before = handler_invocations.load();
    auto res = post({});  // httplib fills in Content-Type: text/plain

    BOOST_REQUIRE(res);
    BOOST_CHECK_EQUAL(res->status, 415);
    BOOST_CHECK_EQUAL(res->body, "Unsupported Content-Type: text/plain");
    BOOST_CHECK_EQUAL(handler_invocations.load(), before);
}

/// The interop case `strip_media_type_parameters` exists for. A charset is
/// spec-legal and common; treating `application/json; charset=utf-8` as an
/// unknown media type would 415 an ordinary request.
BOOST_AUTO_TEST_CASE(a_charset_parameter_does_not_defeat_the_match,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    const auto before = handler_invocations.load();
    auto res = post({{"Content-Type", "application/json; charset=utf-8"}});

    BOOST_REQUIRE(res);
    BOOST_CHECK_EQUAL(res->status, 200);
    BOOST_CHECK_EQUAL(handler_invocations.load(), before + 1);
}

/// `Accept: */*` is curl's default and a great many client libraries'.
BOOST_AUTO_TEST_CASE(a_wildcard_accept_is_served_with_the_default,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    auto res = post({{"Content-Type", "application/json"}, {"Accept", "*/*"}});

    BOOST_REQUIRE(res);
    BOOST_CHECK_EQUAL(res->status, 200);
    BOOST_CHECK_EQUAL(res->get_header_value("Content-Type"), "application/json");
}

/// A `type/*` wildcard must resolve within that top-level type rather than
/// matching nothing.
BOOST_AUTO_TEST_CASE(a_subtype_wildcard_accept_is_served,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    auto res = post({{"Content-Type", "application/json"}, {"Accept", "application/*"}});

    BOOST_REQUIRE(res);
    BOOST_CHECK_EQUAL(res->status, 200);
    BOOST_CHECK_EQUAL(res->get_header_value("Content-Type"), "application/json");
}

/// An unranked list containing something we support: `q` is stripped, not
/// honoured, and the first entry we actually support wins.
BOOST_AUTO_TEST_CASE(an_accept_list_picks_the_first_supported_entry,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    auto res = post({{"Content-Type", "application/json"},
                     {"Accept", "application/x-nothing;q=0.9, application/json;q=0.1"}});

    BOOST_REQUIRE(res);
    BOOST_CHECK_EQUAL(res->status, 200);
    BOOST_CHECK_EQUAL(res->get_header_value("Content-Type"), "application/json");
}

/// A body that is genuinely malformed, in a media type we *do* support, must be
/// 400 rather than 415. The two are different problems with different fixes —
/// telling a peer to change its payload when it needs to change its format (or
/// vice versa) sends it in the wrong direction.
BOOST_AUTO_TEST_CASE(a_malformed_body_in_a_supported_type_is_400_not_415,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    auto res = post({{"Content-Type", "application/json"}}, "this is not json at all");

    BOOST_REQUIRE(res);
    BOOST_CHECK_EQUAL(res->status, 400);
}

/// @brief Requirement 7.3 over Proxygen: a client whose preferred encoding this
///        JSON-only server refuses retries in one it speaks.
///
/// Uses `proxygen_client` rather than the raw `httplib::Client` the rest of this
/// file needs, because the behaviour under test is the client's. Proxygen is the
/// transport where this matters most to verify at runtime: it has **two**
/// independent send paths chosen by an `if constexpr` on the future backend, and
/// the retry is attached once at the `send_rpc` dispatch point so both inherit
/// it. This case exercises whichever path this build selected; the other is
/// compile-checked by building the suite under the opposite backend.
BOOST_AUTO_TEST_CASE(a_multi_serializer_proxygen_client_retries_after_415) {
    folly::IOThreadPoolExecutor client_io(2);
    std::unordered_map<std::uint64_t, std::string> node_map{
        {1, std::string("http://") + bind_address + ":" + std::to_string(bind_port)}};
    kythira::proxygen_client<multi_cbor_first_types> client(client_io, node_map, {},
                                                            kythira::noop_metrics{});

    const auto before = handler_invocations.load();
    kythira::request_vote_request<> req{};
    req._term = 5;
    req._candidate_id = 42;

    bool succeeded = false;
    try {
        auto resp =
            std::move(client.send_request_vote(1, req, kythira::testing::scaled_deadline(5000)))
                .get();
        succeeded = resp.term() == 6 && resp.vote_granted();
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("retry did not recover: " << e.what());
    }

    BOOST_TEST(succeeded);
    // Exactly one handler entry: the CBOR attempt was refused before the
    // handler, and only the JSON retry reached it.
    BOOST_TEST(handler_invocations.load() == before + 1);
}

/// The client half of `Accept-Post`, and the case that can tell it from the
/// blind walk at all.
///
/// Three serializers, `{cbor, alt-json, json}`, against this file's JSON-only
/// server. cbor is refused; the 415 names `application/json`; the retry goes
/// straight there. **One retry, naming json** is the assertion — the blind walk
/// would record two, the first of them `alt-json`, and would otherwise finish
/// with the identical response and the identical handler count.
///
/// Two serializers could not distinguish the two policies: the second would be
/// the only candidate left and both would pick it.
BOOST_AUTO_TEST_CASE(a_proxygen_client_retries_straight_to_the_type_the_peer_named,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    folly::IOThreadPoolExecutor client_io(2);
    std::unordered_map<std::uint64_t, std::string> node_map{
        {1, std::string("http://") + bind_address + ":" + std::to_string(bind_port)}};
    kythira::testing::recording_metrics client_metrics;
    kythira::proxygen_client<multi_three_recording_types> client(client_io, node_map, {},
                                                                 client_metrics);

    const auto before = handler_invocations.load();
    kythira::request_vote_request<> req{};
    req._term = 5;
    req._candidate_id = 42;

    bool succeeded = false;
    try {
        auto resp =
            std::move(client.send_request_vote(1, req, kythira::testing::scaled_deadline(5000)))
                .get();
        succeeded = resp.term() == 6 && resp.vote_granted();
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("informed retry did not recover: " << e.what());
    }

    BOOST_TEST(succeeded);
    BOOST_TEST(handler_invocations.load() == before + 1);

    const auto retries =
        client_metrics.recorder()->media_types_of("proxygen_http.client.media_type_retry");
    BOOST_REQUIRE_EQUAL(retries.size(), 1U);
    BOOST_CHECK_EQUAL(retries.front(), "application/json");
}

/// @brief Exhaustion over Proxygen: nothing left to offer fails cleanly.
///
/// The path that first shipped a `BrokenPromise` in the other two transports —
/// throwing out of a Future-returning `thenError` rather than returning an
/// exceptional future. It is also every single-serializer deployment, which is
/// why each transport pins it rather than assuming the shared shape is enough.
BOOST_AUTO_TEST_CASE(a_proxygen_client_with_no_alternative_fails_cleanly) {
    folly::IOThreadPoolExecutor client_io(2);
    std::unordered_map<std::uint64_t, std::string> node_map{
        {1, std::string("http://") + bind_address + ":" + std::to_string(bind_port)}};
    kythira::proxygen_client<cbor_only_types> client(client_io, node_map, {},
                                                     kythira::noop_metrics{});

    const auto before = handler_invocations.load();
    kythira::request_vote_request<> req{};
    req._term = 5;

    int status = 0;
    std::string what;
    try {
        std::move(client.send_request_vote(1, req, kythira::testing::scaled_deadline(5000))).get();
    } catch (const kythira::http_client_error& e) {
        status = e.status_code();
    } catch (const std::exception& e) {
        what = e.what();
    }
    BOOST_TEST(status == 415, "expected a typed 415; other exception was: " << what);
    BOOST_TEST(handler_invocations.load() == before);
}

BOOST_AUTO_TEST_SUITE_END()
