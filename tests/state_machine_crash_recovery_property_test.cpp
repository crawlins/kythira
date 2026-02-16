#define BOOST_TEST_MODULE state_machine_crash_recovery_property_test
#include <boost/test/unit_test.hpp>
#include <random>
#include <vector>
#include "state_machine_test_utilities.hpp"

namespace {
    constexpr std::size_t num_iterations = 100;
    constexpr std::size_t num_commands_before_crash = 50;
    constexpr std::size_t num_commands_after_recovery = 30;
}

// Test state machine with crash simulation
class crash_recoverable_state_machine {
public:
    auto apply(const std::vector<std::byte>& command, std::uint64_t index) -> std::vector<std::byte> {
        if (command.empty()) return {};
        
        auto cmd_type = static_cast<std::uint8_t>(command[0]);
        std::size_t offset = 1;
        
        // Extract key
        std::uint32_t key_len;
        std::memcpy(&key_len, command.data() + offset, sizeof(key_len));
        offset += sizeof(key_len);
        
        std::string key(reinterpret_cast<const char*>(command.data() + offset), key_len);
        offset += key_len;
        
        if (cmd_type == 0) { // GET
            _last_applied_index = index;
            auto it = _store.find(key);
            if (it != _store.end()) {
                return std::vector<std::byte>(
                    reinterpret_cast<const std::byte*>(it->second.data()),
                    reinterpret_cast<const std::byte*>(it->second.data() + it->second.size())
                );
            }
            return {};
        } else if (cmd_type == 1) { // SET
            std::uint32_t val_len;
            std::memcpy(&val_len, command.data() + offset, sizeof(val_len));
            offset += sizeof(val_len);
            
            std::string value(reinterpret_cast<const char*>(command.data() + offset), val_len);
            _store[key] = value;
            _last_applied_index = index;
            return {};
        } else if (cmd_type == 2) { // DELETE
            _store.erase(key);
            _last_applied_index = index;
            return {};
        }
        
        return {};
    }
    
    auto get_state() const -> std::vector<std::byte> {
        std::vector<std::byte> state;
        
        // Include last applied index in snapshot
        auto index_bytes = reinterpret_cast<const std::byte*>(&_last_applied_index);
        state.insert(state.end(), index_bytes, index_bytes + sizeof(_last_applied_index));
        
        for (const auto& [key, value] : _store) {
            state.insert(state.end(),
                        reinterpret_cast<const std::byte*>(key.data()),
                        reinterpret_cast<const std::byte*>(key.data() + key.size()));
            state.push_back(std::byte{0});
            state.insert(state.end(),
                        reinterpret_cast<const std::byte*>(value.data()),
                        reinterpret_cast<const std::byte*>(value.data() + value.size()));
            state.push_back(std::byte{0});
        }
        return state;
    }
    
    auto restore_from_snapshot(const std::vector<std::byte>& snapshot) -> void {
        _store.clear();
        
        if (snapshot.size() < sizeof(_last_applied_index)) return;
        
        // Restore last applied index
        std::memcpy(&_last_applied_index, snapshot.data(), sizeof(_last_applied_index));
        
        // Parse remaining snapshot
        std::size_t offset = sizeof(_last_applied_index);
        while (offset < snapshot.size()) {
            std::string key, value;
            while (offset < snapshot.size() && snapshot[offset] != std::byte{0}) {
                key += static_cast<char>(snapshot[offset++]);
            }
            offset++; // Skip null terminator
            
            if (offset >= snapshot.size()) break;
            
            while (offset < snapshot.size() && snapshot[offset] != std::byte{0}) {
                value += static_cast<char>(snapshot[offset++]);
            }
            offset++; // Skip null terminator
            
            if (!key.empty()) {
                _store[key] = value;
            }
        }
    }
    
    auto get_last_applied_index() const -> std::uint64_t {
        return _last_applied_index;
    }

private:
    std::map<std::string, std::string> _store;
    std::uint64_t _last_applied_index = 0;
};

BOOST_AUTO_TEST_CASE(property_crash_recovery_preserves_state, * boost::unit_test::timeout(90)) {
    // Property: After crash and recovery from snapshot, state should be preserved
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iter = 0; iter < num_iterations; ++iter) {
        crash_recoverable_state_machine sm;
        
        // Apply commands before "crash"
        std::vector<std::vector<std::byte>> commands;
        for (std::size_t i = 0; i < num_commands_before_crash; ++i) {
            auto cmd = kythira::test::command_generator::generate_random_command(rng);
            commands.push_back(cmd);
            sm.apply(cmd, i + 1);
        }
        
        // Take snapshot before "crash"
        auto snapshot = sm.get_state();
        auto last_index = sm.get_last_applied_index();
        
        // Simulate crash - create new state machine
        crash_recoverable_state_machine recovered_sm;
        
        // Restore from snapshot
        recovered_sm.restore_from_snapshot(snapshot);
        
        // Verify state preserved
        auto recovered_snapshot = recovered_sm.get_state();
        BOOST_CHECK_EQUAL(snapshot.size(), recovered_snapshot.size());
        BOOST_CHECK_EQUAL(last_index, recovered_sm.get_last_applied_index());
    }
}

BOOST_AUTO_TEST_CASE(property_recovery_continues_from_last_index, * boost::unit_test::timeout(90)) {
    // Property: After recovery, can continue applying commands from last index
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iter = 0; iter < num_iterations; ++iter) {
        crash_recoverable_state_machine sm;
        
        // Apply commands before crash
        for (std::size_t i = 0; i < num_commands_before_crash; ++i) {
            auto cmd = kythira::test::command_generator::generate_random_command(rng);
            sm.apply(cmd, i + 1);
        }
        
        auto snapshot = sm.get_state();
        auto last_index = sm.get_last_applied_index();
        
        // Recover
        crash_recoverable_state_machine recovered_sm;
        recovered_sm.restore_from_snapshot(snapshot);
        
        // Continue applying new commands
        for (std::size_t i = 0; i < num_commands_after_recovery; ++i) {
            auto cmd = kythira::test::command_generator::generate_random_command(rng);
            recovered_sm.apply(cmd, last_index + i + 1);
        }
        
        // Should have applied all commands
        BOOST_CHECK_EQUAL(recovered_sm.get_last_applied_index(), 
                         last_index + num_commands_after_recovery);
    }
}

BOOST_AUTO_TEST_CASE(property_multiple_crashes_preserve_state, * boost::unit_test::timeout(120)) {
    // Property: Multiple crash-recovery cycles preserve state correctly
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iter = 0; iter < num_iterations; ++iter) {
        crash_recoverable_state_machine sm;
        std::uint64_t current_index = 0;
        
        // Simulate 5 crash-recovery cycles
        for (std::size_t cycle = 0; cycle < 5; ++cycle) {
            // Apply some commands
            for (std::size_t i = 0; i < 10; ++i) {
                auto cmd = kythira::test::command_generator::generate_random_command(rng);
                sm.apply(cmd, ++current_index);
            }
            
            // Take snapshot
            auto snapshot = sm.get_state();
            
            // Crash and recover
            crash_recoverable_state_machine new_sm;
            new_sm.restore_from_snapshot(snapshot);
            
            // Verify index preserved
            BOOST_CHECK_EQUAL(new_sm.get_last_applied_index(), current_index);
            
            // Continue with recovered machine
            sm = std::move(new_sm);
        }
        
        // Final state should be valid
        BOOST_CHECK_EQUAL(sm.get_last_applied_index(), current_index);
    }
}

BOOST_AUTO_TEST_CASE(property_partial_snapshot_recovery_safe, * boost::unit_test::timeout(60)) {
    // Property: Recovery from corrupted/partial snapshot should be safe
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iter = 0; iter < num_iterations; ++iter) {
        crash_recoverable_state_machine sm;
        
        // Apply commands
        for (std::size_t i = 0; i < 20; ++i) {
            auto cmd = kythira::test::command_generator::generate_random_command(rng);
            sm.apply(cmd, i + 1);
        }
        
        auto snapshot = sm.get_state();
        
        // Simulate partial/corrupted snapshot (truncate)
        if (snapshot.size() > 10) {
            snapshot.resize(snapshot.size() / 2);
        }
        
        // Recovery should not crash
        crash_recoverable_state_machine recovered_sm;
        BOOST_CHECK_NO_THROW(recovered_sm.restore_from_snapshot(snapshot));
    }
}

BOOST_AUTO_TEST_CASE(property_empty_snapshot_recovery, * boost::unit_test::timeout(60)) {
    // Property: Recovery from empty snapshot should result in empty state
    for (std::size_t iter = 0; iter < num_iterations; ++iter) {
        crash_recoverable_state_machine sm;
        
        std::vector<std::byte> empty_snapshot;
        
        // Recovery from empty snapshot should not crash
        BOOST_CHECK_NO_THROW(sm.restore_from_snapshot(empty_snapshot));
        
        // State should be empty
        BOOST_CHECK_EQUAL(sm.get_last_applied_index(), 0);
    }
}
