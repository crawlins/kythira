// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE raft_replication_acknowledgment_collection_property_test
#include "raft_membership_test_cluster.hpp"

#include <boost/test/unit_test.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace kythira::test::membership;

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {
            const_cast<char*>("raft_replication_acknowledgment_collection_property_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

/// The bytes a command's future resolved with.
struct result {
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::mutex mutex;
    std::vector<std::byte> value;

    [[nodiscard]] auto text() -> std::string {
        const std::lock_guard<std::mutex> lock(mutex);
        std::string out;
        for (auto b : value) {
            out.push_back(static_cast<char>(b));
        }
        return out;
    }
};

auto watch_value(test_node::future_type future, result& out) -> void {
    std::move(future)
        .thenValue([&out](std::vector<std::byte> v) {
            {
                const std::lock_guard<std::mutex> lock(out.mutex);
                out.value = std::move(v);
            }
            out.done = true;
        })
        .thenError([&out](const std::exception_ptr&) {
            out.failed = true;
            out.done = true;
        })
        .detach();
}

/// Submits a put to the leader and returns the index of its entry.
auto put(cluster& c, const std::string& key, outcome& out) -> std::uint64_t {
    c.submit(key, out);
    return c.leader().last_log_index();
}

auto commit_index(test_node& n) -> std::uint64_t {
    return n.debug_state().commit_index;
}

auto match_of(test_node& leader, std::uint64_t peer) -> std::uint64_t {
    return leader.match_index_of(peer).value_or(0);
}

/// Ticks the leader until both followers hold its whole log, the no-op it
/// appended on election included. A case that cuts a follower off before
/// then leaves it rejecting every AppendEntries as "log too short", waiting
/// on replies the leader may never get.
auto settle(cluster& c) -> void {
    const auto last = c.leader().last_log_index();
    BOOST_REQUIRE(
        c.pump([&] { return match_of(c.leader(), 2) >= last && match_of(c.leader(), 3) >= last; }));
}

/// Ticks `n`'s heartbeat until `pred` holds or `deadline` elapses, for a
/// leader other than node 1, which `cluster::pump()` cannot drive.
template<typename Pred>
auto pump_node(test_node& n, Pred pred, std::chrono::milliseconds deadline = 5000ms) -> bool {
    const auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        n.check_heartbeat_timeout();
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

}  // namespace

/**
 * Property 82: Replication Acknowledgment Collection Implementation
 *
 * The leader records each follower's acknowledgements as its match index,
 * advances the commit index once a majority (itself included) holds an entry
 * of its own term, applies what it commits, and settles the waiting client
 * futures. The cases drive a three-node cluster led by node 1 over the
 * network simulator.
 *
 * Validates: Requirements 16.3, 20.1, 20.2, 20.3, 20.4, 20.5, 27.3
 */

BOOST_AUTO_TEST_CASE(property_replication_tracks_follower_acknowledgments,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 82.1: Replication tracks follower acknowledgments");

    std::array<outcome, 2> out;
    cluster c;
    settle(c);
    const auto first = put(c, "a", out[0]);
    BOOST_REQUIRE(c.pump(
        [&] { return match_of(c.leader(), 2) >= first && match_of(c.leader(), 3) >= first; }));
    BOOST_TEST(c.node(2).last_log_index() >= first);
    BOOST_TEST(c.node(3).last_log_index() >= first);

    // With node 3 cut off, only node 2's acknowledgement moves forward.
    c.isolate(3);
    const auto second = put(c, "b", out[1]);
    BOOST_REQUIRE(c.pump([&] { return match_of(c.leader(), 2) >= second; }));
    c.pump_for(200ms);
    BOOST_TEST(match_of(c.leader(), 3) < second);
    BOOST_TEST(c.node(3).last_log_index() < second);
}

BOOST_AUTO_TEST_CASE(property_replication_advances_commit_on_majority,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 82.2: Replication advances commit index on majority");

    // With both followers cut off the leader alone holds the entry: one of
    // three, so the commit index stays put however long it waits.
    outcome out;
    cluster c;
    settle(c);
    c.isolate(2);
    c.isolate(3);
    const auto before = commit_index(c.leader());
    const auto index = put(c, "k", out);
    c.pump_for(500ms);
    BOOST_TEST(commit_index(c.leader()) == before);
    BOOST_TEST(!out.done());

    // Once a follower holds it too, a majority does, and it commits.
    c.reconnect(2);
    BOOST_REQUIRE(c.pump([&] { return commit_index(c.leader()) >= index; }));
    BOOST_REQUIRE(wait_until([&] { return out.done(); }));
    BOOST_TEST(out.resolved.load());
}

BOOST_AUTO_TEST_CASE(property_replication_includes_leader_self_acknowledgment,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 82.3: Replication includes leader self-acknowledgment");

    // Node 3 never sees the entry, so node 2 is the only follower holding it:
    // one acknowledgement of three, a majority only with the leader's own.
    outcome out;
    cluster c;
    settle(c);
    c.isolate(3);
    const auto index = put(c, "k", out);
    BOOST_REQUIRE(c.pump([&] { return commit_index(c.leader()) >= index; }));

    BOOST_TEST(match_of(c.leader(), 2) >= index);
    BOOST_TEST(match_of(c.leader(), 3) < index);
    BOOST_TEST(c.node(3).last_log_index() < index);
    BOOST_REQUIRE(wait_until([&] { return out.done(); }));
    BOOST_TEST(out.resolved.load());
}

BOOST_AUTO_TEST_CASE(property_replication_continues_with_slow_followers,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 82.4: Replication continues with slow followers");

    // Node 3's replies take 1.5 s to reach the leader, inside the RPC
    // deadline, so node 3 is slow rather than gone. The command commits on
    // node 2's acknowledgement long before node 3's arrives.
    auto cfg = make_fast_config();
    cfg._append_entries_timeout = 4000ms;
    outcome out;
    cluster c{cfg};
    settle(c);
    c.slow(3, 1, 1500ms);

    const auto start = std::chrono::steady_clock::now();
    const auto index = put(c, "k", out);
    BOOST_REQUIRE(c.pump([&] { return out.done(); }));
    const auto took = std::chrono::steady_clock::now() - start;
    BOOST_TEST(out.resolved.load());
    BOOST_TEST(took < 1500ms, "the commit waited for the slow follower");
    BOOST_TEST(match_of(c.leader(), 3) < index);

    // Node 3's acknowledgement still arrives and is recorded.
    BOOST_REQUIRE(c.pump([&] { return match_of(c.leader(), 3) >= index; }, 10000ms));
}

BOOST_AUTO_TEST_CASE(property_replication_marks_unresponsive_followers,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 82.5: Replication marks unresponsive followers");

    // Node 3 answers nothing. The leader records each failed RPC to it, by
    // log and metric, while the cluster keeps committing with node 2.
    std::array<outcome, 3> out;
    cluster c;
    settle(c);
    c.isolate(3);
    std::uint64_t last = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        last = put(c, "k" + std::to_string(i), out[i]);
    }
    BOOST_REQUIRE(c.pump([&] {
        return c.metrics(1).count("raft_heartbeat_failed") +
                   c.metrics(1).count("raft_append_entries_failed") >
               0;
    }));
    BOOST_REQUIRE(c.pump([&] { return commit_index(c.leader()) >= last; }));

    auto targets = c.metrics(1).dimension_values("raft_heartbeat_failed", "target");
    const auto appends = c.metrics(1).dimension_values("raft_append_entries_failed", "target");
    targets.insert(targets.end(), appends.begin(), appends.end());
    BOOST_TEST(!targets.empty());
    BOOST_TEST(std::all_of(targets.begin(), targets.end(), [](const auto& t) { return t == "3"; }),
               "a reachable follower was reported as failing");
    BOOST_TEST(c.logs(1).field_values("Heartbeat failed after retries", "target").size() +
                   c.metrics(1).count("raft_append_entries_failed") >
               0u);
    for (auto& o : out) {
        BOOST_REQUIRE(wait_until([&] { return o.done(); }));
        BOOST_TEST(o.resolved.load());
    }
}

BOOST_AUTO_TEST_CASE(property_replication_only_commits_current_term_entries,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 82.6: Replication only commits current term entries");

    // Node 1 replicates an entry to both followers but never hears their
    // acknowledgements (nothing reaches node 1), so it stays uncommitted
    // everywhere. Node 2 then wins the next term holding that entry. A
    // majority already has it, but it is from an earlier term: node 2 must
    // not commit it on its own, only together with its own term's no-op.
    outcome out;
    cluster c;
    settle(c);
    c.isolate(1);
    const auto index = put(c, "k", out);
    BOOST_REQUIRE(c.pump([&] {
        return c.node(2).last_log_index() >= index && c.node(3).last_log_index() >= index;
    }));
    const auto old_term = c.leader().get_current_term();
    BOOST_TEST(commit_index(c.leader()) < index);
    BOOST_TEST(commit_index(c.node(2)) < index);
    BOOST_TEST(commit_index(c.node(3)) < index);

    // Node 2 campaigns once its followers' leader contact has gone stale.
    BOOST_REQUIRE(wait_until(
        [&] {
            std::this_thread::sleep_for(c.config()._election_timeout_max + 20ms);
            c.node(2).check_election_timeout();
            return wait_until([&] { return c.node(2).is_leader(); }, 1000ms);
        },
        8000ms));
    BOOST_TEST(c.node(2).get_current_term() > old_term);

    // Its no-op follows the inherited entry; committing the no-op commits both.
    const auto no_op = c.node(2).last_log_index();
    BOOST_TEST(no_op > index);
    BOOST_REQUIRE(pump_node(c.node(2), [&] { return commit_index(c.node(2)) >= no_op; }));

    const auto advanced = c.logs(2).field_values("Advanced commit index", "new_commit_index");
    BOOST_REQUIRE(!advanced.empty());
    BOOST_TEST(std::none_of(advanced.begin(), advanced.end(),
                            [&](const auto& v) { return v == std::to_string(index); }),
               "the earlier term's entry was committed by counting replicas");
    BOOST_TEST(std::stoull(advanced.front()) >= no_op);
}

BOOST_AUTO_TEST_CASE(property_replication_triggers_state_machine_application,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 82.7: Replication triggers state machine application");

    outcome put_out;
    result get_out;
    cluster c;
    settle(c);
    const auto index = put(c, "k", put_out);
    BOOST_REQUIRE(c.pump([&] { return c.leader().last_applied_index() >= index; }));
    BOOST_TEST(c.leader().last_applied_index() <= commit_index(c.leader()));

    // The put reached the state machine: a get reads it back.
    watch_value(c.leader().submit_command(sm_t::make_get_command("k"), 3000ms), get_out);
    BOOST_REQUIRE(c.pump([&] { return get_out.done.load(); }));
    BOOST_TEST(!get_out.failed.load());
    BOOST_TEST(get_out.text() == "v");

    // The followers apply it too, once a heartbeat carries the commit index.
    BOOST_REQUIRE(c.pump([&] {
        return c.node(2).last_applied_index() >= index && c.node(3).last_applied_index() >= index;
    }));
}

BOOST_AUTO_TEST_CASE(property_replication_notifies_commit_waiter, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 82.8: Replication notifies commit waiter");

    // Five commands wait while nothing can commit, and every one settles as
    // soon as a follower comes back.
    std::array<outcome, 5> out;
    cluster c;
    settle(c);
    c.isolate(2);
    c.isolate(3);
    for (std::size_t i = 0; i < out.size(); ++i) {
        put(c, "k" + std::to_string(i), out[i]);
    }
    c.pump_for(300ms);
    BOOST_TEST(std::none_of(out.begin(), out.end(), [](const auto& o) { return o.done(); }));

    c.reconnect(3);
    BOOST_REQUIRE(c.pump([&] {
        return std::all_of(out.begin(), out.end(), [](const auto& o) { return o.done(); });
    }));
    for (auto& o : out) {
        BOOST_TEST(o.resolved.load());
    }
}

BOOST_AUTO_TEST_CASE(property_replication_emits_commit_metrics, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Property 82.9: Replication emits commit metrics");

    outcome out;
    cluster c;
    settle(c);
    const auto committed_before = c.metrics(1).count("entries_committed");
    const auto index = put(c, "k", out);
    BOOST_REQUIRE(c.pump([&] {
        return commit_index(c.leader()) >= index && match_of(c.leader(), 2) >= index &&
               match_of(c.leader(), 3) >= index;
    }));

    BOOST_REQUIRE(c.metrics(1).count("entries_committed") > committed_before);
    BOOST_TEST(c.metrics(1).dimension_values("entries_committed", "node_id").back() == "1");
    const auto targets = c.metrics(1).dimension_values("raft_entries_replicated", "target");
    BOOST_TEST(std::count(targets.begin(), targets.end(), "2") > 0);
    BOOST_TEST(std::count(targets.begin(), targets.end(), "3") > 0);
    BOOST_TEST(std::count(targets.begin(), targets.end(), "1") == 0);
    const auto advanced = c.logs(1).field_values("Advanced commit index", "new_commit_index");
    BOOST_REQUIRE(!advanced.empty());
    BOOST_TEST(advanced.back() == std::to_string(commit_index(c.leader())));
    // Followers learn the commit index; they do not compute it.
    BOOST_TEST(c.metrics(2).count("entries_committed") == 0u);
}

BOOST_AUTO_TEST_CASE(property_replication_handles_follower_catchup,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 82.10: Replication handles follower catch-up");

    // Node 3 misses twenty commits, then comes back.
    std::array<outcome, 20> missed;
    outcome after;
    cluster c;
    settle(c);
    c.isolate(3);
    std::uint64_t last = 0;
    for (std::size_t i = 0; i < missed.size(); ++i) {
        last = put(c, "m" + std::to_string(i), missed[i]);
    }
    BOOST_REQUIRE(c.pump([&] { return commit_index(c.leader()) >= last; }));
    BOOST_TEST(c.node(3).last_log_index() < last);

    c.reconnect(3);
    BOOST_REQUIRE(c.pump(
        [&] { return match_of(c.leader(), 3) >= last && c.node(3).last_applied_index() >= last; }));
    BOOST_TEST(commit_index(c.node(3)) >= last);

    // Caught up, node 3 counts toward the majority again: with node 2 cut
    // off, the next command commits on node 3's acknowledgement.
    c.isolate(2);
    const auto index = put(c, "after", after);
    BOOST_REQUIRE(c.pump([&] { return after.done(); }));
    BOOST_TEST(after.resolved.load());
    BOOST_TEST(match_of(c.leader(), 3) >= index);
    BOOST_TEST(match_of(c.leader(), 2) < index);
}
