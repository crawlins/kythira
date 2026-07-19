#pragma once

#include <raft/raft.hpp>
#include <raft/tcp_raft_types.hpp>

#include <httplib.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace chaos_node {

// HTTP control plane for a running kythira::node<Types>.
// Endpoints:
//   GET  /health           → 200 {"status":"running"} or 503
//   GET  /status           → 200 {node_id,role,term,leader_id,commit_index,last_applied}
//   POST /command          → body {"key":"k","value":"v"} → 200/503
//   GET  /log/:index       → 200 {index,term,command_b64} or 404

template<typename NodeType> class http_control_tmpl {
public:
    using node_t = NodeType;

    http_control_tmpl(node_t& n, std::uint64_t node_id, std::uint16_t port)
        : _node(n), _node_id(node_id), _port(port) {}

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
            std::string role;
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
            auto body = std::string(R"({"node_id":)") + std::to_string(_node_id) + R"(,"role":")" +
                        role + '"' + R"(,"term":)" + std::to_string(state.current_term) +
                        R"(,"commit_index":)" + std::to_string(state.commit_index) +
                        R"(,"last_applied":)" + std::to_string(state.last_applied) + '}';
            res.set_content(body, "application/json");
        });

        _server.Post("/command", [this](const httplib::Request& req, httplib::Response& res) {
            if (!_node.is_leader()) {
                res.status = 503;
                res.set_content(R"({"error":"not_leader"})", "application/json");
                return;
            }
            auto kpos = req.body.find("\"key\"");
            auto vpos = req.body.find("\"value\"");
            if (kpos == std::string::npos || vpos == std::string::npos) {
                res.status = 400;
                res.set_content(R"({"error":"bad_request"})", "application/json");
                return;
            }
            auto extract_val = [&](std::size_t pos) -> std::string {
                auto c = req.body.find(':', pos);
                auto q1 = req.body.find('"', c + 1);
                auto q2 = req.body.find('"', q1 + 1);
                return req.body.substr(q1 + 1, q2 - q1 - 1);
            };
            std::string key = extract_val(kpos);
            std::string value = extract_val(vpos);

            // test_key_value_state_machine::apply() (test_state_machine.hpp)
            // expects [command_type:1][key_length:4][key][value_length:4]
            // [value], not free-form text — must match its memcpy-based
            // parser exactly, including native (little-endian on every
            // platform this project targets) length encoding.
            auto key_length = static_cast<std::uint32_t>(key.size());
            auto value_length = static_cast<std::uint32_t>(value.size());
            std::vector<std::byte> cmd;
            cmd.reserve(1 + sizeof(key_length) + key.size() + sizeof(value_length) + value.size());
            cmd.push_back(static_cast<std::byte>(1));  // command_type::put
            const auto* key_length_bytes = reinterpret_cast<const std::byte*>(&key_length);
            cmd.insert(cmd.end(), key_length_bytes, key_length_bytes + sizeof(key_length));
            const auto* key_bytes = reinterpret_cast<const std::byte*>(key.data());
            cmd.insert(cmd.end(), key_bytes, key_bytes + key.size());
            const auto* value_length_bytes = reinterpret_cast<const std::byte*>(&value_length);
            cmd.insert(cmd.end(), value_length_bytes, value_length_bytes + sizeof(value_length));
            const auto* value_bytes = reinterpret_cast<const std::byte*>(value.data());
            cmd.insert(cmd.end(), value_bytes, value_bytes + value.size());

            try {
                auto fut = _node.submit_command(cmd, std::chrono::seconds{5});
                auto result = std::move(fut).get();
                auto state = _node.debug_state();
                res.set_content(std::string(R"({"success":true,"commit_index":)") +
                                    std::to_string(state.commit_index) + '}',
                                "application/json");
            } catch (const std::exception& e) {
                res.status = 503;
                res.set_content(
                    std::string(R"({"error":"commit_failed","detail":")") + e.what() + "\"}",
                    "application/json");
            }
        });

        _server.Get(R"(/log/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
            auto idx = static_cast<std::uint64_t>(std::stoull(req.matches[1]));
            res.status = 404;
            res.set_content(R"({"error":"not_implemented"})", "application/json");
            (void)idx;
        });

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
    std::uint64_t _node_id;
    std::uint16_t _port;
    httplib::Server _server;
    std::thread _thread;
};

// Backward-compatible alias for the existing tcp_raft_types node type
using http_control = http_control_tmpl<kythira::node<kythira::tcp_raft_types>>;

}  // namespace chaos_node
