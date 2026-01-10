/**
 * Example: Raft Failure Scenarios
 * 
 * This example demonstrates:
 * 1. Leader failure and re-election
 * 2. Node crash and recovery
 * 3. Handling of election timeouts
 * 
 * Note: Uses single-node scenarios due to implementation constraints
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

auto test_leader_failure_and_reelection() -> bool {
    std::cout << "Test 1: Leader Failure and Re-election\n";
    
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
        
        // Become leader
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        if (!node.is_leader()) {
            std::cerr << "  ✗ Failed: Node did not become leader\n";
            node.stop();
            return false;
        }
        
        auto first_term = node.get_current_term();
        std::cout << "  Node became leader in term " << first_term << "\n";
        
        // Simulate failure by stopping
        node.stop();
        std::cout << "  Simulated leader failure (node stopped)\n";
        
        // Restart
        node.start();
        std::cout << "  Node restarted\n";
        
        // Trigger re-election
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        if (!node.is_leader()) {
            std::cerr << "  ✗ Failed: Node did not become leader after restart\n";
            node.stop();
            return false;
        }
        
        std::cout << "  ✓ Scenario passed (Node recovered and became leader)\n";
        
        node.stop();
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto test_follower_crash_and_recovery() -> bool {
    std::cout << "\nTest 2: Follower Crash and Recovery\n";
    
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
        std::cout << "  Node started as follower\n";
        
        // Simulate crash
        node.stop();
        std::cout << "  Simulated follower crash (node stopped)\n";
        
        // Recovery
        node.start();
        std::cout << "  Node recovered and restarted\n";
        
        if (!node.is_running()) {
            std::cerr << "  ✗ Failed: Node is not running after recovery\n";
            return false;
        }
        
        std::cout << "  ✓ Scenario passed (Follower recovered successfully)\n";
        
        node.stop();
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto test_election_timeout_handling() -> bool {
    std::cout << "\nTest 3: Election Timeout Handling\n";
    
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
        
        // Wait for election timeout
        std::cout << "  Waiting for election timeout...\n";
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        
        // Trigger election
        node.check_election_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        if (!node.is_leader()) {
            std::cerr << "  ✗ Failed: Node did not become leader after timeout\n";
            node.stop();
            return false;
        }
        
        std::cout << "  ✓ Scenario passed (Election timeout handled correctly)\n";
        
        node.stop();
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto main(int argc, char* argv[]) -> int {
    folly::Init init(&argc, &argv);
    
    std::cout << "========================================\n";
    std::cout << "  Raft Failure Scenarios Example\n";
    std::cout << "========================================\n\n";
    
    int failed_scenarios = 0;
    
    if (!test_leader_failure_and_reelection()) failed_scenarios++;
    if (!test_follower_crash_and_recovery()) failed_scenarios++;
    if (!test_election_timeout_handling()) failed_scenarios++;
    
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
