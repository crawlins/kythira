/// @file serializer_registry_unit_test.cpp
/// @brief Unit tests for `single_serializer_registry` / `multi_serializer_registry`
///        and the media-type negotiation they implement
///        (`.kiro/specs/transport-multi-serializer/`, Tasks 1-4, 6, 12).
///
/// Links no transport, so the negotiation rules are pinned independently of any
/// one protocol's status codes and header syntax. The transports consume
/// `select_output_media_type`'s `std::nullopt` and turn it into 406 or 4.06;
/// what "no acceptable type" *means* is decided here and tested here.

#include <raft/cbor_serializer.hpp>
#include <raft/http_content_negotiation.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#define BOOST_TEST_MODULE serializer_registry_unit_test
#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace {

using data_type = std::vector<std::byte>;

/// A minimal serializer carrying a compile-time media type, used to exercise
/// registries with N >= 2 without pulling in a second real wire format.
///
/// It round-trips a single `int` through one tagged byte, which is enough to
/// prove *which* serializer a registry dispatched to — the property the
/// registry is responsible for. Whether any given real serializer encodes
/// correctly is that serializer's own suite's job, not this one's.
template<char Tag, const char* MediaType> struct tagged_serializer {
    [[nodiscard]] auto name() const -> std::string { return std::string{"tagged-"} + Tag; }
    [[nodiscard]] auto media_type() const -> std::string { return MediaType; }

    [[nodiscard]] auto serialize(int value) const -> data_type {
        return {static_cast<std::byte>(Tag), static_cast<std::byte>(value)};
    }

    template<typename T> [[nodiscard]] auto deserialize(const data_type& data) const -> T {
        // The tag is checked, not skipped: a registry that dispatched to the
        // wrong serializer must fail loudly here rather than silently return a
        // plausible value.
        if (data.size() != 2 || data[0] != static_cast<std::byte>(Tag)) {
            throw kythira::serialization_error("tagged_serializer: wrong tag");
        }
        return static_cast<T>(data[1]);
    }
};

constexpr char kAlphaMedia[] = "application/x-test-alpha";
constexpr char kBetaMedia[] = "application/x-test-beta";
constexpr char kGammaMedia[] = "application/x-test-gamma";

using alpha_serializer = tagged_serializer<'A', kAlphaMedia>;
using beta_serializer = tagged_serializer<'B', kBetaMedia>;
// kGammaMedia is deliberately never registered anywhere: it is the "type no
// serializer claims" used to drive the negotiation-failure paths.

using single_alpha = kythira::single_serializer_registry<alpha_serializer>;
using multi_ab = kythira::multi_serializer_registry<alpha_serializer, beta_serializer>;
using multi_ba = kythira::multi_serializer_registry<beta_serializer, alpha_serializer>;

}  // namespace

// ── Concept conformance ────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(registries_satisfy_the_serializer_registry_concept) {
    static_assert(kythira::rpc_serializer<alpha_serializer, data_type>);
    static_assert(kythira::serializer_registry<single_alpha, data_type>);
    static_assert(kythira::serializer_registry<multi_ab, data_type>);
    static_assert(kythira::serializer_registry<
                  kythira::single_serializer_registry<kythira::json_rpc_serializer<data_type>>,
                  data_type>);
}

/// `media_type()` is what the concept newly requires.
BOOST_AUTO_TEST_CASE(json_reports_its_iana_media_type) {
    const kythira::json_rpc_serializer<data_type> s;
    BOOST_TEST(s.media_type() == "application/json");
    // name() is deliberately unchanged: it is a metrics label, not a wire token.
    BOOST_TEST(s.name() == "json");
}

/// Distinct media types across the shipped serializers is a real correctness
/// requirement, not a style point: `multi_serializer_registry` dispatches by
/// `media_type()`, so two serializers claiming the same token would make one of
/// them permanently unreachable, with no diagnostic anywhere. The registry
/// cannot check this itself — `media_type()` is a runtime call, so there is
/// nothing to `static_assert` on — which is precisely why it is checked here,
/// where the values are actually chosen.
///
/// Only the two serializers that build unconditionally are covered; protobuf
/// and Ion need optional dependencies this test deliberately does not take.
BOOST_AUTO_TEST_CASE(shipped_serializers_have_distinct_media_types) {
    const kythira::json_rpc_serializer<data_type> json;
    const kythira::cbor_rpc_serializer<data_type> cbor;
    BOOST_TEST(json.media_type() != cbor.media_type());
    BOOST_TEST(cbor.media_type() == "application/cbor");

    // And a registry over both really does reach each one.
    const kythira::multi_serializer_registry<kythira::json_rpc_serializer<data_type>,
                                             kythira::cbor_rpc_serializer<data_type>>
        reg;
    const auto types = reg.preferred_media_types();
    BOOST_REQUIRE_EQUAL(types.size(), 2u);
    BOOST_TEST(reg.supports("application/json"));
    BOOST_TEST(reg.supports("application/cbor"));

    const kythira::request_vote_request<> req{9, 8, 7, 6};
    const auto as_json = reg.encode_with("application/json", req);
    const auto as_cbor = reg.encode_with("application/cbor", req);
    // Same message, genuinely different wire bytes — proof the fold dispatched
    // to two different serializers rather than to the same one twice.
    BOOST_TEST(as_json != as_cbor);
    BOOST_TEST(
        reg.decode_with<kythira::request_vote_request<>>("application/json", as_json).term() ==
        req.term());
    BOOST_TEST(
        reg.decode_with<kythira::request_vote_request<>>("application/cbor", as_cbor).term() ==
        req.term());
}

// ── single_serializer_registry ─────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(single_registry_reports_its_one_type) {
    const single_alpha reg;
    BOOST_TEST(reg.default_media_type() == kAlphaMedia);
    BOOST_REQUIRE_EQUAL(reg.preferred_media_types().size(), 1u);
    BOOST_TEST(reg.preferred_media_types()[0] == kAlphaMedia);
    BOOST_TEST(reg.supports(kAlphaMedia));
    BOOST_TEST(!reg.supports(kBetaMedia));
}

/// An absent `Accept` is "no preference", not "accepts nothing" — the common
/// case for a peer that predates negotiation. It must yield the default rather
/// than a 406.
BOOST_AUTO_TEST_CASE(an_empty_accept_list_selects_the_default) {
    const single_alpha single;
    const multi_ab multi;
    BOOST_TEST(single.select_output_media_type({}).value() == kAlphaMedia);
    BOOST_TEST(multi.select_output_media_type({}).value() == kAlphaMedia);
}

BOOST_AUTO_TEST_CASE(single_registry_selects_only_its_own_type) {
    const single_alpha reg;
    BOOST_TEST(reg.select_output_media_type({kAlphaMedia}).value() == kAlphaMedia);
    BOOST_TEST(reg.select_output_media_type({kBetaMedia, kAlphaMedia}).value() == kAlphaMedia);
    BOOST_TEST(!reg.select_output_media_type({kBetaMedia}).has_value());
    BOOST_TEST(!reg.select_output_media_type({kBetaMedia, kGammaMedia}).has_value());
}

BOOST_AUTO_TEST_CASE(single_registry_round_trips_its_own_type) {
    const single_alpha reg;
    const auto encoded = reg.encode_with(kAlphaMedia, 42);
    BOOST_REQUIRE_EQUAL(encoded.size(), 2u);
    BOOST_TEST(std::to_integer<int>(encoded[0]) == static_cast<int>('A'));
    BOOST_TEST(reg.decode_with<int>(kAlphaMedia, encoded) == 42);
}

BOOST_AUTO_TEST_CASE(single_registry_rejects_a_foreign_type_on_both_paths) {
    const single_alpha reg;
    BOOST_CHECK_THROW(reg.encode_with(kBetaMedia, 42), kythira::unsupported_media_type_error);
    const auto encoded = reg.encode_with(kAlphaMedia, 42);
    BOOST_CHECK_THROW(reg.decode_with<int>(kBetaMedia, encoded),
                      kythira::unsupported_media_type_error);
}

// ── multi_serializer_registry ──────────────────────────────────────────────

/// Declaration order is preference order, because it becomes the `Accept`
/// header this side sends. It must not be sorted or otherwise normalised.
BOOST_AUTO_TEST_CASE(multi_registry_preserves_declaration_order) {
    const multi_ab ab;
    const multi_ba ba;
    BOOST_TEST(ab.default_media_type() == kAlphaMedia);
    BOOST_TEST(ba.default_media_type() == kBetaMedia);

    const auto ab_types = ab.preferred_media_types();
    BOOST_REQUIRE_EQUAL(ab_types.size(), 2u);
    BOOST_TEST(ab_types[0] == kAlphaMedia);
    BOOST_TEST(ab_types[1] == kBetaMedia);

    const auto ba_types = ba.preferred_media_types();
    BOOST_REQUIRE_EQUAL(ba_types.size(), 2u);
    BOOST_TEST(ba_types[0] == kBetaMedia);
    BOOST_TEST(ba_types[1] == kAlphaMedia);
}

BOOST_AUTO_TEST_CASE(multi_registry_supports_every_registered_type) {
    const multi_ab reg;
    BOOST_TEST(reg.supports(kAlphaMedia));
    BOOST_TEST(reg.supports(kBetaMedia));
    BOOST_TEST(!reg.supports(kGammaMedia));
}

/// The peer's ranking wins, not ours. Both registries below support both types
/// and differ only in their own declaration order; each must still honour the
/// order the peer sent, or the negotiated type would depend on which side asked.
BOOST_AUTO_TEST_CASE(the_peers_accept_order_decides_the_output_type) {
    const multi_ab ab;
    const multi_ba ba;
    BOOST_TEST(ab.select_output_media_type({kBetaMedia, kAlphaMedia}).value() == kBetaMedia);
    BOOST_TEST(ba.select_output_media_type({kAlphaMedia, kBetaMedia}).value() == kAlphaMedia);
}

BOOST_AUTO_TEST_CASE(unknown_types_are_skipped_and_a_disjoint_list_fails) {
    const multi_ab reg;
    // Overlap after an unsupported entry: the unsupported one is skipped.
    BOOST_TEST(reg.select_output_media_type({kGammaMedia, kBetaMedia}).value() == kBetaMedia);
    // No overlap at all: nullopt, which the transports render as 406/4.06.
    BOOST_TEST(!reg.select_output_media_type({kGammaMedia}).has_value());
}

/// The dispatch property: the bytes must come from the serializer that claims
/// the requested media type, not merely from some serializer in the tuple.
BOOST_AUTO_TEST_CASE(multi_registry_dispatches_to_the_named_serializer) {
    const multi_ab reg;

    const auto as_alpha = reg.encode_with(kAlphaMedia, 7);
    BOOST_REQUIRE_EQUAL(as_alpha.size(), 2u);
    BOOST_TEST(std::to_integer<int>(as_alpha[0]) == static_cast<int>('A'));

    const auto as_beta = reg.encode_with(kBetaMedia, 7);
    BOOST_REQUIRE_EQUAL(as_beta.size(), 2u);
    BOOST_TEST(std::to_integer<int>(as_beta[0]) == static_cast<int>('B'));

    BOOST_TEST(reg.decode_with<int>(kAlphaMedia, as_alpha) == 7);
    BOOST_TEST(reg.decode_with<int>(kBetaMedia, as_beta) == 7);
}

/// Decoding alpha bytes as beta must fail rather than return something
/// plausible — the tagged serializer checks its own tag precisely so a
/// mis-dispatch cannot pass silently.
BOOST_AUTO_TEST_CASE(decoding_under_the_wrong_registered_type_fails_loudly) {
    const multi_ab reg;
    const auto as_alpha = reg.encode_with(kAlphaMedia, 7);
    BOOST_CHECK_THROW(reg.decode_with<int>(kBetaMedia, as_alpha), kythira::serialization_error);
}

BOOST_AUTO_TEST_CASE(multi_registry_rejects_an_unregistered_type_on_both_paths) {
    const multi_ab reg;
    BOOST_CHECK_THROW(reg.encode_with(kGammaMedia, 7), kythira::unsupported_media_type_error);
    const auto encoded = reg.encode_with(kAlphaMedia, 7);
    BOOST_CHECK_THROW(reg.decode_with<int>(kGammaMedia, encoded),
                      kythira::unsupported_media_type_error);
}

/// `unsupported_media_type_error` carries the offending type so a transport can
/// build its 415/406 response without re-parsing the header it just read.
BOOST_AUTO_TEST_CASE(the_error_carries_the_offending_media_type) {
    const multi_ab reg;
    try {
        (void)reg.encode_with(kGammaMedia, 7);
        BOOST_FAIL("expected unsupported_media_type_error");
    } catch (const kythira::unsupported_media_type_error& ex) {
        BOOST_TEST(ex.media_type() == kGammaMedia);
        BOOST_TEST(std::string{ex.what()}.find(kGammaMedia) != std::string::npos);
    }
}

// ── Real serializer, end to end ────────────────────────────────────────────

/// The tagged serializers above prove dispatch; this proves a registry is
/// actually usable with a real wire format and real RPC types.
BOOST_AUTO_TEST_CASE(a_registry_round_trips_a_real_rpc_through_json) {
    const kythira::single_serializer_registry<kythira::json_rpc_serializer<data_type>> reg;
    const kythira::request_vote_request<> req{1, 2, 3, 4};

    const auto encoded = reg.encode_with("application/json", req);
    BOOST_TEST(!encoded.empty());

    const auto decoded =
        reg.decode_with<kythira::request_vote_request<>>("application/json", encoded);
    BOOST_TEST(decoded.term() == req.term());
    BOOST_TEST(decoded.candidate_id() == req.candidate_id());
    BOOST_TEST(decoded.last_log_index() == req.last_log_index());
    BOOST_TEST(decoded.last_log_term() == req.last_log_term());
}

// ── Accept-header parsing ──────────────────────────────────────────────────
//
// Shared by both HTTP servers (Task 8), so tested once here rather than twice
// per transport. A `q`-value bug fixed in one server's copy but not the other's
// would be invisible until a peer happened to use the unfixed transport.

BOOST_AUTO_TEST_SUITE(accept_header_parsing)

BOOST_AUTO_TEST_CASE(splits_on_commas_and_trims_whitespace) {
    const auto types = kythira::parse_accept_header("application/cbor, application/json");
    BOOST_REQUIRE_EQUAL(types.size(), 2u);
    BOOST_TEST(types[0] == "application/cbor");
    BOOST_TEST(types[1] == "application/json");
}

BOOST_AUTO_TEST_CASE(strips_parameters_including_q_values) {
    const auto types = kythira::parse_accept_header(
        "application/cbor;q=0.9, application/json;q=1.0;charset=utf-8");
    BOOST_REQUIRE_EQUAL(types.size(), 2u);
    BOOST_TEST(types[0] == "application/cbor");
    BOOST_TEST(types[1] == "application/json");
}

/// Pinning the documented limitation rather than the RFC: source order is kept,
/// `q` does not reorder. If that is ever changed, this test is the thing that
/// makes the change visible instead of silent.
BOOST_AUTO_TEST_CASE(q_values_do_not_reorder_the_result) {
    const auto types =
        kythira::parse_accept_header("application/cbor;q=0.1, application/json;q=0.9");
    BOOST_REQUIRE_EQUAL(types.size(), 2u);
    BOOST_TEST(types[0] == "application/cbor");  // despite the lower q
}

BOOST_AUTO_TEST_CASE(an_empty_header_yields_no_types) {
    BOOST_TEST(kythira::parse_accept_header("").empty());
    BOOST_TEST(kythira::parse_accept_header("   ").empty());
    // Which is what makes a blank Accept behave as "no preference": the empty
    // vector is exactly what select_output_media_type treats as the default.
    const single_alpha reg;
    BOOST_TEST(reg.select_output_media_type(kythira::parse_accept_header("")).value() ==
               kAlphaMedia);
}

/// A malformed header must degrade to the types it did carry, never to an empty
/// media type that can silently match nothing.
BOOST_AUTO_TEST_CASE(malformed_entries_are_skipped_not_emitted_as_empty) {
    const auto types = kythira::parse_accept_header("application/json,,  ,application/cbor,");
    BOOST_REQUIRE_EQUAL(types.size(), 2u);
    BOOST_TEST(types[0] == "application/json");
    BOOST_TEST(types[1] == "application/cbor");
}

BOOST_AUTO_TEST_CASE(a_single_type_needs_no_comma) {
    const auto types = kythira::parse_accept_header("application/json");
    BOOST_REQUIRE_EQUAL(types.size(), 1u);
    BOOST_TEST(types[0] == "application/json");
}

/// Wildcards pass through the parser verbatim; the registry is what interprets
/// them. `*/*` is what curl and many HTTP client libraries send by default, so
/// it must yield a type rather than a 406.
BOOST_AUTO_TEST_CASE(a_wildcard_passes_through_the_parser_verbatim) {
    const auto types = kythira::parse_accept_header("*/*");
    BOOST_REQUIRE_EQUAL(types.size(), 1u);
    BOOST_TEST(types[0] == "*/*");
    const single_alpha reg;
    BOOST_TEST(reg.select_output_media_type(types).value() == kAlphaMedia);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Accept wildcards ───────────────────────────────────────────────────────
//
// `Accept: */*` is the default header curl and many HTTP client libraries send.
// An earlier revision of this suite asserted that it matched nothing, which
// would have made every such request answer 406 Not Acceptable — a
// self-inflicted interop failure against the most common header value there is.

BOOST_AUTO_TEST_SUITE(accept_wildcards)

BOOST_AUTO_TEST_CASE(match_all_yields_the_default_not_a_406) {
    const single_alpha single;
    const multi_ab multi;
    BOOST_TEST(single.select_output_media_type({"*/*"}).value() == kAlphaMedia);
    BOOST_TEST(multi.select_output_media_type({"*/*"}).value() == kAlphaMedia);
    // Real-world shape: curl's default, verbatim.
    BOOST_TEST(multi.select_output_media_type(kythira::parse_accept_header("*/*")).value() ==
               kAlphaMedia);
}

BOOST_AUTO_TEST_CASE(a_subtype_wildcard_matches_within_its_top_level_type) {
    const multi_ab reg;  // both types are application/x-test-*
    BOOST_TEST(reg.select_output_media_type({"application/*"}).value() == kAlphaMedia);
    // A top-level type we serve nothing for still fails, rather than matching
    // by accident.
    BOOST_TEST(!reg.select_output_media_type({"text/*"}).has_value());
    BOOST_TEST(!reg.select_output_media_type({"image/*"}).has_value());
}

/// An exact entry earlier in the peer's list beats a later wildcard: the peer
/// ranked them, and honouring the wildcard first would discard that ranking.
BOOST_AUTO_TEST_CASE(an_earlier_exact_entry_beats_a_later_wildcard) {
    const multi_ab reg;  // our own order is alpha, then beta
    BOOST_TEST(reg.select_output_media_type({kBetaMedia, "*/*"}).value() == kBetaMedia);
}

/// Within a single wildcard entry the peer expressed no preference among the
/// types it covers, so our declaration order breaks the tie.
BOOST_AUTO_TEST_CASE(within_one_wildcard_our_declaration_order_breaks_the_tie) {
    const multi_ab ab;
    const multi_ba ba;
    BOOST_TEST(ab.select_output_media_type({"application/*"}).value() == kAlphaMedia);
    BOOST_TEST(ba.select_output_media_type({"application/*"}).value() == kBetaMedia);
}

/// A wildcard must not be treated as a literal media type by encode/decode —
/// those still require a concrete, negotiated token.
BOOST_AUTO_TEST_CASE(encode_still_rejects_a_wildcard_as_a_literal_type) {
    const multi_ab reg;
    BOOST_CHECK_THROW(reg.encode_with("*/*", 7), kythira::unsupported_media_type_error);
}

BOOST_AUTO_TEST_SUITE_END()
