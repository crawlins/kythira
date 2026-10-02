// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * Network partition scenarios on a real multi-node cluster.
 *
 * - leader isolation: the majority elects a new leader, the old leader cannot
 *   commit, and its uncommitted entry is discarded when the partition heals;
 * - follower isolation: the cluster keeps committing under the same leader,
 *   the follower does not disrupt it on return (PreVote), and catches up;
 * - split brain: a minority never elects a leader or commits, and an even
 *   split commits on neither side.
 *
 * Every case also checks Election Safety (at most one leader per term) and
 * that committed prefixes match across all nodes.
 *
 * Requirements: raft-consensus 2.1, 2.2, 2.3, 2.4, 5.1, 7.1, 7.2, 7.3
 * Tasks: raft-consensus 710, 711, 712, 713
 */

#define BOOST_TEST_MODULE raft_multi_node_partition_test
#include <boost/test/unit_test.hpp>

#include "raft_multi_node_test_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

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

auto make_running_cluster(std::size_t nodes) -> std::unique_ptr<raft_multi_node_fixture> {
    cluster_config cfg;
    cfg.node_count = nodes;
    auto f = std::make_unique<raft_multi_node_fixture>(cfg);
    f->initialize_cluster();
    f->start_all_nodes();
    return f;
}

auto others(const raft_multi_node_fixture& f, node_id excluded) -> std::vector<node_id> {
    std::vector<node_id> out;
    for (auto id : f.get_node_ids()) {
        if (id != excluded) {
            out.push_back(id);
        }
    }
    return out;
}

auto has_command(const std::vector<raft_multi_node_fixture::log_entry_type>& log,
                 const std::vector<std::byte>& cmd) -> bool {
    return std::any_of(log.begin(), log.end(), [&](const auto& e) { return e.command() == cmd; });
}

auto check_safety(const raft_multi_node_fixture& f) -> void {
    for (const auto& v : f.election_safety_violations()) {
        BOOST_ERROR("two leaders in one " << v);
    }
    auto mismatch = f.committed_logs_mismatch();
    BOOST_CHECK_MESSAGE(!mismatch.has_value(), mismatch.value_or(""));
}

}  // namespace

BOOST_AUTO_TEST_SUITE(partition_scenarios)

// Task 711.
BOOST_AUTO_TEST_CASE(isolated_leader_is_replaced_and_its_entry_discarded,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_running_cluster(5);
    auto old_leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(old_leader.has_value());
    BOOST_REQUIRE(f->submit(raft_multi_node_fixture::put_command("k", "before"), 3s).ok);
    auto old_term = f->term_of(*old_leader);
    auto majority = others(*f, *old_leader);

    f->isolate(*old_leader);

    // A command sent to the stranded leader is appended but can never commit.
    auto stale = raft_multi_node_fixture::put_command("k", "stale");
    auto stale_result = f->submit_to(*old_leader, stale, 600ms);
    BOOST_CHECK(!stale_result.ok);

    auto new_leader = f->wait_for_leader(scaled_deadline(5000), majority);
    BOOST_REQUIRE(new_leader.has_value());
    BOOST_CHECK_GT(f->term_of(*new_leader), old_term);
    auto fresh = raft_multi_node_fixture::put_command("k", "fresh");
    auto r = f->submit_to(*new_leader, fresh, 3s);
    BOOST_REQUIRE_MESSAGE(r.ok, r.error);

    f->heal();
    BOOST_CHECK(
        wait_until([&] { return !f->node(*old_leader).is_leader(); }, scaled_deadline(3000)));
    BOOST_CHECK(f->wait_for_convergence(scaled_deadline(5000), f->commit_index_of(*new_leader)));

    for (auto id : f->get_node_ids()) {
        auto log = f->committed_entries(id);
        BOOST_CHECK_MESSAGE(!has_command(log, stale),
                            "node " << id << " committed the stale entry");
        BOOST_CHECK_MESSAGE(has_command(log, fresh), "node " << id << " lacks the new entry");
    }
    auto read = f->submit(raft_multi_node_fixture::get_command("k"), 3s);
    BOOST_REQUIRE(read.ok);
    BOOST_CHECK_EQUAL(
        std::string(reinterpret_cast<const char*>(read.value.data()), read.value.size()), "fresh");
    check_safety(*f);
}

// Task 712.
BOOST_AUTO_TEST_CASE(isolated_follower_does_not_disrupt_and_catches_up,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_running_cluster(5);
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());
    BOOST_REQUIRE(f->wait_for_convergence(scaled_deadline(3000), 1));
    auto term = f->term_of(*leader);
    auto follower = others(*f, *leader).front();

    f->isolate(follower);
    for (int i = 0; i < 10; ++i) {
        auto r =
            f->submit_to(*leader, raft_multi_node_fixture::put_command(std::to_string(i), "v"), 3s);
        BOOST_REQUIRE_MESSAGE(r.ok, r.error);
    }
    // Long enough for the follower's election timer to fire several times.
    std::this_thread::sleep_for(3 * f->config().election_timeout_max);
    BOOST_CHECK(f->node(*leader).is_leader());
    BOOST_CHECK_EQUAL(f->term_of(*leader), term);
    BOOST_CHECK(!f->node(follower).is_leader());

    f->heal();
    BOOST_CHECK(f->wait_for_convergence(scaled_deadline(5000), f->commit_index_of(*leader)));

    // PreVote: the returning follower failed every pre-vote while cut off, so
    // it never raised its term and the leader is undisturbed.
    BOOST_CHECK(f->node(*leader).is_leader());
    BOOST_CHECK_EQUAL(f->term_of(*leader), term);
    BOOST_CHECK_EQUAL(f->term_of(follower), term);
    check_safety(*f);
}

// Task 713: 2|3 split with the leader on the minority side.
BOOST_AUTO_TEST_CASE(minority_never_elects_or_commits,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_running_cluster(5);
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());
    auto rest = others(*f, *leader);
    std::vector<node_id> minority{*leader, rest[0]};
    std::vector<node_id> majority{rest[1], rest[2], rest[3]};
    auto minority_commit = f->commit_index_of(rest[0]);
    auto old_term = f->term_of(*leader);

    f->partition({minority, majority});
    auto stale = raft_multi_node_fixture::put_command("split", "minority");
    BOOST_CHECK(!f->submit_to(*leader, stale, 800ms).ok);

    auto new_leader = f->wait_for_leader(scaled_deadline(5000), majority);
    BOOST_REQUIRE(new_leader.has_value());
    BOOST_REQUIRE(
        f->submit_to(*new_leader, raft_multi_node_fixture::put_command("split", "maj"), 3s).ok);

    // Nothing new committed on the minority side, and only the old leader ever
    // claimed leadership there, in its old term.
    BOOST_CHECK_EQUAL(f->commit_index_of(rest[0]), minority_commit);
    BOOST_CHECK(!f->node(rest[0]).is_leader());
    for (const auto& [term, leaders] : f->observed_leaders()) {
        if (leaders.contains(rest[0])) {
            BOOST_ERROR("minority follower " << rest[0] << " became leader in term " << term);
        }
        if (leaders.contains(*leader)) {
            BOOST_CHECK_EQUAL(term, old_term);
        }
    }

    f->heal();
    BOOST_CHECK(f->wait_for_convergence(scaled_deadline(5000), f->commit_index_of(*new_leader)));
    for (auto id : f->get_node_ids()) {
        BOOST_CHECK(!has_command(f->committed_entries(id), stale));
    }
    check_safety(*f);
}

// Task 713: an even 2|2 split has no majority anywhere, so nothing commits
// until it heals.
BOOST_AUTO_TEST_CASE(even_split_commits_on_neither_side,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    auto f = make_running_cluster(4);
    auto leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(leader.has_value());
    BOOST_REQUIRE(f->submit(raft_multi_node_fixture::put_command("even", "0"), 3s).ok);
    BOOST_REQUIRE(f->wait_for_convergence(scaled_deadline(3000)));
    auto rest = others(*f, *leader);
    std::vector<node_id> side_a{*leader, rest[0]};
    std::vector<node_id> side_b{rest[1], rest[2]};
    auto old_term = f->term_of(*leader);
    std::map<node_id, std::uint64_t> commit_before;
    for (auto id : f->get_node_ids()) {
        commit_before[id] = f->commit_index_of(id);
    }

    f->partition({side_a, side_b});
    BOOST_CHECK(
        !f->submit_to(*leader, raft_multi_node_fixture::put_command("even", "a"), 800ms).ok);
    std::this_thread::sleep_for(4 * f->config().election_timeout_max);

    BOOST_CHECK(!f->get_leader(side_b).has_value());
    for (auto id : f->get_node_ids()) {
        BOOST_CHECK_EQUAL(f->commit_index_of(id), commit_before[id]);
    }
    for (const auto& [term, leaders] : f->observed_leaders()) {
        BOOST_CHECK_MESSAGE(term == old_term,
                            "a leader was elected in term " << term << " during the split");
    }

    f->heal();
    auto healed_leader = f->wait_for_leader(scaled_deadline(5000));
    BOOST_REQUIRE(healed_leader.has_value());
    BOOST_REQUIRE(f->submit(raft_multi_node_fixture::put_command("even", "healed"), 3s).ok);
    BOOST_CHECK(f->wait_for_convergence(scaled_deadline(5000)));
    check_safety(*f);
}

BOOST_AUTO_TEST_SUITE_END()
