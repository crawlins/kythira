// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file http_content_negotiation_unit_test.cpp
/// @brief The negotiation pieces the HTTP transports share
///        (`.kiro/specs/transport-multi-serializer/`, Tasks 9 and 10):
///        `format_accept_header`, `strip_media_type_parameters`, and
///        `peer_capability_cache`.
///
/// Links no transport. `cpp_httplib_server` and `boost_beast_server` differ
/// only in how they *obtain* a header string; what the string means is decided
/// here, once, so the two cannot disagree about it. The transports' own suites
/// cover the wiring; this covers the rules.

#include <raft/http_content_negotiation.hpp>
#include <raft/peer_capability_cache.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/cbor_serializer.hpp>
#include <raft/json_serializer.hpp>

#define BOOST_TEST_MODULE http_content_negotiation_unit_test
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

namespace {
using data_type = std::vector<std::byte>;
using json_serializer = kythira::json_rpc_serializer<data_type>;
using cbor_serializer = kythira::cbor_rpc_serializer<data_type>;

/// A serializer byte-identical to JSON that announces a different media type.
///
/// The `Accept-Post` cases need a registry with **three** types before the
/// peer's choice can differ from our own next preference — with two, the second
/// is the only candidate and an informed jump is indistinguishable from a blind
/// walk. A third *codec* would add nothing a third *label* does not here, since
/// the policy compares media-type strings and never encodes anything, and using
/// a real one would tie these cases to whichever optional serializer happened to
/// be installed.
struct alt_json_serializer : json_serializer {
    [[nodiscard]] auto media_type() const -> std::string { return "application/x-alt-json"; }
    [[nodiscard]] auto name() const -> std::string { return "alt-json"; }
};
}  // namespace

BOOST_AUTO_TEST_SUITE(format_accept_header_tests)

BOOST_AUTO_TEST_CASE(joins_in_preference_order_without_q_values) {
    BOOST_CHECK_EQUAL(kythira::format_accept_header({"application/cbor", "application/json"}),
                      "application/cbor, application/json");
}

BOOST_AUTO_TEST_CASE(a_single_type_has_no_separator) {
    BOOST_CHECK_EQUAL(kythira::format_accept_header({"application/json"}), "application/json");
}

/// An empty list must yield an empty string, not `""` wrapped in separators —
/// the clients test for emptiness to decide whether to send the header at all.
/// Sending `Accept:` with an empty value means "accept nothing" to a strict
/// peer, which earns a 406 for no reason.
BOOST_AUTO_TEST_CASE(an_empty_list_yields_an_empty_string) {
    BOOST_CHECK_EQUAL(kythira::format_accept_header({}), "");
}

/// The round trip both transports actually perform: what one side formats, the
/// other must parse back identically. Asserted rather than assumed, because
/// these are two separate functions that could drift apart.
BOOST_AUTO_TEST_CASE(round_trips_through_parse_accept_header) {
    const std::vector<std::string> types{"application/cbor", "application/json",
                                         "application/x-amzn-ion"};
    BOOST_TEST(kythira::parse_accept_header(kythira::format_accept_header(types)) == types,
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(strip_media_type_parameters_tests)

BOOST_AUTO_TEST_CASE(a_bare_media_type_is_unchanged) {
    BOOST_CHECK_EQUAL(kythira::strip_media_type_parameters("application/json"), "application/json");
}

/// The case that matters for interop: a peer is entitled to send a charset, and
/// failing to strip it turns an ordinary spec-legal request into a 415.
BOOST_AUTO_TEST_CASE(parameters_are_stripped) {
    BOOST_CHECK_EQUAL(kythira::strip_media_type_parameters("application/json; charset=utf-8"),
                      "application/json");
    BOOST_CHECK_EQUAL(kythira::strip_media_type_parameters("application/json;charset=utf-8"),
                      "application/json");
}

BOOST_AUTO_TEST_CASE(surrounding_whitespace_is_trimmed) {
    BOOST_CHECK_EQUAL(kythira::strip_media_type_parameters("  application/cbor  "),
                      "application/cbor");
}

/// A header that is entirely empty or entirely parameters yields an empty
/// string, which both transports treat as "absent" and fall back to their
/// default. That is deliberately the same outcome as no header at all: a peer
/// sending `Content-Type: ; charset=utf-8` has told us nothing, and guessing is
/// better than 415-ing it.
BOOST_AUTO_TEST_CASE(a_degenerate_value_yields_empty) {
    BOOST_CHECK_EQUAL(kythira::strip_media_type_parameters(""), "");
    BOOST_CHECK_EQUAL(kythira::strip_media_type_parameters("   "), "");
    BOOST_CHECK_EQUAL(kythira::strip_media_type_parameters("; charset=utf-8"), "");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(peer_capability_cache_tests)

BOOST_AUTO_TEST_CASE(an_unknown_peer_has_no_entry) {
    const kythira::peer_capability_cache<std::uint64_t> cache;
    BOOST_CHECK(!cache.get(42).has_value());
    BOOST_CHECK_EQUAL(cache.size(), 0U);
}

BOOST_AUTO_TEST_CASE(a_recorded_type_is_returned_and_overwritten_in_place) {
    kythira::peer_capability_cache<std::uint64_t> cache;
    cache.record(42, "application/cbor");
    BOOST_CHECK_EQUAL(cache.get(42).value(), "application/cbor");

    cache.record(42, "application/json");
    BOOST_CHECK_EQUAL(cache.get(42).value(), "application/json");
    BOOST_CHECK_EQUAL(cache.size(), 1U);  // corrected, not accumulated
}

BOOST_AUTO_TEST_CASE(peers_do_not_share_entries) {
    kythira::peer_capability_cache<std::uint64_t> cache;
    cache.record(1, "application/json");
    cache.record(2, "application/cbor");
    BOOST_CHECK_EQUAL(cache.get(1).value(), "application/json");
    BOOST_CHECK_EQUAL(cache.get(2).value(), "application/cbor");
}

BOOST_AUTO_TEST_CASE(forget_removes_only_the_named_peer) {
    kythira::peer_capability_cache<std::uint64_t> cache;
    cache.record(1, "application/json");
    cache.record(2, "application/cbor");
    cache.forget(1);
    BOOST_CHECK(!cache.get(1).has_value());
    BOOST_CHECK_EQUAL(cache.get(2).value(), "application/cbor");
}

/// The cache is read on the RPC path from whatever thread is sending, and
/// written from whatever thread completes a response. This does not prove
/// thread-safety — no test can — but it does exercise the lock under real
/// contention, so a missing `lock_guard` shows up as a TSan finding in CI
/// rather than as a rare corruption in production.
BOOST_AUTO_TEST_CASE(concurrent_record_and_get_do_not_race) {
    kythira::peer_capability_cache<std::uint64_t> cache;
    constexpr int iterations = 2000;

    std::thread writer([&] {
        for (int i = 0; i < iterations; ++i) {
            cache.record(1, i % 2 == 0 ? "application/json" : "application/cbor");
        }
    });
    std::thread reader([&] {
        for (int i = 0; i < iterations; ++i) {
            if (const auto v = cache.get(1)) {
                // Whatever we read must be one of the two values ever written —
                // never a torn string.
                BOOST_TEST((*v == "application/json" || *v == "application/cbor"));
            }
        }
    });
    writer.join();
    reader.join();
    BOOST_CHECK_EQUAL(cache.size(), 1U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(select_request_media_type_tests)

BOOST_AUTO_TEST_CASE(with_no_cache_entry_the_default_is_used) {
    const kythira::single_serializer_registry<json_serializer> registry;
    const kythira::peer_capability_cache<std::uint64_t> cache;
    BOOST_CHECK_EQUAL(kythira::select_request_media_type(registry, cache, 7U), "application/json");
}

BOOST_AUTO_TEST_CASE(a_supported_cached_type_wins) {
    const kythira::single_serializer_registry<json_serializer> registry;
    kythira::peer_capability_cache<std::uint64_t> cache;
    cache.record(7, "application/json");
    BOOST_CHECK_EQUAL(kythira::select_request_media_type(registry, cache, 7U), "application/json");
}

/// The case the `registry.supports(...)` guard exists for. A registry can be
/// reconfigured between calls, and honouring a cached type this client can no
/// longer decode would break the *response* leg — we would ask for something we
/// cannot read. Falling back to the default is always safe because the default
/// is by construction one we speak.
BOOST_AUTO_TEST_CASE(an_unsupported_cached_type_falls_back_to_the_default) {
    const kythira::single_serializer_registry<json_serializer> registry;
    kythira::peer_capability_cache<std::uint64_t> cache;
    cache.record(7, "application/x-no-longer-configured");
    BOOST_CHECK_EQUAL(kythira::select_request_media_type(registry, cache, 7U), "application/json");
}

BOOST_AUTO_TEST_SUITE_END()

/// The retry policy behind Requirement 7.3. The transports own their own retry
/// loops — four different async shapes — but they all ask this one function what
/// to try next, so the rule itself is pinned once, here, without a socket.
BOOST_AUTO_TEST_SUITE(next_request_media_type_after_rejection_tests)

using multi_json_cbor = kythira::multi_serializer_registry<json_serializer, cbor_serializer>;

/// A single-serializer registry has nothing to fall back to: its one type is
/// always the one just refused. This is every configuration shipping today, so
/// it is the case that decides whether the retry costs anything at all — and it
/// must be `nullopt` on the *first* rejection, not after a wasted re-send.
BOOST_AUTO_TEST_CASE(a_single_serializer_registry_has_no_alternative) {
    const kythira::single_serializer_registry<json_serializer> registry;
    BOOST_CHECK(!kythira::next_request_media_type_after_rejection(registry, {"application/json"}));
}

BOOST_AUTO_TEST_CASE(the_next_unrefused_type_is_offered_in_preference_order) {
    const multi_json_cbor registry;
    const auto next =
        kythira::next_request_media_type_after_rejection(registry, {"application/json"});
    BOOST_REQUIRE(next.has_value());
    BOOST_CHECK_EQUAL(*next, "application/cbor");
}

/// Exhaustion. Walking the whole list must terminate rather than cycle — this is
/// what bounds every transport's retry loop, since each appends the result and
/// asks again.
BOOST_AUTO_TEST_CASE(every_type_refused_yields_nothing) {
    const multi_json_cbor registry;
    BOOST_CHECK(!kythira::next_request_media_type_after_rejection(
        registry, {"application/json", "application/cbor"}));
}

/// Termination stated as the property the callers actually rely on: append each
/// answer to the tried list and the sequence ends after one attempt per
/// registered serializer, with no repeats.
BOOST_AUTO_TEST_CASE(appending_each_answer_terminates_without_repeating) {
    const multi_json_cbor registry;
    std::vector<std::string> tried{"application/json"};
    std::size_t iterations = 0;
    while (auto next = kythira::next_request_media_type_after_rejection(registry, tried)) {
        BOOST_REQUIRE(std::find(tried.begin(), tried.end(), *next) == tried.end());
        tried.push_back(*next);
        BOOST_REQUIRE_LT(++iterations, 10U);  // a cycle would blow this, not hang the suite
    }
    BOOST_CHECK_EQUAL(tried.size(), 2U);
}

/// An unknown entry in the tried list is ignored rather than confusing the scan
/// — it can only come from a registry reconfigured mid-flight, and the answer
/// should still be a type we currently speak.
BOOST_AUTO_TEST_CASE(an_unrecognised_tried_entry_does_not_block_a_real_candidate) {
    const multi_json_cbor registry;
    const auto next =
        kythira::next_request_media_type_after_rejection(registry, {"application/x-gone"});
    BOOST_REQUIRE(next.has_value());
    BOOST_CHECK_EQUAL(*next, "application/json");
}

BOOST_AUTO_TEST_SUITE_END()

/// The `Accept-Post` overload (W3C Linked Data Platform 1.0 §7.1): the same
/// choice, but informed by what the rejecting peer said it would take.
///
/// Every case here is written against the same three-serializer registry,
/// because three is the smallest number that can tell the two policies apart. In
/// `{cbor, json, alt-json}` with cbor just refused, the blind walk always
/// answers `application/json`; an answer of `application/x-alt-json` can only
/// have come from reading the header.
BOOST_AUTO_TEST_SUITE(accept_post_informed_retry_tests)

using multi_three =
    kythira::multi_serializer_registry<cbor_serializer, json_serializer, alt_json_serializer>;

constexpr const char* cbor_media = "application/cbor";
constexpr const char* json_media = "application/json";
constexpr const char* alt_media = "application/x-alt-json";

/// The point of the whole feature: the peer names a type that is *not* our next
/// preference, and we go straight to it instead of spending a round trip on the
/// one it did not ask for.
BOOST_AUTO_TEST_CASE(the_peers_named_type_wins_over_our_own_next_preference) {
    const multi_three registry;
    const auto next =
        kythira::next_request_media_type_after_rejection(registry, {cbor_media}, alt_media);
    BOOST_REQUIRE(next.has_value());
    BOOST_CHECK_EQUAL(*next, alt_media);
    // The control that makes the line above mean something: without the header,
    // the very same call answers differently.
    const auto blind = kythira::next_request_media_type_after_rejection(registry, {cbor_media});
    BOOST_REQUIRE(blind.has_value());
    BOOST_CHECK_EQUAL(*blind, json_media);
}

/// Among several types we both speak, the peer's order decides — it is the side
/// that just refused something, so its stated preference is better information,
/// and either choice would have worked.
BOOST_AUTO_TEST_CASE(the_peers_order_decides_between_two_types_we_both_speak) {
    const multi_three registry;
    const auto next = kythira::next_request_media_type_after_rejection(
        registry, {cbor_media}, std::string(alt_media) + ", " + json_media);
    BOOST_REQUIRE(next.has_value());
    BOOST_CHECK_EQUAL(*next, alt_media);
}

/// A peer may list types we do not have. Those are skipped rather than chosen
/// and then failing to encode.
BOOST_AUTO_TEST_CASE(a_type_we_do_not_support_is_skipped) {
    const multi_three registry;
    const auto next = kythira::next_request_media_type_after_rejection(
        registry, {cbor_media}, std::string("application/x-nonesuch, ") + alt_media);
    BOOST_REQUIRE(next.has_value());
    BOOST_CHECK_EQUAL(*next, alt_media);
}

/// A peer that names the type it just rejected — or one we already tried — must
/// not send us round the same loop again. This is what keeps the retry bounded
/// once the header is in play.
BOOST_AUTO_TEST_CASE(a_type_already_tried_is_skipped) {
    const multi_three registry;
    const auto next = kythira::next_request_media_type_after_rejection(
        registry, {cbor_media, json_media}, std::string(cbor_media) + ", " + json_media);
    BOOST_REQUIRE(next.has_value());
    BOOST_CHECK_EQUAL(*next, alt_media);
}

/// The common case, and the one that must stay free: an unmodified peer sends no
/// such header, and the overload has to be exactly the blind walk.
BOOST_AUTO_TEST_CASE(an_absent_header_is_the_blind_walk) {
    const multi_three registry;
    const auto informed = kythira::next_request_media_type_after_rejection(registry, {cbor_media},
                                                                           std::string_view{});
    const auto blind = kythira::next_request_media_type_after_rejection(registry, {cbor_media});
    BOOST_REQUIRE(informed.has_value());
    BOOST_REQUIRE(blind.has_value());
    BOOST_CHECK_EQUAL(*informed, *blind);
}

/// A header naming nothing we can use falls back to the walk rather than giving
/// up. A peer can be wrong about itself; trusting it must be able to cost a
/// wasted round trip and never a failed exchange.
BOOST_AUTO_TEST_CASE(a_header_naming_nothing_usable_falls_back_to_the_walk) {
    const multi_three registry;
    const auto next = kythira::next_request_media_type_after_rejection(
        registry, {cbor_media}, "application/x-nonesuch, application/x-also-nonesuch");
    BOOST_REQUIRE(next.has_value());
    BOOST_CHECK_EQUAL(*next, json_media);
}

/// A wildcard narrows nothing, so it must not be *treated* as narrowing
/// something. It fails `supports()` and lands on the same fallback.
BOOST_AUTO_TEST_CASE(a_wildcard_falls_back_to_the_walk) {
    const multi_three registry;
    const auto next =
        kythira::next_request_media_type_after_rejection(registry, {cbor_media}, "*/*");
    BOOST_REQUIRE(next.has_value());
    BOOST_CHECK_EQUAL(*next, json_media);
}

/// Exhaustion still terminates. A peer insisting on a type we have already tried
/// cannot resurrect the loop.
BOOST_AUTO_TEST_CASE(exhaustion_still_yields_nothing_with_a_header_present) {
    const multi_three registry;
    BOOST_CHECK(!kythira::next_request_media_type_after_rejection(
        registry, {cbor_media, json_media, alt_media}, std::string(json_media) + ", " + alt_media));
}

/// A single-serializer registry pays nothing even when a peer does send the
/// header: its one type is the one just refused, and no header can change that.
BOOST_AUTO_TEST_CASE(a_single_serializer_registry_still_has_no_alternative) {
    const kythira::single_serializer_registry<json_serializer> registry;
    BOOST_CHECK(!kythira::next_request_media_type_after_rejection(registry, {json_media},
                                                                  "application/cbor"));
}

BOOST_AUTO_TEST_SUITE_END()
