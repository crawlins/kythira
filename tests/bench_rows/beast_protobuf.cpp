// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file beast_protobuf.cpp
/// @brief The one stack instantiation for `Boost.Beast x Protocol Buffers`, alone in
///        its own translation unit.
///
/// One of nine. `tests/multi_raft_bench_row_runners.hpp` says why each pair
/// gets a file to itself, and what it cost when they shared one.

#include "../multi_raft_bench_row_runners.hpp"

#if defined(KYTHIRA_BENCH_HAS_BEAST) && defined(KYTHIRA_BENCH_HAS_PROTOBUF)

#include <raft/protobuf_serializer.hpp>

namespace kythira::testing::rows {

auto write_beast_protobuf(const write_row_spec& spec, const row_observer& observer)
    -> repeated_result {
    return throughput_row<beast_http_transport<kythira::protobuf_serializer>>(spec, observer);
}

}  // namespace kythira::testing::rows

#endif
