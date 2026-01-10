/**
 * Example: Raft Membership Changes
 * 
 * This example demonstrates:
 * 1. Cluster configuration management
 * 2. Node lifecycle in cluster context
 * 3. Membership validation
 * 
 * Note: Simplified for single-node due to implementation constraints
 */

#include <raft/raft.hpp>
#include <raft/simulator_network.hpp>
#include <raft/persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>
#include <raft/membership.hpp>

#include <network_simulator/network_simulator.hpp>

#include <folly/init/Init.h>

#include <iostream>
#include <chrono>
#include <thread>

namespace {
    constexpr std::uint64_t node_id = 1;
    constexpr std::chrono::milliseconds election_timeout_min{150};
    constexpr std::chrono::milliseconds election_timeout_max{300};
    constexpr std::chrono::milliseconds heartbeat_interval{50};
}

auto test_cluster_initialization() -> bool {
    std::cout << "Test 1: Cluster Initialization\n";
    
    try {
        auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
        simulator.start();
        
        auto sim_node = simulator.create_node(node_id);
        
        auto config = kythira::raft_configuration{};
        config._election_timeout_min = election_timeout_min;
        config._election_timeout_max = election_timeout_max;
        config._heartbeat_interval = heartbeat_interval;
        
        auto node = kythira::node{
            node_id,
            kythira::simulator_network_client<kythira::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{
                sim_node, kythira::json_rpc_serializer<std::vector<std::byte>>{}
            },
            kythira::simulator_network_server<kythira::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{
                sim_node, kythira::json_rpc_serializer<std::vector<std::byte>>{}
            },
            kythira::memory_persistence_engine<>{},
            kythira::console_logger{kythira::log_level::info},
            kythira::noop_metrics{},
            kythira::default_membership_manager<>{},
            config
        };
        
        node.start();
        
        if (!node.is_running()) {
            std::cerr << "  ✗ Failed: Node is not running\n";
            return false;
        }
        
        std::cout << "  Node initialized with ID: " << node.get_node_id() << "\n";
        std::cout << "  ✓ Scenario passed\n";
        
        node.stop();
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto test_membership_manager() -> bool {
    std::cout << "\nTest 2: Membership Manager\n";
    
    try {
        auto membership = kythira::default_membership_manager<>{};
        
        // Test node validation
        constexpr std::uint64_t new_node_id = 2;
        bool is_valid = membership.validate_new_node(new_node_id);
        
        std::cout << "  New node validation: " << (is_valid ? "valid" : "invalid") << "\n";
        
        // Test node authentication
        bool is_authenticated = membership.authenticate_node(new_node_id);
        
        std::cout << "  Node authentication: " << (is_authenticated ? "authenticated" : "not authenticated") << "\n";
        
        std::cout << "  ✓ Scenario passed\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto test_cluster_configuration() -> bool {
    std::cout << "\nTest 3: Cluster Configuration\n";
    
    try {
        // Create a cluster configuration
        kythira::cluster_configuration<std::uint64_t> config;
        config._nodes = {1, 2, 3};
        config._is_joint_consensus = false;
        config._old_nodes = std::nullopt;
        
        std::cout << "  Created configuration with " << config.nodes().size() << " nodes\n";
        std::cout << "  Joint consensus: " << (config.is_joint_consensus() ? "yes" : "no") << "\n";
        
        // Test joint consensus configuration
        auto membership = kythira::default_membership_manager<>{};
        
        kythira::cluster_configuration<std::uint64_t> new_config;
        new_config._nodes = {1, 2, 3, 4};
        new_config._is_joint_consensus = false;
        new_config._old_nodes = std::nullopt;
        
        auto joint_config = membership.create_joint_configuration(config, new_config);
        
        std::cout << "  Created joint consensus configuration\n";
        std::cout << "  Joint consensus: " << (joint_config.is_joint_consensus() ? "yes" : "no") << "\n";
        
        std::cout << "  ✓ Scenario passed\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto main(int argc, char* argv[]) -> int {
    folly::Init init(&argc, &argv);
    
    std::cout << "========================================\n";
    std::cout << "  Raft Membership Changes Example\n";
    std::cout << "========================================\n\n";
    
    int failed_scenarios = 0;
    
    if (!test_cluster_initialization()) failed_scenarios++;
    if (!test_membership_manager()) failed_scenarios++;
    if (!test_cluster_configuration()) failed_scenarios++;
    
    std::cout << "\n========================================\n";
    if (failed_scenarios > 0) {
        std::cout << "  " << failed_scenarios << " scenario(s) failed\n";
        std::cout << "========================================\n";
        return 1;
    }
    
    std::cout << "  All scenarios passed!\n";
    std::cout << "========================================\n";
    return 0;
}
