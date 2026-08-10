/// @file accept_post_negotiation_test.cpp
/// @brief `Accept-Post` as an optimisation on top of the 415 retry
///        (W3C Linked Data Platform 1.0 §7.1), over cpp-httplib.
///
/// The 415 retry (`doc/TODO.md`, Requirement 7.3) is a **blind walk**: a client
/// whose first guess is refused works through the rest of its own preference
/// list, one round trip per guess. That is the mechanism, and it has to stay the
/// mechanism, because an unmodified single-serializer peer — the case the
/// requirement is about — tells us nothing about itself.
///
/// `Accept-Post` is the peer telling us anyway. A server that puts it on its 415
/// names the request media types it *would* have taken, and a client that reads
/// it converges on the second attempt instead of possibly the Nth.
///
/// **Three serializers, not two, and that is the whole design of this file.**
/// With two, the second is the only candidate left after the first rejection, so
/// an informed jump and a blind walk pick the same type and no assertion can
/// tell them apart — the tests would pass with the feature deleted. With
/// `{cbor, alt-json, json}` against a JSON-only peer, the blind walk *must*
/// spend a round trip on `alt-json` and the informed jump *must not*, so the
/// recorded retry sequence separates them outright.
///
/// The load-bearing assertions are therefore about the retry sequence and the
/// handler count, never about the RPC merely succeeding: a blind walk succeeds
/// too, just more slowly, which is exactly why this needs to measure rather than
/// check a return value.

#define BOOST_TEST_MODULE accept_post_negotiation_test
#include <boost/test/unit_test.hpp>

#include "negotiation_test_harness.hpp"

#include <raft/cbor_serializer.hpp>
#include <raft/http_content_negotiation.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace kythira::negotiation_test;

using data_type = std::vector<std::byte>;
using json_serializer = kythira::json_rpc_serializer<data_type>;
using cbor_serializer = kythira::cbor_rpc_serializer<data_type>;

/// A serializer byte-identical to JSON that announces a different media type.
///
/// A third *label* is all this needs — the thing under test is which media type
/// the client picks for its retry, and it never gets as far as encoding in this
/// one on the path that matters. A third real codec would tie the file to
/// whichever optional serializer happened to be installed on the build leg,
/// which is a dependency this file has no reason to acquire.
struct alt_json_serializer : json_serializer {
    [[nodiscard]] auto media_type() const -> std::string { return "application/x-alt-json"; }
    [[nodiscard]] auto name() const -> std::string { return "alt-json"; }
};

constexpr const char* alt_media = "application/x-alt-json";

// Fresh ports: the other negotiation suites hold 18230-18239 and 18295-18296,
// and may run concurrently under `ctest -j`.
constexpr std::uint16_t advertise_port = 18310;
constexpr std::uint16_t informed_retry_port = 18311;
constexpr std::uint16_t single_serializer_port = 18312;

using single_json = kythira::single_serializer_registry<json_serializer>;
/// Preference order `{cbor, alt-json, json}`: cbor is the default and therefore
/// the first guess, `alt-json` is what a blind walk tries next, and `json` is
/// what the peer will actually name.
using multi_three =
    kythira::multi_serializer_registry<cbor_serializer, alt_json_serializer, json_serializer>;

using single_json_types = negotiating_transport_types<json_serializer, single_json>;
using multi_three_types = negotiating_transport_types<cbor_serializer, multi_three>;
using single_cbor_types =
    negotiating_transport_types<cbor_serializer,
                                kythira::single_serializer_registry<cbor_serializer>>;

}  // namespace

BOOST_AUTO_TEST_SUITE(accept_post_negotiation_tests)

/// The server half, checked on the wire rather than through our own client: a
/// raw `httplib::Client` sends a `Content-Type` the server cannot decode and
/// reads the response headers directly.
///
/// A foreign client is the only honest way to ask this. Going through
/// `cpp_httplib_client` would prove at most that our client and our server agree
/// with each other, which is not what a header on the wire is for.
BOOST_AUTO_TEST_CASE(a_server_names_what_it_would_have_taken_on_a_415) {
    recording_metrics server_metrics;
    kythira::cpp_httplib_server<single_json_types> server(bind_address, advertise_port, {},
                                                          server_metrics);
    int invocations = 0;
    server.register_request_vote_handler([&](const kythira::request_vote_request<>&) {
        ++invocations;
        return kythira::request_vote_response<>{};
    });
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    httplib::Client raw(bind_address, advertise_port);
    raw.set_connection_timeout(5, 0);
    auto resp = raw.Post("/v1/raft/request_vote", "{}", alt_media);
    BOOST_REQUIRE_MESSAGE(resp, "the request itself failed");
    BOOST_TEST(resp->status == 415);
    // The header is present and says exactly what this server can decode.
    BOOST_TEST(resp->has_header(kythira::header_accept_post));
    BOOST_CHECK_EQUAL(resp->get_header_value(kythira::header_accept_post), "application/json");
    // Still answered before the handler ran, which is what makes retrying safe
    // in the first place. Advertising must not have moved the rejection later.
    BOOST_TEST(invocations == 0);

    server.stop();
}

/// A 415 is the only status this belongs on. A 404 carries no statement about
/// media types, and a client that read one there would be acting on a header
/// that answered a different question.
BOOST_AUTO_TEST_CASE(a_server_does_not_advertise_on_an_unrelated_error) {
    recording_metrics server_metrics;
    kythira::cpp_httplib_server<single_json_types> server(bind_address, advertise_port, {},
                                                          server_metrics);
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>&) { return kythira::request_vote_response<>{}; });
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    httplib::Client raw(bind_address, advertise_port);
    raw.set_connection_timeout(5, 0);
    auto resp = raw.Post("/v1/raft/no_such_endpoint", "{}", "application/json");
    BOOST_REQUIRE_MESSAGE(resp, "the request itself failed");
    BOOST_TEST(resp->status == 404);
    BOOST_TEST(!resp->has_header(kythira::header_accept_post));

    server.stop();
}

/// The client half, and the case the whole feature exists for.
///
/// `{cbor, alt-json, json}` client against a JSON-only server. cbor is refused;
/// the peer's 415 names `application/json`; the retry goes straight there.
/// **Exactly one retry, naming json** is the assertion — a blind walk would
/// record two, the first of them `alt-json`, and would otherwise look identical
/// from the outside.
BOOST_AUTO_TEST_CASE(a_client_retries_straight_to_the_type_the_peer_named) {
    const auto obs = run_exchange<multi_three_types, single_json_types>(informed_retry_port);

    BOOST_TEST(obs.failed_rpcs == 0, "RPC failed: " << obs.first_error);
    BOOST_TEST(obs.round_trips_correct);
    // Three RPCs, three handler entries: the 415s cost nothing on the server and
    // nothing was applied twice.
    BOOST_TEST(obs.handler_invocations == 3);

    BOOST_REQUIRE_EQUAL(obs.retry_media_types.size(), 1U);
    BOOST_CHECK_EQUAL(obs.retry_media_types.front(), json_media);
    // Stated as its own check because it is the specific waste being removed:
    // `alt-json` is what the blind walk would have burned a round trip on.
    const auto tried_alt = std::find(obs.retry_media_types.begin(), obs.retry_media_types.end(),
                                     alt_media) != obs.retry_media_types.end();
    BOOST_CHECK_MESSAGE(
        !tried_alt, "the retry walked through " << alt_media << " instead of reading Accept-Post");

    // The whole wire sequence, since `request.sent` is emitted per *attempt*
    // rather than per RPC: cbor is refused, the retry sends json, and RPCs 2 and
    // 3 open with json because the capability cache recorded it after the first
    // success. Four sends for three RPCs. A blind walk would make it five, with
    // alt-json second — so this pins the saving as a count, not just as an
    // absence.
    BOOST_REQUIRE_EQUAL(obs.client_request_types.size(), 4U);
    BOOST_CHECK_EQUAL(obs.client_request_types[0], cbor_media);
    BOOST_CHECK_EQUAL(obs.client_request_types[1], json_media);
    BOOST_CHECK_EQUAL(obs.client_request_types[2], json_media);
    BOOST_CHECK_EQUAL(obs.client_request_types[3], json_media);
}

/// Every configuration shipping today has one serializer, so this is the path
/// that must not change. The peer advertises, and it still cannot help: the one
/// type we have is the one just refused, so the 415 surfaces as the error it is
/// rather than being retried or turned into a broken promise.
BOOST_AUTO_TEST_CASE(a_single_serializer_client_still_fails_cleanly_when_advertised_at) {
    const auto obs = run_exchange<single_cbor_types, single_json_types>(single_serializer_port);

    BOOST_TEST(obs.failed_rpcs == 3);
    BOOST_TEST(obs.handler_invocations == 0);
    BOOST_TEST(obs.retry_media_types.empty());
    BOOST_TEST(obs.first_error.find("415") != std::string::npos,
               "expected the 415 itself, got: " << obs.first_error);
}

BOOST_AUTO_TEST_SUITE_END()
