// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file txn_fabric.cpp
/// @brief The tier-1 transaction rows: `redis_kv_state_machine` under the
///        `mvcc::off` wrapper, at `read_committed`, on the in-process fabric.
///
/// One stack instantiation, alone in its own translation unit, as
/// `tests/multi_raft_bench_row_runners.hpp` requires of every row.

#include "../multi_raft_bench_row_runners.hpp"
#include "../multi_raft_txn_bench_rows.hpp"

#include <raft/redis_kv_state_machine.hpp>

namespace kythira::testing::rows {

auto txn_fabric(const txn_row_spec& spec, const row_observer& observer) -> repeated_result {
    return txn_row<kythira::redis_kv_state_machine<std::uint64_t>>(spec, observer);
}

}  // namespace kythira::testing::rows
