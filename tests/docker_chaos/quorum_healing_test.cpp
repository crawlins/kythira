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

using namespace std::chrono_literals;

// ── Guard: only run when KYTHIRA_DOCKER_INTEGRATION_TESTS=1 ──────────────────

namespace {

bool docker_integration_tests_enabled() {
    const char* v = std::getenv("KYTHIRA_DOCKER_INTEGRATION_TESTS");
    return (v != nullptr && std::string{v} == "1");
}

}  // namespace

// ── Req 19 AC 4 — follower kill self-heals to 3 nodes ────────────────────────

BOOST_AUTO_TEST_CASE(follower_kill_heals_to_target, *boost::unit_test::timeout(120)) {
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
    BOOST_CHECK_MESSAGE(f.wait_for_cluster_size(3, 60s),
                        "cluster did not self-heal to 3 nodes within 60 s");

    // The killed node's original container should have been decommissioned
    BOOST_CHECK_NO_THROW(f.assert_container_absent(follower_id));

    f.assert_no_split_brain();
}

// ── Req 19 AC 5 — follower stop self-heals ───────────────────────────────────

BOOST_AUTO_TEST_CASE(follower_stop_heals_to_target, *boost::unit_test::timeout(120)) {
    if (!docker_integration_tests_enabled()) {
        BOOST_TEST_MESSAGE("skipped: set KYTHIRA_DOCKER_INTEGRATION_TESTS=1 to enable");
        return;
    }

    docker_chaos::QuorumHealingFixture f{"follower-stop"};

    auto& leader = f.wait_for_leader(30s);
    int follower_id = (leader.id() == 1) ? 2 : 1;

    f.node(follower_id).stop();

    BOOST_CHECK_MESSAGE(f.wait_for_cluster_size(3, 60s),
                        "cluster did not self-heal to 3 nodes after follower stop");

    BOOST_CHECK_NO_THROW(f.assert_container_absent(follower_id));
    f.assert_no_split_brain();
}

// ── Req 19 AC 6 — killing the leader triggers re-election + healing ───────────

BOOST_AUTO_TEST_CASE(leader_kill_new_leader_heals, *boost::unit_test::timeout(120)) {
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
    BOOST_CHECK_MESSAGE(f.wait_for_cluster_size(3, 60s),
                        "cluster did not self-heal to 3 nodes after leader kill");

    BOOST_CHECK_NO_THROW(f.assert_container_absent(old_leader_id));
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

    // Count initial running containers
    std::size_t initial_count = 3;

    // Pause a follower for fewer ticks than the heartbeat failure threshold (3)
    // then immediately unpause.  The default heartbeat interval is 50ms so
    // 2 × 50ms < threshold × interval is easily achieved with 80ms.
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

BOOST_AUTO_TEST_CASE(sustained_pause_triggers_replacement, *boost::unit_test::timeout(120)) {
    if (!docker_integration_tests_enabled()) {
        BOOST_TEST_MESSAGE("skipped: set KYTHIRA_DOCKER_INTEGRATION_TESTS=1 to enable");
        return;
    }

    docker_chaos::QuorumHealingFixture f{"sustained-pause"};
    f.wait_for_leader(30s);

    f.pause(2);

    // Wait long enough for assessment + provisioning
    // quorum_check_interval (default 30s) + threshold × heartbeat (3 × 50ms) + margin
    std::this_thread::sleep_for(35s);

    BOOST_CHECK_MESSAGE(f.wait_for_cluster_size(3, 30s),
                        "cluster did not provision replacement after sustained pause");

    // Unpause the original — cluster should handle the returning node gracefully
    f.unpause(2);
    std::this_thread::sleep_for(5s);

    // Either 3 or 4 running containers is acceptable; quorum must not be lost
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
