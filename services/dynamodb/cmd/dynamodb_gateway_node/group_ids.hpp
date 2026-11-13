// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Group ids for the children of automatic splits (task 24).
//
// The host refuses to split without a cluster-scope id authority, because
// two shards sharing one group id is unrecoverable. The daemon has no
// placement driver, so it does what the core's no_op_shard_placement_driver
// does: each voter owns a reserved range of the id space, and only hands out
// ids from its own range. Ranges are disjoint by construction:
// [(ordinal + 1) << 48, (ordinal + 2) << 48), where `ordinal` is the voter's
// position in the sorted peer list every node is configured with. The
// bootstrap groups (1 and 2) lie below every range.
//
// Unlike that driver, the next id survives a restart. A node that kept its
// data reserves ids a block at a time, recording the block's end in
// `<data dir>/group-ids` and syncing it before it hands out an id from the
// block, so a SIGKILL can waste ids but never reuse one. A memory-engine node
// keeps nothing, so it starts past the current time instead (milliseconds
// times 16, far inside its range for centuries). A memory node that restarts
// has lost its groups' state anyway.

#include "durable_file.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dynamodb_gateway {

class reserved_group_ids {
public:
    static constexpr unsigned k_range_bits = 48;
    static constexpr std::uint64_t k_block = 64;
    static constexpr std::size_t k_max_ordinal = 254;

    /// `ordinal`: this voter's position among the sorted peers. `state`:
    /// the file the reservation is kept in, or none for a memory node.
    reserved_group_ids(std::size_t ordinal, std::optional<std::filesystem::path> state)
        : _state(std::move(state)) {
        if (ordinal > k_max_ordinal) {
            throw std::invalid_argument("reserved_group_ids: more than 255 voters");
        }
        _first = static_cast<std::uint64_t>(ordinal + 1) << k_range_bits;
        _last = static_cast<std::uint64_t>(ordinal + 2) << k_range_bits;
        _next = _first;
        if (_state) {
            if (const auto stored = read_mark()) {
                if (*stored < _first || *stored > _last) {
                    throw std::runtime_error("reserved_group_ids: " + _state->string() +
                                             " holds an id outside this voter's range");
                }
                _next = *stored;
            }
        } else {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
            _next = _first + (static_cast<std::uint64_t>(ms) << 4U);
        }
        _reserved = _state ? _next : _last;
    }

    [[nodiscard]] auto first() const -> std::uint64_t { return _first; }
    [[nodiscard]] auto last() const -> std::uint64_t { return _last; }

    /// Up to `count` fresh ids. Fewer (none) when the range is spent or the
    /// reservation cannot be recorded; the host then abandons the split.
    auto allocate(std::size_t count) -> std::vector<std::uint64_t> {
        const std::lock_guard lock(_mu);
        std::vector<std::uint64_t> out;
        while (out.size() < count && _next < _last) {
            if (_next == _reserved && !reserve()) {
                break;
            }
            out.push_back(_next++);
        }
        return out;
    }

private:
    [[nodiscard]] auto read_mark() const -> std::optional<std::uint64_t> {
        std::ifstream in(*_state);
        std::uint64_t v = 0;
        if (in >> v) {
            return v;
        }
        return std::nullopt;
    }

    /// Records a new block's end, durably, before any id in it is used.
    auto reserve() -> bool {
        const auto end = (_last - _reserved < k_block) ? _last : _reserved + k_block;
        if (!write_durably(*_state, std::to_string(end) + "\n")) {
            return false;
        }
        _reserved = end;
        return true;
    }

    std::optional<std::filesystem::path> _state;
    std::mutex _mu;
    std::uint64_t _first = 0;
    std::uint64_t _last = 0;
    std::uint64_t _next = 0;
    /// Ids below this are recorded as reserved.
    std::uint64_t _reserved = 0;
};

}  // namespace dynamodb_gateway
