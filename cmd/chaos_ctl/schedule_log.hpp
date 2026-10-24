// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The schedule log (.kiro/specs/cairn-chaos-testing/ Requirement 3.3,
// design §4): one JSONL file per run, the run's artifact. Each tool call is
// two lines, sharing a `seq`:
//
//   {"seq":3,"status":"started","tool":"fault.kill","args":{...},
//    "wall":"...","mono_ms":..., "seed":..., "kythira_commit":"...",
//    "image_id":"..."}
//   {"seq":3,"status":"ok","result":{...},"violations":[...],
//    "wall":"...","mono_ms":...}
//
// The first is written and flushed before the call executes and the second
// after, so a call that crashes or hangs chaos_ctl is still on record as
// `started` with no outcome. That is the design's "append, then update the
// line": appending a second line keeps the file append-only, so a reader
// never sees a half-rewritten line, and fold() joins the two.
//
// `mono_ms` is CLOCK_MONOTONIC in milliseconds, which every process on one
// host shares, so offsets between calls made by different chaos_ctl
// processes are real gaps. Replay honours them.

#include <boost/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace chaos_ctl {

/// One tool call as the log records it, both lines folded together.
struct call_record {
    std::int64_t seq{0};
    std::string tool;
    boost::json::object args;
    std::int64_t started_mono_ms{0};
    /// "started" if no outcome line was written, else "ok", "error" or
    /// "unsupported".
    std::string status{"started"};
    boost::json::value result;
    std::string error;
    /// Every violation record the call produced (post-step Raft safety and
    /// any checker the call ran), each with an "invariant" key.
    boost::json::array violations;
    boost::json::object started_line;
    std::optional<boost::json::object> done_line;

    /// The invariants this call saw violated.
    [[nodiscard]] auto violated() const -> std::set<std::string> {
        std::set<std::string> out;
        for (const auto& v : violations) {
            if (const auto* o = v.if_object(); o != nullptr) {
                if (const auto* inv = o->if_contains("invariant");
                    inv != nullptr && inv->is_string()) {
                    out.insert(std::string(inv->as_string()));
                }
            }
        }
        return out;
    }
};

/// Appends lines to a schedule log, flushing each one.
class schedule_log {
public:
    explicit schedule_log(std::string path) : _path(std::move(path)) {}

    void append(const boost::json::object& line) const {
        std::ofstream out(_path, std::ios::app);
        if (!out) {
            throw std::runtime_error("schedule log: cannot open " + _path);
        }
        out << boost::json::serialize(line) << '\n' << std::flush;
        if (!out) {
            throw std::runtime_error("schedule log: write to " + _path + " failed");
        }
    }

    [[nodiscard]] auto path() const -> const std::string& { return _path; }

private:
    std::string _path;
};

/// The lines of a schedule log, in file order. A last line without its
/// newline is a write cut short and is dropped.
[[nodiscard]] inline auto read_lines(const std::string& path) -> std::vector<boost::json::object> {
    std::ifstream in(path);
    std::vector<boost::json::object> out;
    if (!in) {
        return out;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            break;
        }
        const auto line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) {
            continue;
        }
        out.push_back(boost::json::parse(line).as_object());
    }
    return out;
}

/// Joins each call's started line with its outcome line, in seq order.
[[nodiscard]] inline auto fold(const std::vector<boost::json::object>& lines)
    -> std::vector<call_record> {
    std::map<std::int64_t, call_record> calls;
    for (const auto& line : lines) {
        const auto seq = line.at("seq").to_number<std::int64_t>();
        const auto status = std::string(line.at("status").as_string());
        if (status == "started") {
            call_record r;
            r.seq = seq;
            r.tool = std::string(line.at("tool").as_string());
            r.args = line.at("args").as_object();
            r.started_mono_ms = line.at("mono_ms").to_number<std::int64_t>();
            r.started_line = line;
            calls[seq] = std::move(r);
            continue;
        }
        auto it = calls.find(seq);
        if (it == calls.end()) {
            throw std::runtime_error("schedule log: outcome for seq " + std::to_string(seq) +
                                     " has no started line");
        }
        auto& r = it->second;
        r.status = status;
        if (const auto* v = line.if_contains("result"); v != nullptr) {
            r.result = *v;
        }
        if (const auto* v = line.if_contains("error"); v != nullptr && v->is_string()) {
            r.error = std::string(v->as_string());
        }
        if (const auto* v = line.if_contains("violations"); v != nullptr && v->is_array()) {
            r.violations = v->as_array();
        }
        r.done_line = line;
    }
    std::vector<call_record> out;
    out.reserve(calls.size());
    for (auto& [seq, r] : calls) {
        out.push_back(std::move(r));
    }
    return out;
}

[[nodiscard]] inline auto read_calls(const std::string& path) -> std::vector<call_record> {
    return fold(read_lines(path));
}

/// The next seq for a log: one past the highest so far.
[[nodiscard]] inline auto next_seq(const std::vector<boost::json::object>& lines) -> std::int64_t {
    std::int64_t top = 0;
    for (const auto& l : lines) {
        top = std::max(top, l.at("seq").to_number<std::int64_t>());
    }
    return top + 1;
}

/// Every invariant any call in the log saw violated.
[[nodiscard]] inline auto violated(const std::vector<call_record>& calls) -> std::set<std::string> {
    std::set<std::string> out;
    for (const auto& c : calls) {
        auto v = c.violated();
        out.insert(v.begin(), v.end());
    }
    return out;
}

/// The log's lines for the given calls only, in order: what minimize writes.
[[nodiscard]] inline auto lines_of(const std::vector<call_record>& calls)
    -> std::vector<boost::json::object> {
    std::vector<boost::json::object> out;
    for (const auto& c : calls) {
        out.push_back(c.started_line);
        if (c.done_line) {
            out.push_back(*c.done_line);
        }
    }
    return out;
}

}  // namespace chaos_ctl
