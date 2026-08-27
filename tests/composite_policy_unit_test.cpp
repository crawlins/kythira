// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file composite_policy_unit_test.cpp
/// @brief `composite_split_merge_policy` and the tri-state merge verdict
///        (task 35 of `.kiro/specs/multi-raft/`).
///
/// Composition forces exactly one type change on the rest of the system, and
/// the first suite below is about why. Under the composite's **unanimity** rule
/// a merge needs one `propose` and no `veto`; with a two-state `bool`, a policy
/// that simply does not reason about merges — a load-only policy, say —
/// returns `false` and is indistinguishable from one objecting. It would veto
/// every merge in the cluster while looking perfectly correct in isolation.
///
/// The second property under test is that **member order is not observable**.
/// A first-wins rule anywhere in the combination would let reordering a
/// composition silently change how a cluster shards, which is the kind of
/// coupling discovered during an incident rather than during review.

#define BOOST_TEST_MODULE composite_policy_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/split_merge_policy.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using kythira::composite_split_merge_policy;
using kythira::merge_direction;
using kythira::merge_reason;
using kythira::merge_verdict;
using kythira::shard_stats;
using kythira::split_decision;
using kythira::split_merge_policy;
using kythira::split_reason;
using kythira::threshold_split_merge_policy;
using kythira::threshold_split_merge_policy_config;

namespace {

using key_type = std::string;
using group_type = std::uint64_t;
using stats_type = shard_stats<group_type, key_type>;
using threshold_type = threshold_split_merge_policy<group_type, key_type>;

constexpr std::size_t k_mib = 1024ULL * 1024;

auto sized(std::size_t bytes, std::size_t keys = 0) -> stats_type {
    stats_type s;
    s._size_available = true;
    s._approximate_size_bytes = bytes;
    s._approximate_key_count = keys;
    s._time_since_last_split = std::chrono::hours{24};
    s._time_since_last_merge = std::chrono::hours{24};
    return s;
}

/// @brief A policy that only ever looks at load, and says so.
///
/// The archetype the tri-state verdict exists for: it has a genuine opinion
/// about splits and no opinion whatsoever about merges.
class load_only_policy {
public:
    explicit load_only_policy(double qps_threshold = 1000.0) : _threshold(qps_threshold) {}

    auto evaluate_split(const stats_type& self) -> split_decision<key_type> {
        if (self._write_qps < _threshold) {
            return {};
        }
        return split_decision<key_type>{._split = true,
                                        ._at_keys = _keys,
                                        ._reason = split_reason::write_load,
                                        ._policy = name()};
    }

    auto evaluate_merge(const stats_type&, const stats_type&) -> kythira::merge_decision {
        // Abstain. Not "no" — this policy has nothing to say about merges, and
        // the whole point of the tri-state is that those are different answers.
        return kythira::merge_decision::abstain();
    }

    [[nodiscard]] auto cooldown() const -> std::chrono::milliseconds {
        return std::chrono::minutes{5};
    }
    [[nodiscard]] auto validate() const -> bool { return true; }
    [[nodiscard]] auto get_validation_errors() const -> std::vector<std::string> { return {}; }
    [[nodiscard]] static auto name() -> const char* { return "load_only"; }

    auto set_split_keys(std::vector<key_type> keys) -> void { _keys = std::move(keys); }

private:
    double _threshold;
    std::vector<key_type> _keys;
};

/// @brief A policy whose merge answer is dictated by the test.
class scripted_merge_policy {
public:
    explicit scripted_merge_policy(kythira::merge_decision answer, const char* label = "scripted")
        : _answer(answer), _label(label) {
        _answer._policy = _label;
    }

    auto evaluate_split(const stats_type&) -> split_decision<key_type> { return {}; }
    auto evaluate_merge(const stats_type&, const stats_type&) -> kythira::merge_decision {
        return _answer;
    }
    [[nodiscard]] auto cooldown() const -> std::chrono::milliseconds { return {}; }
    [[nodiscard]] auto validate() const -> bool { return true; }
    [[nodiscard]] auto get_validation_errors() const -> std::vector<std::string> { return {}; }
    [[nodiscard]] auto name() const -> const char* { return _label; }

private:
    kythira::merge_decision _answer;
    const char* _label;
};

/// @brief A policy that names split keys and nothing else.
class keys_policy {
public:
    keys_policy(std::vector<key_type> keys, const char* label)
        : _keys(std::move(keys)), _label(label) {}

    auto evaluate_split(const stats_type&) -> split_decision<key_type> {
        return split_decision<key_type>{
            ._split = true, ._at_keys = _keys, ._reason = split_reason::size, ._policy = _label};
    }
    auto evaluate_merge(const stats_type&, const stats_type&) -> kythira::merge_decision {
        return kythira::merge_decision::abstain();
    }
    [[nodiscard]] auto cooldown() const -> std::chrono::milliseconds { return {}; }
    [[nodiscard]] auto validate() const -> bool { return true; }
    [[nodiscard]] auto get_validation_errors() const -> std::vector<std::string> { return {}; }
    [[nodiscard]] auto name() const -> const char* { return _label; }
    /// Exposes the oscillation bounds so it never trips the uncheckable rule.
    [[nodiscard]] auto split_floor() const -> std::size_t { return 96 * k_mib; }
    [[nodiscard]] auto merge_ceiling() const -> std::size_t { return 20 * k_mib; }

private:
    std::vector<key_type> _keys;
    const char* _label;
};

auto contains(const std::vector<std::string>& haystack, const std::string& needle) -> bool {
    return std::any_of(haystack.begin(), haystack.end(),
                       [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

}  // namespace

BOOST_AUTO_TEST_SUITE(composite_policy_unit)

// ── the tri-state verdict ────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_default_merge_decision_abstains_rather_than_refusing) {
    // The default has to be `abstain`, so that a policy which never thinks
    // about merges behaves exactly as it did when the field was a `bool`.
    const kythira::merge_decision d;
    BOOST_CHECK(d.abstained());
    BOOST_CHECK(!d.should_merge());
    BOOST_CHECK(!d.vetoed());
    BOOST_CHECK(d.verdict() == merge_verdict::abstain);
}

BOOST_AUTO_TEST_CASE(abstain_and_veto_are_distinguishable) {
    // `!should_merge()` is true for both, which is exactly the conflation the
    // tri-state exists to prevent. Anything reading only that predicate is
    // reading a `bool` again.
    const auto abstained = kythira::merge_decision::abstain();
    const auto vetoed = kythira::merge_decision::veto(merge_reason::size);

    BOOST_CHECK(!abstained.should_merge());
    BOOST_CHECK(!vetoed.should_merge());
    BOOST_CHECK_NE(abstained.vetoed(), vetoed.vetoed());
}

BOOST_AUTO_TEST_CASE(the_threshold_policy_vetoes_rather_than_abstaining_when_shards_are_too_big) {
    // "These shards are too big to merge" is a genuine objection, and under
    // unanimity it must be expressed as one: abstaining would let a load-driven
    // member merge two 90 MiB shards into one that splits straight back.
    threshold_type p;
    const auto decision = p.evaluate_merge(sized(90 * k_mib), sized(90 * k_mib));
    BOOST_CHECK(decision.vetoed());

    // ...but an unmeasurable shard is an abstention: this policy has nothing to
    // say about shards it cannot size.
    stats_type blind;
    blind._size_available = false;
    BOOST_CHECK(p.evaluate_merge(blind, sized(1 * k_mib)).abstained());
}

// ── the concept ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_composite_is_itself_a_policy) {
    using composite = composite_split_merge_policy<threshold_type, load_only_policy>;
    static_assert(split_merge_policy<composite, group_type, key_type>);
    static_assert(split_merge_policy<composite_split_merge_policy<>, group_type, key_type>);
    BOOST_CHECK(true);
}

// ── split: any-wins, union of keys ───────────────────────────────────────────

BOOST_AUTO_TEST_CASE(either_member_can_carry_a_split) {
    composite_split_merge_policy<threshold_type, load_only_policy> c{threshold_type{},
                                                                     load_only_policy{1000.0}};

    // Neither fires.
    BOOST_CHECK(!c.evaluate_split(sized(1 * k_mib)).should_split());

    // The threshold member alone.
    auto by_size = c.evaluate_split(sized(200 * k_mib));
    BOOST_CHECK(by_size.should_split());
    BOOST_CHECK(by_size.reason() == split_reason::size);

    // The load member alone, on a shard far too small for the other to care.
    auto hot = sized(1 * k_mib);
    hot._write_qps = 5000;
    auto by_load = c.evaluate_split(hot);
    BOOST_CHECK(by_load.should_split());
    BOOST_CHECK(by_load.reason() == split_reason::write_load);
    BOOST_CHECK_EQUAL(std::string{by_load.policy()}, "load_only");
}

BOOST_AUTO_TEST_CASE(concrete_keys_from_several_members_are_unioned_sorted_and_deduplicated) {
    composite_split_merge_policy<keys_policy, keys_policy> c{keys_policy{{"m", "c"}, "alpha"},
                                                             keys_policy{{"t", "c"}, "bravo"}};

    const auto d = c.evaluate_split(sized(1 * k_mib));
    BOOST_REQUIRE(d.should_split());
    const std::vector<key_type> expected{"c", "m", "t"};
    BOOST_CHECK(d.at_keys() == expected);
}

BOOST_AUTO_TEST_CASE(a_member_deferring_yields_to_a_member_that_named_keys) {
    // "Split, you choose" cannot be unioned with a concrete key vector, so
    // concrete keys win outright rather than being diluted.
    composite_split_merge_policy<threshold_type, keys_policy> c{threshold_type{},
                                                                keys_policy{{"m"}, "bravo"}};

    // The threshold member proposes with empty keys ("you choose"); the other
    // names "m".
    const auto d = c.evaluate_split(sized(200 * k_mib));
    BOOST_REQUIRE(d.should_split());
    const std::vector<key_type> expected{"m"};
    BOOST_CHECK(d.at_keys() == expected);
}

BOOST_AUTO_TEST_CASE(deferral_alone_stays_a_deferral) {
    // With nobody naming a key, the empty vector IS the deferral, which the
    // host turns into the state machine's `suggest_split_keys`.
    composite_split_merge_policy<threshold_type> c{threshold_type{}};
    const auto d = c.evaluate_split(sized(200 * k_mib));
    BOOST_CHECK(d.should_split());
    BOOST_CHECK(d.at_keys().empty());
}

// ── merge: unanimity ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(an_abstaining_member_does_not_block_a_merge) {
    // Requirement 6.10, and the reason the verdict is tri-state: a policy that
    // reasons only about load must be able to have no opinion about merges
    // without thereby preventing every merge in the cluster.
    composite_split_merge_policy<threshold_type, load_only_policy> c{threshold_type{},
                                                                     load_only_policy{}};

    const auto d = c.evaluate_merge(sized(1 * k_mib), sized(1 * k_mib));
    BOOST_CHECK_MESSAGE(d.should_merge(), "an abstention must not count against a proposal");
    BOOST_CHECK_EQUAL(std::string{d.policy()}, "threshold");
}

BOOST_AUTO_TEST_CASE(a_single_veto_blocks_a_merge_every_other_member_wants) {
    // The same stub, now objecting rather than abstaining. Merge is not the
    // mirror of split: it destroys a group and moves data through `absorb`, so
    // one objection is enough.
    composite_split_merge_policy<threshold_type, scripted_merge_policy> c{
        threshold_type{},
        scripted_merge_policy{kythira::merge_decision::veto(merge_reason::size), "objector"}};

    const auto d = c.evaluate_merge(sized(1 * k_mib), sized(1 * k_mib));
    BOOST_CHECK(d.vetoed());
    BOOST_CHECK(!d.should_merge());
    BOOST_CHECK_EQUAL(std::string{d.policy()}, "objector");
}

BOOST_AUTO_TEST_CASE(a_composition_where_nobody_proposes_abstains) {
    // Not a veto. "Nobody asked for a merge" and "somebody forbade one" are
    // different facts, and the host counts only the second.
    composite_split_merge_policy<load_only_policy, load_only_policy> c{load_only_policy{},
                                                                       load_only_policy{}};
    BOOST_CHECK(c.evaluate_merge(sized(1 * k_mib), sized(1 * k_mib)).abstained());
}

BOOST_AUTO_TEST_CASE(members_proposing_opposite_directions_are_a_mutual_veto) {
    // Silently picking one would make the composite's answer depend on the
    // order its members were written down.
    using left = scripted_merge_policy;
    composite_split_merge_policy<left, left> c{
        left{kythira::merge_decision::propose(merge_direction::into_left_sibling,
                                              merge_reason::size),
             "wants_left"},
        left{kythira::merge_decision::propose(merge_direction::into_right_sibling,
                                              merge_reason::size),
             "wants_right"}};

    const auto d = c.evaluate_merge(sized(1 * k_mib), sized(1 * k_mib));
    BOOST_CHECK(d.vetoed());
    BOOST_CHECK(!d.should_merge());

    // ...and it is logged, not swallowed.
    BOOST_CHECK_MESSAGE(!c.notices().empty(),
                        "a mutual veto from opposing directions must leave a trace");
    BOOST_CHECK(contains(c.notices(), "opposite merge directions"));
}

// ── order independence ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(member_order_changes_no_decision) {
    // Every combination rule is commutative and associative, including the
    // attribution tie-break. A randomised sweep over inputs, comparing the two
    // orderings at every point.
    composite_split_merge_policy<threshold_type, load_only_policy> ab{threshold_type{},
                                                                      load_only_policy{1000.0}};
    composite_split_merge_policy<load_only_policy, threshold_type> ba{load_only_policy{1000.0},
                                                                      threshold_type{}};

    std::mt19937 rng{20260826};
    std::uniform_int_distribution<std::size_t> bytes{0, 300 * k_mib};
    std::uniform_int_distribution<std::size_t> keys{0, 2'000'000};
    std::uniform_real_distribution<double> qps{0.0, 5000.0};

    for (int i = 0; i < 500; ++i) {
        auto self = sized(bytes(rng), keys(rng));
        self._write_qps = qps(rng);
        auto sibling = sized(bytes(rng), keys(rng));
        sibling._write_qps = qps(rng);

        const auto s1 = ab.evaluate_split(self);
        const auto s2 = ba.evaluate_split(self);
        BOOST_REQUIRE_EQUAL(s1.should_split(), s2.should_split());
        BOOST_REQUIRE(s1.at_keys() == s2.at_keys());
        BOOST_REQUIRE(s1.reason() == s2.reason());
        BOOST_REQUIRE_EQUAL(std::string{s1.policy() == nullptr ? "" : s1.policy()},
                            std::string{s2.policy() == nullptr ? "" : s2.policy()});

        const auto m1 = ab.evaluate_merge(self, sibling);
        const auto m2 = ba.evaluate_merge(self, sibling);
        BOOST_REQUIRE(m1.verdict() == m2.verdict());
        BOOST_REQUIRE(m1.direction() == m2.direction());
    }
}

// ── validation ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_composition_of_two_shipped_defaults_validates) {
    composite_split_merge_policy<threshold_type, threshold_type> c{threshold_type{},
                                                                   threshold_type{}};
    const auto errors = c.get_validation_errors();
    const std::string first_error = errors.empty() ? std::string{} : errors.front();
    BOOST_CHECK_MESSAGE(errors.empty(), "unexpected validation error: " << first_error);
    BOOST_CHECK(c.validate());
}

BOOST_AUTO_TEST_CASE(interleaved_thresholds_are_rejected_and_both_members_are_named) {
    // Each member validates cleanly alone. Member A merges up to 60 MiB;
    // member B splits at 96 MiB. Two shards A merges make one B splits straight
    // back, forever — moving real data through `split_state` and `absorb` every
    // cycle, and invisible to any check that looks at one policy at a time.
    threshold_split_merge_policy_config merger;
    merger._shard_merge_max_size_bytes = 60 * k_mib;
    merger._shard_split_size_bytes = 200 * k_mib;
    merger._shard_max_size_bytes = 300 * k_mib;

    threshold_split_merge_policy_config splitter;
    splitter._shard_merge_max_size_bytes = 4 * k_mib;
    splitter._shard_split_size_bytes = 96 * k_mib;

    BOOST_REQUIRE(threshold_type{merger}.validate());
    BOOST_REQUIRE(threshold_type{splitter}.validate());

    composite_split_merge_policy<threshold_type, threshold_type> c{threshold_type{merger},
                                                                   threshold_type{splitter}};
    BOOST_CHECK(!c.validate());

    const auto errors = c.get_validation_errors();
    BOOST_REQUIRE(!errors.empty());
    BOOST_CHECK(contains(errors, "member 0"));
    BOOST_CHECK(contains(errors, "member 1"));
    BOOST_CHECK_MESSAGE(contains(errors, "oscillat"),
                        "the rejection must name the failure it prevents");
}

BOOST_AUTO_TEST_CASE(a_member_without_the_bounds_accessors_is_reported_uncheckable) {
    // Not silently assumed safe. "We did not check" and "we checked and it is
    // fine" are different facts, and only one of them justifies running the
    // composition.
    composite_split_merge_policy<threshold_type, load_only_policy> c{threshold_type{},
                                                                     load_only_policy{}};
    const auto errors = c.get_validation_errors();
    BOOST_CHECK(contains(errors, "uncheckable"));
    BOOST_CHECK(contains(errors, "load_only"));
}

BOOST_AUTO_TEST_CASE(a_members_own_errors_are_reported_and_attributed) {
    threshold_split_merge_policy_config bad;
    bad._shard_merge_max_size_bytes = 60 * k_mib;
    bad._shard_split_size_bytes = 96 * k_mib;

    composite_split_merge_policy<threshold_type, threshold_type> c{threshold_type{},
                                                                   threshold_type{bad}};
    const auto errors = c.get_validation_errors();
    BOOST_CHECK(contains(errors, "member 1"));
    BOOST_CHECK(contains(errors, "threshold"));
}

// ── cooldown and the empty composition ───────────────────────────────────────

BOOST_AUTO_TEST_CASE(cooldown_is_the_maximum_over_members) {
    composite_split_merge_policy<threshold_type, load_only_policy> c{threshold_type{},
                                                                     load_only_policy{}};
    // The threshold default is an hour; the load stub is five minutes.
    BOOST_CHECK(c.cooldown() == std::chrono::hours{1});
}

BOOST_AUTO_TEST_CASE(an_empty_composition_proposes_nothing_and_says_so_once) {
    // `{}` is far too easy to arrive at by accident for "no policies
    // configured" and "policies configured, never firing" to be
    // indistinguishable.
    composite_split_merge_policy<> c;
    BOOST_CHECK_EQUAL(c.size(), 0U);
    BOOST_CHECK(!c.evaluate_split(sized(500 * k_mib)).should_split());
    BOOST_CHECK(c.evaluate_merge(sized(1), sized(1)).abstained());
    BOOST_CHECK(c.cooldown() == std::chrono::milliseconds{0});

    BOOST_REQUIRE_EQUAL(c.notices().size(), 1U);
    BOOST_CHECK(contains(c.notices(), "zero members"));

    // Wiring the sink up afterwards replays it rather than losing it.
    std::vector<std::string> logged;
    c.set_notice_sink([&logged](const std::string& m) { logged.push_back(m); });
    BOOST_CHECK_EQUAL(logged.size(), 1U);
}

BOOST_AUTO_TEST_SUITE_END()
