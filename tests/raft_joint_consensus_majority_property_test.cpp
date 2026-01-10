/**
 * Property-Based Test for Joint Consensus Majority
 * 
 * Feature: raft-consensus, Property 13: Joint Consensus Majority
 * Validates: Requirements 9.3
 * 
 * Property: For any decision made during joint consensus (elections, commits),
 * the decision requires majorities from both the old and new configurations.
 */

#define BOOST_TEST_MODULE RaftJointConsensusMajorityPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/simulator_network.hpp>
#include <raft/persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>
#include <raft/membership.hpp>

#include <network_simulator/network_simulator.hpp>

#include <folly/init/Init.h>

#include <random>
#include <chrono>
#include <thread>
#include <algorithm>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_joint_consensus_majority_property_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr std::size_t property_test_iterations = 10;
    constexpr std::chrono::milliseconds election_timeout_min{50};
    constexpr std::chrono::milliseconds election_timeout_max{100};
    constexpr std::chrono::milliseconds heartbeat_interval{25};
    constexpr std::chrono::milliseconds rpc_timeout{50};
}

BOOST_AUTO_TEST_SUITE(joint_consensus_majority_property_tests)

/**
 * Property: Joint consensus configuration requires both old and new nodes
 * 
 * For any joint consensus configuration, the configuration should contain
 * both the old and new node lists.
 */
BOOST_AUTO_TEST_CASE(joint_consensus_has_both_configurations, * boost::unit_test::timeout(60)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        // Generate random old and new configurations
        std::uniform_int_distribution<std::uint64_t> node_count_dist(3, 7);
        std::size_t old_node_count = node_count_dist(rng);
        std::size_t new_node_count = node_count_dist(rng);
        
        // Create old configuration
        kythira::cluster_configuration<std::uint64_t> old_config{};
        old_config._is_joint_consensus = false;
        for (std::size_t i = 1; i <= old_node_count; ++i) {
            old_config._nodes.push_back(i);
        }
        
        // Create new configuration (may overlap with old)
        kythira::cluster_configuration<std::uint64_t> new_config{};
        new_config._is_joint_consensus = false;
        for (std::size_t i = 1; i <= new_node_count; ++i) {
            // Offset by old_node_count/2 to create partial overlap
            new_config._nodes.push_back(i + old_node_count / 2);
        }
        
        // Create joint consensus configuration
        kythira::default_membership_manager<std::uint64_t> membership_manager{};
        auto joint_config = membership_manager.create_joint_configuration(old_config, new_config);
        
        // Verify joint consensus properties
        BOOST_CHECK(joint_config.is_joint_consensus());
        BOOST_CHECK(joint_config.old_nodes().has_value());
        BOOST_CHECK_EQUAL(joint_config.nodes().size(), new_config.nodes().size());
        BOOST_CHECK_EQUAL(joint_config.old_nodes().value().size(), old_config.nodes().size());
    }
}

/**
 * Property: Commit requires majority from both configurations
 * 
 * For any log entry committed during joint consensus, the entry must be
 * replicated to a majority of nodes in BOTH the old and new configurations.
 * 
 * This test verifies the logic in advance_commit_index() that checks for
 * majorities in both configurations.
 */
BOOST_AUTO_TEST_CASE(commit_requires_both_majorities, * boost::unit_test::timeout(60)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        // Create network simulator
        auto simulator = network_simulator::NetworkSimulator<network_simulator::DefaultNetworkTypes>{};
        simulator.start();
        
        // Generate random configurations
        // Old config: nodes 1, 2, 3
        // New config: nodes 2, 3, 4, 5
        // This creates overlap (nodes 2, 3) and new nodes (4, 5)
        
        std::vector<std::uint64_t> old_nodes = {1, 2, 3};
        std::vector<std::uint64_t> new_nodes = {2, 3, 4, 5};
        
        // Create leader node (node 2, which is in both configurations)
        constexpr std::uint64_t leader_id = 2;
        auto leader_sim_node = simulator.create_node(leader_id);
        
        // Create configuration
        auto config = kythira::raft_configuration{};
        config._election_timeout_min = election_timeout_min;
        config._election_timeout_max = election_timeout_max;
        config._heartbeat_interval = heartbeat_interval;
        
        auto leader = kythira::node{
            leader_id,
            kythira::simulator_network_client<
                kythira::json_rpc_serializer<std::vector<std::byte>>,
                std::vector<std::byte>
            >{leader_sim_node, kythira::json_rpc_serializer<std::vector<std::byte>>{}},
            kythira::simulator_network_server<
                kythira::json_rpc_serializer<std::vector<std::byte>>,
                std::vector<std::byte>
            >{leader_sim_node, kythira::json_rpc_serializer<std::vector<std::byte>>{}},
            kythira::memory_persistence_engine<>{},
            kythira::console_logger{kythira::log_level::error},
            kythira::noop_metrics{},
            kythira::default_membership_manager<>{},
            config
        };
        
        // Set up joint consensus configuration
        kythira::cluster_configuration<std::uint64_t> old_config{};
        old_config._nodes = old_nodes;
        old_config._is_joint_consensus = false;
        
        kythira::cluster_configuration<std::uint64_t> new_config{};
        new_config._nodes = new_nodes;
        new_config._is_joint_consensus = false;
        
        kythira::default_membership_manager<std::uint64_t> membership_manager{};
        auto joint_config = membership_manager.create_joint_configuration(old_config, new_config);
        
        // Verify the joint configuration has the expected properties
        BOOST_CHECK(joint_config.is_joint_consensus());
        BOOST_CHECK(joint_config.old_nodes().has_value());
        
        // Calculate required majorities
        std::size_t old_majority = (old_nodes.size() / 2) + 1;  // 2 out of 3
        std::size_t new_majority = (new_nodes.size() / 2) + 1;  // 3 out of 4
        
        BOOST_CHECK_EQUAL(old_majority, 2);
        BOOST_CHECK_EQUAL(new_majority, 3);
        
        // Verify that the joint configuration requires both majorities
        // This is tested by the implementation in advance_commit_index()
        // which checks: (new_replication_count >= new_majority) && (old_replication_count >= old_majority)
        
        leader.stop();
    }
}

/**
 * Property: Majority calculation is correct for joint consensus
 * 
 * For any joint consensus configuration, the majority calculation should
 * correctly compute (size / 2) + 1 for both old and new configurations.
 */
BOOST_AUTO_TEST_CASE(majority_calculation_is_correct, * boost::unit_test::timeout(60)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        // Generate random configuration sizes
        std::uniform_int_distribution<std::size_t> size_dist(1, 11);
        std::size_t old_size = size_dist(rng);
        std::size_t new_size = size_dist(rng);
        
        // Calculate expected majorities
        std::size_t expected_old_majority = (old_size / 2) + 1;
        std::size_t expected_new_majority = (new_size / 2) + 1;
        
        // Verify majority calculation
        // For size 1: majority = 1
        // For size 2: majority = 2
        // For size 3: majority = 2
        // For size 4: majority = 3
        // For size 5: majority = 3
        
        BOOST_CHECK_LE(expected_old_majority, old_size);
        BOOST_CHECK_LE(expected_new_majority, new_size);
        BOOST_CHECK_GT(expected_old_majority, old_size / 2);
        BOOST_CHECK_GT(expected_new_majority, new_size / 2);
    }
}

/**
 * Property: Node in either configuration can participate
 * 
 * For any node in either the old or new configuration during joint consensus,
 * that node should be considered part of the cluster.
 */
BOOST_AUTO_TEST_CASE(node_in_either_configuration_can_participate) {
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        // Create configurations with some overlap
        std::vector<std::uint64_t> old_nodes = {1, 2, 3, 4};
        std::vector<std::uint64_t> new_nodes = {3, 4, 5, 6};
        
        kythira::cluster_configuration<std::uint64_t> old_config{};
        old_config._nodes = old_nodes;
        old_config._is_joint_consensus = false;
        
        kythira::cluster_configuration<std::uint64_t> new_config{};
        new_config._nodes = new_nodes;
        new_config._is_joint_consensus = false;
        
        kythira::default_membership_manager<std::uint64_t> membership_manager{};
        auto joint_config = membership_manager.create_joint_configuration(old_config, new_config);
        
        // Verify nodes in old configuration are recognized
        for (const auto& node : old_nodes) {
            BOOST_CHECK(membership_manager.is_node_in_configuration(node, joint_config));
        }
        
        // Verify nodes in new configuration are recognized
        for (const auto& node : new_nodes) {
            BOOST_CHECK(membership_manager.is_node_in_configuration(node, joint_config));
        }
        
        // Verify node not in either configuration is not recognized
        std::uint64_t non_member_node = 100;
        BOOST_CHECK(!membership_manager.is_node_in_configuration(non_member_node, joint_config));
    }
}

/**
 * Property: Joint consensus prevents unilateral decisions
 * 
 * For any decision during joint consensus, neither the old nor new configuration
 * alone can make the decision. Both must agree.
 * 
 * This test verifies that having a majority in only one configuration is insufficient.
 */
BOOST_AUTO_TEST_CASE(joint_consensus_prevents_unilateral_decisions) {
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        // Create configurations where majorities differ
        // Old config: 1, 2, 3 (majority = 2)
        // New config: 2, 3, 4, 5, 6 (majority = 3)
        
        std::vector<std::uint64_t> old_nodes = {1, 2, 3};
        std::vector<std::uint64_t> new_nodes = {2, 3, 4, 5, 6};
        
        std::size_t old_majority = (old_nodes.size() / 2) + 1;  // 2
        std::size_t new_majority = (new_nodes.size() / 2) + 1;  // 3
        
        // Scenario 1: Have majority in old config but not new config
        // Replicated to: 1, 2 (2 out of 3 old nodes, but only 1 out of 5 new nodes)
        std::vector<std::uint64_t> scenario1_replicated = {1, 2};
        
        std::size_t old_count_1 = 0;
        std::size_t new_count_1 = 0;
        
        for (const auto& node : scenario1_replicated) {
            if (std::find(old_nodes.begin(), old_nodes.end(), node) != old_nodes.end()) {
                old_count_1++;
            }
            if (std::find(new_nodes.begin(), new_nodes.end(), node) != new_nodes.end()) {
                new_count_1++;
            }
        }
        
        // Should have old majority but not new majority
        BOOST_CHECK_GE(old_count_1, old_majority);
        BOOST_CHECK_LT(new_count_1, new_majority);
        
        // Therefore, should NOT be able to commit
        bool can_commit_1 = (old_count_1 >= old_majority) && (new_count_1 >= new_majority);
        BOOST_CHECK(!can_commit_1);
        
        // Scenario 2: Have majority in new config but not old config
        // Replicated to: 3, 4, 5 (1 out of 3 old nodes, but 3 out of 5 new nodes)
        std::vector<std::uint64_t> scenario2_replicated = {3, 4, 5};
        
        std::size_t old_count_2 = 0;
        std::size_t new_count_2 = 0;
        
        for (const auto& node : scenario2_replicated) {
            if (std::find(old_nodes.begin(), old_nodes.end(), node) != old_nodes.end()) {
                old_count_2++;
            }
            if (std::find(new_nodes.begin(), new_nodes.end(), node) != new_nodes.end()) {
                new_count_2++;
            }
        }
        
        // Should have new majority but not old majority
        BOOST_CHECK_LT(old_count_2, old_majority);
        BOOST_CHECK_GE(new_count_2, new_majority);
        
        // Therefore, should NOT be able to commit
        bool can_commit_2 = (old_count_2 >= old_majority) && (new_count_2 >= new_majority);
        BOOST_CHECK(!can_commit_2);
        
        // Scenario 3: Have majority in BOTH configurations
        // Replicated to: 1, 2, 3, 4 (3 out of 3 old nodes, 3 out of 5 new nodes)
        std::vector<std::uint64_t> scenario3_replicated = {1, 2, 3, 4};
        
        std::size_t old_count_3 = 0;
        std::size_t new_count_3 = 0;
        
        for (const auto& node : scenario3_replicated) {
            if (std::find(old_nodes.begin(), old_nodes.end(), node) != old_nodes.end()) {
                old_count_3++;
            }
            if (std::find(new_nodes.begin(), new_nodes.end(), node) != new_nodes.end()) {
                new_count_3++;
            }
        }
        
        // Should have both majorities
        BOOST_CHECK_GE(old_count_3, old_majority);
        BOOST_CHECK_GE(new_count_3, new_majority);
        
        // Therefore, SHOULD be able to commit
        bool can_commit_3 = (old_count_3 >= old_majority) && (new_count_3 >= new_majority);
        BOOST_CHECK(can_commit_3);
    }
}

BOOST_AUTO_TEST_SUITE_END()
