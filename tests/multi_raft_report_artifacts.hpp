// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file multi_raft_report_artifacts.hpp
/// @brief The CSV and JSON a measured row is written out as — Requirement 11
///        of `.kiro/specs/multi-raft-performance/`.
///
/// ### Why a row is written twice
///
/// CSV is what a reader diffs between two runs and what a spreadsheet opens.
/// JSON is what carries the things a flat table cannot: the machine, the
/// per-repetition detail behind a headline, and the `std::optional` fields that
/// must stay absent rather than becoming a zero. Requirement 11.1 asks for
/// both, following the `test_results/*_<timestamp>.*` convention
/// `future_backend_benchmark_report` established.
///
/// ### Self-describing (Requirement 11.5)
///
/// A JSON artifact carries every field Requirements 3, 5 and 6.4 name, so that
/// a result found on disk a year from now needs none of the prose around it:
/// the tier and what that tier forbids, the transport and both serializers, the
/// cluster shape, the load mode *and its controlling parameter*, the routing
/// mode, the tick cadence, the repetition count, the spread, the verdict, the
/// per-cause failure tally, the replication counters, and the machine.
///
/// The two rules the writers hold that a hand-written exporter would not:
///
/// 1. **An absent value is written as `null`, never as `0`.** A p99 that the
///    row did not have the samples for is not a p99 of zero, and a batching
///    ratio over a window in which nothing replicated is not "no batching".
///    Every `std::optional` in `benchmark_result` and `repeated_result` reaches
///    the artifact as `null` — including `headline_ops_per_second`, which is
///    empty below `k_required_repetitions` and must stay that way on disk.
/// 2. **The verdict travels with the number.** Requirement 6.3 gates a row out
///    of a comparison table above ±10% spread, and a CSV read by a script that
///    never saw the printed output is exactly where that gate gets lost. Both
///    artifacts carry `verdict` and `comparable` per row.

#include "multi_raft_kv_workload.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace kythira::testing {

/// @brief One row of a report, with the labels a subset selector filters on.
///
/// `_axis` and `_scenario` are what Requirement 11.6's `--axis` / `--scenario`
/// match against. They are carried beside the result rather than parsed back
/// out of it because a filter that re-derives its key from a formatted string
/// is a filter that breaks when the formatting changes.
struct report_row {
    /// The axis this row belongs to — `transport`, `serializer`, `value-size`,
    /// `concurrency`, `distribution`, `read-taxonomy`, `shard-size`,
    /// `tick-cadence`, `cost-attribution`, `open-loop`.
    std::string _axis;
    /// The individual cell within that axis, e.g. `beast/json/128B/16`.
    std::string _scenario;
    repeated_result _result;
};

/// @brief A row this build cannot run, and why.
///
/// Requirement 13.4: a tier or scenario that is unavailable has to be **named
/// with its reason**, because a silently smaller matrix reads as a completed
/// one. It is in the artifact and not only on the console for the same reason
/// the verdict is: a consumer reading the file a year from now has no other way
/// to tell a build that measured everything from one that measured what it
/// could.
struct dropped_row {
    std::string _axis;
    std::string _scenario;
    std::string _reason;
};

/// @brief `YYYYmmdd_HHMMSS` in local time, matching the existing
///        `test_results/*_<timestamp>.*` filenames exactly.
[[nodiscard]] inline auto make_timestamp() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto as_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&as_time_t, &tm_buf);
    std::ostringstream out;
    out << std::put_time(&tm_buf, "%Y%m%d_%H%M%S");
    return out.str();
}

/// @brief Escape a string for a JSON scalar.
///
/// Control characters are escaped as well as quote and backslash. The machine
/// description reads `/proc` and a mount table, and a CPU model or device name
/// with a stray control byte in it would otherwise produce an artifact no
/// parser accepts — a failure that would surface long after the run that could
/// have been repeated.
[[nodiscard]] inline auto json_escape(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    std::ostringstream esc;
                    esc << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(c);
                    out += esc.str();
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

/// @brief Quote a CSV field, doubling any embedded quote.
///
/// Every string field goes through this, not only the ones expected to contain
/// a comma: `_cxx_flags` routinely does, and a field that is quoted only when
/// somebody remembered is a field that eventually is not.
[[nodiscard]] inline auto csv_field(std::string_view s) -> std::string {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') {
            out += "\"\"";
        } else if (c == '\n' || c == '\r') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    out += '"';
    return out;
}

namespace detail {

/// @brief A `std::optional` numeric as JSON: its value, or `null`.
template<typename T> auto json_optional(const std::optional<T>& v) -> std::string {
    if (!v.has_value()) {
        return "null";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << *v;
    return out.str();
}

inline auto json_optional_ns(const std::optional<std::chrono::nanoseconds>& v) -> std::string {
    return v.has_value() ? std::to_string(v->count()) : std::string{"null"};
}

/// @brief The same, for CSV, where an absent value is an **empty field**.
///
/// Empty rather than `0`, and rather than the string `null`: a spreadsheet
/// reading `0` averages it in, and a reader seeing `null` in a numeric column
/// cannot tell it from a literal. An empty cell is the one spelling every
/// consumer treats as missing.
template<typename T> auto csv_optional(const std::optional<T>& v) -> std::string {
    if (!v.has_value()) {
        return {};
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(4) << *v;
    return out.str();
}

inline auto csv_optional_ns(const std::optional<std::chrono::nanoseconds>& v) -> std::string {
    return v.has_value() ? std::to_string(v->count()) : std::string{};
}

}  // namespace detail

/// @brief Durability barriers per second per host, for the artifact.
///
/// Requirement 3.4 asks a durable row for this and for entries per fsync;
/// Requirement 11.5 asks the artifact to carry every field Requirement 3
/// requires, so both belong here and not only in the suite's printed table.
/// Zero on a memory row and on a `file/buffered` one — and those two zeroes
/// mean different things, which is why `durability` is carried beside them.
inline auto fsyncs_per_second_per_host(const benchmark_result& m) -> double {
    const auto seconds = std::chrono::duration<double>(m._duration).count();
    if (seconds <= 0.0 || m._nodes == 0) {
        return 0.0;
    }
    return static_cast<double>(m._durability_counts._barriers) / seconds /
           static_cast<double>(m._nodes);
}

/// @brief The CSV header, kept beside the writer so the two cannot drift.
///
/// One line per **row**, describing its median run, because a CSV is what a
/// reader diffs and a file with five lines per cell is not diffable. The
/// per-repetition detail is in the JSON, which is where it belongs.
inline constexpr std::string_view k_csv_header =
    "axis,scenario,tier,tier_comparable_externally,transport,wire_serializer,"
    "node_serializer,routing,load_mode,offered_rate_per_second,nodes,groups,value_bytes,"
    "in_flight,tick_interval_ms,durability,durability_barriers,"
    "durability_empty_batches,durability_entries,fsyncs_per_second_per_host,"
    "entries_per_fsync,read_kind,read_consistency,repetitions,warmup_operations,"
    "measured_operations,headline_ops_per_second,min_ops_per_second,max_ops_per_second,"
    "spread_fraction,p50_spread_fraction,governing_spread_fraction,governing_spread_is,"
    "verdict,comparable,p50_ns,p95_ns,p99_ns,"
    "latency_samples,mean_schedule_lag_ns,offered,completed,not_leader,timeout,epoch_mismatch,"
    "merging,other,duration_ns,bytes_returned,bytes_per_operation,bytes_per_second,"
    "append_entries,append_entries_empty,entries,request_vote,install_snapshot,"
    "entries_per_append_entries,rpcs_per_committed_entry,machine_quiet_at_start\n";

/// @brief Write one row's median run as a CSV line.
inline auto write_csv_row(std::ostream& out, const report_row& row,
                          const machine_description& machine) -> void {
    const auto& r = row._result;
    if (r.runs() == 0) {
        return;
    }
    const auto& m = r.median_run();
    out << csv_field(row._axis) << ',' << csv_field(row._scenario) << ','
        << csv_field(to_string(m._tier)) << ','
        << (publishable_as_like_for_like(m._tier) ? "true" : "false") << ','
        << csv_field(m._transport) << ',' << csv_field(m._serializer) << ','
        << csv_field(m._node_serializer) << ',' << csv_field(to_string(m._routing)) << ','
        << csv_field(to_string(m._load)) << ',' << m._offered_rate_per_second << ',' << m._nodes
        << ',' << m._groups << ',' << m._value_bytes << ',' << m._in_flight << ','
        << m._tick_interval.count() << ',' << csv_field(to_string(m._durability)) << ','
        << m._durability_counts._barriers << ',' << m._durability_counts._empty_batches << ','
        << m._durability_counts._entries << ',' << fsyncs_per_second_per_host(m) << ','
        << m._durability_counts.entries_per_barrier() << ','
        << csv_field(m._read_kind.has_value() ? to_string(*m._read_kind)
                                              : std::string_view{"write"})
        << ','
        << csv_field(m._read_kind.has_value() ? consistency_of(*m._read_kind)
                                              : std::string_view{"n/a (write)"})
        << ',' << r.runs() << ',' << r._warmup_operations << ',' << r._measured_operations << ','
        << detail::csv_optional(r.headline_ops_per_second()) << ',' << r.min_ops_per_second() << ','
        << r.max_ops_per_second() << ',' << r.spread() << ',' << r.p50_spread() << ','
        << r.governing_spread() << ','
        << csv_field(m._load == load_mode::open_loop ? "p50" : "throughput") << ','
        << csv_field(to_string(r.verdict())) << ',' << (r.comparable() ? "true" : "false") << ','
        << m._p50.count() << ',' << m._p95.count() << ',' << detail::csv_optional_ns(m._p99) << ','
        << m._tally._completed << ',' << m._mean_schedule_lag.count() << ',' << m._tally._offered
        << ',' << m._tally._completed << ',' << m._tally._not_leader << ',' << m._tally._timeout
        << ',' << m._tally._epoch_mismatch << ',' << m._tally._merging << ',' << m._tally._other
        << ',' << m._duration.count() << ',' << m._bytes_returned << ','
        << detail::csv_optional(m.bytes_per_operation()) << ',' << m.bytes_per_second() << ','
        << m._rpc._append_entries << ',' << m._rpc._append_entries_empty << ',' << m._rpc._entries
        << ',' << m._rpc._request_vote << ',' << m._rpc._install_snapshot << ','
        << detail::csv_optional(m._rpc.entries_per_append_entries()) << ','
        << detail::csv_optional(m.rpcs_per_committed_entry()) << ','
        << (machine._quiet_at_start ? "true" : "false") << '\n';
}

/// @brief Write the whole set of rows as CSV.
inline auto write_csv(const std::filesystem::path& path, const std::vector<report_row>& rows,
                      const machine_description& machine) -> void {
    std::ofstream out(path);
    out << std::fixed << std::setprecision(4);
    out << k_csv_header;
    for (const auto& row : rows) {
        write_csv_row(out, row, machine);
    }
}

/// @brief The machine, as the JSON object Requirement 6.4 lists the fields of.
inline auto write_json_machine(std::ostream& out, const machine_description& machine,
                               std::string_view indent) -> void {
    out << indent << "{\n"
        << indent << "  \"cpu_model\": \"" << json_escape(machine._cpu_model) << "\",\n"
        << indent << "  \"logical_cpus\": " << machine._logical_cpus << ",\n"
        << indent << "  \"memory_bytes\": " << machine._memory_bytes << ",\n"
        << indent << "  \"kernel\": \"" << json_escape(machine._kernel) << "\",\n"
        << indent << "  \"described_path\": \"" << json_escape(machine._described_path) << "\",\n"
        << indent << "  \"filesystem\": \"" << json_escape(machine._filesystem) << "\",\n"
        << indent << "  \"device\": \"" << json_escape(machine._device) << "\",\n"
        << indent << "  \"device_kind\": \"" << json_escape(machine._device_kind) << "\",\n"
        << indent << "  \"compiler\": \"" << json_escape(machine._compiler) << "\",\n"
        << indent << "  \"build_type\": \"" << json_escape(machine._build_type) << "\",\n"
        << indent << "  \"cxx_flags\": \"" << json_escape(machine._cxx_flags) << "\",\n"
        << indent << "  \"sanitizer\": \"" << json_escape(machine._sanitizer) << "\",\n"
        << indent << "  \"future_backend\": \"" << json_escape(machine._future_backend) << "\",\n"
        << indent << "  \"load_average_1m\": " << machine._load_average_1m
        << ",\n"
        // Requirement 6.5: a run taken on a busy machine is not
        // publication-grade, and the artifact says so in the same place as the
        // number rather than in prose the file does not carry.
        << indent << "  \"quiet_at_start\": " << (machine._quiet_at_start ? "true" : "false")
        << ",\n"
        << indent << "  \"publication_grade\": " << (machine._quiet_at_start ? "true" : "false")
        << "\n"
        << indent << "}";
}

/// @brief One repetition, as JSON. Every repetition is written, not only the
///        median.
///
/// Doctrine from task 8: print the per-repetition value of anything you intend
/// to argue from. An artifact that carried only the median run would let a
/// reader compute a spread it cannot check, and would have made task 8's
/// strongest row — 20% throughput spread against a 1% batching spread —
/// unrecoverable from the file.
inline auto write_json_run(std::ostream& out, const benchmark_result& m, std::string_view indent)
    -> void {
    out << indent << "{\n"
        << indent << "  \"ops_per_second\": " << m._ops_per_second << ",\n"
        << indent << "  \"duration_ns\": " << m._duration.count() << ",\n"
        << indent << "  \"p50_ns\": " << m._p50.count() << ",\n"
        << indent << "  \"p95_ns\": " << m._p95.count() << ",\n"
        << indent << "  \"p99_ns\": " << detail::json_optional_ns(m._p99) << ",\n"
        << indent << "  \"latency_samples\": " << m._tally._completed << ",\n"
        << indent << "  \"mean_schedule_lag_ns\": " << m._mean_schedule_lag.count() << ",\n"
        << indent << "  \"tally\": {\n"
        << indent << "    \"offered\": " << m._tally._offered << ",\n"
        << indent << "    \"completed\": " << m._tally._completed << ",\n"
        << indent << "    \"not_leader\": " << m._tally._not_leader << ",\n"
        << indent << "    \"timeout\": " << m._tally._timeout << ",\n"
        << indent << "    \"epoch_mismatch\": " << m._tally._epoch_mismatch << ",\n"
        << indent << "    \"merging\": " << m._tally._merging << ",\n"
        << indent << "    \"other\": " << m._tally._other << "\n"
        << indent << "  },\n"
        << indent << "  \"replication\": {\n"
        << indent << "    \"append_entries\": " << m._rpc._append_entries << ",\n"
        << indent << "    \"append_entries_empty\": " << m._rpc._append_entries_empty << ",\n"
        << indent << "    \"entries\": " << m._rpc._entries << ",\n"
        << indent << "    \"request_vote\": " << m._rpc._request_vote << ",\n"
        << indent << "    \"install_snapshot\": " << m._rpc._install_snapshot << ",\n"
        << indent << "    \"entries_per_append_entries\": "
        << detail::json_optional(m._rpc.entries_per_append_entries()) << ",\n"
        << indent << "    \"rpcs_per_committed_entry\": "
        << detail::json_optional(m.rpcs_per_committed_entry()) << "\n"
        << indent << "  },\n"
        << indent << "  \"bytes_returned\": " << m._bytes_returned << ",\n"
        << indent << "  \"bytes_per_operation\": " << detail::json_optional(m.bytes_per_operation())
        << ",\n"
        << indent << "  \"bytes_per_second\": " << m.bytes_per_second() << "\n"
        << indent << "}";
}

/// @brief One row, as JSON: its configuration, its verdict, and every
///        repetition behind it.
inline auto write_json_row(std::ostream& out, const report_row& row, std::string_view indent)
    -> void {
    const auto& r = row._result;
    const auto& m = r.median_run();
    out << indent << "{\n"
        << indent << "  \"axis\": \"" << json_escape(row._axis) << "\",\n"
        << indent << "  \"scenario\": \"" << json_escape(row._scenario)
        << "\",\n"
        // Requirement 3.1 and 3.3 together: the tier, and what publishing from
        // it is not allowed to claim, on the row rather than in a preamble a
        // consumer of this file will never read.
        << indent << "  \"tier\": \"" << json_escape(to_string(m._tier)) << "\",\n"
        << indent << "  \"like_for_like_comparison_permitted\": "
        << (publishable_as_like_for_like(m._tier) ? "true" : "false") << ",\n"
        << indent << "  \"transport\": \"" << json_escape(m._transport) << "\",\n"
        << indent << "  \"wire_serializer\": \"" << json_escape(m._serializer) << "\",\n"
        << indent << "  \"node_serializer\": \"" << json_escape(m._node_serializer) << "\",\n"
        << indent << "  \"routing\": \"" << json_escape(to_string(m._routing)) << "\",\n"
        << indent << "  \"load_mode\": \"" << json_escape(to_string(m._load)) << "\",\n"
        << indent << "  \"offered_rate_per_second\": " << m._offered_rate_per_second << ",\n"
        << indent << "  \"nodes\": " << m._nodes << ",\n"
        << indent << "  \"groups\": " << m._groups << ",\n"
        << indent << "  \"value_bytes\": " << m._value_bytes << ",\n"
        << indent << "  \"in_flight\": " << m._in_flight << ",\n"
        << indent << "  \"tick_interval_ms\": " << m._tick_interval.count() << ",\n"
        << indent << "  \"durability\": \"" << json_escape(to_string(m._durability)) << "\",\n"
        << indent << "  \"durability_barriers\": " << m._durability_counts._barriers << ",\n"
        << indent << "  \"durability_empty_batches\": " << m._durability_counts._empty_batches
        << ",\n"
        << indent << "  \"durability_entries\": " << m._durability_counts._entries << ",\n"
        << indent << "  \"fsyncs_per_second_per_host\": " << fsyncs_per_second_per_host(m) << ",\n"
        << indent << "  \"entries_per_fsync\": " << m._durability_counts.entries_per_barrier()
        << ",\n"
        << indent << "  \"read_kind\": "
        << (m._read_kind.has_value()
                ? "\"" + std::string{json_escape(to_string(*m._read_kind))} + "\""
                : std::string{"null"})
        << ",\n"
        << indent << "  \"read_consistency\": "
        << (m._read_kind.has_value()
                ? "\"" + std::string{json_escape(consistency_of(*m._read_kind))} + "\""
                : std::string{"null"})
        << ",\n"
        << indent << "  \"repetitions\": " << r.runs() << ",\n"
        << indent << "  \"required_repetitions\": " << k_required_repetitions << ",\n"
        << indent << "  \"warmup_operations_per_run\": " << r._warmup_operations << ",\n"
        << indent << "  \"measured_operations_per_run\": " << r._measured_operations
        << ",\n"
        // Null, not zero, below `k_required_repetitions`. Requirement 6.2 is a
        // property of the type in memory and has to survive serialization.
        << indent
        << "  \"headline_ops_per_second\": " << detail::json_optional(r.headline_ops_per_second())
        << ",\n"
        << indent << "  \"min_ops_per_second\": " << r.min_ops_per_second() << ",\n"
        << indent << "  \"max_ops_per_second\": " << r.max_ops_per_second() << ",\n"
        << indent << "  \"spread_fraction\": " << r.spread() << ",\n"
        << indent << "  \"p50_spread_fraction\": " << r.p50_spread()
        << ",\n"
        // Which of the two the verdict was computed from, so a consumer can see
        // that an open-loop row was judged on latency rather than on a
        // throughput its own schedule had already pinned.
        << indent << "  \"governing_spread_fraction\": " << r.governing_spread() << ",\n"
        << indent << "  \"governing_spread_is\": \""
        << (m._load == load_mode::open_loop ? "p50" : "throughput") << "\",\n"
        << indent << "  \"verdict\": \"" << json_escape(to_string(r.verdict())) << "\",\n"
        << indent << "  \"comparable\": " << (r.comparable() ? "true" : "false") << ",\n"
        << indent << "  \"runs\": [\n";
    for (std::size_t i = 0; i < r._runs.size(); ++i) {
        write_json_run(out, r._runs[i], std::string{indent} + "    ");
        out << (i + 1 == r._runs.size() ? "\n" : ",\n");
    }
    out << indent << "  ]\n" << indent << "}";
}

/// @brief Write the whole set of rows as one self-describing JSON document.
///
/// A document with a machine block and a row array, not a bare array: the
/// machine is a property of the *run*, and repeating it on every row would
/// invite a consumer to concatenate two runs' rows and lose the distinction.
inline auto write_json(const std::filesystem::path& path, const std::vector<report_row>& rows,
                       const machine_description& machine, std::string_view timestamp,
                       const std::vector<dropped_row>& dropped = {}) -> void {
    std::ofstream out(path);
    out << std::fixed << std::setprecision(6);
    out << "{\n"
        << "  \"schema\": \"kythira.multi_raft_performance.v1\",\n"
        << "  \"spec\": \".kiro/specs/multi-raft-performance/\",\n"
        << "  \"timestamp\": \"" << json_escape(timestamp) << "\",\n"
        << "  \"machine\": ";
    write_json_machine(out, machine, "  ");
    out << ",\n  \"rows\": [\n";
    // The separator is decided by what has already been written, not by the
    // loop index: a row with no repetitions is skipped, and an index-based
    // comma would emit a trailing one and produce a file no parser accepts.
    bool first = true;
    for (const auto& row : rows) {
        if (row._result.runs() == 0) {
            continue;
        }
        if (!first) {
            out << ",\n";
        }
        first = false;
        write_json_row(out, row, "    ");
    }
    out << "\n  ],\n  \"dropped_rows\": [\n";
    for (std::size_t i = 0; i < dropped.size(); ++i) {
        out << "    {\"axis\": \"" << json_escape(dropped[i]._axis) << "\", \"scenario\": \""
            << json_escape(dropped[i]._scenario) << "\", \"reason\": \""
            << json_escape(dropped[i]._reason) << "\"}" << (i + 1 == dropped.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
}

}  // namespace kythira::testing
