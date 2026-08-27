// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file timeout_now_wire_property_test.cpp
/// @brief `timeout_now_request` / `timeout_now_response` round-trip through the
///        JSON and CBOR serializers, and are refused when they are not what the
///        caller asked for.
///
/// TimeoutNow is the newest RPC in the system and the only one whose *absence*
/// is a supported configuration — a transport that has not implemented it is
/// legal, and `network_client_with_timeout_now` is what lets the core tell.
/// That makes the wire encoding easy to get subtly wrong without anything
/// noticing: nothing else in the build would fail if a field were dropped,
/// because leadership transfer is exercised over the in-process fabric.
///
/// Two properties, and the second matters more than it looks:
///
///  1. Every field survives a round trip, including `group_id`.
///  2. A payload of one type does not decode as another. Every serializer here
///     tags its payloads, and the tag is what stops a `request_vote_request`
///     — which has a very similar shape — from being read as a TimeoutNow and
///     handing a follower permission to campaign.
///
/// The protobuf and Ion encodings of the same messages are covered by their own
/// suites, which are only built when those optional dependencies are present.

#define BOOST_TEST_MODULE timeout_now_wire_property_test
#include <boost/test/unit_test.hpp>

#include <raft/cbor_serializer.hpp>
#include <raft/json_serializer.hpp>
#include <raft/types.hpp>

#include <concepts>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

using data_type = std::vector<std::byte>;
using kythira::cbor_rpc_serializer;
using kythira::json_rpc_serializer;
using kythira::request_vote_request;
using kythira::serialization_exception;
using kythira::timeout_now_request;
using kythira::timeout_now_response;

namespace {

constexpr std::uint64_t k_group = 0xDEADBEEFull;

template<typename Serializer>
auto round_trip_request(const timeout_now_request<>& original) -> timeout_now_request<> {
    const Serializer serializer;
    return serializer.template deserialize_timeout_now_request<>(serializer.serialize(original));
}

template<typename Serializer>
auto round_trip_response(const timeout_now_response<>& original) -> timeout_now_response<> {
    const Serializer serializer;
    return serializer.template deserialize_timeout_now_response<>(serializer.serialize(original));
}

/// @brief The largest value a serializer round-trips.
///
/// Not the same for both, and the difference is a property of the JSON format
/// rather than of TimeoutNow: `json_rpc_serializer` reads every integer back
/// through boost::json's `as_int64()`, so a `std::uint64_t` above `INT64_MAX`
/// throws on decode. That applies to every message it carries — terms, log
/// indices, node ids — and predates this RPC; it is recorded here because this
/// is the first test to probe the top of the range, not because leadership
/// transfer introduced it. Terms and log indices count real events, so the
/// reachable range is nowhere near either bound.
template<typename Serializer> constexpr auto max_round_trippable() -> std::uint64_t {
    if constexpr (std::same_as<Serializer, json_rpc_serializer<data_type>>) {
        return static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    } else {
        return std::numeric_limits<std::uint64_t>::max();
    }
}

template<typename Serializer> auto check_request_round_trip(const std::string& what) -> void {
    // Boundary values as well as ordinary ones: the encoders narrow through
    // several integer types on the way in and out, and a truncation would only
    // show up at the top of the range.
    constexpr auto top = max_round_trippable<Serializer>();
    const std::vector<timeout_now_request<>> cases{
        timeout_now_request<>{._term = 0, ._leader_id = 0, ._last_log_index = 0, ._group_id = 0},
        timeout_now_request<>{
            ._term = 7, ._leader_id = 3, ._last_log_index = 42, ._group_id = k_group},
        timeout_now_request<>{
            ._term = top, ._leader_id = top, ._last_log_index = top, ._group_id = top},
    };

    for (const auto& original : cases) {
        const auto decoded = round_trip_request<Serializer>(original);
        BOOST_CHECK_MESSAGE(decoded.term() == original.term(), what << ": term");
        BOOST_CHECK_MESSAGE(decoded.leader_id() == original.leader_id(), what << ": leader_id");
        BOOST_CHECK_MESSAGE(decoded.last_log_index() == original.last_log_index(),
                            what << ": last_log_index");
        BOOST_CHECK_MESSAGE(decoded.group_id() == original.group_id(), what << ": group_id");
    }
}

template<typename Serializer> auto check_response_round_trip(const std::string& what) -> void {
    for (bool success : {true, false}) {
        const timeout_now_response<> original{
            ._term = 9, ._success = success, ._group_id = k_group};
        const auto decoded = round_trip_response<Serializer>(original);
        BOOST_CHECK_MESSAGE(decoded.term() == original.term(), what << ": term");
        BOOST_CHECK_MESSAGE(decoded.success() == original.success(), what << ": success");
        BOOST_CHECK_MESSAGE(decoded.group_id() == original.group_id(), what << ": group_id");
    }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(timeout_now_wire_property)

BOOST_AUTO_TEST_CASE(json_round_trips_every_field) {
    check_request_round_trip<json_rpc_serializer<data_type>>("json request");
    check_response_round_trip<json_rpc_serializer<data_type>>("json response");
}

BOOST_AUTO_TEST_CASE(cbor_round_trips_every_field) {
    check_request_round_trip<cbor_rpc_serializer<data_type>>("cbor request");
    check_response_round_trip<cbor_rpc_serializer<data_type>>("cbor response");
}

BOOST_AUTO_TEST_CASE(a_randomised_sweep_round_trips_through_both_encodings) {
    std::mt19937_64 rng{20260826};
    // Bounded by the narrower of the two formats, so one sweep can check both
    // against the same inputs. CBOR's own full-width behaviour is covered by
    // the boundary case above.
    std::uniform_int_distribution<std::uint64_t> any{
        0, max_round_trippable<json_rpc_serializer<data_type>>()};

    for (int i = 0; i < 500; ++i) {
        const timeout_now_request<> original{._term = any(rng),
                                             ._leader_id = any(rng),
                                             ._last_log_index = any(rng),
                                             ._group_id = any(rng)};

        const auto as_json = round_trip_request<json_rpc_serializer<data_type>>(original);
        const auto as_cbor = round_trip_request<cbor_rpc_serializer<data_type>>(original);

        BOOST_REQUIRE_EQUAL(as_json.term(), original.term());
        BOOST_REQUIRE_EQUAL(as_json.leader_id(), original.leader_id());
        BOOST_REQUIRE_EQUAL(as_json.last_log_index(), original.last_log_index());
        BOOST_REQUIRE_EQUAL(as_json.group_id(), original.group_id());

        BOOST_REQUIRE_EQUAL(as_cbor.term(), original.term());
        BOOST_REQUIRE_EQUAL(as_cbor.leader_id(), original.leader_id());
        BOOST_REQUIRE_EQUAL(as_cbor.last_log_index(), original.last_log_index());
        BOOST_REQUIRE_EQUAL(as_cbor.group_id(), original.group_id());
    }
}

BOOST_AUTO_TEST_CASE(cbor_carries_the_full_unsigned_range_that_json_cannot) {
    // Stated as its own case rather than left implicit in the helper above,
    // because "these two formats do not carry the same range" is a fact about
    // the system that should be visible in a test name.
    constexpr auto beyond_int64 =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
    const timeout_now_request<> original{._term = beyond_int64,
                                         ._leader_id = beyond_int64,
                                         ._last_log_index = beyond_int64,
                                         ._group_id = beyond_int64};

    const auto decoded = round_trip_request<cbor_rpc_serializer<data_type>>(original);
    BOOST_CHECK_EQUAL(decoded.term(), beyond_int64);
    BOOST_CHECK_EQUAL(decoded.last_log_index(), beyond_int64);
    BOOST_CHECK_EQUAL(decoded.group_id(), beyond_int64);
}

BOOST_AUTO_TEST_CASE(a_different_message_type_does_not_decode_as_a_timeout_now) {
    // The shapes are close enough to be dangerous: `request_vote_request`
    // carries a term, an id and a last_log_index too. Decoding one as the other
    // would hand a follower permission to campaign on the strength of a vote
    // request, so the type tag has to be checked rather than assumed.
    const request_vote_request<> vote{
        ._term = 4, ._candidate_id = 2, ._last_log_index = 10, ._last_log_term = 3, ._group_id = 1};

    {
        const json_rpc_serializer<data_type> serializer;
        const auto payload = serializer.serialize(vote);
        BOOST_CHECK_THROW(std::ignore = serializer.deserialize_timeout_now_request<>(payload),
                          serialization_exception);
    }
    {
        const cbor_rpc_serializer<data_type> serializer;
        const auto payload = serializer.serialize(vote);
        BOOST_CHECK_THROW(std::ignore = serializer.deserialize_timeout_now_request<>(payload),
                          serialization_exception);
    }
}

BOOST_AUTO_TEST_CASE(a_response_payload_does_not_decode_as_a_request) {
    // The other direction of the same guarantee, and the one an implementation
    // is likelier to get wrong: request and response share a discriminant
    // prefix in every encoding here.
    const timeout_now_response<> response{._term = 5, ._success = true, ._group_id = k_group};

    {
        const json_rpc_serializer<data_type> serializer;
        const auto payload = serializer.serialize(response);
        BOOST_CHECK_THROW(std::ignore = serializer.deserialize_timeout_now_request<>(payload),
                          serialization_exception);
    }
    {
        const cbor_rpc_serializer<data_type> serializer;
        const auto payload = serializer.serialize(response);
        BOOST_CHECK_THROW(std::ignore = serializer.deserialize_timeout_now_request<>(payload),
                          serialization_exception);
    }
}

BOOST_AUTO_TEST_SUITE_END()
