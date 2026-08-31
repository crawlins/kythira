// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file multi_raft_row_report.hpp
/// @brief How one measured row is written down, for every consumer that
///        produces one.
///
/// Extracted from `multi_raft_http_benchmark_test.cpp` by
/// `.kiro/specs/multi-raft-host-binary/`, whose Requirement 3.4 asks that the
/// out-of-process driver emit the same fields a Tier B row carries so the two
/// are readable side by side. Two printers would satisfy that on the day they
/// were written and drift the week after; one printer satisfies it structurally.
///
/// Nothing here measures anything. It formats a `repeated_result` that
/// somebody else produced, which is why it is a header and not a library.

#include "multi_raft_kv_workload.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>

namespace kythira::testing {

namespace detail {
/// A duration in microseconds, for a table whose other columns are.
///
/// Deliberately not named `us`: `multi_raft_benchmark_rows.hpp` already has one
/// in this namespace, and a consumer including both headers would get an
/// ambiguity for a two-line helper.
[[nodiscard]] inline auto to_us(std::chrono::nanoseconds d) -> double {
    return std::chrono::duration<double, std::micro>(d).count();
}
}  // namespace detail

/// @brief The replication cost of one measured window, in the two ratios
///        Hypotheses H1 and H2 are stated in.
///
/// Printed for the *median run* rather than summed across repetitions: the
/// headline names a real run (Requirement 6.2), and a batching factor averaged
/// over five clusters would not describe the run the headline came from.
///
/// Both ratios are `std::optional` at source and print `n/a` with the reason
/// rather than a zero — 0/0 entries per AppendEntries is "nothing replicated",
/// which is a different statement from "no batching".
///
/// **A row whose measuring process could not see the cluster's counters says
/// so and prints nothing else.** That is every out-of-process row: the host is
/// deliberately uninstrumented, so a zero here would report "no replication
/// happened" for a cluster that replicated normally.
[[nodiscard]] inline auto replication_cost(const benchmark_result& row) -> std::string {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    if (!row._internal_counters) {
        return "replication: NOT MEASURED at this tier — the host process is deliberately "
               "uninstrumented (Requirement 8.2 keeps measurement counters out of the measured "
               "process), so these columns are absent rather than zero";
    }
    out << "replication (median run): " << row._rpc._append_entries << " AppendEntries ("
        << row._rpc._append_entries_empty << " empty) carrying " << row._rpc._entries
        << " entries, " << row._rpc._request_vote << " RequestVote, " << row._rpc._install_snapshot
        << " InstallSnapshot";
    out << "\n      H1 entries/AppendEntries: ";
    if (const auto batching = row._rpc.entries_per_append_entries()) {
        out << *batching << " (over the " << row._rpc.carrying() << " that carried anything)";
    } else {
        out << "n/a — no AppendEntries carried an entry";
    }
    out << "\n      H2 RPCs/committed entry:  ";
    if (const auto cost = row.rpcs_per_committed_entry()) {
        out << *cost << " (" << row._rpc.total_rpcs() << " RPCs / " << row._tally._completed
            << " commits)";
    } else {
        out << "n/a — nothing committed";
    }
    return out.str();
}

/// @brief One row, in the shape the comparison document wants: a headline that
///        exists only when enough repetitions stand behind it, the spread
///        beside it, and the verdict that decides whether the row may be
///        compared to anything.
///
/// @param quiet_machine Whether the machine was quiet when the run started. The
///        caller supplies it because the machine is described once per process
///        and this header measures nothing.
[[nodiscard]] inline auto describe_row(const repeated_result& row, bool quiet_machine)
    -> std::string {
    if (row.runs() == 0) {
        return "  (no runs)";
    }
    const auto& median = row.median_run();
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    out << "  " << median._transport << " / " << median._serializer << " / " << median._scenario
        << ": nodes=" << median._nodes << " groups=" << median._groups
        << " value=" << median._value_bytes << "B in_flight=" << median._in_flight
        << " tick=" << median._tick_interval.count()
        << "ms"
        // Requirement 3.1: exactly one tier per result, and 3.3: say so at the
        // point of the number rather than once in a preamble a quoted row
        // leaves behind.
        << "\n      tier:     " << to_string(median._tier)
        << (publishable_as_like_for_like(median._tier)
                ? " — a like-for-like comparison with an external number is permitted at this "
                  "tier (Requirement 3.3)"
                : " — NOT a like-for-like comparison with any external number (Requirement 3.3)")
        << "\n      routing:  " << to_string(median._routing) << "\n      headline: ";
    if (const auto headline = row.headline_ops_per_second()) {
        out << *headline << " ops/sec (median of " << row.runs() << " runs)";
    } else {
        out << "NONE — " << row.runs() << " run(s), " << k_required_repetitions
            << " required before a headline exists";
    }
    out << " | min " << row.min_ops_per_second() << " max " << row.max_ops_per_second()
        << " | spread " << std::setprecision(1) << (row.spread() * 100.0) << "% of median"
        << " (p50 spread " << (row.p50_spread() * 100.0) << "%)"
        << "\n      verdict:  " << to_string(row.verdict())
        << (median._load == load_mode::open_loop
                ? " (judged on the p50 spread; in open loop the offered rate is an input, so its "
                  "spread is a tautology)"
                : " (judged on the throughput spread)");
    if (!row.comparable()) {
        out << " — MUST NOT ENTER A COMPARISON TABLE (Requirement 6.3)";
    }
    if (!quiet_machine) {
        out << "; machine was not quiet at start";
    }
    out << "\n      median run: p50=" << detail::to_us(median._p50)
        << "us p95=" << detail::to_us(median._p95) << "us";
    if (median._p99.has_value()) {
        out << " p99=" << detail::to_us(*median._p99) << "us";
    } else {
        out << " p99=n/a (" << median._tally._completed << " samples)";
    }
    out << "\n      counts:   " << row._warmup_operations
        << " warm-up operations discarded per run, " << row._measured_operations
        << " offered per measured run (Requirement 6.1)";
    if (median._read_kind.has_value()) {
        out << "\n      read kind: " << to_string(*median._read_kind)
            << "\n      consistency: " << consistency_of(*median._read_kind)
            << "\n      bytes:    " << median._bytes_returned << " returned in the median run, "
            << median.bytes_per_second() / (1024.0 * 1024.0) << " MiB/sec";
        if (const auto per_op = median.bytes_per_operation()) {
            out << ", " << *per_op << " bytes/operation";
        } else {
            out << ", bytes/operation n/a — nothing completed";
        }
    }
    out << "\n      " << replication_cost(median);
    return out.str();
}

}  // namespace kythira::testing
