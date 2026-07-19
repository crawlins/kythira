#define BOOST_TEST_MODULE crash_recovery_test
#include <boost/test/unit_test.hpp>

#include "harness.hpp"

#include <thread>

using namespace std::chrono_literals;
using namespace docker_chaos;

static constexpr auto k_election_max = 300ms;
static constexpr auto k_heartbeat = 50ms;

BOOST_AUTO_TEST_SUITE(docker_chaos_crash_recovery)

// Req 8.2: kill follower, submit commands, restart, verify catch-up.
BOOST_FIXTURE_TEST_CASE(follower_crash_and_catch_up, ChaosFixture,
                        *boost::unit_test::timeout(120)) {
    auto& leader = cluster.wait_for_leader(10s);

    // Pick a non-leader to kill.
    ChaosNode* victim = nullptr;
    for (auto* n : cluster.all_nodes()) {
        if (n->id() != leader.id()) {
            victim = n;
            break;
        }
    }
    BOOST_REQUIRE(victim != nullptr);

    victim->kill();
    std::this_thread::sleep_for(200ms);

    // Submit 5 commands to the surviving majority.
    for (int i = 0; i < 5; ++i) {
        leader.submit_command("key" + std::to_string(i), "val" + std::to_string(i));
    }
    auto cluster_commit = leader.status()["commit_index"].as_int64();

    // Restart and wait for the victim to catch up.
    victim->restart(/*wait=*/true, 20s);

    auto deadline = std::chrono::steady_clock::now() + k_heartbeat * 5 + 5s;
    bool caught_up = false;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            if (victim->status()["commit_index"].as_int64() >= cluster_commit) {
                caught_up = true;
                break;
            }
        } catch (...) {
        }
        std::this_thread::sleep_for(500ms);
    }
    BOOST_TEST(caught_up, "restarted node did not reach cluster commit_index");
    cluster.assert_no_split_brain();
}

// Req 8.3: kill the current leader, verify new leader elected, no split brain.
BOOST_FIXTURE_TEST_CASE(leader_crash_and_reelection, ChaosFixture,
                        *boost::unit_test::timeout(120)) {
    auto& old_leader = cluster.wait_for_leader(10s);
    int old_id = old_leader.id();
    auto old_term = old_leader.status()["term"].as_int64();
    BOOST_TEST_MESSAGE("old_leader=" + std::to_string(old_id) +
                       " term=" + std::to_string(old_term));

    old_leader.kill();
    std::this_thread::sleep_for(200ms);

    auto& new_leader = cluster.wait_for_leader(k_election_max * 6);
    BOOST_TEST_MESSAGE("wait_for_leader() returned node " + std::to_string(new_leader.id()));
    BOOST_TEST(new_leader.id() != old_id, "killed leader cannot be the new leader");

    // Diagnostics: poll new_leader.status() up to 10 times (200ms apart)
    // rather than a single, immediately-fatal call, and log every node's
    // reachability/role at each attempt — this test has failed twice on a
    // real arm64 runner with "GET /status returned HTTP 0" moments after
    // wait_for_leader() confirmed the same node was reachable and leading,
    // and the raw job log alone hasn't been enough to tell whether that
    // node stays unreachable, recovers, or something else is going on.
    std::optional<std::int64_t> new_term;
    for (int attempt = 0; attempt < 10; ++attempt) {
        for (auto* n : cluster.all_nodes()) {
            std::string role = "unreachable";
            try {
                role = n->status()["role"].as_string();
            } catch (const std::exception& e) {
                role = std::string("unreachable (") + e.what() + ")";
            }
            BOOST_TEST_MESSAGE("  attempt " + std::to_string(attempt) + " node " +
                               std::to_string(n->id()) + ": " + role);
        }
        try {
            new_term = new_leader.status()["term"].as_int64();
            break;
        } catch (const std::exception& e) {
            BOOST_TEST_MESSAGE("attempt " + std::to_string(attempt) +
                               " new_leader.status() failed: " + e.what());
        }
        std::this_thread::sleep_for(200ms);
    }
    BOOST_REQUIRE_MESSAGE(new_term.has_value(),
                          "new_leader.status() never succeeded after 10 attempts");
    BOOST_TEST(*new_term > old_term, "new leader must have a higher term");
    cluster.assert_no_split_brain();
}

BOOST_AUTO_TEST_SUITE_END()
