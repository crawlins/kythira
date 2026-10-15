// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * Property 92: Complete Add Server Implementation
 *
 * add_server() admits the new server as a learner, promotes it with joint
 * consensus once it has caught up (C_old -> C_old,new -> C_new), validates
 * its inputs, and settles the returned future on every outcome. Every case
 * below drives real nodes over the network simulator.
 *
 * Several cases hold the joint phase open on purpose: once the new server
 * has caught up, node 3 and the new server stop receiving messages, so the
 * joint entry reaches a majority of C_old ({1, 2} of {1, 2, 3}) but
 * not of C_new ({1, 2} of {1, 2, 3, 4}). Raft must not commit it, and the
 * test can look at the cluster mid-change before reconnecting.
 *
 * Validates: Requirements 9.2, 9.3, 9.4, 13.7, 17.1, 17.3, 18.2, 23.2, 23.4,
 * 24.3, 25.1, 29.1, 29.2, 29.3
 */

#define BOOST_TEST_MODULE raft_add_server_implementation_property_test
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
        char* argv0[] = {const_cast<char*>("raft_add_server_implementation_property_test"),
                         nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

using namespace kythira::test::membership;

BOOST_AUTO_TEST_SUITE(add_server_implementation)

BOOST_AUTO_TEST_CASE(property_add_server_requires_leadership, *boost::unit_test::timeout(60)) {
    // Requirement 17.3: only the leader can start a configuration change.
    BOOST_TEST_MESSAGE("Property 92.1: Add server requires leadership");
    cluster c;
    c.spare();

    for (std::uint64_t follower : {2u, 3u}) {
        const auto log_size = c.node(follower).debug_state().log.size();
        outcome out;
        watch(c.node(follower).add_server(4), out);
        BOOST_REQUIRE(wait_until([&] { return out.done(); }));
        BOOST_CHECK(out.failed);
        BOOST_CHECK(mentions(out.message(), "Not leader"));
        // Refused outright: nothing was appended and node 4 is nowhere.
        BOOST_CHECK_EQUAL(c.node(follower).debug_state().log.size(), log_size);
        const auto view = c.node(follower).current_membership();
        BOOST_CHECK(!contains(view.voters, 4));
        BOOST_CHECK(!contains(view.learners, 4));
    }

    // The leader accepts the same request.
    outcome out;
    watch(c.leader().add_server(4), out);
    BOOST_REQUIRE(c.pump([&] { return out.done(); }, 10000ms));
    BOOST_CHECK(out.resolved);
}

BOOST_AUTO_TEST_CASE(property_add_server_validates_duplicate_nodes,
                     *boost::unit_test::timeout(60)) {
    // Requirement 23.4: adding a server that is already a member is refused.
    BOOST_TEST_MESSAGE("Property 92.2: Add server validates duplicate nodes");
    cluster c;
    const auto log_size = c.leader().debug_state().log.size();

    for (std::uint64_t existing : {1u, 2u, 3u}) {
        outcome out;
        watch(c.leader().add_server(existing), out);
        BOOST_REQUIRE(wait_until([&] { return out.done(); }));
        BOOST_CHECK(out.failed);
        BOOST_CHECK(mentions(out.message(), "already in configuration"));
    }
    BOOST_CHECK_EQUAL(c.leader().debug_state().log.size(), log_size);
    BOOST_CHECK_EQUAL(c.leader().get_cluster_size(), 3u);

    // A server added once is a duplicate the second time.
    c.spare();
    outcome first;
    watch(c.leader().add_server(4), first);
    BOOST_REQUIRE(c.pump([&] { return first.done(); }, 10000ms));
    BOOST_REQUIRE(first.resolved);
    outcome again;
    watch(c.leader().add_server(4), again);
    BOOST_REQUIRE(wait_until([&] { return again.done(); }));
    BOOST_CHECK(again.failed);
    BOOST_CHECK(mentions(again.message(), "already in configuration"));
}

BOOST_AUTO_TEST_CASE(property_add_server_prevents_concurrent_changes,
                     *boost::unit_test::timeout(60)) {
    // Requirement 17.3: one configuration change at a time, whether the first
    // is still catching its learner up or already in joint consensus.
    BOOST_TEST_MESSAGE("Property 92.3: Add server prevents concurrent changes");
    cluster c;
    c.spare(/*reachable=*/false);

    outcome pending;
    watch(c.leader().add_server(4), pending);

    auto expect_refused = [&](test_node::future_type f) {
        outcome out;
        watch(std::move(f), out);
        BOOST_REQUIRE(wait_until([&] { return out.done(); }));
        BOOST_CHECK(out.failed);
        BOOST_CHECK(mentions(out.message(), "already in progress"));
    };

    // Catch-up phase.
    BOOST_CHECK(!pending.done());
    expect_refused(c.leader().add_server(5));
    expect_refused(c.leader().remove_server(3));
    expect_refused(c.leader().promote_to_voter(4));
    const auto view = c.leader().current_membership();
    BOOST_CHECK(!contains(view.learners, 5));
    BOOST_CHECK(contains(view.voters, 3));

    // Once the first change has settled, the next one is accepted.
    c.link_all({1, 2, 3, 4});
    BOOST_REQUIRE(c.pump([&] { return pending.done(); }, 10000ms));
    BOOST_CHECK(pending.resolved);
    outcome next;
    watch(c.leader().remove_server(4), next);
    BOOST_REQUIRE(c.pump([&] { return next.done(); }, 10000ms));
    BOOST_CHECK(next.resolved);
}

BOOST_AUTO_TEST_CASE(property_add_server_enters_joint_consensus, *boost::unit_test::timeout(90)) {
    // Requirement 29.1: the promotion runs under C_old,new, which names both
    // voting sets.
    BOOST_TEST_MESSAGE("Property 92.4: Add server enters joint consensus");
    cluster c;
    outcome added;
    const auto joint_index = c.hold_in_joint_phase(added);

    const auto view = c.leader().current_membership();
    BOOST_CHECK(view.joint);
    BOOST_CHECK(contains(view.voters, 4));
    BOOST_CHECK(!contains(view.learners, 4));

    const auto entries = configuration_entries(c.leader());
    const auto& joint = entries.back().second;
    BOOST_CHECK_EQUAL(entries.back().first, joint_index);
    BOOST_REQUIRE(joint.old_nodes().has_value());
    BOOST_CHECK((joint.nodes() == std::vector<std::uint64_t>{1, 2, 3, 4}));
    BOOST_CHECK((*joint.old_nodes() == std::vector<std::uint64_t>{1, 2, 3}));

    // The follower that received it holds the same entry.
    BOOST_CHECK(c.pump([&] {
        const auto on_2 = configuration_entries(c.node(2));
        return !on_2.empty() && on_2.back().first == joint_index &&
               on_2.back().second.is_joint_consensus();
    }));

    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_CHECK(added.resolved);
}

BOOST_AUTO_TEST_CASE(property_add_server_waits_for_joint_consensus_commit,
                     *boost::unit_test::timeout(90)) {
    // Requirement 29.2: C_new is appended only after C_old,new commits.
    BOOST_TEST_MESSAGE("Property 92.5: Add server waits for joint consensus commit");
    cluster c;
    outcome added;
    const auto joint_index = c.hold_in_joint_phase(added);

    c.pump_for(600ms);
    BOOST_CHECK_LT(c.leader().debug_state().commit_index, joint_index);
    // Nothing after the joint entry: C_new waits for it to commit.
    BOOST_CHECK_EQUAL(c.leader().debug_state().log.back().index(), joint_index);
    BOOST_CHECK(!added.done());

    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_CHECK(added.resolved);
    const auto entries = configuration_entries(c.leader());
    BOOST_REQUIRE_GE(entries.size(), 2u);
    BOOST_CHECK_GT(entries.back().first, joint_index);
    BOOST_CHECK(!entries.back().second.is_joint_consensus());
}

BOOST_AUTO_TEST_CASE(property_add_server_commits_final_configuration,
                     *boost::unit_test::timeout(90)) {
    // Requirement 29.3: once C_old,new commits, C_new is committed and the
    // caller's future resolves only after that.
    BOOST_TEST_MESSAGE("Property 92.6: Add server commits final configuration");
    cluster c;
    outcome added;
    const auto joint_index = c.hold_in_joint_phase(added);
    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_REQUIRE(added.resolved);

    const auto entries = configuration_entries(c.leader());
    const auto& [final_index, final_config] = entries.back();
    BOOST_CHECK_GT(final_index, joint_index);
    BOOST_CHECK(!final_config.is_joint_consensus());
    BOOST_CHECK((final_config.nodes() == std::vector<std::uint64_t>{1, 2, 3, 4}));
    BOOST_CHECK(!contains(final_config.learners(), 4));
    BOOST_CHECK_GE(c.leader().debug_state().commit_index, final_index);
    BOOST_CHECK_EQUAL(c.leader().get_cluster_size(), 4u);

    // Every replica, the new one included, ends on C_new.
    for (std::uint64_t id : {1u, 2u, 3u, 4u}) {
        BOOST_CHECK(c.pump([&] {
            const auto v = c.node(id).current_membership();
            return !v.joint && v.voters.size() == 4 && contains(v.voters, 4);
        }));
    }
}

BOOST_AUTO_TEST_CASE(property_add_server_requires_majority_in_both_configs,
                     *boost::unit_test::timeout(90)) {
    // Requirement 9.3: under C_old,new an entry commits only with a majority of
    // each set. {1, 2} is a majority of C_old but not of C_new, so neither the
    // joint entry nor a client command behind it may commit. (The converse,
    // a C_new majority without a C_old one, cannot arise when one server is
    // added.)
    BOOST_TEST_MESSAGE("Property 92.7: Add server requires majority in both configs");
    cluster c;
    outcome added;
    const auto joint_index = c.hold_in_joint_phase(added);

    outcome cmd;
    c.submit("during-joint", cmd);
    c.pump_for(600ms);
    // Node 2 holds both entries: C_old's majority alone is not enough.
    BOOST_CHECK(wait_until([&] { return c.node(2).debug_state().log.size() > joint_index; }));
    BOOST_CHECK_LT(c.leader().debug_state().commit_index, joint_index);
    BOOST_CHECK(!cmd.done());
    BOOST_CHECK(!added.done());

    // With node 4 back, {1, 2, 4} is a majority of C_new as well.
    c.reconnect(4);
    BOOST_REQUIRE(c.pump([&] { return cmd.done() && added.done(); }, 10000ms));
    BOOST_CHECK(cmd.resolved);
    BOOST_CHECK(added.resolved);
    c.reconnect(3);
}

BOOST_AUTO_TEST_CASE(property_add_server_uses_joint_consensus_protocol,
                     *boost::unit_test::timeout(90)) {
    // Requirement 9.2: the log records the whole change, in order: the learner
    // entry, C_old,new, then C_new.
    BOOST_TEST_MESSAGE("Property 92.8: Add server uses joint consensus protocol");
    cluster c;
    const auto before = configuration_entries(c.leader()).size();
    c.spare();
    outcome added;
    watch(c.leader().add_server(4), added);
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_REQUIRE(added.resolved);

    auto entries = configuration_entries(c.leader());
    BOOST_REQUIRE_EQUAL(entries.size(), before + 3);
    entries.erase(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(before));
    const auto& learner = entries[0].second;
    const auto& joint = entries[1].second;
    const auto& final_config = entries[2].second;

    BOOST_CHECK(!learner.is_joint_consensus());
    BOOST_CHECK((learner.nodes() == std::vector<std::uint64_t>{1, 2, 3}));
    BOOST_CHECK(contains(learner.learners(), 4));

    BOOST_CHECK(joint.is_joint_consensus());
    BOOST_CHECK((joint.nodes() == std::vector<std::uint64_t>{1, 2, 3, 4}));
    BOOST_REQUIRE(joint.old_nodes().has_value());
    BOOST_CHECK((*joint.old_nodes() == std::vector<std::uint64_t>{1, 2, 3}));

    BOOST_CHECK(!final_config.is_joint_consensus());
    BOOST_CHECK((final_config.nodes() == std::vector<std::uint64_t>{1, 2, 3, 4}));
    BOOST_CHECK(!contains(final_config.learners(), 4));

    BOOST_CHECK_LT(entries[0].first, entries[1].first);
    BOOST_CHECK_LT(entries[1].first, entries[2].first);
}

BOOST_AUTO_TEST_CASE(property_add_server_adds_as_non_voting_initially,
                     *boost::unit_test::timeout(120)) {
    // Requirement 9.4: until it has caught up the new server is a learner,
    // outside the quorum. With node 3 cut off as well, {1, 2} still commits:
    // a majority of three voters, which it would not be if node 4 counted.
    BOOST_TEST_MESSAGE("Property 92.9: Add server adds as non-voting initially");
    cluster c;
    c.spare(/*reachable=*/false);
    outcome added;
    watch(c.leader().add_server(4), added);

    auto view = c.leader().current_membership();
    BOOST_CHECK(contains(view.learners, 4));
    BOOST_CHECK(!contains(view.voters, 4));
    BOOST_CHECK(!view.joint);
    BOOST_CHECK_EQUAL(c.leader().get_cluster_size(), 3u);

    c.isolate(3);
    outcome cmd;
    c.submit("without-the-learner", cmd);
    BOOST_REQUIRE(c.pump([&] { return cmd.done(); }));
    BOOST_CHECK(cmd.resolved);
    BOOST_CHECK(wait_until([&] { return contains(c.node(2).current_membership().learners, 4); }));
    BOOST_CHECK(!added.done());
    c.reconnect(3);

    // Reachable now: it catches up, then votes.
    c.link_all({1, 2, 3, 4});
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_CHECK(added.resolved);
    view = c.leader().current_membership();
    BOOST_CHECK(contains(view.voters, 4));
    BOOST_CHECK(!contains(view.learners, 4));
}

BOOST_AUTO_TEST_CASE(property_add_server_uses_configurable_retry_policy,
                     *boost::unit_test::timeout(90)) {
    // Requirement 23.2: how long add_server() keeps retrying a server that
    // makes no progress is ten of the configured append_entries_timeout, not
    // a hard-coded constant.
    BOOST_TEST_MESSAGE("Property 92.10: Add server uses configurable retry policy");
    for (auto aet : {100ms, 300ms}) {
        auto cfg = make_fast_config();
        cfg._append_entries_timeout = aet;
        cluster c{cfg};
        c.spare(/*reachable=*/false);

        outcome added;
        const auto start = std::chrono::steady_clock::now();
        watch(c.leader().add_server(4), added);
        BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
        const auto took = std::chrono::steady_clock::now() - start;

        BOOST_CHECK(added.failed);
        BOOST_CHECK(mentions(added.message(), "no replication progress"));
        BOOST_CHECK(took >= aet * 10);
        // Generous upper bound: the budget plus scheduling slack.
        BOOST_CHECK(took < aet * 10 + 2000ms);
    }
}

BOOST_AUTO_TEST_CASE(property_add_server_validates_configuration, *boost::unit_test::timeout(60)) {
    // Requirement 23.4: each refusal says why, and is logged as a warning.
    BOOST_TEST_MESSAGE("Property 92.11: Add server validates configuration");
    cluster c;

    outcome not_leader;
    watch(c.node(2).add_server(4), not_leader);
    outcome duplicate;
    watch(c.leader().add_server(2), duplicate);
    c.spare(/*reachable=*/false);
    outcome first;
    watch(c.leader().add_server(4), first);
    outcome busy;
    watch(c.leader().add_server(5), busy);

    BOOST_REQUIRE(wait_until([&] { return not_leader.done() && duplicate.done() && busy.done(); }));
    BOOST_CHECK(not_leader.failed);
    BOOST_CHECK_EQUAL(not_leader.message(), "Not leader - cannot add server");
    BOOST_CHECK(duplicate.failed);
    BOOST_CHECK_EQUAL(duplicate.message(), "Node already in configuration");
    BOOST_CHECK(busy.failed);
    BOOST_CHECK_EQUAL(busy.message(), "Configuration change already in progress");

    BOOST_CHECK(c.logs(2).level_of("Cannot add server: not leader") == kythira::log_level::warning);
    BOOST_CHECK(c.logs(1).level_of("Cannot add server: already in configuration") ==
                kythira::log_level::warning);
    BOOST_CHECK(c.logs(1).level_of("Cannot add server: configuration change already in progress") ==
                kythira::log_level::warning);
    // The warnings name the server that was refused.
    const auto dup_nodes =
        c.logs(1).field_values("Cannot add server: already in configuration", "new_node");
    BOOST_CHECK((dup_nodes == std::vector<std::string>{"2"}));
}

BOOST_AUTO_TEST_CASE(property_add_server_uses_two_phase_protocol, *boost::unit_test::timeout(90)) {
    // Requirement 17.1: the phases are observable in order on a follower too,
    // learner first, then joint, then final, and the caller hears of success
    // only after the last.
    BOOST_TEST_MESSAGE("Property 92.12: Add server uses two-phase protocol");
    cluster c;
    outcome added;
    const auto joint_index = c.hold_in_joint_phase(added);
    // Phase one is under way on node 2: it holds the joint entry, and C_new
    // does not exist anywhere yet.
    BOOST_REQUIRE(c.pump([&] { return c.node(2).debug_state().log.size() >= joint_index; }));
    BOOST_CHECK_EQUAL(c.leader().debug_state().log.back().index(), joint_index);
    BOOST_CHECK(!added.done());

    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_CHECK(added.resolved);
    const auto final_index = c.leader().debug_state().commit_index;
    BOOST_REQUIRE(c.pump([&] { return c.node(2).debug_state().last_applied >= final_index; }));
    const auto v = c.node(2).current_membership();
    BOOST_CHECK(!v.joint);
    BOOST_CHECK(contains(v.voters, 4));

    // Node 2 applied the three entries in order.
    const auto applied = c.logs(2).field_values("Applied configuration entry", "is_joint");
    BOOST_REQUIRE_GE(applied.size(), 3u);
    BOOST_CHECK_EQUAL(applied[applied.size() - 3], "false");
    BOOST_CHECK_EQUAL(applied[applied.size() - 2], "true");
    BOOST_CHECK_EQUAL(applied[applied.size() - 1], "false");
}

BOOST_AUTO_TEST_CASE(property_add_server_handles_leadership_loss, *boost::unit_test::timeout(90)) {
    // Requirement 17.3: a leader that is deposed while its new server is
    // catching up fails the caller on its next tick rather than leaving the
    // future pending, and the server never becomes a voter.
    BOOST_TEST_MESSAGE("Property 92.13: Add server handles leadership loss");
    cluster c;
    c.spare(/*reachable=*/false);
    outcome added;
    watch(c.leader().add_server(4), added);
    c.pump_for(100ms);
    BOOST_REQUIRE(!added.done());

    depose_leader(c);
    c.leader().check_heartbeat_timeout();
    BOOST_REQUIRE(wait_until([&] { return added.done(); }, 2000ms));
    BOOST_CHECK(added.failed);
    BOOST_CHECK(mentions(added.message(), "leadership lost"));
    BOOST_CHECK(!contains(c.leader().current_membership().voters, 4));
    BOOST_CHECK_EQUAL(c.metrics(1).count("raft_add_server_failed"), 1u);
}

BOOST_AUTO_TEST_CASE(property_add_server_emits_metrics, *boost::unit_test::timeout(60)) {
    // Requirement 13.7: started, then success or failed, each naming the
    // server; the promotion's own pair in between.
    BOOST_TEST_MESSAGE("Property 92.14: Add server emits metrics");
    cluster c;
    auto& m = c.metrics(1);

    c.spare();
    outcome added;
    watch(c.leader().add_server(4), added);
    BOOST_CHECK_EQUAL(m.count("raft_add_server_started"), 1u);
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_REQUIRE(added.resolved);
    BOOST_CHECK_EQUAL(m.count("raft_promote_to_voter_started"), 1u);
    BOOST_CHECK_EQUAL(m.count("raft_promote_to_voter_success"), 1u);
    BOOST_CHECK_EQUAL(m.count("raft_add_server_success"), 1u);
    BOOST_CHECK_EQUAL(m.count("raft_add_server_failed"), 0u);
    BOOST_CHECK((m.dimension_values("raft_add_server_started", "new_node") ==
                 std::vector<std::string>{"4"}));
    BOOST_CHECK((m.dimension_values("raft_add_server_success", "new_node") ==
                 std::vector<std::string>{"4"}));
    BOOST_CHECK((m.dimension_values("raft_add_server_success", "node_id") ==
                 std::vector<std::string>{"1"}));

    // A server that never answers: started, then failed.
    outcome lost;
    watch(c.leader().add_server(5), lost);
    BOOST_REQUIRE(c.pump([&] { return lost.done(); }, 10000ms));
    BOOST_CHECK(lost.failed);
    BOOST_CHECK_EQUAL(m.count("raft_add_server_started"), 2u);
    BOOST_CHECK((m.dimension_values("raft_add_server_failed", "new_node") ==
                 std::vector<std::string>{"5"}));
    BOOST_CHECK_EQUAL(m.count("raft_add_server_success"), 1u);
}

BOOST_AUTO_TEST_CASE(property_add_server_logs_progress, *boost::unit_test::timeout(60)) {
    // Requirement 24.3: each step is logged, and a failure says why.
    BOOST_TEST_MESSAGE("Property 92.15: Add server logs progress");
    cluster c;
    auto& log = c.logs(1);

    c.spare();
    outcome added;
    watch(c.leader().add_server(4), added);
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_REQUIRE(added.resolved);
    for (const auto* step :
         {"Add server requested", "Starting server addition: catching up as a learner first",
          "New server caught up; promoting it with joint consensus",
          "Starting learner promotion with joint consensus",
          "Appended C_new entry after joint commit", "Add server completed successfully"}) {
        BOOST_CHECK_MESSAGE(log.count(step) == 1, "expected one \"" << step << "\"");
    }
    BOOST_CHECK((log.field_values("Add server completed successfully", "new_node") ==
                 std::vector<std::string>{"4"}));

    outcome lost;
    watch(c.leader().add_server(5), lost);
    BOOST_REQUIRE(c.pump([&] { return lost.done(); }, 10000ms));
    BOOST_CHECK(lost.failed);
    BOOST_CHECK(log.level_of("Add server abandoned") == kythira::log_level::warning);
    const auto reasons = log.field_values("Add server abandoned", "reason");
    BOOST_REQUIRE_EQUAL(reasons.size(), 1u);
    BOOST_CHECK(mentions(reasons[0], "no replication progress"));
}

BOOST_AUTO_TEST_CASE(property_add_server_handles_replication_failures,
                     *boost::unit_test::timeout(90)) {
    // Requirement 18.2: replication to the new server is retried while it is
    // unreachable; an outage shorter than the stall budget only delays the
    // change.
    BOOST_TEST_MESSAGE("Property 92.16: Add server handles replication failures");
    cluster c;  // stall budget: 10 x 200 ms
    c.spare(/*reachable=*/false);
    outcome added;
    watch(c.leader().add_server(4), added);

    c.pump_for(800ms);
    BOOST_CHECK(!added.done());
    BOOST_CHECK_EQUAL(c.node(4).debug_state().log.size(), 0u);

    c.link_all({1, 2, 3, 4});
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_CHECK(added.resolved);
    BOOST_CHECK(contains(c.leader().current_membership().voters, 4));
    BOOST_CHECK(c.pump([&] {
        return c.node(4).debug_state().last_applied >= c.leader().debug_state().last_applied;
    }));
}

BOOST_AUTO_TEST_CASE(property_add_server_synchronizes_with_config_synchronizer,
                     *boost::unit_test::timeout(90)) {
    // Requirement 17.1: while the synchronizer holds the joint phase, every
    // other membership change is refused; once C_new commits it lets go.
    BOOST_TEST_MESSAGE("Property 92.17: Add server synchronizes with ConfigurationSynchronizer");
    cluster c;
    outcome added;
    c.hold_in_joint_phase(added);

    auto expect_refused = [&](test_node::future_type f) {
        outcome out;
        watch(std::move(f), out);
        BOOST_REQUIRE(wait_until([&] { return out.done(); }));
        BOOST_CHECK(out.failed);
        BOOST_CHECK_EQUAL(out.message(), "Configuration change already in progress");
    };
    expect_refused(c.leader().add_server(5));
    expect_refused(c.leader().remove_server(2));
    expect_refused(c.leader().promote_to_voter(4));

    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_REQUIRE(added.resolved);

    outcome removed;
    watch(c.leader().remove_server(4), removed);
    BOOST_REQUIRE(c.pump([&] { return removed.done(); }, 10000ms));
    BOOST_CHECK(removed.resolved);
    BOOST_CHECK(!contains(c.leader().current_membership().voters, 4));
}

BOOST_AUTO_TEST_CASE(property_add_server_returns_future_on_completion,
                     *boost::unit_test::timeout(90)) {
    // Requirement 25.1: the future is pending until the change completes,
    // then holds an empty value; a failure arrives as its exception.
    BOOST_TEST_MESSAGE("Property 92.18: Add server returns future on completion");
    cluster c;
    outcome added;
    c.hold_in_joint_phase(added);
    c.pump_for(200ms);
    BOOST_CHECK(!added.done());

    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 10000ms));
    BOOST_CHECK(added.resolved);
    BOOST_CHECK_EQUAL(added.value_size.load(), 0u);
    BOOST_CHECK_GE(c.leader().get_cluster_size(), 4u);

    outcome lost;
    watch(c.leader().add_server(5), lost);
    BOOST_REQUIRE(c.pump([&] { return lost.done(); }, 10000ms));
    BOOST_CHECK(lost.failed);
    BOOST_CHECK(!lost.message().empty());
}

BOOST_AUTO_TEST_CASE(property_add_server_handles_timeout, *boost::unit_test::timeout(90)) {
    // Requirement 23.2: a joint phase that cannot commit fails the caller once
    // the change's timeout (10 x append_entries_timeout) elapses. The joint
    // entry stays in the log, so new changes are still refused until it
    // commits; when it does, the leader completes the change anyway.
    BOOST_TEST_MESSAGE("Property 92.19: Add server handles timeout");
    cluster c;  // timeout: 10 x 200 ms
    outcome added;
    const auto start = std::chrono::steady_clock::now();
    c.hold_in_joint_phase(added);

    BOOST_REQUIRE(c.pump([&] { return added.done(); }, 6000ms));
    BOOST_CHECK(added.failed);
    BOOST_CHECK(mentions(added.message(), "timed out"));
    BOOST_CHECK(std::chrono::steady_clock::now() - start >=
                c.config()._append_entries_timeout * 10);

    outcome busy;
    watch(c.leader().add_server(5), busy);
    BOOST_REQUIRE(wait_until([&] { return busy.done(); }));
    BOOST_CHECK(busy.failed);
    BOOST_CHECK(mentions(busy.message(), "in progress"));

    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] {
        const auto v = c.leader().current_membership();
        return !v.joint && contains(v.voters, 4);
    }));

    outcome next;
    watch(c.leader().remove_server(4), next);
    BOOST_REQUIRE(c.pump([&] { return next.done(); }, 10000ms));
    BOOST_CHECK(next.resolved);
}

BOOST_AUTO_TEST_CASE(property_add_server_maintains_safety_during_transition,
                     *boost::unit_test::timeout(120)) {
    // Requirement 9.3: across the whole change, with commands committed
    // before, during and after it, there is one leader and every replica
    // agrees on every committed entry.
    BOOST_TEST_MESSAGE("Property 92.20: Add server maintains safety during transition");
    cluster c;
    outcome before;
    c.submit("before", before);
    BOOST_REQUIRE(c.pump([&] { return before.done(); }));

    outcome added;
    c.hold_in_joint_phase(added);
    outcome during;
    c.submit("during", during);
    c.release_joint_phase();
    BOOST_REQUIRE(c.pump([&] { return added.done() && during.done(); }, 10000ms));
    outcome after;
    c.submit("after", after);
    BOOST_REQUIRE(c.pump([&] { return after.done(); }));
    BOOST_CHECK(before.resolved);
    BOOST_CHECK(during.resolved);
    BOOST_CHECK(added.resolved);
    BOOST_CHECK(after.resolved);

    const auto commit = c.leader().debug_state().commit_index;
    BOOST_REQUIRE(c.pump([&] {
        for (std::uint64_t id : {2u, 3u, 4u}) {
            if (c.node(id).debug_state().last_applied < commit) {
                return false;
            }
        }
        return true;
    }));
    // Quiesce before reading the logs.
    std::this_thread::sleep_for(100ms);

    const auto leader_log = c.leader().debug_state().log;
    for (std::uint64_t id : {1u, 2u, 3u, 4u}) {
        BOOST_CHECK_EQUAL(c.node(id).is_leader(), id == 1);
        const auto log = c.node(id).debug_state().log;
        BOOST_REQUIRE_GE(log.size(), commit);
        for (std::size_t i = 0; i < commit; ++i) {
            BOOST_CHECK_EQUAL(log[i].index(), leader_log[i].index());
            BOOST_CHECK_EQUAL(log[i].term(), leader_log[i].term());
            BOOST_CHECK(log[i].type() == leader_log[i].type());
            BOOST_CHECK(log[i].command() == leader_log[i].command());
        }
        const auto v = c.node(id).current_membership();
        BOOST_CHECK(!v.joint);
        BOOST_CHECK((v.voters == std::vector<std::uint64_t>{1, 2, 3, 4}));
    }
}

BOOST_AUTO_TEST_SUITE_END()
