// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file persistence_engine_memory.cpp
/// @brief The persistence rows' engine path without a disk under it:
///        `persistent_kv_state_machine` over `memory_kv_engine`.
///
/// One stack instantiation, alone in its own translation unit, as
/// `tests/multi_raft_bench_row_runners.hpp` requires of every row.

#include "multi_raft_bench_row_runners.hpp"
#include "../multi_raft_persistence_bench_rows.hpp"

#include <kythira/redis/persistent_kv_state_machine.hpp>
#include <raft/memory_kv_engine.hpp>

namespace kythira::testing::rows {

auto persistence_engine_memory(const persistence_row_spec& spec, const row_observer& observer)
    -> repeated_result {
    observer._require(spec._kind == persistence_row_kind::write,
                      "persistence: memory_kv_engine does not survive a restart");
    return persistence_row<
        kythira::persistent_kv_state_machine<kythira::memory_kv_engine, std::uint64_t>>(
        spec, "engine-memory", observer);
}

}  // namespace kythira::testing::rows
