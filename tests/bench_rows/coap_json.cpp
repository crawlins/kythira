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
/// and the libcoap client's I/O thread drains replies only once per 5 ms. At
/// 2 ms the servers receive a fraction of what is sent and the rest queues in
/// libcoap: the standard-tick smoke check commits its PUTs and then times out
/// on a GET queued behind tens of thousands of stale re-sends. Shortening the
/// pacing to 1 ms, as an experiment, cleared the backlog; the spec forbids
/// making that change here. 20 ms keeps the four standard groups inside what
/// the client drains, and is the cadence `multi_raft_coap_test` already uses
/// for the same reason (`doc/multi_raft_performance_comparison.md`, "The CoAP
/// row"). What this proves is what Requirement 17.14 asks -- that a KV cluster
/// over CoAP elects, commits and reads back -- not that it does so at the
/// standard row's cadence, which it does not.
auto smoke_coap_json(const row_observer& observer) -> void {
    auto options = standard_cluster_options();
    options._tick_interval = std::chrono::milliseconds{20};
    detail::smoke<transport>(observer, std::move(options));
}

}  // namespace kythira::testing::rows

#endif  // KYTHIRA_BENCH_HAS_COAP
