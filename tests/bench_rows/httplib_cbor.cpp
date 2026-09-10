// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file httplib_cbor.cpp
/// @brief The one stack instantiation for `cpp-httplib x CBOR`, alone in
///        its own translation unit.
///
/// One of nine. `tests/multi_raft_bench_row_runners.hpp` says why each pair
/// gets a file to itself, and what it cost when they shared one.

#include "../multi_raft_bench_row_runners.hpp"

#include <raft/cbor_serializer.hpp>

namespace kythira::testing::rows {

auto write_httplib_cbor(const write_row_spec& spec, const row_observer& observer)
    -> repeated_result {
    return throughput_row<cpp_httplib_transport<kythira::cbor_serializer>>(spec, observer);
}

}  // namespace kythira::testing::rows
