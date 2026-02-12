#pragma once

#include <cstdint>
#include <vector>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>

namespace kythira::examples {

// Single-value register with versioning
class register_state_machine {
public:
    // Commands: WRITE <value>, READ, CAS <expected> <new>
    auto apply(const std::vector<std::byte>& command, std::uint64_t) -> std::vector<std::byte> {
        std::string cmd(reinterpret_cast<const char*>(command.data()), command.size());
        
        if (cmd.starts_with("WRITE ")) {
            _value = cmd.substr(6);
            ++_version;
            return to_bytes("OK");
        } else if (cmd == "READ") {
            return to_bytes(_value);
        } else if (cmd.starts_with("CAS ")) {
            auto parts = split(cmd.substr(4));
            if (parts.size() == 2 && _value == parts[0]) {
                _value = parts[1];
                ++_version;
                return to_bytes("OK");
            }
            return to_bytes("FAILED");
        }
        throw std::invalid_argument("Unknown command type");
    }
    
    auto get_state() const -> std::vector<std::byte> {
        std::string state = std::to_string(_version) + ":" + _value;
        return {reinterpret_cast<const std::byte*>(state.data()),
                reinterpret_cast<const std::byte*>(state.data() + state.size())};
    }
    
    auto restore_from_snapshot(const std::vector<std::byte>& state, std::uint64_t) -> void {
        std::string s(reinterpret_cast<const char*>(state.data()), state.size());
        auto pos = s.find(':');
        if (pos != std::string::npos) {
            _version = std::stoull(s.substr(0, pos));
            _value = s.substr(pos + 1);
        }
    }

private:
    std::string _value;
    std::uint64_t _version = 0;
    
    static auto split(const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> result;
        std::size_t start = 0, end;
        while ((end = s.find(' ', start)) != std::string::npos) {
            if (end > start) result.push_back(s.substr(start, end - start));
            start = end + 1;
        }
        if (start < s.size()) result.push_back(s.substr(start));
        return result;
    }
    
    static auto to_bytes(const std::string& s) -> std::vector<std::byte> {
        return {reinterpret_cast<const std::byte*>(s.data()),
                reinterpret_cast<const std::byte*>(s.data() + s.size())};
    }
};

} // namespace kythira::examples
