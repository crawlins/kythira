// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * Joint-consensus safety on a real multi-node cluster.
 *
 * - A joint configuration entry commits only with a majority of BOTH the old
 *   and the new voter sets, driven through the leader's real
 *   advance_commit_index(): one case lacks only the new majority, the other
 *   only the old one.
 * - A follower that took an uncommitted configuration entry reverts to the
 *   previous configuration when a new leader truncates that entry.
 * - add_server never produces two leaders, and a leader crash in the middle of
 *   a change never produces two leaders in one term.
 *
 * Specification: .kiro/specs/membership-change/
 * Requirements: 2.1, 2.2, 6.2, 7.1
 * Tasks: 6, 7, 12, 13, 15
 */

#define BOOST_TEST_MODULE membership_change_joint_safety_test
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

auto followers_of(const raft_multi_node_fixture& f, node_id leader) -> id_list {
    id_list out;
    for (auto id : f.get_node_ids()) {
        if (id != leader) {
            out.push_back(id);
        }
    }
    return out;
}

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

/// Holds for `window` that no node has committed `index`.
auto stays_uncommitted(raft_multi_node_fixture& f, std::uint64_t index,
                       std::chrono::milliseconds window) -> bool {
    auto end = std::chrono::steady_clock::now() + window;
    while (std::chrono::steady_clock::now() < end) {
        for (auto id : f.running_node_ids()) {
            if (f.commit_index_of(id) >= index) {
                return false;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(joint_quorum)

// C_old = {L, R, O}, C_new = {L, O}. With O down, L and R are a majority of
// C_old but L alone is not a majority of C_new, so the joint entry must not
// commit. An implementation that counted only C_old would commit it.
BOOST_AUTO_TEST_CASE(joint_entry_waits_for_the_new_majority,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_running_cluster(3);
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());
    BOOST_REQUIRE(f->wait_for_convergence(scaled_deadline(3000), 1));
    auto followers = followers_of(*f, *leader);
    auto removed = followers[0];
    auto other = followers[1];

    f->stop_node(other);
    auto change = f->node(*leader).remove_server(removed);
    auto joint_index = f->node(*leader).last_log_index();
    BOOST_REQUIRE(f->node(*leader).current_membership().joint);

    // R holds the entry, so only the C_new majority is missing.
    BOOST_REQUIRE(wait_until([&] { return f->node(removed).last_log_index() >= joint_index; },
                             scaled_deadline(3000)));
    BOOST_CHECK_MESSAGE(stays_uncommitted(*f, joint_index, 4 * f->config().election_timeout_max),
                        "joint entry committed without a C_new majority:" << f->cluster_summary());

    f->start_node(other);
    auto r = await_result(std::move(change), scaled_deadline(8000));
    BOOST_CHECK_MESSAGE(r.completed && r.ok, "remove_server: " << r.error);
    auto final_leader = f->wait_for_leader(scaled_deadline(5000), {*leader, other});
    BOOST_REQUIRE(final_leader.has_value());
    BOOST_CHECK_GE(f->commit_index_of(*final_leader), joint_index);
    BOOST_CHECK(wait_for_membership(*f, {*leader, other}, {*leader, other}));
    check_safety(*f);
}

// C_old = {L, X, Y, R}, C_new = {L, X, Y}. With Y and R down, L and X are a
// majority of C_new (2 of 3) but not of C_old (2 of 4), so the joint entry must
// not commit. An implementation that counted only C_new would commit it.
BOOST_AUTO_TEST_CASE(joint_entry_waits_for_the_old_majority,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_running_cluster(4);
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());
    BOOST_REQUIRE(f->wait_for_convergence(scaled_deadline(3000), 1));
    auto followers = followers_of(*f, *leader);
    auto x = followers[0];
    auto y = followers[1];
    auto removed = followers[2];

    f->stop_node(y);
    f->stop_node(removed);
    auto change = f->node(*leader).remove_server(removed);
    auto joint_index = f->node(*leader).last_log_index();
    BOOST_REQUIRE(f->node(*leader).current_membership().joint);

    BOOST_REQUIRE(wait_until([&] { return f->node(x).last_log_index() >= joint_index; },
                             scaled_deadline(3000)));
    BOOST_CHECK_MESSAGE(stays_uncommitted(*f, joint_index, 4 * f->config().election_timeout_max),
                        "joint entry committed without a C_old majority:" << f->cluster_summary());

    f->start_node(y);
    auto r = await_result(std::move(change), scaled_deadline(8000));
    BOOST_CHECK_MESSAGE(r.completed && r.ok, "remove_server: " << r.error);
    id_list expected{*leader, x, y};
    auto final_leader = f->wait_for_leader(scaled_deadline(5000), expected);
    BOOST_REQUIRE(final_leader.has_value());
    BOOST_CHECK_GE(f->commit_index_of(*final_leader), joint_index);
    BOOST_CHECK(wait_for_membership(*f, expected, expected));
    check_safety(*f);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(configuration_truncation)

// Task 12 / Requirement 6.2: a configuration entry takes effect when it is
// appended, so a follower that appended one which never committed must go
// back to the previous configuration when a new leader overwrites it.
BOOST_AUTO_TEST_CASE(truncated_configuration_entry_is_reverted,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_running_cluster(5);
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());
    BOOST_REQUIRE(f->wait_for_convergence(scaled_deadline(3000), 1));
    auto followers = followers_of(*f, *leader);
    auto partner = followers[0];
    id_list minority{*leader, partner};
    id_list majority{followers[1], followers[2], followers[3]};
    const id_list everyone{1, 2, 3, 4, 5};

    f->partition({minority, majority});
    auto change = f->node(*leader).remove_server(followers[3]);
    auto joint_index = f->node(*leader).last_log_index();

    // The partner takes the joint entry and switches to it, uncommitted.
    BOOST_REQUIRE(wait_until([&] { return f->node(partner).current_membership().joint; },
                             scaled_deadline(3000)));
    BOOST_CHECK_LT(f->commit_index_of(partner), joint_index);

    // The majority elects a leader that never saw the entry, and writes over
    // its index.
    auto new_leader = f->wait_for_leader(scaled_deadline(5000), majority);
    BOOST_REQUIRE(new_leader.has_value());
    auto new_term = f->term_of(*new_leader);
    BOOST_REQUIRE(f->submit_to(*new_leader, raft_multi_node_fixture::put_command("k", "v"), 3s).ok);
    BOOST_REQUIRE_GE(f->commit_index_of(*new_leader), joint_index);

    f->heal();
    BOOST_CHECK(wait_for_membership(*f, minority, everyone));
    for (auto id : minority) {
        auto at = f->node(id).log_entries_between(joint_index, joint_index);
        BOOST_REQUIRE_EQUAL(at.size(), 1u);
        BOOST_CHECK_GE(at.front().term(), new_term);
        BOOST_CHECK(at.front().type() != kythira::entry_type::configuration);
    }
    BOOST_CHECK(!await_result(std::move(change), 1s).ok);
    BOOST_CHECK(f->wait_for_convergence(scaled_deadline(5000)));
    check_safety(*f);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(leadership_during_change)

// Requirement 7.1 / Task 13: on a healthy network, adding a server never puts
// two nodes in the leader role at once, in any term.
BOOST_AUTO_TEST_CASE(add_server_never_has_two_leaders,
                     *boost::unit_test::timeout(scaled_timeout(90))) {
    for (int iteration = 0; iteration < 3; ++iteration) {
        BOOST_TEST_CONTEXT("iteration " << iteration) {
            auto f = make_running_cluster(3);
            auto leader = f->wait_for_leader(scaled_deadline(5000));
            BOOST_REQUIRE(leader.has_value());
            f->add_node(4, {1, 2, 3}, {4});
            f->start_node(4);

            auto r = await_result(f->node(*leader).add_server(4), scaled_deadline(8000));
            BOOST_REQUIRE_MESSAGE(r.completed && r.ok, "add_server: " << r.error);
            BOOST_CHECK(wait_for_membership(*f, {1, 2, 3, 4}, {1, 2, 3, 4}));
            BOOST_CHECK(f->submit(raft_multi_node_fixture::put_command("k", "v"), 3s).ok);

            BOOST_CHECK_EQUAL(f->max_simultaneous_leaders(), 1u);
            auto leaders = f->observed_leaders();
            BOOST_CHECK_EQUAL(leaders.size(), 1u);
            check_safety(*f);
        }
    }
}

// Task 15: the leader crashes right after starting a change. Whatever the
// survivors elect, no term has two leaders, and every voter ends on one
// settled configuration.
BOOST_AUTO_TEST_CASE(leader_crash_mid_change_keeps_one_leader_per_term,
                     *boost::unit_test::timeout(scaled_timeout(90))) {
    for (int iteration = 0; iteration < 3; ++iteration) {
        BOOST_TEST_CONTEXT("iteration " << iteration) {
            auto f = make_running_cluster(5);
            auto leader = f->wait_for_leader(scaled_deadline(5000));
            BOOST_REQUIRE(leader.has_value());
            auto removed = followers_of(*f, *leader).back();

            auto change = f->node(*leader).remove_server(removed);
            // Let the entry reach some followers on later iterations.
            std::this_thread::sleep_for(std::chrono::milliseconds{iteration * 15});
            f->stop_node(*leader);

            auto survivors = followers_of(*f, *leader);
            auto new_leader = f->wait_for_leader(scaled_deadline(5000), survivors);
            BOOST_REQUIRE(new_leader.has_value());
            BOOST_REQUIRE(
                f->submit(raft_multi_node_fixture::put_command("k", "v"), scaled_deadline(5000))
                    .ok);

            f->start_node(*leader);
            BOOST_CHECK(wait_until(
                [&] {
                    auto reference = f->node(*new_leader).current_membership();
                    if (reference.joint) {
                        return false;
                    }
                    // A server the change removed stops hearing from the
                    // leader, so only the remaining voters are compared.
                    for (auto id : reference.voters) {
                        auto m = f->node(id).current_membership();
                        if (m.joint || sorted(m.voters) != sorted(reference.voters)) {
                            return false;
                        }
                    }
                    return true;
                },
                scaled_deadline(5000)));
            (void)await_result(std::move(change), 100ms);
            check_safety(*f);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
