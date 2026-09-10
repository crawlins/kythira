// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file proxygen_json.cpp
/// @brief The one stack instantiation for `Proxygen x JSON`, alone in
///        its own translation unit.
///
/// One of nine. `tests/multi_raft_bench_row_runners.hpp` says why each pair
/// gets a file to itself, and what it cost when they shared one.

#include "../multi_raft_bench_row_runners.hpp"

#if defined(KYTHIRA_BENCH_HAS_PROXYGEN)

#include "smoke_row.hpp"

#include <raft/json_serializer.hpp>

namespace kythira::testing::rows {
namespace {
using transport = proxygen_http_transport<kythira::json_serializer>;
}  // namespace

auto write_proxygen_json(const write_row_spec& spec, const row_observer& observer)
    -> repeated_result {
    return throughput_row<transport>(spec, observer);
}

auto smoke_proxygen_json(const row_observer& observer) -> void {
    detail::smoke<transport>(observer);
}

}  // namespace kythira::testing::rows

#endif
