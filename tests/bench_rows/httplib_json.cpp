// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file httplib_json.cpp
/// @brief The one stack instantiation for `cpp-httplib x JSON`, alone in
///        its own translation unit.
///
/// One of nine. `tests/multi_raft_bench_row_runners.hpp` says why each pair
/// gets a file to itself, and what it cost when they shared one.

#include "../multi_raft_bench_row_runners.hpp"
#include "smoke_row.hpp"

#include <raft/json_serializer.hpp>

namespace kythira::testing::rows {
namespace {
using transport = cpp_httplib_transport<kythira::json_serializer>;
}  // namespace

auto write_httplib_json(const write_row_spec& spec, const row_observer& observer)
    -> repeated_result {
    return throughput_row<transport>(spec, observer);
}

auto smoke_httplib_json(const row_observer& observer) -> void {
    detail::smoke<transport>(observer);
}

}  // namespace kythira::testing::rows
