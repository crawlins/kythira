// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_oscillation_test.cpp
/// @brief The anti-oscillation defences, demonstrated rather than asserted
///        (task 33 of `.kiro/specs/multi-raft/`).
///
/// Oscillation is the failure mode that costs the most and shows the least.
/// Two shards each just under the merge threshold merge into one just over the
/// split threshold, which splits back into two just under the merge threshold,
/// forever — burning an election and a `split_state`/`absorb` pass each way
/// round, while every individual decision looks locally correct. Nothing in a
/// log line says "this is a loop".
///
/// There are three defences, and this file drives each one over a long horizon
/// rather than checking it once:
///
///  1. `threshold_split_merge_policy::validate()` rejects a configuration whose
///     thresholds overlap (design §6.1.2).
///  2. The **host** enforces `split_merge_interval` independently of the policy
///     (Requirement 7.6), so a policy that forgets its own cooldown — or has
///     its `validate()` ignored — still cannot oscillate faster than the
///     interval.
///  3. `composite_split_merge_policy::validate()` extends check 1 *across*
///     members (Requirement 8.6). This is the case check 1 cannot see: two
///     members each validate cleanly alone while the pair oscillates.
///
/// ### What this file simulates, and what it does not
///
/// The shard population, the sizes, and the clock are simulated; the policy and
/// its `validate()` are the real ones. The host's interval gate is modelled
/// here by the same rule the host applies (`_time_since_last_split <
/// split_merge_interval` refuses), because driving ten thousand real ticks
/// through `multi_raft` would measure the executor rather than the guard. That
/// the *host* actually applies that rule is checked separately, against the
/// real host, in `multi_raft_arbiter_unit_test`'s cooldown-gate cases; this
/// file checks what the rule is worth over ten thousand ticks.

#define BOOST_TEST_MODULE multi_raft_oscillation_test
#include <boost/test/unit_test.hpp>

#include <raft/split_merge_policy.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using kythira::composite_split_merge_policy;
using kythira::merge_direction;
using kythira::shard_stats;
using kythira::threshold_split_merge_policy;
using kythira::threshold_split_merge_policy_config;

namespace {

using key_type = std::string;
using group_type = std::uint64_t;
using stats_type = shard_stats<group_type, key_type>;
using threshold_type = threshold_split_merge_policy<group_type, key_type>;

constexpr std::size_t k_mib = 1024ULL * 1024;
constexpr int k_ticks = 10'000;
constexpr auto k_tick = std::chrono::milliseconds{10};

/// One shard in the simulated population.
struct sim_shard {
    std::size_t _bytes{0};
    std::size_t _keys{0};
    std::chrono::milliseconds _since_split{std::chrono::hours{24}};
    std::chrono::milliseconds _since_merge{std::chrono::hours{24}};
};

struct sim_result {
    std::uint64_t _splits{0};
    std::uint64_t _merges{0};
    std::uint64_t _gate_refusals{0};
    std::size_t _final_shard_count{0};
};

auto stats_of(const sim_shard& s) -> stats_type {
    stats_type out;
    out._size_available = true;
    out._approximate_size_bytes = s._bytes;
    out._approximate_key_count = s._keys;
    out._time_since_last_split = s._since_split;
    out._time_since_last_merge = s._since_merge;
    return out;
}

/// @brief Drive `policy` over a population for `k_ticks`, applying the host's
///        interval gate.
///
/// @param interval The host-level `split_merge_interval`. Zero removes the
///        guard entirely, which is how the test shows what it is worth.
template<typename Policy>
auto simulate(Policy& policy, std::vector<sim_shard> shards, std::chrono::milliseconds interval)
    -> sim_result {
    sim_result result;

    // The host refuses an operation on a shard that changed shape too recently.
    // This is Requirement 7.6's gate, and it is deliberately checked against
    // the shard's own history rather than a global clock: two quiet shards must
    // not be held back by a third busy one.
    const auto gated = [&](const sim_shard& s) {
        return interval > std::chrono::milliseconds::zero() &&
               (s._since_split < interval || s._since_merge < interval);
    };

    for (int tick = 0; tick < k_ticks; ++tick) {
        for (auto& s : shards) {
            s._since_split += k_tick;
            s._since_merge += k_tick;
        }

        // ── split ────────────────────────────────────────────────────────────
        std::vector<sim_shard> after_split;
        after_split.reserve(shards.size() * 2);
        for (const auto& s : shards) {
            const auto decision = policy.evaluate_split(stats_of(s));
            if (!decision.should_split()) {
                after_split.push_back(s);
                continue;
            }
            if (gated(s)) {
                ++result._gate_refusals;
                after_split.push_back(s);
                continue;
            }
            ++result._splits;
            sim_shard left{._bytes = s._bytes / 2,
                           ._keys = s._keys / 2,
                           ._since_split = std::chrono::milliseconds{0},
                           ._since_merge = s._since_merge};
            sim_shard right{._bytes = s._bytes - left._bytes,
                            ._keys = s._keys - left._keys,
                            ._since_split = std::chrono::milliseconds{0},
                            ._since_merge = s._since_merge};
            after_split.push_back(left);
            after_split.push_back(right);
        }
        shards = std::move(after_split);

        // ── merge, left-adjacent pairs ───────────────────────────────────────
        std::vector<sim_shard> after_merge;
        after_merge.reserve(shards.size());
        for (std::size_t i = 0; i < shards.size();) {
            if (i + 1 >= shards.size()) {
                after_merge.push_back(shards[i]);
                ++i;
                continue;
            }
            const auto decision =
                policy.evaluate_merge(stats_of(shards[i]), stats_of(shards[i + 1]));
            if (!decision.should_merge()) {
                after_merge.push_back(shards[i]);
                ++i;
                continue;
            }
            if (gated(shards[i]) || gated(shards[i + 1])) {
                ++result._gate_refusals;
                after_merge.push_back(shards[i]);
                ++i;
                continue;
            }
            ++result._merges;
            after_merge.push_back(sim_shard{
                ._bytes = shards[i]._bytes + shards[i + 1]._bytes,
                ._keys = shards[i]._keys + shards[i + 1]._keys,
                ._since_split = std::min(shards[i]._since_split, shards[i + 1]._since_split),
                ._since_merge = std::chrono::milliseconds{0}});
            i += 2;
        }
        shards = std::move(after_merge);
    }

    result._final_shard_count = shards.size();
    return result;
}

/// A population parked between the merge ceiling and the split floor.
auto parked_population(std::size_t bytes, std::size_t keys, std::size_t count)
    -> std::vector<sim_shard> {
    return std::vector<sim_shard>(count, sim_shard{._bytes = bytes, ._keys = keys});
}

auto contains(const std::vector<std::string>& haystack, const std::string& needle) -> bool {
    return std::any_of(haystack.begin(), haystack.end(),
                       [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_oscillation)

// ── run 1: the shipped defaults, parked between the thresholds ───────────────

BOOST_AUTO_TEST_CASE(the_shipped_policy_does_not_oscillate_over_ten_thousand_ticks,
                     *boost::unit_test::timeout(300)) {
    threshold_type policy;
    BOOST_REQUIRE(policy.validate());

    // 50 MiB and 500 000 keys: comfortably above the 20 MiB / 200 000 merge
    // ceiling and comfortably below the 96 MiB / 960 000 split floor. Nothing
    // should ever happen to these shards.
    auto shards = parked_population(50 * k_mib, 500'000, 8);
    const auto result = simulate(policy, shards, std::chrono::hours{1});

    BOOST_CHECK_EQUAL(result._splits, 0u);
    BOOST_CHECK_EQUAL(result._merges, 0u);
    BOOST_CHECK_EQUAL(result._final_shard_count, 8u);
}

BOOST_AUTO_TEST_CASE(a_shard_just_over_the_split_floor_settles_rather_than_cycling,
                     *boost::unit_test::timeout(300)) {
    // The interesting stability case: something does happen, and then stops.
    // One 100 MiB shard splits once into two 50 MiB shards, which are then
    // parked — above the merge ceiling, below the split floor.
    threshold_type policy;
    auto shards = parked_population(100 * k_mib, 500'000, 1);
    const auto result = simulate(policy, shards, std::chrono::hours{1});

    BOOST_CHECK_EQUAL(result._splits, 1u);
    BOOST_CHECK_EQUAL(result._merges, 0u);
    BOOST_CHECK_EQUAL(result._final_shard_count, 2u);
}

// ── run 2: a rejected configuration, forced past its own validate() ──────────

BOOST_AUTO_TEST_CASE(an_oscillating_configuration_is_rejected_by_validate) {
    threshold_split_merge_policy_config bad;
    bad._shard_merge_max_size_bytes = 60 * k_mib;
    bad._shard_split_size_bytes = 96 * k_mib;
    bad._shard_merge_max_keys = 600'000;
    bad._shard_split_keys = 960'000;

    const threshold_type policy{bad};
    BOOST_CHECK(!policy.validate());
    BOOST_CHECK(contains(policy.get_validation_errors(), "oscillat"));
}

BOOST_AUTO_TEST_CASE(without_the_host_gate_a_bad_configuration_really_does_oscillate,
                     *boost::unit_test::timeout(300)) {
    // The control. Defence in depth is only worth claiming if the thing it
    // defends against is demonstrated first — otherwise the next case proves
    // nothing but that a bound was not reached.
    threshold_split_merge_policy_config bad;
    bad._shard_merge_max_size_bytes = 60 * k_mib;
    bad._shard_split_size_bytes = 96 * k_mib;
    bad._shard_max_size_bytes = 144 * k_mib;
    bad._shard_merge_max_keys = 600'000;
    bad._shard_split_keys = 960'000;
    bad._shard_max_keys = 1'440'000;
    bad._split_merge_interval = std::chrono::milliseconds{0};
    threshold_type policy{bad};

    // Two shards at 59 MiB: each under the 60 MiB merge ceiling, and their sum
    // is over the 96 MiB split floor.
    auto shards = parked_population(59 * k_mib, 590'000, 2);
    const auto result = simulate(policy, shards, std::chrono::milliseconds{0});

    BOOST_CHECK_MESSAGE(
        result._merges > 1000u,
        "with no interval gate the pair should cycle freely; saw " << result._merges << " merges");
    BOOST_CHECK_GT(result._splits, 1000u);
    BOOST_CHECK_EQUAL(result._gate_refusals, 0u);
}

BOOST_AUTO_TEST_CASE(the_host_interval_gate_bounds_a_configuration_validate_would_have_rejected,
                     *boost::unit_test::timeout(300)) {
    // Requirement 7.6's defence in depth, demonstrated: the same configuration
    // as the control above, with `validate()` ignored, bounded purely by the
    // host's own interval.
    threshold_split_merge_policy_config bad;
    bad._shard_merge_max_size_bytes = 60 * k_mib;
    bad._shard_split_size_bytes = 96 * k_mib;
    bad._shard_max_size_bytes = 144 * k_mib;
    bad._shard_merge_max_keys = 600'000;
    bad._shard_split_keys = 960'000;
    bad._shard_max_keys = 1'440'000;
    // The policy's own cooldown is removed too, so the ONLY thing bounding the
    // count is the host.
    bad._split_merge_interval = std::chrono::milliseconds{0};
    threshold_type policy{bad};
    BOOST_REQUIRE(!policy.validate());

    constexpr auto interval = std::chrono::seconds{1};
    auto shards = parked_population(59 * k_mib, 590'000, 2);
    const auto result = simulate(policy, shards, interval);

    // Ten thousand ticks of 10 ms is 100 seconds of simulated time. One
    // interval is one second, and one full cycle is a merge plus a split, so
    // the ceiling is roughly one cycle per interval.
    constexpr std::uint64_t simulated_seconds = (k_ticks * 10) / 1000;
    const auto ceiling = static_cast<std::uint64_t>(simulated_seconds / interval.count()) + 2;

    BOOST_CHECK_MESSAGE(result._merges <= ceiling,
                        "merges " << result._merges << " exceeded the interval bound " << ceiling);
    BOOST_CHECK_MESSAGE(result._splits <= ceiling,
                        "splits " << result._splits << " exceeded the interval bound " << ceiling);
    // ...and the guard is the reason, not luck: it refused, repeatedly.
    BOOST_CHECK_GT(result._gate_refusals, 1000u);
}

// ── run 3: cross-member oscillation ──────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_composition_whose_members_interleave_is_rejected_naming_both) {
    // Neither member can see this on its own. Member A merges up to 60 MiB;
    // member B splits at 96 MiB. Both validate cleanly alone, and the pair
    // oscillates forever — moving real data through `split_state` and `absorb`
    // on every cycle.
    threshold_split_merge_policy_config merger;
    merger._shard_merge_max_size_bytes = 60 * k_mib;
    merger._shard_split_size_bytes = 200 * k_mib;
    merger._shard_max_size_bytes = 300 * k_mib;

    threshold_split_merge_policy_config splitter;
    splitter._shard_merge_max_size_bytes = 4 * k_mib;
    splitter._shard_split_size_bytes = 96 * k_mib;

    BOOST_REQUIRE(threshold_type{merger}.validate());
    BOOST_REQUIRE(threshold_type{splitter}.validate());

    composite_split_merge_policy<threshold_type, threshold_type> composite{
        threshold_type{merger}, threshold_type{splitter}};
    BOOST_CHECK(!composite.validate());

    const auto errors = composite.get_validation_errors();
    BOOST_CHECK(contains(errors, "member 0"));
    BOOST_CHECK(contains(errors, "member 1"));
    BOOST_CHECK(contains(errors, "oscillat"));
}

BOOST_AUTO_TEST_CASE(the_host_gate_bounds_an_interleaved_composition_too,
                     *boost::unit_test::timeout(300)) {
    // Forced past the composite's own `validate()`, exactly as the
    // single-policy case above. The host's interval knows nothing about
    // policies or compositions, which is the whole point of it living there.
    threshold_split_merge_policy_config merger;
    merger._shard_merge_max_size_bytes = 60 * k_mib;
    merger._shard_merge_max_keys = 600'000;
    merger._shard_split_size_bytes = 200 * k_mib;
    merger._shard_max_size_bytes = 300 * k_mib;
    merger._shard_split_keys = 2'000'000;
    merger._shard_max_keys = 3'000'000;
    merger._split_merge_interval = std::chrono::milliseconds{0};

    threshold_split_merge_policy_config splitter;
    splitter._shard_merge_max_size_bytes = 4 * k_mib;
    splitter._shard_merge_max_keys = 40'000;
    splitter._shard_split_size_bytes = 96 * k_mib;
    splitter._split_merge_interval = std::chrono::milliseconds{0};

    composite_split_merge_policy<threshold_type, threshold_type> composite{
        threshold_type{merger}, threshold_type{splitter}};
    BOOST_REQUIRE(!composite.validate());

    constexpr auto interval = std::chrono::seconds{1};
    auto shards = parked_population(59 * k_mib, 500'000, 2);
    const auto result = simulate(composite, shards, interval);

    constexpr std::uint64_t simulated_seconds = (k_ticks * 10) / 1000;
    const auto ceiling = static_cast<std::uint64_t>(simulated_seconds / interval.count()) + 2;
    BOOST_CHECK_MESSAGE(result._merges <= ceiling,
                        "merges " << result._merges << " exceeded the interval bound " << ceiling);
    BOOST_CHECK_MESSAGE(result._splits <= ceiling,
                        "splits " << result._splits << " exceeded the interval bound " << ceiling);
}

BOOST_AUTO_TEST_CASE(unanimous_merge_already_defuses_most_of_the_cross_member_hazard,
                     *boost::unit_test::timeout(300)) {
    // The second reason the merge rule is unanimity rather than any-wins: a
    // member that would SPLIT a shard will not consent to merging it, so the
    // composite refuses even before `validate()` is consulted. This is why the
    // interleaved case above needs its members' merge ceilings arranged so that
    // both agree — and why an accidental interleave is usually inert.
    threshold_split_merge_policy_config merger;
    merger._shard_merge_max_size_bytes = 60 * k_mib;
    merger._shard_merge_max_keys = 600'000;
    merger._shard_split_size_bytes = 200 * k_mib;
    merger._shard_max_size_bytes = 300 * k_mib;
    merger._shard_split_keys = 2'000'000;
    merger._shard_max_keys = 3'000'000;

    // This one's merge ceiling is far below the parked size, so it VETOES.
    threshold_split_merge_policy_config objector;
    objector._shard_merge_max_size_bytes = 1 * k_mib;
    objector._shard_merge_max_keys = 1000;
    objector._shard_split_size_bytes = 96 * k_mib;

    composite_split_merge_policy<threshold_type, threshold_type> composite{
        threshold_type{merger}, threshold_type{objector}};

    auto shards = parked_population(59 * k_mib, 500'000, 2);
    const auto result = simulate(composite, shards, std::chrono::milliseconds{0});

    BOOST_CHECK_EQUAL(result._merges, 0u);
    BOOST_CHECK_EQUAL(result._splits, 0u);
}

BOOST_AUTO_TEST_SUITE_END()
