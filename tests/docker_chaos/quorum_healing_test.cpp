// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Docker quorum healing tests (Req 19)
//
// These tests exercise the full self-healing loop against real Docker
// infrastructure: a kythira-quorum-test cluster is brought up using
// docker/docker-compose.quorum.yml (with QUORUM_MANAGER=docker inside each
// container), then individual containers are killed/stopped/paused to
// simulate node failures, and the test waits for the leader to provision
// replacements.
//
// Guarded by the env var KYTHIRA_DOCKER_INTEGRATION_TESTS=1.

#define BOOST_TEST_MODULE quorum_healing_test
#include <boost/test/unit_test.hpp>

#include "quorum_harness.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ── Guard: only run when KYTHIRA_DOCKER_INTEGRATION_TESTS=1 ──────────────────

namespace {

bool docker_integration_tests_enabled() {
    const char* v = std::getenv("KYTHIRA_DOCKER_INTEGRATION_TESTS");
    return (v != nullptr && std::string{v} == "1");
}

}  // namespace

// A failed healing check is decided inside the nodes and the container API
// proxy, so on failure dump their state and logs; a bare timeout says nothing.
#define CHECK_OR_DUMP(fixture, cond, msg) \
    do {                                  \
        const bool ok_ = (cond);          \
        BOOST_CHECK_MESSAGE(ok_, msg);    \
        if (!ok_) {                       \
            (fixture).dump_diagnostics(); \
        }                                 \
    } while (0)

// ── Req 19 AC 4 — follower kill self-heals to 3 nodes ────────────────────────

BOOST_AUTO_TEST_CASE(follower_kill_heals_to_target, *boost::unit_test::timeout(240)) {
    if (!docker_integration_tests_enabled()) {
        BOOST_TEST_MESSAGE("skipped: set KYTHIRA_DOCKER_INTEGRATION_TESTS=1 to enable");
        return;
    }

    docker_chaos::QuorumHealingFixture f{"follower-kill"};

    auto& leader = f.wait_for_leader(30s);
    int leader_id = leader.id();

    // Kill a follower (the node with id != leader_id)
    int follower_id = (leader_id == 1) ? 2 : 1;
    f.node(follower_id).kill();

    // Wait for the cluster to self-heal to 3 running containers
    CHECK_OR_DUMP(f, f.wait_for_cluster_size(3, 60s),
                  "cluster did not self-heal to 3 nodes within 60 s");

    // The killed node's original container should have been decommissioned
    CHECK_OR_DUMP(f, f.wait_for_container_absent(follower_id, 60s),
                  "the failed follower was not decommissioned within 60 s");

    f.assert_no_split_brain();
}

// ── Req 19 AC 5 — follower stop self-heals ───────────────────────────────────

BOOST_AUTO_TEST_CASE(follower_stop_heals_to_target, *boost::unit_test::timeout(240)) {
    if (!docker_integration_tests_enabled()) {
        BOOST_TEST_MESSAGE("skipped: set KYTHIRA_DOCKER_INTEGRATION_TESTS=1 to enable");
        return;
    }

    docker_chaos::QuorumHealingFixture f{"follower-stop"};

    auto& leader = f.wait_for_leader(30s);
    int follower_id = (leader.id() == 1) ? 2 : 1;

    f.node(follower_id).stop();

    CHECK_OR_DUMP(f, f.wait_for_cluster_size(3, 60s),
                  "cluster did not self-heal to 3 nodes after follower stop");

    CHECK_OR_DUMP(f, f.wait_for_container_absent(follower_id, 60s),
                  "the failed follower was not decommissioned within 60 s");
    f.assert_no_split_brain();
}

// ── Req 19 AC 6 — killing the leader triggers re-election + healing ───────────

BOOST_AUTO_TEST_CASE(leader_kill_new_leader_heals, *boost::unit_test::timeout(240)) {
    if (!docker_integration_tests_enabled()) {
        BOOST_TEST_MESSAGE("skipped: set KYTHIRA_DOCKER_INTEGRATION_TESTS=1 to enable");
        return;
    }

    docker_chaos::QuorumHealingFixture f{"leader-kill"};

    auto& old_leader = f.wait_for_leader(30s);
    int old_leader_id = old_leader.id();

    old_leader.kill();

    // A new leader should be elected among the surviving two nodes
    auto& new_leader = f.wait_for_leader(20s);
    BOOST_CHECK_NE(new_leader.id(), old_leader_id);

    // The new leader should provision a replacement
    CHECK_OR_DUMP(f, f.wait_for_cluster_size(3, 60s),
                  "cluster did not self-heal to 3 nodes after leader kill");

    CHECK_OR_DUMP(f, f.wait_for_container_absent(old_leader_id, 60s),
                  "the failed old leader was not decommissioned within 60 s");
    f.assert_no_split_brain();
}

// ── Req 19 AC 7 — brief pause below failure threshold: no replacement ─────────

BOOST_AUTO_TEST_CASE(transient_pause_below_threshold_no_replacement,
                     *boost::unit_test::timeout(60)) {
    if (!docker_integration_tests_enabled()) {
        BOOST_TEST_MESSAGE("skipped: set KYTHIRA_DOCKER_INTEGRATION_TESTS=1 to enable");
        return;
    }

    docker_chaos::QuorumHealingFixture f{"transient-pause"};
    f.wait_for_leader(30s);

    // A new leader assesses immediately, and every assessment reads each
    // container's state from the daemon, where a paused container is not
    // running. Pausing during one would rightly count node 2 as failed, so
    // pause between the first assessment and the next (5 s later).
    std::this_thread::sleep_for(2s);

    // Count initial running containers
    std::size_t initial_count = 3;

    // Pause a follower for fewer ticks than the heartbeat failure threshold (3)
    // then immediately unpause.  docker-compose.quorum.yml's heartbeat
    // interval is 100 ms, so an 80 ms pause stays well below
    // threshold × interval.
    f.pause(2);
    std::this_thread::sleep_for(80ms);
    f.unpause(2);

    // After a short reconnect window there should still be exactly 3 containers
    // with the SAME IDs — no new container provisioned.
    std::this_thread::sleep_for(2s);
    BOOST_CHECK(f.wait_for_cluster_size(initial_count, 10s));

    // No decommission should have happened for node 2
    BOOST_CHECK_THROW(f.assert_container_absent(2), std::runtime_error);

    f.assert_no_split_brain();
}

// ── Req 19 AC 8 — sustained pause triggers replacement ───────────────────────

BOOST_AUTO_TEST_CASE(sustained_pause_triggers_replacement, *boost::unit_test::timeout(240)) {
    if (!docker_integration_tests_enabled()) {
        BOOST_TEST_MESSAGE("skipped: set KYTHIRA_DOCKER_INTEGRATION_TESTS=1 to enable");
        return;
    }

    docker_chaos::QuorumHealingFixture f{"sustained-pause"};
    f.wait_for_leader(30s);

    f.pause(2);

    // A paused container is not running, so the leader's next assessment
    // counts node 2 unreachable and provisions a replacement. Once that
    // replacement is promoted, node 2 is removed from the configuration and
    // its container decommissioned, which also removes the paused container:
    // there is nothing left to unpause.
    CHECK_OR_DUMP(f, f.wait_for_container_absent(2, 90s),
                  "the paused node was not replaced and decommissioned within 90 s");
    CHECK_OR_DUMP(f, f.wait_for_cluster_size(3, 30s),
                  "cluster is not back to 3 running nodes after the sustained pause");

    f.assert_no_split_brain();
}

// ── Req 19 AC 9 — two followers of a 5-node cluster fail at once ─────────────

BOOST_AUTO_TEST_CASE(dual_follower_kill_5_node_cluster, *boost::unit_test::timeout(300)) {
    if (!docker_integration_tests_enabled()) {
        BOOST_TEST_MESSAGE("skipped: set KYTHIRA_DOCKER_INTEGRATION_TESTS=1 to enable");
        return;
    }

    docker_chaos::QuorumHealingFixture f{"dual-kill", 5};

    auto& leader = f.wait_for_leader(30s);
    std::vector<int> victims;
    for (int id = 1; id <= 5 && victims.size() < 2; ++id) {
        if (id != leader.id()) {
            victims.push_back(id);
        }
    }

    // Both at once, so no assessment sees only one of them gone.
    std::thread first{[&] { f.node(victims[0]).kill(); }};
    f.node(victims[1]).kill();
    first.join();

    // Three of five voters remain, so the cluster must keep committing for
    // the whole healing interval, through both replacements' joins and
    // promotions and both failed nodes' removals.
    int submitted = 0;
    int committed = 0;
    auto healed = [&] {
        return f.running_container_count() == 5 && f.wait_for_container_absent(victims[0], 0s) &&
               f.wait_for_container_absent(victims[1], 0s);
    };
    auto deadline = std::chrono::steady_clock::now() + 65s;
    while (std::chrono::steady_clock::now() < deadline && !healed()) {
        ++submitted;
        try {
            auto resp =
                f.wait_for_leader(10s).submit_command("dualkill" + std::to_string(submitted), "v");
            if (resp.contains("success") && resp["success"].as_bool()) {
                ++committed;
            }
        } catch (const std::exception& ex) {
            BOOST_TEST_MESSAGE("command " << submitted << " failed: " << ex.what());
        }
        std::this_thread::sleep_for(1s);
    }

    BOOST_TEST_MESSAGE(committed << " of " << submitted << " commands committed while healing");
    BOOST_CHECK_MESSAGE(f.wait_for_cluster_size(5, 5s),
                        "cluster did not self-heal to 5 nodes after the dual kill");
    BOOST_CHECK_MESSAGE(f.wait_for_container_absent(victims[0], 5s),
                        "first killed follower was not decommissioned");
    BOOST_CHECK_MESSAGE(f.wait_for_container_absent(victims[1], 5s),
                        "second killed follower was not decommissioned");
    BOOST_CHECK_GT(submitted, 0);
    BOOST_CHECK_MESSAGE(committed == submitted, "only " << committed << " of " << submitted
                                                        << " commands committed while healing");

    f.assert_no_split_brain();
}

// ── Req 19 AC 10 — quorum loss: no autonomous provisioning ───────────────────

BOOST_AUTO_TEST_CASE(quorum_loss_no_autonomous_provisioning, *boost::unit_test::timeout(120)) {
    if (!docker_integration_tests_enabled()) {
        BOOST_TEST_MESSAGE("skipped: set KYTHIRA_DOCKER_INTEGRATION_TESTS=1 to enable");
        return;
    }

    docker_chaos::QuorumHealingFixture f{"quorum-loss"};
    f.wait_for_leader(30s);

    // Kill two nodes to cause quorum loss (majority of 3 gone)
    f.node(1).kill();
    f.node(2).kill();

    // Wait past quorum_check_interval + margin
    std::this_thread::sleep_for(45s);

    // Assert cluster did NOT self-expand (no autonomous provisioning under quorum loss)
    // Only the sole survivor should be running — the count must NOT reach 3.
    bool healed = f.wait_for_cluster_size(3, 5s);
    BOOST_CHECK_MESSAGE(
        !healed,
        "leader autonomously provisioned replacements after quorum loss — should not happen");
}
