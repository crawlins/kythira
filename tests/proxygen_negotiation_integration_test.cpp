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
#include <raft/json_serializer.hpp>
#include <raft/executor_default.hpp>

#include <folly/executors/IOThreadPoolExecutor.h>
#include <httplib.h>

#include "test_timeout_scale.hpp"

#include <atomic>
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

BOOST_AUTO_TEST_SUITE_END()
