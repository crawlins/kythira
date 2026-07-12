#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace kythira::examples {

// Distributed lock state machine with expiration based on log index.
//
// Expiration is measured in applied log entries, not wall-clock time:
// wall-clock reads inside apply() are non-deterministic across Raft
// replicas (clock skew, GC pauses, different machines), so two replicas
// could disagree on whether a lock has expired. `index` is the one value
// every replica is guaranteed to agree on for a given apply() call, so
// timeouts are expressed as "expires after N subsequently applied entries."
class distributed_lock_state_machine {
public:
    // Commands: ACQUIRE <lock_id> <owner> <timeout_entries>, RELEASE <lock_id> <owner>, QUERY
    // <lock_id>
    auto apply(const std::vector<std::byte>& command, std::uint64_t index)
        -> std::vector<std::byte> {
        std::string cmd(reinterpret_cast<const char*>(command.data()),
                        command.size());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)

        if (cmd.starts_with("ACQUIRE ")) {
            auto parts = split(cmd.substr(8));
            if (parts.size() != 3) {
                throw std::invalid_argument("ACQUIRE requires lock_id, owner, timeout_entries");
            }
            return acquire(parts[0], parts[1], std::stoull(parts[2]), index);
        }
        if (cmd.starts_with("RELEASE ")) {
            auto parts = split(cmd.substr(8));
            if (parts.size() != 2) {
                throw std::invalid_argument("RELEASE requires lock_id, owner");
            }
            return release(parts[0], parts[1]);
        }
        if (cmd.starts_with("QUERY ")) {
            return query(cmd.substr(6), index);
        }
        throw std::invalid_argument("Unknown command");
    }

    auto get_state() const -> std::vector<std::byte> {
        std::string state;
        for (const auto& [lock_id, lock] : _locks) {
            state += lock_id + ":" + lock.owner + ":" + std::to_string(lock.expiry_index) + ";";
        }
        return {reinterpret_cast<const std::byte*>(
                    state.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                reinterpret_cast<const std::byte*>(
                    state.data() +
                    state.size())};  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    }

    auto restore_from_snapshot(const std::vector<std::byte>& state, std::uint64_t) -> void {
        _locks.clear();
        std::string s(reinterpret_cast<const char*>(state.data()),
                      state.size());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
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
        std::uint64_t expiry_index;
    };
    std::unordered_map<std::string, lock_info> _locks;

    auto acquire(const std::string& lock_id, const std::string& owner,
                 std::uint64_t timeout_entries, std::uint64_t index) -> std::vector<std::byte> {
        auto& lock = _locks[lock_id];

        if (lock.owner.empty() || index >= lock.expiry_index) {
            lock.owner = owner;
            lock.expiry_index = index + timeout_entries;
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

    auto query(const std::string& lock_id, std::uint64_t index) -> std::vector<std::byte> {
        auto it = _locks.find(lock_id);
        if (it == _locks.end()) {
            return to_bytes("FREE");
        }

        if (index >= it->second.expiry_index) {
            _locks.erase(it);
            return to_bytes("FREE");
        }
        return to_bytes("LOCKED:" + it->second.owner);
    }

    static auto split(const std::string& s, char delim = ' ') -> std::vector<std::string> {
        std::vector<std::string> result;
        std::size_t start = 0, end = std::string::npos;
        while ((end = s.find(delim, start)) != std::string::npos) {
            if (end > start) {
                result.push_back(s.substr(start, end - start));
            }
            start = end + 1;
        }
        if (start < s.size()) {
            result.push_back(s.substr(start));
        }
        return result;
    }

    static auto to_bytes(const std::string& s) -> std::vector<std::byte> {
        return {reinterpret_cast<const std::byte*>(
                    s.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                reinterpret_cast<const std::byte*>(
                    s.data() + s.size())};  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    }
};

}  // namespace kythira::examples
