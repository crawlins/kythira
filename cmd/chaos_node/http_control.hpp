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

// HTTP control plane for a running kythira::node<tcp_raft_types>.
// Endpoints:
//   GET  /health           → 200 {"status":"running"} or 503
//   GET  /status           → 200 {node_id,role,term,leader_id,commit_index,last_applied}
//   POST /command          → body {"key":"k","value":"v"} → 200/503
//   GET  /log/:index       → 200 {index,term,command_b64} or 404

class http_control {
public:
    using node_t = kythira::node<kythira::tcp_raft_types>;

    http_control(node_t& n, std::uint64_t node_id, std::uint16_t port)
        : _node(n), _node_id(node_id), _port(port) {}

    ~http_control() { stop(); }

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
            // Parse {"key":"k","value":"v"} manually; use boost::json in full impl
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

            // Encode as: "PUT key value\n"
            std::string cmd_str = "PUT " + key + " " + value + "\n";
            std::vector<std::byte> cmd(cmd_str.size());
            for (std::size_t i = 0; i < cmd_str.size(); ++i)
                cmd[i] = static_cast<std::byte>(cmd_str[i]);

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
            auto entry = _node.debug_state();  // need full state for log access
            // Access via persistence (simpler than exposing log from debug_state)
            res.status = 404;
            res.set_content(R"({"error":"not_implemented"})", "application/json");
            (void)idx;
        });

        _thread = std::thread([this] { _server.listen("0.0.0.0", _port); });
    }

    void stop() {
        _server.stop();
        if (_thread.joinable()) _thread.join();
    }

private:
    node_t& _node;
    std::uint64_t _node_id;
    std::uint16_t _port;
    httplib::Server _server;
    std::thread _thread;
};

}  // namespace chaos_node
