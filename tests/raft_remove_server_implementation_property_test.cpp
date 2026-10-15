// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * Property 93: Complete Remove Server Implementation
 *
 * remove_server() validates its request, then moves the voting set through
 * joint consensus (C_old -> C_old,new -> C_new) under the
 * ConfigurationSynchronizer, settles the returned future on every outcome,
 * and steps the leader down when it removed itself. Every case below drives
 * real nodes over the network simulator, on the cluster harness
 * raft_add_server_implementation_property_test uses.
 *
 * Most cases remove node 3 from {1, 2, 3} and hold the joint phase open by
 * cutting node 2 off first: the joint entry then reaches node 3, a majority
 * of C_old ({1, 3} of {1, 2, 3}) but not of C_new ({1} of {1, 2}). Raft must
 * not commit it, and the case can look at the cluster mid-change before
 * reconnecting node 2.
 *
 * Validates: Requirements 9.2, 9.3, 9.5, 17.2, 17.4, 23.5, 29.1, 29.2, 29.4,
 * 29.5
 */

#define BOOST_TEST_MODULE raft_remove_server_implementation_property_test
#include <boost/test/unit_test.hpp>
#include "raft_membership_test_cluster.hpp"

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("raft_remove_server_implementation_property_test"),
                         nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

using namespace kythira::test::membership;

namespace {

/// Starts remove_server(3) with node 2 cut off, so the joint entry cannot
/// commit (see the file comment). Returns the joint entry's index once node 3
/// holds it, which proves the entry left the leader.
auto hold_removal_in_joint_phase(cluster& c, outcome& removed) -> std::uint64_t {
    c.isolate(2);
    watch(c.leader().remove_server(3), removed);
    const auto entries = configuration_entries(c.leader());
    BOOST_REQUIRE(!entries.empty());
    BOOST_REQUIRE(entries.back().second.is_joint_consensus());
    const auto joint_index = entries.back().first;
    BOOST_REQUIRE(c.pump([&] {
        const auto on_3 = configuration_entries(c.node(3));
        return !on_3.empty() && on_3.back().first == joint_index;
    }));
    return joint_index;
}

auto release_removal(cluster& c) -> void {
    c.reconnect(2);
}

/// Elects node `id` by ticking its election timer, as the harness does for
/// node 1.
auto elect(cluster& c, std::uint64_t id) -> bool {
    return wait_until(
        [&] {
            std::this_thread::sleep_for(c.config()._election_timeout_max + 20ms);
            c.node(id).check_election_timeout();
            return wait_until([&] { return c.node(id).is_leader(); }, 500ms);
        },
        8000ms);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(remove_server_implementation)

BOOST_AUTO_TEST_CASE(property_remove_server_requires_leadership, *boost::unit_test::timeout(60)) {
    // Requirement 17.2: only the leader can start a configuration change.
    BOOST_TEST_MESSAGE("Property 93.1: Remove server requires leadership");
    cluster c;

    for (std::uint64_t follower : {2u, 3u}) {
        const auto log_size = c.node(follower).debug_state().log.size();
        outcome out;
        watch(c.node(follower).remove_server(3), out);
        BOOST_REQUIRE(wait_until([&] { return out.done(); }));
        BOOST_CHECK(out.failed);
        BOOST_CHECK(mentions(out.message(), "Not leader"));
        // Refused outright: nothing was appended and node 3 is still a voter.
        BOOST_CHECK_EQUAL(c.node(follower).debug_state().log.size(), log_size);
        BOOST_CHECK(contains(c.node(follower).current_membership().voters, 3));
    }

    // The leader accepts the same request.
    outcome out;
    watch(c.leader().remove_server(3), out);
    BOOST_REQUIRE(c.pump([&] { return out.done(); }, 10000ms));
    BOOST_CHECK(out.resolved);
    BOOST_CHECK(!contains(c.leader().current_membership().voters, 3));
}

BOOST_AUTO_TEST_CASE(property_remove_server_validates_node_exists, *boost::unit_test::timeout(60)) {
    // Requirement 23.5: removing a server that is not a voter is refused with
    // a reason, and changes nothing.
    BOOST_TEST_MESSAGE("Property 93.2: Remove server validates node exists");
    cluster c;
    const auto log_size = c.leader().debug_state().log.size();

    for (std::uint64_t stranger : {4u, 99u}) {
        outcome out;
        watch(c.leader().remove_server(stranger), out);
        BOOST_REQUIRE(wait_until([&] { return out.done(); }));
        BOOST_CHECK(out.failed);
        BOOST_CHECK_EQUAL(out.message(), "Node not in configuration");
    }
    BOOST_CHECK_EQUAL(c.leader().debug_state().log.size(), log_size);
    const auto view = c.leader().current_membership();
    BOOST_CHECK(!view.joint);
    BOOST_CHECK((view.voters == std::vector<std::uint64_t>{1, 2, 3}));
    BOOST_CHECK_EQUAL(c.logs(1).count("Cannot remove server: not in configuration"), 2u);

    // A refused request leaves nothing in progress: a valid one goes through.
    outcome valid;
    watch(c.leader().remove_server(3), valid);
    BOOST_REQUIRE(c.pump([&] { return valid.done(); }, 10000ms));
    BOOST_CHECK(valid.resolved);
}

BOOST_AUTO_TEST_CASE(property_remove_server_prevents_concurrent_changes,
                     *boost::unit_test::timeout(60)) {
    // Requirement 17.2: one configuration change at a time. While a removal
    // is in its joint phase, another removal, an addition and a promotion are
    // all refused, and none of them touches the log.
    BOOST_TEST_MESSAGE("Property 93.3: Remove server prevents concurrent changes");
    cluster c;
    outcome removed;
    hold_removal_in_joint_phase(c, removed);
    const auto log_size = c.leader().debug_state().log.size();

    auto expect_refused = [&](test_node::future_type f) {
        outcome out;
        watch(std::move(f), out);
        BOOST_REQUIRE(wait_until([&] { return out.done(); }));
        BOOST_CHECK(out.failed);
        BOOST_CHECK_EQUAL(out.message(), "Configuration change already in progress");
    };
    expect_refused(c.leader().remove_server(2));
    expect_refused(c.leader().remove_server(3));
    expect_refused(c.leader().add_server(4));
    BOOST_CHECK_EQUAL(c.leader().debug_state().log.size(), log_size);
    BOOST_CHECK(!removed.done());

    // Once the first change has settled, the next one is accepted.
    release_removal(c);
    BOOST_REQUIRE(c.pump([&] { return removed.done(); }, 10000ms));
    BOOST_CHECK(removed.resolved);
    outcome next;
    watch(c.leader().remove_server(2), next);
    BOOST_REQUIRE(c.pump([&] { return next.done(); }, 10000ms));
    BOOST_CHECK(next.resolved);
    BOOST_CHECK((c.leader().current_membership().voters == std::vector<std::uint64_t>{1}));
}

BOOST_AUTO_TEST_CASE(property_remove_server_enters_joint_consensus,
                     *boost::unit_test::timeout(90)) {
    // Requirement 29.1: the removal runs under C_old,new, which names both
    // voting sets, and the leader adopts it as soon as it is appended.
    BOOST_TEST_MESSAGE("Property 93.4: Remove server enters joint consensus");
    cluster c;
    outcome removed;
    const auto joint_index = hold_removal_in_joint_phase(c, removed);

    const auto view = c.leader().current_membership();
    BOOST_CHECK(view.joint);
    BOOST_CHECK((view.voters == std::vector<std::uint64_t>{1, 2}));

    const auto entries = configuration_entries(c.leader());
    const auto& joint = entries.back().second;
    BOOST_CHECK_EQUAL(entries.back().first, joint_index);
    BOOST_REQUIRE(joint.old_nodes().has_value());
    BOOST_CHECK((joint.nodes() == std::vector<std::uint64_t>{1, 2}));
    BOOST_CHECK((*joint.old_nodes() == std::vector<std::uint64_t>{1, 2, 3}));

    // The server being removed holds the same entry and runs under it too.
    BOOST_CHECK(c.pump([&] { return c.node(3).current_membership().joint; }));

    release_removal(c);
    BOOST_REQUIRE(c.pump([&] { return removed.done(); }, 10000ms));
    BOOST_CHECK(removed.resolved);
}

BOOST_AUTO_TEST_CASE(property_remove_server_waits_for_joint_consensus_commit,
                     *boost::unit_test::timeout(90)) {
    // Requirement 29.2: C_old,new needs a majority of both voting sets. A
    // majority of C_old alone (nodes 1 and 3) does not commit it, and C_new is
    // appended only once it commits.
    BOOST_TEST_MESSAGE("Property 93.5: Remove server waits for joint consensus commit");
    cluster c;
    outcome removed;
    const auto joint_index = hold_removal_in_joint_phase(c, removed);

    c.pump_for(600ms);
    BOOST_CHECK_LT(c.leader().debug_state().commit_index, joint_index);
    // Nothing after the joint entry: C_new waits for it to commit.
    BOOST_CHECK_EQUAL(c.leader().debug_state().log.back().index(), joint_index);
    BOOST_CHECK_EQUAL(c.logs(1).count("Appended C_new entry after joint commit"), 0u);
    BOOST_CHECK(!removed.done());

    release_removal(c);
    BOOST_REQUIRE(c.pump([&] { return removed.done(); }, 10000ms));
    BOOST_CHECK(removed.resolved);
    BOOST_CHECK_GE(c.leader().debug_state().commit_index, joint_index);
    const auto entries = configuration_entries(c.leader());
    BOOST_REQUIRE_GE(entries.size(), 2u);
    BOOST_CHECK_GT(entries.back().first, joint_index);
    BOOST_CHECK(!entries.back().second.is_joint_consensus());
}

BOOST_AUTO_TEST_CASE(property_remove_server_commits_final_configuration,
                     *boost::unit_test::timeout(90)) {
    // Requirement 29.4: C_new, without the removed server, is committed
    // before the caller's future resolves, and the remaining replicas end on
    // it.
    BOOST_TEST_MESSAGE("Property 93.6: Remove server commits final configuration");
    cluster c;
    outcome removed;
    const auto joint_index = hold_removal_in_joint_phase(c, removed);
    release_removal(c);
    BOOST_REQUIRE(c.pump([&] { return removed.done(); }, 10000ms));
    BOOST_REQUIRE(removed.resolved);
    BOOST_CHECK_EQUAL(removed.value_size.load(), 0u);

    const auto entries = configuration_entries(c.leader());
    const auto& [final_index, final_config] = entries.back();
    BOOST_CHECK_GT(final_index, joint_index);
    BOOST_CHECK(!final_config.is_joint_consensus());
    BOOST_CHECK((final_config.nodes() == std::vector<std::uint64_t>{1, 2}));
    BOOST_CHECK_GE(c.leader().debug_state().commit_index, final_index);
    BOOST_CHECK_EQUAL(c.leader().get_cluster_size(), 2u);

    for (std::uint64_t id : {1u, 2u}) {
        BOOST_CHECK(c.pump([&] {
            const auto v = c.node(id).current_membership();
            return !v.joint && v.voters == std::vector<std::uint64_t>{1, 2};
        }));
    }

    // The two remaining voters commit on their own: node 3 is no longer
    // needed, nor counted.
    c.isolate(3);
    outcome after;
    c.submit("after", after);
    BOOST_REQUIRE(c.pump([&] { return after.done(); }));
    BOOST_CHECK(after.resolved);
}

BOOST_AUTO_TEST_CASE(property_remove_server_leader_steps_down, *boost::unit_test::timeout(90)) {
    // Requirement 9.5: a leader that removes itself steps down once C_new
    // commits, and the remaining voters elect a leader among themselves.
    BOOST_TEST_MESSAGE("Property 93.7: Remove server causes leader step-down when removing self");
    cluster c;
    const auto term = c.leader().get_current_term();

    outcome removed;
    watch(c.leader().remove_server(1), removed);
    BOOST_REQUIRE(c.pump([&] { return removed.done(); }, 10000ms));
    BOOST_REQUIRE(removed.resolved);

    BOOST_CHECK(!c.leader().is_leader());
    BOOST_CHECK_EQUAL(c.logs(1).count("Leader removed from C_new, stepping down"), 1u);
    // Stepping down is not a new term: nobody has campaigned yet.
    BOOST_CHECK_EQUAL(c.leader().get_current_term(), term);
    BOOST_CHECK((c.metrics(1).dimension_values("raft_remove_server_started", "removing_self") ==
                 std::vector<std::string>{"true"}));

    // The remaining voters ran under C_new = {2, 3} and elect from it.
    for (std::uint64_t id : {2u, 3u}) {
        BOOST_CHECK(wait_until([&] {
            const auto v = c.node(id).current_membership();
            return !v.joint && v.voters == std::vector<std::uint64_t>{2, 3};
        }));
    }
    BOOST_REQUIRE(elect(c, 2));
    BOOST_CHECK_GT(c.node(2).get_current_term(), term);
    BOOST_CHECK(!c.node(1).is_leader());
    BOOST_CHECK(!c.node(3).is_leader());

    // The new leader commits with node 3 alone; node 1 is not consulted.
    c.isolate(1);
    outcome after;
    watch(c.node(2).submit_command(sm_t::make_put_command("after", "v"), 3000ms), after);
    BOOST_REQUIRE(wait_until(
        [&] {
            c.node(2).check_heartbeat_timeout();
            return after.done();
        },
        5000ms));
    BOOST_CHECK(after.resolved);
}

BOOST_AUTO_TEST_CASE(property_remove_server_handles_failures_with_rollback,
                     *boost::unit_test::timeout(90)) {
    // Requirement 29.5. A joint phase that cannot commit fails the caller once
    // the change's timeout (10 x append_entries_timeout) elapses, and only the
    // caller is released. The implementation does not roll the joint entry
    // back: it stays in the log (it may already sit on a majority of C_old),
    // so the cluster keeps requiring both majorities and refusing new changes
    // until it commits, and then completes the removal (rolls forward). What
    // this checks is that the failure is reported and that nothing unsafe
    // happens on the way: no C_new before the joint commit, no new change
    // admitted, and one consistent outcome afterwards.
    BOOST_TEST_MESSAGE("Property 93.8: Remove server handles failures with rollback");
    cluster c;  // timeout: 10 x 200 ms
    outcome removed;
    const auto start = std::chrono::steady_clock::now();
    const auto joint_index = hold_removal_in_joint_phase(c, removed);

    BOOST_REQUIRE(c.pump([&] { return removed.done(); }, 6000ms));
    BOOST_CHECK(removed.failed);
    BOOST_CHECK(mentions(removed.message(), "timed out"));
    BOOST_CHECK(std::chrono::steady_clock::now() - start >=
                c.config()._append_entries_timeout * 10);
    BOOST_CHECK_LT(c.leader().debug_state().commit_index, joint_index);
    BOOST_CHECK(c.leader().current_membership().joint);
    BOOST_CHECK(c.leader().is_leader());

    outcome busy;
    watch(c.leader().remove_server(2), busy);
    BOOST_REQUIRE(wait_until([&] { return busy.done(); }));
    BOOST_CHECK(busy.failed);
    BOOST_CHECK(mentions(busy.message(), "in progress"));

    release_removal(c);
    BOOST_REQUIRE(c.pump([&] {
        const auto v = c.leader().current_membership();
        return !v.joint && v.voters == std::vector<std::uint64_t>{1, 2};
    }));
    BOOST_CHECK(c.pump([&] {
        const auto v = c.node(2).current_membership();
        return !v.joint && v.voters == std::vector<std::uint64_t>{1, 2};
    }));

    // Settled: the next change is admitted.
    outcome next;
    watch(c.leader().remove_server(2), next);
    BOOST_REQUIRE(c.pump([&] { return next.done(); }, 10000ms));
    BOOST_CHECK(next.resolved);
}

BOOST_AUTO_TEST_CASE(property_remove_server_logs_progress, *boost::unit_test::timeout(90)) {
    // Requirement 17.4: each step is logged, naming the server; a refusal
    // and a failure each say what went wrong.
    BOOST_TEST_MESSAGE("Property 93.9: Remove server logs comprehensive progress");
    cluster c;
    auto& log = c.logs(1);

    outcome removed;
    watch(c.leader().remove_server(3), removed);
    BOOST_REQUIRE(c.pump([&] { return removed.done(); }, 10000ms));
    BOOST_REQUIRE(removed.resolved);
    for (const auto* step :
         {"Remove server requested", "Starting server removal with joint consensus",
          "Appended C_new entry after joint commit", "Remove server completed successfully"}) {
        BOOST_CHECK_MESSAGE(log.count(step) == 1, "expected one \"" << step << "\"");
    }
    BOOST_CHECK(
        (log.field_values("Remove server requested", "old_node") == std::vector<std::string>{"3"}));
    BOOST_CHECK((log.field_values("Starting server removal with joint consensus",
                                  "removing_self") == std::vector<std::string>{"false"}));
    BOOST_CHECK((log.field_values("Remove server completed successfully", "old_node") ==
                 std::vector<std::string>{"3"}));
    BOOST_CHECK_EQUAL(log.count("Leader removed from C_new, stepping down"), 0u);

    // A refusal is a warning naming the reason.
    outcome refused;
    watch(c.leader().remove_server(3), refused);
    BOOST_REQUIRE(wait_until([&] { return refused.done(); }));
    BOOST_CHECK(log.level_of("Cannot remove server: not in configuration") ==
                kythira::log_level::warning);

    // A change that times out is logged as an error.
    c.isolate(2);
    outcome lost;
    watch(c.leader().remove_server(2), lost);
    BOOST_REQUIRE(c.pump([&] { return lost.done(); }, 6000ms));
    BOOST_CHECK(lost.failed);
    BOOST_CHECK(log.level_of("Remove server failed") == kythira::log_level::error);
    BOOST_CHECK(
        (log.field_values("Remove server failed", "old_node") == std::vector<std::string>{"2"}));
}

BOOST_AUTO_TEST_CASE(property_remove_server_emits_metrics, *boost::unit_test::timeout(90)) {
    // Requirement 17.4: started, then success or failed, each naming the
    // server and the node that ran the change.
    BOOST_TEST_MESSAGE("Property 93.10: Remove server emits comprehensive metrics");
    cluster c;
    auto& m = c.metrics(1);

    outcome removed;
    watch(c.leader().remove_server(3), removed);
    BOOST_CHECK_EQUAL(m.count("raft_remove_server_started"), 1u);
    BOOST_REQUIRE(c.pump([&] { return removed.done(); }, 10000ms));
    BOOST_REQUIRE(removed.resolved);
    BOOST_CHECK_EQUAL(m.count("raft_remove_server_success"), 1u);
    BOOST_CHECK_EQUAL(m.count("raft_remove_server_failed"), 0u);
    BOOST_CHECK((m.dimension_values("raft_remove_server_started", "old_node") ==
                 std::vector<std::string>{"3"}));
    BOOST_CHECK((m.dimension_values("raft_remove_server_started", "removing_self") ==
                 std::vector<std::string>{"false"}));
    BOOST_CHECK((m.dimension_values("raft_remove_server_success", "old_node") ==
                 std::vector<std::string>{"3"}));
    BOOST_CHECK((m.dimension_values("raft_remove_server_success", "node_id") ==
                 std::vector<std::string>{"1"}));

    // A refusal is not a started change, so it emits nothing.
    outcome refused;
    watch(c.leader().remove_server(3), refused);
    BOOST_REQUIRE(wait_until([&] { return refused.done(); }));
    BOOST_CHECK_EQUAL(m.count("raft_remove_server_started"), 1u);

    // A change that times out: started, then failed.
    c.isolate(2);
    outcome lost;
    watch(c.leader().remove_server(2), lost);
    BOOST_REQUIRE(c.pump([&] { return lost.done(); }, 6000ms));
    BOOST_CHECK(lost.failed);
    BOOST_CHECK_EQUAL(m.count("raft_remove_server_started"), 2u);
    BOOST_CHECK((m.dimension_values("raft_remove_server_failed", "old_node") ==
                 std::vector<std::string>{"2"}));
    BOOST_CHECK_EQUAL(m.count("raft_remove_server_success"), 1u);
}

BOOST_AUTO_TEST_CASE(property_remove_server_uses_configuration_synchronizer,
                     *boost::unit_test::timeout(90)) {
    // Requirement 17.2: the synchronizer holds the change from the joint
    // entry until C_new commits, and the caller's future is its future: it
    // stays pending across the joint commit and resolves only at C_new.
    BOOST_TEST_MESSAGE("Property 93.11: Remove server uses ConfigurationSynchronizer");
    cluster c;
    outcome removed;
    const auto joint_index = hold_removal_in_joint_phase(c, removed);

    auto try_add = [&] {
        outcome out;
        watch(c.leader().add_server(4), out);
        BOOST_REQUIRE(wait_until([&] { return out.done(); }));
        return out.failed ? out.message() : std::string{};
    };

    c.pump_for(200ms);
    BOOST_CHECK(!removed.done());
    BOOST_CHECK_EQUAL(try_add(), "Configuration change already in progress");

    release_removal(c);
    std::uint64_t commit_when_resolved = 0;
    BOOST_REQUIRE(c.pump(
        [&] {
            if (removed.done()) {
                commit_when_resolved = c.leader().debug_state().commit_index;
                return true;
            }
            return false;
        },
        10000ms));
    BOOST_REQUIRE(removed.resolved);
    const auto entries = configuration_entries(c.leader());
    BOOST_CHECK_GE(commit_when_resolved, entries.back().first);
    BOOST_CHECK_GT(entries.back().first, joint_index);

    // Released: the next change is admitted.
    outcome next;
    watch(c.leader().remove_server(2), next);
    BOOST_REQUIRE(c.pump([&] { return next.done(); }, 10000ms));
    BOOST_CHECK(next.resolved);
}

BOOST_AUTO_TEST_CASE(property_remove_server_cancels_pending_operations_on_step_down,
                     *boost::unit_test::timeout(90)) {
    // Requirement 9.5: a leader that removes itself leaves no client waiting.
    //
    // Commands are replicated in log order, so one appended before C_new
    // commits with it and resolves normally; the step-down goes through
    // become_follower(), which fails anything still registered as leadership
    // lost (the leadership_lost metric below is emitted only on that path).
    // So this checks the observable contract: every future submitted around
    // the change settles, the step-down runs exactly once, and once the
    // leader has stepped down it refuses new commands rather than accepting
    // work it can never commit. "Exactly once" caught a real race: the
    // removal's own continuation used to step down a second time, without
    // the node's lock, and could cancel and emit twice.
    BOOST_TEST_MESSAGE("Property 93.12: Remove server cancels operations on step-down");
    cluster c;

    // Hold the self-removal's joint phase: with node 3 cut off, C_new = {2, 3}
    // has no majority.
    c.isolate(3);
    outcome removed;
    watch(c.leader().remove_server(1), removed);
    outcome during;
    c.submit("during", during);
    c.pump_for(300ms);
    BOOST_CHECK(!removed.done());
    BOOST_CHECK(!during.done());

    c.reconnect(3);
    BOOST_REQUIRE(c.pump([&] { return removed.done() && during.done(); }, 10000ms));
    BOOST_CHECK(removed.resolved);
    BOOST_CHECK(during.resolved);
    BOOST_CHECK(!c.leader().is_leader());
    BOOST_CHECK_EQUAL(c.metrics(1).count("leadership_lost"), 1u);
    BOOST_CHECK_EQUAL(c.logs(1).count("Leadership lost, cancelling pending operations"), 1u);

    outcome refused;
    watch(c.leader().submit_command(sm_t::make_put_command("late", "v"), 1000ms), refused);
    BOOST_REQUIRE(wait_until([&] { return refused.done(); }));
    BOOST_CHECK(refused.failed);
    BOOST_CHECK_EQUAL(refused.message(), "Not leader");
}

BOOST_AUTO_TEST_SUITE_END()
