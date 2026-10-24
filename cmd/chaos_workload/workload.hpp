// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The workload client's core (.kiro/specs/cairn-chaos-testing/ Requirement
// 4, design §2.2): which operation to run next, how to run one against a
// chaos_node cluster through leader redirects, and how each HTTP outcome
// maps to Jepsen's ok / fail / info. The transport and the clock are
// injected, so chaos_workload_unit_test drives all of it without sockets.
//
// The one rule everything here serves: `fail` only when the operation
// definitely did not take effect. A write that may have reached a leader's
// log is `info`, never `fail`, because a checker that treats an
// indeterminate write as absent reports violations that never happened.

#include "history.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace chaos_workload {

using docker_chaos::history::attempt;
using docker_chaos::history::event;
using docker_chaos::history::event_type;
using docker_chaos::history::op_kind;

/// What a transport reports for one request. `sent` is false only when the
/// request certainly never reached the server (the connection was refused);
/// a timeout or reset after connecting is `sent` with status 0.
struct response {
    int status{0};
    std::string body;
    bool sent{true};
    std::string error;
};

/// GET when body is empty, POST otherwise. `address` is "host:port".
using transport = std::function<response(const std::string& address, const std::string& path,
                                         const std::string& body)>;

struct operation {
    op_kind f{op_kind::read};
    std::string key;
    std::optional<std::string> value;     // write: the value; cas: the new value
    std::optional<std::string> expected;  // cas: the value it must replace
    bool local_read{false};               // read: mode=local instead of linearizable
};

// ── Operation generator ──────────────────────────────────────────────────────

/// Relative weights of the four operation shapes.
struct mix {
    unsigned linearizable_read{3};
    unsigned local_read{1};
    unsigned write{4};
    unsigned cas{2};
};

/// Parses "read=3,local=1,write=4,cas=2"; unnamed shapes keep their default.
[[nodiscard]] inline auto parse_mix(std::string_view text) -> mix {
    mix out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto end = text.find(',', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        auto item = text.substr(pos, end - pos);
        auto eq = item.find('=');
        if (eq == std::string_view::npos) {
            throw std::invalid_argument("mix: expected name=weight, got '" + std::string(item) +
                                        "'");
        }
        auto name = item.substr(0, eq);
        auto weight = static_cast<unsigned>(std::stoul(std::string(item.substr(eq + 1))));
        if (name == "read") {
            out.linearizable_read = weight;
        } else if (name == "local") {
            out.local_read = weight;
        } else if (name == "write") {
            out.write = weight;
        } else if (name == "cas") {
            out.cas = weight;
        } else {
            throw std::invalid_argument("mix: unknown operation '" + std::string(name) + "'");
        }
        pos = end + 1;
    }
    if (out.linearizable_read + out.local_read + out.write + out.cas == 0) {
        throw std::invalid_argument("mix: every weight is zero");
    }
    return out;
}

/// Random operations over keys <prefix>0..<prefix><keys-1> (prefix "k" by
/// default) and values 0..<values-1>.
///
/// The value domain is small on purpose, as in Jepsen's cas-register: with
/// a handful of values a CAS's expected value is often the current one, so
/// both its outcomes are exercised. The checker distinguishes writes of the
/// same value by their timing, not by the value.
class generator {
public:
    generator(std::uint64_t seed, unsigned keys, unsigned values, mix weights,
              std::string key_prefix = "k")
        : _rng(seed), _keys(keys), _values(values), _mix(weights), _prefix(std::move(key_prefix)) {
        if (keys == 0 || values == 0) {
            throw std::invalid_argument("generator: keys and values must be at least 1");
        }
    }

    auto next() -> operation {
        operation op;
        op.key = _prefix + std::to_string(pick(_keys));
        const unsigned total = _mix.linearizable_read + _mix.local_read + _mix.write + _mix.cas;
        const auto roll = pick(total);
        const unsigned local_from = _mix.linearizable_read;
        const unsigned write_from = local_from + _mix.local_read;
        const unsigned cas_from = write_from + _mix.write;
        if (roll < local_from) {
            op.f = op_kind::read;
        } else if (roll < write_from) {
            op.f = op_kind::read;
            op.local_read = true;
        } else if (roll < cas_from) {
            op.f = op_kind::write;
            op.value = std::to_string(pick(_values));
        } else {
            op.f = op_kind::cas;
            op.expected = std::to_string(pick(_values));
            op.value = std::to_string(pick(_values));
        }
        return op;
    }

private:
    auto pick(unsigned n) -> unsigned {
        return std::uniform_int_distribution<unsigned>(0, n - 1)(_rng);
    }

    std::mt19937_64 _rng;
    unsigned _keys;
    unsigned _values;
    mix _mix;
    std::string _prefix;
};

// ── Node table ───────────────────────────────────────────────────────────────

/// Parses "1=c5node1:8080,2=c5node2:8080" into node id → address.
[[nodiscard]] inline auto parse_nodes(std::string_view text) -> std::map<int, std::string> {
    std::map<int, std::string> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto end = text.find(',', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        auto item = text.substr(pos, end - pos);
        auto eq = item.find('=');
        if (eq == std::string_view::npos || eq == 0 || eq + 1 == item.size()) {
            throw std::invalid_argument("nodes: expected id=host:port, got '" + std::string(item) +
                                        "'");
        }
        out[std::stoi(std::string(item.substr(0, eq)))] = std::string(item.substr(eq + 1));
        pos = end + 1;
    }
    if (out.empty()) {
        throw std::invalid_argument("nodes: empty");
    }
    return out;
}

/// A node id from /status's leader_id: a number, or decimal text in a
/// text-id build. Empty for null or anything that is not a small integer.
[[nodiscard]] inline auto leader_of(const std::string& status_body) -> std::optional<int> {
    boost::system::error_code ec;
    auto doc = boost::json::parse(status_body, ec);
    if (ec || !doc.is_object()) {
        return std::nullopt;
    }
    const auto* id = doc.as_object().if_contains("leader_id");
    if (id == nullptr) {
        return std::nullopt;
    }
    if (id->is_int64() || id->is_uint64()) {
        return static_cast<int>(id->to_number<std::int64_t>());
    }
    if (id->is_string()) {
        try {
            return std::stoi(std::string(id->as_string()));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// ── Running one operation ────────────────────────────────────────────────────

class client {
public:
    client(std::map<int, std::string> nodes, transport send)
        : _nodes(std::move(nodes)), _send(std::move(send)) {
        if (_nodes.empty()) {
            throw std::invalid_argument("client: no nodes");
        }
        _leader_guess = _nodes.begin()->first;
    }

    /// Runs `op` and returns its completion: type, value, node and every
    /// attempt. Process and time are the caller's to fill in. A local read
    /// goes to `local_node` when given, so a run can pin stale-read probes
    /// to one replica; everything else chases the leader.
    auto run(const operation& op, std::optional<int> local_node = std::nullopt) -> event {
        event done;
        done.f = op.f;
        done.key = op.key;
        done.expected = op.expected;
        if (op.f == op_kind::read) {
            done.mode = op.local_read ? "local" : "linearizable";
        } else {
            done.value = op.value;
        }

        int target = op.local_read && local_node ? *local_node : _leader_guess;
        // Each node at most twice: enough to follow a redirect through a
        // leader change, few enough that a leaderless cluster fails fast.
        const std::size_t budget = _nodes.size() * 2;
        for (std::size_t i = 0; i < budget; ++i) {
            auto r = _send(_nodes.at(target), path_of(op), body_of(op));
            done.attempts.push_back(attempt{.node = target, .status = r.status, .error = r.error});

            if (!r.sent) {
                // Never delivered: no effect, so try the next node.
                target = next_node(target);
                continue;
            }
            if (r.status == 0) {
                return finish(done, event_type::info, target, "no response after send: " + r.error);
            }
            if (r.status == 400) {
                return finish(done, event_type::fail, target, "rejected: " + r.body);
            }
            if (r.status == 200) {
                if (op.f != op_kind::read) {
                    _leader_guess = target;
                }
                return complete_200(done, op, target, r.body);
            }
            if (r.status == 503 && redirectable(op, r.body)) {
                // Not a leader, so nothing was submitted: ask it who leads.
                auto s = _send(_nodes.at(target), "/status", "");
                auto leader = s.status == 200 ? leader_of(s.body) : std::nullopt;
                target = leader && _nodes.contains(*leader) && *leader != target
                             ? *leader
                             : next_node(target);
                _leader_guess = target;
                continue;
            }
            // A 503 from a leader that accepted the command (commit_failed),
            // or anything unrecognised: it may have taken effect.
            return finish(done, event_type::info, target,
                          "HTTP " + std::to_string(r.status) + ": " + r.body);
        }
        // Every attempt was refused before submission.
        return finish(done, event_type::fail, 0, "no leader reachable");
    }

private:
    static auto path_of(const operation& op) -> std::string {
        if (op.f == op_kind::read) {
            return "/kv/" + op.key + (op.local_read ? "?mode=local" : "?mode=linearizable");
        }
        return "/command";
    }

    static auto body_of(const operation& op) -> std::string {
        if (op.f == op_kind::read) {
            return {};
        }
        boost::json::object body{{"key", op.key}, {"value", op.value.value_or("")}};
        if (op.f == op_kind::cas) {
            body["op"] = "cas";
            body["expected"] =
                op.expected ? boost::json::value(*op.expected) : boost::json::value(nullptr);
        }
        return boost::json::serialize(body);
    }

    /// Whether a 503 says the node refused before doing anything. A write's
    /// not_leader is checked before submit_command; a linearizable read that
    /// fails has no side effect whatever the reason, so it may move on.
    static auto redirectable(const operation& op, const std::string& body) -> bool {
        if (op.f == op_kind::read) {
            return true;
        }
        return body.find("not_leader") != std::string::npos;
    }

    [[nodiscard]] auto next_node(int after) const -> int {
        auto it = _nodes.upper_bound(after);
        return it == _nodes.end() ? _nodes.begin()->first : it->first;
    }

    static auto finish(event& e, event_type type, int node, std::string error) -> event {
        e.type = type;
        e.node = node;
        e.error = std::move(error);
        return e;
    }

    static auto complete_200(event& e, const operation& op, int node, const std::string& body)
        -> event {
        boost::system::error_code ec;
        auto doc = boost::json::parse(body, ec);
        if (ec || !doc.is_object()) {
            // The node answered, so a write took effect; what it said is lost.
            return finish(e, op.f == op_kind::read ? event_type::fail : event_type::info, node,
                          "unparseable 200 body");
        }
        const auto& obj = doc.as_object();
        if (op.f == op_kind::read) {
            const auto* v = obj.if_contains("value");
            if (v != nullptr && v->is_string()) {
                e.value = std::string(v->as_string());
            } else {
                e.value.reset();
            }
            return finish(e, event_type::ok, node, {});
        }
        if (op.f == op_kind::cas) {
            const auto* applied = obj.if_contains("applied");
            if (applied == nullptr || !applied->is_bool()) {
                return finish(e, event_type::info, node, "cas reply without applied");
            }
            // A CAS that committed but did not match changed nothing: fail.
            return finish(e, applied->as_bool() ? event_type::ok : event_type::fail, node,
                          applied->as_bool() ? std::string{} : "expected value did not match");
        }
        return finish(e, event_type::ok, node, {});
    }

    std::map<int, std::string> _nodes;
    transport _send;
    int _leader_guess;
};

}  // namespace chaos_workload
