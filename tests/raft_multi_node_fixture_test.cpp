// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * Multi-node fixture and cluster initialization tests.
 *
 * Every case runs real Raft nodes through raft_multi_node_test_fixture.hpp:
 * cluster bootstrap at 3, 5 and 7 nodes, node lifecycle (crash and restart),
 * leader crash, and the fixture's network-condition and log-inspection
 * utilities.
 *
 * Requirements: raft-consensus 1.1, 1.2, 1.3, 2.1, 2.2
 * Tasks: raft-consensus 700, 701, 730
 */

#define BOOST_TEST_MODULE raft_multi_node_fixture_test
#include <boost/test/unit_test.hpp>

#include "raft_multi_node_test_fixture.hpp"

#include <chrono>
#include <string>

using kythira::test::cluster_config;
using kythira::test::raft_multi_node_fixture;
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

auto make_fixture(std::size_t nodes) -> std::unique_ptr<raft_multi_node_fixture> {
    cluster_config cfg;
    cfg.node_count = nodes;
    auto f = std::make_unique<raft_multi_node_fixture>(cfg);
    f->initialize_cluster();
    return f;
}

auto check_election_safety(const raft_multi_node_fixture& f) -> void {
    auto violations = f.election_safety_violations();
    for (const auto& v : violations) {
        BOOST_ERROR("two leaders in one " << v);
    }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(cluster_initialization)

// Task 701: nodes come up as followers in term 0, one leader is elected, and
// every node learns who it is.
BOOST_AUTO_TEST_CASE(bootstraps_and_elects_one_leader_at_3_5_7,
                     *boost::unit_test::timeout(scaled_timeout(90))) {
    for (std::size_t n : {3u, 5u, 7u}) {
        BOOST_TEST_CONTEXT("cluster of " << n) {
            auto f = make_fixture(n);
            BOOST_CHECK_EQUAL(f->get_node_count(), n);
            for (auto id : f->get_node_ids()) {
                BOOST_CHECK(f->node(id).get_state() == kythira::server_state::follower);
                BOOST_CHECK_EQUAL(f->term_of(id), 0u);
                BOOST_CHECK_EQUAL(f->node(id).get_cluster_size(), n);
            }

            f->start_all_nodes();
            auto leader = f->wait_for_leader(scaled_deadline(5000));
            BOOST_REQUIRE(leader.has_value());

            // Randomized timeouts keep split votes from repeating forever: the
            // first leader should arrive within a handful of terms.
            BOOST_CHECK_LE(f->term_of(*leader), 5u);

            BOOST_CHECK(kythira::test::wait_until(
                [&] {
                    for (auto id : f->get_node_ids()) {
                        if (id != *leader && f->node(id).known_leader() != leader) {
                            return false;
                        }
                    }
                    return true;
                },
                scaled_deadline(3000)));

            // The leader's no-op commits on every node.
            BOOST_CHECK(f->wait_for_convergence(scaled_deadline(3000), 1));
            check_election_safety(*f);
            BOOST_CHECK_EQUAL(f->max_simultaneous_leaders(), 1u);
        }
    }
}

BOOST_AUTO_TEST_CASE(rejects_invalid_cluster_size, *boost::unit_test::timeout(10)) {
    cluster_config cfg;
    cfg.node_count = 0;
    BOOST_CHECK_THROW(raft_multi_node_fixture{cfg}, std::invalid_argument);
    cfg.node_count = 10;
    BOOST_CHECK_THROW(raft_multi_node_fixture{cfg}, std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(node_lifecycle)

// A crashed follower misses entries, and catches up from its persisted state
// after restart.
BOOST_AUTO_TEST_CASE(follower_crash_and_restart_catches_up,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_fixture(3);
    f->start_all_nodes();
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());
    auto follower = *leader == 1 ? 2u : 1u;

    BOOST_REQUIRE(f->submit(raft_multi_node_fixture::put_command("a", "1"), 3s).ok);
    BOOST_REQUIRE(f->wait_for_convergence(scaled_deadline(3000)));
    auto term_before = f->term_of(follower);

    f->stop_node(follower);
    BOOST_CHECK(!f->is_node_running(follower));
    BOOST_CHECK(!f->node(follower).is_running());

    // Two of three nodes still form a majority.
    for (int i = 0; i < 5; ++i) {
        auto r = f->submit(raft_multi_node_fixture::put_command("k" + std::to_string(i), "v"), 3s);
        BOOST_REQUIRE_MESSAGE(r.ok, r.error);
    }
    auto target = f->commit_index_of(*leader);
    BOOST_CHECK_LT(f->last_applied_of(follower), target);

    f->start_node(follower);
    BOOST_CHECK(f->is_node_running(follower));
    BOOST_CHECK_GE(f->term_of(follower), term_before);
    BOOST_CHECK(f->wait_for_convergence(scaled_deadline(5000), target));
    BOOST_CHECK(!f->committed_logs_mismatch().has_value());
    check_election_safety(*f);
}

// Crashing the leader elects a new one in a later term, and the cluster keeps
// committing; the old leader rejoins as a follower.
BOOST_AUTO_TEST_CASE(leader_crash_elects_new_leader,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_fixture(5);
    f->start_all_nodes();
    auto old_leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(old_leader.has_value());
    BOOST_REQUIRE(f->submit(raft_multi_node_fixture::put_command("before", "1"), 3s).ok);
    auto old_term = f->term_of(*old_leader);

    f->stop_node(*old_leader);
    auto new_leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(new_leader.has_value());
    BOOST_CHECK_NE(*new_leader, *old_leader);
    BOOST_CHECK_GT(f->term_of(*new_leader), old_term);
    BOOST_REQUIRE(f->submit(raft_multi_node_fixture::put_command("after", "2"), 3s).ok);

    f->restart_node(*old_leader);
    BOOST_CHECK(kythira::test::wait_until([&] { return !f->node(*old_leader).is_leader(); },
                                          scaled_deadline(3000)));
    BOOST_CHECK(f->wait_for_convergence(scaled_deadline(5000), f->commit_index_of(*new_leader)));
    BOOST_CHECK(!f->committed_logs_mismatch().has_value());
    check_election_safety(*f);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(fixture_utilities)

// Task 730: latency and loss injection degrade, but do not stop, replication.
BOOST_AUTO_TEST_CASE(commits_through_latency_and_loss,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_fixture(3);
    f->start_all_nodes();
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());

    for (auto id : f->get_node_ids()) {
        if (id != *leader) {
            f->set_node_network(id, 15ms, 0.8);
        }
    }
    for (int i = 0; i < 5; ++i) {
        auto r = f->submit(raft_multi_node_fixture::put_command("slow" + std::to_string(i), "v"),
                           scaled_deadline(5000));
        BOOST_CHECK_MESSAGE(r.ok, r.error);
    }
    for (auto id : f->get_node_ids()) {
        f->clear_node_network(id);
    }
    BOOST_CHECK(f->wait_for_convergence(scaled_deadline(5000), f->commit_index_of(*leader)));
    BOOST_CHECK(!f->committed_logs_mismatch().has_value());
}

// Task 730: the log helpers see the commands the cluster committed, and the
// state machine answers reads with them.
BOOST_AUTO_TEST_CASE(log_inspection_reports_committed_commands,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_fixture(3);
    f->start_all_nodes();
    BOOST_REQUIRE(f->wait_for_leader(scaled_deadline(5000)).has_value());

    auto cmd = raft_multi_node_fixture::put_command("x", "42");
    BOOST_REQUIRE(f->submit(cmd, 3s).ok);
    BOOST_REQUIRE(f->wait_for_convergence(scaled_deadline(3000)));

    for (auto id : f->get_node_ids()) {
        auto log = f->committed_entries(id);
        auto found =
            std::any_of(log.begin(), log.end(), [&](const auto& e) { return e.command() == cmd; });
        BOOST_CHECK_MESSAGE(found, "node " << id << " is missing the committed command");
    }
    BOOST_CHECK(!f->committed_logs_mismatch().has_value());

    auto read = f->submit(raft_multi_node_fixture::get_command("x"), 3s);
    BOOST_REQUIRE(read.ok);
    std::string value(reinterpret_cast<const char*>(read.value.data()), read.value.size());
    BOOST_CHECK_EQUAL(value, "42");
}

BOOST_AUTO_TEST_SUITE_END()
