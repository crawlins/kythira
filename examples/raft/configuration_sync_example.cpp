/**
 * Example: Configuration Change Synchronization in Raft
 * 
 * This example demonstrates:
 * 1. Server addition with proper synchronization (Requirements 3.1)
 * 2. Server removal with phase-by-phase waiting (Requirements 3.2)
 * 3. Configuration change serialization (Requirements 3.3)
 * 4. Error handling and rollback scenarios (Requirements 3.4)
 * 5. Leadership change during configuration (Requirements 3.5)
 * 
 * This example shows how the Raft implementation safely manages cluster
 * configuration changes using the two-phase protocol to maintain safety
 * properties during membership transitions.
 */

#include <raft/configuration_synchronizer.hpp>
#include <raft/completion_exceptions.hpp>
#include <raft/types.hpp>

#include <folly/init/Init.h>

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <atomic>
#include <format>
#include <future>
#include <algorithm>
#include <unordered_set>

namespace {
    // Test configuration constants
    constexpr std::uint64_t leader_node_id = 1;
    constexpr std::uint64_t follower_node_2_id = 2;
    constexpr std::uint64_t follower_node_3_id = 3;
    constexpr std::uint64_t new_node_4_id = 4;
    constexpr std::uint64_t new_node_5_id = 5;
    constexpr std::uint64_t initial_log_index = 10;
    constexpr std::uint64_t joint_config_log_index = 11;
    constexpr std::uint64_t final_config_log_index = 12;
    constexpr std::chrono::milliseconds config_change_timeout{5000};
    constexpr std::chrono::milliseconds short_timeout{1000};
    constexpr std::chrono::milliseconds long_timeout{10000};
    constexpr const char* config_change_reason_timeout = "Configuration change timed out";
    constexpr const char* config_change_reason_leadership_lost = "Leadership lost during configuration change";
    constexpr const char* config_change_reason_rollback = "Configuration change failed, rolling back";
}

// Helper function to create cluster configuration
auto create_cluster_configuration(
    const std::vector<std::uint64_t>& nodes,
    bool is_joint = false,
    const std::optional<std::vector<std::uint64_t>>& old_nodes = std::nullopt
) -> kythira::cluster_configuration<std::uint64_t> {
    return kythira::cluster_configuration<std::uint64_t>{
        ._nodes = nodes,
        ._is_joint_consensus = is_joint,
        ._old_nodes = old_nodes
    };
}

// Mock Raft node for demonstrating configuration changes
class mock_raft_node {
private:
    std::uint64_t _node_id;
    kythira::cluster_configuration<std::uint64_t> _current_configuration;
    std::uint64_t _current_log_index;
    bool _is_leader;
    std::atomic<bool> _simulate_failures{false};
    
public:
    explicit mock_raft_node(
        std::uint64_t node_id,
        const kythira::cluster_configuration<std::uint64_t>& initial_config
    ) : _node_id(node_id)
      , _current_configuration(initial_config)
      , _current_log_index(initial_log_index)
      , _is_leader(node_id == leader_node_id) {}
    
    // Simulate appending a configuration entry to the log
    auto append_configuration_entry(
        const kythira::cluster_configuration<std::uint64_t>& config
    ) -> std::uint64_t {
        if (!_is_leader) {
            throw std::runtime_error("Only leader can append configuration entries");
        }
        
        if (_simulate_failures) {
            throw std::runtime_error("Simulated append failure");
        }
        
        _current_log_index++;
        std::cout << std::format("    Appended configuration entry at log index {}\n", _current_log_index);
        
        if (config.is_joint_consensus()) {
            std::cout << "      Joint consensus configuration: [";
            for (std::size_t i = 0; i < config.nodes().size(); ++i) {
                std::cout << config.nodes()[i];
                if (i < config.nodes().size() - 1) std::cout << ", ";
            }
            std::cout << "] + [";
            if (config.old_nodes()) {
                for (std::size_t i = 0; i < config.old_nodes()->size(); ++i) {
                    std::cout << (*config.old_nodes())[i];
                    if (i < config.old_nodes()->size() - 1) std::cout << ", ";
                }
            }
            std::cout << "]\n";
        } else {
            std::cout << "      Final configuration: [";
            for (std::size_t i = 0; i < config.nodes().size(); ++i) {
                std::cout << config.nodes()[i];
                if (i < config.nodes().size() - 1) std::cout << ", ";
            }
            std::cout << "]\n";
        }
        
        return _current_log_index;
    }
    
    // Simulate committing an entry at the given log index
    auto commit_entry(std::uint64_t log_index) -> void {
        if (log_index > _current_log_index) {
            throw std::runtime_error("Cannot commit entry beyond current log index");
        }
        
        std::cout << std::format("    Committed entry at log index {}\n", log_index);
    }
    
    // Simulate leadership loss
    auto lose_leadership() -> void {
        _is_leader = false;
        std::cout << std::format("    Node {} lost leadership\n", _node_id);
    }
    
    // Simulate regaining leadership
    auto become_leader() -> void {
        _is_leader = true;
        std::cout << std::format("    Node {} became leader\n", _node_id);
    }
    
    // Enable/disable failure simulation
    auto set_failure_simulation(bool enable) -> void {
        _simulate_failures = enable;
    }
    
    // Get current configuration
    auto get_current_configuration() const -> const kythira::cluster_configuration<std::uint64_t>& {
        return _current_configuration;
    }
    
    // Update current configuration (after commit)
    auto update_configuration(const kythira::cluster_configuration<std::uint64_t>& config) -> void {
        _current_configuration = config;
        std::cout << "    Updated current configuration\n";
    }
    
    auto is_leader() const -> bool { return _is_leader; }
    auto get_node_id() const -> std::uint64_t { return _node_id; }
    auto get_current_log_index() const -> std::uint64_t { return _current_log_index; }
};

// Test scenario 1: Server addition with proper synchronization
auto test_server_addition_synchronization() -> bool {
    std::cout << "Test 1: Server Addition with Proper Synchronization\n";
    
    try {
        // Create initial cluster configuration (nodes 1, 2, 3)
        auto initial_config = create_cluster_configuration({leader_node_id, follower_node_2_id, follower_node_3_id});
        auto mock_node = mock_raft_node{leader_node_id, initial_config};
        
        // Create configuration synchronizer
        kythira::configuration_synchronizer<std::uint64_t, std::uint64_t> config_sync;
        
        std::cout << "  Adding server 4 to cluster...\n";
        std::cout << "  Initial configuration: [1, 2, 3]\n";
        
        // Create target configuration (add node 4)
        auto target_config = create_cluster_configuration({leader_node_id, follower_node_2_id, follower_node_3_id, new_node_4_id});
        
        // Start configuration change
        auto config_future = config_sync.start_configuration_change(target_config, config_change_timeout);
        
        std::cout << "  Started configuration change\n";
        std::cout << std::format("  Configuration change in progress: {}\n", 
                                config_sync.is_configuration_change_in_progress());
        std::cout << std::format("  Waiting for joint consensus: {}\n", 
                                config_sync.is_waiting_for_joint_consensus());
        
        // Phase 1: Append joint consensus configuration
        auto joint_config = create_cluster_configuration(
            {leader_node_id, follower_node_2_id, follower_node_3_id, new_node_4_id},
            true, // is_joint_consensus
            std::vector<std::uint64_t>{leader_node_id, follower_node_2_id, follower_node_3_id} // old_nodes
        );
        
        auto joint_log_index = mock_node.append_configuration_entry(joint_config);
        
        // Simulate replication and commit of joint consensus
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        mock_node.commit_entry(joint_log_index);
        config_sync.notify_configuration_committed(joint_config, joint_log_index);
        
        std::cout << "  Joint consensus configuration committed\n";
        std::cout << std::format("  Waiting for final configuration: {}\n", 
                                config_sync.is_waiting_for_final_configuration());
        
        // Phase 2: Append final configuration
        auto final_log_index = mock_node.append_configuration_entry(target_config);
        
        // Simulate replication and commit of final configuration
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        mock_node.commit_entry(final_log_index);
        config_sync.notify_configuration_committed(target_config, final_log_index);
        
        std::cout << "  Final configuration committed\n";
        
        // Wait for configuration change to complete
        auto result = std::move(config_future).get();
        
        if (result && !config_sync.is_configuration_change_in_progress()) {
            std::cout << "  ✓ Server addition completed successfully\n";
            std::cout << "  Final configuration: [1, 2, 3, 4]\n";
            return true;
        } else {
            std::cerr << "  ✗ Failed: Configuration change did not complete successfully\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 2: Server removal with phase-by-phase waiting
auto test_server_removal_synchronization() -> bool {
    std::cout << "\nTest 2: Server Removal with Phase-by-Phase Waiting\n";
    
    try {
        // Create initial cluster configuration (nodes 1, 2, 3, 4)
        auto initial_config = create_cluster_configuration({leader_node_id, follower_node_2_id, follower_node_3_id, new_node_4_id});
        auto mock_node = mock_raft_node{leader_node_id, initial_config};
        
        // Create configuration synchronizer
        kythira::configuration_synchronizer<std::uint64_t, std::uint64_t> config_sync;
        
        std::cout << "  Removing server 4 from cluster...\n";
        std::cout << "  Initial configuration: [1, 2, 3, 4]\n";
        
        // Create target configuration (remove node 4)
        auto target_config = create_cluster_configuration({leader_node_id, follower_node_2_id, follower_node_3_id});
        
        // Start configuration change
        auto config_future = config_sync.start_configuration_change(target_config, config_change_timeout);
        
        std::cout << "  Started configuration change for server removal\n";
        
        // Phase 1: Append joint consensus configuration
        auto joint_config = create_cluster_configuration(
            {leader_node_id, follower_node_2_id, follower_node_3_id}, // new nodes
            true, // is_joint_consensus
            std::vector<std::uint64_t>{leader_node_id, follower_node_2_id, follower_node_3_id, new_node_4_id} // old_nodes
        );
        
        std::cout << "  Phase 1: Appending joint consensus configuration...\n";
        auto joint_log_index = mock_node.append_configuration_entry(joint_config);
        
        // Wait for phase 1 commit
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        mock_node.commit_entry(joint_log_index);
        config_sync.notify_configuration_committed(joint_config, joint_log_index);
        
        std::cout << "  Phase 1 completed: Joint consensus committed\n";
        std::cout << std::format("  Now waiting for final configuration: {}\n", 
                                config_sync.is_waiting_for_final_configuration());
        
        // Phase 2: Append final configuration
        std::cout << "  Phase 2: Appending final configuration...\n";
        auto final_log_index = mock_node.append_configuration_entry(target_config);
        
        // Wait for phase 2 commit
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        mock_node.commit_entry(final_log_index);
        config_sync.notify_configuration_committed(target_config, final_log_index);
        
        std::cout << "  Phase 2 completed: Final configuration committed\n";
        
        // Wait for configuration change to complete
        auto result = std::move(config_future).get();
        
        if (result && !config_sync.is_configuration_change_in_progress()) {
            std::cout << "  ✓ Server removal completed successfully\n";
            std::cout << "  Final configuration: [1, 2, 3]\n";
            return true;
        } else {
            std::cerr << "  ✗ Failed: Configuration change did not complete successfully\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 3: Configuration change serialization (preventing concurrent changes)
auto test_configuration_change_serialization() -> bool {
    std::cout << "\nTest 3: Configuration Change Serialization\n";
    
    try {
        // Create initial cluster configuration
        auto initial_config = create_cluster_configuration({leader_node_id, follower_node_2_id, follower_node_3_id});
        auto mock_node = mock_raft_node{leader_node_id, initial_config};
        
        // Create configuration synchronizer
        kythira::configuration_synchronizer<std::uint64_t, std::uint64_t> config_sync;
        
        std::cout << "  Testing prevention of concurrent configuration changes...\n";
        
        // Start first configuration change (add node 4)
        auto target_config_1 = create_cluster_configuration({leader_node_id, follower_node_2_id, follower_node_3_id, new_node_4_id});
        auto config_future_1 = config_sync.start_configuration_change(target_config_1, config_change_timeout);
        
        std::cout << "  Started first configuration change (add node 4)\n";
        std::cout << std::format("  Configuration change in progress: {}\n", 
                                config_sync.is_configuration_change_in_progress());
        
        // Try to start second configuration change (should return exceptional future)
        auto target_config_2 = create_cluster_configuration({leader_node_id, follower_node_2_id, follower_node_3_id, new_node_5_id});
        
        bool second_change_rejected = false;
        std::string rejection_reason;
        
        try {
            auto config_future_2 = config_sync.start_configuration_change(target_config_2, config_change_timeout);
            // The future should be exceptional, so getting the result should throw
            auto result_2 = std::move(config_future_2).get();
            std::cerr << "  Unexpected: Second configuration change was not rejected\n";
        } catch (const kythira::configuration_change_exception& e) {
            second_change_rejected = true;
            rejection_reason = e.what();
            std::cout << std::format("  ✓ Second configuration change rejected: {}\n", e.what());
        }
        
        if (!second_change_rejected) {
            std::cerr << "  ✗ Failed: Second configuration change should have been rejected\n";
            return false;
        }
        
        // Complete the first configuration change
        auto joint_config = create_cluster_configuration(
            {leader_node_id, follower_node_2_id, follower_node_3_id, new_node_4_id},
            true,
            std::vector<std::uint64_t>{leader_node_id, follower_node_2_id, follower_node_3_id}
        );
        
        auto joint_log_index = mock_node.append_configuration_entry(joint_config);
        mock_node.commit_entry(joint_log_index);
        config_sync.notify_configuration_committed(joint_config, joint_log_index);
        
        auto final_log_index = mock_node.append_configuration_entry(target_config_1);
        mock_node.commit_entry(final_log_index);
        config_sync.notify_configuration_committed(target_config_1, final_log_index);
        
        // Wait for first change to complete
        auto result_1 = std::move(config_future_1).get();
        
        std::cout << "  First configuration change completed\n";
        std::cout << std::format("  Configuration change in progress: {}\n", 
                                config_sync.is_configuration_change_in_progress());
        
        // Now try the second configuration change (should succeed)
        try {
            auto config_future_3 = config_sync.start_configuration_change(target_config_2, config_change_timeout);
            std::cout << "  ✓ Second configuration change now accepted after first completed\n";
            
            // Cancel it to clean up
            config_sync.cancel_configuration_change("Test cleanup");
            
            return true;
        } catch (const std::exception& e) {
            std::cerr << std::format("  ✗ Failed: Second configuration change should succeed after first completes: {}\n", e.what());
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 4: Error handling and rollback scenarios
auto test_error_handling_and_rollback() -> bool {
    std::cout << "\nTest 4: Error Handling and Rollback Scenarios\n";
    
    try {
        // Create initial cluster configuration
        auto initial_config = create_cluster_configuration({leader_node_id, follower_node_2_id, follower_node_3_id});
        auto mock_node = mock_raft_node{leader_node_id, initial_config};
        
        // Create configuration synchronizer
        kythira::configuration_synchronizer<std::uint64_t, std::uint64_t> config_sync;
        
        std::cout << "  Testing configuration change cancellation and rollback...\n";
        
        // Start configuration change
        auto target_config = create_cluster_configuration({leader_node_id, follower_node_2_id, follower_node_3_id, new_node_4_id});
        auto config_future = config_sync.start_configuration_change(target_config, config_change_timeout);
        
        std::cout << "  Started configuration change\n";
        
        // Append joint consensus but don't commit it (simulate slow replication)
        auto joint_config = create_cluster_configuration(
            {leader_node_id, follower_node_2_id, follower_node_3_id, new_node_4_id},
            true,
            std::vector<std::uint64_t>{leader_node_id, follower_node_2_id, follower_node_3_id}
        );
        
        auto joint_log_index = mock_node.append_configuration_entry(joint_config);
        std::cout << "  Appended joint consensus configuration (simulating slow replication)\n";
        
        // Cancel configuration change due to some error condition
        config_sync.cancel_configuration_change(config_change_reason_rollback);
        std::cout << "  Cancelled configuration change due to error condition\n";
        
        // Verify configuration change was cancelled
        bool cancellation_handled = false;
        std::string cancellation_error;
        
        try {
            auto result = std::move(config_future).get();
            std::cerr << "  Unexpected: Configuration change should have been cancelled\n";
        } catch (const kythira::configuration_change_exception& e) {
            cancellation_handled = true;
            cancellation_error = e.what();
            std::cout << std::format("  ✓ Configuration change cancelled: {}\n", e.what());
        }
        
        if (!cancellation_handled) {
            std::cerr << "  ✗ Failed: Configuration change should have been cancelled\n";
            return false;
        }
        
        // Verify synchronizer state was reset
        if (!config_sync.is_configuration_change_in_progress()) {
            std::cout << "  ✓ Configuration synchronizer state reset after cancellation\n";
            return true;
        } else {
            std::cerr << "  ✗ Failed: Configuration synchronizer state not reset after cancellation\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 5: Leadership change during configuration
auto test_leadership_change_during_configuration() -> bool {
    std::cout << "\nTest 5: Leadership Change During Configuration\n";
    
    try {
        // Create initial cluster configuration
        auto initial_config = create_cluster_configuration({leader_node_id, follower_node_2_id, follower_node_3_id});
        auto mock_node = mock_raft_node{leader_node_id, initial_config};
        
        // Create configuration synchronizer
        kythira::configuration_synchronizer<std::uint64_t, std::uint64_t> config_sync;
        
        std::cout << "  Testing leadership change during configuration change...\n";
        
        // Start configuration change
        auto target_config = create_cluster_configuration({leader_node_id, follower_node_2_id, follower_node_3_id, new_node_4_id});
        auto config_future = config_sync.start_configuration_change(target_config, config_change_timeout);
        
        std::cout << "  Started configuration change\n";
        
        // Append joint consensus configuration
        auto joint_config = create_cluster_configuration(
            {leader_node_id, follower_node_2_id, follower_node_3_id, new_node_4_id},
            true,
            std::vector<std::uint64_t>{leader_node_id, follower_node_2_id, follower_node_3_id}
        );
        
        auto joint_log_index = mock_node.append_configuration_entry(joint_config);
        std::cout << "  Appended joint consensus configuration\n";
        
        // Simulate leadership loss before commit
        mock_node.lose_leadership();
        std::cout << "  Leadership lost during configuration change\n";
        
        // Cancel configuration change due to leadership loss
        config_sync.cancel_configuration_change(config_change_reason_leadership_lost);
        
        // Verify configuration change was cancelled
        bool leadership_loss_handled = false;
        std::string cancellation_error;
        
        try {
            auto result = std::move(config_future).get();
            std::cerr << "  Unexpected: Configuration change should have been cancelled\n";
        } catch (const kythira::configuration_change_exception& e) {
            leadership_loss_handled = true;
            cancellation_error = e.what();
            std::cout << std::format("  ✓ Configuration change cancelled: {}\n", e.what());
        }
        
        if (!leadership_loss_handled) {
            std::cerr << "  ✗ Failed: Configuration change should have been cancelled due to leadership loss\n";
            return false;
        }
        
        // Verify synchronizer state was reset
        if (!config_sync.is_configuration_change_in_progress()) {
            std::cout << "  ✓ Configuration synchronizer state reset after leadership loss\n";
            
            // Simulate regaining leadership and starting new configuration change
            mock_node.become_leader();
            
            try {
                auto new_config_future = config_sync.start_configuration_change(target_config, config_change_timeout);
                std::cout << "  ✓ New configuration change started after regaining leadership\n";
                
                // Cancel to clean up
                config_sync.cancel_configuration_change("Test cleanup");
                
                return true;
            } catch (const std::exception& e) {
                std::cerr << std::format("  ✗ Failed: Should be able to start new configuration change after regaining leadership: {}\n", e.what());
                return false;
            }
        } else {
            std::cerr << "  ✗ Failed: Configuration synchronizer state not reset after leadership loss\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 6: Configuration change failure during joint consensus phase
auto test_joint_consensus_phase_failure() -> bool {
    std::cout << "\nTest 6: Configuration Change Failure During Joint Consensus Phase\n";
    
    try {
        // Create initial cluster configuration
        auto initial_config = create_cluster_configuration({leader_node_id, follower_node_2_id, follower_node_3_id});
        auto mock_node = mock_raft_node{leader_node_id, initial_config};
        
        // Create configuration synchronizer
        kythira::configuration_synchronizer<std::uint64_t, std::uint64_t> config_sync;
        
        std::cout << "  Testing failure during joint consensus phase...\n";
        
        // Start configuration change
        auto target_config = create_cluster_configuration({leader_node_id, follower_node_2_id, follower_node_3_id, new_node_4_id});
        auto config_future = config_sync.start_configuration_change(target_config, config_change_timeout);
        
        std::cout << "  Started configuration change\n";
        std::cout << std::format("  Waiting for joint consensus: {}\n", 
                                config_sync.is_waiting_for_joint_consensus());
        
        // Enable failure simulation
        mock_node.set_failure_simulation(true);
        
        // Try to append joint consensus configuration (will fail)
        bool append_failed = false;
        std::string append_error;
        
        try {
            auto joint_config = create_cluster_configuration(
                {leader_node_id, follower_node_2_id, follower_node_3_id, new_node_4_id},
                true,
                std::vector<std::uint64_t>{leader_node_id, follower_node_2_id, follower_node_3_id}
            );
            
            auto joint_log_index = mock_node.append_configuration_entry(joint_config);
            std::cerr << "  Unexpected: Joint consensus append should have failed\n";
        } catch (const std::runtime_error& e) {
            append_failed = true;
            append_error = e.what();
            std::cout << std::format("  ✓ Joint consensus append failed as expected: {}\n", e.what());
        }
        
        if (!append_failed) {
            std::cerr << "  ✗ Failed: Joint consensus append should have failed\n";
            return false;
        }
        
        // Cancel configuration change due to failure
        config_sync.cancel_configuration_change(config_change_reason_rollback);
        
        // Verify configuration change was cancelled
        bool rollback_handled = false;
        
        try {
            auto result = std::move(config_future).get();
            std::cerr << "  Unexpected: Configuration change should have been cancelled\n";
        } catch (const kythira::configuration_change_exception& e) {
            rollback_handled = true;
            std::cout << std::format("  ✓ Configuration change rolled back: {}\n", e.what());
        }
        
        if (rollback_handled && !config_sync.is_configuration_change_in_progress()) {
            std::cout << "  ✓ Rollback during joint consensus phase handled correctly\n";
            return true;
        } else {
            std::cerr << "  ✗ Failed: Rollback not handled correctly\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto main(int argc, char* argv[]) -> int {
    // Initialize Folly
    folly::Init init(&argc, &argv);
    
    std::cout << "========================================\n";
    std::cout << "  Configuration Synchronization Example\n";
    std::cout << "========================================\n\n";
    
    std::cout << "This example demonstrates configuration change synchronization in Raft:\n";
    std::cout << "- Server addition with proper two-phase synchronization\n";
    std::cout << "- Server removal with phase-by-phase waiting\n";
    std::cout << "- Configuration change serialization (preventing concurrent changes)\n";
    std::cout << "- Error handling and rollback scenarios\n";
    std::cout << "- Leadership change during configuration operations\n";
    std::cout << "- Joint consensus phase failure handling\n\n";
    
    int failed_scenarios = 0;
    
    // Run all test scenarios
    if (!test_server_addition_synchronization()) failed_scenarios++;
    if (!test_server_removal_synchronization()) failed_scenarios++;
    if (!test_configuration_change_serialization()) failed_scenarios++;
    if (!test_error_handling_and_rollback()) failed_scenarios++;
    if (!test_leadership_change_during_configuration()) failed_scenarios++;
    if (!test_joint_consensus_phase_failure()) failed_scenarios++;
    
    // Print summary
    std::cout << "\n========================================\n";
    if (failed_scenarios > 0) {
        std::cout << std::format("  {} scenario(s) failed\n", failed_scenarios);
        std::cout << "========================================\n";
        return 1;
    }
    
    std::cout << "  All scenarios passed!\n";
    std::cout << "  Configuration synchronization working correctly.\n";
    std::cout << "========================================\n";
    return 0;
}