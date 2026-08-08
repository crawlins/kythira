/// @file multi_serializer_interop_test.cpp
/// @brief Multi-serializer <-> single-serializer interoperability over HTTP
///        (`.kiro/specs/transport-multi-serializer/`, Task 15).
///
/// Requirement 7.3: a node running a multi-serializer transport talking to a
/// node running a *single*-serializer transport of a serializer the
/// multi-serializer node also supports SHALL interoperate correctly **in both
/// directions**, without requiring the single-serializer node to change.
///
/// The two directions are not symmetric, and that asymmetry is the whole reason
/// this suite exists rather than being folded into the negotiation property
/// test. HTTP negotiates the *response* through `Accept`, which the server reads
/// before answering — so a single-serializer server can always satisfy a
/// multi-serializer client's response leg. But the *request* carries whatever
/// the client chose before it had heard anything from the server at all. There
/// is no mechanism by which a client learns a server's formats in advance, so
/// the request leg is not negotiated; it is guessed, from the registry default.
///
/// Every cell therefore names which leg it is exercising, and the cells are
/// chosen so that a pass cannot be explained by both sides simply happening to
/// share a default.

#define BOOST_TEST_MODULE multi_serializer_interop_test
#include <boost/test/unit_test.hpp>

#include "negotiation_test_harness.hpp"

#include <raft/cbor_serializer.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace {

using namespace kythira::negotiation_test;

using data_type = std::vector<std::byte>;
using json_serializer = kythira::json_rpc_serializer<data_type>;
using cbor_serializer = kythira::cbor_rpc_serializer<data_type>;

// The single-serializer registries are exactly what every shipped `Types` bundle
// uses today (`http_transport.hpp:53`), so a "single" side here is a stand-in
// for an unmodified node — which is what Requirement 7.3's "without requiring
// the single-serializer node to change" is about.
using single_json = kythira::single_serializer_registry<json_serializer>;
using single_cbor = kythira::single_serializer_registry<cbor_serializer>;
using multi_json_first = kythira::multi_serializer_registry<json_serializer, cbor_serializer>;
using multi_cbor_first = kythira::multi_serializer_registry<cbor_serializer, json_serializer>;

using single_json_types = negotiating_transport_types<json_serializer, single_json>;
using single_cbor_types = negotiating_transport_types<cbor_serializer, single_cbor>;
using multi_json_first_types = negotiating_transport_types<json_serializer, multi_json_first>;
using multi_cbor_first_types = negotiating_transport_types<cbor_serializer, multi_cbor_first>;

static_assert(kythira::transport_types<single_json_types>);
static_assert(kythira::transport_types<multi_json_first_types>);

auto check_interoperates(const exchange_observation& obs, const std::string& expected) -> void {
    BOOST_TEST(obs.failed_rpcs == 0, "RPC failed: " << obs.first_error);
    BOOST_TEST(obs.round_trips_correct);
    BOOST_TEST(obs.handler_invocations == 3);
    BOOST_REQUIRE_EQUAL(obs.client_request_types.size(), 3u);
    for (const auto& t : obs.client_request_types) {
        BOOST_TEST(t == expected);
    }
    BOOST_REQUIRE_EQUAL(obs.client_response_types.size(), 3u);
    for (const auto& t : obs.client_response_types) {
        BOOST_TEST(t == expected);
    }
    BOOST_TEST(obs.server_response_type == expected);
}

}  // namespace

// ── Single-serializer as client, multi-serializer as server ────────────────

/// The unmodified node speaks JSON; the new node speaks both and prefers JSON.
/// The easy cell, included because its *absence* would leave the harder ones
/// with nothing to be compared against.
BOOST_AUTO_TEST_CASE(single_json_client_against_multi_server_preferring_json) {
    const auto obs = run_exchange<single_json_types, multi_json_first_types>(18260);
    check_interoperates(obs, json_media);
}

/// The load-bearing cell of this direction: the unmodified node speaks **only**
/// JSON while the multi-serializer server *prefers* CBOR. The server has to set
/// its own preference aside and answer JSON because that is all the client's
/// `Accept` offers. A server that answered its own default here would produce a
/// response the client cannot decode — the exact failure Requirement 7.3 forbids.
BOOST_AUTO_TEST_CASE(single_json_client_against_multi_server_preferring_cbor) {
    const auto obs = run_exchange<single_json_types, multi_cbor_first_types>(18261);
    check_interoperates(obs, json_media);
}

/// The same shape with the formats swapped, so neither cell above can be passing
/// merely because JSON is the value everything falls back to.
BOOST_AUTO_TEST_CASE(single_cbor_client_against_multi_server_preferring_json) {
    const auto obs = run_exchange<single_cbor_types, multi_json_first_types>(18262);
    check_interoperates(obs, cbor_media);
}

// ── Multi-serializer as client, single-serializer as server ────────────────

/// The multi-serializer client's default is the one format the server speaks, so
/// its unnegotiated request leg happens to be right.
BOOST_AUTO_TEST_CASE(multi_client_preferring_json_against_single_json_server) {
    const auto obs = run_exchange<multi_json_first_types, single_json_types>(18263);
    check_interoperates(obs, json_media);
}

/// @brief Exhaustion: a client with nothing else to offer fails cleanly.
///
/// A `single_serializer_registry<cbor>` client against a JSON-only server has
/// no second format to fall back to, so the retry loop must terminate
/// immediately rather than re-sending the only type it has. This is the case
/// that proves the loop is bounded rather than merely usually short — and it is
/// also every single-serializer deployment shipping today, which is why it must
/// cost exactly one attempt and not two.
BOOST_AUTO_TEST_CASE(a_client_with_no_alternative_format_fails_after_one_attempt) {
    const auto obs = run_exchange<single_cbor_types, single_json_types>(18265);

    BOOST_TEST(obs.failed_rpcs == 3);
    BOOST_TEST(obs.first_error.find("415") != std::string::npos,
               "expected a 415, got: " << obs.first_error);
    BOOST_TEST(obs.handler_invocations == 0);

    // Exactly one attempt per RPC -- three, not six. A retry loop that re-tried
    // the type it had just been refused would show six here and still
    // eventually fail, which is the shape this asserts against.
    BOOST_REQUIRE_EQUAL(obs.client_request_types.size(), 3u);
    for (const auto& t : obs.client_request_types) {
        BOOST_TEST(t == cbor_media);
    }
    BOOST_TEST(obs.client_response_types.empty());
}

/// @brief **The cell Requirement 7.3 turns on**, and the one that drove the
///        blind-retry fix.
///
/// The client supports JSON and CBOR and prefers CBOR; the server is an
/// unmodified JSON-only node. JSON is "a serializer the multi-serializer node
/// also supports", so Requirement 7.3 says these two SHALL interoperate in this
/// direction, "without requiring the single-serializer node to change".
///
/// **Before the fix they did not.** Measured August 8, 2026:
///
///     RPC failed: HTTP client error 415: Unsupported Content-Type: application/cbor
///
/// on all three RPCs, handler never entered, and *permanently* — the client did
/// not retry, and the capability cache is deliberately not written on failure
/// (correctly: caching the type that just failed would repeat the mistake with
/// more confidence), so every later request re-sent the same default.
///
/// The cause was structural rather than a missing branch, and still is: HTTP
/// negotiates the *response* through `Accept`, which the server reads before
/// answering — that is why the three cells above always passed. The *request*
/// has no such mechanism, so the client must commit to an encoding before it has
/// heard anything from the peer.
///
/// What changed is what happens **after** the rejection. The client now walks
/// the rest of `preferred_media_types()` on a 415 and caches whichever works.
/// `Accept-Post` (W3C Linked Data Platform 1.0 §7.1) would converge in one retry
/// instead of up to N, but it requires the *peer* to emit a header, and 7.3
/// promises interoperation without the peer changing — so it is an optimisation
/// to layer on later, not the mechanism.
///
/// The assertions below are deliberately not `check_interoperates`: the whole
/// point is that the first exchange costs **two** attempts and the rest cost
/// one, which a uniform "every request used JSON" check would hide.
BOOST_AUTO_TEST_CASE(multi_client_preferring_cbor_interoperates_with_a_single_json_server) {
    const auto obs = run_exchange<multi_cbor_first_types, single_json_types>(18264);

    BOOST_TEST(obs.failed_rpcs == 0, "RPC failed: " << obs.first_error);
    BOOST_TEST(obs.round_trips_correct);
    BOOST_TEST(obs.handler_invocations == 3);

    // Four attempts for three RPCs: the first RPC guessed CBOR, was refused,
    // and retried in JSON; the second and third went straight out in JSON from
    // the cache the retry populated. This exact shape is the evidence that the
    // retry happened *and* that it converged -- a client that retried on every
    // request would show six attempts, and one that never retried would show
    // three failures.
    BOOST_REQUIRE_EQUAL(obs.client_request_types.size(), 4u);
    BOOST_TEST(obs.client_request_types[0] == cbor_media);
    BOOST_TEST(obs.client_request_types[1] == json_media);
    BOOST_TEST(obs.client_request_types[2] == json_media);
    BOOST_TEST(obs.client_request_types[3] == json_media);

    // Three successful responses, all decoded as JSON -- one per RPC, so the
    // refused attempt produced no response leg at all.
    BOOST_REQUIRE_EQUAL(obs.client_response_types.size(), 3u);
    for (const auto& t : obs.client_response_types) {
        BOOST_TEST(t == json_media);
    }
    BOOST_TEST(obs.server_response_type == json_media);
}
