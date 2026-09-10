// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file fabric.cpp
/// @brief The one stack instantiation for the in-process fabric, which has no
///        wire serializer, alone in its own translation unit.
///
/// One of nine. `tests/multi_raft_bench_row_runners.hpp` says why each pair
/// gets a file to itself, and what it cost when they shared one.

#include "../multi_raft_bench_row_runners.hpp"

namespace kythira::testing::rows {

auto write_fabric(const write_row_spec& spec, const row_observer& observer) -> repeated_result {
    return throughput_row<fabric_transport>(spec, observer);
}

}  // namespace kythira::testing::rows
