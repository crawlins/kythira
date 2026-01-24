#pragma once

#include <raft/types.hpp>
#include <unordered_map>
#include <string>
#include <cstring>
#include <stdexcept>

namespace kythira {

// Simple in-memory key-value store state machine for testing
// Demonstrates the state_machine concept implementation
template<typename LogIndex = std::uint64_t>
requires log_index<LogIndex>
class test_key_value_state_machine {
public:
    // Command types
    enum class command_type : std::uint8_t {
        put = 1,
        get = 2,
        del = 3
    };
    
    test_key_value_state_machine() = default;
    
    // Apply a committed log entry to the state machine
    // Command format: [command_type (1 byte)][key_length (4 bytes)][key][value_length (4 bytes)][value]
    // Returns: For GET commands, returns the value; for PUT/DEL, returns empty
    auto apply(const std::vector<std::byte>& command, LogIndex index) -> std::vector<std::byte> {
        if (command.empty()) {
            throw std::invalid_argument("Empty command");
        }
        
        // Update last applied index
        _last_applied_index = index;
        
        // Parse command type
        auto cmd_type = static_cast<command_type>(command[0]);
        std::size_t offset = 1;
        
        // Parse key
        if (offset + sizeof(std::uint32_t) > command.size()) {
            throw std::invalid_argument("Invalid command format: missing key length");
        }
        
        std::uint32_t key_length;
        std::memcpy(&key_length, &command[offset], sizeof(std::uint32_t));
        offset += sizeof(std::uint32_t);
        
        if (offset + key_length > command.size()) {
            throw std::invalid_argument("Invalid command format: key length exceeds command size");
        }
        
        std::string key(reinterpret_cast<const char*>(&command[offset]), key_length);
        offset += key_length;
        
        // Execute command based on type
        switch (cmd_type) {
            case command_type::put: {
                // Parse value
                if (offset + sizeof(std::uint32_t) > command.size()) {
                    throw std::invalid_argument("Invalid PUT command: missing value length");
                }
                
                std::uint32_t value_length;
                std::memcpy(&value_length, &command[offset], sizeof(std::uint32_t));
                offset += sizeof(std::uint32_t);
                
                if (offset + value_length > command.size()) {
                    throw std::invalid_argument("Invalid PUT command: value length exceeds command size");
                }
                
                std::string value(reinterpret_cast<const char*>(&command[offset]), value_length);
                _store[key] = value;
                
                return {}; // PUT returns empty
            }
            
            case command_type::get: {
                auto it = _store.find(key);
                if (it == _store.end()) {
                    return {}; // Key not found, return empty
                }
                
                // Return value as bytes
                const auto& value = it->second;
                std::vector<std::byte> result(value.size());
                std::memcpy(result.data(), value.data(), value.size());
                return result;
            }
            
            case command_type::del: {
                _store.erase(key);
                return {}; // DEL returns empty
            }
            
            default:
                throw std::invalid_argument("Unknown command type");
        }
    }
    
    // Get the current state of the state machine for snapshot creation
    // Format: [num_entries (8 bytes)][entry1_key_len (4 bytes)][entry1_key][entry1_val_len (4 bytes)][entry1_val]...
    auto get_state() const -> std::vector<std::byte> {
        std::vector<std::byte> state;
        
        // Write number of entries
        std::uint64_t num_entries = _store.size();
        std::size_t offset = 0;
        state.resize(sizeof(std::uint64_t));
        std::memcpy(state.data(), &num_entries, sizeof(std::uint64_t));
        offset += sizeof(std::uint64_t);
        
        // Write each key-value pair
        for (const auto& [key, value] : _store) {
            // Write key length and key
            std::uint32_t key_length = static_cast<std::uint32_t>(key.size());
            state.resize(state.size() + sizeof(std::uint32_t) + key.size());
            std::memcpy(state.data() + offset, &key_length, sizeof(std::uint32_t));
            offset += sizeof(std::uint32_t);
            std::memcpy(state.data() + offset, key.data(), key.size());
            offset += key.size();
            
            // Write value length and value
            std::uint32_t value_length = static_cast<std::uint32_t>(value.size());
            state.resize(state.size() + sizeof(std::uint32_t) + value.size());
            std::memcpy(state.data() + offset, &value_length, sizeof(std::uint32_t));
            offset += sizeof(std::uint32_t);
            std::memcpy(state.data() + offset, value.data(), value.size());
            offset += value.size();
        }
        
        return state;
    }
    
    // Restore the state machine from a snapshot
    auto restore_from_snapshot(const std::vector<std::byte>& snapshot_data, LogIndex index) -> void {
        _store.clear();
        _last_applied_index = index;
        
        if (snapshot_data.empty()) {
            return; // Empty snapshot is valid (empty state machine)
        }
        
        std::size_t offset = 0;
        
        // Read number of entries
        if (offset + sizeof(std::uint64_t) > snapshot_data.size()) {
            throw std::invalid_argument("Invalid snapshot format: missing entry count");
        }
        
        std::uint64_t num_entries;
        std::memcpy(&num_entries, snapshot_data.data(), sizeof(std::uint64_t));
        offset += sizeof(std::uint64_t);
        
        // Read each key-value pair
        for (std::uint64_t i = 0; i < num_entries; ++i) {
            // Read key
            if (offset + sizeof(std::uint32_t) > snapshot_data.size()) {
                throw std::invalid_argument("Invalid snapshot format: missing key length");
            }
            
            std::uint32_t key_length;
            std::memcpy(&key_length, snapshot_data.data() + offset, sizeof(std::uint32_t));
            offset += sizeof(std::uint32_t);
            
            if (offset + key_length > snapshot_data.size()) {
                throw std::invalid_argument("Invalid snapshot format: key length exceeds data size");
            }
            
            std::string key(reinterpret_cast<const char*>(snapshot_data.data() + offset), key_length);
            offset += key_length;
            
            // Read value
            if (offset + sizeof(std::uint32_t) > snapshot_data.size()) {
                throw std::invalid_argument("Invalid snapshot format: missing value length");
            }
            
            std::uint32_t value_length;
            std::memcpy(&value_length, snapshot_data.data() + offset, sizeof(std::uint32_t));
            offset += sizeof(std::uint32_t);
            
            if (offset + value_length > snapshot_data.size()) {
                throw std::invalid_argument("Invalid snapshot format: value length exceeds data size");
            }
            
            std::string value(reinterpret_cast<const char*>(snapshot_data.data() + offset), value_length);
            offset += value_length;
            
            _store[key] = value;
        }
    }
    
    // Helper methods for testing
    [[nodiscard]] auto size() const -> std::size_t {
        return _store.size();
    }
    
    [[nodiscard]] auto contains(const std::string& key) const -> bool {
        return _store.find(key) != _store.end();
    }
    
    [[nodiscard]] auto get_value(const std::string& key) const -> std::optional<std::string> {
        auto it = _store.find(key);
        if (it == _store.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    
    [[nodiscard]] auto get_last_applied_index() const -> LogIndex {
        return _last_applied_index;
    }
    
    // Helper to create PUT command
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
    
    // Helper to create GET command
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
    
    // Helper to create DEL command
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

private:
    std::unordered_map<std::string, std::string> _store;
    LogIndex _last_applied_index{0};
};

// Validate that test_key_value_state_machine satisfies the state_machine concept
static_assert(state_machine<test_key_value_state_machine<std::uint64_t>, std::uint64_t>,
              "test_key_value_state_machine must satisfy state_machine concept");

} // namespace kythira
