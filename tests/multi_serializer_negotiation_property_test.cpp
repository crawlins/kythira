// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_serializer_negotiation_property_test.cpp
/// @brief Two-serializer content negotiation over a live HTTP transport
///        (`.kiro/specs/transport-multi-serializer/`, Task 14).
///
/// Everything before this task tested negotiation with the *rules* in one place
/// (`serializer_registry_unit_test`, no transport) and the *rendering* in
/// another (`http_negotiation_integration_test`, a single-serializer server
/// driven by a raw client). Neither ran a multi-serializer registry through a
/// socket, so the feature the whole spec exists for — a node that can speak two
/// wire formats actually choosing between them with a peer — had never been
/// exercised end to end. This suite is that exercise.
///
/// **The negotiated media type is observed, not inferred.** A round-trip that
/// merely succeeds proves the two sides agreed on *something*; it cannot tell
/// "negotiated CBOR" from "both defaulted to JSON and ignored the headers",
/// which is precisely the failure mode a negotiation test has to exclude. So the
/// bundles carry a recording metrics backend and the assertions read the
/// `media_type` dimension the transport emits on both sides. That also makes
/// this the first suite anywhere to check that dimension carries the right
/// *value* rather than merely being present (Requirements 10.1-10.3).
///
/// The property, over all four client-order x server-order combinations:
/// **the negotiated type is the client's first preference, whatever the
/// server's own order is.** That is `select_output_media_type`'s "the peer's
/// order wins" rule (Task 3), and the four combinations are what distinguish it
/// from "our order wins" — under which the two mixed-order cells would come back
/// in the *server's* preferred type instead.
///
/// Mutation-tested, both halves of the mechanism:
///  * inverting `select_output_media_type` to scan our own order first fails the
///    two mixed-order cells and the convergence case, and correctly leaves the
///    two same-order cells passing — they cannot distinguish the rule;
///  * suppressing the client's `Accept` header fails the same three.
/// The second mutation also measured what the tree covered *before* this suite:
/// with `Accept` removed entirely, `http_negotiation_integration_test`,
/// `http_integration_test` and `http_client_test` all still pass. Nothing
/// protected that header until now, because every shipped bundle is
/// single-serializer, where a missing `Accept` still yields the one type the
/// server has.

#define BOOST_TEST_MODULE multi_serializer_negotiation_property_test
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

using registry_json_first = kythira::multi_serializer_registry<json_serializer, cbor_serializer>;
using registry_cbor_first = kythira::multi_serializer_registry<cbor_serializer, json_serializer>;

using json_first_types = negotiating_transport_types<json_serializer, registry_json_first>;
using cbor_first_types = negotiating_transport_types<cbor_serializer, registry_cbor_first>;

static_assert(kythira::transport_types<json_first_types>);
static_assert(kythira::transport_types<cbor_first_types>);

/// The four cells share every assertion but the expected type, so they share a
/// checker: a cell that passed for the wrong reason would otherwise be one
/// copy-paste away.
auto check_all_three_negotiated(const exchange_observation& obs, const std::string& expected)
    -> void {
    BOOST_TEST(obs.failed_rpcs == 0, "unexpected RPC failure: " << obs.first_error);
    BOOST_TEST(obs.round_trips_correct);
    BOOST_TEST(obs.handler_invocations == 3);

    // Three requests, each labelled with the negotiated type — not one, and not
    // the default with two silently unlabelled.
    BOOST_REQUIRE_EQUAL(obs.client_request_types.size(), 3u);
    for (const auto& t : obs.client_request_types) {
        BOOST_TEST(t == expected);
    }
    // And the response leg, which a request-only assertion misses: the server is
    // free to answer in a type other than the one it was asked in, so "the
    // client decoded with `expected`" is a separate claim.
    BOOST_REQUIRE_EQUAL(obs.client_response_types.size(), 3u);
    for (const auto& t : obs.client_response_types) {
        BOOST_TEST(t == expected);
    }
    BOOST_TEST(!obs.server_response_type.empty(),
               "server emitted no media_type dimension at all -- the assertion below would pass "
               "vacuously against an empty string");
    BOOST_TEST(obs.server_response_type == expected);
}

}  // namespace

// ── The four client-order x server-order cells ─────────────────────────────

/// Both sides prefer JSON. The baseline: no disagreement to resolve, so a
/// failure here is a wiring fault rather than a negotiation one.
BOOST_AUTO_TEST_CASE(json_first_client_against_json_first_server_uses_json) {
    const auto obs = run_exchange<json_first_types, json_first_types>(18250);
    check_all_three_negotiated(obs, json_media);
}

/// The client prefers JSON, the server prefers CBOR. **This is the cell that
/// distinguishes the rule from its opposite**: under "our order wins" the
/// server would answer CBOR here.
BOOST_AUTO_TEST_CASE(json_first_client_against_cbor_first_server_still_uses_json) {
    const auto obs = run_exchange<json_first_types, cbor_first_types>(18251);
    check_all_three_negotiated(obs, json_media);
}

/// The mirror image, and the one that proves the previous cell is not simply
/// "JSON always wins" — here the same rule selects CBOR.
BOOST_AUTO_TEST_CASE(cbor_first_client_against_json_first_server_still_uses_cbor) {
    const auto obs = run_exchange<cbor_first_types, json_first_types>(18252);
    check_all_three_negotiated(obs, cbor_media);
}

/// Both sides prefer CBOR. Together with the first cell this pins that the
/// transport really can run a non-JSON format end to end — the hardcoded
/// `application/json` that Requirement 9.4 removed would fail exactly here.
BOOST_AUTO_TEST_CASE(cbor_first_client_against_cbor_first_server_uses_cbor) {
    const auto obs = run_exchange<cbor_first_types, cbor_first_types>(18253);
    check_all_three_negotiated(obs, cbor_media);
}

/// @brief The capability cache converges and stays converged (Task 14).
///
/// Convergence is asserted as *stability across three exchanges*, which is what
/// the requirement is about: the cache is an optimisation, so the observable
/// promise is that it never sends the client somewhere worse than where it
/// started.
///
/// **This cell no longer distinguishes a written cache from an absent one, and
/// that is deliberate rather than an oversight.** It used to: the cache held the
/// media type the peer *answered* in, so an "our order wins" mutation on the
/// server made the response json, the cache recorded json, and requests 2 and 3
/// visibly diverged from request 1. Requirement 6.4 now stores the type the peer
/// **accepted** — the request's own — precisely because the response type is a
/// different fact (see `peer_capability_cache.hpp`), and under that rule a
/// client with no cache at all sends its default all three times and looks
/// identical from here.
///
/// What still pins that the cache is written is `accept_post_negotiation_test`,
/// where the first attempt is *refused*: requests 2 and 3 open with the type the
/// retry found only if the first exchange left a record, and the attempt count
/// separates the two outright. This case keeps the stability property, which is
/// the one Task 14 names.
BOOST_AUTO_TEST_CASE(the_capability_cache_converges_after_the_first_exchange) {
    const auto obs = run_exchange<cbor_first_types, json_first_types>(18254);

    BOOST_TEST(obs.failed_rpcs == 0, "unexpected RPC failure: " << obs.first_error);
    BOOST_REQUIRE_EQUAL(obs.client_request_types.size(), 3u);
    // All three requests went out in the same type: the first from the registry
    // default, the second and third from the cache entry the first wrote.
    BOOST_TEST(obs.client_request_types[0] == cbor_media);
    BOOST_TEST(obs.client_request_types[1] == obs.client_request_types[0]);
    BOOST_TEST(obs.client_request_types[2] == obs.client_request_types[0]);
    BOOST_TEST(obs.server_response_type == cbor_media);
}
