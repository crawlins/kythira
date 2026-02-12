#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <cstddef>
#include <cstring>
#include <chrono>
#include <stdexcept>

namespace kythira::examples {

// Distributed lock state machine with timeout-based expiration
class distributed_lock_state_machine {
public:
    // Commands: ACQUIRE <lock_id> <owner> <timeout_ms>, RELEASE <lock_id> <owner>, QUERY <lock_id>
    auto apply(const std::vector<std::byte>& command, std::uint64_t) -> std::vector<std::byte> {
        std::string cmd(reinterpret_cast<const char*>(command.data()), command.size());
        
        if (cmd.starts_with("ACQUIRE ")) {
            auto parts = split(cmd.substr(8));
            if (parts.size() != 3) throw std::invalid_argument("ACQUIRE requires lock_id, owner, timeout_ms");
            return acquire(parts[0], parts[1], std::stoull(parts[2]));
        } else if (cmd.starts_with("RELEASE ")) {
            auto parts = split(cmd.substr(8));
            if (parts.size() != 2) throw std::invalid_argument("RELEASE requires lock_id, owner");
            return release(parts[0], parts[1]);
        } else if (cmd.starts_with("QUERY ")) {
            return query(cmd.substr(6));
        }
        throw std::invalid_argument("Unknown command");
    }
    
    auto get_state() const -> std::vector<std::byte> {
        std::string state;
        for (const auto& [lock_id, lock] : _locks) {
            state += lock_id + ":" + lock.owner + ":" + std::to_string(lock.expiry_ms) + ";";
        }
        return {reinterpret_cast<const std::byte*>(state.data()), 
                reinterpret_cast<const std::byte*>(state.data() + state.size())};
    }
    
    auto restore_from_snapshot(const std::vector<std::byte>& state, std::uint64_t) -> void {
        _locks.clear();
        std::string s(reinterpret_cast<const char*>(state.data()), state.size());
        for (const auto& entry : split(s, ';')) {
            auto parts = split(entry, ':');
            if (parts.size() == 3) {
                _locks[parts[0]] = {parts[1], std::stoull(parts[2])};
            }
        }
    }

private:
    struct lock_info {
        std::string owner;
        std::uint64_t expiry_ms;
    };
    std::unordered_map<std::string, lock_info> _locks;
    
    auto acquire(const std::string& lock_id, const std::string& owner, std::uint64_t timeout_ms) -> std::vector<std::byte> {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        auto& lock = _locks[lock_id];
        
        if (lock.owner.empty() || lock.expiry_ms < static_cast<std::uint64_t>(now)) {
            lock.owner = owner;
            lock.expiry_ms = now + timeout_ms * 1000000;
            return to_bytes("OK");
        }
        return to_bytes("LOCKED");
    }
    
    auto release(const std::string& lock_id, const std::string& owner) -> std::vector<std::byte> {
        auto it = _locks.find(lock_id);
        if (it != _locks.end() && it->second.owner == owner) {
            _locks.erase(it);
            return to_bytes("OK");
        }
        return to_bytes("NOT_OWNER");
    }
    
    auto query(const std::string& lock_id) -> std::vector<std::byte> {
        auto it = _locks.find(lock_id);
        if (it == _locks.end()) return to_bytes("FREE");
        
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        if (it->second.expiry_ms < static_cast<std::uint64_t>(now)) {
            _locks.erase(it);
            return to_bytes("FREE");
        }
        return to_bytes("LOCKED:" + it->second.owner);
    }
    
    static auto split(const std::string& s, char delim = ' ') -> std::vector<std::string> {
        std::vector<std::string> result;
        std::size_t start = 0, end;
        while ((end = s.find(delim, start)) != std::string::npos) {
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
