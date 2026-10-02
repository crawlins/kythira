// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE ChaosCommitRecoveryTest
#include <boost/test/unit_test.hpp>

#include <fiu.h>
#include <fiu-control.h>

#include "chaos_test_types.hpp"
#include "fault_profiles.hpp"
#include "liveness_assertions.hpp"
#include "safety_assertions.hpp"

#include <chrono>
#include <format>
#include <thread>
#include <vector>
#include <memory>

using ChaosFixture = kythira::chaos::chaos_test_fixture;
BOOST_GLOBAL_FIXTURE(ChaosFixture);

namespace {
constexpr std::chrono::milliseconds k_election_min{50};
constexpr std::chrono::milliseconds k_election_max{100};
constexpr std::chrono::milliseconds k_heartbeat{20};
constexpr std::chrono::milliseconds k_step{5};
constexpr std::chrono::milliseconds k_election_budget{2000};
// Long enough that the command is still pending when the fault lifts.
constexpr std::chrono::milliseconds k_command_timeout{5000};
// Budget for the client future and follower apply after the commit itself
// has been checked against the requirement's deadline.
constexpr std::chrono::milliseconds k_settle_budget{2000};
}  // namespace

BOOST_AUTO_TEST_SUITE(chaos_commit_recovery)

// Liveness (Requirement 6.2): a pending command commits after network faults
// stop.
//
// Scenario: elect n1; submit a command with network_partition_profile active;
// confirm the fault held the command back; lift the profile; require the
// command to commit within 10x heartbeat_interval, then to complete
// successfully and be applied on every node.
//
// libfiu fault points are process-wide, so the profile cuts every node's
// sends at once rather than a minority's. For this test that only makes the
// fault stronger: the leader cannot reach anyone, so the command is certainly
// still pending when the fault lifts.
BOOST_AUTO_TEST_CASE(commit_succeeds_after_fault_removal, *boost::unit_test::timeout(120)) {
    using namespace kythira::chaos;
    kythira::chaos::clear_all_faults();

    auto sim = std::make_shared<
        network_simulator::NetworkSimulator<chaos_raft_types::raft_network_types>>();
    sim->start();

    kythira::raft_configuration cfg;
    cfg._election_timeout_min = k_election_min;
    cfg._election_timeout_max = k_election_max;
    cfg._heartbeat_interval = k_heartbeat;

    auto n1 = make_chaos_node(1, sim, cfg);
    auto n2 = make_chaos_node(2, sim, cfg);
    auto n3 = make_chaos_node(3, sim, cfg);
    wire_full_mesh(sim, {"1", "2", "3"});

    n1->set_cluster_configuration({1, 2, 3});
    n2->set_cluster_configuration({1, 2, 3});
    n3->set_cluster_configuration({1, 2, 3});

    n1->start();
    n2->start();
    n3->start();

    std::vector<chaos_node*> nodes = {n1.get(), n2.get(), n3.get()};

    // Elect n1.
    std::this_thread::sleep_for(k_election_max + std::chrono::milliseconds{20});
    require_elected(*n1, nodes, k_election_budget, k_step, "initial election");

    auto cmd = kythira::test_key_value_state_machine<>::make_put_command("recovery_key", "value");
    const auto index = last_log_index(*n1) + 1;

    // Submit while every send fails. The submit must still be accepted into
    // the leader's log; only replication is blocked.
    network_partition_profile partition;
    auto result = n1->submit_command(cmd, k_command_timeout);
    for (int i = 0; i < 3; ++i) {
        n1->check_heartbeat_timeout();
        std::this_thread::sleep_for(k_heartbeat);
    }
    BOOST_REQUIRE_MESSAGE(
        last_log_index(*n1) >= index,
        "submit_command did not append to the leader's log; " + describe_nodes(nodes));
    BOOST_REQUIRE_MESSAGE(n1->debug_state().commit_index < index,
                          "command committed while the partition was active, so the "
                          "scenario never exercised recovery; " +
                              describe_nodes(nodes));
    BOOST_REQUIRE_MESSAGE(!result.isReady(), "command completed while the partition was active; " +
                                                 describe_nodes(nodes));

    partition.disable();

    // Requirement 6.2: committed within 10x heartbeat_interval of the fault
    // stopping.
    const bool committed = drive_until(
        k_heartbeat * 10, k_step, [&] { n1->check_heartbeat_timeout(); },
        [&] { return n1->debug_state().commit_index >= index; });
    BOOST_REQUIRE_MESSAGE(
        committed, std::format("pending command at index {} did not commit within "
                               "10 x heartbeat_interval ({} ms) of the fault lifting; {}; {}",
                               index, (k_heartbeat * 10).count(), describe_nodes(nodes),
                               describe_fault_points()));

    // The client sees success, and every replica applies it.
    require_command_succeeds(*n1, result, k_settle_budget, k_step, nodes, "pending command");
    require_applied_through(*n1, nodes, index, k_settle_budget, k_step, "pending command");

    assert_election_safety(nodes);
    assert_log_matching(nodes);

    n1->stop();
    n2->stop();
    n3->stop();
    sim->stop();
}

BOOST_AUTO_TEST_SUITE_END()
