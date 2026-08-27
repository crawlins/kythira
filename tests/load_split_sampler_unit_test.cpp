// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file load_split_sampler_unit_test.cpp
/// @brief TiKV RFC 0045's load-based split sampler (task 30 of
///        `.kiro/specs/multi-raft/`).
///
/// Three of the cases below are about the sampler *refusing* to propose, and
/// that is the right emphasis. Proposing a split at the median of a balanced
/// access pattern is the easy half; the two ways this feature does damage are
/// splitting for a five-second spike (an election per child, a scatter, and
/// nothing that outlives the burst) and splitting a shard whose load is one
/// indivisible key — which does not help, leaves the shard hot, and proposes
/// again, shrinking shards toward one key each.

#define BOOST_TEST_MODULE load_split_sampler_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/load_split_sampler.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using kythira::load_sampler_state;
using kythira::load_split_sampler;
using kythira::load_split_sampler_config;

namespace {

using key_type = std::string;
using sampler_type = load_split_sampler<key_type>;
using clock = sampler_type::clock;

auto enabled_config() -> load_split_sampler_config {
    load_split_sampler_config cfg;
    cfg._enabled = true;
    cfg._qps_threshold = 100.0;
    cfg._bytes_threshold = 1e12;  // out of the way; QPS is the trigger here
    cfg._duration = std::chrono::seconds{10};
    cfg._sample_keys = 20;
    cfg._one_sided_fraction = 0.99;
    cfg._backoff = std::chrono::minutes{10};
    cfg._seed = 20260826;  // a policy may be non-deterministic; a test may not
    return cfg;
}

/// Two-digit decimal keys, so lexicographic order is numeric order.
auto key_of(int n) -> key_type {
    std::string s = std::to_string(n);
    return std::string(2 - std::min<std::size_t>(2, s.size()), '0') + s;
}

/// Drive `count` observations spread evenly over `[00, 99]`.
auto drive_balanced(sampler_type& s, int count) -> void {
    for (int i = 0; i < count; ++i) {
        s.observe(key_of(i % 100));
    }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(load_split_sampler_unit)

// ── the disabled path ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_sampler_is_off_by_default) {
    const sampler_type s;
    BOOST_CHECK(!s.enabled());
    BOOST_CHECK(s.state() == load_sampler_state::idle);
}

BOOST_AUTO_TEST_CASE(a_disabled_sampler_never_leaves_its_first_branch) {
    // Requirement 9.7's cost claim, asserted with a call-count instrument
    // rather than a timing measurement: `observation_count()` is incremented
    // only *past* the state check, so a disabled sampler that did any work at
    // all would show it here.
    sampler_type s;
    for (int i = 0; i < 100'000; ++i) {
        s.observe(key_of(i % 100));
    }
    BOOST_CHECK_EQUAL(s.observation_count(), 0U);
    BOOST_CHECK_EQUAL(s.candidate_count(), 0U);

    // And `evaluate` is inert too, however hot the shard looks.
    const auto out = s.evaluate(1e9, 1e9, 1e9, 1e9, clock::now());
    BOOST_CHECK(out.empty());
    BOOST_CHECK_EQUAL(s.windows_started(), 0U);
}

BOOST_AUTO_TEST_CASE(an_enabled_but_idle_sampler_also_does_nothing_per_request) {
    // Between windows the same branch is taken. A shard under the threshold
    // pays nothing for having the feature configured on.
    sampler_type s{enabled_config()};
    const auto t0 = clock::now();
    BOOST_REQUIRE(s.evaluate(1.0, 1.0, 0.0, 0.0, t0).empty());
    BOOST_REQUIRE(s.state() == load_sampler_state::idle);

    drive_balanced(s, 1000);
    BOOST_CHECK_EQUAL(s.observation_count(), 0U);
}

// ── entering and abandoning ──────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(crossing_the_threshold_starts_a_window) {
    sampler_type s{enabled_config()};
    const auto t0 = clock::now();

    BOOST_CHECK(s.evaluate(0.0, 50.0, 0.0, 0.0, t0).empty());
    BOOST_CHECK(s.state() == load_sampler_state::idle);

    BOOST_CHECK(s.evaluate(0.0, 500.0, 0.0, 0.0, t0).empty());
    BOOST_CHECK(s.state() == load_sampler_state::sampling);
    BOOST_CHECK_EQUAL(s.windows_started(), 1U);
}

BOOST_AUTO_TEST_CASE(either_rate_or_either_byte_measure_can_open_a_window) {
    // A shard can be hot in requests without being hot in bytes, and the
    // reverse. Requiring both would miss half the shards the feature exists for.
    load_split_sampler_config cfg = enabled_config();
    cfg._bytes_threshold = 1024.0;

    const auto t0 = clock::now();
    {
        sampler_type s{cfg};
        s.evaluate(0.0, 0.0, 0.0, 4096.0, t0);
        BOOST_CHECK(s.state() == load_sampler_state::sampling);
    }
    {
        sampler_type s{cfg};
        s.evaluate(500.0, 0.0, 0.0, 0.0, t0);
        BOOST_CHECK(s.state() == load_sampler_state::sampling);
    }
}

BOOST_AUTO_TEST_CASE(a_five_second_spike_produces_no_proposal) {
    // RFC 0045, verbatim: "splitting is meaningless for momentary and short
    // loads (<10s)". A split costs an election per child and a scatter; paying
    // that for a burst that is already over is a net loss.
    sampler_type s{enabled_config()};
    const auto t0 = clock::now();

    BOOST_REQUIRE(s.evaluate(0.0, 500.0, 0.0, 0.0, t0).empty());
    BOOST_REQUIRE(s.state() == load_sampler_state::sampling);
    drive_balanced(s, 5000);

    // Five seconds in, the load goes away.
    const auto out = s.evaluate(0.0, 1.0, 0.0, 0.0, t0 + std::chrono::seconds{5});
    BOOST_CHECK(out.empty());
    BOOST_CHECK(s.state() == load_sampler_state::idle);
    BOOST_CHECK_EQUAL(s.abandoned_spike_count(), 1U);
    BOOST_CHECK_EQUAL(s.proposal_count(), 0U);

    // The evidence is discarded with the window: carrying it into the next one
    // would let two unrelated bursts add up to a proposal.
    BOOST_CHECK_EQUAL(s.candidate_count(), 0U);
}

BOOST_AUTO_TEST_CASE(a_window_still_open_proposes_nothing_yet) {
    sampler_type s{enabled_config()};
    const auto t0 = clock::now();
    s.evaluate(0.0, 500.0, 0.0, 0.0, t0);
    drive_balanced(s, 2000);

    BOOST_CHECK(s.evaluate(0.0, 500.0, 0.0, 0.0, t0 + std::chrono::seconds{9}).empty());
    BOOST_CHECK(s.state() == load_sampler_state::sampling);
}

// ── the balanced case ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_sustained_balanced_load_proposes_a_split_near_the_median) {
    sampler_type s{enabled_config()};
    const auto t0 = clock::now();

    BOOST_REQUIRE(s.evaluate(0.0, 500.0, 0.0, 0.0, t0).empty());
    drive_balanced(s, 20'000);

    const auto out = s.evaluate(0.0, 500.0, 0.0, 0.0, t0 + std::chrono::seconds{10});
    BOOST_REQUIRE_EQUAL(out.size(), 1U);
    BOOST_CHECK_EQUAL(s.proposal_count(), 1U);
    BOOST_CHECK(s.state() == load_sampler_state::idle);

    const auto& sample = out.front();
    // Balanced enough to be worth cutting: the whole test of "is a split worth
    // it" is this fraction, not the key's identity.
    BOOST_CHECK_LT(sample.one_sided_fraction(), 0.99);
    BOOST_CHECK_GT(sample.total_accesses(), 0U);

    // Uniform access over "00".."99" means the most balanced boundary should
    // land somewhere near the middle. A generous band: the reservoir picks
    // candidates at random, and the guarantee is "near the median", not "the
    // median".
    const int cut = std::stoi(sample.key());
    BOOST_CHECK_MESSAGE(
        cut >= 20 && cut <= 80,
        "expected a cut near the median of a uniform key range, got " + sample.key());
}

BOOST_AUTO_TEST_CASE(a_second_window_can_run_after_a_proposal) {
    // A proposal returns the sampler to idle rather than to a terminal state:
    // the shard may still be hot after the split, and the next window is how
    // that is discovered.
    sampler_type s{enabled_config()};
    auto t = clock::now();
    s.evaluate(0.0, 500.0, 0.0, 0.0, t);
    drive_balanced(s, 20'000);
    t += std::chrono::seconds{10};
    BOOST_REQUIRE_EQUAL(s.evaluate(0.0, 500.0, 0.0, 0.0, t).size(), 1U);

    // Immediately eligible again.
    BOOST_CHECK(s.evaluate(0.0, 500.0, 0.0, 0.0, t).empty());
    BOOST_CHECK(s.state() == load_sampler_state::sampling);
    BOOST_CHECK_EQUAL(s.windows_started(), 2U);
}

// ── the single hot key ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_single_hot_key_proposes_nothing_and_backs_the_shard_off) {
    // The failure this prevents is not "no benefit" but unbounded harm: without
    // the back-off the sampler proposes, the split does not help because the
    // key is indivisible, the shard is still hot, and it proposes again.
    sampler_type s{enabled_config()};
    const auto t0 = clock::now();

    BOOST_REQUIRE(s.evaluate(0.0, 500.0, 0.0, 0.0, t0).empty());
    for (int i = 0; i < 20'000; ++i) {
        s.observe("50");
    }

    const auto out = s.evaluate(0.0, 500.0, 0.0, 0.0, t0 + std::chrono::seconds{10});
    BOOST_CHECK(out.empty());
    BOOST_CHECK_EQUAL(s.single_hot_key_count(), 1U);
    BOOST_CHECK_EQUAL(s.proposal_count(), 0U);
    BOOST_CHECK(s.state() == load_sampler_state::backoff);
}

BOOST_AUTO_TEST_CASE(a_backed_off_shard_stays_ineligible_until_the_period_expires) {
    load_split_sampler_config cfg = enabled_config();
    cfg._backoff = std::chrono::minutes{10};
    sampler_type s{cfg};

    const auto t0 = clock::now();
    s.evaluate(0.0, 500.0, 0.0, 0.0, t0);
    for (int i = 0; i < 20'000; ++i) {
        s.observe("50");
    }
    BOOST_REQUIRE(s.evaluate(0.0, 500.0, 0.0, 0.0, t0 + std::chrono::seconds{10}).empty());
    BOOST_REQUIRE(s.state() == load_sampler_state::backoff);

    // Still hot, still refused, and no new window opened.
    const auto windows_before = s.windows_started();
    BOOST_CHECK(s.evaluate(0.0, 5000.0, 0.0, 0.0, t0 + std::chrono::minutes{5}).empty());
    BOOST_CHECK(s.state() == load_sampler_state::backoff);
    BOOST_CHECK_EQUAL(s.windows_started(), windows_before);

    // Past the back-off it becomes eligible again — on the tick after the one
    // that clears the state, since clearing and entering are separate
    // decisions.
    BOOST_CHECK(s.evaluate(0.0, 5000.0, 0.0, 0.0, t0 + std::chrono::minutes{11}).empty());
    BOOST_CHECK(s.state() == load_sampler_state::idle);
    BOOST_CHECK(s.evaluate(0.0, 5000.0, 0.0, 0.0, t0 + std::chrono::minutes{11}).empty());
    BOOST_CHECK(s.state() == load_sampler_state::sampling);
}

BOOST_AUTO_TEST_CASE(a_load_just_inside_the_one_sided_bound_still_proposes) {
    // The bound is a knob, and the case either side of it must behave
    // differently — otherwise it is not a bound, it is a constant.
    load_split_sampler_config cfg = enabled_config();
    cfg._one_sided_fraction = 0.90;
    sampler_type s{cfg};

    const auto t0 = clock::now();
    s.evaluate(0.0, 500.0, 0.0, 0.0, t0);
    // 80/20 across the key space: lopsided, but well inside a 90 % bound.
    for (int i = 0; i < 20'000; ++i) {
        s.observe(i % 5 == 0 ? key_of(90) : key_of(10));
    }
    const auto out = s.evaluate(0.0, 500.0, 0.0, 0.0, t0 + std::chrono::seconds{10});
    BOOST_CHECK_EQUAL(out.size(), 1U);
    BOOST_CHECK_EQUAL(s.single_hot_key_count(), 0U);
}

// ── the candidate set ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_candidate_set_is_bounded_by_its_configured_size) {
    // This bound is the per-request cost while sampling: every observation
    // updates every candidate's counters.
    load_split_sampler_config cfg = enabled_config();
    cfg._sample_keys = 8;
    sampler_type s{cfg};

    s.evaluate(0.0, 500.0, 0.0, 0.0, clock::now());
    drive_balanced(s, 50'000);
    BOOST_CHECK_EQUAL(s.candidate_count(), 8U);
}

BOOST_AUTO_TEST_CASE(a_window_with_no_key_addressed_traffic_proposes_nothing) {
    // Hot by the rate counters, but nothing reached the sampler — a shard whose
    // reads are not key-addressed, say. Nothing to propose, and nothing to
    // blame on a single hot key either, so no back-off.
    sampler_type s{enabled_config()};
    const auto t0 = clock::now();
    s.evaluate(0.0, 500.0, 0.0, 0.0, t0);

    const auto out = s.evaluate(0.0, 500.0, 0.0, 0.0, t0 + std::chrono::seconds{10});
    BOOST_CHECK(out.empty());
    BOOST_CHECK_EQUAL(s.single_hot_key_count(), 0U);
    BOOST_CHECK(s.state() == load_sampler_state::idle);
}

BOOST_AUTO_TEST_SUITE_END()
