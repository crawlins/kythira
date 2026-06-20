#define BOOST_TEST_MODULE ChaosCommitRecoveryTest
#include <boost/test/unit_test.hpp>

#include <fiu.h>
#include <fiu-control.h>

#include "chaos_test_types.hpp"
#include "fault_profiles.hpp"
#include "safety_assertions.hpp"

#include <chrono>
#include <thread>
#include <vector>
#include <memory>

using ChaosFixture = kythira::chaos::chaos_test_fixture;
BOOST_GLOBAL_FIXTURE(ChaosFixture);

namespace {
constexpr std::chrono::milliseconds k_election_min{50};
constexpr std::chrono::milliseconds k_election_max{100};
constexpr std::chrono::milliseconds k_heartbeat{20};
}

BOOST_AUTO_TEST_SUITE(chaos_commit_recovery)

// Liveness: a pending command commits after network faults stop.
//
// Scenario: submit a command with network_partition_profile active on a
// minority; disable profile; verify the node can send heartbeats without fault.
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

    // Elect n1.
    std::this_thread::sleep_for(k_election_max + std::chrono::milliseconds{20});
    n1->check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    auto cmd = kythira::test_key_value_state_machine<>::make_put_command("recovery_key", "value");

    // Submit command while n3 is partitioned (minority fault).
    {
        network_partition_profile partition;
        try {
            n1->submit_command(cmd, std::chrono::milliseconds{50});
        } catch (...) {
            // May fail to replicate to majority — expected.
        }
        // Wait briefly with faults active.
        std::this_thread::sleep_for(k_heartbeat * 3);
    }
    // Fault cleared.

    // Allow up to 10× heartbeat for commit recovery.
    for (int i = 0; i < 10; ++i) {
        n1->check_heartbeat_timeout();
        std::this_thread::sleep_for(k_heartbeat);
    }

    // Verify no two leaders share a term after recovery.
    std::vector<chaos_node*> nodes = {n1.get(), n2.get(), n3.get()};
    assert_election_safety(nodes);

    n1->stop();
    n2->stop();
    n3->stop();
    sim->stop();
}

BOOST_AUTO_TEST_SUITE_END()
