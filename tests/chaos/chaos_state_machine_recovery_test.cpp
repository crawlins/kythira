// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE ChaosStateMachineRecoveryTest
#include <boost/test/unit_test.hpp>

#include <fiu.h>
#include <fiu-control.h>

#include "chaos_test_types.hpp"
#include "fault_profiles.hpp"
#include "liveness_assertions.hpp"
#include "safety_assertions.hpp"

#include <chrono>
#include <format>
#include <string>
#include <tuple>
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
constexpr std::chrono::milliseconds k_command_timeout{5000};
constexpr std::chrono::milliseconds k_settle_budget{2000};
}  // namespace

BOOST_AUTO_TEST_SUITE(chaos_state_machine_recovery)

// Liveness: a node with state machine faults resumes applying entries once the
// fault is removed.
//
// Scenario: apply state_machine_fault_profile (100%); commit 5 commands, none
// of which any node can apply; disable the profile; require every node to
// resume from where it stopped and reach the commit index without any new
// write, then require a new write to commit and apply everywhere.
//
// libfiu fault points are process-wide, so the profile faults every node's
// state machine, not one follower's.
BOOST_AUTO_TEST_CASE(node_resumes_apply_after_state_machine_fault_removed,
                     *boost::unit_test::timeout(120)) {
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

    // Let every node apply the leader's opening entries, so all of them stop
    // at the same index when the fault starts.
    const auto stopped_at = last_log_index(*n1);
    require_applied_through(*n1, nodes, stopped_at, k_settle_budget, k_step,
                            "entries before the fault");

    constexpr int k_commands = 5;
    auto key_for = [](int i) { return std::string("sm_key") + std::to_string(i); };

    // Fail every apply (100%). The submits' futures are deliberately dropped:
    // under the default halt policy the first failed entry's future carries
    // the apply error and the rest time out.
    state_machine_fault_profile profile{1.0};
    for (int i = 0; i < k_commands; ++i) {
        auto cmd = kythira::test_key_value_state_machine<>::make_put_command(key_for(i), "val");
        std::ignore = n1->submit_command(cmd, std::chrono::milliseconds{50});
        n1->check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    const auto last_index = last_log_index(*n1);

    // The entries still commit everywhere; only application is blocked. If
    // either half of that fails the scenario is not testing recovery.
    const bool committed = drive_until(
        k_settle_budget, k_step, [&] { n1->check_heartbeat_timeout(); },
        [&] {
            for (auto* n : nodes) {
                if (n->debug_state().commit_index < last_index) {
                    return false;
                }
            }
            return true;
        });
    BOOST_REQUIRE_MESSAGE(committed,
                          std::format("the {} commands did not commit while only the "
                                      "state machine was faulted; {}; {}",
                                      k_commands, describe_nodes(nodes), describe_fault_points()));
    for (auto* n : nodes) {
        BOOST_REQUIRE_MESSAGE(n->debug_state().last_applied == stopped_at,
                              std::format("node id {} applied past index {} with every apply "
                                          "faulted; {}",
                                          n->get_node_id(), stopped_at, describe_nodes(nodes)));
    }

    profile.disable();

    // Requirement 6.4 / task 18: with the fault lifted and no new writes,
    // every node resumes from the entry it stopped at and reaches the commit
    // index. The commit index never moves here, so this holds only because a
    // lagging applied index is retried on the heartbeat (raft-consensus
    // Requirement 19.5), not just when the commit index next advances.
    require_applied_through(*n1, nodes, last_index, k_settle_budget, k_step,
                            "every node after state machine fault lifted");

    // A new write still commits and applies everywhere, and no node skipped
    // any of the entries whose first apply failed.
    const auto index = last_index + 1;
    auto cmd = kythira::test_key_value_state_machine<>::make_put_command("after_fault", "val");
    auto result = n1->submit_command(cmd, k_command_timeout);
    require_command_succeeds(*n1, result, k_settle_budget, k_step, nodes,
                             "command after state machine fault lifted");
    require_applied_through(*n1, nodes, index, k_settle_budget, k_step,
                            "command after state machine fault lifted");
    for (auto* n : nodes) {
        for (int i = 0; i < k_commands; ++i) {
            BOOST_CHECK_MESSAGE(
                n->with_state_machine([&](auto& sm) { return sm.contains(key_for(i)); }),
                std::format("node {} skipped {} (applied through {}); {}", n->get_node_id(),
                            key_for(i), index, describe_nodes(nodes)));
        }
    }

    assert_election_safety(nodes);
    assert_log_matching(nodes);

    n1->stop();
    n2->stop();
    n3->stop();
    sim->stop();
}

BOOST_AUTO_TEST_SUITE_END()
