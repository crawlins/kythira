// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * Membership management on a real multi-node cluster.
 *
 * add_server, remove_server (of a follower and of the leader itself), the
 * learner catch-up path, rejection of concurrent and non-leader changes, and
 * changes that run into a crashed node, a partition, or each other. Every case
 * also checks that no term ever had two leaders.
 *
 * Requirements: raft-consensus 11.1, 11.2, 11.3, 11.4, 11.5
 * Task: raft-consensus 702
 */

#define BOOST_TEST_MODULE raft_membership_management_unit_test
#include <boost/test/unit_test.hpp>

#include "raft_multi_node_test_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using kythira::test::await_result;
using kythira::test::cluster_config;
using kythira::test::raft_multi_node_fixture;
using kythira::test::wait_until;
using kythira::testing::scaled_deadline;
using kythira::testing::scaled_timeout;
using namespace std::chrono_literals;

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
// BOOST_GLOBAL_FIXTURE pastes its argument into an identifier, so the
// fixture must be named unqualified.
using kythira::test::folly_init_fixture;
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using node_id = raft_multi_node_fixture::node_id_type;
using id_list = std::vector<node_id>;

constexpr auto change_timeout = std::chrono::milliseconds{5000};

auto make_running_cluster(std::size_t nodes) -> std::unique_ptr<raft_multi_node_fixture> {
    cluster_config cfg;
    cfg.node_count = nodes;
    auto f = std::make_unique<raft_multi_node_fixture>(cfg);
    f->initialize_cluster();
    f->start_all_nodes();
    return f;
}

auto sorted(id_list ids) -> id_list {
    std::sort(ids.begin(), ids.end());
    return ids;
}

auto voters_of(raft_multi_node_fixture& f, node_id id) -> id_list {
    return sorted(f.node(id).current_membership().voters);
}

/// Waits until every node in `ids` runs a settled (non-joint) configuration
/// whose voters are exactly `expected`.
auto wait_for_membership(raft_multi_node_fixture& f, const id_list& ids, const id_list& expected)
    -> bool {
    return wait_until(
        [&] {
            for (auto id : ids) {
                auto m = f.node(id).current_membership();
                if (m.joint || sorted(m.voters) != sorted(expected)) {
                    return false;
                }
            }
            return true;
        },
        scaled_deadline(5000));
}

auto check_safety(const raft_multi_node_fixture& f) -> void {
    for (const auto& v : f.election_safety_violations()) {
        BOOST_ERROR("two leaders in one " << v);
    }
    auto mismatch = f.committed_logs_mismatch();
    BOOST_CHECK_MESSAGE(!mismatch.has_value(), mismatch.value_or(""));
}

auto first_follower(const raft_multi_node_fixture& f, node_id leader) -> node_id {
    for (auto id : f.get_node_ids()) {
        if (id != leader) {
            return id;
        }
    }
    throw std::logic_error("no follower");
}

/// Adds a stopped node that knows the voters but lists itself as a learner,
/// so it cannot campaign before the leader's configuration reaches it.
auto add_joining_node(raft_multi_node_fixture& f, node_id id, const id_list& voters) -> void {
    f.add_node(id, voters, {id});
    f.start_node(id);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(membership_management)

// Requirement 11.1: add_server goes through joint consensus and the new node
// ends up a voter holding the whole log.
BOOST_AUTO_TEST_CASE(add_server_admits_a_voter_with_the_full_log,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_running_cluster(3);
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());
    for (int i = 0; i < 5; ++i) {
        BOOST_REQUIRE(
            f->submit(raft_multi_node_fixture::put_command("pre" + std::to_string(i), "v"), 3s).ok);
    }
    auto before = f->commit_index_of(*leader);

    add_joining_node(*f, 4, {1, 2, 3});
    auto r = await_result(f->node(*leader).add_server(4), scaled_deadline(change_timeout.count()));
    BOOST_REQUIRE_MESSAGE(r.completed && r.ok, "add_server: " << r.error);

    BOOST_CHECK(wait_for_membership(*f, {1, 2, 3, 4}, {1, 2, 3, 4}));
    BOOST_CHECK(f->wait_for_convergence(scaled_deadline(5000), before));
    BOOST_CHECK(f->submit(raft_multi_node_fixture::put_command("post", "v"), 3s).ok);
    BOOST_CHECK(f->wait_for_convergence(scaled_deadline(5000), f->commit_index_of(*leader)));
    check_safety(*f);
}

// Requirement 11.2: removing a follower drops it from every configuration and
// from the leader's replication state, and the rest keep committing.
BOOST_AUTO_TEST_CASE(remove_server_cleans_up_the_follower,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_running_cluster(5);
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());
    auto removed = first_follower(*f, *leader);
    BOOST_REQUIRE(f->wait_for_convergence(scaled_deadline(3000), 1));
    BOOST_REQUIRE(f->node(*leader).match_index_of(removed).has_value());

    auto r = await_result(f->node(*leader).remove_server(removed),
                          scaled_deadline(change_timeout.count()));
    BOOST_REQUIRE_MESSAGE(r.completed && r.ok, "remove_server: " << r.error);

    id_list rest;
    for (auto id : f->get_node_ids()) {
        if (id != removed) {
            rest.push_back(id);
        }
    }
    BOOST_CHECK(wait_for_membership(*f, rest, rest));
    BOOST_CHECK(wait_until([&] { return !f->node(*leader).match_index_of(removed).has_value(); },
                           scaled_deadline(3000)));

    f->stop_node(removed);
    BOOST_CHECK(f->submit(raft_multi_node_fixture::put_command("after", "v"), 3s).ok);
    BOOST_CHECK(f->wait_for_convergence(scaled_deadline(5000), f->commit_index_of(*leader), rest));
    check_safety(*f);
}

// Requirement 11.5: a leader that removes itself steps down once the change
// commits, and the remaining nodes elect a leader among themselves.
BOOST_AUTO_TEST_CASE(leader_steps_down_after_removing_itself,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_running_cluster(3);
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());

    auto r = await_result(f->node(*leader).remove_server(*leader),
                          scaled_deadline(change_timeout.count()));
    BOOST_REQUIRE_MESSAGE(r.completed && r.ok, "remove_server(self): " << r.error);

    id_list rest;
    for (auto id : f->get_node_ids()) {
        if (id != *leader) {
            rest.push_back(id);
        }
    }
    BOOST_CHECK(wait_until([&] { return !f->node(*leader).is_leader(); }, scaled_deadline(3000)));
    BOOST_CHECK(wait_for_membership(*f, rest, rest));

    auto new_leader = f->wait_for_leader(scaled_deadline(5000), rest);
    BOOST_REQUIRE_MESSAGE(new_leader.has_value(),
                          "no leader among the rest:" << f->cluster_summary());
    BOOST_CHECK_NE(*new_leader, *leader);
    f->stop_node(*leader);
    BOOST_CHECK(f->submit_to(*new_leader, raft_multi_node_fixture::put_command("k", "v"), 3s).ok);
    check_safety(*f);
}

// Requirement 11.4: a new node catches up as a learner, never votes or
// campaigns while it does, and is then promoted to voter.
BOOST_AUTO_TEST_CASE(learner_catches_up_before_promotion,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_running_cluster(3);
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());
    for (int i = 0; i < 10; ++i) {
        BOOST_REQUIRE(
            f->submit(raft_multi_node_fixture::put_command("log" + std::to_string(i), "v"), 3s).ok);
    }

    add_joining_node(*f, 4, {1, 2, 3});
    auto added =
        await_result(f->node(*leader).add_learner(4), scaled_deadline(change_timeout.count()));
    BOOST_REQUIRE_MESSAGE(added.completed && added.ok, "add_learner: " << added.error);
    auto m = f->node(*leader).current_membership();
    BOOST_CHECK(std::find(m.learners.begin(), m.learners.end(), 4u) != m.learners.end());
    BOOST_CHECK(std::find(m.voters.begin(), m.voters.end(), 4u) == m.voters.end());

    BOOST_CHECK(f->wait_for_convergence(scaled_deadline(5000), f->commit_index_of(*leader)));
    BOOST_CHECK_EQUAL(f->term_of(4), f->term_of(*leader));

    auto promoted =
        await_result(f->node(*leader).promote_to_voter(4), scaled_deadline(change_timeout.count()));
    BOOST_REQUIRE_MESSAGE(promoted.completed && promoted.ok,
                          "promote_to_voter: " << promoted.error);
    BOOST_CHECK(wait_for_membership(*f, {1, 2, 3, 4}, {1, 2, 3, 4}));

    for (const auto& [term, leaders] : f->observed_leaders()) {
        BOOST_CHECK_MESSAGE(!leaders.contains(4), "the learner led term " << term);
    }
    check_safety(*f);
}

// Requirements 11.1, 11.2: one change at a time, and only on the leader.
BOOST_AUTO_TEST_CASE(rejects_concurrent_and_non_leader_changes,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_running_cluster(5);
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());
    auto follower = first_follower(*f, *leader);

    auto on_follower = await_result(f->node(follower).remove_server(*leader), 1s);
    BOOST_CHECK(on_follower.completed && !on_follower.ok);
    BOOST_CHECK_NE(on_follower.error.find("leader"), std::string::npos);

    auto first_fut = f->node(*leader).remove_server(follower);
    auto second = await_result(f->node(*leader).add_server(9), 1s);
    BOOST_CHECK(second.completed && !second.ok);
    BOOST_CHECK_NE(second.error.find("in progress"), std::string::npos);

    auto first = await_result(std::move(first_fut), scaled_deadline(change_timeout.count()));
    BOOST_CHECK_MESSAGE(first.completed && first.ok, "first change: " << first.error);
    check_safety(*f);
}

// Requirement 11.3: a crashed node outside the change does not block it, as
// long as both configurations keep a majority.
BOOST_AUTO_TEST_CASE(change_completes_with_a_node_down,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_running_cluster(5);
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());
    id_list followers;
    for (auto id : f->get_node_ids()) {
        if (id != *leader) {
            followers.push_back(id);
        }
    }
    auto crashed = followers[0];
    auto removed = followers[1];
    f->stop_node(crashed);

    auto r = await_result(f->node(*leader).remove_server(removed),
                          scaled_deadline(change_timeout.count()));
    BOOST_REQUIRE_MESSAGE(r.completed && r.ok, "remove_server: " << r.error);

    id_list expected;
    for (auto id : f->get_node_ids()) {
        if (id != removed) {
            expected.push_back(id);
        }
    }
    id_list live{*leader, followers[2], followers[3]};
    BOOST_CHECK(wait_for_membership(*f, live, expected));

    // The crashed node learns the new configuration when it comes back.
    f->start_node(crashed);
    BOOST_CHECK(wait_for_membership(*f, {crashed}, expected));
    check_safety(*f);
}

// Requirement 11.3: a change started by a leader that is then cut off cannot
// split the cluster: after healing, every node runs one configuration.
BOOST_AUTO_TEST_CASE(partition_during_change_leaves_one_configuration,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_running_cluster(5);
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());
    BOOST_REQUIRE(f->wait_for_convergence(scaled_deadline(3000), 1));
    id_list followers;
    for (auto id : f->get_node_ids()) {
        if (id != *leader) {
            followers.push_back(id);
        }
    }
    id_list minority{*leader, followers[0]};
    id_list majority{followers[1], followers[2], followers[3]};

    f->partition({minority, majority});
    auto pending = f->node(*leader).remove_server(followers[3]);

    auto new_leader = f->wait_for_leader(scaled_deadline(5000), majority);
    BOOST_REQUIRE(new_leader.has_value());
    BOOST_REQUIRE(f->submit_to(*new_leader, raft_multi_node_fixture::put_command("k", "v"), 3s).ok);

    f->heal();
    // The majority never saw the change, so it is discarded everywhere.
    BOOST_CHECK(wait_for_membership(*f, f->get_node_ids(), {1, 2, 3, 4, 5}));
    auto r = await_result(std::move(pending), scaled_deadline(change_timeout.count()));
    BOOST_CHECK(!r.ok);
    BOOST_CHECK(f->wait_for_convergence(scaled_deadline(5000)));
    check_safety(*f);
}

// Requirements 11.1, 11.2: changes run back to back, each after the last
// commits.
BOOST_AUTO_TEST_CASE(sequential_changes, *boost::unit_test::timeout(scaled_timeout(90))) {
    auto f = make_running_cluster(5);
    BOOST_REQUIRE(f->wait_for_leader(scaled_deadline(5000)).has_value());

    auto change = [&](auto&& op, const char* what) {
        auto leader = f->wait_for_leader(scaled_deadline(5000));
        BOOST_REQUIRE(leader.has_value());
        auto r = await_result(op(f->node(*leader)), scaled_deadline(change_timeout.count()));
        BOOST_REQUIRE_MESSAGE(r.completed && r.ok, what << ": " << r.error);
    };

    auto leader = *f->wait_for_leader(scaled_deadline(5000));
    id_list victims;
    for (auto id : f->get_node_ids()) {
        if (id != leader && victims.size() < 2) {
            victims.push_back(id);
        }
    }

    change([&](auto& n) { return n.remove_server(victims[0]); }, "remove first");
    f->stop_node(victims[0]);
    change([&](auto& n) { return n.remove_server(victims[1]); }, "remove second");
    f->stop_node(victims[1]);

    id_list remaining;
    for (auto id : f->get_node_ids()) {
        if (id != victims[0] && id != victims[1]) {
            remaining.push_back(id);
        }
    }
    add_joining_node(*f, 6, remaining);
    change([&](auto& n) { return n.add_server(6); }, "add");

    auto expected = remaining;
    expected.push_back(6);
    BOOST_CHECK(wait_for_membership(*f, expected, expected));
    BOOST_CHECK(f->submit(raft_multi_node_fixture::put_command("k", "v"), 3s).ok);
    BOOST_CHECK(f->wait_for_convergence(scaled_deadline(5000), 0, expected));
    check_safety(*f);
}

BOOST_AUTO_TEST_SUITE_END()
