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
#include <raft/test_state_machine.hpp>

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
    
    // Define custom raft types for simulator-based examples
    struct simulator_raft_types {
        // Future types
        using future_type = kythira::Future<std::vector<std::byte>>;
        using promise_type = kythira::Promise<std::vector<std::byte>>;
        using try_type = kythira::Try<std::vector<std::byte>>;
        
        // Basic data types
        using node_id_type = std::uint64_t;
        using term_id_type = std::uint64_t;
        using log_index_type = std::uint64_t;
        
        // Serializer and data types
        using serialized_data_type = std::vector<std::byte>;
        using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;
        
        // Network types
        using network_types = kythira::raft_simulator_network_types<std::string>;
        using network_client_type = kythira::simulator_network_client<network_types, serializer_type, serialized_data_type>;
        using network_server_type = kythira::simulator_network_server<network_types, serializer_type, serialized_data_type>;
        
        // Other component types
        using persistence_engine_type = kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
        using logger_type = kythira::console_logger;
        using metrics_type = kythira::noop_metrics;
        using membership_manager_type = kythira::default_membership_manager<node_id_type>;
        using state_machine_type = kythira::test_key_value_state_machine<log_index_type>;
        
        // Configuration type
        using configuration_type = kythira::raft_configuration;
        
        // Type aliases for commonly used compound types
        using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
        using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
        using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;
        
        // RPC message types
        using request_vote_request_type = kythira::request_vote_request<node_id_type, term_id_type, log_index_type>;
        using request_vote_response_type = kythira::request_vote_response<term_id_type>;
        using append_entries_request_type = kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type>;
        using append_entries_response_type = kythira::append_entries_response<term_id_type, log_index_type>;
        using install_snapshot_request_type = kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type>;
        using install_snapshot_response_type = kythira::install_snapshot_response<term_id_type>;
    };
}

auto test_leader_failure_and_reelection() -> bool {
    std::cout << "Test 1: Leader Failure and Re-election\n";
    
    try {
        // Create network simulator
        using network_types = simulator_raft_types::network_types;
        auto simulator = network_simulator::NetworkSimulator<network_types>{};
        simulator.start();
        
        auto sim_node = simulator.create_node(std::to_string(node_id));
        
        auto config = kythira::raft_configuration{};
        config._election_timeout_min = election_timeout_min;
        config._election_timeout_max = election_timeout_max;
        config._heartbeat_interval = heartbeat_interval;
        
        auto node = kythira::node<simulator_raft_types>{
            node_id,
            simulator_raft_types::network_client_type{
                sim_node, kythira::json_rpc_serializer<std::vector<std::byte>>{}
            },
            simulator_raft_types::network_server_type{
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
        // Create network simulator
        using network_types = simulator_raft_types::network_types;
        auto simulator = network_simulator::NetworkSimulator<network_types>{};
        simulator.start();
        
        auto sim_node = simulator.create_node(std::to_string(node_id));
        
        auto config = kythira::raft_configuration{};
        config._election_timeout_min = election_timeout_min;
        config._election_timeout_max = election_timeout_max;
        config._heartbeat_interval = heartbeat_interval;
        
        auto node = kythira::node<simulator_raft_types>{
            node_id,
            simulator_raft_types::network_client_type{
                sim_node, kythira::json_rpc_serializer<std::vector<std::byte>>{}
            },
            simulator_raft_types::network_server_type{
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
        // Create network simulator
        using network_types = simulator_raft_types::network_types;
        auto simulator = network_simulator::NetworkSimulator<network_types>{};
        simulator.start();
        
        auto sim_node = simulator.create_node(std::to_string(node_id));
        
        auto config = kythira::raft_configuration{};
        config._election_timeout_min = election_timeout_min;
        config._election_timeout_max = election_timeout_max;
        config._heartbeat_interval = heartbeat_interval;
        
        auto node = kythira::node<simulator_raft_types>{
            node_id,
            simulator_raft_types::network_client_type{
                sim_node, kythira::json_rpc_serializer<std::vector<std::byte>>{}
            },
            simulator_raft_types::network_server_type{
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
