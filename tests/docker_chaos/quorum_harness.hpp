#pragma once

#include "harness.hpp"
#include "os_faults.hpp"

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace docker_chaos {

using namespace std::chrono_literals;

// ── Port layout for the quorum compose file ───────────────────────────────────
// Mirrors docker/docker-compose.quorum.yml port bindings.

inline const std::map<int, NodePorts> k_quorum_node_map{
    {1, {7101, 8181, 9101, ""}},  // container name is cluster-specific; set per-fixture
    {2, {7102, 8182, 9102, ""}},
    {3, {7103, 8183, 9103, ""}},
};

// ── QuorumHealingFixture ──────────────────────────────────────────────────────
//
// Per-test fixture for quorum healing tests.  Each instance uses a unique
// QUORUM_CLUSTER name derived from the test name (passed as the constructor
// argument) so that concurrent or interrupted runs don't interfere.
//
// Usage:
//   struct MyTest : QuorumHealingFixture {
//       MyTest() : QuorumHealingFixture("my_test") {}
//   };

class QuorumHealingFixture {
public:
    // cluster_suffix is appended to "kythira-quorum-" to form the unique
    // cluster name for this test run.
    explicit QuorumHealingFixture(std::string cluster_suffix)
        : _cluster_name("kythira-quorum-" + cluster_suffix),
          _exec(os::real_exec),
          _compose_file(default_quorum_compose_file()) {
        _setup_env();
        _start_cluster();
    }

    ~QuorumHealingFixture() { _teardown(); }

    // ── Pause / unpause ───────────────────────────────────────────────────────

    void pause(int node_id) {
        os::checked_exec(_exec, {os::container_runtime(), "pause", container_name(node_id)});
    }

    void unpause(int node_id) {
        os::checked_exec(_exec, {os::container_runtime(), "unpause", container_name(node_id)});
    }

    // ── Cluster-size polling ──────────────────────────────────────────────────

    // Polls `docker ps --filter label=kythira.cluster=<cluster_name>` until
    // exactly `n` containers are in Running state or `timeout` expires.
    // Returns true if the target was reached, false on timeout.
    bool wait_for_cluster_size(std::size_t n, std::chrono::milliseconds timeout = 60s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (_running_container_count() == n) {
                return true;
            }
            std::this_thread::sleep_for(1s);
        }
        return false;
    }

    // Throws if `wait_for_cluster_size` does not succeed within timeout.
    void assert_cluster_size(std::size_t n, std::chrono::milliseconds timeout = 60s) {
        if (!wait_for_cluster_size(n, timeout)) {
            throw std::runtime_error("cluster did not reach size " + std::to_string(n) +
                                     " within timeout (cluster=" + _cluster_name + ")");
        }
    }

    // Throws if the container kythira-{cluster_name}-{node_id} still exists.
    void assert_container_absent(std::uint64_t node_id) {
        auto name = _cluster_name + "-" + std::to_string(node_id);
        auto res =
            _exec({os::container_runtime(), "inspect", "--format", "{{.State.Status}}", name});
        if (res.code == 0) {
            throw std::runtime_error("container " + name +
                                     " still exists (expected decommissioned)");
        }
    }

    // Returns the container name for the given initial node ID
    // (nodes 1-3 that started with the compose file).
    [[nodiscard]] std::string container_name(int initial_node_id) const {
        return _cluster_name + "-chaos-" + std::to_string(initial_node_id);
    }

    // ── ChaosNode access ─────────────────────────────────────────────────────

    // Returns a ChaosNode for one of the three original compose nodes.
    // Port layout is fixed per docker-compose.quorum.yml.
    ChaosNode& node(int id) {
        auto it = _nodes.find(id);
        if (it == _nodes.end()) {
            throw std::invalid_argument("QuorumHealingFixture: unknown node_id " +
                                        std::to_string(id));
        }
        return it->second;
    }

    // Waits for a leader to be elected among the three original nodes.
    ChaosNode& wait_for_leader(std::chrono::milliseconds timeout = 30s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto& [id, n] : _nodes) {
                if (n.is_leader()) {
                    return n;
                }
            }
            std::this_thread::sleep_for(200ms);
        }
        throw std::runtime_error("no leader elected within timeout");
    }

    // Verifies no two nodes claim leadership in the same term.
    void assert_no_split_brain() {
        std::map<std::int64_t, int> term_leaders;
        for (auto& [id, n] : _nodes) {
            try {
                auto s = n.status();
                if (s["role"].as_string() == "leader") {
                    std::int64_t term = s["term"].as_int64();
                    if (term_leaders.contains(term) != 0u) {
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

    [[nodiscard]] const std::string& cluster_name() const { return _cluster_name; }

private:
    std::string _cluster_name;
    os::CmdExecutor _exec;
    std::string _compose_file;
    std::map<int, ChaosNode> _nodes;

    static std::string default_quorum_compose_file() {
        const char* env = std::getenv("KYTHIRA_QUORUM_COMPOSE_FILE");
        if ((env != nullptr) && (*env != 0)) {
            return env;
        }
        return "docker/docker-compose.quorum.yml";
    }

    // Set environment variables that docker-compose.quorum.yml substitutes.
    void _setup_env() {
        ::setenv("QUORUM_CLUSTER", _cluster_name.c_str(), /*overwrite=*/1);
        ::setenv("QUORUM_NETWORK", (_cluster_name + "-net").c_str(), 1);
    }

    void _start_cluster() {
        // Build ChaosNode entries using the fixed quorum port layout.
        // The direct ChaosNode constructor is used so that the cluster-specific
        // container names and quorum ports (7101-7103 / 8181-8183) are respected
        // rather than the defaults from k_node_map.
        for (const auto& [id, ports] : k_quorum_node_map) {
            auto cname = container_name(id);
            _nodes.emplace(std::piecewise_construct, std::forward_as_tuple(id),
                           std::forward_as_tuple(id, ports.http_port,
                                                 static_cast<std::uint16_t>(ports.fiu_port), cname,
                                                 _exec, real_http_get, real_http_post));
        }

        // Bring up the compose stack with the cluster-specific name
        auto prefix = os::compose_prefix();
        std::vector<std::string> up_cmd = prefix;
        up_cmd.insert(up_cmd.end(), {"-f", _compose_file, "-p", _cluster_name, "up", "-d"});
        os::checked_exec(_exec, up_cmd);

        // Wait for all three nodes to report healthy
        auto deadline = std::chrono::steady_clock::now() + 60s;
        for (auto& [id, n] : _nodes) {
            while (std::chrono::steady_clock::now() < deadline) {
                if (n.is_healthy()) {
                    break;
                }
                std::this_thread::sleep_for(500ms);
            }
            if (!n.is_healthy()) {
                throw std::runtime_error("quorum node " + std::to_string(id) +
                                         " did not become healthy");
            }
        }
    }

    void _teardown() {
        // Req 19 AC 12 — force-remove all containers for this cluster name
        try {
            const auto& rt = os::container_runtime();
            // List all container IDs for this cluster
            auto res =
                _exec({rt, "ps", "-aq", "--filter", "label=kythira.cluster=" + _cluster_name});
            if (res.code == 0 && !res.out.empty()) {
                // Split on whitespace and remove each
                std::vector<std::string> rm_cmd = {rt, "rm", "--force"};
                std::istringstream ss(res.out);
                std::string id;
                while (ss >> id) {
                    rm_cmd.push_back(id);
                }
                if (rm_cmd.size() > 3) {
                    os::try_exec(_exec, rm_cmd);
                }
            }
        } catch (...) {
        }

        // Also bring down the compose stack
        try {
            auto prefix = os::compose_prefix();
            std::vector<std::string> down_cmd = prefix;
            down_cmd.insert(down_cmd.end(),
                            {"-f", _compose_file, "-p", _cluster_name, "down", "--volumes"});
            os::try_exec(_exec, down_cmd);
        } catch (...) {
        }

        ::unsetenv("QUORUM_CLUSTER");
        ::unsetenv("QUORUM_NETWORK");
    }

    // Count containers in Running state with the kythira.cluster label.
    std::size_t _running_container_count() {
        auto res = _exec({os::container_runtime(), "ps", "--filter",
                          "label=kythira.cluster=" + _cluster_name, "--filter", "status=running",
                          "--quiet"});
        if (res.code != 0 || res.out.empty()) {
            return 0;
        }
        std::size_t count = 0;
        std::istringstream ss(res.out);
        std::string tok;
        while (ss >> tok) {
            ++count;
        }
        return count;
    }
};

}  // namespace docker_chaos
