// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <raft/fault_injection.hpp>
#include <raft/shard_types.hpp>
#include <raft/splittable_state_machine.hpp>
#include <raft/types.hpp>
#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace kythira {

// Simple in-memory key-value store state machine for testing
// Demonstrates the state_machine concept implementation
template<typename LogIndex = std::uint64_t>
requires log_index<LogIndex>
class test_key_value_state_machine {
public:
    // Command types
    enum class command_type : std::uint8_t {
        get = 0,
        put = 1,
        del = 2
    };

    test_key_value_state_machine() = default;

    // Apply a committed log entry to the state machine
    // Command format: [command_type (1 byte)][key_length (4 bytes)][key][value_length (4
    // bytes)][value] Returns: For GET commands, returns the value; for PUT/DEL, returns empty
    auto apply(const std::vector<std::byte>& command, LogIndex index) -> std::vector<std::byte> {
        fiu_do_on("raft/state_machine/apply",
                  throw std::runtime_error("chaos: state_machine/apply"););
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

        std::uint32_t key_length{};
        std::memcpy(&key_length, &command[offset], sizeof(std::uint32_t));
        offset += sizeof(std::uint32_t);

        if (offset + key_length > command.size()) {
            throw std::invalid_argument("Invalid command format: key length exceeds command size");
        }

        std::string key(reinterpret_cast<const char*>(&command[offset]),
                        key_length);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        offset += key_length;

        // Execute command based on type
        switch (cmd_type) {
            case command_type::put: {
                // Parse value
                if (offset + sizeof(std::uint32_t) > command.size()) {
                    throw std::invalid_argument("Invalid PUT command: missing value length");
                }

                std::uint32_t value_length{};
                std::memcpy(&value_length, &command[offset], sizeof(std::uint32_t));
                offset += sizeof(std::uint32_t);

                if (offset + value_length > command.size()) {
                    throw std::invalid_argument(
                        "Invalid PUT command: value length exceeds command size");
                }

                std::string value(
                    reinterpret_cast<const char*>(&command[offset]),
                    value_length);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                _store[key] = value;

                return {};  // PUT returns empty
            }

            case command_type::get: {
                auto it = _store.find(key);
                if (it == _store.end()) {
                    return {};  // Key not found, return empty
                }

                // Return value as bytes
                const auto& value = it->second;
                std::vector<std::byte> result(value.size());
                std::memcpy(result.data(), value.data(), value.size());
                return result;
            }

            case command_type::del: {
                _store.erase(key);
                return {};  // DEL returns empty
            }

            default:
                throw std::invalid_argument("Unknown command type");
        }
    }

    // Get the current state of the state machine for snapshot creation
    // Format: [num_entries (8 bytes)][entry1_key_len (4 bytes)][entry1_key][entry1_val_len (4
    // bytes)][entry1_val]...
    //
    // Entries are written in key order, because `_store` is ordered. That makes
    // the snapshot bytes a function of the store's CONTENTS rather than of the
    // order the keys were written in, which is what lets two replicas that took
    // the same writes in different orders be compared byte for byte.
    [[nodiscard]] auto get_state() const -> std::vector<std::byte> {
        return serialize_store(_store);
    }

    // Restore the state machine from a snapshot
    auto restore_from_snapshot(const std::vector<std::byte>& snapshot_data, LogIndex index)
        -> void {
        _store.clear();
        _last_applied_index = index;

        if (snapshot_data.empty()) {
            return;  // Empty snapshot is valid (empty state machine)
        }

        std::size_t offset = 0;

        // Read number of entries
        if (offset + sizeof(std::uint64_t) > snapshot_data.size()) {
            throw std::invalid_argument("Invalid snapshot format: missing entry count");
        }

        std::uint64_t num_entries{};
        std::memcpy(&num_entries, snapshot_data.data(), sizeof(std::uint64_t));
        offset += sizeof(std::uint64_t);

        // Read each key-value pair
        for (std::uint64_t i = 0; i < num_entries; ++i) {
            // Read key
            if (offset + sizeof(std::uint32_t) > snapshot_data.size()) {
                throw std::invalid_argument("Invalid snapshot format: missing key length");
            }

            std::uint32_t key_length{};
            std::memcpy(&key_length, snapshot_data.data() + offset, sizeof(std::uint32_t));
            offset += sizeof(std::uint32_t);

            if (offset + key_length > snapshot_data.size()) {
                throw std::invalid_argument(
                    "Invalid snapshot format: key length exceeds data size");
            }

            std::string key(reinterpret_cast<const char*>(
                                snapshot_data.data() +
                                offset),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                            key_length);
            offset += key_length;

            // Read value
            if (offset + sizeof(std::uint32_t) > snapshot_data.size()) {
                throw std::invalid_argument("Invalid snapshot format: missing value length");
            }

            std::uint32_t value_length{};
            std::memcpy(&value_length, snapshot_data.data() + offset, sizeof(std::uint32_t));
            offset += sizeof(std::uint32_t);

            if (offset + value_length > snapshot_data.size()) {
                throw std::invalid_argument(
                    "Invalid snapshot format: value length exceeds data size");
            }

            std::string value(reinterpret_cast<const char*>(
                                  snapshot_data.data() +
                                  offset),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                              value_length);
            offset += value_length;

            _store[key] = value;
        }
    }

    // Helper methods for testing
    [[nodiscard]] auto size() const -> std::size_t { return _store.size(); }

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

    [[nodiscard]] auto get_last_applied_index() const -> LogIndex { return _last_applied_index; }

    // Helper to create PUT command
    static auto make_put_command(const std::string& key, const std::string& value)
        -> std::vector<std::byte> {
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

    // ── splittable_state_machine (multi-Raft, design §6.4) ───────────────────
    //
    // The reference implementation of the extension, and the vehicle its
    // round-trip law is tested against. All six hooks are straightforward here
    // for one reason: `_store` is ordered, so "everything below k" is a
    // contiguous run rather than a filter over an arbitrary iteration order.

    /// @brief Approximate bytes held, counting the same framing `get_state()`
    /// writes so the figure a policy sees matches the snapshot it would produce.
    [[nodiscard]] auto approximate_size_bytes() const -> std::size_t {
        std::size_t total = sizeof(std::uint64_t);
        for (const auto& [key, value] : _store) {
            total += 2 * sizeof(std::uint32_t) + key.size() + value.size();
        }
        return total;
    }

    [[nodiscard]] auto approximate_key_count() const -> std::size_t { return _store.size(); }

    /// @brief Up to `max` evenly spaced split keys, in key order.
    ///
    /// Evenly spaced by *key count* rather than by bytes: this state machine's
    /// values are uniform in the tests that use it, and a size-weighted walk
    /// would be a second, untested implementation of the `SizeChecker` the
    /// default policy already owns.
    ///
    /// Never returns the very first key — cutting there produces an empty left
    /// child, which is a legal shard and a useless one.
    [[nodiscard]] auto suggest_split_keys(std::size_t max) -> std::vector<std::string> {
        std::vector<std::string> out;
        if (max == 0 || _store.size() < 2) {
            return out;
        }
        const std::size_t wanted = std::min(max, _store.size() - 1);
        const std::size_t stride = _store.size() / (wanted + 1);
        if (stride == 0) {
            return out;
        }
        std::size_t index = 0;
        std::size_t taken = 0;
        for (const auto& [key, value] : _store) {
            ++index;
            if (taken < wanted && index % stride == 0 && index < _store.size()) {
                out.push_back(key);
                ++taken;
            }
        }
        return out;
    }

    /// @brief The application's veto. Permissive by default.
    ///
    /// A real state machine refuses to cut between a row and its index entries;
    /// this one has no such structure, so the veto is a settable predicate that
    /// exists to make the host's fallback chain testable. Without it the
    /// "vetoed key falls back to the state machine's suggestion" path would
    /// have no way to be exercised.
    [[nodiscard]] auto can_split_at(const std::string& key) const -> bool {
        return _split_veto ? !_split_veto(key) : true;
    }

    /// @brief Install a veto predicate. Test hook; see `can_split_at`.
    auto set_split_veto(std::function<bool(const std::string&)> veto) -> void {
        _split_veto = std::move(veto);
    }

    /// @brief Cut the store at `keys`, returning `keys.size() + 1` blobs.
    ///
    /// Deterministic: the store is ordered and each blob is written in the same
    /// `get_state()` format, so every replica produces byte-identical children.
    /// Keys that are out of order or duplicated are sorted and deduplicated
    /// first, so a caller's ordering mistake cannot silently produce
    /// overlapping children.
    [[nodiscard]] auto split_state(const std::vector<std::string>& keys)
        -> std::vector<std::vector<std::byte>> {
        std::vector<std::string> cuts = keys;
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

        std::vector<std::vector<std::byte>> out;
        out.reserve(cuts.size() + 1);

        auto it = _store.begin();
        for (const auto& cut : cuts) {
            std::map<std::string, std::string> part;
            while (it != _store.end() && it->first < cut) {
                part.insert(*it);
                ++it;
            }
            out.push_back(serialize_store(part));
        }
        std::map<std::string, std::string> tail;
        while (it != _store.end()) {
            tail.insert(*it);
            ++it;
        }
        out.push_back(serialize_store(tail));
        return out;
    }

    /// @brief Take on another shard's state.
    ///
    /// `range` is unused here and that is not an oversight: this state machine
    /// keeps no per-range metadata, so there is nothing for it to update. A
    /// state machine that maintains range-scoped structure — a secondary index
    /// bounded by the shard's range, say — needs it, which is why it is in the
    /// concept.
    auto absorb(const std::vector<std::byte>& other_state, const shard_range<std::string>& range)
        -> void {
        (void)range;
        auto incoming = deserialize_store(other_state);
        for (auto& [key, value] : incoming) {
            _store[key] = std::move(value);
        }
    }

    /// @brief Write a store in exactly `get_state()`'s format.
    ///
    /// Shared with `get_state()` rather than reimplemented, because the
    /// round-trip law compares `split_state` output against `get_state()`
    /// output and two encoders that drifted apart would break it silently.
    ///
    /// Public alongside its inverse below, because the sharding invariant tests
    /// have to read back what `get_state()` produced. A test that re-implemented
    /// this format would be checking its own copy of the encoder against the
    /// real one, which is precisely the drift this pair exists to prevent.
    [[nodiscard]] static auto serialize_store(const std::map<std::string, std::string>& store)
        -> std::vector<std::byte> {
        std::vector<std::byte> state;
        std::uint64_t num_entries = store.size();
        std::size_t offset = 0;
        state.resize(sizeof(std::uint64_t));
        std::memcpy(state.data(), &num_entries, sizeof(std::uint64_t));
        offset += sizeof(std::uint64_t);

        for (const auto& [key, value] : store) {
            auto key_length = static_cast<std::uint32_t>(key.size());
            state.resize(state.size() + sizeof(std::uint32_t) + key.size());
            std::memcpy(state.data() + offset, &key_length, sizeof(std::uint32_t));
            offset += sizeof(std::uint32_t);
            std::memcpy(state.data() + offset, key.data(), key.size());
            offset += key.size();

            auto value_length = static_cast<std::uint32_t>(value.size());
            state.resize(state.size() + sizeof(std::uint32_t) + value.size());
            std::memcpy(state.data() + offset, &value_length, sizeof(std::uint32_t));
            offset += sizeof(std::uint32_t);
            std::memcpy(state.data() + offset, value.data(), value.size());
            offset += value.size();
        }
        return state;
    }

    [[nodiscard]] static auto deserialize_store(const std::vector<std::byte>& blob)
        -> std::map<std::string, std::string> {
        std::map<std::string, std::string> out;
        if (blob.empty()) {
            return out;
        }
        std::size_t offset = 0;
        if (offset + sizeof(std::uint64_t) > blob.size()) {
            throw std::invalid_argument("Invalid state blob: missing entry count");
        }
        std::uint64_t num_entries{};
        std::memcpy(&num_entries, blob.data(), sizeof(std::uint64_t));
        offset += sizeof(std::uint64_t);

        for (std::uint64_t i = 0; i < num_entries; ++i) {
            if (offset + sizeof(std::uint32_t) > blob.size()) {
                throw std::invalid_argument("Invalid state blob: missing key length");
            }
            std::uint32_t key_length{};
            std::memcpy(&key_length, blob.data() + offset, sizeof(std::uint32_t));
            offset += sizeof(std::uint32_t);
            if (offset + key_length > blob.size()) {
                throw std::invalid_argument("Invalid state blob: key length exceeds data size");
            }
            std::string key(reinterpret_cast<const char*>(blob.data() + offset),  // NOLINT
                            key_length);
            offset += key_length;

            if (offset + sizeof(std::uint32_t) > blob.size()) {
                throw std::invalid_argument("Invalid state blob: missing value length");
            }
            std::uint32_t value_length{};
            std::memcpy(&value_length, blob.data() + offset, sizeof(std::uint32_t));
            offset += sizeof(std::uint32_t);
            if (offset + value_length > blob.size()) {
                throw std::invalid_argument("Invalid state blob: value length exceeds data size");
            }
            std::string value(reinterpret_cast<const char*>(blob.data() + offset),  // NOLINT
                              value_length);
            offset += value_length;

            out.emplace(std::move(key), std::move(value));
        }
        return out;
    }

private:
    /// Ordered, not hashed. Two reasons, and the second is the load-bearing
    /// one: `split_state` needs "everything below k" to be a contiguous run,
    /// and `get_state()` has to produce byte-identical output on every replica
    /// regardless of the order the keys were written in. An unordered container
    /// gives neither, and the divergence it would cause is invisible at the
    /// Raft level — the logs would still match.
    std::map<std::string, std::string> _store;
    std::function<bool(const std::string&)> _split_veto;
    LogIndex _last_applied_index{0};
};

// The extension is structural: this is the reference implementation, and the
// assertion is here so that a change to either the concept or the class that
// broke the pairing fails at the definition rather than at some distant use.
static_assert(splittable_state_machine<test_key_value_state_machine<std::uint64_t>, std::string>,
              "test_key_value_state_machine must satisfy splittable_state_machine");

// Validate that test_key_value_state_machine satisfies the state_machine concept
static_assert(state_machine<test_key_value_state_machine<std::uint64_t>, std::uint64_t>,
              "test_key_value_state_machine must satisfy state_machine concept");

}  // namespace kythira
