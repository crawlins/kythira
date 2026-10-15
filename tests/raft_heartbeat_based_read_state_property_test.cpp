// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE raft_heartbeat_based_read_state_property_test
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
#include <string_view>
#include <thread>
#include <vector>

using namespace kythira::test::membership;

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("raft_heartbeat_based_read_state_property_test"),
                         nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

/// How a read settled: the state it returned, or its exception's message.
struct read_result {
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    std::atomic<std::chrono::steady_clock::rep> took{0};
    std::mutex mutex;
    std::vector<std::byte> state;
    std::string error;

    /// Whether the returned state holds `text` (the test state machine
    /// serialises its keys and values as raw bytes).
    [[nodiscard]] auto holds(std::string_view text) -> bool {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto* begin = reinterpret_cast<const char*>(state.data());
        return std::string_view(begin, state.size()).find(text) != std::string_view::npos;
    }
    [[nodiscard]] auto message() -> std::string {
        const std::lock_guard<std::mutex> lock(mutex);
        return error;
    }
    [[nodiscard]] auto elapsed() const -> std::chrono::steady_clock::duration {
        return std::chrono::steady_clock::duration{took.load()};
    }
};

auto read(test_node& n, std::chrono::milliseconds timeout, read_result& out) -> void {
    out.started = std::chrono::steady_clock::now();
    n.read_state(timeout)
        .thenValue([&out](std::vector<std::byte> v) {
            {
                const std::lock_guard<std::mutex> lock(out.mutex);
                out.state = std::move(v);
            }
            out.took = (std::chrono::steady_clock::now() - out.started).count();
            out.done = true;
        })
        .thenError([&out](const std::exception_ptr& e) {
            {
                const std::lock_guard<std::mutex> lock(out.mutex);
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    out.error = ex.what();
                } catch (...) {
                    out.error = "non-standard exception";
                }
            }
            out.took = (std::chrono::steady_clock::now() - out.started).count();
            out.failed = true;
            out.done = true;
        })
        .detach();
}

auto commit_index(test_node& n) -> std::uint64_t {
    return n.debug_state().commit_index;
}

/// Ticks the leader until both followers hold its whole log, election no-op
/// included, so later isolation leaves no follower stuck rejecting entries.
auto settle(cluster& c) -> void {
    const auto last = c.leader().last_log_index();
    BOOST_REQUIRE(c.pump([&] {
        return c.leader().match_index_of(2).value_or(0) >= last &&
               c.leader().match_index_of(3).value_or(0) >= last;
    }));
}

/// Commits a put of `key` through the leader and waits for its client.
auto commit_put(cluster& c, const std::string& key, outcome& out) -> void {
    c.submit(key, out);
    BOOST_REQUIRE(c.pump([&] { return out.done(); }));
    BOOST_REQUIRE(out.resolved.load());
}

/// The targets of the heartbeats `id` sent to confirm reads.
auto read_heartbeat_targets(cluster& c, std::uint64_t id) -> std::vector<std::string> {
    return c.logs(id).field_values("Sending heartbeat for read verification", "target");
}

}  // namespace

/**
 * Property 80: Heartbeat-Based Read State Implementation
 *
 * read_state() serves a linearizable read from the leader's state machine
 * once a round of heartbeats shows a majority still follows it in its term.
 * The cases drive the shared three-node simulator cluster led by node 1.
 *
 * Validates: Requirements 21.1, 21.2, 21.3, 21.4, 21.5, 28.1, 28.2, 28.3, 28.4, 28.5
 */

BOOST_AUTO_TEST_CASE(property_read_state_sends_heartbeats_to_followers,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 80.1: Read state sends heartbeats to all followers");

    outcome put;
    read_result r;
    cluster c;
    settle(c);
    commit_put(c, "read-key", put);

    read(c.leader(), 2000ms, r);
    BOOST_REQUIRE(wait_until([&] { return r.done.load(); }));
    BOOST_TEST(!r.failed.load(), r.message());
    BOOST_TEST(r.holds("read-key"));

    auto targets = read_heartbeat_targets(c, 1);
    std::sort(targets.begin(), targets.end());
    BOOST_TEST((targets == std::vector<std::string>{"2", "3"}));
}

BOOST_AUTO_TEST_CASE(property_read_state_confirms_majority_response,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 80.2: Read state confirms leader validity with majority response");

    // Node 3 never answers. Node 2's reply and the leader's own make a
    // majority, and the read returns without waiting out node 3's timeout.
    outcome put;
    read_result r;
    cluster c;
    settle(c);
    commit_put(c, "read-key", put);
    c.isolate(3);

    read(c.leader(), 3000ms, r);
    BOOST_REQUIRE(wait_until([&] { return r.done.load(); }));
    BOOST_TEST(!r.failed.load(), r.message());
    BOOST_TEST(r.holds("read-key"));
    BOOST_TEST(r.elapsed() < 1500ms, "the read waited for the unreachable follower");
    BOOST_TEST(c.metrics(1).dimension_values("raft_read_request_success", "cluster_type") ==
                   std::vector<std::string>{"multi_node"},
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(property_read_state_rejects_on_heartbeat_failure,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 80.3: Read state rejects read when heartbeat majority fails");

    // Neither follower answers, so the leader cannot show it still leads:
    // the read fails rather than return what may be stale.
    read_result r;
    cluster c;
    settle(c);
    c.isolate(2);
    c.isolate(3);

    read(c.leader(), 500ms, r);
    BOOST_REQUIRE(wait_until([&] { return r.done.load(); }, 3000ms));
    BOOST_TEST(r.failed.load());
    BOOST_TEST(c.metrics(1).dimension_values("raft_read_request_failed", "reason") ==
                   std::vector<std::string>{"heartbeat_collection_failed"},
               boost::test_tools::per_element());
    BOOST_TEST(c.metrics(1).count("raft_read_request_success") == 0u);
    // A failed confirmation alone does not depose the leader.
    BOOST_TEST(c.leader().is_leader());
}

BOOST_AUTO_TEST_CASE(property_read_state_detects_higher_term, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 80.4: Read state detects higher term in heartbeat responses");

    // Node 3 has moved to a later term (it voted for node 2 there); node 2
    // is cut off, so node 3's reply is the one the read counts. Its term
    // shows node 1 may no longer lead: the read fails and node 1 steps down.
    read_result r;
    cluster c;
    settle(c);
    const auto term = c.leader().get_current_term();
    auto client = c.raw_client();
    kythira::request_vote_request<> rv{._term = term + 5,
                                       ._candidate_id = 2,
                                       ._last_log_index = c.leader().last_log_index() + 10,
                                       ._last_log_term = term + 5};
    BOOST_REQUIRE(client.send_request_vote(3, rv, 1000ms).get().vote_granted());
    c.isolate(2);

    read(c.leader(), 2000ms, r);
    BOOST_REQUIRE(wait_until([&] { return r.done.load(); }));
    BOOST_TEST(r.failed.load());
    BOOST_TEST(!c.leader().is_leader());
    BOOST_TEST(c.leader().get_current_term() == term + 5);
    BOOST_TEST(c.metrics(1).dimension_values("raft_read_request_aborted", "reason") ==
                   std::vector<std::string>{"higher_term_discovered"},
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(property_read_state_optimizes_concurrent_reads,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 80.5: Read state optimizes concurrent read requests");

    // Reads do not share a heartbeat round: each read runs its own, so N
    // concurrent reads send N rounds. What a caller can rely on is that every
    // one of them is confirmed and returns the same committed state.
    outcome put;
    std::array<read_result, 5> reads;
    cluster c;
    settle(c);
    commit_put(c, "read-key", put);

    for (auto& r : reads) {
        read(c.leader(), 2000ms, r);
    }
    BOOST_REQUIRE(wait_until([&] {
        return std::all_of(reads.begin(), reads.end(), [](auto& r) { return r.done.load(); });
    }));
    for (auto& r : reads) {
        BOOST_TEST(!r.failed.load(), r.message());
        BOOST_TEST(r.holds("read-key"));
    }
    BOOST_TEST(read_heartbeat_targets(c, 1).size() == 2 * reads.size());
}

BOOST_AUTO_TEST_CASE(property_read_state_rejects_non_leader, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Property 80.6: Read state rejects requests from non-leaders");

    read_result r;
    cluster c;
    settle(c);

    read(c.node(2), 2000ms, r);
    BOOST_REQUIRE(wait_until([&] { return r.done.load(); }, 1000ms));
    BOOST_TEST(r.failed.load());
    BOOST_TEST(r.elapsed() < 500ms);
    BOOST_TEST(read_heartbeat_targets(c, 2).empty());
    BOOST_TEST(c.metrics(2).dimension_values("raft_read_request_rejected", "reason") ==
                   std::vector<std::string>{"not_leader"},
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(property_read_state_handles_single_node_cluster,
                     *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Property 80.7: Read state handles single-node cluster");

    // A one-voter cluster is its own majority: the read needs no heartbeat.
    read_result r;
    sim_t sim;
    sim.start();
    auto log = std::make_shared<log_sink>();
    auto met = std::make_shared<metric_sink>();
    auto net = sim.create_node("1");
    test_node n{kythira::node_config<test_types>{
        .node_id = 1,
        .network_client = {net, test_types::serializer_type{}},
        .network_server = {net, test_types::serializer_type{}},
        .persistence = {},
        .logger = recording_logger{log},
        .metrics = recording_metrics{met},
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

    read(n, 2000ms, r);
    BOOST_REQUIRE(wait_until([&] { return r.done.load(); }, 1000ms));
    BOOST_TEST(!r.failed.load(), r.message());
    BOOST_TEST(log->count("Sending heartbeat for read verification") == 0u);
    BOOST_TEST(met->dimension_values("raft_read_request_success", "cluster_type") ==
                   std::vector<std::string>{"single_node"},
               boost::test_tools::per_element());

    n.stop();
    sim.stop();
}

BOOST_AUTO_TEST_CASE(property_read_state_timeout_handling, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 80.8: Read state handles timeout correctly");

    // Both followers answer, but their replies take two seconds. The read
    // fails once its own 400 ms timeout elapses: not at once, and not when
    // the late replies land. (Isolating them would not do: a send with no
    // route fails at once, which is 80.3's case.)
    read_result r;
    cluster c;
    settle(c);
    c.slow(2, 1, 2000ms);
    c.slow(3, 1, 2000ms);

    read(c.leader(), 400ms, r);
    BOOST_REQUIRE(wait_until([&] { return r.done.load(); }, 3000ms));
    BOOST_TEST(r.failed.load());
    BOOST_TEST(r.elapsed() >= 350ms);
    BOOST_TEST(r.elapsed() < 1500ms);
    BOOST_TEST(c.logs(1).count("Read request failed") == 1u);
}

BOOST_AUTO_TEST_CASE(property_read_state_serves_only_committed_state_of_its_term,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 80.9: A new leader's read includes every acknowledged write");

    // Node 1 commits a put and acknowledges it to its client, then is cut off
    // before its followers learn the new commit index. Node 2 wins the next
    // term holding the put in its log but not yet knowing it committed. A
    // read from node 2 must still see the put (Raft §6.4: a leader serves
    // reads only once it has committed an entry of its own term).
    outcome put;
    read_result r;
    cluster c;
    settle(c);
    c.submit("acknowledged-key", put);
    BOOST_REQUIRE(c.pump([&] { return put.done(); }));
    BOOST_REQUIRE(put.resolved.load());
    c.isolate(1);
    const auto put_index = c.leader().last_log_index();
    BOOST_REQUIRE_MESSAGE(commit_index(c.node(2)) < put_index,
                          "node 2 learned the commit before node 1 was cut off");

    BOOST_REQUIRE(wait_until(
        [&] {
            std::this_thread::sleep_for(c.config()._election_timeout_max + 20ms);
            c.node(2).check_election_timeout();
            return wait_until([&] { return c.node(2).is_leader(); }, 1000ms);
        },
        8000ms));

    // Read before node 2 has committed anything in its term. The read waits
    // for node 2's election no-op to commit, which node 2's own heartbeats
    // drive, and then returns state that holds the put.
    read(c.node(2), 3000ms, r);
    BOOST_REQUIRE(wait_until(
        [&] {
            std::this_thread::sleep_for(c.config()._heartbeat_interval);
            c.node(2).check_heartbeat_timeout();
            return r.done.load();
        },
        5000ms));
    BOOST_TEST(!r.failed.load(), r.message());
    BOOST_TEST(r.holds("acknowledged-key"), "a read on the new leader lost an acknowledged write");
}
