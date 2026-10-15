// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE raft_replicate_to_followers_property_test
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
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace kythira::test::membership;

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("raft_replicate_to_followers_property_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

using record = std::map<std::string, std::string>;

/// The fields of every entry `sink` logged as `message`, in order.
auto records(log_sink& sink, std::string_view message) -> std::vector<record> {
    const std::lock_guard<std::mutex> lock(sink.mutex);
    std::vector<record> out;
    for (const auto& e : sink.entries) {
        if (e.message == message) {
            out.push_back(e.fields);
        }
    }
    return out;
}

/// As records(), keeping only entries whose `field` is `value`.
auto records_where(log_sink& sink, std::string_view message, const std::string& field,
                   const std::string& value) -> std::vector<record> {
    auto all = records(sink, message);
    std::erase_if(all, [&](const record& r) {
        auto it = r.find(field);
        return it == r.end() || it->second != value;
    });
    return all;
}

/// How many messages `sink` logged that start with `prefix`. Some messages
/// are formatted strings with their values inline.
auto count_prefix(log_sink& sink, std::string_view prefix) -> std::size_t {
    const std::lock_guard<std::mutex> lock(sink.mutex);
    return static_cast<std::size_t>(
        std::count_if(sink.entries.begin(), sink.entries.end(),
                      [&](const log_sink::entry& e) { return e.message.starts_with(prefix); }));
}

auto num(const record& r, const std::string& field) -> std::uint64_t {
    return std::stoull(r.at(field));
}

auto commit_index(test_node& n) -> std::uint64_t {
    return n.debug_state().commit_index;
}

auto match_of(test_node& leader, std::uint64_t peer) -> std::uint64_t {
    return leader.match_index_of(peer).value_or(0);
}

/// Every entry's (index, term) in `n`'s log.
auto shape(test_node& n) -> std::vector<std::pair<std::uint64_t, std::uint64_t>> {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    for (const auto& e : n.debug_state().log) {
        out.emplace_back(e.index(), e.term());
    }
    return out;
}

/// Whether `n`'s log holds a put of `key`.
auto holds_put(test_node& n, const std::string& key) -> bool {
    const auto command = sm_t::make_put_command(key, "v");
    const auto log = n.debug_state().log;
    return std::any_of(log.begin(), log.end(),
                       [&](const auto& e) { return e.command() == command; });
}

/// Submits a put to the leader and returns the index of its entry.
auto put(cluster& c, const std::string& key, outcome& out) -> std::uint64_t {
    c.submit(key, out);
    return c.leader().last_log_index();
}

/// Commits `n` puts named `prefix0`.. through the leader.
auto commit_puts(cluster& c, const std::string& prefix, std::size_t n) -> std::uint64_t {
    std::vector<std::unique_ptr<outcome>> outs;
    std::uint64_t last = 0;
    for (std::size_t i = 0; i < n; ++i) {
        outs.push_back(std::make_unique<outcome>());
        last = put(c, prefix + std::to_string(i), *outs.back());
    }
    BOOST_REQUIRE(c.pump([&] {
        return std::all_of(outs.begin(), outs.end(), [](const auto& o) { return o->done(); });
    }));
    for (const auto& o : outs) {
        BOOST_REQUIRE_MESSAGE(o->resolved.load(), o->message());
    }
    return last;
}

/// The fast config, but with AppendEntries replies allowed to take seconds:
/// a slowed follower's late replies then still count instead of timing out.
auto patient_config() -> kythira::raft_configuration {
    auto cfg = make_fast_config();
    cfg._append_entries_timeout = 4000ms;
    return cfg;
}

/// Waits until node `id` has stopped reporting failed AppendEntries for two
/// seconds, longer than the error handler's whole backoff (100 + 200 + 400 +
/// 800 ms). The error handler retries a failed send after a backoff, so a
/// retry started while a follower was cut off can still deliver once the
/// follower is reconnected; cases that need the follower to stay behind wait
/// those retries out first.
auto quiesce(cluster& c, std::uint64_t id) -> void {
    auto failures = count_prefix(c.logs(id), "AppendEntries RPC failed after retries");
    for (;;) {
        std::this_thread::sleep_for(2000ms);
        const auto now = count_prefix(c.logs(id), "AppendEntries RPC failed after retries");
        if (now == failures) {
            return;
        }
        failures = now;
    }
}

/// Ticks the leader until both followers hold its whole log, election no-op
/// included, so isolating one later leaves it merely behind.
auto settle(cluster& c) -> void {
    const auto last = c.leader().last_log_index();
    BOOST_REQUIRE(
        c.pump([&] { return match_of(c.leader(), 2) >= last && match_of(c.leader(), 3) >= last; }));
}

/// Ticks `n`'s heartbeat until `pred` holds, for a leader other than node 1.
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

/// Lets node 2 win an election. Node 1 stops being ticked and is isolated,
/// so nothing it sends afterwards confirms its leadership.
auto elect_node_2(cluster& c) -> void {
    c.isolate(1);
    BOOST_REQUIRE(wait_until(
        [&] {
            std::this_thread::sleep_for(c.config()._election_timeout_max + 20ms);
            c.node(2).check_election_timeout();
            return wait_until([&] { return c.node(2).is_leader(); }, 1000ms);
        },
        8000ms));
}

/// Cuts node 3 off, commits `before` puts, snapshots the leader at its last
/// applied entry (compacting the log past everything node 3 holds),
/// reconnects node 3 and commits `after` more. Returns the snapshot's index.
auto leave_node_3_behind_a_snapshot(cluster& c, std::size_t before, std::size_t after)
    -> std::uint64_t {
    settle(c);
    const auto node_3_last = c.node(3).last_log_index();
    c.isolate(3);
    const auto last = commit_puts(c, "s", before);
    BOOST_REQUIRE(c.pump([&] { return c.leader().last_applied_index() >= last; }));
    quiesce(c, 1);
    BOOST_REQUIRE(c.node(3).last_log_index() == node_3_last);
    c.leader().create_snapshot();
    const auto log = c.leader().debug_state().log;
    BOOST_REQUIRE(log.empty() || log.front().index() > node_3_last + 1);
    c.reconnect(3);
    if (after > 0) {
        commit_puts(c, "t", after);
    }
    return last;
}

}  // namespace

/**
 * Property 88: Replicate to Followers Implementation
 *
 * The leader sends each follower the entries from its next index on, or a
 * snapshot when those entries are compacted away, moves next and match
 * indices with the replies, backs off on rejections using the follower's
 * conflict hint, and commits what a majority (itself included) holds. The
 * cases drive the shared three-node simulator cluster led by node 1.
 *
 * Validates: Requirements 7.1, 7.2, 7.3, 16.3, 20.1, 20.2, 20.3
 */

BOOST_AUTO_TEST_CASE(property_replicate_sends_parallel_append_entries,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.1: Replicate sends parallel AppendEntries RPCs");

    // Each round sends to both followers, and a follower whose replies take
    // 1.5 s holds nothing up: node 2's acknowledgement commits the put while
    // node 3's is still on its way.
    outcome out;
    cluster c{patient_config()};
    settle(c);
    c.slow(3, 1, 1500ms);
    const auto index = put(c, "k", out);
    const auto started = std::chrono::steady_clock::now();
    BOOST_REQUIRE(c.pump([&] { return out.done(); }));
    BOOST_TEST(out.resolved.load(), out.message());
    BOOST_TEST((std::chrono::steady_clock::now() - started) < 1000ms);

    std::set<std::string> targets;
    for (const auto& r : records(c.logs(1), "Sending AppendEntries")) {
        if (num(r, "num_entries") > 0 && num(r, "prev_log_index") + 1 == index) {
            targets.insert(r.at("target"));
        }
    }
    BOOST_TEST((targets == std::set<std::string>{"2", "3"}));
}

BOOST_AUTO_TEST_CASE(property_replicate_uses_future_collector, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.2: Replicate handles each follower's reply on its own");

    // There is no collector to wait on: each reply is counted as it lands. The
    // put commits on node 2's reply alone, and node 3's late reply still
    // moves its match index afterwards.
    outcome out;
    cluster c{patient_config()};
    settle(c);
    c.slow(3, 1, 1500ms);
    const auto index = put(c, "k", out);
    BOOST_REQUIRE(c.pump([&] { return out.done(); }));
    BOOST_TEST(match_of(c.leader(), 3) < index);
    BOOST_REQUIRE(c.pump([&] { return match_of(c.leader(), 3) >= index; }));
    BOOST_TEST(commit_index(c.leader()) >= index);
}

BOOST_AUTO_TEST_CASE(property_replicate_updates_next_index_on_success,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.3: Replicate updates next_index on success");

    outcome out;
    cluster c;
    settle(c);
    const auto index = put(c, "k", out);
    BOOST_REQUIRE(c.pump([&] { return match_of(c.leader(), 2) >= index; }));
    const auto acks = records_where(c.logs(1), "AppendEntries succeeded", "target", "2");
    BOOST_REQUIRE(!acks.empty());
    BOOST_TEST(std::any_of(acks.begin(), acks.end(),
                           [&](const record& r) { return num(r, "next_index") == index + 1; }));

    // The next round starts from there: its prev entry is the put.
    c.pump_for(200ms);
    const auto sent = records_where(c.logs(1), "Sending AppendEntries", "target", "2");
    BOOST_TEST(num(sent.back(), "prev_log_index") == index);
}

BOOST_AUTO_TEST_CASE(property_replicate_updates_match_index_on_success,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.4: Replicate updates match_index on success");

    std::array<outcome, 3> out;
    cluster c;
    settle(c);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto index = put(c, "k" + std::to_string(i), out.at(i));
        BOOST_REQUIRE(c.pump([&] { return out.at(i).done(); }));
        BOOST_REQUIRE(c.pump(
            [&] { return match_of(c.leader(), 2) >= index && match_of(c.leader(), 3) >= index; }));
        BOOST_TEST(match_of(c.leader(), 2) == index);
        BOOST_TEST(match_of(c.leader(), 3) == index);
    }
}

BOOST_AUTO_TEST_CASE(property_replicate_decrements_next_index_on_rejection,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.5: Replicate backs next_index off on rejection");

    // Node 1 appends a put while cut off from both followers, so only its log
    // has it. Node 2 then wins the next term and puts its own no-op at that
    // index. Node 2's first AppendEntries to node 1 names a prev entry node 1
    // holds in another term; node 1 rejects it, node 2 backs next_index off,
    // and node 1's stray entry is overwritten.
    outcome stray;
    cluster c;
    settle(c);
    c.isolate(2);
    c.isolate(3);
    const auto stray_index = put(c, "stray", stray);
    c.pump_for(100ms);
    quiesce(c, 1);
    c.reconnect(2);
    c.reconnect(3);
    elect_node_2(c);
    const auto node_2_last = c.node(2).last_log_index();
    BOOST_REQUIRE(node_2_last >= stray_index);
    BOOST_REQUIRE(pump_node(c.node(2), [&] { return commit_index(c.node(2)) >= node_2_last; }));

    c.reconnect(1);
    BOOST_REQUIRE(pump_node(c.node(2), [&] { return match_of(c.node(2), 1) >= node_2_last; }));
    BOOST_TEST(!c.leader().is_leader());
    BOOST_TEST(c.logs(1).count("Rejecting AppendEntries: term mismatch at prevLogIndex") >= 1u);
    const auto retries = records_where(c.logs(2), "Retrying with new next_index", "target", "1");
    BOOST_REQUIRE(!retries.empty());
    BOOST_TEST(num(retries.front(), "new_next_index") <= stray_index);
    BOOST_TEST(!holds_put(c.leader(), "stray"));
    BOOST_TEST((shape(c.leader()) == shape(c.node(2))));
    BOOST_REQUIRE(wait_until([&] { return stray.done(); }));
    BOOST_TEST(stray.failed.load());
}

BOOST_AUTO_TEST_CASE(property_replicate_uses_conflict_info_optimization,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.6: Replicate uses conflict info optimization");

    // Node 3 misses twelve commits. Node 2 then wins the next term and starts
    // node 3's next index past its own log. Node 3's rejection says where its
    // log ends, and node 2 jumps straight there: one rejection, not twelve.
    cluster c;
    settle(c);
    c.isolate(3);
    commit_puts(c, "k", 12);
    quiesce(c, 1);
    const auto node_3_last = c.node(3).last_log_index();
    c.reconnect(3);
    elect_node_2(c);
    const auto node_2_last = c.node(2).last_log_index();
    BOOST_REQUIRE(node_2_last > node_3_last + 10);
    BOOST_REQUIRE(pump_node(c.node(2), [&] { return match_of(c.node(2), 3) >= node_2_last; }));

    BOOST_TEST(c.logs(3).count("Rejecting AppendEntries: log too short") >= 1u);
    const auto rejected =
        records_where(c.logs(2), "AppendEntries rejected, decrementing next_index", "target", "3");
    BOOST_TEST(rejected.size() <= 2u, "node 2 walked back one entry at a time");
    const auto retries = records_where(c.logs(2), "Retrying with new next_index", "target", "3");
    BOOST_REQUIRE(!retries.empty());
    BOOST_TEST(num(retries.front(), "new_next_index") == node_3_last + 1);
    BOOST_TEST((shape(c.node(3)) == shape(c.node(2))));
}

BOOST_AUTO_TEST_CASE(property_replicate_detects_snapshot_needed, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.7: Replicate detects when snapshot is needed");

    cluster c;
    leave_node_3_behind_a_snapshot(c, 8, 1);
    BOOST_REQUIRE(
        c.pump([&] { return c.logs(1).count("Sending InstallSnapshot to follower") > 0; }));
    const auto detected =
        records_where(c.logs(1), "Follower needs snapshot", "follower", "3").size() +
        records_where(c.logs(1), "Previous entry not in log, switching to snapshot", "target", "3")
            .size();
    BOOST_TEST(detected >= 1u);
    // Node 2 holds everything and is never sent a snapshot.
    BOOST_TEST(
        records_where(c.logs(1), "Sending InstallSnapshot to follower", "target", "2").empty());
}

BOOST_AUTO_TEST_CASE(property_replicate_switches_to_install_snapshot,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.8: Replicate switches to InstallSnapshot");

    // Node 3 takes the snapshot, then the entries after it by AppendEntries.
    cluster c;
    const auto snapshot_index = leave_node_3_behind_a_snapshot(c, 8, 2);
    const auto last = c.leader().last_log_index();
    BOOST_REQUIRE(c.pump([&] { return match_of(c.leader(), 3) >= last; }));
    BOOST_TEST(
        !records_where(c.logs(1), "Sending InstallSnapshot to follower", "target", "3").empty());
    const auto installed = records(c.logs(3), "Successfully installed snapshot");
    BOOST_REQUIRE_EQUAL(installed.size(), 1u);
    BOOST_TEST(num(installed.front(), "last_included_index") == snapshot_index);
    BOOST_REQUIRE(c.pump([&] { return c.node(3).last_applied_index() >= last; }));
    BOOST_TEST(holds_put(c.node(3), "t1"));
}

BOOST_AUTO_TEST_CASE(property_replicate_batches_log_entries, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.9: Replicate batches log entries");

    // Node 3 misses thirty commits and gets them back in one AppendEntries.
    cluster c;
    settle(c);
    c.isolate(3);
    const auto last = commit_puts(c, "k", 30);
    c.reconnect(3);
    BOOST_REQUIRE(c.pump([&] { return match_of(c.leader(), 3) >= last; }));
    std::uint64_t largest = 0;
    for (const auto& r : records_where(c.logs(1), "Sending AppendEntries", "target", "3")) {
        largest = std::max(largest, num(r, "num_entries"));
    }
    BOOST_TEST(largest >= 30u);
}

BOOST_AUTO_TEST_CASE(property_replicate_respects_batch_size_limit, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.10: Replicate respects batch size limit");

    auto cfg = make_fast_config();
    cfg._max_entries_per_append = 5;
    cluster c{cfg};
    settle(c);
    c.isolate(3);
    const auto last = commit_puts(c, "k", 23);
    c.reconnect(3);
    BOOST_REQUIRE(c.pump([&] { return match_of(c.leader(), 3) >= last; }));
    std::uint64_t largest = 0;
    for (const auto& r : records(c.logs(1), "Sending AppendEntries")) {
        largest = std::max(largest, num(r, "num_entries"));
    }
    BOOST_TEST(largest == 5u);
    BOOST_TEST((shape(c.node(3)) == shape(c.leader())));
}

BOOST_AUTO_TEST_CASE(property_replicate_includes_leader_commit_index,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.11: Replicate includes leader commit index");

    // The round after a commit carries the new commit index, and the
    // followers take it as theirs.
    outcome out;
    cluster c;
    settle(c);
    const auto index = put(c, "k", out);
    BOOST_REQUIRE(c.pump([&] { return out.done(); }));
    BOOST_REQUIRE(c.pump(
        [&] { return commit_index(c.node(2)) >= index && commit_index(c.node(3)) >= index; }));
    const auto sent = records(c.logs(1), "Sending AppendEntries");
    BOOST_TEST(std::any_of(sent.begin(), sent.end(),
                           [&](const record& r) { return num(r, "leader_commit") >= index; }));
    BOOST_TEST(commit_index(c.node(2)) <= commit_index(c.leader()));
}

BOOST_AUTO_TEST_CASE(property_replicate_calculates_prev_log_correctly,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.12: Replicate calculates prev log correctly");

    // Every AppendEntries names, as its prev entry, the entry before the
    // first one it carries, with that entry's term in the leader's log.
    cluster c;
    settle(c);
    commit_puts(c, "k", 4);
    c.pump_for(100ms);
    std::map<std::uint64_t, std::uint64_t> terms;
    for (const auto& [index, term] : shape(c.leader())) {
        terms[index] = term;
    }
    const auto sent = records(c.logs(1), "Sending AppendEntries");
    BOOST_REQUIRE(!sent.empty());
    for (const auto& r : sent) {
        const auto prev = num(r, "prev_log_index");
        const auto expected = prev == 0 ? 0u : terms.at(prev);
        BOOST_TEST(num(r, "prev_log_term") == expected, "prev entry " << prev);
    }
}

BOOST_AUTO_TEST_CASE(property_replicate_handles_compacted_prev_entry,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.13: Replicate handles compacted prev entry");

    // Node 3 holds exactly the leader's snapshot. Its next entry is the first
    // after the snapshot, so the prev entry is the compacted boundary: the
    // leader names it with the snapshot's term and sends AppendEntries, not
    // a snapshot.
    cluster c;
    settle(c);
    const auto boundary = commit_puts(c, "k", 4);
    // Both followers, not just node 3: the leader commits with node 3 alone,
    // and a node 2 still behind the boundary would be sent the snapshot.
    BOOST_REQUIRE(c.pump([&] {
        return match_of(c.leader(), 2) >= boundary && match_of(c.leader(), 3) >= boundary &&
               c.leader().last_applied_index() >= boundary;
    }));
    c.isolate(3);
    const auto boundary_term = shape(c.leader()).back().second;
    c.leader().create_snapshot();
    const auto last = commit_puts(c, "m", 3);
    c.reconnect(3);
    BOOST_REQUIRE(c.pump([&] { return match_of(c.leader(), 3) >= last; }));

    BOOST_TEST(c.logs(1).count("Sending InstallSnapshot to follower") == 0u);
    const auto sent = records_where(c.logs(1), "Sending AppendEntries", "target", "3");
    BOOST_TEST(std::any_of(sent.begin(), sent.end(), [&](const record& r) {
        return num(r, "prev_log_index") == boundary && num(r, "prev_log_term") == boundary_term &&
               num(r, "num_entries") > 0;
    }));
    BOOST_TEST(holds_put(c.node(3), "m2"));
}

BOOST_AUTO_TEST_CASE(property_replicate_advances_commit_on_majority,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.14: Replicate advances commit on majority");

    outcome out;
    cluster c;
    settle(c);
    c.isolate(3);
    const auto index = put(c, "k", out);
    BOOST_REQUIRE(c.pump([&] { return out.done(); }));
    BOOST_TEST(out.resolved.load());
    BOOST_TEST(commit_index(c.leader()) >= index);
    BOOST_TEST(match_of(c.leader(), 3) < index);
}

BOOST_AUTO_TEST_CASE(property_replicate_includes_leader_in_majority,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.15: Replicate includes leader in majority");

    // With both followers cut off nothing commits. One follower's
    // acknowledgement plus the leader's own then makes two of three.
    outcome out;
    cluster c;
    settle(c);
    c.isolate(2);
    c.isolate(3);
    const auto index = put(c, "k", out);
    c.pump_for(300ms);
    BOOST_TEST(!out.done());
    BOOST_TEST(commit_index(c.leader()) < index);
    c.reconnect(2);
    BOOST_REQUIRE(c.pump([&] { return out.done(); }));
    BOOST_TEST(out.resolved.load());
    BOOST_TEST(match_of(c.leader(), 3) < index);
}

BOOST_AUTO_TEST_CASE(property_replicate_handles_single_node_cluster,
                     *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Property 88.16: Replicate handles single-node cluster");

    // A one-voter cluster commits on its own append and sends nothing.
    outcome out;
    sim_t sim;
    sim.start();
    auto log = std::make_shared<log_sink>();
    auto net = sim.create_node("1");
    test_node n{kythira::node_config<test_types>{
        .node_id = 1,
        .network_client = {net, test_types::serializer_type{}},
        .network_server = {net, test_types::serializer_type{}},
        .persistence = {},
        .logger = recording_logger{log},
        .metrics = recording_metrics{},
        .membership = {},
        .config = make_fast_config(),
        .self_address = "1",
        .peer_discovery = preset_peer_discovery<std::uint64_t, std::string>{},
    }};
    n.set_cluster_configuration({1});
    n.start();
    BOOST_REQUIRE(wait_until([&] {
        std::this_thread::sleep_for(make_fast_config()._election_timeout_max + 20ms);
        n.check_election_timeout();
        return n.is_leader();
    }));

    watch(n.submit_command(sm_t::make_put_command("k", "v"), 3000ms), out);
    BOOST_REQUIRE(wait_until([&] {
        n.check_heartbeat_timeout();
        return out.done();
    }));
    BOOST_TEST(out.resolved.load(), out.message());
    BOOST_TEST(n.debug_state().commit_index == n.last_log_index());
    BOOST_TEST(log->count("Sending AppendEntries") == 0u);

    n.stop();
    sim.stop();
}

BOOST_AUTO_TEST_CASE(property_replicate_checks_leader_state, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.17: Replicate checks leader state");

    // Once deposed, node 1's ticks send no more AppendEntries.
    cluster c;
    settle(c);
    depose_leader(c);
    const auto before = c.logs(1).count("Sending AppendEntries");
    c.pump_for(300ms);
    BOOST_TEST(c.logs(1).count("Sending AppendEntries") == before);
}

BOOST_AUTO_TEST_CASE(property_replicate_handles_higher_term_in_response,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.18: Replicate handles higher term in response");

    // Node 3 votes in a later term. The leader's next AppendEntries to it
    // comes back with that term, and the leader steps down to it.
    cluster c;
    settle(c);
    const auto term = c.leader().get_current_term();
    auto client = c.raw_client();
    kythira::request_vote_request<> rv{._term = term + 3,
                                       ._candidate_id = 2,
                                       ._last_log_index = c.leader().last_log_index() + 10,
                                       ._last_log_term = term + 3};
    BOOST_REQUIRE(client.send_request_vote(3, rv, 1000ms).get().vote_granted());
    BOOST_REQUIRE(c.pump([&] { return !c.leader().is_leader(); }));
    BOOST_TEST(c.leader().get_current_term() == term + 3);
    BOOST_TEST(count_prefix(c.logs(1), "Discovered higher term in AppendEntries response") >= 1u);
}

BOOST_AUTO_TEST_CASE(property_replicate_ignores_stale_responses, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.19: Replicate ignores stale responses");

    // Node 3's replies take 800 ms. Node 1 is deposed while some are in
    // flight; when they land, node 1 is a follower in a later term and
    // records none of them.
    outcome out;
    cluster c;
    settle(c);
    c.slow(3, 1, 800ms);
    put(c, "k", out);
    BOOST_REQUIRE(c.pump([&] { return out.done(); }));
    c.leader().check_heartbeat_timeout();
    depose_leader(c);
    const auto acks_before =
        records_where(c.logs(1), "AppendEntries succeeded", "target", "3").size();
    std::this_thread::sleep_for(1500ms);
    BOOST_TEST(!c.leader().is_leader());
    BOOST_TEST(records_where(c.logs(1), "AppendEntries succeeded", "target", "3").size() ==
               acks_before);
    BOOST_TEST(!c.leader().match_index_of(3).has_value());
}

BOOST_AUTO_TEST_CASE(property_replicate_uses_error_handler_for_retry,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.20: Replicate uses error handler for retry");

    // AppendEntries to an unreachable follower are retried by the error
    // handler and, once its attempts run out, reported as failed for that
    // follower alone.
    cluster c;
    settle(c);
    c.isolate(3);
    BOOST_REQUIRE(
        c.pump([&] { return c.metrics(1).count("raft_append_entries_failed") > 0; }, 8000ms));
    const auto targets = c.metrics(1).dimension_values("raft_append_entries_failed", "target");
    BOOST_TEST(
        std::all_of(targets.begin(), targets.end(), [](const std::string& t) { return t == "3"; }));
    BOOST_TEST(count_prefix(c.logs(1), "AppendEntries RPC failed after retries") >= 1u);
}

BOOST_AUTO_TEST_CASE(property_replicate_emits_replication_metrics, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.21: Replicate emits replication metrics");

    outcome out;
    cluster c;
    settle(c);
    put(c, "k", out);
    BOOST_REQUIRE(c.pump([&] { return out.done(); }));
    c.pump_for(100ms);
    auto& m = c.metrics(1);
    BOOST_TEST(m.count("raft_replication_round") > 0u);
    for (const char* name : {"raft_append_entries_latency", "raft_entries_replicated"}) {
        auto targets = m.dimension_values(name, "target");
        std::set<std::string> distinct(targets.begin(), targets.end());
        BOOST_TEST((distinct == std::set<std::string>{"2", "3"}), name);
    }
    BOOST_TEST(m.count("raft_append_entries_failed") == 0u);
}

BOOST_AUTO_TEST_CASE(property_replicate_logs_replication_progress, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.22: Replicate logs replication progress");

    outcome out;
    cluster c;
    settle(c);
    const auto index = put(c, "k", out);
    // Each follower's ack is checked below, so wait for both of them.
    BOOST_REQUIRE(c.pump(
        [&] { return match_of(c.leader(), 2) >= index && match_of(c.leader(), 3) >= index; }));
    BOOST_TEST(c.logs(1).count("Replicating to followers") > 0u);
    for (const std::string target : {"2", "3"}) {
        const auto sent = records_where(c.logs(1), "Sending AppendEntries", "target", target);
        BOOST_TEST(std::any_of(sent.begin(), sent.end(),
                               [](const record& r) { return num(r, "num_entries") > 0; }));
        const auto acks = records_where(c.logs(1), "AppendEntries succeeded", "target", target);
        BOOST_TEST(std::any_of(acks.begin(), acks.end(),
                               [&](const record& r) { return num(r, "match_index") == index; }));
    }
}

BOOST_AUTO_TEST_CASE(property_replicate_handles_missing_log_entries,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.23: Replicate handles entries missing from the leader's log");

    // The entries node 3 needs are gone from the leader's log (compacted into
    // a snapshot). Node 3 is sent the snapshot; node 2 keeps committing
    // throughout.
    std::array<outcome, 3> out;
    cluster c;
    leave_node_3_behind_a_snapshot(c, 6, 0);
    for (std::size_t i = 0; i < out.size(); ++i) {
        put(c, "after" + std::to_string(i), out.at(i));
        BOOST_REQUIRE(c.pump([&] { return out.at(i).done(); }));
        BOOST_TEST(out.at(i).resolved.load());
    }
    const auto last = c.leader().last_log_index();
    BOOST_REQUIRE(c.pump([&] { return match_of(c.leader(), 3) >= last; }));
    BOOST_TEST(c.logs(1).count("Log entry not found during replication") == 0u);
    BOOST_TEST(holds_put(c.node(3), "after2"));
}

BOOST_AUTO_TEST_CASE(property_replicate_handles_network_failures, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.24: Replicate handles network failures");

    // Node 3 is unreachable for three commits and fine again afterwards.
    cluster c;
    settle(c);
    c.isolate(3);
    const auto last = commit_puts(c, "k", 3);
    BOOST_TEST(match_of(c.leader(), 3) < last);
    c.reconnect(3);
    BOOST_REQUIRE(c.pump([&] { return match_of(c.leader(), 3) >= last; }));
    BOOST_TEST((shape(c.node(3)) == shape(c.leader())));
}

BOOST_AUTO_TEST_CASE(property_replicate_continues_after_collection_failure,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.25: Replicate continues after a round without a majority");

    // Rounds that reach no follower commit nothing but lose nothing either:
    // the put commits as soon as one follower is back.
    outcome out;
    cluster c;
    settle(c);
    c.isolate(2);
    c.isolate(3);
    const auto index = put(c, "k", out);
    c.pump_for(500ms);
    BOOST_TEST(!out.done());
    c.reconnect(3);
    BOOST_REQUIRE(c.pump([&] { return out.done(); }));
    BOOST_TEST(out.resolved.load());
    BOOST_TEST(commit_index(c.leader()) >= index);
}

BOOST_AUTO_TEST_CASE(property_replicate_tracks_follower_lag, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.26: Replicate tracks follower lag");

    // raft_follower_lag reports, per follower, how far its match index trails
    // the leader's log: nothing for a caught-up follower. A follower slowed
    // while commands keep arriving reports some lag, and none once it has
    // caught up.
    std::array<outcome, 6> out;
    cluster c{patient_config()};
    settle(c);
    c.slow(3, 1, 300ms);
    for (std::size_t i = 0; i < out.size(); ++i) {
        put(c, "k" + std::to_string(i), out.at(i));
        c.leader().check_heartbeat_timeout();
    }
    BOOST_REQUIRE(c.pump([&] {
        return std::all_of(out.begin(), out.end(), [](const auto& o) { return o.done(); });
    }));
    const auto last = c.leader().last_log_index();
    BOOST_REQUIRE(c.pump([&] { return match_of(c.leader(), 3) >= last; }));
    c.pump_for(800ms);

    const auto lag_3 = c.metrics(1).values_where("raft_follower_lag", "target", "3");
    BOOST_REQUIRE(!lag_3.empty());
    BOOST_TEST(*std::max_element(lag_3.begin(), lag_3.end()) > 0.0);
    BOOST_TEST(lag_3.back() == 0.0);
    const auto lag_2 = c.metrics(1).values_where("raft_follower_lag", "target", "2");
    BOOST_REQUIRE(!lag_2.empty());
    BOOST_TEST(lag_2.back() == 0.0);
}

BOOST_AUTO_TEST_CASE(property_replicate_preserves_log_consistency, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.27: Replicate preserves log consistency");

    // Each follower in turn misses some commits; once all are back, every
    // log is the leader's, entry for entry.
    cluster c;
    settle(c);
    for (std::uint64_t cut : {2u, 3u, 2u}) {
        c.isolate(cut);
        commit_puts(c, "k" + std::to_string(cut), 4);
        c.reconnect(cut);
    }
    const auto last = c.leader().last_log_index();
    BOOST_REQUIRE(c.pump([&] {
        return match_of(c.leader(), 2) >= last && match_of(c.leader(), 3) >= last &&
               commit_index(c.node(2)) >= last && commit_index(c.node(3)) >= last;
    }));
    BOOST_TEST((shape(c.node(2)) == shape(c.leader())));
    BOOST_TEST((shape(c.node(3)) == shape(c.leader())));
    BOOST_TEST(c.node(2).debug_state().log.back().command() ==
               c.leader().debug_state().log.back().command());
}

BOOST_AUTO_TEST_CASE(property_replicate_maintains_next_index_invariant,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.28: Replicate maintains next_index invariant");

    // Every acknowledgement that moves a follower's match index sets its next
    // index one past it. (A late acknowledgement of an older, shorter request
    // moves neither, and is logged with the indices as they already were.)
    cluster c;
    settle(c);
    c.isolate(3);
    commit_puts(c, "k", 5);
    c.reconnect(3);
    const auto last = c.leader().last_log_index();
    BOOST_REQUIRE(c.pump([&] { return match_of(c.leader(), 3) >= last; }));
    const auto acks = records(c.logs(1), "AppendEntries succeeded");
    BOOST_REQUIRE(!acks.empty());
    std::map<std::string, std::uint64_t> highest;
    std::size_t advancing = 0;
    for (const auto& r : acks) {
        auto& seen = highest[r.at("target")];
        if (num(r, "match_index") <= seen) {
            continue;
        }
        seen = num(r, "match_index");
        ++advancing;
        BOOST_TEST(num(r, "next_index") == seen + 1, "target " << r.at("target"));
    }
    BOOST_TEST(advancing >= 2u);
    BOOST_TEST(highest["3"] == last);
}

BOOST_AUTO_TEST_CASE(property_replicate_never_decrements_match_index,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.29: Replicate never decrements match_index");

    // Node 3's replies are delayed by varying amounts so that replies to
    // older, shorter requests land after newer ones; match_index never
    // moves back.
    cluster c;
    settle(c);
    std::atomic<bool> stop{false};
    std::atomic<bool> went_back{false};
    std::thread sampler([&] {
        std::uint64_t seen = 0;
        while (!stop.load()) {
            const auto now = match_of(c.leader(), 3);
            if (now < seen) {
                went_back = true;
            }
            seen = std::max(seen, now);
            std::this_thread::sleep_for(1ms);
        }
    });
    std::vector<std::unique_ptr<outcome>> outs;
    for (std::size_t i = 0; i < 12; ++i) {
        c.slow(3, 1, std::chrono::milliseconds{(i % 3) * 60});
        outs.push_back(std::make_unique<outcome>());
        put(c, "k" + std::to_string(i), *outs.back());
        c.leader().check_heartbeat_timeout();
        std::this_thread::sleep_for(15ms);
    }
    c.slow(3, 1, 0ms);
    const auto last = c.leader().last_log_index();
    const bool caught_up = c.pump([&] { return match_of(c.leader(), 3) >= last; });
    stop = true;
    sampler.join();
    BOOST_REQUIRE(caught_up);
    BOOST_TEST(!went_back.load());
}

BOOST_AUTO_TEST_CASE(property_replicate_handles_concurrent_calls, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 88.30: Replicate handles concurrent calls");

    // Four threads submit five puts each while the leader is ticked. Every
    // put commits once, and the followers' logs end up the leader's.
    constexpr std::size_t threads = 4;
    constexpr std::size_t per_thread = 5;
    std::array<std::array<outcome, per_thread>, threads> out;
    cluster c;
    settle(c);
    std::vector<std::thread> workers;
    for (std::size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            for (std::size_t i = 0; i < per_thread; ++i) {
                watch(c.leader().submit_command(
                          sm_t::make_put_command("k" + std::to_string(t) + "-" + std::to_string(i),
                                                 "v"),
                          3000ms),
                      out.at(t).at(i));
            }
        });
    }
    const bool all_done = c.pump([&] {
        return std::all_of(out.begin(), out.end(), [](const auto& row) {
            return std::all_of(row.begin(), row.end(), [](const auto& o) { return o.done(); });
        });
    });
    for (auto& w : workers) {
        w.join();
    }
    BOOST_REQUIRE(all_done);
    for (auto& row : out) {
        for (auto& o : row) {
            BOOST_TEST(o.resolved.load(), o.message());
        }
    }
    const auto last = c.leader().last_log_index();
    BOOST_REQUIRE(
        c.pump([&] { return match_of(c.leader(), 2) >= last && match_of(c.leader(), 3) >= last; }));
    BOOST_TEST((shape(c.node(2)) == shape(c.leader())));
    BOOST_TEST((shape(c.node(3)) == shape(c.leader())));
    std::size_t puts = 0;
    for (const auto& e : c.leader().debug_state().log) {
        if (e.type() == kythira::entry_type::normal) {
            ++puts;
        }
    }
    BOOST_TEST(puts == threads * per_thread);
}

BOOST_AUTO_TEST_CASE(property_replicate_ignores_stale_rejections, *boost::unit_test::timeout(90)) {
    BOOST_TEST_MESSAGE("Property 88.31: Replicate ignores rejections of superseded requests");

    // Node 2 leads with node 3 twelve entries behind, and node 3's replies
    // take 300 ms. Every heartbeat node 2 sends before the first rejection
    // lands is rejected too. The first rejection moves next_index back and
    // the retry catches node 3 up; the later ones answer requests that next
    // index has moved past, and change nothing.
    cluster c{patient_config()};
    settle(c);
    c.isolate(3);
    commit_puts(c, "k", 12);
    quiesce(c, 1);
    c.reconnect(3);
    elect_node_2(c);
    c.slow(3, 2, 300ms);
    const auto last = c.node(2).last_log_index();
    BOOST_REQUIRE(pump_node(c.node(2), [&] { return match_of(c.node(2), 3) >= last; }, 10000ms));
    // Let every rejection still in flight land.
    const auto until = std::chrono::steady_clock::now() + 1000ms;
    while (std::chrono::steady_clock::now() < until) {
        c.node(2).check_heartbeat_timeout();
        std::this_thread::sleep_for(20ms);
    }

    BOOST_TEST(c.logs(3).count("Rejecting AppendEntries: log too short") >= 2u);
    BOOST_TEST(records_where(c.logs(2), "Retrying with new next_index", "target", "3").size() <=
               2u);
    BOOST_TEST(
        !records_where(c.logs(2), "Ignoring stale AppendEntries rejection", "target", "3").empty());
    BOOST_TEST(match_of(c.node(2), 3) >= last);
    BOOST_TEST((shape(c.node(3)) == shape(c.node(2))));
}
