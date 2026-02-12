#pragma once

#include <cstdint>
#include <vector>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace kythira::examples {

// Simple atomic counter state machine
class counter_state_machine {
public:
    // Commands: INC, DEC, RESET, GET
    auto apply(const std::vector<std::byte>& command, std::uint64_t) -> std::vector<std::byte> {
        std::string cmd(reinterpret_cast<const char*>(command.data()), command.size());
        
        if (cmd == "INC") {
            ++_value;
        } else if (cmd == "DEC") {
            --_value;
        } else if (cmd == "RESET") {
            _value = 0;
        } else if (cmd == "GET") {
            // Return current value
        } else {
            throw std::invalid_argument("Unknown command type");
        }
        
        std::string result = std::to_string(_value);
        return {reinterpret_cast<const std::byte*>(result.data()),
                reinterpret_cast<const std::byte*>(result.data() + result.size())};
    }
    
    auto get_state() const -> std::vector<std::byte> {
        return {reinterpret_cast<const std::byte*>(&_value),
                reinterpret_cast<const std::byte*>(&_value) + sizeof(_value)};
    }
    
    auto restore_from_snapshot(const std::vector<std::byte>& state, std::uint64_t) -> void {
        if (state.size() >= sizeof(_value)) {
            std::memcpy(&_value, state.data(), sizeof(_value));
        }
    }
    
    auto get_value() const -> std::int64_t { return _value; }

private:
    std::int64_t _value = 0;
};

} // namespace kythira::examples
