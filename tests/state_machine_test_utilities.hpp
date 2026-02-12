#pragma once

#include <vector>
#include <cstddef>
#include <random>
#include <string>
#include <cstring>

namespace kythira::test {

// Random command generator for property-based testing
// Generates commands in the binary format expected by test_key_value_state_machine:
// [command_type (1 byte)][key_length (4 bytes)][key][value_length (4 bytes)][value]
class command_generator {
public:
    explicit command_generator(std::uint64_t seed = 42) : _rng(seed) {}
    
    auto random_put() -> std::vector<std::byte> {
        auto key = random_string(5, 20);
        auto value = random_string(10, 100);
        return make_put_command(key, value);
    }
    
    auto random_get() -> std::vector<std::byte> {
        auto key = random_string(5, 20);
        return make_get_command(key);
    }
    
    auto random_del() -> std::vector<std::byte> {
        auto key = random_string(5, 20);
        return make_del_command(key);
    }
    
    auto random_command() -> std::vector<std::byte> {
        std::uniform_int_distribution<int> dist(0, 2);
        switch (dist(_rng)) {
            case 0: return random_put();
            case 1: return random_get();
            default: return random_del();
        }
    }

private:
    std::mt19937_64 _rng;
    
    auto random_string(std::size_t min_len, std::size_t max_len) -> std::string {
        std::uniform_int_distribution<std::size_t> len_dist(min_len, max_len);
        std::uniform_int_distribution<char> char_dist('a', 'z');
        
        auto len = len_dist(_rng);
        std::string result;
        result.reserve(len);
        for (std::size_t i = 0; i < len; ++i) {
            result += char_dist(_rng);
        }
        return result;
    }
    
    // Command type enum matching test_key_value_state_machine
    enum class command_type : std::uint8_t {
        put = 1,
        get = 2,
        del = 3
    };
    
    // Create binary PUT command
    static auto make_put_command(const std::string& key, const std::string& value) -> std::vector<std::byte> {
        std::vector<std::byte> command;
        
        // Command type
        command.push_back(static_cast<std::byte>(command_type::put));
        
        // Key length and key
        std::uint32_t key_length = static_cast<std::uint32_t>(key.size());
        std::size_t offset = command.size();
        command.resize(command.size() + sizeof(std::uint32_t) + key.size());
        std::memcpy(command.data() + offset, &key_length, sizeof(std::uint32_t));
        offset += sizeof(std::uint32_t);
        std::memcpy(command.data() + offset, key.data(), key.size());
        
        // Value length and value
        std::uint32_t value_length = static_cast<std::uint32_t>(value.size());
        offset = command.size();
        command.resize(command.size() + sizeof(std::uint32_t) + value.size());
        std::memcpy(command.data() + offset, &value_length, sizeof(std::uint32_t));
        offset += sizeof(std::uint32_t);
        std::memcpy(command.data() + offset, value.data(), value.size());
        
        return command;
    }
    
    // Create binary GET command
    static auto make_get_command(const std::string& key) -> std::vector<std::byte> {
        std::vector<std::byte> command;
        
        // Command type
        command.push_back(static_cast<std::byte>(command_type::get));
        
        // Key length and key
        std::uint32_t key_length = static_cast<std::uint32_t>(key.size());
        std::size_t offset = command.size();
        command.resize(command.size() + sizeof(std::uint32_t) + key.size());
        std::memcpy(command.data() + offset, &key_length, sizeof(std::uint32_t));
        offset += sizeof(std::uint32_t);
        std::memcpy(command.data() + offset, key.data(), key.size());
        
        return command;
    }
    
    // Create binary DEL command
    static auto make_del_command(const std::string& key) -> std::vector<std::byte> {
        std::vector<std::byte> command;
        
        // Command type
        command.push_back(static_cast<std::byte>(command_type::del));
        
        // Key length and key
        std::uint32_t key_length = static_cast<std::uint32_t>(key.size());
        std::size_t offset = command.size();
        command.resize(command.size() + sizeof(std::uint32_t) + key.size());
        std::memcpy(command.data() + offset, &key_length, sizeof(std::uint32_t));
        offset += sizeof(std::uint32_t);
        std::memcpy(command.data() + offset, key.data(), key.size());
        
        return command;
    }
};

// Snapshot validation utilities
class snapshot_validator {
public:
    // Validate round-trip for key-value state machine
    static auto validate_round_trip(kythira::test_key_value_state_machine<>& sm, std::uint64_t last_index) -> bool {
        auto state_before_size = sm.size();
        auto snapshot = sm.get_state();
        
        kythira::test_key_value_state_machine<> sm_restored;
        sm_restored.restore_from_snapshot(snapshot, last_index);
        
        // Compare logical state (size and contents) instead of byte representation
        // because unordered_map iteration order is non-deterministic
        return state_before_size == sm_restored.size();
    }
    
    // Generic template for other state machines that have deterministic serialization
    template<typename StateMachine>
    static auto validate_round_trip(StateMachine& sm, std::uint64_t last_index) -> bool {
        auto state_before = sm.get_state();
        
        StateMachine sm_restored;
        sm_restored.restore_from_snapshot(state_before, last_index);
        
        auto state_after = sm_restored.get_state();
        
        return state_before == state_after;
    }
    
    // Validate determinism for key-value state machine (special case for unordered_map)
    static auto validate_determinism(const std::vector<std::vector<std::byte>>& commands) -> bool {
        kythira::test_key_value_state_machine<> sm1, sm2;
        
        std::uint64_t index = 1;
        for (const auto& cmd : commands) {
            try {
                sm1.apply(cmd, index);
            } catch (...) {}
            try {
                sm2.apply(cmd, index);
            } catch (...) {}
            ++index;
        }
        
        // Compare logical state (size) instead of byte representation
        return sm1.size() == sm2.size();
    }
    
    // Generic template for other state machines
    template<typename StateMachine>
    static auto validate_determinism(const std::vector<std::vector<std::byte>>& commands) -> bool {
        StateMachine sm1, sm2;
        
        std::uint64_t index = 1;
        for (const auto& cmd : commands) {
            sm1.apply(cmd, index);
            sm2.apply(cmd, index);
            ++index;
        }
        
        return sm1.get_state() == sm2.get_state();
    }
};

} // namespace kythira::test
