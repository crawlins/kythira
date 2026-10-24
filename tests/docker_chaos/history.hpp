// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// One line of a workload history (.kiro/specs/cairn-chaos-testing/ design
// §2.2), shared by cmd/chaos_workload, which writes it, and the checkers in
// tests/docker_chaos/checkers/, which read it. JSONL, one event per line:
//
//   {"p":3,"type":"invoke","f":"cas","key":"k2","value":[4,7],"t":18273645}
//   {"p":3,"type":"ok","f":"cas","key":"k2","value":[4,7],"t":18291002,"node":2}
//
// `type` has Jepsen's meaning. `fail`: the operation definitely did not take
// effect. `info`: indeterminate (a timeout or a lost connection after the
// request was sent); a process that records `info` never invokes again, so
// no process has two operations outstanding. `t` is nanoseconds on the
// workload's one monotonic clock.

#include <boost/json.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace docker_chaos::history {

enum class event_type : std::uint8_t {
    invoke,
    ok,
    fail,
    info
};
enum class op_kind : std::uint8_t {
    read,
    write,
    cas
};

[[nodiscard]] inline auto to_string(event_type t) -> std::string_view {
    switch (t) {
        case event_type::invoke:
            return "invoke";
        case event_type::ok:
            return "ok";
        case event_type::fail:
            return "fail";
        case event_type::info:
            return "info";
    }
    return "info";
}

[[nodiscard]] inline auto to_string(op_kind k) -> std::string_view {
    switch (k) {
        case op_kind::read:
            return "read";
        case op_kind::write:
            return "write";
        case op_kind::cas:
            return "cas";
    }
    return "read";
}

/// One request a completion made, in order: the node asked and what came
/// back (an HTTP status, or 0 for no response). Requirement 4.3 asks for
/// every attempt, including the redirects before the one that counted.
struct attempt {
    int node{0};
    int status{0};
    std::string error;
};

struct event {
    std::int64_t process{0};
    event_type type{event_type::invoke};
    op_kind f{op_kind::read};
    std::string key;
    /// read: the value read, absent for "not found" or on invoke. write: the
    /// value written. cas: [expected, new], with expected absent meaning
    /// "key must be absent".
    std::optional<std::string> value;
    std::optional<std::string> expected;  // cas only
    std::int64_t time_ns{0};
    /// read only: "linearizable" or "local".
    std::string mode;
    /// The node that answered the completion; 0 on invoke or when none did.
    int node{0};
    std::vector<attempt> attempts;
    /// Why a fail or info happened, for a human reading the history.
    std::string error;
};

[[nodiscard]] inline auto to_json(const event& e) -> boost::json::object {
    auto opt = [](const std::optional<std::string>& v) {
        return v ? boost::json::value(*v) : boost::json::value(nullptr);
    };
    // Key order follows design §2.2's example lines.
    boost::json::object out{
        {"p", e.process},
        {"type", to_string(e.type)},
        {"f", to_string(e.f)},
        {"key", e.key},
        {"value", e.f == op_kind::cas
                      ? boost::json::value(boost::json::array{opt(e.expected), opt(e.value)})
                      : opt(e.value)},
        {"t", e.time_ns},
    };
    if (!e.mode.empty()) {
        out["mode"] = e.mode;
    }
    if (e.node != 0) {
        out["node"] = e.node;
    }
    if (!e.attempts.empty()) {
        boost::json::array list;
        for (const auto& a : e.attempts) {
            boost::json::object one{{"node", a.node}, {"status", a.status}};
            if (!a.error.empty()) {
                one["error"] = a.error;
            }
            list.push_back(std::move(one));
        }
        out["attempts"] = std::move(list);
    }
    if (!e.error.empty()) {
        out["error"] = e.error;
    }
    return out;
}

[[nodiscard]] inline auto to_line(const event& e) -> std::string {
    return boost::json::serialize(to_json(e));
}

namespace detail {
inline auto opt_string(const boost::json::value& v) -> std::optional<std::string> {
    if (v.is_null()) {
        return std::nullopt;
    }
    return std::string(v.as_string());
}

inline auto parse_type(std::string_view s) -> event_type {
    if (s == "invoke") {
        return event_type::invoke;
    }
    if (s == "ok") {
        return event_type::ok;
    }
    if (s == "fail") {
        return event_type::fail;
    }
    if (s == "info") {
        return event_type::info;
    }
    throw std::invalid_argument("history: unknown type '" + std::string(s) + "'");
}

inline auto parse_kind(std::string_view s) -> op_kind {
    if (s == "read") {
        return op_kind::read;
    }
    if (s == "write") {
        return op_kind::write;
    }
    if (s == "cas") {
        return op_kind::cas;
    }
    throw std::invalid_argument("history: unknown f '" + std::string(s) + "'");
}
}  // namespace detail

/// Throws std::invalid_argument (or boost::system::system_error from the
/// JSON layer) on a line that is not a history event.
[[nodiscard]] inline auto from_line(std::string_view line) -> event {
    auto obj = boost::json::parse(line).as_object();
    event e;
    e.process = obj.at("p").to_number<std::int64_t>();
    e.type = detail::parse_type(obj.at("type").as_string());
    e.f = detail::parse_kind(obj.at("f").as_string());
    e.key = std::string(obj.at("key").as_string());
    e.time_ns = obj.at("t").to_number<std::int64_t>();
    const auto& v = obj.at("value");
    if (e.f == op_kind::cas) {
        const auto& pair = v.as_array();
        if (pair.size() != 2) {
            throw std::invalid_argument("history: cas value must be [expected, new]");
        }
        e.expected = detail::opt_string(pair[0]);
        e.value = detail::opt_string(pair[1]);
    } else {
        e.value = detail::opt_string(v);
    }
    if (const auto* m = obj.if_contains("mode")) {
        e.mode = std::string(m->as_string());
    }
    if (const auto* n = obj.if_contains("node")) {
        e.node = static_cast<int>(n->to_number<std::int64_t>());
    }
    if (const auto* list = obj.if_contains("attempts")) {
        for (const auto& a : list->as_array()) {
            const auto& o = a.as_object();
            attempt one{.node = static_cast<int>(o.at("node").to_number<std::int64_t>()),
                        .status = static_cast<int>(o.at("status").to_number<std::int64_t>()),
                        .error = {}};
            if (const auto* err = o.if_contains("error")) {
                one.error = std::string(err->as_string());
            }
            e.attempts.push_back(std::move(one));
        }
    }
    if (const auto* err = obj.if_contains("error")) {
        e.error = std::string(err->as_string());
    }
    return e;
}

/// The history lines of `text`: every line that starts with '{', so a
/// container log that interleaves the workload's stderr still parses.
[[nodiscard]] inline auto parse_history(std::string_view text) -> std::vector<event> {
    std::vector<event> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        auto line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!line.empty() && line.front() == '{') {
            out.push_back(from_line(line));
        }
        pos = end + 1;
    }
    return out;
}

/// The structural rules every well-formed history obeys, independent of any
/// consistency model: each completion follows an invoke by the same process,
/// with the same f and key; no process has two operations outstanding; and a
/// process that completed with `info` never invokes again. Returns the first
/// violation, or an empty string.
[[nodiscard]] inline auto structural_error(const std::vector<event>& h) -> std::string {
    struct state {
        bool outstanding{false};
        bool retired{false};
        op_kind f{op_kind::read};
        std::string key;
    };
    std::vector<std::pair<std::int64_t, state>> procs;
    auto find = [&procs](std::int64_t p) -> state& {
        for (auto& [id, s] : procs) {
            if (id == p) {
                return s;
            }
        }
        procs.emplace_back(p, state{});
        return procs.back().second;
    };
    for (std::size_t i = 0; i < h.size(); ++i) {
        const auto& e = h[i];
        auto& s = find(e.process);
        const auto where =
            "event " + std::to_string(i) + " (process " + std::to_string(e.process) + "): ";
        if (e.type == event_type::invoke) {
            if (s.retired) {
                return where + "invoke after info";
            }
            if (s.outstanding) {
                return where + "invoke with an operation outstanding";
            }
            s.outstanding = true;
            s.f = e.f;
            s.key = e.key;
            continue;
        }
        if (!s.outstanding) {
            return where + "completion with nothing outstanding";
        }
        if (s.f != e.f || s.key != e.key) {
            return where + "completion does not match its invoke";
        }
        s.outstanding = false;
        if (e.type == event_type::info) {
            s.retired = true;
        }
    }
    return {};
}

}  // namespace docker_chaos::history
