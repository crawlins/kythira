// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// ExportJournal (.kiro/specs/verifiable-ledger/ task 8, Requirement 10.4):
// the blocks committed in a time range, written to an object-store prefix
// as Ion text, Ion binary or JSON, with a manifest, as a background job
// whose state is in the ledger.
//
// The job's life is two Raft entries around work the leader does outside
// the log:
//
//   {type: ledger_export_start, jobId, start, end, format, bucket, prefix}
//     records the job as in_progress, and with it the tip at that moment:
//     the export covers blocks up to that tip whose timestamps fall in
//     [start, end], so what it holds does not depend on when it runs.
//   export_journal() (leader) writes the objects and the manifest.
//   {type: ledger_export_finish, jobId, status, objects, manifest, message}
//     records completed or failed.
//
// A leader that loses leadership mid-export leaves the job in_progress;
// the new leader runs it again from the start (the objects are rewritten
// whole, so a rerun is idempotent).
//
// Objects: <prefix><jobId>/<first seq>-<last seq>.<ext>, up to
// blocks_per_object blocks each, in block order. Ion binary objects (.10n)
// are an Ion stream of the blocks; Ion text (.ion) and JSON (.json, one
// block per line) have one block per line. The manifest,
// <prefix><jobId>/manifest.json, lists every object with its block range,
// size and SHA-256.

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_segments.hpp>
#include <kythira/ledger/journal_types.hpp>

#include <raft/key_object_store.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::ledger {

enum class export_format : std::uint8_t {
    ion_text,
    ion_binary,
    json
};

inline auto export_format_name(export_format f) -> std::string_view {
    switch (f) {
        case export_format::ion_text:
            return "ion_text";
        case export_format::ion_binary:
            return "ion_binary";
        case export_format::json:
            return "json";
    }
    return "";
}

inline auto export_format_from(std::string_view name) -> std::optional<export_format> {
    for (const auto f : {export_format::ion_text, export_format::ion_binary, export_format::json}) {
        if (export_format_name(f) == name) {
            return f;
        }
    }
    return std::nullopt;
}

inline auto export_extension(export_format f) -> std::string_view {
    switch (f) {
        case export_format::ion_text:
            return "ion";
        case export_format::ion_binary:
            return "10n";
        case export_format::json:
            return "json";
    }
    return "";
}

/// An export job as the ledger records it.
struct export_job {
    std::string job_id;
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;  // inclusive
    export_format format = export_format::ion_text;
    std::string bucket;
    std::string prefix;
    std::optional<std::uint64_t> tip;    // the last block when the job began; none if empty
    std::string status = "in_progress";  // in_progress, completed, failed
    std::uint64_t objects = 0;
    std::string manifest;  // object key of the manifest, once completed
    std::string message;   // why it failed

    [[nodiscard]] auto to_ion() const -> ion_value {
        ion_value v = ion_value::make_struct();
        v.set("jobId", ion_value::string(job_id))
            .set("start", timestamp_value(start_ms))
            .set("end", timestamp_value(end_ms))
            .set("format", ion_value::symbol(export_format_name(format)))
            .set("bucket", ion_value::string(bucket))
            .set("prefix", ion_value::string(prefix))
            .set("status", ion_value::symbol(status))
            .set("objects", ion_value::integer(static_cast<std::int64_t>(objects)))
            .set("manifest", ion_value::string(manifest))
            .set("message", ion_value::string(message));
        if (tip) {
            v.set("tip", ion_value::integer(static_cast<std::int64_t>(*tip)));
        }
        return v;
    }

    static auto from_ion(const ion_value& v) -> export_job {
        namespace jd = journal_detail;
        export_job j;
        j.job_id = jd::text_field(v, "jobId");
        j.start_ms = jd::time_field(v, "start");
        j.end_ms = jd::time_field(v, "end");
        const auto f = export_format_from(jd::text_field(v, "format"));
        if (!f) {
            throw ledger_format_error("unknown export format");
        }
        j.format = *f;
        j.bucket = jd::text_field(v, "bucket");
        j.prefix = jd::text_field(v, "prefix");
        if (v.find("tip") != nullptr) {
            j.tip = jd::uint_field(v, "tip");
        }
        if (v.find("status") != nullptr) {
            j.status = jd::text_field(v, "status");
            j.objects = jd::uint_field(v, "objects");
            j.manifest = jd::text_field(v, "manifest");
            j.message = jd::text_field(v, "message");
        }
        return j;
    }
};

// ── Entries ─────────────────────────────────────────────────────────────────

inline constexpr std::string_view export_start_type = "ledger_export_start";
inline constexpr std::string_view export_finish_type = "ledger_export_finish";

/// Start entry for `job` (its job_id, range, format, bucket and prefix).
inline auto encode_export_start(const export_job& job) -> std::vector<std::uint8_t> {
    ion_value v = ion_value::make_struct();
    v.set("type", ion_value::symbol(export_start_type))
        .set("jobId", ion_value::string(job.job_id))
        .set("start", timestamp_value(job.start_ms))
        .set("end", timestamp_value(job.end_ms))
        .set("format", ion_value::symbol(export_format_name(job.format)))
        .set("bucket", ion_value::string(job.bucket))
        .set("prefix", ion_value::string(job.prefix));
    return v.to_binary();
}

inline auto decode_export_start(const ion_value& v) -> export_job {
    namespace jd = journal_detail;
    export_job j;
    j.job_id = jd::text_field(v, "jobId");
    j.start_ms = jd::time_field(v, "start");
    j.end_ms = jd::time_field(v, "end");
    const auto f = export_format_from(jd::text_field(v, "format"));
    j.bucket = jd::text_field(v, "bucket");
    j.prefix = jd::text_field(v, "prefix");
    if (!valid_id(j.job_id) || !f || j.end_ms < j.start_ms || j.bucket.empty()) {
        throw ledger_format_error("invalid export start entry");
    }
    j.format = *f;
    return j;
}

/// How an export ended.
struct export_outcome {
    std::string job_id;
    bool completed = false;
    std::uint64_t objects = 0;
    std::string manifest;
    std::string message;
};

inline auto encode_export_finish(const export_outcome& o) -> std::vector<std::uint8_t> {
    ion_value v = ion_value::make_struct();
    v.set("type", ion_value::symbol(export_finish_type))
        .set("jobId", ion_value::string(o.job_id))
        .set("status", ion_value::symbol(o.completed ? "completed" : "failed"))
        .set("objects", ion_value::integer(static_cast<std::int64_t>(o.objects)))
        .set("manifest", ion_value::string(o.manifest))
        .set("message", ion_value::string(o.message));
    return v.to_binary();
}

inline auto decode_export_finish(const ion_value& v) -> export_outcome {
    namespace jd = journal_detail;
    export_outcome o;
    o.job_id = jd::text_field(v, "jobId");
    const auto status = jd::text_field(v, "status");
    if (!valid_id(o.job_id) || (status != "completed" && status != "failed")) {
        throw ledger_format_error("invalid export finish entry");
    }
    o.completed = status == "completed";
    o.objects = jd::uint_field(v, "objects");
    o.manifest = jd::text_field(v, "manifest");
    o.message = jd::text_field(v, "message");
    return o;
}

// ── Running a job (leader) ──────────────────────────────────────────────────

inline constexpr std::size_t default_blocks_per_object = 1000;

namespace export_detail {

inline auto encode_block(const block& b, export_format f) -> std::string {
    const ion_value v = b.to_ion();
    switch (f) {
        case export_format::ion_binary: {
            const auto bytes = v.to_binary();
            return {bytes.begin(), bytes.end()};
        }
        case export_format::ion_text:
            return v.to_text() + "\n";
        case export_format::json:
            return v.to_json() + "\n";
    }
    return {};
}

inline auto object_key(const export_job& job, std::uint64_t first, std::uint64_t last)
    -> std::string {
    std::array<char, 48> buf{};
    std::snprintf(buf.data(), buf.size(), "%020llu-%020llu.",
                  static_cast<unsigned long long>(first), static_cast<unsigned long long>(last));
    return job.prefix + job.job_id + "/" + buf.data() + std::string(export_extension(job.format));
}

}  // namespace export_detail

/// Write `job`'s objects and manifest to `store`; returns the outcome to
/// propose in a finish entry. `sm` is anything with blocks_between() and
/// get_block() (basic_ledger_state_machine). Failures (a store error, an
/// unreadable segment) come back as a failed outcome, not an exception.
template<typename StateMachine, kythira::key_object_store Store>
auto export_journal(const StateMachine& sm, const Store& store, const export_job& job,
                    std::size_t blocks_per_object = default_blocks_per_object) -> export_outcome {
    export_outcome out;
    out.job_id = job.job_id;
    try {
        const auto [first, end] = sm.blocks_between(job.start_ms, job.end_ms, job.tip);
        ion_value objects = ion_value::make_list();
        for (std::uint64_t from = first; from < end; from += blocks_per_object) {
            const std::uint64_t last = std::min<std::uint64_t>(from + blocks_per_object, end) - 1;
            std::string body;
            for (std::uint64_t seq = from; seq <= last; ++seq) {
                const auto b = sm.get_block(seq);
                if (!b) {
                    throw ledger_segment_error("block " + std::to_string(seq) + " is missing");
                }
                body += export_detail::encode_block(*b, job.format);
            }
            const auto key = export_detail::object_key(job, from, last);
            (void)store.put_object(job.bucket, key, body);
            ion_value entry = ion_value::make_struct();
            entry.set("key", ion_value::string(key))
                .set("firstBlock", ion_value::integer(static_cast<std::int64_t>(from)))
                .set("lastBlock", ion_value::integer(static_cast<std::int64_t>(last)))
                .set("size", ion_value::integer(static_cast<std::int64_t>(body.size())))
                .set("sha256", ion_value::string(to_hex(sha256_of(body))));
            objects.push_back(std::move(entry));
            ++out.objects;
        }
        ion_value manifest = ion_value::make_struct();
        manifest.set("jobId", ion_value::string(job.job_id))
            .set("start", timestamp_value(job.start_ms))
            .set("end", timestamp_value(job.end_ms))
            .set("format", ion_value::string(export_format_name(job.format)))
            .set("blocks", ion_value::integer(static_cast<std::int64_t>(end - first)))
            .set("objects", std::move(objects));
        if (end > first) {
            manifest.set("firstBlock", ion_value::integer(static_cast<std::int64_t>(first)))
                .set("lastBlock", ion_value::integer(static_cast<std::int64_t>(end - 1)));
        }
        out.manifest = job.prefix + job.job_id + "/manifest.json";
        (void)store.put_object(job.bucket, out.manifest, manifest.to_json());
        out.completed = true;
    } catch (const std::exception& e) {
        out.completed = false;
        out.manifest.clear();
        out.message = e.what();
    }
    return out;
}

}  // namespace kythira::ledger
