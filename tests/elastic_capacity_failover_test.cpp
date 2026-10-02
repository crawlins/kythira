// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file elastic_capacity_failover_test.cpp
/// @brief Property, failover and chaos suites for the elastic capacity
///        controller (task 15 of `.kiro/specs/elastic-shard-capacity/`).
///
/// Everything runs on the manual clock and the manual executor of the shared
/// harness: no wall-clock sleep anywhere, and every random choice comes from a
/// seeded generator, so a failing seed replays exactly (Requirement 16.7).
///
/// - **Properties** (Requirement 16.2): over randomised decisions, provider
///   outcomes and split windows, after every round the cluster stays inside
///   [floor, ceiling], no group's voter count dips through an admission or a
///   drain, every machine the provider holds for us has an intent behind it,
///   and no operator ever reaches a shard mid-split.
/// - **Failover** (Requirement 16.3): the lease holder dies with its intent in
///   each non-terminal state; a successor over the same ledger and provider
///   reconciles to at most one machine per intent, with every orphan reaped.
/// - **Chaos** (Requirement 16.4): a provider call that never returns, a
///   machine that boots but never joins, the lease lost mid-admission.

#define BOOST_TEST_MODULE elastic_capacity_failover_test
#include <boost/test/unit_test.hpp>

#include "elastic_capacity_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kythira::capacity_intent_kind;
using kythira::capacity_intent_state;
using kythira::capacity_reason;
using kythira::elastic_capacity_config;
using kythira::shard_operation_state;
using kythira::testing::base_config;
using kythira::testing::cap_decision;
using kythira::testing::cap_group;
using kythira::testing::cap_intent;
using kythira::testing::cap_node;
using kythira::testing::evidence;
using kythira::testing::harness;
using kythira::testing::harness_t;
using kythira::testing::mock_provision_outcome;
using kythira::testing::scale_out;

/// Machines the provider still holds, ours or not.
template<typename H> auto live_machines(const H& h) -> std::size_t {
    const auto all = h.mgr.machines();
    return static_cast<std::size_t>(
        std::count_if(all.begin(), all.end(), [](const auto& m) { return !m._decommissioned; }));
}

/// Live provider machines created under `key`.
template<typename H> auto live_for_key(const H& h, const std::string& key) -> std::size_t {
    const auto all = h.mgr.machines();
    return static_cast<std::size_t>(std::count_if(all.begin(), all.end(), [&](const auto& m) {
        return !m._decommissioned && m._idempotency_key == key;
    }));
}

template<typename H> auto scale_out_intents(const H& h) -> std::vector<cap_intent> {
    std::vector<cap_intent> out;
    for (const auto& i : h.intents()) {
        if (i._kind == capacity_intent_kind::scale_out) {
            out.push_back(i);
        }
    }
    return out;
}

template<typename H> auto all_terminal(const H& h) -> bool {
    const auto all = h.intents();
    return std::all_of(all.begin(), all.end(), [](const auto& i) { return i.terminal(); });
}

/// Rounds until `pred` holds, at most `limit` of them; whether it did.
template<typename H>
auto rounds_until(H& h, const std::function<bool()>& pred, std::size_t limit,
                  bool run_executor = true) -> bool {
    for (std::size_t n = 0; n < limit; ++n) {
        if (pred()) {
            return true;
        }
        h.round(10s, run_executor);
    }
    return pred();
}

/// The only intent's state, if there is exactly one.
template<typename H> auto only_state(const H& h) -> std::optional<capacity_intent_state> {
    const auto all = h.intents();
    if (all.size() != 1) {
        return std::nullopt;
    }
    return all.front()._state;
}

// ═════════════════════════════════════════════════════════════════════════════
// Properties
// ═════════════════════════════════════════════════════════════════════════════

constexpr std::size_t floor_size = 3;
constexpr std::size_t ceiling_size = 6;

auto property_config() -> elastic_capacity_config {
    auto c = base_config();
    c.scale_in_enabled = true;
    c.min_cluster_size = floor_size;
    c.max_cluster_size = ceiling_size;
    c.provision_deadline = 3min;
    c.join_deadline = 5min;
    c.admit_deadline = 15min;
    c.drain_deadline = 15min;
    c.provider_backoff.initial_delay = 1min;
    c.provider_backoff.max_delay = 4min;
    c.circuit_cool_off = 5min;
    return c;
}

/// One seeded run: random decisions, provider outcomes and split windows,
/// every invariant checked after every round.
auto run_property_seed(std::uint32_t seed) -> void {
    BOOST_TEST_CONTEXT("seed " << seed) {
        harness h{property_config()};
        h.start();
        h.settle_in();
        std::mt19937 rng{seed};
        const auto chance = [&](double p) { return std::bernoulli_distribution{p}(rng); };
        const std::vector<std::string> zones{"a", "b", "c"};
        const auto pick = [&](std::size_t n) {
            return std::uniform_int_distribution<std::size_t>{0, n - 1}(rng);
        };
        std::map<cap_group, std::size_t> busy_for;

        for (std::size_t round = 0; round < 400; ++round) {
            // The policy's mind changes; the controller's bounds do not.
            if (chance(0.08)) {
                h.policy.push(scale_out(1 + pick(3)));
            }
            if (chance(0.05)) {
                std::vector<cap_node> hosts;
                for (const auto& r : h.sim.node_reports()) {
                    hosts.push_back(r._node_id);
                }
                const auto n = hosts[pick(hosts.size())];
                h.policy.push(cap_decision::scale_in(zones[pick(zones.size())], n,
                                                     capacity_reason::density, evidence()));
            }
            // The provider misbehaves sometimes. Scripts queue until a call
            // consumes one, so this is roughly one call in three.
            if (chance(0.025)) {
                static const std::vector<mock_provision_outcome> bad{
                    mock_provision_outcome::failure, mock_provision_outcome::stock_out,
                    mock_provision_outcome::partial, mock_provision_outcome::never_joins};
                h.mgr.script({{._outcome = bad[pick(bad.size())]}});
            }
            // Shards split for a while, then settle.
            if (chance(0.06)) {
                const auto g = static_cast<cap_group>(1 + pick(h.sim.shards().size()));
                h.sim.set_operation(g, shard_operation_state::splitting);
                busy_for[g] = 3 + pick(6);
            }
            for (auto it = busy_for.begin(); it != busy_for.end();) {
                if (--it->second == 0) {
                    h.sim.set_operation(it->first, shard_operation_state::stable);
                    it = busy_for.erase(it);
                } else {
                    ++it;
                }
            }

            h.round();

            BOOST_TEST_CONTEXT("round " << round) {
                BOOST_REQUIRE_LE(live_machines(h), ceiling_size);
                BOOST_REQUIRE_GE(h.sim.node_reports().size(), floor_size);
                BOOST_REQUIRE(!h.sim.quorum_ever_reduced());
                BOOST_REQUIRE_LE(h.mgr.live_created_count(), scale_out_intents(h).size());
                for (const auto& op : h.sim._sent_while_busy) {
                    BOOST_TEST_MESSAGE("busy: group " << op.group_id() << " " << op.name());
                }
                BOOST_REQUIRE(h.sim._sent_while_busy.empty());
            }
        }

        // Quiesce: nothing new, every split done, deadlines all passed.
        for (const auto& [g, _] : busy_for) {
            h.sim.set_operation(g, shard_operation_state::stable);
        }
        h.policy.repeat(std::nullopt);
        BOOST_REQUIRE(rounds_until(h, [&] { return all_terminal(h); }, 600));
        std::map<std::string, std::size_t> by_state;
        for (const auto& i : h.intents()) {
            ++by_state[std::string(kythira::to_string(i._kind)) + "/" +
                       std::string(kythira::to_string(i._state))];
        }
        for (const auto& [k, n] : by_state) {
            BOOST_TEST_MESSAGE("seed " << seed << " " << k << "=" << n);
        }
        // Every machine the provider still holds for us joined the cluster.
        for (const auto& m : h.mgr.machines()) {
            if (!m._decommissioned && !m._idempotency_key.empty()) {
                BOOST_CHECK_MESSAGE(h.sim.has_host(m._id),
                                    "machine " << m._id << " is live but never joined");
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Failover
// ═════════════════════════════════════════════════════════════════════════════

/// Kills the lease holder once its only intent is in `at`, starts a successor
/// with a newer fencing token over the same ledger and provider, and checks
/// what the successor converges to.
template<bool Keyed>
auto fail_over_in(capacity_intent_state at, kythira::testing::mock_provision_script script,
                  bool run_executor_before_kill) -> void {
    BOOST_TEST_CONTEXT("state " << kythira::to_string(at) << " keyed " << Keyed) {
        harness_t<Keyed> h;
        h.lease.set(true, 1);
        h.start();
        h.settle_in();
        h.mgr.script({script});
        h.policy.push(scale_out());
        BOOST_REQUIRE(
            rounds_until(h, [&] { return only_state(h) == at; }, 200, run_executor_before_kill));

        // The holder dies. Provider work it had queued still runs: a call
        // that went out is not recalled by its caller dying.
        h.ctl.reset();
        h.lease.set(true, 2);
        h.start();
        BOOST_REQUIRE(rounds_until(h, [&] { return all_terminal(h); }, 600));

        const auto intents = scale_out_intents(h);
        BOOST_REQUIRE_EQUAL(intents.size(), 1U);
        const auto& i = intents.front();
        if (Keyed) {
            BOOST_CHECK_LE(live_for_key(h, i._key), 1U);
        }
        if (i._state == capacity_intent_state::completed) {
            BOOST_REQUIRE(i._node.has_value());
            BOOST_CHECK(h.sim.has_host(*i._node));
            BOOST_CHECK_EQUAL(h.mgr.live_created_count(), 1U);
        } else {
            // Not completed: nothing of ours may be left running.
            BOOST_CHECK_EQUAL(h.mgr.live_created_count(), 0U);
        }
        // Never a second provision for one intent.
        BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 1U);
        BOOST_CHECK(!h.sim.quorum_ever_reduced());
    }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(properties)

BOOST_AUTO_TEST_CASE(bounds_quorum_attribution_and_stability_hold_on_every_round) {
    for (std::uint32_t seed = 1; seed <= 24; ++seed) {
        run_property_seed(seed);
    }
}

BOOST_AUTO_TEST_CASE(a_seed_replays_exactly) {
    // Determinism (Requirement 16.7): the same seed, the same ledger.
    const auto trace = [](std::uint32_t seed) {
        harness h{property_config()};
        h.start();
        h.settle_in();
        std::mt19937 rng{seed};
        for (int r = 0; r < 120; ++r) {
            if (std::bernoulli_distribution{0.1}(rng)) {
                h.policy.push(scale_out());
            }
            h.round();
        }
        std::vector<std::string> out;
        for (const auto& i : h.intents()) {
            out.push_back(i._key + ":" + std::string(kythira::to_string(i._state)));
        }
        out.push_back(std::to_string(h.mgr.call_count("provision")));
        return out;
    };
    BOOST_CHECK(trace(5) == trace(5));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(failover)

BOOST_AUTO_TEST_CASE(holder_dies_before_its_call_went_out) {
    // `requested`: recorded durably, then the holder died before the record's
    // commit reached it, so no call ever went out. A memory ledger commits in
    // the same step, so the predecessor's ledger is written directly.
    harness h;
    h.ledger.record(kythira::testing::predecessor_intent(h.clock, "k-never-called"));
    h.lease.set(true, 2);
    h.start();
    BOOST_REQUIRE(rounds_until(h, [&] { return all_terminal(h); }, 100));
    BOOST_CHECK(h.ledger.find("k-never-called")->_state == capacity_intent_state::failed);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 0U);
    BOOST_CHECK_EQUAL(h.mgr.live_created_count(), 0U);
}

BOOST_AUTO_TEST_CASE(holder_dies_with_the_call_queued_but_not_started) {
    // The executor never ran before the death: the call starts afterwards,
    // under nobody, and the successor still finds and uses that one machine.
    fail_over_in<true>(capacity_intent_state::provisioning,
                       {._outcome = mock_provision_outcome::success}, false);
}

BOOST_AUTO_TEST_CASE(holder_dies_with_the_call_in_flight) {
    fail_over_in<true>(capacity_intent_state::provisioning,
                       {._outcome = mock_provision_outcome::success, ._latency = 5min}, true);
}

BOOST_AUTO_TEST_CASE(holder_dies_after_the_machine_was_created) {
    fail_over_in<true>(capacity_intent_state::provisioned,
                       {._outcome = mock_provision_outcome::never_joins}, true);
}

BOOST_AUTO_TEST_CASE(holder_dies_mid_admission) {
    fail_over_in<true>(capacity_intent_state::admitting,
                       {._outcome = mock_provision_outcome::success}, true);
}

BOOST_AUTO_TEST_CASE(holder_dies_mid_reap) {
    fail_over_in<true>(capacity_intent_state::reaping,
                       {._outcome = mock_provision_outcome::never_joins}, true);
}

BOOST_AUTO_TEST_CASE(an_unkeyed_manager_fails_over_mid_admission) {
    fail_over_in<false>(capacity_intent_state::admitting,
                        {._outcome = mock_provision_outcome::success}, true);
}

BOOST_AUTO_TEST_CASE(an_unkeyed_manager_fails_over_with_a_machine_that_never_joins) {
    fail_over_in<false>(capacity_intent_state::provisioned,
                        {._outcome = mock_provision_outcome::partial}, true);
}

BOOST_AUTO_TEST_CASE(a_drain_resumes_under_the_successor) {
    auto cfg = base_config();
    cfg.scale_in_enabled = true;
    harness h{cfg};
    h.sim.add_host(4, "a");
    h.mgr.add_existing(4, "a");
    h.sim.add_shard(10, {4, 2, 3});
    h.sim.set_leader(10, 4);
    h.lease.set(true, 1);
    h.start();
    h.settle_in();
    h.policy.push(cap_decision::scale_in("a", 4, capacity_reason::density, evidence()));
    BOOST_REQUIRE(
        rounds_until(h, [&] { return only_state(h) == capacity_intent_state::draining; }, 100));
    h.round();  // part of the way through
    h.ctl.reset();
    h.lease.set(true, 2);
    h.start();
    BOOST_REQUIRE(rounds_until(h, [&] { return all_terminal(h); }, 300));
    BOOST_CHECK(h.intents().front()._state == capacity_intent_state::completed);
    for (const auto& [g, s] : h.sim.shards()) {
        BOOST_CHECK(!s._d.has_replica(4));
    }
    BOOST_CHECK(!h.sim.quorum_ever_reduced());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(chaos)

BOOST_AUTO_TEST_CASE(a_provider_call_that_never_returns_is_given_up_and_its_machine_reaped) {
    harness h;
    h.start();
    h.settle_in();
    h.mgr.script({{._outcome = mock_provision_outcome::never_returns}});
    h.policy.push(scale_out());
    h.rounds(3);
    BOOST_REQUIRE(only_state(h) == capacity_intent_state::provisioning);
    // Rounds keep completing: nothing in the step waits on the call.
    BOOST_REQUIRE(rounds_until(h, [&] { return all_terminal(h); }, 400));
    const auto i = h.intents().front();
    BOOST_CHECK(i._state == capacity_intent_state::orphaned ||
                i._state == capacity_intent_state::failed);
    // The machine the lost call created is found by key and torn down.
    BOOST_CHECK_EQUAL(h.mgr.live_created_count(), 0U);
}

BOOST_AUTO_TEST_CASE(a_machine_that_boots_but_never_joins_is_reaped_after_its_deadline) {
    harness h;
    h.start();
    h.settle_in();
    h.mgr.script({{._outcome = mock_provision_outcome::never_joins}});
    h.policy.push(scale_out());
    BOOST_REQUIRE(
        rounds_until(h, [&] { return only_state(h) == capacity_intent_state::provisioned; }, 50));
    BOOST_CHECK_EQUAL(h.mgr.live_created_count(), 1U);
    BOOST_REQUIRE(rounds_until(h, [&] { return all_terminal(h); }, 400));
    BOOST_CHECK(h.intents().front()._state == capacity_intent_state::orphaned);
    BOOST_CHECK_EQUAL(h.mgr.live_created_count(), 0U);
    BOOST_CHECK_EQUAL(h.counter("orphan_reaped"), 1U);
}

BOOST_AUTO_TEST_CASE(losing_the_lease_mid_admission_stops_moves_and_resumes_safely) {
    harness h;
    h.lease.set(true, 1);
    h.start();
    h.settle_in();
    h.policy.push(scale_out());
    BOOST_REQUIRE(
        rounds_until(h, [&] { return only_state(h) == capacity_intent_state::admitting; }, 80));
    h.lease.set_held(false);
    const auto accepted_before = h.sim._accepted.size();
    for (int r = 0; r < 20; ++r) {
        h.round();
        BOOST_CHECK(h.last_ops.empty());  // no holder, no operators
    }
    BOOST_CHECK_EQUAL(h.sim._accepted.size(), accepted_before);
    // Another controller held it meanwhile; this one returns with a newer token.
    h.lease.set(true, 3);
    BOOST_REQUIRE(rounds_until(h, [&] { return all_terminal(h); }, 300));
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 1U);
    BOOST_CHECK_LE(h.mgr.live_created_count(), 1U);
    BOOST_CHECK(!h.sim.quorum_ever_reduced());
}

BOOST_AUTO_TEST_SUITE_END()
