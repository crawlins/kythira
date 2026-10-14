// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_json.cpp
/// @brief The one stack instantiation for `libcoap x JSON`, alone in its own
///        translation unit.
///
/// The matrix's CoAP row: `.kiro/specs/multi-raft-performance/`
/// Requirement 17a and `.kiro/specs/coap-transport-multi-raft/` task 14.
/// `tests/multi_raft_bench_row_runners.hpp` says why each pair gets a file to
/// itself.

#include "../multi_raft_bench_row_runners.hpp"

#if defined(KYTHIRA_BENCH_HAS_COAP)

#include "smoke_row.hpp"

#include <raft/json_serializer.hpp>

#include <chrono>
#include <utility>

namespace kythira::testing::rows {
namespace {
using transport = coap_transport<kythira::json_serializer>;
}  // namespace

auto write_coap_json(const write_row_spec& spec, const row_observer& observer) -> repeated_result {
    return throughput_row<transport>(spec, observer);
}

/// The one smoke check not taken at the standard row's 2 ms tick, and the
/// reason is a measured property of the transport rather than a preference.
///
/// `multi_raft` sends one AppendEntries per group per follower on every tick,
/// and when this row was written the libcoap client's I/O thread drained
/// replies only once per 5 ms. At 2 ms the servers received a fraction of what
/// was sent and the rest queued in libcoap: the standard-tick smoke check
/// committed its PUTs and then timed out on a GET queued behind tens of
/// thousands of stale re-sends. 20 ms kept the four standard groups inside
/// what the client drained, and is the cadence `multi_raft_coap_test` uses for
/// the same reason (`doc/multi_raft_performance_comparison.md`, "The CoAP
/// row").
///
/// The client now waits on readiness and drains every ready reply
/// (`.kiro/specs/coap-client-event-driven-io/`), and the group-count sweep
/// commits every write at 8 and 64 groups on 2 ms ("The CoAP row after the
/// event-driven client"). This check stays at 20 ms until it has been run at
/// 2 ms on CI. What it proves is what Requirement 17.14 asks -- that a KV
/// cluster over CoAP elects, commits and reads back.
auto smoke_coap_json(const row_observer& observer) -> void {
    auto options = standard_cluster_options();
    options._tick_interval = std::chrono::milliseconds{20};
    detail::smoke<transport>(observer, std::move(options));
}

}  // namespace kythira::testing::rows

#endif  // KYTHIRA_BENCH_HAS_COAP
