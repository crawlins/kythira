// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file persistence_memory.cpp
/// @brief The persistence rows' baseline: `redis_kv_state_machine`, every
///        value in RAM, over the same file-backed Raft log as the engine arms.
///
/// One stack instantiation, alone in its own translation unit, as
/// `tests/multi_raft_bench_row_runners.hpp` requires of every row.

#include "multi_raft_bench_row_runners.hpp"
#include "../multi_raft_persistence_bench_rows.hpp"

#include <kythira/redis/redis_kv_state_machine.hpp>

namespace kythira::testing::rows {

auto persistence_memory(const persistence_row_spec& spec, const row_observer& observer)
    -> repeated_result {
    return persistence_row<kythira::redis_kv_state_machine<std::uint64_t>>(spec, "memory",
                                                                           observer);
}

}  // namespace kythira::testing::rows
