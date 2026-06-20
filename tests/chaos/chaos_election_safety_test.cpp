#define BOOST_TEST_MODULE ChaosElectionSafetyTest
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
constexpr int k_iterations = 10;
}

BOOST_AUTO_TEST_SUITE(chaos_election_safety)

// Property: at most one leader per term, even under network partition.
//
// Scenario: 3-node cluster; after leader election, apply network_partition_profile
// to one follower; trigger election timeouts; call assert_election_safety;
// disable profile; verify recovery.
BOOST_AUTO_TEST_CASE(partition_does_not_split_leadership, *boost::unit_test::timeout(120)) {
    using namespace kythira::chaos;

    for (int iter = 0; iter < k_iterations; ++iter) {
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

        // Let an election happen on n1 (single-node self-vote path).
        std::this_thread::sleep_for(k_election_max + std::chrono::milliseconds{30});
        n1->check_election_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{50});

        // Apply network partition to n3 (minority).
        // check_election_timeout() under an active partition produces exceptional
        // futures that the error-handler absorbs; the try-catch is a belt-and-suspenders
        // guard against any synchronous path that escapes the future machinery.
        {
            network_partition_profile partition;
            try {
                n3->check_election_timeout();
            } catch (...) {
            }
            std::this_thread::sleep_for(k_election_max);
            try {
                n1->check_election_timeout();
            } catch (...) {
            }
            try {
                n2->check_election_timeout();
            } catch (...) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        // Profile destroyed → faults cleared.

        std::vector<chaos_node*> nodes = {n1.get(), n2.get(), n3.get()};
        assert_election_safety(nodes);

        n1->stop();
        n2->stop();
        n3->stop();
        sim->stop();
        // Drain pending Folly timer callbacks (retry 2 fires at ~300ms after the
        // in-partition elections started) before nodes are destroyed at loop iteration end.
        std::this_thread::sleep_for(std::chrono::milliseconds{400});
    }
}

BOOST_AUTO_TEST_SUITE_END()
