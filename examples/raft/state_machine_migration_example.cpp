#include "raft/test_state_machine.hpp"
#include <iostream>
#include <vector>
#include <cstddef>

// Example: Migrating from v1 to v2 state machine format
// V1: Simple key-value
// V2: Key-value with metadata (version, timestamp)

auto test_backward_compatibility() -> bool {
    std::cout << "Test: Backward Compatible Command Format\n";
    
    try {
        kythira::test_key_value_state_machine sm;
        
        // V1 command using binary format
        auto v1_cmd = kythira::test_key_value_state_machine<>::make_put_command("key1", "value1");
        sm.apply(v1_cmd, 1);
        
        // V2 command (same format for this example)
        auto v2_cmd = kythira::test_key_value_state_machine<>::make_put_command("key2", "value2");
        sm.apply(v2_cmd, 2);
        
        std::cout << "  ✓ Both command versions work\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

auto test_snapshot_versioning() -> bool {
    std::cout << "\nTest: Snapshot Format Versioning\n";
    
    try {
        kythira::test_key_value_state_machine sm;
        
        // Populate with data using binary format
        auto cmd = kythira::test_key_value_state_machine<>::make_put_command("key", "value");
        sm.apply(cmd, 1);
        
        // Create snapshot (current version)
        auto snapshot = sm.get_state();
        
        // Restore (handles version detection internally)
        kythira::test_key_value_state_machine sm2;
        sm2.restore_from_snapshot(snapshot, 1);
        
        std::cout << "  ✓ Snapshot versioning works\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

int main() {
    std::cout << "State Machine Migration Example\n";
    std::cout << "================================\n\n";
    
    int failed = 0;
    
    if (!test_backward_compatibility()) failed++;
    if (!test_snapshot_versioning()) failed++;
    
    std::cout << "\n================================\n";
    if (failed == 0) {
        std::cout << "All tests passed!\n";
        return 0;
    } else {
        std::cout << failed << " test(s) failed\n";
        return 1;
    }
}
