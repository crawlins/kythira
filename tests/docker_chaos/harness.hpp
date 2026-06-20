#pragma once

#include "fault_control.hpp"
#include "os_faults.hpp"

#include <httplib.h>

#include <boost/json.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace docker_chaos {

using namespace std::chrono_literals;

// ── Port layout — mirrors docker-compose.yml ──────────────────────────────────

struct NodePorts {
    int rpc_port;
    int http_port;
    int fiu_port;
    std::string container_name;
};

inline const std::map<int, NodePorts> k_node_map{
    {1, {7001, 8081, 9001, "chaos_node_1"}},
    {2, {7002, 8082, 9002, "chaos_node_2"}},
    {3, {7003, 8083, 9003, "chaos_node_3"}},
};

// ── HTTP call types ───────────────────────────────────────────────────────────

struct HttpResult {
    int status{0};
    std::string body;
};

using HttpGet = std::function<HttpResult(const std::string& url)>;
using HttpPost = std::function<HttpResult(const std::string& url, const std::string& body)>;

namespace detail {

inline std::pair<std::string, int> split_host_port(const std::string& url) {
    // Expects http://host:port/path
    auto after_scheme = url.find("://");
    std::string rest = (after_scheme == std::string::npos) ? url : url.substr(after_scheme + 3);
    auto slash = rest.find('/');
    auto host_port = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    auto colon = host_port.rfind(':');
    if (colon == std::string::npos) return {host_port, 80};
    return {host_port.substr(0, colon), std::stoi(host_port.substr(colon + 1))};
}

inline std::string path_of(const std::string& url) {
    auto after_scheme = url.find("://");
    std::string rest = (after_scheme == std::string::npos) ? url : url.substr(after_scheme + 3);
    auto slash = rest.find('/');
    return (slash == std::string::npos) ? std::string("/") : rest.substr(slash);
}

}  // namespace detail

inline HttpResult real_http_get(const std::string& url) {
    auto [host, port] = detail::split_host_port(url);
    httplib::Client cli(host, port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);
    auto res = cli.Get(detail::path_of(url));
    if (!res) return {0, "connection failed"};
    return {res->status, res->body};
}

inline HttpResult real_http_post(const std::string& url, const std::string& body) {
    auto [host, port] = detail::split_host_port(url);
    httplib::Client cli(host, port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(10);
    auto res = cli.Post(detail::path_of(url), body, "application/json");
    if (!res) return {0, "connection failed"};
    return {res->status, res->body};
}

// ── HTTP response parsers (pure — testable without network) ───────────────────

inline bool parse_health(const HttpResult& r) {
    return r.status == 200;
}

inline boost::json::object parse_status(const HttpResult& r) {
    if (r.status != 200) {
        throw std::runtime_error("GET /status returned HTTP " + std::to_string(r.status));
    }
    return boost::json::parse(r.body).as_object();
}

inline boost::json::object parse_command_response(const HttpResult& r) {
    if (r.body.empty()) {
        throw std::runtime_error("POST /command: empty response, HTTP " + std::to_string(r.status));
    }
    return boost::json::parse(r.body).as_object();
}

inline boost::json::object parse_log_entry(const HttpResult& r) {
    if (r.status == 404) throw std::out_of_range("log index out of range");
    if (r.status != 200) {
        throw std::runtime_error("GET /log returned HTTP " + std::to_string(r.status));
    }
    return boost::json::parse(r.body).as_object();
}

// ── ChaosNode ─────────────────────────────────────────────────────────────────

class ChaosNode {
public:
    // Production ctor — uses real subprocess and HTTP calls.
    explicit ChaosNode(int node_id)
        : ChaosNode(node_id, os::real_exec, real_http_get, real_http_post) {}

    // Testable ctor — caller injects executor and HTTP stubs.
    ChaosNode(int node_id, os::CmdExecutor exec, HttpGet http_get, HttpPost http_post)
        : _node_id(node_id),
          _exec(std::move(exec)),
          _http_get(std::move(http_get)),
          _http_post(std::move(http_post)) {
        auto it = k_node_map.find(node_id);
        if (it == k_node_map.end()) {
            throw std::invalid_argument("ChaosNode: unknown node_id " + std::to_string(node_id));
        }
        _http_port = it->second.http_port;
        _fiu_port = static_cast<std::uint16_t>(it->second.fiu_port);
        _container = it->second.container_name;
    }

    int id() const { return _node_id; }
    const std::string& container() const { return _container; }

    // ── Status queries ─────────────────────────────────────────────────────

    boost::json::object status() { return parse_status(_http_get(http_url("/status"))); }

    bool is_healthy() {
        try {
            return parse_health(_http_get(http_url("/health")));
        } catch (...) {
            return false;
        }
    }

    bool is_leader() {
        try {
            auto s = status();
            return s["role"].as_string() == "leader";
        } catch (...) {
            return false;
        }
    }

    boost::json::object submit_command(const std::string& key, const std::string& value) {
        std::string body = R"({"key":")" + key + R"(","value":")" + value + R"("})";
        return parse_command_response(_http_post(http_url("/command"), body));
    }

    boost::json::object log_entry(int index) {
        return parse_log_entry(_http_get(http_url("/log/" + std::to_string(index))));
    }

    std::string container_ip() {
        auto out = os::checked_exec(_exec, os::container_ip_cmd(_container));
        while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
        return out;
    }

    // ── Application-layer faults (fiu_rc_tcp) ─────────────────────────────

    void enable_fault(std::string_view name, std::string_view mode = "always",
                      double probability = 1.0) {
        fiu::enable_fault("localhost", _fiu_port, name, mode, probability);
    }

    void disable_fault(std::string_view name) { fiu::disable_fault("localhost", _fiu_port, name); }

    void disable_all_faults() { fiu::disable_all_faults("localhost", _fiu_port); }

    // ── OS-layer network faults (docker exec) ─────────────────────────────

    void apply_tc_netem(const std::string& loss = "0%", const std::string& delay = "0ms",
                        const std::string& corrupt = "0%") {
        os::checked_exec(_exec, os::tc_netem_cmd(_container, loss, delay, corrupt));
    }

    void clear_tc_netem() { os::try_exec(_exec, os::tc_clear_cmd(_container)); }

    void partition_from(const std::vector<std::string>& peer_ips) {
        for (const auto& ip : peer_ips) {
            os::checked_exec(_exec, os::iptables_drop_src_cmd(_container, ip));
            os::checked_exec(_exec, os::iptables_drop_dst_cmd(_container, ip));
        }
    }

    void unpartition() {
        os::try_exec(_exec, os::iptables_flush_chaos_cmd(_container));
        os::try_exec(_exec, os::iptables_flush_output_cmd(_container));
    }

    // ── Container lifecycle ───────────────────────────────────────────────

    void kill() { os::checked_exec(_exec, os::docker_kill_cmd(_container)); }

    void stop() { os::checked_exec(_exec, os::docker_stop_cmd(_container)); }

    void restart(bool wait = true,
                 std::chrono::milliseconds health_timeout = std::chrono::seconds{15}) {
        os::checked_exec(_exec, os::docker_start_cmd(_container));
        if (!wait) return;
        auto deadline = std::chrono::steady_clock::now() + health_timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (is_healthy()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }
        throw std::runtime_error("node " + std::to_string(_node_id) +
                                 " did not become healthy after restart");
    }

private:
    std::string http_url(const std::string& path) const {
        return "http://localhost:" + std::to_string(_http_port) + path;
    }

    int _node_id;
    int _http_port;
    std::uint16_t _fiu_port;
    std::string _container;
    os::CmdExecutor _exec;
    HttpGet _http_get;
    HttpPost _http_post;
};

// ── ChaosCluster ─────────────────────────────────────────────────────────────

class ChaosCluster {
public:
    explicit ChaosCluster(std::string compose_file,
                          std::chrono::milliseconds startup_timeout = std::chrono::seconds{30})
        : ChaosCluster(std::move(compose_file), startup_timeout, os::real_exec, real_http_get,
                       real_http_post) {}

    ChaosCluster(std::string compose_file, std::chrono::milliseconds startup_timeout,
                 os::CmdExecutor exec, HttpGet http_get, HttpPost http_post)
        : _compose_file(std::move(compose_file)),
          _startup_timeout(startup_timeout),
          _exec(exec),
          _http_get(http_get),
          _http_post(http_post) {
        for (const auto& [id, _] : k_node_map) {
            _nodes.emplace(std::piecewise_construct, std::forward_as_tuple(id),
                           std::forward_as_tuple(id, exec, http_get, http_post));
        }
    }

    ~ChaosCluster() {
        // Best-effort cleanup — called by RAII even on test failure.
        try {
            stop();
        } catch (...) {
        }
    }

    void start() {
        os::checked_exec(_exec, os::compose_up_cmd(_compose_file));
        auto deadline = std::chrono::steady_clock::now() + _startup_timeout;
        for (auto& [id, node] : _nodes) {
            while (std::chrono::steady_clock::now() < deadline) {
                if (node.is_healthy()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds{500});
            }
            if (!node.is_healthy()) {
                throw std::runtime_error("node " + std::to_string(id) +
                                         " did not become healthy within startup timeout");
            }
        }
    }

    void stop() { os::try_exec(_exec, os::compose_down_cmd(_compose_file)); }

    ChaosNode& node(int id) {
        auto it = _nodes.find(id);
        if (it == _nodes.end()) {
            throw std::invalid_argument("ChaosCluster: unknown node_id " + std::to_string(id));
        }
        return it->second;
    }

    std::vector<ChaosNode*> all_nodes() {
        std::vector<ChaosNode*> result;
        result.reserve(_nodes.size());
        for (auto& [id, n] : _nodes) result.push_back(&n);
        return result;
    }

    ChaosNode& wait_for_leader(std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto& [id, n] : _nodes) {
                if (n.is_leader()) return n;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        throw std::runtime_error("no leader elected within timeout");
    }

    void assert_no_split_brain() {
        std::map<std::int64_t, int> term_leaders;  // term → node_id
        for (auto& [id, n] : _nodes) {
            try {
                auto s = n.status();
                if (s["role"].as_string() == "leader") {
                    std::int64_t term = s["term"].as_int64();
                    if (term_leaders.count(term)) {
                        throw std::runtime_error(
                            "split brain: nodes " + std::to_string(term_leaders[term]) + " and " +
                            std::to_string(id) + " both claim leadership in term " +
                            std::to_string(term));
                    }
                    term_leaders[term] = id;
                }
            } catch (const std::runtime_error&) {
                throw;
            } catch (...) {
                // Node temporarily unreachable — not a safety violation.
            }
        }
    }

    // Returns the last `tail_lines` log lines for the given node.
    std::vector<std::string> log_lines(int node_id, int tail_lines = 200) {
        auto out = _exec(os::docker_logs_cmd(k_node_map.at(node_id).container_name, tail_lines));
        std::vector<std::string> lines;
        std::istringstream ss(out.out);
        std::string line;
        while (std::getline(ss, line)) lines.push_back(std::move(line));
        return lines;
    }

    void wait_for_log(int node_id, std::string_view pattern,
                      std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& line : log_lines(node_id)) {
                if (line.find(pattern) != std::string::npos) return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        throw std::runtime_error("pattern not found in node " + std::to_string(node_id) +
                                 " logs: " + std::string(pattern));
    }

private:
    std::string _compose_file;
    std::chrono::milliseconds _startup_timeout;
    os::CmdExecutor _exec;
    HttpGet _http_get;
    HttpPost _http_post;
    std::map<int, ChaosNode> _nodes;
};

// ── Test fixture helpers ──────────────────────────────────────────────────────

inline std::string default_compose_file() {
    const char* env = std::getenv("KYTHIRA_COMPOSE_FILE");
    if (env && *env) return env;
    return "docker/docker-compose.yml";
}

// Per-test-case fixture: starts a fresh cluster in ctor, stops in dtor.
struct ChaosFixture {
    ChaosCluster cluster;

    ChaosFixture() : cluster(default_compose_file(), std::chrono::seconds{30}) {
        cluster.start();
        cluster.wait_for_leader(std::chrono::seconds{15});
    }

    ~ChaosFixture() {
        try {
            cluster.stop();
        } catch (...) {
        }
    }
};

}  // namespace docker_chaos
