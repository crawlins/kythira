// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file elastic_capacity_controller_test.cpp
/// @brief The elastic capacity controller against the deterministic mock
///        manager and a simulated cluster (tasks 8-12 and 14 of
///        `.kiro/specs/elastic-shard-capacity/`).
///
/// Every test runs on a manual clock and a manual executor: nothing happens
/// unless the test moves time or runs the queue, and there is no sleep
/// anywhere (Requirement 16.1). That is also what lets the most important
/// property be asserted directly — that `step()` never calls a provider and
/// never waits on one.

#define BOOST_TEST_MODULE elastic_capacity_controller_test
#include <boost/test/unit_test.hpp>

#include "elastic_capacity_harness.hpp"

#include <raft/capacity_ledger.hpp>
#include <raft/elastic_capacity_controller.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kythira::capacity_evidence;
using kythira::capacity_intent_kind;
using kythira::capacity_intent_state;
using kythira::capacity_reason;
using kythira::elastic_capacity_config;
using kythira::elastic_capacity_controller;
using kythira::memory_capacity_ledger;
using kythira::quorum_status;
using kythira::shard_operation_state;
using kythira::skipped_operator_reason;
using kythira::testing::cap_decision;
using kythira::testing::cap_group;
using kythira::testing::cap_key;
using kythira::testing::cap_node;
using kythira::testing::cap_operation;
using kythira::testing::cap_pg;
using kythira::testing::manual_clock;
using kythira::testing::manual_executor;
using kythira::testing::mock_provision_outcome;
using kythira::testing::scripted_capacity_policy;
using kythira::testing::simulated_cluster;
using kythira::testing::toggle_lease;

using kythira::testing::base_config;
using kythira::testing::evidence;
using kythira::testing::harness;
using kythira::testing::harness_t;
using kythira::testing::predecessor_intent;
using kythira::testing::scale_out;
using kythira::testing::seed;
using kythira::testing::three_zones;

using ledger_t = kythira::testing::cap_ledger;
using intent_t = kythira::testing::cap_intent;

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Task 8: the core loop, its bounds, the kill switch and dry run
// ═════════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_SUITE(controller_core)

BOOST_AUTO_TEST_CASE(invalid_configuration_is_refused_with_every_error) {
    harness h;
    h.cfg.max_in_flight_intents = 0;
    h.cfg.evaluation_interval = 0ms;
    h.policy.set_errors({"watermarks inverted"});
    try {
        h.start();
        BOOST_FAIL("expected the controller to refuse to start");
    } catch (const std::invalid_argument& e) {
        const std::string what = e.what();
        BOOST_CHECK(what.find("max_in_flight_intents") != std::string::npos);
        BOOST_CHECK(what.find("evaluation_interval") != std::string::npos);
        BOOST_CHECK(what.find("watermarks inverted") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(a_disabled_controller_checks_nothing_and_calls_nothing) {
    harness h;
    h.cfg.enabled = false;
    h.start();
    h.policy.repeat(scale_out());
    h.rounds(10);
    BOOST_CHECK(h.mgr.calls().empty());
    BOOST_CHECK_EQUAL(h.lease.checks(), 0U);
    BOOST_CHECK_EQUAL(h.policy.evaluations(), 0U);
}

BOOST_AUTO_TEST_CASE(step_never_calls_a_provider_and_never_waits_on_one) {
    // Requirement 13.1. The executor holds the work; a provision that takes
    // an hour leaves every step returning immediately.
    harness h;
    h.start();
    h.mgr.script({{._outcome = mock_provision_outcome::success, ._latency = 1h}});
    h.settle_in();
    h.policy.push(scale_out());

    const auto before = h.mgr.call_count("provision");
    h.round(10s, /*run_executor=*/false);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), before);  // not inline
    BOOST_CHECK_GE(h.ex.pending(), 1U);
    h.ex.run_all();
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), before + 1);

    // Many steps with the future pending: none blocks, none re-calls.
    for (int i = 0; i < 20; ++i) {
        h.round(10s);
    }
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), before + 1);
    BOOST_CHECK_EQUAL(h.count_state(capacity_intent_state::provisioning), 1U);
}

BOOST_AUTO_TEST_CASE(nothing_happens_without_the_lease_and_unknown_is_not_held) {
    harness h;
    h.start();
    h.lease.set_held(false);
    h.policy.repeat(scale_out());
    h.rounds(10);
    BOOST_CHECK(h.mgr.calls().empty());
    BOOST_CHECK_EQUAL(h.policy.evaluations(), 0U);

    h.lease.set_held(true);
    h.lease.set_throws(true);  // Requirement 7.4
    h.rounds(10);
    BOOST_CHECK(h.mgr.calls().empty());
    BOOST_CHECK(h.intents().empty());
}

BOOST_AUTO_TEST_CASE(the_kill_switch_stops_everything_within_one_interval) {
    harness h;
    h.cfg.kill_switch = true;
    h.start();
    h.policy.repeat(scale_out());
    h.rounds(12);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 0U);
    BOOST_CHECK_GE(h.counter("refused{bound=kill_switch}"), 1U);
    // Counted once per interval, not once per step.
    BOOST_CHECK_LE(h.counter("refused{bound=kill_switch}"), 3U);

    h.ctl->set_kill_switch(false);
    h.rounds(6);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 1U);

    // Flipped back on mid-flight: no further provider call, no operator.
    h.ctl->set_kill_switch(true);
    const auto calls = h.mgr.calls().size();
    h.rounds(10);
    BOOST_CHECK_EQUAL(h.mgr.calls().size(), calls);
    BOOST_CHECK(h.last_ops.empty());
}

BOOST_AUTO_TEST_CASE(dry_run_decides_logs_and_counts_but_calls_and_writes_nothing) {
    harness h;
    h.cfg.dry_run = true;
    h.start();
    h.settle_in();
    h.policy.push(scale_out());
    h.rounds(3);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 0U);
    BOOST_CHECK(h.intents().empty());
    BOOST_CHECK_EQUAL(h.counter("decision{action=scale_out,reason=density}"), 1U);
    const auto record = h.ctl->last_decision_record();
    BOOST_CHECK(record.find("dry_run=true") != std::string::npos);
    BOOST_CHECK(record.find("outcome=dry_run") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(max_cluster_size_is_a_named_refusal) {
    harness h;
    h.cfg.max_cluster_size = 3;  // already there
    h.start();
    h.settle_in();
    h.policy.push(scale_out());
    h.rounds(2);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 0U);
    BOOST_CHECK_EQUAL(h.counter("refused{bound=max_cluster_size}"), 1U);
    BOOST_CHECK(h.ctl->last_decision_record().find("outcome=refused:max_cluster_size") !=
                std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_policy_asking_for_many_machines_gets_one_at_a_time) {
    // Requirement 12.2: the bound is the controller's, whatever the policy.
    harness h;
    h.start();
    h.mgr.script({{._outcome = mock_provision_outcome::success, ._latency = 1h}});
    h.settle_in();
    h.policy.push(scale_out(5));
    h.rounds(2);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 1U);
    BOOST_CHECK_EQUAL(h.counter("refused{bound=max_in_flight_intents}"), 1U);
}

BOOST_AUTO_TEST_CASE(the_provider_call_interval_holds_whatever_the_policy_says) {
    harness h;
    h.cfg.min_provider_call_interval = 30min;
    h.cfg.max_in_flight_intents = 4;
    h.start();
    h.settle_in();
    h.policy.push(scale_out());
    h.rounds(2);
    h.clock.advance(1min);
    h.policy.push(scale_out());
    h.rounds(2);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 1U);
    BOOST_CHECK_EQUAL(h.counter("refused{bound=min_provider_call_interval}"), 1U);
}

BOOST_AUTO_TEST_CASE(the_rolling_budget_caps_provisioning_per_window) {
    harness h;
    h.cfg.max_provisions_per_window = 2;
    h.cfg.provision_window = 1h;
    h.cfg.max_in_flight_intents = 10;
    h.cfg.max_cluster_size = 50;
    h.start();
    h.settle_in();
    for (int i = 0; i < 4; ++i) {
        h.policy.push(scale_out());
        h.rounds(7);  // past the evaluation interval each time
    }
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 2U);
    BOOST_CHECK_GE(h.counter("refused{bound=provision_budget}"), 1U);

    // The window rolls on.
    h.clock.advance(1h);
    h.policy.push(scale_out());
    h.rounds(7);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 3U);
}

BOOST_AUTO_TEST_CASE(never_provision_on_lost_quorum) {
    // Requirement 13.4, mirroring raft.hpp's single-group rule.
    harness h;
    h.start();
    h.mgr.set_status_override(quorum_status::lost);
    h.settle_in();
    h.policy.push(scale_out());
    h.rounds(2);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 0U);
    BOOST_CHECK_EQUAL(h.counter("refused{bound=quorum_lost}"), 1U);
}

BOOST_AUTO_TEST_CASE(stale_reports_are_unknown_not_absent_and_never_grounds_to_grow) {
    // Requirement 2.4.
    harness h;
    h.start();
    h.settle_in();
    // Silence every host past the staleness window.
    h.sim.kill_host(1);
    h.sim.kill_host(2);
    h.sim.kill_host(3);
    h.rounds(5, 10s);
    const auto s = h.ctl->snapshot();
    BOOST_CHECK_EQUAL(s.cluster_size(), 3U);  // still counted
    BOOST_CHECK_EQUAL(s.fresh_node_count(), 0U);
    BOOST_CHECK(
        std::all_of(s._nodes.begin(), s._nodes.end(), [](const auto& n) { return n._stale; }));

    h.policy.push(scale_out());
    h.rounds(7);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 0U);
    BOOST_CHECK_EQUAL(h.counter("refused{bound=stale_inventory}"), 1U);
}

BOOST_AUTO_TEST_CASE(the_quorum_managers_view_of_liveness_wins) {
    // Requirement 2.5: a fresh heartbeat from a machine the provider says is
    // unreachable is not trusted.
    harness h;
    h.start();
    h.mgr.script({{._outcome = mock_provision_outcome::partial}});
    h.settle_in();
    h.policy.push(scale_out());
    h.rounds(2);
    const auto machines = h.mgr.machines();
    const auto created =
        std::find_if(machines.begin(), machines.end(), [](const auto& m) { return m._id >= 1000; });
    BOOST_REQUIRE(created != machines.end());
    // Pretend it heartbeats anyway.
    h.sim.add_host(created->_id, created->_group);
    h.rounds(8);  // past an assessment
    const auto s = h.ctl->snapshot();
    const auto v = std::find_if(s._nodes.begin(), s._nodes.end(),
                                [&](const auto& n) { return n.node_id() == created->_id; });
    BOOST_REQUIRE(v != s._nodes.end());
    BOOST_CHECK(v->_stale);
}

BOOST_AUTO_TEST_CASE(failures_back_off_and_consecutive_failures_open_the_circuit) {
    // Requirement 13.3.
    harness h;
    h.cfg.max_cluster_size = 50;
    h.cfg.provider_backoff.initial_delay = 2min;
    h.cfg.provider_backoff.max_attempts = 3;
    h.cfg.provider_backoff.jitter_factor = 0.0;
    h.cfg.circuit_cool_off = 1h;
    h.start();
    h.mgr.set_group_outcome("a", {._outcome = mock_provision_outcome::failure});
    h.mgr.set_group_outcome("b", {._outcome = mock_provision_outcome::failure});
    h.mgr.set_group_outcome("c", {._outcome = mock_provision_outcome::failure});
    h.settle_in();

    // One decision falls back across all three groups: three failures.
    h.policy.push(scale_out());
    h.rounds(6);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 3U);
    BOOST_CHECK(h.ctl->circuit_open());
    BOOST_CHECK_EQUAL(h.counter("circuit{event=opened}"), 1U);

    // While open, scale-out is suspended.
    h.policy.push(scale_out());
    h.rounds(7);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 3U);
    BOOST_CHECK_GE(h.counter("refused{bound=circuit_open}"), 1U);

    // After the cool-off one trial goes out; a success closes it.
    h.clock.advance(1h);
    h.mgr.set_group_outcome("a", {});
    h.mgr.set_group_outcome("b", {});
    h.mgr.set_group_outcome("c", {});
    h.policy.push(scale_out());
    h.rounds(7);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 4U);
    BOOST_CHECK(!h.ctl->circuit_open());
    BOOST_CHECK_EQUAL(h.counter("circuit{event=closed}"), 1U);
}

BOOST_AUTO_TEST_CASE(a_single_failure_backs_off_the_next_attempt) {
    harness h;
    h.cfg.max_cluster_size = 50;
    h.cfg.provider_backoff.initial_delay = 10min;
    h.cfg.provider_backoff.jitter_factor = 0.0;
    h.start();
    h.settle_in();
    h.policy.push(cap_decision::scale_out_in("a", 1, capacity_reason::density, evidence()));
    h.mgr.script({{._outcome = mock_provision_outcome::failure}});
    h.rounds(3);
    BOOST_CHECK_EQUAL(h.count_state(capacity_intent_state::failed), 1U);

    h.policy.push(scale_out());
    h.rounds(7);
    BOOST_CHECK_GE(h.counter("refused{bound=backoff}"), 1U);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 1U);
}

BOOST_AUTO_TEST_CASE(the_policy_is_not_consulted_inside_its_own_cooldown) {
    // Requirement 3.5.
    harness h;
    h.cfg.max_cluster_size = 50;
    h.start();
    h.policy.set_cooldown(30min);
    h.settle_in();
    h.policy.repeat(scale_out());
    h.rounds(7);
    const auto evaluations = h.policy.evaluations();
    h.rounds(60);  // ten minutes
    BOOST_CHECK_EQUAL(h.policy.evaluations(), evaluations);
}

BOOST_AUTO_TEST_CASE(a_split_is_a_trigger_edge) {
    // Requirement 5.1: evaluated now, not at the next interval.
    harness h;
    h.cfg.evaluation_interval = 1h;
    h.start();
    h.settle_in();
    h.round();  // the evaluation settle_in left due
    const auto before = h.policy.evaluations();
    h.rounds(3);
    BOOST_CHECK_EQUAL(h.policy.evaluations(), before);
    kythira::testing::cap_descriptor parent = h.sim.shard_of(1)._d;
    auto child = parent;
    child._group_id = 99;
    h.ctl->observe_split(parent, {parent, child});
    h.round();
    BOOST_CHECK_EQUAL(h.policy.evaluations(), before + 1);
    BOOST_CHECK_GE(h.ctl->snapshot()._splits_in_window, 1U);
}

BOOST_AUTO_TEST_CASE(a_newer_fencing_token_in_the_ledger_stops_this_controller) {
    // Requirement 7.6: detected where the mechanism allows it.
    harness h;
    h.lease.set(true, 5);
    h.start();
    intent_t foreign;
    foreign._key = "theirs";
    foreign._state = capacity_intent_state::requested;
    foreign._group = "a";
    foreign._fencing_token = 9;
    h.ledger.record(foreign);
    h.policy.repeat(scale_out());
    h.rounds(10);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 0U);
    BOOST_CHECK_GE(h.counter("fencing_conflict"), 1U);
}

BOOST_AUTO_TEST_SUITE_END()

// ═════════════════════════════════════════════════════════════════════════════
// Task 8 + 11: the whole scale-out path, end to end on the simulator
// ═════════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_SUITE(admission)

BOOST_AUTO_TEST_CASE(scale_out_provisions_admits_and_completes_with_a_voter) {
    harness h;
    h.start();
    h.settle_in();
    h.policy.push(scale_out());
    h.rounds(40);

    BOOST_REQUIRE_EQUAL(h.intents().size(), 1U);
    const auto i = h.intents().front();
    BOOST_CHECK(i._state == capacity_intent_state::completed);
    BOOST_REQUIRE(i._node.has_value());
    const auto node = *i._node;
    // Requirement 10.8: completed only holding a voting replica.
    std::size_t voting = 0;
    for (const auto& [g, s] : h.sim.shards()) {
        if (s._d.has_voter(node)) {
            ++voting;
        }
    }
    BOOST_CHECK_GE(voting, 1U);
    // Filled towards the mean: 18 replicas over 4 machines.
    BOOST_CHECK_GE(voting, 4U);
    BOOST_CHECK(!h.sim.quorum_ever_reduced());
    BOOST_CHECK_EQUAL(h.counter("intent{terminal_state=completed}"), 1U);
    BOOST_CHECK_EQUAL(h.counter("admission{outcome=completed}"), 1U);
    BOOST_CHECK_GE(h.counter("shards_moved"), 4U);
}

BOOST_AUTO_TEST_CASE(a_replica_is_added_as_learner_promoted_then_the_old_one_removed) {
    // Requirement 10.3: never remove before the replacement votes.
    harness h;
    h.start();
    h.settle_in();
    h.policy.push(scale_out());
    h.rounds(40);
    std::map<cap_group, std::vector<std::string>> steps;
    for (const auto& op : h.sim._accepted) {
        std::string s = op.name();
        if (const auto* a = std::get_if<kythira::add_replica_operator<cap_node>>(&op.kind())) {
            s += a->_as_learner ? ":learner" : ":promote";
        }
        steps[op.group_id()].push_back(s);
    }
    BOOST_REQUIRE(!steps.empty());
    for (const auto& [g, seq] : steps) {
        const auto learner = std::find(seq.begin(), seq.end(), "add_replica:learner");
        const auto promote = std::find(seq.begin(), seq.end(), "add_replica:promote");
        const auto remove = std::find(seq.begin(), seq.end(), "remove_replica");
        BOOST_REQUIRE(learner != seq.end());
        BOOST_REQUIRE(promote != seq.end());
        BOOST_CHECK(learner < promote);
        if (remove != seq.end()) {
            BOOST_CHECK(promote < remove);
        }
    }
}

BOOST_AUTO_TEST_CASE(moves_per_target_are_capped) {
    harness h;
    h.cfg.max_moves_per_target = 1;
    h.start();
    h.settle_in();
    h.policy.push(scale_out());
    for (int i = 0; i < 40; ++i) {
        h.round();
        BOOST_CHECK_LE(h.ctl->moves_in_flight(), 1U);
    }
}

BOOST_AUTO_TEST_CASE(non_stable_pending_down_and_cooling_shards_are_never_moved) {
    // Requirement 10.5.
    harness h(base_config(), three_zones(), 4);
    h.sim._catch_up_steps = 1;
    h.start();
    h.sim.set_operation(1, shard_operation_state::splitting);
    h.sim.set_operation(2, shard_operation_state::merging_source);
    h.settle_in();
    h.ctl->observe_split(h.sim.shard_of(3)._d, {h.sim.shard_of(3)._d});  // 3 cools down
    h.policy.push(scale_out());
    h.rounds(20);
    for (const auto& op : h.sim._accepted) {
        BOOST_CHECK(op.group_id() != 1);
        BOOST_CHECK(op.group_id() != 2);
        BOOST_CHECK(op.group_id() != 3);
    }
    // Only shard 4 was free to move.
    BOOST_CHECK(std::any_of(h.sim._accepted.begin(), h.sim._accepted.end(),
                            [](const auto& op) { return op.group_id() == 4; }));
}

BOOST_AUTO_TEST_CASE(shard_busy_backs_off_instead_of_resending) {
    // Requirement 13.6.
    harness h(base_config(), three_zones(), 1);
    h.start();
    h.settle_in();
    h.sim._force_skip[1] = skipped_operator_reason::shard_busy;
    h.policy.push(scale_out());
    h.rounds(6);
    const auto skips_after_first = h.sim._skipped.size();
    BOOST_CHECK_GE(skips_after_first, 1U);
    h.rounds(6);  // one minute: inside the two-minute busy back-off
    BOOST_CHECK_EQUAL(h.sim._skipped.size(), skips_after_first);
    h.rounds(12);
    BOOST_CHECK_GT(h.sim._skipped.size(), skips_after_first);
    BOOST_CHECK_GE(h.counter("operator_skipped{reason=shard_busy}"), 1U);
}

BOOST_AUTO_TEST_CASE(stale_epoch_replans_from_the_next_report) {
    harness h(base_config(), three_zones(), 1);
    h.start();
    h.settle_in();
    h.policy.push(scale_out());
    h.sim._force_skip[1] = skipped_operator_reason::stale_epoch;
    h.rounds(6);
    BOOST_CHECK_GE(h.counter("operator_skipped{reason=stale_epoch}"), 1U);
    h.sim._force_skip.clear();
    h.rounds(30);
    BOOST_CHECK_EQUAL(h.count_state(capacity_intent_state::completed), 1U);
}

BOOST_AUTO_TEST_CASE(an_unsupported_operator_abandons_the_move_and_rolls_back_the_learner) {
    harness h(base_config(), three_zones(), 1);
    h.start();
    h.settle_in();
    h.policy.push(scale_out());
    // Let the learner be added, then refuse everything.
    for (int i = 0; i < 30 && h.sim.shard_of(1)._d._learners.empty(); ++i) {
        h.round();
    }
    BOOST_REQUIRE(!h.sim.shard_of(1)._d._learners.empty());
    h.sim._force_skip[1] = skipped_operator_reason::unsupported;
    h.rounds(2);
    BOOST_CHECK_GE(h.counter("move{outcome=abandoned}"), 1U);
    h.sim._force_skip.clear();
    h.rounds(6);
    // The abandoned learner was taken back out: the group is as it was.
    BOOST_CHECK(h.sim.shard_of(1)._d._learners.empty());
}

BOOST_AUTO_TEST_CASE(an_admission_past_its_deadline_is_abandoned_and_the_machine_kept) {
    harness h(base_config(), three_zones(), 1);
    h.cfg.admit_deadline = 3min;
    h.start();
    h.settle_in();
    h.sim.set_operation(1, shard_operation_state::frozen);  // nothing can move
    h.policy.push(scale_out());
    h.rounds(30);
    BOOST_CHECK_EQUAL(h.count_state(capacity_intent_state::abandoned), 1U);
    BOOST_CHECK_EQUAL(h.counter("admission{outcome=abandoned}"), 1U);
    BOOST_CHECK_EQUAL(h.mgr.call_count("decommission"), 0U);
    BOOST_CHECK_EQUAL(h.mgr.live_created_count(), 1U);
}

BOOST_AUTO_TEST_CASE(lookup_descriptor_names_the_moves_target_before_the_report_does) {
    // Requirement 10.2: a new machine materialises its replica from the
    // first AppendEntries, so the descriptor it looks up must name it.
    harness h(base_config(), three_zones(), 1);
    h.start();
    h.settle_in();
    h.policy.push(scale_out());
    for (int i = 0; i < 30 && h.ctl->moves_in_flight() == 0; ++i) {
        h.round();
    }
    BOOST_REQUIRE_EQUAL(h.ctl->moves_in_flight(), 1U);
    const auto node = *h.intents().front()._node;
    const auto d = h.ctl->lookup_descriptor(1);
    BOOST_REQUIRE(d.has_value());
    BOOST_CHECK(d->has_replica(node));
    BOOST_CHECK(!h.ctl->lookup_descriptor(424242).has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// ═════════════════════════════════════════════════════════════════════════════
// Task 9: reconciliation and orphan reaping
// ═════════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_SUITE(reconciliation)

BOOST_AUTO_TEST_CASE(no_new_provider_call_until_reconciliation_finishes_or_times_out) {
    harness h;
    h.cfg.reconcile_deadline = 2min;
    h.start();
    h.policy.repeat(scale_out());
    // The executor never runs: the assessment never answers.
    for (int i = 0; i < 6; ++i) {
        h.round(10s, /*run_executor=*/false);
        BOOST_CHECK(h.ctl->reconciling());
    }
    BOOST_CHECK_EQUAL(h.policy.evaluations(), 0U);
    for (int i = 0; i < 8; ++i) {  // past the deadline, still unanswered
        h.round(10s, /*run_executor=*/false);
    }
    BOOST_CHECK(!h.ctl->reconciling());
    BOOST_CHECK_EQUAL(h.counter("reconcile{outcome=timed_out}"), 1U);
}

BOOST_AUTO_TEST_CASE(a_keyed_machine_that_joined_is_matched_and_admitted) {
    harness h;
    const auto pre = predecessor_intent(h.clock, "k-joined");
    h.ledger.record(pre);
    // The predecessor's call went out and the machine came up.
    static_cast<void>(h.mgr.provision_node_keyed("a", std::nullopt, "k-joined"));
    h.start();
    h.rounds(40);
    const auto i = h.ledger.find("k-joined");
    BOOST_REQUIRE(i.has_value());
    BOOST_CHECK(i->_state == capacity_intent_state::completed);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 1U);  // never re-provisioned
    BOOST_CHECK_GE(h.mgr.call_count("find_by_key"), 1U);
}

BOOST_AUTO_TEST_CASE(a_keyed_intent_whose_machine_is_nowhere_fails_and_backs_off) {
    // Requirement 8.5.
    harness h;
    h.cfg.provider_backoff.initial_delay = 20min;
    h.ledger.record(predecessor_intent(h.clock, "k-missing"));
    h.start();
    h.rounds(4);
    const auto i = h.ledger.find("k-missing");
    BOOST_REQUIRE(i.has_value());
    BOOST_CHECK(i->_state == capacity_intent_state::failed);
    BOOST_CHECK_EQUAL(h.counter("intent{terminal_state=failed}"), 1U);
    h.clock.advance(h.cfg.evaluation_interval);
    h.policy.push(scale_out());
    h.rounds(2);
    BOOST_CHECK_GE(h.counter("refused{bound=backoff}"), 1U);
}

BOOST_AUTO_TEST_CASE(a_keyed_machine_that_never_joined_is_reaped_after_its_deadline) {
    // Requirement 8.4.
    harness h;
    h.ledger.record(predecessor_intent(h.clock, "k-silent"));
    h.mgr.script({{._outcome = mock_provision_outcome::never_joins}});
    static_cast<void>(h.mgr.provision_node_keyed("a", std::nullopt, "k-silent"));
    h.start();
    h.rounds(6);
    BOOST_CHECK(h.ledger.find("k-silent")->_state == capacity_intent_state::provisioning);
    h.clock.advance(30min);
    h.rounds(6);
    const auto i = h.ledger.find("k-silent");
    BOOST_CHECK(i->_state == capacity_intent_state::orphaned);
    BOOST_CHECK_EQUAL(h.mgr.live_created_count(), 0U);
    BOOST_CHECK_EQUAL(h.counter("orphan_reaped"), 1U);
}

BOOST_AUTO_TEST_CASE(reaping_twice_is_harmless) {
    // Requirement 8.6: the concept's idempotency, not our bookkeeping.
    harness h;
    intent_t i = predecessor_intent(h.clock, "k-reap", capacity_intent_state::requested);
    h.ledger.record(i);
    h.mgr.script({{._outcome = mock_provision_outcome::partial}});
    const auto peer = std::move(h.mgr.provision_node_keyed("a", std::nullopt, "k-reap")).get();
    // A predecessor already reaped it once.
    static_cast<void>(h.mgr.decommission_node(peer.node_id));
    h.mgr.add_existing(peer.node_id, "a");  // the provider still lists it
    BOOST_REQUIRE(h.ledger.find("k-reap").has_value());
    h.ledger.transition("k-reap", capacity_intent_state::reaping,
                        {._node = peer.node_id, ._deadline = h.clock.now() + 10min}, h.clock.now());
    h.start();
    h.rounds(6);
    BOOST_CHECK(h.ledger.find("k-reap")->_state == capacity_intent_state::orphaned);
    BOOST_CHECK_GE(h.mgr.call_count("decommission"), 2U);
}

BOOST_AUTO_TEST_CASE(an_unkeyed_manager_still_reaps_by_join_deadline) {
    // No metadata surface: matched by the provider's view and the deadline.
    harness_t<false> h;
    seed(h, predecessor_intent(h.clock, "u-1"), capacity_intent_state::provisioning);
    h.mgr.script({{._outcome = mock_provision_outcome::partial}});
    static_cast<void>(h.mgr.provision_node("a", std::nullopt));
    h.start();
    h.rounds(4);
    BOOST_CHECK(!h.ledger.find("u-1")->terminal());
    h.clock.advance(40min);
    h.rounds(8);
    BOOST_CHECK(h.ledger.find("u-1")->_state == capacity_intent_state::orphaned);
    BOOST_CHECK_EQUAL(h.mgr.live_created_count(), 0U);
}

BOOST_AUTO_TEST_CASE(an_unkeyed_machine_that_joined_is_attributed_by_arrival) {
    harness_t<false> h;
    seed(h, predecessor_intent(h.clock, "u-2"), capacity_intent_state::provisioning);
    static_cast<void>(h.mgr.provision_node("a", std::nullopt));
    h.start();
    h.rounds(40);
    BOOST_CHECK(h.ledger.find("u-2")->_state == capacity_intent_state::completed);
    BOOST_CHECK(h.ledger.find("u-2")->_node.has_value());
}

BOOST_AUTO_TEST_CASE(a_result_arriving_after_the_lease_is_lost_is_not_acted_on) {
    // Requirement 7.3.
    harness h;
    h.start();
    h.mgr.script({{._outcome = mock_provision_outcome::success, ._latency = 5min}});
    h.settle_in();
    h.policy.push(scale_out());
    h.rounds(2);
    BOOST_REQUIRE_EQUAL(h.count_state(capacity_intent_state::provisioning), 1U);
    h.lease.set_held(false);
    h.rounds(40);  // the provision completes in here
    BOOST_CHECK_EQUAL(h.count_state(capacity_intent_state::provisioning), 1U);
    BOOST_CHECK_EQUAL(h.count_state(capacity_intent_state::provisioned), 0U);
}

BOOST_AUTO_TEST_SUITE_END()

// ═════════════════════════════════════════════════════════════════════════════
// Task 10: placement-group selection
// ═════════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_SUITE(placement)

namespace {
auto provisioned_groups(const auto& mgr) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& c : mgr.calls()) {
        if (c._op == "provision") {
            out.push_back(c._group);
        }
    }
    return out;
}
}  // namespace

BOOST_AUTO_TEST_CASE(scoring_table) {
    struct row {
        const char* _name;
        kythira::desired_topology<std::string> _topology;
        std::string _expected;
    };
    const std::vector<row> rows{
        // All at target, equal shares: tie, lowest id.
        {"tie", three_zones(), "a"},
        // b is declared twice as large but holds one of three: most
        // under-represented, and below its floor.
        {"under_target",
         {.groups = {{.group_id = "a", .target_count = 1},
                     {.group_id = "b", .target_count = 2},
                     {.group_id = "c", .target_count = 1}}},
         "b"},
        // Ratios above the floor: c's declared share is the largest.
        {"ratio",
         {.groups = {{.group_id = "a", .target_count = 1},
                     {.group_id = "b", .target_count = 1},
                     {.group_id = "c", .target_count = 1},
                     {.group_id = "d", .target_count = 0}}},
         "a"},
    };
    for (const auto& r : rows) {
        BOOST_TEST_CONTEXT(r._name) {
            harness h(base_config(), r._topology);
            h.start();
            h.settle_in();
            h.policy.push(scale_out());
            h.rounds(2);
            const auto groups = provisioned_groups(h.mgr);
            BOOST_REQUIRE_EQUAL(groups.size(), 1U);
            BOOST_CHECK_EQUAL(groups.front(), r._expected);
        }
    }
}

BOOST_AUTO_TEST_CASE(a_recent_refusal_ranks_a_group_down_and_decays) {
    harness h;
    h.cfg.max_cluster_size = 50;
    h.cfg.max_in_flight_intents = 5;
    h.cfg.provider_backoff.initial_delay = 1s;
    h.cfg.provider_backoff.jitter_factor = 0.0;
    h.cfg.placement_refusal_decay = 10min;
    h.start();
    h.settle_in();
    h.mgr.script({{._outcome = mock_provision_outcome::stock_out}});
    h.policy.push(scale_out());
    h.rounds(4);
    // a refused, fell back to b.
    auto groups = provisioned_groups(h.mgr);
    BOOST_REQUIRE_GE(groups.size(), 2U);
    BOOST_CHECK_EQUAL(groups[0], "a");
    BOOST_CHECK_EQUAL(groups[1], "b");
    BOOST_CHECK_EQUAL(h.counter("placement{outcome=fallback}"), 1U);
    // Both the refusal and the fallback are on record (Requirement 9.3).
    bool noted = false;
    for (const auto& i : h.intents()) {
        if (!i._attempts.empty() && i._attempts.front()._group == "a") {
            noted = true;
        }
    }
    BOOST_CHECK(noted);
}

BOOST_AUTO_TEST_CASE(a_stock_out_everywhere_tries_every_group_then_fails) {
    harness h;
    h.cfg.max_cluster_size = 50;
    h.start();
    for (const auto* g : {"a", "b", "c"}) {
        h.mgr.set_group_outcome(g, {._outcome = mock_provision_outcome::stock_out});
    }
    h.settle_in();
    h.policy.push(scale_out());
    h.rounds(6);
    const auto groups = provisioned_groups(h.mgr);
    BOOST_CHECK_EQUAL(groups.size(), 3U);
    BOOST_CHECK_EQUAL(h.count_state(capacity_intent_state::failed), 3U);
    BOOST_CHECK_EQUAL(h.mgr.live_created_count(), 0U);
}

BOOST_AUTO_TEST_CASE(never_a_group_the_topology_does_not_name) {
    // Requirement 9.4.
    harness h;
    h.start();
    h.settle_in();
    h.policy.push(cap_decision::scale_out_in("nowhere", 1, capacity_reason::manual, evidence()));
    h.rounds(2);
    BOOST_CHECK_EQUAL(h.mgr.call_count("provision"), 0U);
    BOOST_CHECK_EQUAL(h.counter("refused{bound=group_not_in_topology}"), 1U);
}

BOOST_AUTO_TEST_CASE(the_resizable_refinement_is_used_only_when_present) {
    using plain = kythira::testing::mock_capacity_quorum_manager<true, false>;
    using resizable = kythira::testing::mock_capacity_quorum_manager<true, true>;
    using unkeyed = kythira::testing::mock_capacity_quorum_manager<false, false>;
    static_assert(!kythira::resizable_quorum_manager<plain>);
    static_assert(kythira::resizable_quorum_manager<resizable>);
    static_assert(kythira::keyed_quorum_manager<plain>);
    static_assert(!kythira::keyed_quorum_manager<unkeyed>);

    harness_t<true, true> h;
    h.start();
    h.settle_in();
    h.policy.push(scale_out());
    h.rounds(3);
    BOOST_CHECK_EQUAL(h.mgr.call_count("set_group_target"), 1U);
    BOOST_CHECK_EQUAL(h.mgr.group_targets().at("a"), 2U);

    harness_t<true, false> p;
    p.start();
    p.settle_in();
    p.policy.push(scale_out());
    p.rounds(3);
    BOOST_CHECK_EQUAL(p.mgr.call_count("set_group_target"), 0U);
}

BOOST_AUTO_TEST_SUITE_END()

// ═════════════════════════════════════════════════════════════════════════════
// Task 12: scale-in and drain
// ═════════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_SUITE(drain)

namespace {
/// Four machines, the fourth carrying replicas, ready to be drained.
auto four_machines(elastic_capacity_config cfg = base_config()) -> std::unique_ptr<harness> {
    cfg.scale_in_enabled = true;
    auto h = std::make_unique<harness>(cfg);
    h->sim.add_host(4, "a");
    h->mgr.add_existing(4, "a");
    h->sim.add_shard(10, {4, 2, 3});
    h->sim.add_shard(11, {1, 4, 3});
    h->sim.set_leader(10, 4);
    return h;
}

auto scale_in(cap_node n, const std::string& group = "a") -> cap_decision {
    return cap_decision::scale_in(group, n, capacity_reason::density, evidence());
}
}  // namespace

BOOST_AUTO_TEST_CASE(a_drain_moves_leadership_then_replicas_then_decommissions) {
    auto h = four_machines();
    h->start();
    h->settle_in();
    h->policy.push(scale_in(4));
    h->rounds(60);
    BOOST_REQUIRE_EQUAL(h->intents().size(), 1U);
    BOOST_CHECK(h->intents().front()._state == capacity_intent_state::completed);
    for (const auto& [g, s] : h->sim.shards()) {
        BOOST_CHECK(!s._d.has_replica(4));
    }
    BOOST_CHECK_EQUAL(h->mgr.call_count("decommission"), 1U);
    BOOST_CHECK(!h->sim.quorum_ever_reduced());
    // Leadership moved off before any replica of 4 was removed.
    const auto& acc = h->sim._accepted;
    const auto first_transfer = std::find_if(acc.begin(), acc.end(), [](const auto& op) {
        return std::string(op.name()) == "transfer_leader";
    });
    const auto first_removal = std::find_if(acc.begin(), acc.end(), [](const auto& op) {
        const auto* r = std::get_if<kythira::remove_replica_operator<cap_node>>(&op.kind());
        return r != nullptr && r->_node == 4;
    });
    BOOST_REQUIRE(first_transfer != acc.end());
    BOOST_REQUIRE(first_removal != acc.end());
    BOOST_CHECK(first_transfer < first_removal);
    BOOST_CHECK_EQUAL(h->counter("drain{outcome=completed}"), 1U);
}

BOOST_AUTO_TEST_CASE(scale_in_is_off_by_default_independently_of_scale_out) {
    harness h;  // scale_in_enabled false
    h.sim.add_host(4, "a");
    h.mgr.add_existing(4, "a");
    h.start();
    h.settle_in();
    h.policy.push(cap_decision::scale_in("a", 4, capacity_reason::density, evidence()));
    h.rounds(3);
    BOOST_CHECK(h.intents().empty());
    BOOST_CHECK_EQUAL(h.counter("refused{bound=scale_in_disabled}"), 1U);
}

BOOST_AUTO_TEST_CASE(scale_in_needs_healthy_quorum) {
    auto h = four_machines();
    h->start();
    h->mgr.set_status_override(quorum_status::degraded);
    h->settle_in();
    h->policy.push(scale_in(4));
    h->rounds(3);
    BOOST_CHECK(h->intents().empty());
    BOOST_CHECK_EQUAL(h->counter("refused{bound=quorum_not_healthy}"), 1U);
}

BOOST_AUTO_TEST_CASE(scale_in_never_goes_below_the_floor_or_the_group_target) {
    auto cfg = base_config();
    cfg.min_cluster_size = 4;
    auto h = four_machines(cfg);
    h->start();
    h->settle_in();
    h->policy.push(scale_in(4));
    h->rounds(3);
    BOOST_CHECK_EQUAL(h->counter("refused{bound=min_cluster_size}"), 1U);

    // Group b is at its target of one: its only machine may not leave.
    auto g = four_machines();
    g->start();
    g->settle_in();
    g->policy.push(scale_in(2, "b"));
    g->rounds(3);
    BOOST_CHECK_EQUAL(g->counter("refused{bound=topology_floor}"), 1U);
}

BOOST_AUTO_TEST_CASE(one_machine_drains_at_a_time) {
    auto cfg = base_config();
    cfg.max_in_flight_intents = 4;
    auto h = four_machines(cfg);
    h->sim.add_host(5, "a");
    h->mgr.add_existing(5, "a");
    h->sim.set_operation(10, shard_operation_state::frozen);  // keep the drain going
    h->start();
    h->settle_in();
    h->policy.push(scale_in(4));
    h->rounds(7);
    h->policy.push(scale_in(5));
    h->rounds(7);
    BOOST_CHECK_EQUAL(h->intents().size(), 1U);
    BOOST_CHECK_EQUAL(h->counter("refused{bound=drain_in_progress}"), 1U);
}

BOOST_AUTO_TEST_CASE(a_drain_past_its_deadline_returns_the_machine_to_service) {
    // Requirement 11.5: never decommission a half-drained machine.
    auto cfg = base_config();
    cfg.drain_deadline = 4min;
    auto h = four_machines(cfg);
    h->sim.set_operation(11, shard_operation_state::frozen);  // 4 can never leave 11
    h->start();
    h->settle_in();
    h->policy.push(scale_in(4));
    h->rounds(40);
    BOOST_REQUIRE_EQUAL(h->intents().size(), 1U);
    BOOST_CHECK(h->intents().front()._state == capacity_intent_state::abandoned);
    BOOST_CHECK_EQUAL(h->mgr.call_count("decommission"), 0U);
    BOOST_CHECK(h->sim.shard_of(11)._d.has_replica(4));
    BOOST_CHECK_EQUAL(h->counter("drain{outcome=abandoned}"), 1U);
}

BOOST_AUTO_TEST_CASE(a_removal_waits_while_the_group_has_a_down_replica) {
    // Requirement 11.3, checked before each removal, not once at the start.
    auto h = four_machines();
    h->start();
    h->settle_in();
    h->policy.push(scale_in(4));
    h->rounds(3);
    h->sim.kill_host(3);  // a replica of 10 and 11 is now down
    h->rounds(20);
    for (const auto& op : h->sim._accepted) {
        if (const auto* r = std::get_if<kythira::remove_replica_operator<cap_node>>(&op.kind())) {
            BOOST_CHECK(r->_node != 4);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ═════════════════════════════════════════════════════════════════════════════
// Task 14: observability
// ═════════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_SUITE(observability)

BOOST_AUTO_TEST_CASE(a_decision_is_one_record_on_one_line) {
    // Requirement 14.3.
    harness h;
    h.start();
    h.settle_in();
    auto ev = evidence();
    ev._projection = kythira::capacity_projection{._current_shards_per_node = 150.0,
                                                  ._split_rate_per_minute = 2.0,
                                                  ._horizon_minutes = 30.0,
                                                  ._live_node_count = 3,
                                                  ._projected_shards_per_node = 210.0};
    h.policy.push(cap_decision::scale_out(1, capacity_reason::split_pressure, ev));
    h.rounds(2);
    const auto r = h.ctl->last_decision_record();
    BOOST_CHECK(r.find('\n') == std::string::npos);
    for (const auto* field :
         {"reason=split_pressure", "signals=shards_per_node=250/200", "projection=current=150",
          "projected=210", "group=a", "idempotency_key=cap-",
          "bounds=", "max_cluster_size=", "max_in_flight_intents=", "outcome=recorded"}) {
        BOOST_TEST_CONTEXT(field) {
            BOOST_CHECK(r.find(field) != std::string::npos);
        }
    }
    // The key in the record is the key in the ledger.
    BOOST_REQUIRE_EQUAL(h.intents().size(), 1U);
    BOOST_CHECK(r.find("idempotency_key=" + h.intents().front()._key) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(every_terminal_state_is_counted) {
    harness h;
    h.cfg.max_cluster_size = 50;
    h.cfg.max_in_flight_intents = 5;
    h.cfg.provider_backoff.initial_delay = 1s;
    h.cfg.provider_backoff.max_attempts = 100;
    h.cfg.provider_backoff.jitter_factor = 0.0;
    h.cfg.join_deadline = 2min;
    h.start();
    h.settle_in();
    // failed
    h.mgr.script({{._outcome = mock_provision_outcome::failure},
                  {._outcome = mock_provision_outcome::failure},
                  {._outcome = mock_provision_outcome::failure}});
    h.policy.push(scale_out());
    h.rounds(7);
    // orphaned: created, never joins
    h.mgr.script({{._outcome = mock_provision_outcome::never_joins}});
    h.policy.push(scale_out());
    h.rounds(40);
    // completed
    h.policy.push(scale_out());
    h.rounds(60);
    BOOST_CHECK_GE(h.counter("intent{terminal_state=failed}"), 1U);
    BOOST_CHECK_EQUAL(h.counter("intent{terminal_state=orphaned}"), 1U);
    BOOST_CHECK_GE(h.counter("intent{terminal_state=completed}"), 1U);
    BOOST_CHECK_GE(h.counter("provider_call{op=provision,outcome=ok}"), 1U);
    BOOST_CHECK_GE(h.counter("provider_call{op=provision,outcome=error}"), 1U);
    BOOST_CHECK_GE(h.counter("decision{action=scale_out,reason=density}"), 3U);
}

BOOST_AUTO_TEST_SUITE_END()
