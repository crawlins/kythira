// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file persistence_engine_rocksdb.cpp
/// @brief The persistence rows' gateway engine node:
///        `persistent_kv_state_machine` over `rocksdb_kv_engine`.
///
/// One stack instantiation, alone in its own translation unit, as
/// `tests/multi_raft_bench_row_runners.hpp` requires of every row. Built only
/// where `raft_rocksdb_engine` exists.

#include "multi_raft_bench_row_runners.hpp"
#include "../multi_raft_persistence_bench_rows.hpp"

#include <kythira/redis/persistent_kv_state_machine.hpp>
#include <raft/rocksdb_kv_engine.hpp>

namespace kythira::testing::rows {

auto persistence_engine_rocksdb(const persistence_row_spec& spec, const row_observer& observer)
    -> repeated_result {
    return persistence_row<
        kythira::persistent_kv_state_machine<kythira::rocksdb_kv_engine, std::uint64_t>>(
        spec, "engine-rocksdb", observer);
}

}  // namespace kythira::testing::rows
