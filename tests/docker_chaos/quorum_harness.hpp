// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "harness.hpp"
#include "os_faults.hpp"

#include <chrono>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace docker_chaos {

using namespace std::chrono_literals;

// ── Port layout for the quorum compose file ───────────────────────────────────
// Mirrors the port bindings of docker/docker-compose.quorum.yml (nodes 1-3)
// and docker/docker-compose.quorum-5.yml (nodes 1-5).

inline const std::map<int, NodePorts> k_quorum_node_map{
    {1, {7101, 8181, 9101, ""}},  // container name is cluster-specific; set per-fixture
    {2, {7102, 8182, 9102, ""}}, {3, {7103, 8183, 9103, ""}},
    {4, {7104, 8184, 9104, ""}}, {5, {7105, 8185, 9105, ""}},
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
    // cluster name for this test run.  node_count is 3 (docker-compose.quorum.yml)
    // or 5 (docker-compose.quorum-5.yml), and is also the quorum target.
    explicit QuorumHealingFixture(std::string cluster_suffix, std::size_t node_count = 3)
        : _cluster_name("kythira-quorum-" + cluster_suffix),
          _node_count(node_count),
          _exec(os::real_exec),
          _compose_file(default_quorum_compose_file(node_count)) {
        if (node_count != 3 && node_count != 5) {
            throw std::invalid_argument("QuorumHealingFixture: node_count must be 3 or 5");
        }
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
            if (running_container_count() == n) {
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
        auto name = container_name(node_id);
        auto res =
            _exec({os::container_runtime(), "inspect", "--format", "{{.State.Status}}", name});
        if (res.code == 0) {
            throw std::runtime_error("container " + name +
                                     " still exists (expected decommissioned)");
        }
    }

    // Polls until the container kythira-{cluster_name}-{node_id} no longer
    // exists. The leader decommissions a failed node only after its
    // replacement has joined, been promoted and the failed node removed from
    // the configuration, so this trails wait_for_cluster_size() by a few
    // quorum checks.
    // Checks at least once, so a zero timeout is a non-blocking probe.
    bool wait_for_container_absent(std::uint64_t node_id, std::chrono::milliseconds timeout = 60s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            auto res = _exec({os::container_runtime(), "inspect", "--format", "{{.State.Status}}",
                              container_name(node_id)});
            if (res.code != 0) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(1s);
        }
    }

    // Container name for a node ID: kythira-{cluster_name}-{node_id}, the
    // scheme docker_quorum_manager uses for the containers it provisions and
    // docker-compose.quorum.yml uses for the bootstrap nodes.
    [[nodiscard]] std::string container_name(std::uint64_t node_id) const {
        return "kythira-" + _cluster_name + "-" + std::to_string(node_id);
    }

    // ── ChaosNode access ─────────────────────────────────────────────────────

    // Returns a ChaosNode for one of the original compose nodes.
    // Port layout is fixed per docker-compose.quorum.yml.
    ChaosNode& node(int id) {
        auto it = _nodes.find(id);
        if (it == _nodes.end()) {
            throw std::invalid_argument("QuorumHealingFixture: unknown node_id " +
                                        std::to_string(id));
        }
        return it->second;
    }

    // Waits for a leader to be elected among the original nodes.
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
                // Node temporarily unreachable, e.g. the one the test just
                // killed: status() throws a plain std::runtime_error for
                // it, which is not a safety violation.
            }
        }
    }

    [[nodiscard]] const std::string& cluster_name() const { return _cluster_name; }

    // Count containers in Running state with the kythira.cluster label.
    std::size_t running_container_count() {
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

    // Prints, to stderr, every container this cluster owns: the compose
    // project's (the three bootstrap nodes and the container API proxy) and
    // the ones docker_quorum_manager provisioned (labelled but outside the
    // project), each with its state and the tail of its logs. A healing
    // failure is decided inside the nodes and the proxy, so without this the
    // only evidence is a timeout. `compose ps -a` is not used: podman-compose
    // rejects `-a` (see metrics_scenario_support.hpp).
    //
    // A node logs a DEBUG line per heartbeat and an [ErrorHandler] line per
    // retry, so the last 60 lines of a stalled node were all noise and the
    // state change that mattered had scrolled off. Those are dropped and
    // the last 80 remaining lines printed.
    void dump_diagnostics() {
        const auto& rt = os::container_runtime();
        auto ps = _exec({rt, "ps", "-a", "--filter", "label=kythira.cluster=" + _cluster_name,
                         "--format", "{{.Names}} {{.Status}}"});
        std::cerr << "── quorum cluster " << _cluster_name << " containers ──\n" << ps.out;

        std::vector<std::string> ids_cmd = os::compose_prefix();
        ids_cmd.insert(ids_cmd.end(), {"-f", _compose_file, "-p", _cluster_name, "ps", "-q"});
        auto ids = _exec(ids_cmd);
        auto labelled =
            _exec({rt, "ps", "-aq", "--filter", "label=kythira.cluster=" + _cluster_name});
        std::set<std::string> seen;
        std::istringstream ss(ids.out + "\n" + labelled.out);
        std::string id;
        while (ss >> id) {
            if (!seen.insert(id.substr(0, 12)).second) {
                continue;
            }
            auto name = _exec({rt, "inspect", "--format", "{{.Name}} {{.State.Status}}", id});
            auto logs = _exec({rt, "logs", "--tail", "2000", id});
            std::cerr << "── logs for " << name.out << without_noise(logs.out, 80) << "\n";
        }
    }

    static std::string without_noise(const std::string& logs, std::size_t keep) {
        std::vector<std::string> kept;
        std::istringstream in(logs);
        std::string line;
        while (std::getline(in, line)) {
            if (line.find(" DEBUG: ") != std::string::npos ||
                line.find("[ErrorHandler]") != std::string::npos) {
                continue;
            }
            kept.push_back(line);
        }
        std::string out;
        for (std::size_t i = kept.size() > keep ? kept.size() - keep : 0; i < kept.size(); ++i) {
            out += kept[i] + '\n';
        }
        return out;
    }

private:
    std::string _cluster_name;
    std::size_t _node_count;
    os::CmdExecutor _exec;
    std::string _compose_file;
    std::map<int, ChaosNode> _nodes;

    static std::string default_quorum_compose_file(std::size_t node_count) {
        const char* env = std::getenv(node_count == 5 ? "KYTHIRA_QUORUM5_COMPOSE_FILE"
                                                      : "KYTHIRA_QUORUM_COMPOSE_FILE");
        if ((env != nullptr) && (*env != 0)) {
            return env;
        }
        return node_count == 5 ? "docker/docker-compose.quorum-5.yml"
                               : "docker/docker-compose.quorum.yml";
    }

    // Set environment variables that docker-compose.quorum.yml substitutes.
    void _setup_env() {
        ::setenv("QUORUM_CLUSTER", _cluster_name.c_str(), /*overwrite=*/1);
        ::setenv("QUORUM_NETWORK", (_cluster_name + "-net").c_str(), 1);
        ::setenv("QUORUM_TARGET", std::to_string(_node_count).c_str(), 1);
    }

    void _start_cluster() {
        // Build ChaosNode entries using the fixed quorum port layout.
        // The direct ChaosNode constructor is used so that the cluster-specific
        // container names and quorum ports (7101-7103 / 8181-8183) are respected
        // rather than the defaults from k_node_map.
        for (const auto& [id, ports] : k_quorum_node_map) {
            if (static_cast<std::size_t>(id) > _node_count) {
                continue;
            }
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

        // Wait for every bootstrap node to report healthy
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
        ::unsetenv("QUORUM_TARGET");
    }
};

}  // namespace docker_chaos
