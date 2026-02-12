#pragma once

#include <vector>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace kythira::examples {

// Append-only log state machine
// Demonstrates efficient snapshot strategy for append-only data
class replicated_log_state_machine {
public:
    // Apply command: APPEND <data>
    auto apply(const std::vector<std::byte>& command, std::uint64_t index) -> std::vector<std::byte> {
        if (command.size() < 7 || std::memcmp(command.data(), "APPEND ", 7) != 0) {
            throw std::invalid_argument("Invalid command format");
        }
        
        std::vector<std::byte> data(command.begin() + 7, command.end());
        _entries.push_back({index, std::move(data)});
        
        return {}; // Empty response
    }
    
    // Get current state (all entries)
    auto get_state() const -> std::vector<std::byte> {
        std::vector<std::byte> state;
        for (const auto& entry : _entries) {
            auto idx_bytes = reinterpret_cast<const std::byte*>(&entry.index);
            state.insert(state.end(), idx_bytes, idx_bytes + sizeof(entry.index));
            
            auto size = static_cast<std::uint64_t>(entry.data.size());
            auto size_bytes = reinterpret_cast<const std::byte*>(&size);
            state.insert(state.end(), size_bytes, size_bytes + sizeof(size));
            
            state.insert(state.end(), entry.data.begin(), entry.data.end());
        }
        return state;
    }
    
    // Restore from snapshot
    auto restore_from_snapshot(const std::vector<std::byte>& state, std::uint64_t last_index) -> void {
        _entries.clear();
        
        std::size_t offset = 0;
        while (offset + sizeof(std::uint64_t) * 2 <= state.size()) {
            std::uint64_t index;
            std::memcpy(&index, state.data() + offset, sizeof(index));
            offset += sizeof(index);
            
            std::uint64_t size;
            std::memcpy(&size, state.data() + offset, sizeof(size));
            offset += sizeof(size);
            
            if (offset + size > state.size()) break;
            
            std::vector<std::byte> data(state.begin() + offset, state.begin() + offset + size);
            offset += size;
            
            _entries.push_back({index, std::move(data)});
        }
    }
    
    auto entry_count() const -> std::size_t { return _entries.size(); }

private:
    struct entry {
        std::uint64_t index;
        std::vector<std::byte> data;
    };
    std::vector<entry> _entries;
};

} // namespace kythira::examples
