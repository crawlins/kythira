/// @file single_serializer_regression_test.cpp
/// @brief Single-serializer configurations are unchanged by the introduction of
///        registries (`.kiro/specs/transport-multi-serializer/`, Task 13).
///
/// Requirement 7 is the promise that made adopting a registry an additive change
/// rather than a migration: every shipped `Types` bundle still names one
/// serializer (`single_serializer_registry<RPC_Serializer>`,
/// `http_transport.hpp:53`), and such a node must behave on the wire exactly as
/// it did before negotiation existed. Every other suite in the tree runs that
/// configuration, so an ordinary regression would be caught — but only as
/// "something broke", and only for behaviour those suites happen to assert.
/// What none of them assert is the specific thing Requirement 7 promises: that
/// the registry adds **nothing** to the wire.
///
/// So this suite checks the claim directly rather than transitively. It drives a
/// real `cpp_httplib_server` with a raw `httplib::Client`, because a
/// pre-negotiation peer is precisely what our own client can no longer imitate —
/// `cpp_httplib_client` always sets `Accept` and always sets `Content-Type`, and
/// the peers Requirement 7.3 is about do neither.
///
/// The load-bearing assertion is byte equality against the serializer called
/// directly. "It round-tripped" would also hold for a registry that wrapped,
/// re-encoded or re-framed the payload, so long as it did so symmetrically on
/// both ends — and a node doing that would be silently unable to talk to any
/// unmodified peer, which is the exact regression Requirement 7 exists to
/// prevent.

#define BOOST_TEST_MODULE single_serializer_regression_test
#include <boost/test/unit_test.hpp>

#include "negotiation_test_harness.hpp"

#include <raft/cbor_serializer.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace kythira::negotiation_test;

using data_type = std::vector<std::byte>;
using json_serializer = kythira::json_rpc_serializer<data_type>;
using cbor_serializer = kythira::cbor_rpc_serializer<data_type>;

using single_json = kythira::single_serializer_registry<json_serializer>;
using single_cbor = kythira::single_serializer_registry<cbor_serializer>;

// These two bundles differ from the shipped `http_transport_types` only in the
// metrics backend, which is what lets the assertions read the negotiated type.
using json_server_types = negotiating_transport_types<json_serializer, single_json>;
using cbor_server_types = negotiating_transport_types<cbor_serializer, single_cbor>;

constexpr const char* endpoint_request_vote = "/v1/raft/request_vote";

auto to_string(const data_type& bytes) -> std::string {
    std::string s;
    s.reserve(bytes.size());
    for (auto b : bytes) {
        s.push_back(static_cast<char>(b));
    }
    return s;
}

auto sample_request() -> kythira::request_vote_request<> {
    kythira::request_vote_request<> req;
    req._term = 5;
    req._candidate_id = 42;
    req._last_log_index = 10;
    req._last_log_term = 4;
    return req;
}

/// Stands up one `cpp_httplib_server` over a single-serializer registry and
/// counts handler entries.
template<typename Server_Types> struct server_fixture {
    recording_metrics metrics;
    kythira::cpp_httplib_server<Server_Types> server;
    std::atomic<int> handler_invocations{0};

    explicit server_fixture(std::uint16_t port)
        : server(
              bind_address, port,
              [] {
                  kythira::cpp_httplib_server_config c;
                  c.request_timeout = std::chrono::seconds{5};
                  return c;
              }(),
              metrics) {
        server.register_request_vote_handler([this](const kythira::request_vote_request<>& req) {
            handler_invocations.fetch_add(1);
            kythira::request_vote_response<> resp;
            resp._term = req.term() + 1;
            resp._vote_granted = true;
            return resp;
        });
        server.start();
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
    }

    ~server_fixture() { server.stop(); }

    server_fixture(const server_fixture&) = delete;
    auto operator=(const server_fixture&) -> server_fixture& = delete;
};

}  // namespace

/// @brief Requirement 7.1/7.2: the request body a registry decodes is exactly
///        the bytes the serializer produces, and the response body is exactly
///        the bytes it would produce — no wrapper, no re-framing.
BOOST_AUTO_TEST_CASE(a_single_serializer_server_is_byte_identical_to_its_serializer) {
    server_fixture<json_server_types> fixture(18280);

    const json_serializer serializer;
    const auto request_bytes = serializer.serialize(sample_request());

    httplib::Client client(bind_address, 18280);
    client.set_read_timeout(5, 0);
    httplib::Headers headers{{"Content-Type", json_media}};
    auto result = client.Post(endpoint_request_vote, headers, to_string(request_bytes), json_media);

    BOOST_REQUIRE(result);
    BOOST_TEST(result->status == 200);
    BOOST_TEST(fixture.handler_invocations.load() == 1);

    // The response is labelled with the serializer's own media type -- not the
    // hardcoded `application/json` that Requirement 9.4 removed, which would be
    // indistinguishable here and is why the CBOR case below exists.
    BOOST_TEST(result->get_header_value("Content-Type") == json_media);

    // And the body is byte-for-byte what the serializer alone would emit.
    kythira::request_vote_response<> expected;
    expected._term = 6;
    expected._vote_granted = true;
    // Compared as strings rather than as `data_type`: the claim is byte
    // equality either way, but `std::byte` has no `operator<<`, so a vector
    // comparison could not print what it saw when it failed.
    BOOST_TEST(result->body == to_string(serializer.serialize(expected)));
}

/// @brief The same claim for a non-JSON serializer.
///
/// This is the cell that a hardcoded `application/json` label fails: the JSON
/// case above cannot distinguish "labelled with the configured serializer's
/// type" from "always labelled JSON", because for it the two answers coincide.
BOOST_AUTO_TEST_CASE(a_single_cbor_server_labels_and_encodes_as_cbor) {
    server_fixture<cbor_server_types> fixture(18281);

    const cbor_serializer serializer;
    const auto request_bytes = serializer.serialize(sample_request());

    httplib::Client client(bind_address, 18281);
    client.set_read_timeout(5, 0);
    httplib::Headers headers{{"Content-Type", cbor_media}};
    auto result = client.Post(endpoint_request_vote, headers, to_string(request_bytes), cbor_media);

    BOOST_REQUIRE(result);
    BOOST_TEST(result->status == 200);
    BOOST_TEST(fixture.handler_invocations.load() == 1);
    BOOST_TEST(result->get_header_value("Content-Type") == cbor_media);

    kythira::request_vote_response<> expected;
    expected._term = 6;
    expected._vote_granted = true;
    // Compared as strings rather than as `data_type`: the claim is byte
    // equality either way, but `std::byte` has no `operator<<`, so a vector
    // comparison could not print what it saw when it failed.
    BOOST_TEST(result->body == to_string(serializer.serialize(expected)));
}

/// @brief Requirement 7.3: a peer that predates negotiation — no `Accept` header
///        at all — is served, not rejected.
///
/// This is the case that decides whether adding registries was additive. An
/// absent `Accept` means "no preference", not "accepts nothing"
/// (`single_serializer_registry::select_output_media_type`), so it must yield
/// the server's one type rather than a 406. A 406 here would mean every
/// unmodified node in an existing cluster stopped being able to talk to an
/// upgraded one.
BOOST_AUTO_TEST_CASE(a_peer_sending_no_accept_header_is_served_the_default) {
    server_fixture<json_server_types> fixture(18282);

    const json_serializer serializer;
    const auto request_bytes = serializer.serialize(sample_request());

    httplib::Client client(bind_address, 18282);
    client.set_read_timeout(5, 0);
    // Content-Type only. No Accept -- exactly what a pre-negotiation peer sends,
    // and something `cpp_httplib_client` can no longer be made to produce.
    httplib::Headers headers{{"Content-Type", json_media}};
    auto result = client.Post(endpoint_request_vote, headers, to_string(request_bytes), json_media);

    BOOST_REQUIRE(result);
    BOOST_TEST(result->status == 200);
    BOOST_TEST(fixture.handler_invocations.load() == 1);
    BOOST_TEST(result->get_header_value("Content-Type") == json_media);
}

/// @brief A single-serializer server still answers `Accept: */*`.
///
/// curl and a great many client libraries send `*/*` by default, so treating it
/// as "matches nothing" would 406 the most common header value there is. Pinned
/// here as well as in the unit test because the wildcard rule reaching the wire
/// depends on the server parsing `Accept` and passing it down, not only on the
/// registry's matcher being right.
BOOST_AUTO_TEST_CASE(a_wildcard_accept_is_served_by_a_single_serializer_server) {
    server_fixture<json_server_types> fixture(18283);

    const json_serializer serializer;
    const auto request_bytes = serializer.serialize(sample_request());

    httplib::Client client(bind_address, 18283);
    client.set_read_timeout(5, 0);
    httplib::Headers headers{{"Content-Type", json_media}, {"Accept", "*/*"}};
    auto result = client.Post(endpoint_request_vote, headers, to_string(request_bytes), json_media);

    BOOST_REQUIRE(result);
    BOOST_TEST(result->status == 200);
    BOOST_TEST(result->get_header_value("Content-Type") == json_media);
    BOOST_TEST(fixture.handler_invocations.load() == 1);
}
