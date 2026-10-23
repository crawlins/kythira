// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "control_codec.hpp"

#include <raft/host_node_id.hpp>
#include <raft/raft.hpp>
#include <raft/tcp_raft_types.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace chaos_node {

// HTTP control plane for a running kythira::node<Types>.
// Endpoints (.kiro/specs/cairn-chaos-testing/ Requirement 1, design §1):
//   GET  /health              → 200 {"status":"running"} or 503
//   GET  /status              → 200 {node_id,role,term,leader_id,commit_index,last_applied,
//                                    last_log_index,last_log_term,snapshot_index,
//                                    config{voters,learners,joint}}
//                               (node ids are JSON numbers, or strings in a build
//                               with KYTHIRA_NODE_ID_KIND=text; leader_id is null
//                               before any leader is known)
//   POST /command             → body {"key":"k","value":"v"} (put), or
//                               {"op":"cas","key":"k","expected":"e"|null,"value":"v"}
//                               → 200 {success,commit_index[,applied]} / 400 / 503
//   GET  /log/:index          → 200 {index,term,type,command_b64}, or 404
//                               {"error":"compacted"|"beyond_end"|"missing"}
//   GET  /log?from=a&to=b     → 200 {from,to,snapshot_index,last_log_index,entries[]};
//                               at most control::k_max_log_range entries
//   GET  /elections           → 200 {node_id,elections[{term,at_ms}]}: the terms this
//                               process won, oldest first, at most 1024
//   GET  /kv/:key?mode=linearizable|local
//                             → 200 {key,mode,found,value,commit_index,last_applied}
//   POST /membership/add      → body {"node_id":id} → 200 / 400 / 409 (change pending)
//   POST /membership/remove     / 503 (not leader, or the change failed)

template<typename NodeType> class http_control_tmpl {
public:
    using node_t = NodeType;

    http_control_tmpl(node_t& n, kythira::host_node_id node_id, std::uint16_t port)
        : _node(n), _node_id(std::move(node_id)), _port(port) {}

    ~http_control_tmpl() { stop(); }

    void start() {
        _server.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
            if (_node.is_running()) {
                res.set_content(R"({"status":"running"})", "application/json");
            } else {
                res.status = 503;
                res.set_content(R"({"status":"stopped"})", "application/json");
            }
        });

        _server.Get("/status", [this](const httplib::Request&, httplib::Response& res) {
            auto state = _node.debug_state();
            auto bounds = _node.log_bounds();
            auto membership = _node.current_membership();
            std::string_view role;
            switch (_node.get_state()) {
                case kythira::server_state::leader:
                    role = "leader";
                    break;
                case kythira::server_state::candidate:
                    role = "candidate";
                    break;
                default:
                    role = "follower";
                    break;
            }
            // A leader names itself: known_leader() is only ever set from a
            // leader's AppendEntries, which a leader does not send itself.
            auto leader = role == "leader" ? std::optional<kythira::host_node_id>(_node_id)
                                           : _node.known_leader();
            control::status_fields<kythira::host_node_id> fields{
                .node_id = _node_id,
                .role = role,
                .term = state.current_term,
                .leader_id = std::move(leader),
                .commit_index = state.commit_index,
                .last_applied = state.last_applied,
                .last_log_index = bounds.last_log_index,
                .last_log_term = bounds.last_log_term,
                .snapshot_index = bounds.snapshot_index,
                .voters = std::move(membership.voters),
                .learners = std::move(membership.learners),
                .joint = membership.joint,
            };
            res.set_content(boost::json::serialize(control::status_json(fields)),
                            "application/json");
        });

        _server.Post("/command", [this](const httplib::Request& req, httplib::Response& res) {
            if (!_node.is_leader()) {
                res.status = 503;
                res.set_content(R"({"error":"not_leader"})", "application/json");
                return;
            }
            auto request = control::parse_command_request(req.body);
            if (!request) {
                res.status = 400;
                res.set_content(control::error_json("bad_request", request.error),
                                "application/json");
                return;
            }
            // test_key_value_state_machine::make_put_command/make_cas_command
            // (test_state_machine.hpp): the encodings its apply() parses.
            auto cmd = control::encode_command(*request.value);
            try {
                auto fut = _node.submit_command(cmd, std::chrono::seconds{5});
                auto result = std::move(fut).get();
                auto state = _node.debug_state();
                boost::json::object body{{"success", true}, {"commit_index", state.commit_index}};
                if (request.value->op == control::command_request::op_kind::cas) {
                    body["applied"] = result.size() == 1 && result[0] == std::byte{1};
                }
                res.set_content(boost::json::serialize(body), "application/json");
            } catch (const std::exception& e) {
                res.status = 503;
                res.set_content(control::error_json("commit_failed", e.what()), "application/json");
            }
        });

        _server.Get(R"(/log/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
            auto idx = control::parse_index(req.matches[1].str());
            if (!idx) {
                res.status = 400;
                res.set_content(control::error_json("bad_index"), "application/json");
                return;
            }
            auto entries = _node.log_entries_between(*idx, *idx);
            if (entries.empty()) {
                auto bounds = _node.log_bounds();
                res.status = 404;
                res.set_content(control::error_json(control::missing_entry_reason(
                                    *idx, bounds.snapshot_index, bounds.last_log_index)),
                                "application/json");
                return;
            }
            res.set_content(boost::json::serialize(control::log_entry_json(entries.front())),
                            "application/json");
        });

        _server.Get("/log", [this](const httplib::Request& req, httplib::Response& res) {
            auto param = [&req](const char* name) -> std::optional<std::string> {
                if (!req.has_param(name)) {
                    return std::nullopt;
                }
                return req.get_param_value(name);
            };
            auto from = param("from");
            auto to = param("to");
            auto range = control::parse_log_range(
                from ? std::optional<std::string_view>(*from) : std::nullopt,
                to ? std::optional<std::string_view>(*to) : std::nullopt);
            if (!range) {
                res.status = 400;
                res.set_content(control::error_json("bad_range", range.error), "application/json");
                return;
            }
            // Bounds before entries: an entry compacted between the two reads
            // then shows as missing below a snapshot_index that explains it,
            // never as a gap the reply's own bounds cannot account for.
            auto bounds = _node.log_bounds();
            auto entries = _node.log_entries_between(range.value->from, range.value->to);
            res.set_content(
                boost::json::serialize(control::log_range_json(
                    *range.value, entries, bounds.snapshot_index, bounds.last_log_index)),
                "application/json");
        });

        _server.Get("/elections", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(boost::json::serialize(
                                control::elections_json(_node_id, _node.leadership_history())),
                            "application/json");
        });

        _server.Get(R"(/kv/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
            const std::string key = req.matches[1].str();
            auto mode = control::parse_read_mode(
                req.has_param("mode") ? std::optional<std::string>(req.get_param_value("mode"))
                                      : std::nullopt);
            if (!mode) {
                res.status = 400;
                res.set_content(control::error_json("bad_mode", mode.error), "application/json");
                return;
            }
            std::optional<std::string> value;
            std::uint64_t last_applied = 0;
            if (*mode.value == control::read_mode::linearizable) {
                // read_state() confirms leadership with a heartbeat quorum and
                // returns the whole state machine; decoding one key of it is
                // cheap at chaos-test key counts.
                try {
                    auto state_bytes = std::move(_node.read_state(std::chrono::seconds{5})).get();
                    auto store = control::kv_machine::deserialize_store(state_bytes);
                    if (auto it = store.find(key); it != store.end()) {
                        value = it->second;
                    }
                } catch (const std::exception& e) {
                    res.status = 503;
                    res.set_content(control::error_json("read_failed", e.what()),
                                    "application/json");
                    return;
                }
                last_applied = _node.last_applied_index();
            } else {
                // No quorum round: deliberately allowed to be stale, so a
                // stale-read finding is attributable to the mode that permits
                // it. Value and applied index come from one locked look.
                _node.with_state_machine([&](const auto& sm) {
                    value = sm.get_value(key);
                    last_applied = sm.get_last_applied_index();
                });
            }
            auto state = _node.debug_state();
            res.set_content(boost::json::serialize(control::kv_read_json(
                                key, *mode.value, value, state.commit_index, last_applied)),
                            "application/json");
        });

        auto membership = [this](bool add) {
            return [this, add](const httplib::Request& req, httplib::Response& res) {
                if (!_node.is_leader()) {
                    res.status = 503;
                    res.set_content(R"({"error":"not_leader"})", "application/json");
                    return;
                }
                auto id = control::parse_membership_request<kythira::host_node_id>(req.body);
                if (!id) {
                    res.status = 400;
                    res.set_content(control::error_json("bad_request", id.error),
                                    "application/json");
                    return;
                }
                try {
                    // Returns when the change commits or fails; add_server()
                    // fails on its own once the server stops making progress.
                    auto fut = add ? _node.add_server(*id.value) : _node.remove_server(*id.value);
                    std::move(fut).get();
                    auto state = _node.debug_state();
                    res.set_content(boost::json::serialize(boost::json::object{
                                        {"success", true}, {"commit_index", state.commit_index}}),
                                    "application/json");
                } catch (const std::exception& e) {
                    res.status = control::is_change_in_progress(e.what()) ? 409 : 503;
                    res.set_content(
                        control::error_json(
                            res.status == 409 ? "change_in_progress" : "change_failed", e.what()),
                        "application/json");
                }
            };
        };
        _server.Post("/membership/add", membership(true));
        _server.Post("/membership/remove", membership(false));

        _thread = std::thread([this] { _server.listen("0.0.0.0", _port); });
    }

    void stop() {
        _server.stop();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

private:
    node_t& _node;
    kythira::host_node_id _node_id;
    std::uint16_t _port;
    httplib::Server _server;
    std::thread _thread;
};

// Backward-compatible alias for the host's tcp_raft_types node type
using http_control =
    http_control_tmpl<kythira::node<kythira::basic_tcp_raft_types<kythira::host_node_id>>>;

}  // namespace chaos_node
