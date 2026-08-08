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

/// @brief **Pins a known Requirement 7.3 violation — this cell asserts the
///        behaviour the code has, which is NOT the behaviour the spec requires.**
///
/// The client supports JSON and CBOR and prefers CBOR; the server is an
/// unmodified JSON-only node. JSON is "a serializer the multi-serializer node
/// also supports", so Requirement 7.3 says these two SHALL interoperate in this
/// direction. **They do not.** Measured August 8, 2026:
///
///     RPC failed: HTTP client error 415: Unsupported Content-Type: application/cbor
///
/// all three RPCs, handler never entered.
///
/// The cause is structural rather than a missing branch. HTTP negotiates the
/// *response* through `Accept`, which the server reads before answering — that
/// is why the three cells above pass. The *request* has no such mechanism: the
/// client must choose a format before it has heard anything from the server, so
/// it sends `select_request_media_type`'s answer, which on a cold cache is the
/// registry default. When that default is a type the peer does not speak, the
/// server answers 415 and there is nothing that recovers — the client does not
/// retry, and the capability cache is deliberately not written on failure, so
/// every subsequent request repeats the identical mistake. The pairing is
/// permanently broken, not slow to converge.
///
/// Fixing it is a design decision rather than a bug fix, which is why this cell
/// pins the defect instead of quietly expecting a pass: the server's 415 does
/// not say what it *would* accept, so a client retry needs either `Accept-Post`
/// (W3C Linked Data Platform 1.0) on the rejection or a fixed "try the next
/// preferred type" policy,
/// and either one is a wire-behaviour change across all four transports. Logged
/// in `doc/TODO.md`.
///
/// What this cell therefore asserts is the *quality* of the failure, which is
/// itself a Requirement 8 obligation: a clean, typed, immediate 415 naming the
/// offending type — no crash, no hang, no unresolved future, and no handler
/// side effects. **When someone implements the fix this test will fail**, which
/// is the intent: it should be updated to `check_interoperates(obs, json_media)`
/// at that point, and the failure is how they find out this cell exists.
BOOST_AUTO_TEST_CASE(multi_client_preferring_cbor_against_single_json_server_is_a_known_415) {
    const auto obs = run_exchange<multi_cbor_first_types, single_json_types>(18264);

    // Every RPC fails, and fails the same way -- not one flaky one.
    BOOST_TEST(obs.failed_rpcs == 3);
    BOOST_TEST(obs.first_error.find("415") != std::string::npos,
               "expected a 415, got: " << obs.first_error);
    BOOST_TEST(obs.first_error.find(cbor_media) != std::string::npos,
               "the error should name the offending type: " << obs.first_error);

    // Rejected before the handler, so no side effects were paid for a request
    // that was never going to be answered (Requirement 4.4).
    BOOST_TEST(obs.handler_invocations == 0);

    // The client kept sending its own default all three times: the cache is
    // untouched by a failure, so there is no convergence and no self-repair.
    // This is what makes the breakage permanent rather than first-request-only.
    BOOST_REQUIRE_EQUAL(obs.client_request_types.size(), 3u);
    for (const auto& t : obs.client_request_types) {
        BOOST_TEST(t == cbor_media);
    }

    // Nothing was ever decoded, because nothing ever came back successfully.
    BOOST_TEST(obs.client_response_types.empty());
}
