#include "raft/test_state_machine.hpp"
#include "raft/examples/counter_state_machine.hpp"
#include <iostream>
#include <vector>
#include <cstddef>
#include <cstring>

namespace {
    constexpr const char* test_key = "mykey";
    constexpr const char* test_value = "myvalue";
    constexpr const char* updated_value = "updated";
}

auto test_kv_operations() -> bool {
    std::cout << "Test 1: Key-Value State Machine Operations\n";
    
    try {
        kythira::test_key_value_state_machine sm;
        
        // PUT using binary format
        auto put_cmd = kythira::test_key_value_state_machine<>::make_put_command(test_key, test_value);
        sm.apply(put_cmd, 1);
        
        // GET using binary format
        auto get_cmd = kythira::test_key_value_state_machine<>::make_get_command(test_key);
        auto result = sm.apply(get_cmd, 2);
        
        std::string result_str(reinterpret_cast<const char*>(result.data()), result.size());
        if (result_str != test_value) {
            std::cerr << "  ✗ GET returned wrong value: " << result_str << "\n";
            return false;
        }
        
        // UPDATE using binary format
        auto update_cmd = kythira::test_key_value_state_machine<>::make_put_command(test_key, updated_value);
        sm.apply(update_cmd, 3);
        
        // DELETE using binary format
        auto del_cmd = kythira::test_key_value_state_machine<>::make_del_command(test_key);
        sm.apply(del_cmd, 4);
        
        std::cout << "  ✓ All operations succeeded\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

auto test_counter_operations() -> bool {
    std::cout << "\nTest 2: Counter State Machine Operations\n";
    
    try {
        kythira::examples::counter_state_machine sm;
        
        for (int i = 0; i < 5; ++i) {
            std::string cmd = "INC";
            std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(cmd.data()),
                                        reinterpret_cast<const std::byte*>(cmd.data() + cmd.size()));
            sm.apply(bytes, i + 1);
        }
        
        if (sm.get_value() != 5) {
            std::cerr << "  ✗ Counter value wrong: " << sm.get_value() << "\n";
            return false;
        }
        
        std::cout << "  ✓ Counter operations succeeded\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

auto test_snapshot_operations() -> bool {
    std::cout << "\nTest 3: Snapshot Creation and Restoration\n";
    
    try {
        kythira::test_key_value_state_machine sm1;
        
        // Populate state using binary format
        for (int i = 0; i < 10; ++i) {
            std::string key = "key" + std::to_string(i);
            std::string value = "value" + std::to_string(i);
            auto cmd = kythira::test_key_value_state_machine<>::make_put_command(key, value);
            sm1.apply(cmd, i + 1);
        }
        
        // Create snapshot
        auto snapshot = sm1.get_state();
        
        // Restore to new instance
        kythira::test_key_value_state_machine sm2;
        sm2.restore_from_snapshot(snapshot, 10);
        
        // Verify - check that both have the same number of entries
        if (sm1.size() != sm2.size()) {
            std::cerr << "  ✗ Snapshot restoration failed: size mismatch (sm1=" 
                      << sm1.size() << ", sm2=" << sm2.size() << ")\n";
            return false;
        }
        
        // Verify all keys and values match
        for (int i = 0; i < 10; ++i) {
            std::string key = "key" + std::to_string(i);
            std::string expected_value = "value" + std::to_string(i);
            
            if (!sm2.contains(key)) {
                std::cerr << "  ✗ Snapshot restoration failed: missing key " << key << "\n";
                return false;
            }
            
            auto actual_value = sm2.get_value(key);
            if (!actual_value || *actual_value != expected_value) {
                std::cerr << "  ✗ Snapshot restoration failed: wrong value for key " << key << "\n";
                return false;
            }
        }
        
        std::cout << "  ✓ Snapshot operations succeeded\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

auto test_error_handling() -> bool {
    std::cout << "\nTest 4: Error Handling\n";
    
    try {
        kythira::test_key_value_state_machine sm;
        
        // Invalid command (wrong command type byte)
        try {
            std::vector<std::byte> invalid_cmd = {std::byte{99}}; // Invalid command type
            sm.apply(invalid_cmd, 1);
            std::cerr << "  ✗ Should have thrown exception for invalid command\n";
            return false;
        } catch (const std::invalid_argument&) {
            // Expected
        }
        
        // GET non-existent key (should return empty, not throw)
        auto get_cmd = kythira::test_key_value_state_machine<>::make_get_command("nonexistent");
        auto result = sm.apply(get_cmd, 2);
        if (!result.empty()) {
            std::cerr << "  ✗ GET non-existent key should return empty\n";
            return false;
        }
        
        std::cout << "  ✓ Error handling succeeded\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Unexpected exception: " << e.what() << "\n";
        return false;
    }
}

int main() {
    std::cout << "State Machine Integration Example\n";
    std::cout << "==================================\n\n";
    
    int failed = 0;
    
    if (!test_kv_operations()) failed++;
    if (!test_counter_operations()) failed++;
    if (!test_snapshot_operations()) failed++;
    if (!test_error_handling()) failed++;
    
    std::cout << "\n==================================\n";
    if (failed == 0) {
        std::cout << "All tests passed!\n";
        return 0;
    } else {
        std::cout << failed << " test(s) failed\n";
        return 1;
    }
}
