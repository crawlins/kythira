// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE ChaosLeaderCompletenessTest
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
constexpr std::chrono::milliseconds k_step{5};
constexpr std::chrono::milliseconds k_election_budget{2000};
constexpr std::chrono::milliseconds k_command_timeout{5000};
constexpr std::chrono::milliseconds k_settle_budget{2000};
}  // namespace

BOOST_AUTO_TEST_SUITE(chaos_leader_completeness)

// Property (Requirement 5.3): a committed entry must appear in all future
// leaders' logs.
//
// Scenario: commit one command and confirm every node applied it; isolate the
// leader n1; elect n2 in a higher term; require n2's log to hold the entry at
// the same index, term and bytes; heal; require n1 to step down and the
// cluster to commit a new command on n2 that every node applies.
//
// The isolation removes n1's simulator edges instead of using
// network_partition_profile. libfiu fault points are process-wide, so that
// profile fails every node's sends: n2 could never collect a vote, no new
// leader was ever elected, and the old version of this test passed without
// testing anything.
BOOST_AUTO_TEST_CASE(committed_entry_survives_leader_change, *boost::unit_test::timeout(120)) {
    using namespace kythira::chaos;
    kythira::chaos::clear_all_faults();

    auto sim = std::make_shared<
        network_simulator::NetworkSimulator<chaos_raft_types::raft_network_types>>();
    sim->start();

    kythira::raft_configuration cfg;
    cfg._election_timeout_min = k_election_min;
    cfg._election_timeout_max = k_election_max;
    cfg._heartbeat_interval = std::chrono::milliseconds{20};

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

    // Commit one command on n1, and confirm it really committed: the old
    // test caught and ignored any failure here and asserted nothing after.
    auto cmd = kythira::test_key_value_state_machine<>::make_put_command("committed_key", "val");
    const auto index = last_log_index(*n1) + 1;
    auto result = n1->submit_command(cmd, k_command_timeout);
    require_command_succeeds(*n1, result, k_settle_budget, k_step, nodes, "committed entry");
    require_applied_through(*n1, nodes, index, k_settle_budget, k_step, "committed entry");
    const auto committed = entry_at(*n1, index);
    BOOST_REQUIRE_MESSAGE(committed.has_value() && committed->command() == cmd,
                          std::format("n1 has no entry at index {} carrying the submitted "
                                      "command; {}",
                                      index, describe_nodes(nodes)));
    const auto old_term = n1->get_current_term();

    // Isolate n1 and elect n2 from the remaining majority.
    for (const auto* peer : {"2", "3"}) {
        sim->remove_edge("1", peer);
        sim->remove_edge(peer, "1");
    }
    std::this_thread::sleep_for(k_election_max * 2);
    require_elected(*n2, nodes, k_election_budget, k_step, "election with n1 isolated");
    BOOST_REQUIRE_GT(n2->get_current_term(), old_term);

    // The new leader must hold the committed entry, unchanged.
    const auto held = entry_at(*n2, index);
    BOOST_REQUIRE_MESSAGE(
        held.has_value(),
        std::format("leader completeness violated: new leader n2 (term {}) "
                    "has no entry at committed index {}; {}; {}",
                    n2->get_current_term(), index, describe_nodes(nodes), describe_fault_points()));
    BOOST_REQUIRE_MESSAGE(
        held->term() == committed->term() && held->command() == cmd,
        std::format("leader completeness violated: new leader n2 (term {}) "
                    "holds index {} with term {}, committed term was {}; {}; {}",
                    n2->get_current_term(), index, held->term(), committed->term(),
                    describe_nodes(nodes), describe_fault_points()));

    // Heal. n1 learns of the higher term from n2's heartbeats and steps down,
    // and a write on n2 commits and reaches every node, n1 included.
    wire_full_mesh(sim, {"1", "2", "3"});
    const bool stepped_down = drive_until(
        k_settle_budget, k_step, [&] { n2->check_heartbeat_timeout(); },
        [&] { return !n1->is_leader(); });
    BOOST_REQUIRE_MESSAGE(stepped_down,
                          "isolated old leader n1 did not step down after the "
                          "partition healed; " +
                              describe_nodes(nodes));

    auto next = kythira::test_key_value_state_machine<>::make_put_command("after_heal", "val");
    const auto next_index = last_log_index(*n2) + 1;
    auto next_result = n2->submit_command(next, k_command_timeout);
    require_command_succeeds(*n2, next_result, k_settle_budget, k_step, nodes, "write after heal");
    require_applied_through(*n2, nodes, next_index, k_settle_budget, k_step, "write after heal");

    assert_log_matching(nodes);
    assert_election_safety(nodes);

    n1->stop();
    n2->stop();
    n3->stop();
    sim->stop();
}

BOOST_AUTO_TEST_SUITE_END()
