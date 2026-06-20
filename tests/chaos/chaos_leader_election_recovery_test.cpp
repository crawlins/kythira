#define BOOST_TEST_MODULE ChaosLeaderElectionRecoveryTest
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
// Election timeout must be > retry_delay (100ms ± 10% jitter = max 110ms) so that
// execute_with_retry's retry-1 always fires before collect_majority's within() deadline.
// Using 150ms min gives a reliable 40ms margin over the worst-case retry delay.
constexpr std::chrono::milliseconds k_election_min{150};
constexpr std::chrono::milliseconds k_election_max{200};
}

BOOST_AUTO_TEST_SUITE(chaos_leader_election_recovery)

// Liveness: after a partition heals, a leader is elected within the timeout.
//
// Scenario: isolate the minority with network_partition_profile; wait
// 2× election_timeout_max; disable profile; verify a leader is elected within
// another 2× election_timeout_max.
BOOST_AUTO_TEST_CASE(leader_elected_after_partition_heals, *boost::unit_test::timeout(120)) {
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

    // Initial election: sleep past the max timeout so the check always triggers, then
    // wait k_election_max more to allow retry-1 (fires at ~100ms) to succeed before
    // the partition is applied.
    std::this_thread::sleep_for(k_election_max + std::chrono::milliseconds{20});
    n1->check_election_timeout();
    std::this_thread::sleep_for(k_election_max);

    // Apply partition to n3 (minority) for 2× timeout.
    {
        network_partition_profile partition;
        std::this_thread::sleep_for(k_election_max * 2);
    }
    // Partition healed.

    // Trigger election on one node only; triggering two simultaneously risks split vote.
    n2->check_election_timeout();

    // Poll up to 2 seconds for a leader to emerge.
    bool leader_found = false;
    for (int i = 0; i < 20 && !leader_found; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        leader_found = n1->is_leader() || n2->is_leader() || n3->is_leader();
    }
    BOOST_CHECK_MESSAGE(leader_found, "No leader elected after partition healed");

    std::vector<chaos_node*> nodes = {n1.get(), n2.get(), n3.get()};
    assert_election_safety(nodes);

    n1->stop();
    n2->stop();
    n3->stop();
    sim->stop();
    // Drain any Folly retry callbacks that may fire up to ~300ms after the last
    // check_election_timeout() call before the nodes are destroyed.
    std::this_thread::sleep_for(std::chrono::milliseconds{400});
}

BOOST_AUTO_TEST_SUITE_END()
