// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Request parsing and response building for chaos_node's HTTP control plane
// (http_control.hpp), kept free of httplib and of a running node so that
// chaos_node_control_codec_unit_test can pin every format without sockets.
//
// The original routes in http_control.hpp hand-build their JSON. These use
// boost::json instead, because keys, values and textual node ids here are
// caller-chosen strings that reach a checker, and one unescaped quote in a
// hand-built body would make a whole history unparseable.

#include <raft/host_node_id.hpp>
#include <raft/test_state_machine.hpp>
#include <raft/types.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chaos_node::control {

/// The most entries one `GET /log?from=&to=` returns (design §1).
inline constexpr std::uint64_t k_max_log_range = 1024;

using kv_machine = kythira::test_key_value_state_machine<std::uint64_t>;

/// A parse result: the value, or the reason it was refused.
template<typename T> struct parsed {
    std::optional<T> value;
    std::string error;

    static auto ok(T v) -> parsed { return parsed{std::move(v), {}}; }
    static auto fail(std::string why) -> parsed { return parsed{std::nullopt, std::move(why)}; }
    explicit operator bool() const { return value.has_value(); }
};

[[nodiscard]] inline auto base64_encode(std::span<const std::byte> bytes) -> std::string {
    static constexpr std::string_view k_alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const auto n = (std::to_integer<std::uint32_t>(bytes[i]) << 16U) |
                       (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8U) |
                       std::to_integer<std::uint32_t>(bytes[i + 2]);
        out.push_back(k_alphabet[(n >> 18U) & 63U]);
        out.push_back(k_alphabet[(n >> 12U) & 63U]);
        out.push_back(k_alphabet[(n >> 6U) & 63U]);
        out.push_back(k_alphabet[n & 63U]);
    }
    const std::size_t rest = bytes.size() - i;
    if (rest > 0) {
        auto n = std::to_integer<std::uint32_t>(bytes[i]) << 16U;
        if (rest == 2) {
            n |= std::to_integer<std::uint32_t>(bytes[i + 1]) << 8U;
        }
        out.push_back(k_alphabet[(n >> 18U) & 63U]);
        out.push_back(k_alphabet[(n >> 12U) & 63U]);
        out.push_back(rest == 2 ? k_alphabet[(n >> 6U) & 63U] : '=');
        out.push_back('=');
    }
    return out;
}

[[nodiscard]] inline auto entry_type_name(kythira::entry_type type) -> std::string_view {
    switch (type) {
        case kythira::entry_type::normal:
            return "normal";
        case kythira::entry_type::configuration:
            return "configuration";
        case kythira::entry_type::no_op:
            return "no_op";
        case kythira::entry_type::split:
            return "split";
        case kythira::entry_type::merge_prepare:
            return "merge_prepare";
        case kythira::entry_type::merge_commit:
            return "merge_commit";
        case kythira::entry_type::merge_rollback:
            return "merge_rollback";
        case kythira::entry_type::merge_abandoned:
            return "merge_abandoned";
    }
    return "unknown";
}

/// A node id as a JSON value: a number, or a string in a text-id build.
template<typename NodeId> [[nodiscard]] auto node_id_value(const NodeId& id) -> boost::json::value {
    return boost::json::parse(kythira::node_id_json(id));
}

template<typename Entry>
[[nodiscard]] auto log_entry_json(const Entry& entry) -> boost::json::object {
    return boost::json::object{
        {"index", entry.index()},
        {"term", entry.term()},
        {"type", entry_type_name(entry.type())},
        {"command_b64", base64_encode(entry.command())},
    };
}

/// Why the node holds no entry at `index`: below or at the snapshot it was
/// compacted, past the end it was never written. An index inside the log
/// that is still missing is reported as such rather than guessed at.
[[nodiscard]] inline auto missing_entry_reason(std::uint64_t index, std::uint64_t snapshot_index,
                                               std::uint64_t last_log_index) -> std::string_view {
    if (index == 0 || index <= snapshot_index) {
        return "compacted";
    }
    if (index > last_log_index) {
        return "beyond_end";
    }
    return "missing";
}

[[nodiscard]] inline auto parse_index(std::string_view text) -> std::optional<std::uint64_t> {
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    std::uint64_t out = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (out > (UINT64_MAX - digit) / 10) {
            return std::nullopt;
        }
        out = out * 10 + digit;
    }
    return out;
}

struct log_range {
    std::uint64_t from{0};
    std::uint64_t to{0};
};

/// `from` is required. `to` defaults to, and is clamped to, the last index
/// of a `k_max_log_range`-entry window starting at `from`, so a caller that
/// asks for more learns from the reply's `to` where to resume.
[[nodiscard]] inline auto parse_log_range(std::optional<std::string_view> from,
                                          std::optional<std::string_view> to) -> parsed<log_range> {
    if (!from) {
        return parsed<log_range>::fail("missing from");
    }
    auto f = parse_index(*from);
    if (!f || *f == 0) {
        return parsed<log_range>::fail("from must be a log index of 1 or more");
    }
    const std::uint64_t window_end =
        *f > UINT64_MAX - (k_max_log_range - 1) ? UINT64_MAX : *f + (k_max_log_range - 1);
    std::uint64_t t = window_end;
    if (to) {
        auto parsed_to = parse_index(*to);
        if (!parsed_to) {
            return parsed<log_range>::fail("to must be a log index");
        }
        if (*parsed_to < *f) {
            return parsed<log_range>::fail("to is below from");
        }
        t = std::min(*parsed_to, window_end);
    }
    return parsed<log_range>::ok(log_range{.from = *f, .to = t});
}

template<typename Entry>
[[nodiscard]] auto log_range_json(const log_range& range, const std::vector<Entry>& entries,
                                  std::uint64_t snapshot_index, std::uint64_t last_log_index)
    -> boost::json::object {
    boost::json::array out;
    for (const auto& e : entries) {
        out.push_back(log_entry_json(e));
    }
    return boost::json::object{
        {"from", range.from},
        {"to", range.to},
        {"snapshot_index", snapshot_index},
        {"last_log_index", last_log_index},
        {"entries", std::move(out)},
    };
}

/// Everything `GET /status` reports, gathered by the caller.
template<typename NodeId> struct status_fields {
    NodeId node_id;
    std::string_view role;
    std::uint64_t term{0};
    std::optional<NodeId> leader_id;
    std::uint64_t commit_index{0};
    std::uint64_t last_applied{0};
    std::uint64_t last_log_index{0};
    std::uint64_t last_log_term{0};
    std::uint64_t snapshot_index{0};
    std::vector<NodeId> voters;
    std::vector<NodeId> learners;
    bool joint{false};
};

template<typename NodeId>
[[nodiscard]] auto status_json(const status_fields<NodeId>& s) -> boost::json::object {
    auto ids = [](const std::vector<NodeId>& in) {
        boost::json::array out;
        for (const auto& id : in) {
            out.push_back(node_id_value(id));
        }
        return out;
    };
    return boost::json::object{
        {"node_id", node_id_value(s.node_id)},
        {"role", s.role},
        {"term", s.term},
        // null, not absent, before this node has seen any leader: a checker
        // can then tell "no leader known" from "an old chaos_node".
        {"leader_id", s.leader_id ? node_id_value(*s.leader_id) : boost::json::value(nullptr)},
        {"commit_index", s.commit_index},
        {"last_applied", s.last_applied},
        {"last_log_index", s.last_log_index},
        {"last_log_term", s.last_log_term},
        {"snapshot_index", s.snapshot_index},
        {"config",
         boost::json::object{
             {"voters", ids(s.voters)},
             {"learners", ids(s.learners)},
             {"joint", s.joint},
         }},
    };
}

/// `GET /elections`. `at_ms` is this process's steady clock: monotonic and
/// comparable within one node's list, not across nodes or restarts.
template<typename NodeId, typename Record>
[[nodiscard]] auto elections_json(const NodeId& node_id, const std::vector<Record>& records)
    -> boost::json::object {
    boost::json::array out;
    for (const auto& r : records) {
        out.push_back(boost::json::object{
            {"term", r.term},
            {"at_ms", std::chrono::duration_cast<std::chrono::milliseconds>(r.at.time_since_epoch())
                          .count()},
        });
    }
    return boost::json::object{
        {"node_id", node_id_value(node_id)},
        {"elections", std::move(out)},
    };
}

enum class read_mode : std::uint8_t {
    linearizable,
    local
};

[[nodiscard]] inline auto parse_read_mode(std::optional<std::string_view> mode)
    -> parsed<read_mode> {
    if (!mode || *mode == "linearizable") {
        return parsed<read_mode>::ok(read_mode::linearizable);
    }
    if (*mode == "local") {
        return parsed<read_mode>::ok(read_mode::local);
    }
    return parsed<read_mode>::fail("mode must be linearizable or local");
}

[[nodiscard]] inline auto kv_read_json(std::string_view key, read_mode mode,
                                       const std::optional<std::string>& value,
                                       std::uint64_t commit_index, std::uint64_t last_applied)
    -> boost::json::object {
    return boost::json::object{
        {"key", key},
        {"mode", mode == read_mode::linearizable ? "linearizable" : "local"},
        {"found", value.has_value()},
        {"value", value ? boost::json::value(*value) : boost::json::value(nullptr)},
        {"commit_index", commit_index},
        {"last_applied", last_applied},
    };
}

/// One `POST /command` body: `{"key","value"}` (a put, as before), or
/// `{"op":"cas","key","expected","value"}`, where `"expected":null` means
/// the key must be absent.
struct command_request {
    enum class op_kind : std::uint8_t {
        put,
        cas
    };
    op_kind op{op_kind::put};
    std::string key;
    std::string value;
    std::optional<std::string> expected;
};

[[nodiscard]] inline auto parse_command_request(std::string_view body) -> parsed<command_request> {
    boost::system::error_code ec;
    auto doc = boost::json::parse(body, ec);
    if (ec || !doc.is_object()) {
        return parsed<command_request>::fail("body is not a JSON object");
    }
    const auto& obj = doc.as_object();
    auto string_field = [&obj](std::string_view name) -> const boost::json::string* {
        const auto* v = obj.if_contains(name);
        return v != nullptr ? v->if_string() : nullptr;
    };
    command_request req;
    if (const auto* op = obj.if_contains("op"); op != nullptr) {
        if (!op->is_string()) {
            return parsed<command_request>::fail("op must be a string");
        }
        if (op->as_string() == "cas") {
            req.op = command_request::op_kind::cas;
        } else if (op->as_string() != "put") {
            return parsed<command_request>::fail("op must be put or cas");
        }
    }
    const auto* key = string_field("key");
    const auto* value = string_field("value");
    if (key == nullptr || value == nullptr) {
        return parsed<command_request>::fail("key and value must be strings");
    }
    req.key = std::string(*key);
    req.value = std::string(*value);
    if (req.op == command_request::op_kind::cas) {
        const auto* expected = obj.if_contains("expected");
        if (expected == nullptr) {
            return parsed<command_request>::fail("cas needs expected (a string, or null)");
        }
        if (expected->is_string()) {
            req.expected = std::string(expected->as_string());
        } else if (!expected->is_null()) {
            return parsed<command_request>::fail("expected must be a string or null");
        }
    }
    return parsed<command_request>::ok(std::move(req));
}

[[nodiscard]] inline auto encode_command(const command_request& req) -> std::vector<std::byte> {
    if (req.op == command_request::op_kind::cas) {
        return kv_machine::make_cas_command(req.key, req.expected, req.value);
    }
    return kv_machine::make_put_command(req.key, req.value);
}

/// `{"node_id": <number or string>}`, the body of `/membership/{add,remove}`.
/// A number is accepted in a text-id build too, as its decimal text.
template<kythira::host_node_id_type NodeId>
[[nodiscard]] auto parse_membership_request(std::string_view body) -> parsed<NodeId> {
    boost::system::error_code ec;
    auto doc = boost::json::parse(body, ec);
    if (ec || !doc.is_object()) {
        return parsed<NodeId>::fail("body is not a JSON object");
    }
    const auto* id = doc.as_object().if_contains("node_id");
    std::string text;
    if (id != nullptr && id->is_string()) {
        text = std::string(id->as_string());
    } else if (id != nullptr && id->is_uint64()) {
        text = std::to_string(id->as_uint64());
    } else if (id != nullptr && id->is_int64() && id->as_int64() >= 0) {
        text = std::to_string(id->as_int64());
    } else {
        return parsed<NodeId>::fail("node_id must be a node id number or string");
    }
    auto result = kythira::parse_node_id_text<NodeId>(text);
    if (!result) {
        return parsed<NodeId>::fail(result.error());
    }
    return parsed<NodeId>::ok(std::move(result).value());
}

/// A configuration change refused because another is pending maps to 409;
/// `add_server()`/`remove_server()` report it only by message.
[[nodiscard]] inline auto is_change_in_progress(std::string_view what) -> bool {
    return what.find("already in progress") != std::string_view::npos;
}

[[nodiscard]] inline auto error_json(std::string_view error, std::string_view detail = {})
    -> std::string {
    boost::json::object out{{"error", error}};
    if (!detail.empty()) {
        out["detail"] = detail;
    }
    return boost::json::serialize(out);
}

}  // namespace chaos_node::control
