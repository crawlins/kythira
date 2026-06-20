#define BOOST_TEST_MODULE ChaosStateMachineSafetyTest
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
}

BOOST_AUTO_TEST_SUITE(chaos_state_machine_safety)

// Property: two nodes that have applied to the same index have applied the same commands.
//
// Scenario: 3-node cluster with disk_degradation_profile on two nodes
// (20% AppendEntries failure rate); let commands be committed; call
// assert_state_machine_safety across all nodes that have applied at least one entry.
BOOST_AUTO_TEST_CASE(state_machines_apply_same_commands, *boost::unit_test::timeout(120)) {
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

    // Elect n1.
    std::this_thread::sleep_for(k_election_max + std::chrono::milliseconds{20});
    n1->check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Submit commands with degradation active on n2/n3 (20% failure on persistence).
    {
        disk_degradation_profile profile{0.20};

        constexpr int k_commands = 30;
        for (int i = 0; i < k_commands; ++i) {
            auto key = std::string("key") + std::to_string(i);
            auto cmd = kythira::test_key_value_state_machine<>::make_put_command(key, "value");
            try {
                n1->submit_command(cmd, std::chrono::milliseconds{20});
            } catch (...) {
                // expected under fault
            }
            n1->check_heartbeat_timeout();
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }

    // Stabilise.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    n1->check_heartbeat_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    // Verify state machine safety across all nodes.
    std::vector<chaos_node*> nodes = {n1.get(), n2.get(), n3.get()};
    assert_log_matching(nodes);

    auto snap1 = n1->debug_state();
    assert_state_machine_safety(nodes, snap1.last_applied);

    n1->stop();
    n2->stop();
    n3->stop();
    sim->stop();
}

BOOST_AUTO_TEST_SUITE_END()
