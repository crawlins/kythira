/**
 * Example: Raft Snapshot and Log Compaction
 * 
 * This example demonstrates:
 * 1. Snapshot data structures
 * 2. Snapshot metadata
 * 3. Log compaction concepts
 * 
 * Note: Simplified demonstration of snapshot structures
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

auto test_snapshot_creation() -> bool {
    std::cout << "Test 1: Snapshot Creation\n";
    
    try {
        // Create a snapshot
        kythira::snapshot<std::uint64_t, std::uint64_t, std::uint64_t> snap;
        snap._last_included_index = 100;
        snap._last_included_term = 5;
        snap._configuration._nodes = {1, 2, 3};
        snap._configuration._is_joint_consensus = false;
        snap._configuration._old_nodes = std::nullopt;
        snap._state_machine_state = {std::byte{1}, std::byte{2}, std::byte{3}};
        
        std::cout << "  Created snapshot:\n";
        std::cout << "    Last included index: " << snap.last_included_index() << "\n";
        std::cout << "    Last included term: " << snap.last_included_term() << "\n";
        std::cout << "    Configuration nodes: " << snap.configuration().nodes().size() << "\n";
        std::cout << "    State machine state size: " << snap.state_machine_state().size() << " bytes\n";
        
        std::cout << "  ✓ Scenario passed\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto test_snapshot_persistence() -> bool {
    std::cout << "\nTest 2: Snapshot Persistence\n";
    
    try {
        auto persistence = kythira::memory_persistence_engine<>{};
        
        // Create and save a snapshot
        kythira::snapshot<std::uint64_t, std::uint64_t, std::uint64_t> snap;
        snap._last_included_index = 50;
        snap._last_included_term = 3;
        snap._configuration._nodes = {1};
        snap._configuration._is_joint_consensus = false;
        snap._configuration._old_nodes = std::nullopt;
        snap._state_machine_state = {std::byte{10}, std::byte{20}, std::byte{30}};
        
        persistence.save_snapshot(snap);
        std::cout << "  Saved snapshot to persistence\n";
        
        // Load the snapshot
        auto loaded_snap = persistence.load_snapshot();
        
        if (!loaded_snap.has_value()) {
            std::cerr << "  ✗ Failed: Could not load snapshot\n";
            return false;
        }
        
        std::cout << "  Loaded snapshot from persistence\n";
        std::cout << "    Last included index: " << loaded_snap->last_included_index() << "\n";
        std::cout << "    Last included term: " << loaded_snap->last_included_term() << "\n";
        
        if (loaded_snap->last_included_index() != snap.last_included_index()) {
            std::cerr << "  ✗ Failed: Snapshot data mismatch\n";
            return false;
        }
        
        std::cout << "  ✓ Scenario passed\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto test_log_compaction_concept() -> bool {
    std::cout << "\nTest 3: Log Compaction Concept\n";
    
    try {
        auto persistence = kythira::memory_persistence_engine<>{};
        
        // Add some log entries
        for (std::uint64_t i = 1; i <= 10; ++i) {
            kythira::log_entry<std::uint64_t, std::uint64_t> entry;
            entry._term = 1;
            entry._index = i;
            entry._command = {std::byte{static_cast<unsigned char>(i)}};
            persistence.append_log_entry(entry);
        }
        
        std::cout << "  Added 10 log entries\n";
        std::cout << "  Last log index: " << persistence.get_last_log_index() << "\n";
        
        // Simulate log compaction by deleting entries before index 5
        persistence.delete_log_entries_before(5);
        std::cout << "  Deleted log entries before index 5 (simulating compaction)\n";
        
        // Verify entries were deleted
        auto entry_1 = persistence.get_log_entry(1);
        auto entry_6 = persistence.get_log_entry(6);
        
        if (entry_1.has_value()) {
            std::cerr << "  ✗ Failed: Entry 1 should have been deleted\n";
            return false;
        }
        
        if (!entry_6.has_value()) {
            std::cerr << "  ✗ Failed: Entry 6 should still exist\n";
            return false;
        }
        
        std::cout << "  ✓ Scenario passed (Log compaction demonstrated)\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto main(int argc, char* argv[]) -> int {
    folly::Init init(&argc, &argv);
    
    std::cout << "========================================\n";
    std::cout << "  Raft Snapshot Example\n";
    std::cout << "========================================\n\n";
    
    int failed_scenarios = 0;
    
    if (!test_snapshot_creation()) failed_scenarios++;
    if (!test_snapshot_persistence()) failed_scenarios++;
    if (!test_log_compaction_concept()) failed_scenarios++;
    
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
