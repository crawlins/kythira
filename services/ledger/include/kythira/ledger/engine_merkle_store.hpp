// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The ledger's Merkle node store over a kv_engine
// (.kiro/specs/verifiable-ledger/ task 4, Requirement 4.1): a
// merkle_node_store whose nodes live in the engine's data column family
// under `<prefix><level>/<index>`, 32 raw bytes each.
//
// Writes are buffered, not written: the state machine appends a block's
// hash to the accumulator while it builds the block, then moves the
// buffered nodes into the same engine batch as the block (take_pending), so
// a crash can never leave a block without its Merkle nodes or the reverse.
// Reads see the buffer first, so the accumulator reads its own writes.

#include <kythira/ledger/ledger_hash.hpp>
#include <kythira/ledger/merkle.hpp>

#include <raft/persistent_state_machine.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace kythira::ledger {

template<kythira::kv_engine Engine> class engine_merkle_store {
public:
    /// Nodes under `prefix` (e.g. "l/<id>/merkle/") in `engine`.
    engine_merkle_store(const Engine& engine, std::string prefix)
        : engine_(&engine), prefix_(std::move(prefix)) {}

    [[nodiscard]] auto get(std::uint32_t level, std::uint64_t index) const
        -> std::optional<hash256> {
        if (const auto it = pending_.find({level, index}); it != pending_.end()) {
            return it->second;
        }
        const auto v = engine_->get(kythira::column_family::data, key(level, index));
        if (!v || v->size() != hash_size) {
            return std::nullopt;
        }
        hash256 out{};
        std::ranges::copy(*v, out.begin());
        return out;
    }

    auto put(std::uint32_t level, std::uint64_t index, const hash256& h) -> void {
        pending_[{level, index}] = h;
    }

    /// Move the buffered nodes into `batch`.
    auto take_pending(kythira::write_batch& batch) -> void {
        for (const auto& [at, h] : pending_) {
            batch.put(kythira::column_family::data, key(at.first, at.second),
                      std::string(h.begin(), h.end()));
        }
        pending_.clear();
    }

    [[nodiscard]] auto pending_count() const -> std::size_t { return pending_.size(); }

    /// The engine key of node (level, index): fixed-width numbers, so the
    /// nodes of one level sort by index.
    [[nodiscard]] auto key(std::uint32_t level, std::uint64_t index) const -> std::string {
        std::array<char, 32> buf{};
        std::snprintf(buf.data(), buf.size(), "%02u/%020llu", static_cast<unsigned>(level),
                      static_cast<unsigned long long>(index));
        return prefix_ + buf.data();
    }

private:
    const Engine* engine_;
    std::string prefix_;
    std::map<std::pair<std::uint32_t, std::uint64_t>, hash256> pending_;
};

}  // namespace kythira::ledger
