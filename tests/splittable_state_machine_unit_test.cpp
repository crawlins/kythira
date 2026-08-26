// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file splittable_state_machine_unit_test.cpp
/// @brief The `splittable_state_machine` extension and its two laws (task 16 of
///        `.kiro/specs/multi-raft/`).
///
/// The round-trip law — `absorb` is the exact inverse of `split_state` — is the
/// sharpest edge in the whole design. A state machine that violates it produces
/// replicas that diverge *silently*: their logs still match, every Raft
/// invariant still holds, and nothing notices until a client reads a key and
/// gets the wrong answer. This file is the only thing standing between that bug
/// and production, so it checks the law byte-for-byte rather than by comparing
/// key sets.
///
/// The determinism law gets the same treatment for the same reason: it is
/// tested by building the same store through *different insertion orders* and
/// asserting the outputs are identical, because insertion order is exactly what
/// an unordered container would leak.

#define BOOST_TEST_MODULE splittable_state_machine_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/ca_state_machine.hpp>
#include <raft/splittable_state_machine.hpp>
#include <raft/test_state_machine.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using kythira::shard_range;
using kythira::splittable_state_machine;
using kythira::test_key_value_state_machine;

namespace {

using sm_type = test_key_value_state_machine<std::uint64_t>;

auto range_of(std::optional<std::string> start, std::optional<std::string> end)
    -> shard_range<std::string> {
    return shard_range<std::string>{._start = std::move(start), ._end = std::move(end)};
}

/// Apply a PUT, returning the machine for chaining.
auto put(sm_type& sm, const std::string& key, const std::string& value, std::uint64_t index)
    -> void {
    sm.apply(sm_type::make_put_command(key, value), index);
}

auto filled(const std::vector<std::pair<std::string, std::string>>& pairs) -> sm_type {
    sm_type sm;
    std::uint64_t index = 1;
    for (const auto& [k, v] : pairs) {
        put(sm, k, v, index++);
    }
    return sm;
}

auto default_pairs() -> std::vector<std::pair<std::string, std::string>> {
    return {{"alpha", "1"}, {"bravo", "2"},   {"charlie", "3"}, {"delta", "4"},
            {"echo", "5"},  {"foxtrot", "6"}, {"golf", "7"},    {"hotel", "8"}};
}

}  // namespace

BOOST_AUTO_TEST_SUITE(splittable_state_machine_unit)

// ── detection ────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_extension_is_detected_structurally_and_is_not_required) {
    static_assert(splittable_state_machine<sm_type, std::string>);

    // `ca_state_machine` deliberately does NOT implement it, and must keep
    // compiling and running. That is the whole point of an `if constexpr`
    // extension rather than a widened `state_machine` concept.
    static_assert(!splittable_state_machine<raft::testing::ca_state_machine, std::string>);

    // And a state machine that implements only part of it does not accidentally
    // qualify — a half-implemented extension is worse than none, because the
    // host would call the missing half.
    struct half_implemented {
        [[nodiscard]] auto approximate_size_bytes() const -> std::size_t { return 0; }
        [[nodiscard]] auto approximate_key_count() const -> std::size_t { return 0; }
    };
    static_assert(!splittable_state_machine<half_implemented, std::string>);
    BOOST_CHECK(true);
}

// ── sizing ───────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_size_hooks_track_the_store) {
    sm_type sm;
    BOOST_CHECK_EQUAL(sm.approximate_key_count(), 0u);
    const auto empty_size = sm.approximate_size_bytes();

    put(sm, "alpha", "value", 1);
    BOOST_CHECK_EQUAL(sm.approximate_key_count(), 1u);
    BOOST_CHECK_GT(sm.approximate_size_bytes(), empty_size);

    // The figure matches what a snapshot of the same store would occupy, so the
    // number a policy thresholds on is the number the shard actually costs.
    BOOST_CHECK_EQUAL(sm.approximate_size_bytes(), sm.get_state().size());
}

// ── determinism ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_same_contents_produce_the_same_bytes_whatever_the_write_order) {
    // Insertion order is exactly what an unordered container leaks, and the
    // divergence it causes is invisible at the Raft level: the logs match.
    auto pairs = default_pairs();
    const auto reference = filled(pairs).get_state();

    std::mt19937 rng{20260825};
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::shuffle(pairs.begin(), pairs.end(), rng);
        BOOST_CHECK(filled(pairs).get_state() == reference);
    }
}

BOOST_AUTO_TEST_CASE(split_state_is_deterministic_across_write_orders) {
    auto pairs = default_pairs();
    const std::vector<std::string> cuts{"delta"};
    auto reference_sm = filled(pairs);
    const auto reference = reference_sm.split_state(cuts);

    std::mt19937 rng{20260826};
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::shuffle(pairs.begin(), pairs.end(), rng);
        auto sm = filled(pairs);
        BOOST_CHECK(sm.split_state(cuts) == reference);
    }
}

// ── the cut itself ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(split_state_returns_one_more_blob_than_it_was_given_keys) {
    auto sm = filled(default_pairs());
    BOOST_CHECK_EQUAL(sm.split_state({}).size(), 1u);
    BOOST_CHECK_EQUAL(sm.split_state({"delta"}).size(), 2u);
    BOOST_CHECK_EQUAL(sm.split_state({"charlie", "foxtrot"}).size(), 3u);
}

BOOST_AUTO_TEST_CASE(the_cut_is_half_open_at_each_key) {
    auto sm = filled(default_pairs());
    const auto parts = sm.split_state({"delta"});
    BOOST_REQUIRE_EQUAL(parts.size(), 2u);

    sm_type left;
    left.restore_from_snapshot(parts[0], 1);
    sm_type right;
    right.restore_from_snapshot(parts[1], 1);

    // "delta" itself belongs to the RIGHT child: the range is [delta, …).
    BOOST_CHECK(!left.contains("delta"));
    BOOST_CHECK(right.contains("delta"));
    BOOST_CHECK(left.contains("charlie"));
    BOOST_CHECK(!right.contains("charlie"));
    BOOST_CHECK_EQUAL(left.size() + right.size(), default_pairs().size());
}

BOOST_AUTO_TEST_CASE(out_of_order_or_duplicated_cut_keys_cannot_produce_overlapping_children) {
    // A caller's ordering mistake must not silently hand two children the same
    // key — that is double ownership, arriving through the back door.
    auto sm = filled(default_pairs());
    const auto ordered = sm.split_state({"charlie", "foxtrot"});
    const auto jumbled = sm.split_state({"foxtrot", "charlie", "charlie"});
    BOOST_CHECK(ordered == jumbled);
}

BOOST_AUTO_TEST_CASE(a_cut_outside_the_stores_key_range_yields_an_empty_child) {
    auto sm = filled(default_pairs());

    const auto below = sm.split_state({"aaa"});
    BOOST_REQUIRE_EQUAL(below.size(), 2u);
    sm_type below_left;
    below_left.restore_from_snapshot(below[0], 1);
    BOOST_CHECK_EQUAL(below_left.size(), 0u);

    const auto above = sm.split_state({"zzz"});
    BOOST_REQUIRE_EQUAL(above.size(), 2u);
    sm_type above_right;
    above_right.restore_from_snapshot(above[1], 1);
    BOOST_CHECK_EQUAL(above_right.size(), 0u);
}

// ── the round-trip law ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(absorb_is_the_exact_inverse_of_split_state) {
    // Byte-for-byte, not key-set equality: a state machine whose `absorb`
    // reproduced the right *keys* with different bytes would still diverge
    // replicas the moment one of them snapshotted.
    auto original = filled(default_pairs());
    const auto reference = original.get_state();

    const auto parts = original.split_state({"delta"});
    BOOST_REQUIRE_EQUAL(parts.size(), 2u);

    sm_type rejoined;
    rejoined.restore_from_snapshot(parts[0], 1);
    rejoined.absorb(parts[1], range_of(std::string{"delta"}, std::nullopt));

    BOOST_CHECK(rejoined.get_state() == reference);
}

BOOST_AUTO_TEST_CASE(the_round_trip_law_holds_for_every_valid_cut) {
    // Every key in the store is a valid cut, and so is every gap between them.
    // A law that held only at the median would be a coincidence.
    const auto pairs = default_pairs();
    auto original = filled(pairs);
    const auto reference = original.get_state();

    std::vector<std::string> candidates;
    for (const auto& [k, v] : pairs) {
        candidates.push_back(k);
        candidates.push_back(k + "-mid");
    }
    candidates.emplace_back("aaa");
    candidates.emplace_back("zzz");

    for (const auto& cut : candidates) {
        auto sm = filled(pairs);
        const auto parts = sm.split_state({cut});
        BOOST_REQUIRE_EQUAL(parts.size(), 2u);

        sm_type rejoined;
        rejoined.restore_from_snapshot(parts[0], 1);
        rejoined.absorb(parts[1], range_of(cut, std::nullopt));
        BOOST_CHECK_MESSAGE(rejoined.get_state() == reference,
                            "round-trip law broken at cut " << cut);
    }
}

BOOST_AUTO_TEST_CASE(a_batch_split_round_trips_through_repeated_absorbs) {
    // Batch split is the case TiKV RFC 0006 exists for, and rejoining N children
    // is what a chain of merges does.
    const auto pairs = default_pairs();
    auto original = filled(pairs);
    const auto reference = original.get_state();

    const std::vector<std::string> cuts{"charlie", "echo", "golf"};
    const auto parts = original.split_state(cuts);
    BOOST_REQUIRE_EQUAL(parts.size(), cuts.size() + 1);

    sm_type rejoined;
    rejoined.restore_from_snapshot(parts[0], 1);
    for (std::size_t i = 1; i < parts.size(); ++i) {
        const auto start = cuts[i - 1];
        const auto end = i < cuts.size() ? std::optional{cuts[i]} : std::nullopt;
        rejoined.absorb(parts[i], range_of(start, end));
    }
    BOOST_CHECK(rejoined.get_state() == reference);
}

BOOST_AUTO_TEST_CASE(absorbing_an_empty_blob_changes_nothing) {
    // A merge whose source shard is empty is legal and common — it is what a
    // shard that has been fully deleted looks like.
    auto sm = filled(default_pairs());
    const auto before = sm.get_state();
    sm.absorb(sm_type{}.get_state(), range_of(std::string{"zzz"}, std::nullopt));
    BOOST_CHECK(sm.get_state() == before);
    sm.absorb({}, range_of(std::string{"zzz"}, std::nullopt));
    BOOST_CHECK(sm.get_state() == before);
}

// ── the leader-only hooks ────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(suggest_split_keys_returns_usable_cuts_and_never_the_first_key) {
    // Cutting at the very first key produces an empty left child: a legal shard
    // and a useless one.
    auto sm = filled(default_pairs());

    const auto one = sm.suggest_split_keys(1);
    BOOST_REQUIRE_EQUAL(one.size(), 1u);
    BOOST_CHECK_NE(one[0], "alpha");

    const auto three = sm.suggest_split_keys(3);
    BOOST_CHECK_LE(three.size(), 3u);
    BOOST_CHECK(std::is_sorted(three.begin(), three.end()));
    for (const auto& k : three) {
        BOOST_CHECK(sm.contains(k));
        BOOST_CHECK_NE(k, "alpha");
    }

    // And each suggestion really does cut the store in two.
    for (const auto& k : three) {
        auto copy = filled(default_pairs());
        const auto parts = copy.split_state({k});
        sm_type left;
        left.restore_from_snapshot(parts[0], 1);
        sm_type right;
        right.restore_from_snapshot(parts[1], 1);
        BOOST_CHECK_GT(left.size(), 0u);
        BOOST_CHECK_GT(right.size(), 0u);
    }
}

BOOST_AUTO_TEST_CASE(a_store_too_small_to_split_suggests_nothing) {
    sm_type empty;
    BOOST_CHECK(empty.suggest_split_keys(5).empty());

    sm_type one;
    put(one, "alpha", "1", 1);
    BOOST_CHECK(one.suggest_split_keys(5).empty());

    // And asking for zero keys yields zero keys rather than one.
    auto full = filled(default_pairs());
    BOOST_CHECK(full.suggest_split_keys(0).empty());
}

BOOST_AUTO_TEST_CASE(the_veto_is_permissive_by_default_and_honoured_when_set) {
    auto sm = filled(default_pairs());
    BOOST_CHECK(sm.can_split_at("delta"));
    BOOST_CHECK(sm.can_split_at("not-even-present"));

    sm.set_split_veto([](const std::string& k) { return k == "delta" || k == "echo"; });
    BOOST_CHECK(!sm.can_split_at("delta"));
    BOOST_CHECK(!sm.can_split_at("echo"));
    BOOST_CHECK(sm.can_split_at("foxtrot"));

    sm.set_split_veto({});
    BOOST_CHECK(sm.can_split_at("delta"));
}

BOOST_AUTO_TEST_SUITE_END()
