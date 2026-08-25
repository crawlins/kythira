// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file shard_types_unit_test.cpp
/// @brief Unit tests for the multi-Raft shard value types (task 1).
///
/// The cases that matter here are the ones where an unbounded end changes the
/// answer. `contains()` on a fully bounded range is arithmetic nobody gets
/// wrong; `contains()` on `(-inf, +inf)` and adjacency across an open bound are
/// where a sign error hides, and where the whole "no reserved sentinel key"
/// property of the design lives or dies.

#define BOOST_TEST_MODULE shard_types_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/shard_types.hpp>

#include <cstdint>
#include <string>

using kythira::compare_end_bound;
using kythira::compare_start_bound;
using kythira::hot_key_sample;
using kythira::is_colocated;
using kythira::merge_decision;
using kythira::merge_direction;
using kythira::merge_reason;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_range;
using kythira::shard_stats;
using kythira::split_decision;
using kythira::split_reason;
using kythira::unbounded_shard_range;

namespace {

using key_type = std::string;
using range_type = shard_range<key_type>;

auto bounded(const char* start, const char* end) -> range_type {
    return range_type{._start = key_type{start}, ._end = key_type{end}};
}
auto from(const char* start) -> range_type {
    return range_type{._start = key_type{start}, ._end = std::nullopt};
}
auto until(const char* end) -> range_type {
    return range_type{._start = std::nullopt, ._end = key_type{end}};
}
auto everything() -> range_type {
    return unbounded_shard_range<key_type>();
}

}  // namespace

BOOST_AUTO_TEST_SUITE(shard_types_unit)

// ── concepts ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_expected_types_satisfy_the_concepts) {
    static_assert(kythira::shard_key<std::string>);
    static_assert(kythira::shard_key<std::uint64_t>);
    static_assert(kythira::raft_group_id<std::uint64_t>);
    static_assert(kythira::raft_group_id<std::string>);
    BOOST_CHECK(true);
}

// ── contains(), across all four boundedness combinations ─────────────────────

BOOST_AUTO_TEST_CASE(contains_on_a_fully_bounded_range) {
    const auto r = bounded("b", "m");
    BOOST_CHECK(!r.contains("a"));
    BOOST_CHECK(r.contains("b"));  // half-open: start is inside
    BOOST_CHECK(r.contains("f"));
    BOOST_CHECK(!r.contains("m"));  // half-open: end is outside
    BOOST_CHECK(!r.contains("z"));
}

BOOST_AUTO_TEST_CASE(contains_on_a_range_unbounded_below) {
    const auto r = until("m");
    BOOST_CHECK(r.contains(""));
    BOOST_CHECK(r.contains("a"));
    BOOST_CHECK(!r.contains("m"));
    BOOST_CHECK(!r.contains("z"));
}

BOOST_AUTO_TEST_CASE(contains_on_a_range_unbounded_above) {
    const auto r = from("m");
    BOOST_CHECK(!r.contains("a"));
    BOOST_CHECK(r.contains("m"));
    BOOST_CHECK(r.contains("zzzzzz"));
}

BOOST_AUTO_TEST_CASE(contains_on_the_unbounded_range_accepts_every_key) {
    const auto r = everything();
    BOOST_CHECK(r.contains(""));
    BOOST_CHECK(r.contains("m"));
    BOOST_CHECK(r.contains("\xff\xff"));
}

// ── adjacency ────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(adjacency_holds_only_when_the_facing_bounds_are_equal) {
    BOOST_CHECK(bounded("a", "m").is_adjacent_left_of(bounded("m", "z")));
    BOOST_CHECK(!bounded("a", "m").is_adjacent_left_of(bounded("n", "z")));
    BOOST_CHECK(!bounded("a", "m").is_adjacent_left_of(bounded("l", "z")));
}

BOOST_AUTO_TEST_CASE(adjacency_is_false_when_either_facing_bound_is_unbounded) {
    // An unbounded end has no key at which the next range could begin.
    BOOST_CHECK(!from("a").is_adjacent_left_of(bounded("m", "z")));
    // An unbounded start has no key at which the previous range could stop.
    BOOST_CHECK(!bounded("a", "m").is_adjacent_left_of(until("z")));
    BOOST_CHECK(!everything().is_adjacent_left_of(everything()));
}

BOOST_AUTO_TEST_CASE(adjacency_composes_across_a_three_way_split) {
    const auto left = until("f");
    const auto middle = bounded("f", "m");
    const auto right = from("m");
    BOOST_CHECK(left.is_adjacent_left_of(middle));
    BOOST_CHECK(middle.is_adjacent_left_of(right));
    BOOST_CHECK(!left.is_adjacent_left_of(right));
}

// ── emptiness and coverage ───────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_range_is_empty_only_when_both_bounds_are_set_and_meet) {
    BOOST_CHECK(bounded("m", "m").is_empty());
    BOOST_CHECK(bounded("z", "a").is_empty());
    BOOST_CHECK(!bounded("a", "z").is_empty());
    BOOST_CHECK(!from("z").is_empty());
    BOOST_CHECK(!until("a").is_empty());
    BOOST_CHECK(!everything().is_empty());
}

BOOST_AUTO_TEST_CASE(the_unbounded_range_covers_every_other_range) {
    BOOST_CHECK(everything().covers(bounded("a", "z")));
    BOOST_CHECK(everything().covers(from("a")));
    BOOST_CHECK(everything().covers(until("z")));
    BOOST_CHECK(everything().covers(everything()));
    BOOST_CHECK(!bounded("a", "z").covers(everything()));
    BOOST_CHECK(bounded("a", "z").covers(bounded("b", "y")));
    BOOST_CHECK(!bounded("b", "y").covers(bounded("a", "z")));
}

// ── bound ordering: the asymmetry between start and end ──────────────────────

BOOST_AUTO_TEST_CASE(an_unbounded_start_sorts_first_and_an_unbounded_end_sorts_last) {
    const std::optional<key_type> open;
    const std::optional<key_type> m{key_type{"m"}};

    BOOST_CHECK(compare_start_bound<key_type>(open, m) < 0);
    BOOST_CHECK(compare_start_bound<key_type>(m, open) > 0);
    BOOST_CHECK(compare_start_bound<key_type>(open, open) == 0);

    BOOST_CHECK(compare_end_bound<key_type>(open, m) > 0);
    BOOST_CHECK(compare_end_bound<key_type>(m, open) < 0);
    BOOST_CHECK(compare_end_bound<key_type>(open, open) == 0);
}

BOOST_AUTO_TEST_CASE(ranges_order_left_to_right_along_the_key_line) {
    // This is the ordering the routing table depends on. If an unbounded end
    // sorted first, `(-inf,+inf)` would come before `(-inf,b)` and every
    // tiling check would report a phantom overlap.
    BOOST_CHECK(until("f") < bounded("f", "m"));
    BOOST_CHECK(bounded("f", "m") < from("m"));
    BOOST_CHECK(until("f") < everything());
    BOOST_CHECK(everything() < from("a"));
    BOOST_CHECK(bounded("a", "m") < bounded("a", "z"));
    BOOST_CHECK(bounded("a", "z") < from("a"));
    BOOST_CHECK(bounded("a", "z") == bounded("a", "z"));
}

// ── epoch ────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(epoch_orders_on_version_first_then_conf_version) {
    // Bound to locals rather than written inline: a brace-init list inside a
    // BOOST_CHECK() argument reads as two macro arguments at the comma.
    const shard_epoch v1_cv9{._version = 1, ._conf_version = 9};
    const shard_epoch v2_cv0{._version = 2, ._conf_version = 0};
    const shard_epoch v2_cv1{._version = 2, ._conf_version = 1};
    const shard_epoch v2_cv1_again{._version = 2, ._conf_version = 1};
    const shard_epoch v0_cv1{._version = 0, ._conf_version = 1};

    BOOST_CHECK(v1_cv9 < v2_cv0);
    BOOST_CHECK(v2_cv0 < v2_cv1);
    BOOST_CHECK(v2_cv1 == v2_cv1_again);
    BOOST_CHECK(shard_epoch{} < v0_cv1);
}

BOOST_AUTO_TEST_CASE(a_default_epoch_is_the_zero_generation) {
    const shard_epoch e;
    BOOST_CHECK_EQUAL(e.version(), 0u);
    BOOST_CHECK_EQUAL(e.conf_version(), 0u);
}

// ── descriptor ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_descriptor_reports_its_membership) {
    using desc_type = shard_descriptor<std::uint64_t, key_type, std::uint64_t>;
    const desc_type d{._group_id = 7,
                      ._range = bounded("a", "m"),
                      ._epoch = shard_epoch{._version = 3, ._conf_version = 1},
                      ._voters = {1, 2, 3},
                      ._learners = {4},
                      ._leader_hint = 2};

    BOOST_CHECK(d.has_voter(2));
    BOOST_CHECK(!d.has_voter(4));
    BOOST_CHECK(d.has_replica(4));
    BOOST_CHECK(!d.has_replica(9));
    BOOST_CHECK_EQUAL(d.leader_hint().value(), 2u);
}

BOOST_AUTO_TEST_CASE(colocation_ignores_member_order) {
    using desc_type = shard_descriptor<std::uint64_t, key_type, std::uint64_t>;
    const desc_type a{._group_id = 1, ._voters = {3, 1, 2}, ._learners = {}};
    const desc_type b{._group_id = 2, ._voters = {1, 2, 3}, ._learners = {}};
    const desc_type c{._group_id = 3, ._voters = {1, 2, 4}, ._learners = {}};
    const desc_type d{._group_id = 4, ._voters = {1, 2, 3}, ._learners = {9}};

    BOOST_CHECK(is_colocated(a, b));
    BOOST_CHECK(!is_colocated(a, c));
    BOOST_CHECK(!is_colocated(b, d));
}

// ── decisions and samples ────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_default_decision_proposes_nothing) {
    const split_decision<key_type> s;
    BOOST_CHECK(!s.should_split());
    BOOST_CHECK(s.at_keys().empty());

    const merge_decision m;
    BOOST_CHECK(!m.should_merge());
    BOOST_CHECK(m.direction() == merge_direction::into_left_sibling);
}

BOOST_AUTO_TEST_CASE(a_split_decision_can_defer_the_key_choice) {
    const split_decision<key_type> s{
        ._split = true, ._at_keys = {}, ._reason = split_reason::write_load};
    BOOST_CHECK(s.should_split());
    BOOST_CHECK(s.at_keys().empty());  // "you choose" — the host falls back to the SM
    BOOST_CHECK(s.reason() == split_reason::write_load);
}

BOOST_AUTO_TEST_CASE(a_balanced_hot_key_sample_reports_a_low_one_sided_fraction) {
    const hot_key_sample<key_type> balanced{
        ._key = "m", ._left_accesses = 500, ._right_accesses = 500};
    BOOST_CHECK_CLOSE(balanced.one_sided_fraction(), 0.5, 1e-9);
    BOOST_CHECK_EQUAL(balanced.total_accesses(), 1000u);

    const hot_key_sample<key_type> lopsided{
        ._key = "m", ._left_accesses = 999, ._right_accesses = 1};
    BOOST_CHECK_CLOSE(lopsided.one_sided_fraction(), 0.999, 1e-6);
}

BOOST_AUTO_TEST_CASE(an_empty_hot_key_sample_reads_as_fully_one_sided) {
    // No observed accesses must not read as "perfectly balanced", or the
    // sampler would propose splits from no evidence at all.
    const hot_key_sample<key_type> none{._key = "m"};
    BOOST_CHECK_CLOSE(none.one_sided_fraction(), 1.0, 1e-9);
}

BOOST_AUTO_TEST_CASE(stats_default_to_size_unavailable) {
    const shard_stats<std::uint64_t, key_type> s;
    BOOST_CHECK(!s.size_available());
    BOOST_CHECK_EQUAL(s._approximate_size_bytes, 0u);
    BOOST_CHECK(s._hot_key_samples.empty());
}

BOOST_AUTO_TEST_SUITE_END()
