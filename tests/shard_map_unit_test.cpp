// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file shard_map_unit_test.cpp
/// @brief Unit tests for `kythira::shard_map` (task 2).
///
/// `check_tiling()` is the executable form of the design's most important
/// invariant, so it gets tested from both sides: it must stay quiet through a
/// legitimate split-then-merge round trip, and it must name the offending
/// bound on a hand-constructed gap and a hand-constructed overlap. A tiling
/// check that only ever returns `nullopt` would pass every property test in
/// the suite while catching nothing.

#define BOOST_TEST_MODULE shard_map_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/shard_map.hpp>

#include <cstdint>
#include <string>

using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_map;
using kythira::shard_range;
using kythira::unbounded_shard_range;

namespace {

using key_type = std::string;
using group_type = std::uint64_t;
using node_type = std::uint64_t;
using map_type = shard_map<group_type, key_type, node_type>;
using desc_type = shard_descriptor<group_type, key_type, node_type>;

auto range_of(std::optional<key_type> start, std::optional<key_type> end) -> shard_range<key_type> {
    return shard_range<key_type>{._start = std::move(start), ._end = std::move(end)};
}

auto desc(group_type g, std::optional<key_type> start, std::optional<key_type> end,
          std::uint64_t version = 0, std::vector<node_type> voters = {1, 2, 3}) -> desc_type {
    return desc_type{._group_id = g,
                     ._range = range_of(std::move(start), std::move(end)),
                     ._epoch = shard_epoch{._version = version, ._conf_version = 0},
                     ._voters = std::move(voters),
                     ._learners = {},
                     ._leader_hint = std::nullopt};
}

/// The three-way tiling a batch split of `(-inf,+inf)` at {"f","m"} produces.
/// The parent id is reused by the leftmost (derived) child, exactly as
/// `split_command::_right_derive == false` would produce.
auto split_three_ways() -> map_type {
    map_type m = map_type::single_shard(1, {1, 2, 3});
    m.apply_split(
        desc(1, std::nullopt, std::nullopt, 0),
        {desc(1, std::nullopt, key_type{"f"}, 2), desc(2, key_type{"f"}, key_type{"m"}, 2),
         desc(3, key_type{"m"}, std::nullopt, 2)});
    return m;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(shard_map_unit)

// ── tiling through the legitimate transitions ────────────────────────────────

BOOST_AUTO_TEST_CASE(a_single_unbounded_shard_tiles) {
    const auto m = map_type::single_shard(1, {1, 2, 3});
    BOOST_CHECK_EQUAL(m.size(), 1u);
    BOOST_CHECK(!m.check_tiling().has_value());
}

BOOST_AUTO_TEST_CASE(an_empty_map_tiles_vacuously) {
    const map_type m;
    BOOST_CHECK(m.empty());
    BOOST_CHECK(!m.check_tiling().has_value());
}

BOOST_AUTO_TEST_CASE(a_three_way_split_tiles) {
    const auto m = split_three_ways();
    BOOST_CHECK_EQUAL(m.size(), 3u);
    if (auto problem = m.check_tiling(); problem.has_value()) {
        BOOST_FAIL("split broke the tiling: " << *problem);
    }
}

BOOST_AUTO_TEST_CASE(merging_the_split_back_tiles) {
    auto m = split_three_ways();
    // Merge group 3 (m,+inf) into group 2 [f,m): the survivor takes the union
    // and version = max(2,2) + 1.
    m.apply_merge(desc(3, key_type{"m"}, std::nullopt, 2), desc(2, key_type{"f"}, std::nullopt, 3));
    BOOST_CHECK_EQUAL(m.size(), 2u);
    if (auto problem = m.check_tiling(); problem.has_value()) {
        BOOST_FAIL("merge broke the tiling: " << *problem);
    }

    // And all the way back to one shard.
    m.apply_merge(desc(2, key_type{"f"}, std::nullopt, 3), desc(1, std::nullopt, std::nullopt, 4));
    BOOST_CHECK_EQUAL(m.size(), 1u);
    BOOST_CHECK(!m.check_tiling().has_value());
}

// ── tiling failures are detected and localised ───────────────────────────────

BOOST_AUTO_TEST_CASE(a_gap_is_detected_and_names_both_sides) {
    map_type m;
    m.upsert(desc(1, std::nullopt, key_type{"f"}, 1));
    m.upsert(desc(2, key_type{"m"}, std::nullopt, 1));  // nothing owns [f, m)

    const auto problem = m.check_tiling();
    BOOST_REQUIRE(problem.has_value());
    BOOST_CHECK(problem->find("gap") != std::string::npos);
    BOOST_CHECK(problem->find("f") != std::string::npos);
    BOOST_CHECK(problem->find("m") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(an_overlap_is_detected_and_names_both_sides) {
    map_type m;
    // Same version, so upsert's supersede pass leaves both rows in place and
    // check_tiling() is the thing that has to notice.
    m.upsert(desc(1, std::nullopt, key_type{"m"}, 1));
    m.upsert(desc(2, key_type{"f"}, std::nullopt, 1));  // [f, m) is owned twice

    const auto problem = m.check_tiling();
    BOOST_REQUIRE(problem.has_value());
    BOOST_CHECK(problem->find("overlap") != std::string::npos);
    BOOST_CHECK(problem->find("f") != std::string::npos);
    BOOST_CHECK(problem->find("m") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(an_uncovered_low_end_is_detected) {
    map_type m;
    m.upsert(desc(1, key_type{"f"}, std::nullopt, 1));
    const auto problem = m.check_tiling();
    BOOST_REQUIRE(problem.has_value());
    BOOST_CHECK(problem->find("below") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(an_uncovered_high_end_is_detected) {
    map_type m;
    m.upsert(desc(1, std::nullopt, key_type{"f"}, 1));
    const auto problem = m.check_tiling();
    BOOST_REQUIRE(problem.has_value());
    BOOST_CHECK(problem->find("at or above") != std::string::npos);
}

// ── lookup ───────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(lookup_resolves_every_key_of_a_tiled_map) {
    const auto m = split_three_ways();
    BOOST_CHECK_EQUAL(m.lookup("")->_group_id, 1u);
    BOOST_CHECK_EQUAL(m.lookup("a")->_group_id, 1u);
    BOOST_CHECK_EQUAL(m.lookup("f")->_group_id, 2u);  // half-open: start belongs to the right
    BOOST_CHECK_EQUAL(m.lookup("g")->_group_id, 2u);
    BOOST_CHECK_EQUAL(m.lookup("m")->_group_id, 3u);
    BOOST_CHECK_EQUAL(m.lookup("zzz")->_group_id, 3u);
}

BOOST_AUTO_TEST_CASE(lookup_into_a_gap_returns_nullopt_rather_than_the_row_to_its_left) {
    // Stepping back from upper_bound lands on the row before the gap; without
    // the contains() re-check this would route a command to a shard that does
    // not own the key.
    map_type m;
    m.upsert(desc(1, std::nullopt, key_type{"f"}, 1));
    m.upsert(desc(2, key_type{"m"}, std::nullopt, 1));
    BOOST_CHECK(!m.lookup("h").has_value());
    BOOST_CHECK_EQUAL(m.lookup("a")->_group_id, 1u);
    BOOST_CHECK_EQUAL(m.lookup("z")->_group_id, 2u);
}

BOOST_AUTO_TEST_CASE(lookup_below_the_lowest_row_returns_nullopt) {
    map_type m;
    m.upsert(desc(1, key_type{"f"}, std::nullopt, 1));
    BOOST_CHECK(!m.lookup("a").has_value());
    BOOST_CHECK_EQUAL(m.lookup("f")->_group_id, 1u);
}

// ── range_scan and siblings ──────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(range_scan_returns_every_overlapping_row_in_key_order) {
    const auto m = split_three_ways();

    auto all = m.range_scan(unbounded_shard_range<key_type>());
    BOOST_REQUIRE_EQUAL(all.size(), 3u);
    BOOST_CHECK_EQUAL(all[0]._group_id, 1u);
    BOOST_CHECK_EQUAL(all[1]._group_id, 2u);
    BOOST_CHECK_EQUAL(all[2]._group_id, 3u);

    auto middle = m.range_scan(range_of(key_type{"g"}, key_type{"n"}));
    BOOST_REQUIRE_EQUAL(middle.size(), 2u);
    BOOST_CHECK_EQUAL(middle[0]._group_id, 2u);
    BOOST_CHECK_EQUAL(middle[1]._group_id, 3u);

    // A range that ends exactly where a shard starts does not touch it.
    auto touching = m.range_scan(range_of(key_type{"a"}, key_type{"f"}));
    BOOST_REQUIRE_EQUAL(touching.size(), 1u);
    BOOST_CHECK_EQUAL(touching[0]._group_id, 1u);
}

BOOST_AUTO_TEST_CASE(siblings_are_reported_only_when_adjacent) {
    const auto m = split_three_ways();
    BOOST_CHECK(!m.left_sibling(1).has_value());
    BOOST_CHECK_EQUAL(m.right_sibling(1)->_group_id, 2u);
    BOOST_CHECK_EQUAL(m.left_sibling(2)->_group_id, 1u);
    BOOST_CHECK_EQUAL(m.right_sibling(2)->_group_id, 3u);
    BOOST_CHECK_EQUAL(m.left_sibling(3)->_group_id, 2u);
    BOOST_CHECK(!m.right_sibling(3).has_value());
}

// ── upsert ───────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(upsert_of_a_stale_epoch_row_is_a_no_op) {
    map_type m = map_type::single_shard(1, {1, 2, 3});
    BOOST_CHECK(m.upsert(desc(1, std::nullopt, std::nullopt, 5)));
    BOOST_CHECK_EQUAL(m.find(1)->_epoch._version, 5u);

    // Same epoch: no-op.
    BOOST_CHECK(!m.upsert(desc(1, std::nullopt, std::nullopt, 5)));
    // Older epoch: no-op, and the newer row survives.
    BOOST_CHECK(!m.upsert(desc(1, std::nullopt, std::nullopt, 2)));
    BOOST_CHECK_EQUAL(m.find(1)->_epoch._version, 5u);
    BOOST_CHECK_EQUAL(m.size(), 1u);
}

BOOST_AUTO_TEST_CASE(upsert_of_children_evicts_the_stale_parent_row) {
    // This is the client repair path: a stale map holding only the parent
    // learns the children from an epoch-mismatch rejection.
    map_type m = map_type::single_shard(1, {1, 2, 3});
    BOOST_CHECK(!m.check_tiling().has_value());

    BOOST_CHECK(m.upsert_all(
        {desc(1, std::nullopt, key_type{"m"}, 2), desc(2, key_type{"m"}, std::nullopt, 2)}));
    BOOST_CHECK_EQUAL(m.size(), 2u);
    if (auto problem = m.check_tiling(); problem.has_value()) {
        BOOST_FAIL("repairing the map from a rejection broke the tiling: " << *problem);
    }
}

BOOST_AUTO_TEST_CASE(upsert_of_a_survivor_evicts_the_stale_source_row) {
    auto m = split_three_ways();
    // Group 2 absorbed group 3; the survivor's version exceeds both inputs.
    BOOST_CHECK(m.upsert(desc(2, key_type{"f"}, std::nullopt, 3)));
    BOOST_CHECK_EQUAL(m.size(), 2u);
    BOOST_CHECK(!m.find(3).has_value());
    if (auto problem = m.check_tiling(); problem.has_value()) {
        BOOST_FAIL("absorbing the source row broke the tiling: " << *problem);
    }
}

BOOST_AUTO_TEST_CASE(upsert_moves_a_row_when_its_start_bound_changes) {
    map_type m;
    m.upsert(desc(1, std::nullopt, key_type{"f"}, 1));
    m.upsert(desc(2, key_type{"f"}, std::nullopt, 1));
    // Group 2 absorbs group 1 leftwards: its start bound moves to -inf.
    BOOST_CHECK(m.upsert(desc(2, std::nullopt, std::nullopt, 2)));
    BOOST_CHECK_EQUAL(m.size(), 1u);
    BOOST_CHECK_EQUAL(m.lookup("a")->_group_id, 2u);
    BOOST_CHECK(!m.check_tiling().has_value());
}

BOOST_AUTO_TEST_CASE(erase_group_removes_both_the_row_and_the_index_entry) {
    auto m = split_three_ways();
    BOOST_CHECK(m.erase_group(2));
    BOOST_CHECK(!m.find(2).has_value());
    BOOST_CHECK(!m.erase_group(2));
    BOOST_CHECK_EQUAL(m.size(), 2u);
    BOOST_CHECK(m.check_tiling().has_value());  // the erase left a gap, as it must
}

BOOST_AUTO_TEST_SUITE_END()
