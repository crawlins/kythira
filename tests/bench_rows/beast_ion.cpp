// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file beast_ion.cpp
/// @brief The one stack instantiation for `Boost.Beast x Amazon Ion`, alone in
///        its own translation unit.
///
/// One of nine. `tests/multi_raft_bench_row_runners.hpp` says why each pair
/// gets a file to itself, and what it cost when they shared one.

#include "../multi_raft_bench_row_runners.hpp"

#if defined(KYTHIRA_BENCH_HAS_BEAST) && defined(KYTHIRA_BENCH_HAS_ION)

#include <raft/ion_serializer.hpp>

namespace kythira::testing::rows {

// Left to the default-constructed encoding (binary) rather than naming a media
// type: Ion's `media_type()` is the one in this suite that depends on instance
// state, and `run_put_workload` reads the label off the serializer, so the row
// says which one actually went on the wire.
auto write_beast_ion(const write_row_spec& spec, const row_observer& observer) -> repeated_result {
    return throughput_row<beast_http_transport<kythira::ion_serializer>>(spec, observer);
}

}  // namespace kythira::testing::rows

#endif
