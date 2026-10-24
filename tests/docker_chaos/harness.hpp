// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "fault_control.hpp"
#include "os_faults.hpp"

#include <httplib.h>

#include <boost/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <sstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace docker_chaos {

using namespace std::chrono_literals;

// Dedicated type for ChaosCluster::assert_no_split_brain()'s own detection
// throw, distinct from the plain std::runtime_error an unreachable node's
// status()/parse_status() (harness.hpp) throws — assert_no_split_brain()
// needs to propagate the former (a real safety violation) while swallowing
// the latter (an expected, benign "node is deliberately dead right now",
// e.g. right after a chaos test's own kill()/stop() call). Both being
// plain std::runtime_error made that impossible to distinguish by type
// alone.
struct split_brain_detected : std::runtime_error {
    using std::runtime_error::runtime_error;
};

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

// ── 5-node layout — mirrors docker/chaos-5-compose.yml ───────────────────────
//
// Host ports 7201-7205 / 8281-8285 / 9201-9205, clear of the 3-node file's
// 70xx/80xx/90xx and the quorum files' 71xx/81xx/91xx. Container names are
// "<cluster>-<id>", where <cluster> is $CHAOS5_CLUSTER, the same variable
// the compose file reads, with the same default. harness_unit_test parses
// the compose file and checks it against this table, so the two cannot
// drift apart silently.

inline constexpr std::string_view k_chaos5_default_cluster = "kythira-chaos5";

inline std::string chaos5_cluster_name() {
    const char* env = std::getenv("CHAOS5_CLUSTER");
    if ((env != nullptr) && (*env != 0)) {
        return env;
    }
    return std::string(k_chaos5_default_cluster);
}

inline std::map<int, NodePorts> chaos5_node_table(const std::string& cluster) {
    std::map<int, NodePorts> table;
    for (int id = 1; id <= 5; ++id) {
        table.emplace(
            id, NodePorts{7200 + id, 8280 + id, 9200 + id, cluster + "-" + std::to_string(id)});
    }
    return table;
}

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
    if (colon == std::string::npos) {
        return {host_port, 80};
    }
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
    if (!res) {
        return {0, "connection failed"};
    }
    return {res->status, res->body};
}

inline HttpResult real_http_post(const std::string& url, const std::string& body) {
    auto [host, port] = detail::split_host_port(url);
    httplib::Client cli(host, port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(10);
    auto res = cli.Post(detail::path_of(url), body, "application/json");
    if (!res) {
        return {0, "connection failed"};
    }
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
    if (r.status == 404) {
        // The body says why: "compacted", "beyond_end" or "missing".
        throw std::out_of_range("log index out of range: " + r.body);
    }
    if (r.status != 200) {
        throw std::runtime_error("GET /log returned HTTP " + std::to_string(r.status));
    }
    return boost::json::parse(r.body).as_object();
}

namespace detail {
inline boost::json::object parse_200(const HttpResult& r, const char* what) {
    if (r.status != 200) {
        throw std::runtime_error(std::string(what) + " returned HTTP " + std::to_string(r.status) +
                                 ": " + r.body);
    }
    return boost::json::parse(r.body).as_object();
}
}  // namespace detail

// GET /log?from=&to= → {from,to,snapshot_index,last_log_index,entries[]}
inline boost::json::object parse_log_range(const HttpResult& r) {
    return detail::parse_200(r, "GET /log?from=&to=");
}

// GET /elections → {node_id,elections[{term,at_ms}]}
inline boost::json::object parse_elections(const HttpResult& r) {
    return detail::parse_200(r, "GET /elections");
}

// GET /kv/<key> → {key,mode,found,value,commit_index,last_applied}
inline boost::json::object parse_kv_read(const HttpResult& r) {
    return detail::parse_200(r, "GET /kv");
}

// POST /membership/{add,remove} → {success,commit_index} or {error,detail}.
// Like parse_command_response, a refusal is returned rather than thrown: 409
// (another change pending) and 503 are outcomes a scenario asserts on.
inline boost::json::object parse_membership_response(const HttpResult& r) {
    if (r.body.empty()) {
        throw std::runtime_error("POST /membership: empty response, HTTP " +
                                 std::to_string(r.status));
    }
    auto obj = boost::json::parse(r.body).as_object();
    obj["http_status"] = r.status;
    return obj;
}

// A fault this host cannot inject, reported as such rather than as a fault
// that took effect (cairn-chaos-testing Requirement 2.5 and task 3.4).
class fault_unsupported : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

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

    // Direct ctor — bypasses k_node_map; used when port layout and container
    // names differ from the defaults (e.g. the quorum healing harness).
    ChaosNode(int node_id, int http_port, std::uint16_t fiu_port, std::string container,
              os::CmdExecutor exec, HttpGet http_get, HttpPost http_post)
        : _node_id(node_id),
          _exec(std::move(exec)),
          _http_get(std::move(http_get)),
          _http_post(std::move(http_post)),
          _http_port(http_port),
          _fiu_port(fiu_port),
          _container(std::move(container)) {}

    // Node-table ctor — the ports and container name come from a NodePorts
    // row, so any layout ChaosCluster is given reaches its nodes unchanged.
    ChaosNode(int node_id, const NodePorts& ports, os::CmdExecutor exec, HttpGet http_get,
              HttpPost http_post)
        : ChaosNode(node_id, ports.http_port, static_cast<std::uint16_t>(ports.fiu_port),
                    ports.container_name, std::move(exec), std::move(http_get),
                    std::move(http_post)) {}

    [[nodiscard]] int id() const { return _node_id; }
    [[nodiscard]] int http_port() const { return _http_port; }
    [[nodiscard]] std::uint16_t fiu_port() const { return _fiu_port; }
    [[nodiscard]] const std::string& container() const { return _container; }

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

    // POST /command {"op":"cas"}; std::nullopt expects the key to be absent.
    // The reply's "applied" says whether the swap happened.
    boost::json::object submit_cas(const std::string& key,
                                   const std::optional<std::string>& expected,
                                   const std::string& value) {
        boost::json::object body{{"op", "cas"}, {"key", key}, {"value", value}};
        body["expected"] = expected ? boost::json::value(*expected) : boost::json::value(nullptr);
        return parse_command_response(
            _http_post(http_url("/command"), boost::json::serialize(body)));
    }

    boost::json::object log_range(std::uint64_t from, std::uint64_t to) {
        return parse_log_range(
            _http_get(http_url("/log?from=" + std::to_string(from) + "&to=" + std::to_string(to))));
    }

    boost::json::object elections() { return parse_elections(_http_get(http_url("/elections"))); }

    /// Best effort: a node that is already down or unreachable has nothing
    /// to save, and the fault about to be applied must still be applied.
    void remember_elections() {
        try {
            _remembered_elections.push_back(elections());
        } catch (...) {
        }
    }

    /// Every /elections body saved before this node was killed or stopped.
    [[nodiscard]] const std::vector<boost::json::object>& remembered_elections() const {
        return _remembered_elections;
    }

    // mode is "linearizable" or "local". The key is sent as-is, so keep
    // chaos keys to URL-safe characters.
    boost::json::object kv_read(const std::string& key, const std::string& mode = "linearizable") {
        return parse_kv_read(_http_get(http_url("/kv/" + key + "?mode=" + mode)));
    }

    boost::json::object membership_add(int node_id) {
        return parse_membership_response(_http_post(
            http_url("/membership/add"), R"({"node_id":)" + std::to_string(node_id) + "}"));
    }

    boost::json::object membership_remove(int node_id) {
        return parse_membership_response(_http_post(
            http_url("/membership/remove"), R"({"node_id":)" + std::to_string(node_id) + "}"));
    }

    std::string container_ip() {
        auto out = os::checked_exec(_exec, os::container_ip_cmd(_container));
        while (!out.empty() && (std::isspace(static_cast<unsigned char>(out.back())) != 0)) {
            out.pop_back();
        }
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

    // One qdisc each; see os::tc_delay_cmd / os::tc_loss_cmd.
    void apply_delay(int delay_ms, int jitter_ms = 0) {
        if (delay_ms < 0 || jitter_ms < 0) {
            throw std::invalid_argument("apply_delay: negative delay or jitter");
        }
        os::checked_exec(_exec, os::tc_delay_cmd(_container, delay_ms, jitter_ms));
    }

    void apply_loss(double loss_pct) {
        if (std::isnan(loss_pct) || loss_pct < 0.0 || loss_pct > 100.0) {
            throw std::invalid_argument("apply_loss: percentage outside [0, 100]");
        }
        os::checked_exec(_exec, os::tc_loss_cmd(_container, loss_pct));
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

    // kill() and stop() first save the node's /elections: the node keeps
    // them in memory, so a restart loses every term it won before. A
    // checker reads remembered_elections() alongside the live ones.
    void kill() {
        remember_elections();
        os::checked_exec(_exec, os::docker_kill_cmd(_container));
    }

    void stop() {
        remember_elections();
        os::checked_exec(_exec, os::docker_stop_cmd(_container));
    }

    // Throws fault_unsupported on a host that cannot freeze containers
    // (rootless Podman on cgroup v1), never returns as if the node paused.
    void pause() {
        auto result = _exec(os::pause_cmd(_container));
        if (result.code == 0) {
            return;
        }
        if (os::is_pause_unsupported(result.out)) {
            throw fault_unsupported("pause is unsupported on this host: " + result.out);
        }
        throw std::runtime_error("pause " + _container + " failed (exit " +
                                 std::to_string(result.code) + "): " + result.out);
    }

    void unpause() { os::checked_exec(_exec, os::unpause_cmd(_container)); }

    void restart(bool wait = true,
                 std::chrono::milliseconds health_timeout = std::chrono::seconds{15}) {
        os::checked_exec(_exec, os::docker_start_cmd(_container));
        if (!wait) {
            return;
        }
        auto deadline = std::chrono::steady_clock::now() + health_timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (is_healthy()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }
        throw std::runtime_error("node " + std::to_string(_node_id) +
                                 " did not become healthy after restart");
    }

private:
    [[nodiscard]] std::string http_url(const std::string& path) const {
        return "http://localhost:" + std::to_string(_http_port) + path;
    }

    int _node_id;
    int _http_port;
    std::uint16_t _fiu_port;
    std::string _container;
    os::CmdExecutor _exec;
    HttpGet _http_get;
    HttpPost _http_post;
    std::vector<boost::json::object> _remembered_elections;
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
        : ChaosCluster(std::move(compose_file), k_node_map, startup_timeout, std::move(exec),
                       std::move(http_get), std::move(http_post)) {}

    // Node-table ctors (cairn-chaos-testing Requirement 2.1): any number of
    // nodes, any ports and container names. The two above are this with
    // k_node_map, so the 3-node default and its callers are unchanged.
    ChaosCluster(std::string compose_file, const std::map<int, NodePorts>& node_table,
                 std::chrono::milliseconds startup_timeout = std::chrono::seconds{30})
        : ChaosCluster(std::move(compose_file), node_table, startup_timeout, os::real_exec,
                       real_http_get, real_http_post) {}

    ChaosCluster(std::string compose_file, const std::map<int, NodePorts>& node_table,
                 std::chrono::milliseconds startup_timeout, os::CmdExecutor exec, HttpGet http_get,
                 HttpPost http_post)
        : _compose_file(std::move(compose_file)),
          _startup_timeout(startup_timeout),
          _exec(exec),
          _http_get(http_get),
          _http_post(http_post) {
        if (node_table.empty()) {
            throw std::invalid_argument("ChaosCluster: empty node table");
        }
        for (const auto& [id, ports] : node_table) {
            _nodes.emplace(std::piecewise_construct, std::forward_as_tuple(id),
                           std::forward_as_tuple(id, ports, exec, http_get, http_post));
        }
    }

    ~ChaosCluster() {
        // Best-effort cleanup — called by RAII even on test failure.
        if (_detached) {
            return;
        }
        try {
            stop();
        } catch (...) {
        }
    }

    ChaosCluster(const ChaosCluster&) = delete;
    ChaosCluster& operator=(const ChaosCluster&) = delete;

    // Leaves the cluster running when this object goes away. chaos_ctl
    // (cmd/chaos_ctl/) addresses one long-lived cluster from a new process
    // per call, so no one call's object may take it down.
    void detach() { _detached = true; }

    void start() {
        os::checked_exec(_exec, os::compose_up_cmd(_compose_file));
        auto deadline = std::chrono::steady_clock::now() + _startup_timeout;
        for (auto& [id, node] : _nodes) {
            while (std::chrono::steady_clock::now() < deadline) {
                if (node.is_healthy()) {
                    break;
                }
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
        for (auto& [id, n] : _nodes) {
            result.push_back(&n);
        }
        return result;
    }

    ChaosNode& wait_for_leader(std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto& [id, n] : _nodes) {
                if (n.is_leader()) {
                    // A node can win an election and then immediately lose
                    // leadership again — most visibly right after a cold
                    // cluster start, where every node's election timer fires
                    // close together (ELECTION_TIMEOUT_MIN/MAX_MS is only
                    // 150-300ms in docker-compose.yml). Re-check after
                    // roughly one heartbeat interval so callers get a node
                    // that is actually still leader by the time they act on
                    // it, not one caught mid-election-churn.
                    std::this_thread::sleep_for(std::chrono::milliseconds{100});
                    if (n.is_leader()) {
                        return n;
                    }
                }
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
                    if (static_cast<unsigned int>(term_leaders.contains(term)) != 0u) {
                        throw split_brain_detected(
                            "split brain: nodes " + std::to_string(term_leaders[term]) + " and " +
                            std::to_string(id) + " both claim leadership in term " +
                            std::to_string(term));
                    }
                    term_leaders[term] = id;
                }
            } catch (const split_brain_detected&) {
                throw;
            } catch (...) {
                // Node temporarily unreachable (e.g. deliberately killed by
                // the calling test moments earlier) — not a safety
                // violation. Was `catch (const std::runtime_error&) {
                // throw; }`, which mis-propagated status()/parse_status()'s
                // own plain std::runtime_error("GET /status returned HTTP
                // 0") for an unreachable node as if it were this function's
                // split-brain detection — the two throws were
                // indistinguishable by type alone before
                // split_brain_detected existed.
            }
        }
    }

    // Returns the last `tail_lines` log lines for the given node.
    std::vector<std::string> log_lines(int node_id, int tail_lines = 200) {
        auto out = _exec(os::docker_logs_cmd(node(node_id).container(), tail_lines));
        std::vector<std::string> lines;
        std::istringstream ss(out.out);
        std::string line;
        while (std::getline(ss, line)) {
            lines.push_back(std::move(line));
        }
        return lines;
    }

    void wait_for_log(int node_id, std::string_view pattern,
                      std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& line : log_lines(node_id)) {
                if (line.find(pattern) != std::string::npos) {
                    return;
                }
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
    bool _detached{false};
};

// ── Test fixture helpers ──────────────────────────────────────────────────────

inline std::string default_compose_file() {
    const char* env = std::getenv("KYTHIRA_COMPOSE_FILE");
    if ((env != nullptr) && (*env != 0)) {
        return env;
    }
    return "docker/docker-compose.yml";
}

inline std::string default_chaos5_compose_file() {
    const char* env = std::getenv("KYTHIRA_CHAOS5_COMPOSE_FILE");
    if ((env != nullptr) && (*env != 0)) {
        return env;
    }
    return "docker/chaos-5-compose.yml";
}

// ChaosFixture, the per-test-case fixture, is in chaos_fixture.hpp: it runs
// the cairn checkers, which build on this header.

}  // namespace docker_chaos
