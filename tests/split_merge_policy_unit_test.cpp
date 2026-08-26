// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file split_merge_policy_unit_test.cpp
/// @brief The declarative policy and its oscillation guard (tasks 23 and 24 of
///        `.kiro/specs/multi-raft/`).
///
/// The centre of this file is `validate()`. A policy configured so that two
/// shards just under the merge threshold merge into one just over the split
/// threshold will split it straight back, forever — burning an election and a
/// scatter each way round. The guard is one inequality, and the value of
/// testing it is that the *message* has to name the failure it prevents, or an
/// operator reading "invalid configuration" learns nothing.

#define BOOST_TEST_MODULE split_merge_policy_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/split_merge_policy.hpp>

#include <cstdint>
#include <string>
#include <vector>

using kythira::merge_reason;
using kythira::shard_stats;
using kythira::split_merge_policy;
using kythira::split_reason;
using kythira::threshold_split_merge_policy;
using kythira::threshold_split_merge_policy_config;

namespace {

using key_type = std::string;
using group_type = std::uint64_t;
using policy_type = threshold_split_merge_policy<group_type, key_type>;
using stats_type = shard_stats<group_type, key_type>;

constexpr std::size_t k_mib = 1024ULL * 1024;

auto sized(std::size_t bytes, std::size_t keys = 0) -> stats_type {
    stats_type s;
    s._size_available = true;
    s._approximate_size_bytes = bytes;
    s._approximate_key_count = keys;
    // Far enough in the past that the cooldown never masks the case under test.
    s._time_since_last_split = std::chrono::hours{24};
    s._time_since_last_merge = std::chrono::hours{24};
    return s;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(split_merge_policy_unit)

BOOST_AUTO_TEST_CASE(the_default_policy_satisfies_the_concept) {
    static_assert(split_merge_policy<policy_type, group_type, key_type>);

    // A policy missing any one member does not qualify — a half-implemented
    // policy is worse than none, because the host would call the missing half.
    struct missing_merge {
        auto evaluate_split(const stats_type&) -> kythira::split_decision<key_type> { return {}; }
        auto cooldown() const -> std::chrono::milliseconds { return {}; }
        auto validate() const -> bool { return true; }
        auto get_validation_errors() const -> std::vector<std::string> { return {}; }
    };
    static_assert(!split_merge_policy<missing_merge, group_type, key_type>);
    BOOST_CHECK(true);
}

// ── the oscillation guard ────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_shipped_defaults_validate) {
    const policy_type p;
    BOOST_CHECK(p.validate());
    BOOST_CHECK(p.get_validation_errors().empty());

    // 20 MiB against 96 MiB: comfortably inside `2 * merge < split`.
    BOOST_CHECK_LT(2 * p.config()._shard_merge_max_size_bytes, p.config()._shard_split_size_bytes);
}

BOOST_AUTO_TEST_CASE(an_oscillating_size_configuration_is_rejected_and_the_message_says_why) {
    // The design's own example: merge_max = 60 MiB against split_size = 96 MiB.
    // Two shards at 59 MiB merge to 118 MiB, which is over 96 and splits back
    // to two shards at 59 MiB. Forever.
    threshold_split_merge_policy_config cfg;
    cfg._shard_merge_max_size_bytes = 60 * k_mib;
    cfg._shard_split_size_bytes = 96 * k_mib;
    const policy_type p{cfg};

    BOOST_CHECK(!p.validate());
    const auto errors = p.get_validation_errors();
    BOOST_REQUIRE(!errors.empty());
    bool mentions_oscillation = false;
    for (const auto& e : errors) {
        if (e.find("oscillat") != std::string::npos) {
            mentions_oscillation = true;
        }
    }
    BOOST_CHECK_MESSAGE(mentions_oscillation,
                        "the rejection must name the failure it prevents, not merely refuse");
}

BOOST_AUTO_TEST_CASE(the_key_count_equivalent_is_rejected_too) {
    threshold_split_merge_policy_config cfg;
    cfg._shard_merge_max_keys = 600'000;
    cfg._shard_split_keys = 960'000;
    const policy_type p{cfg};
    BOOST_CHECK(!p.validate());

    bool mentions_keys = false;
    for (const auto& e : p.get_validation_errors()) {
        if (e.find("_shard_split_keys") != std::string::npos) {
            mentions_keys = true;
        }
    }
    BOOST_CHECK(mentions_keys);
}

BOOST_AUTO_TEST_CASE(the_boundary_is_strict) {
    // Exactly `2 * merge == split` still oscillates: the merged shard sits
    // exactly at the split threshold, which is `>=`.
    threshold_split_merge_policy_config cfg;
    cfg._shard_merge_max_size_bytes = 48 * k_mib;
    cfg._shard_split_size_bytes = 96 * k_mib;
    BOOST_CHECK(!policy_type{cfg}.validate());

    cfg._shard_merge_max_size_bytes = 47 * k_mib;
    BOOST_CHECK(policy_type{cfg}.validate());
}

BOOST_AUTO_TEST_CASE(other_incoherent_configurations_are_rejected) {
    {
        threshold_split_merge_policy_config cfg;
        cfg._shard_split_size_bytes = 200 * k_mib;  // above _shard_max_size_bytes
        BOOST_CHECK(!policy_type{cfg}.validate());
    }
    {
        threshold_split_merge_policy_config cfg;
        cfg._batch_split_limit = 0;
        BOOST_CHECK(!policy_type{cfg}.validate());
    }
    {
        threshold_split_merge_policy_config cfg;
        cfg._load_split_one_sided_fraction = 0.4;  // not one-sided at all
        BOOST_CHECK(!policy_type{cfg}.validate());
        cfg._load_split_one_sided_fraction = 1.5;  // unreachable
        BOOST_CHECK(!policy_type{cfg}.validate());
    }
}

// ── split decisions ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_shard_over_the_size_threshold_splits) {
    policy_type p;
    BOOST_CHECK(!p.evaluate_split(sized(50 * k_mib)).should_split());

    const auto decision = p.evaluate_split(sized(100 * k_mib));
    BOOST_CHECK(decision.should_split());
    BOOST_CHECK(decision.reason() == split_reason::size);
    // Empty keys means "you choose" — the state machine picks the cut, which is
    // what lets it refuse to cut through an indivisible entity.
    BOOST_CHECK(decision.at_keys().empty());
}

BOOST_AUTO_TEST_CASE(a_shard_over_the_key_threshold_splits_for_that_reason) {
    policy_type p;
    const auto decision = p.evaluate_split(sized(1 * k_mib, 1'000'000));
    BOOST_CHECK(decision.should_split());
    BOOST_CHECK(decision.reason() == split_reason::key_count);
}

BOOST_AUTO_TEST_CASE(a_state_machine_without_sizing_hooks_never_splits_by_size) {
    // `_size_available == false` must not read as "size zero". It is the
    // difference between "this shard is small" and "I cannot tell", and the
    // host reports the second once at construction rather than leaving an
    // operator to wonder for a week why nothing splits.
    policy_type p;
    stats_type blind;
    blind._size_available = false;
    blind._approximate_size_bytes = 500 * k_mib;  // ignored
    BOOST_CHECK(!p.evaluate_split(blind).should_split());
}

BOOST_AUTO_TEST_CASE(a_balanced_hot_key_sample_produces_a_load_split) {
    threshold_split_merge_policy_config cfg;
    cfg._load_split_enabled = true;
    policy_type p{cfg};

    stats_type hot = sized(1 * k_mib);  // far too small for a size split
    hot._write_qps = 5000;
    hot._hot_key_samples.push_back(kythira::hot_key_sample<key_type>{
        ._key = "m", ._left_accesses = 500, ._right_accesses = 500});

    const auto decision = p.evaluate_split(hot);
    BOOST_REQUIRE(decision.should_split());
    BOOST_CHECK(decision.reason() == split_reason::write_load);
    BOOST_REQUIRE_EQUAL(decision.at_keys().size(), 1u);
    BOOST_CHECK_EQUAL(decision.at_keys()[0], "m");
}

BOOST_AUTO_TEST_CASE(a_single_hot_key_produces_no_split) {
    // Splitting cannot help when all the load is on one key, and proposing it
    // anyway is a split storm that shrinks shards toward one key each.
    threshold_split_merge_policy_config cfg;
    cfg._load_split_enabled = true;
    policy_type p{cfg};

    stats_type hot = sized(1 * k_mib);
    hot._read_qps = 5000;
    hot._hot_key_samples.push_back(kythira::hot_key_sample<key_type>{
        ._key = "m", ._left_accesses = 999, ._right_accesses = 1});
    BOOST_CHECK(!p.evaluate_split(hot).should_split());
}

BOOST_AUTO_TEST_CASE(the_load_sampler_is_off_by_default) {
    policy_type p;
    stats_type hot = sized(1 * k_mib);
    hot._hot_key_samples.push_back(kythira::hot_key_sample<key_type>{
        ._key = "m", ._left_accesses = 500, ._right_accesses = 500});
    BOOST_CHECK(!p.evaluate_split(hot).should_split());
}

// ── merge decisions ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(two_small_neighbours_merge) {
    policy_type p;
    const auto decision = p.evaluate_merge(sized(1 * k_mib), sized(1 * k_mib));
    BOOST_CHECK(decision.should_merge());
    BOOST_CHECK(decision.reason() == merge_reason::size);
}

BOOST_AUTO_TEST_CASE(a_small_shard_beside_a_large_one_does_not_merge) {
    // Merging into a large neighbour produces a shard over the split threshold,
    // which is the oscillation the guard exists to prevent — arriving through
    // the one door the guard does not watch.
    policy_type p;
    BOOST_CHECK(!p.evaluate_merge(sized(1 * k_mib), sized(90 * k_mib)).should_merge());
    BOOST_CHECK(!p.evaluate_merge(sized(90 * k_mib), sized(1 * k_mib)).should_merge());
}

BOOST_AUTO_TEST_CASE(a_shard_that_split_recently_does_not_merge_back) {
    policy_type p;
    auto fresh = sized(1 * k_mib);
    fresh._time_since_last_split = std::chrono::minutes{1};
    BOOST_CHECK(!p.evaluate_merge(fresh, sized(1 * k_mib)).should_merge());
    BOOST_CHECK(!p.evaluate_merge(sized(1 * k_mib), fresh).should_merge());
}

BOOST_AUTO_TEST_CASE(a_blind_state_machine_never_merges_either) {
    policy_type p;
    stats_type blind;
    blind._size_available = false;
    BOOST_CHECK(!p.evaluate_merge(blind, sized(1 * k_mib)).should_merge());
    BOOST_CHECK(!p.evaluate_merge(sized(1 * k_mib), blind).should_merge());
}

// ── split-key generation (TiKV RFC 0006's SizeChecker) ───────────────────────

BOOST_AUTO_TEST_CASE(a_batch_limit_of_one_degenerates_to_single_key_split) {
    // The property that makes the batch path testable against the simple one.
    threshold_split_merge_policy_config cfg;
    cfg._batch_split_limit = 1;
    const policy_type p{cfg};

    BOOST_CHECK_EQUAL(p.split_key_count_for_size(50 * k_mib), 0u);   // under the threshold
    BOOST_CHECK_EQUAL(p.split_key_count_for_size(200 * k_mib), 1u);  // never more than one
    BOOST_CHECK_EQUAL(p.split_key_count_for_size(1000 * k_mib), 1u);
}

BOOST_AUTO_TEST_CASE(a_shard_at_three_times_the_split_size_yields_more_than_one_key) {
    const policy_type p;
    BOOST_CHECK_EQUAL(p.split_key_count_for_size(50 * k_mib), 0u);
    BOOST_CHECK_GE(p.split_key_count_for_size(3 * 96 * k_mib), 2u);
    // And it is bounded by the batch limit however large the shard is.
    BOOST_CHECK_LE(p.split_key_count_for_size(100'000 * k_mib), p.config()._batch_split_limit);
}

BOOST_AUTO_TEST_CASE(a_trailing_remnant_too_small_to_be_worth_a_child_is_discarded) {
    // RFC 0006's rule: discard the trailing key when what is left behind it is
    // not larger than `max - split`. Cutting there costs an election for a
    // child too small to be worth it.
    const policy_type p;
    // 96 MiB exactly: one boundary, but nothing meaningful behind it.
    BOOST_CHECK_EQUAL(p.split_key_count_for_size(96 * k_mib), 0u);
    // 96 + 60 MiB: the remainder (60) is larger than max - split (48), so the
    // cut is worth making.
    BOOST_CHECK_EQUAL(p.split_key_count_for_size(96 * k_mib + 60 * k_mib), 1u);
    // 96 + 10 MiB: the remainder is not, so it is not.
    BOOST_CHECK_EQUAL(p.split_key_count_for_size(96 * k_mib + 10 * k_mib), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
