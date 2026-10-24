// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE harness_unit_test
#include <boost/test/unit_test.hpp>

#include "harness.hpp"
#include "os_faults.hpp"

#include <fstream>
#include <map>
#include <string>
#include <vector>

// ── Helpers ───────────────────────────────────────────────────────────────────

// Mock executor that records all command invocations.
struct MockExec {
    std::vector<std::vector<std::string>> calls;
    int return_code{0};
    std::string return_out;

    docker_chaos::os::CmdResult operator()(const std::vector<std::string>& cmd) {
        calls.push_back(cmd);
        return {return_code, return_out};
    }
};

// Mock HTTP GET that always returns the provided result.
struct MockGet {
    docker_chaos::HttpResult result;
    std::vector<std::string> called_urls;

    docker_chaos::HttpResult operator()(const std::string& url) {
        called_urls.push_back(url);
        return result;
    }
};

// Mock HTTP POST that always returns the provided result.
struct MockPost {
    docker_chaos::HttpResult result{200, R"({"success":true,"commit_index":1})"};

    docker_chaos::HttpResult operator()(const std::string& /*url*/, const std::string& /*body*/) {
        return result;
    }
};

static docker_chaos::HttpResult healthy_get(const std::string&) {
    return {200, R"({"status":"running"})"};
}

static docker_chaos::HttpResult follower_get(const std::string& url) {
    if (url.find("/status") != std::string::npos) {
        return {200,
                R"({"node_id":1,"role":"follower","term":1,"commit_index":0,"last_applied":0})"};
    }
    return {200, R"({"status":"running"})"};
}

static docker_chaos::HttpResult leader_get(const std::string& url) {
    if (url.find("/status") != std::string::npos) {
        return {200, R"({"node_id":1,"role":"leader","term":2,"commit_index":5,"last_applied":5})"};
    }
    return {200, R"({"status":"running"})"};
}

// ── ChaosCluster unit tests ───────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(chaos_cluster_unit)

BOOST_AUTO_TEST_CASE(construction_issues_no_commands) {
    MockExec exec;
    docker_chaos::ChaosCluster cluster(
        "docker/docker-compose.yml", std::chrono::seconds{10}, std::ref(exec), healthy_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, "{}"};
        });
    BOOST_TEST(exec.calls.empty());
}

BOOST_AUTO_TEST_CASE(start_runs_docker_compose_up) {
    MockExec exec;
    docker_chaos::ChaosCluster cluster(
        "docker/docker-compose.yml", std::chrono::seconds{10}, std::ref(exec), healthy_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, "{}"};
        });

    cluster.start();

    // First call must be docker compose up
    BOOST_REQUIRE(!exec.calls.empty());
    auto& first = exec.calls[0];
    BOOST_TEST(first[0] == "docker");
    BOOST_TEST(first[1] == "compose");
    BOOST_TEST(first[2] == "-f");
    BOOST_TEST(first[3] == "docker/docker-compose.yml");
    BOOST_TEST(first[4] == "up");
    BOOST_TEST(first[5] == "-d");
}

BOOST_AUTO_TEST_CASE(stop_runs_docker_compose_down) {
    MockExec exec;
    docker_chaos::ChaosCluster cluster(
        "docker/docker-compose.yml", std::chrono::seconds{10}, std::ref(exec), healthy_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, "{}"};
        });

    cluster.stop();

    BOOST_REQUIRE(!exec.calls.empty());
    auto& last = exec.calls.back();
    BOOST_TEST(last[0] == "docker");
    BOOST_TEST(last[1] == "compose");
    BOOST_TEST(last.back() == "--remove-orphans");
}

BOOST_AUTO_TEST_CASE(wait_for_leader_returns_leader_node) {
    MockExec exec;
    // Node 1 is the leader, others are followers
    auto mixed_get = [](const std::string& url) -> docker_chaos::HttpResult {
        if (url.find(":8081") != std::string::npos && url.find("/status") != std::string::npos) {
            return {200,
                    R"({"node_id":1,"role":"leader","term":2,"commit_index":0,"last_applied":0})"};
        }
        if (url.find("/status") != std::string::npos) {
            return {
                200,
                R"({"node_id":2,"role":"follower","term":2,"commit_index":0,"last_applied":0})"};
        }
        return {200, R"({"status":"running"})"};
    };

    docker_chaos::ChaosCluster cluster(
        "docker/docker-compose.yml", std::chrono::seconds{5}, std::ref(exec), mixed_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, "{}"};
        });

    auto& leader = cluster.wait_for_leader(std::chrono::seconds{2});
    BOOST_TEST(leader.id() == 1);
}

BOOST_AUTO_TEST_CASE(wait_for_leader_throws_on_timeout) {
    MockExec exec;
    docker_chaos::ChaosCluster cluster(
        "docker/docker-compose.yml", std::chrono::seconds{5}, std::ref(exec), follower_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, "{}"};
        });

    BOOST_CHECK_THROW(cluster.wait_for_leader(std::chrono::seconds{1}), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ── ChaosNode unit tests ──────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(chaos_node_unit)

BOOST_AUTO_TEST_CASE(apply_tc_netem_correct_command) {
    MockExec exec;
    docker_chaos::ChaosNode n(
        1, std::ref(exec), leader_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, R"({"success":true,"commit_index":1})"};
        });

    n.apply_tc_netem("30%", "50ms", "1%");

    BOOST_REQUIRE(!exec.calls.empty());
    auto& cmd = exec.calls[0];
    auto expected = docker_chaos::os::tc_netem_cmd("chaos_node_1", "30%", "50ms", "1%");
    BOOST_TEST(cmd == expected);
}

BOOST_AUTO_TEST_CASE(clear_tc_netem_correct_command) {
    MockExec exec;
    docker_chaos::ChaosNode n(
        1, std::ref(exec), leader_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, "{}"};
        });
    n.clear_tc_netem();
    BOOST_REQUIRE(!exec.calls.empty());
    BOOST_TEST(exec.calls[0] == docker_chaos::os::tc_clear_cmd("chaos_node_1"));
}

BOOST_AUTO_TEST_CASE(partition_from_issues_iptables_commands) {
    MockExec exec;
    docker_chaos::ChaosNode n(
        1, std::ref(exec), leader_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, "{}"};
        });

    n.partition_from({"10.0.0.2", "10.0.0.3"});

    // Two IPs × two rules each = 4 commands
    BOOST_TEST(exec.calls.size() == 4u);
    // First command drops src from 10.0.0.2
    BOOST_TEST(exec.calls[0] ==
               docker_chaos::os::iptables_drop_src_cmd("chaos_node_1", "10.0.0.2"));
    // Second command drops dst to 10.0.0.2
    BOOST_TEST(exec.calls[1] ==
               docker_chaos::os::iptables_drop_dst_cmd("chaos_node_1", "10.0.0.2"));
}

BOOST_AUTO_TEST_CASE(unpartition_flushes_both_chains) {
    MockExec exec;
    docker_chaos::ChaosNode n(
        1, std::ref(exec), leader_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, "{}"};
        });

    n.unpartition();

    BOOST_REQUIRE(exec.calls.size() >= 2u);
    BOOST_TEST(exec.calls[0] == docker_chaos::os::iptables_flush_chaos_cmd("chaos_node_1"));
    BOOST_TEST(exec.calls[1] == docker_chaos::os::iptables_flush_output_cmd("chaos_node_1"));
}

BOOST_AUTO_TEST_CASE(kill_issues_docker_kill) {
    MockExec exec;
    docker_chaos::ChaosNode n(
        1, std::ref(exec), leader_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, "{}"};
        });
    n.kill();
    BOOST_REQUIRE(!exec.calls.empty());
    BOOST_TEST(exec.calls[0] == docker_chaos::os::docker_kill_cmd("chaos_node_1"));
}

BOOST_AUTO_TEST_CASE(kill_nonzero_exit_throws) {
    MockExec exec;
    exec.return_code = 1;
    exec.return_out = "No such container";
    docker_chaos::ChaosNode n(
        1, std::ref(exec), leader_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, "{}"};
        });
    BOOST_CHECK_THROW(n.kill(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(stop_issues_docker_stop) {
    MockExec exec;
    docker_chaos::ChaosNode n(
        1, std::ref(exec), leader_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, "{}"};
        });
    n.stop();
    BOOST_REQUIRE(!exec.calls.empty());
    BOOST_TEST(exec.calls[0] == docker_chaos::os::docker_stop_cmd("chaos_node_1"));
}

BOOST_AUTO_TEST_CASE(stop_nonzero_exit_throws) {
    MockExec exec;
    exec.return_code = 1;
    docker_chaos::ChaosNode n(
        1, std::ref(exec), leader_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, "{}"};
        });
    BOOST_CHECK_THROW(n.stop(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(restart_issues_docker_start_then_polls_health) {
    MockExec exec;
    // Health returns healthy immediately so restart returns without looping.
    docker_chaos::ChaosNode n(
        1, std::ref(exec), healthy_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, "{}"};
        });
    n.restart(/*wait=*/true, std::chrono::seconds{5});
    BOOST_REQUIRE(!exec.calls.empty());
    BOOST_TEST(exec.calls[0] == docker_chaos::os::docker_start_cmd("chaos_node_1"));
}

BOOST_AUTO_TEST_CASE(restart_nonzero_exit_throws) {
    MockExec exec;
    exec.return_code = 1;
    docker_chaos::ChaosNode n(
        1, std::ref(exec), leader_get,
        [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
            return {200, "{}"};
        });
    BOOST_CHECK_THROW(n.restart(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(disable_all_faults_command_string) {
    // Verify the command string built for disable_all is correct — exercises
    // fiu::build_disable_all_cmd() indirectly through the fault point constant.
    auto cmd = docker_chaos::fiu::build_disable_all_cmd();
    BOOST_TEST(cmd == "disable_all");
}

BOOST_AUTO_TEST_SUITE_END()

// ── cairn-chaos-testing task 3: node tables, pause, delay, loss ───────────────

namespace {

auto no_post() -> docker_chaos::HttpPost {
    return [](const std::string&, const std::string&) -> docker_chaos::HttpResult {
        return {200, "{}"};
    };
}

}  // namespace

BOOST_AUTO_TEST_SUITE(node_table_and_faults)

BOOST_AUTO_TEST_CASE(builders_are_one_command_each) {
    using namespace docker_chaos::os;
    const std::string& rt = container_runtime();
    BOOST_TEST(pause_cmd("c") == (std::vector<std::string>{rt, "pause", "c"}));
    BOOST_TEST(unpause_cmd("c") == (std::vector<std::string>{rt, "unpause", "c"}));
    BOOST_TEST(tc_delay_cmd("c", 80) ==
               (std::vector<std::string>{rt, "exec", "c", "tc", "qdisc", "replace", "dev", "eth0",
                                         "root", "netem", "delay", "80ms"}));
    BOOST_TEST(tc_delay_cmd("c", 80, 20) ==
               (std::vector<std::string>{rt, "exec", "c", "tc", "qdisc", "replace", "dev", "eth0",
                                         "root", "netem", "delay", "80ms", "20ms"}));
    BOOST_TEST(tc_loss_cmd("c", 30) ==
               (std::vector<std::string>{rt, "exec", "c", "tc", "qdisc", "replace", "dev", "eth0",
                                         "root", "netem", "loss", "30%"}));
    BOOST_TEST(tc_loss_cmd("c", 2.5).back() == "2.5%");
}

BOOST_AUTO_TEST_CASE(pause_unsupported_classifier) {
    using docker_chaos::os::is_pause_unsupported;
    // Rootless Podman on cgroup v1, in the wordings its versions have used.
    BOOST_TEST(is_pause_unsupported(
        "Error: can not pause containers on rootless containers with cgroup V1: this container "
        "does not have a cgroup"));
    BOOST_TEST(is_pause_unsupported("Error: cgroups are not available: pause"));
    BOOST_TEST(is_pause_unsupported("OCI runtime error: cgroup freezer not supported"));
    // Ordinary failures are not "unsupported".
    BOOST_TEST(!is_pause_unsupported("Error: no container with name or ID \"x\" found"));
    BOOST_TEST(!is_pause_unsupported("Error response from daemon: container x is not running"));
    BOOST_TEST(!is_pause_unsupported(""));
}

BOOST_AUTO_TEST_CASE(node_pause_unpause_delay_loss) {
    MockExec exec;
    docker_chaos::ChaosNode n(1, std::ref(exec), healthy_get, no_post());
    n.pause();
    n.unpause();
    n.apply_delay(100, 10);
    n.apply_loss(25);
    BOOST_REQUIRE(exec.calls.size() == 4U);
    BOOST_TEST(exec.calls[0] == docker_chaos::os::pause_cmd("chaos_node_1"));
    BOOST_TEST(exec.calls[1] == docker_chaos::os::unpause_cmd("chaos_node_1"));
    BOOST_TEST(exec.calls[2] == docker_chaos::os::tc_delay_cmd("chaos_node_1", 100, 10));
    BOOST_TEST(exec.calls[3] == docker_chaos::os::tc_loss_cmd("chaos_node_1", 25));
}

BOOST_AUTO_TEST_CASE(bad_delay_or_loss_issues_nothing) {
    MockExec exec;
    docker_chaos::ChaosNode n(1, std::ref(exec), healthy_get, no_post());
    BOOST_CHECK_THROW(n.apply_delay(-1), std::invalid_argument);
    BOOST_CHECK_THROW(n.apply_delay(10, -1), std::invalid_argument);
    BOOST_CHECK_THROW(n.apply_loss(-0.5), std::invalid_argument);
    BOOST_CHECK_THROW(n.apply_loss(100.5), std::invalid_argument);
    BOOST_TEST(exec.calls.empty());
}

BOOST_AUTO_TEST_CASE(pause_on_cgroup_v1_is_unsupported_not_success) {
    MockExec exec;
    exec.return_code = 125;
    exec.return_out = "Error: can not pause containers on rootless containers with cgroup V1";
    docker_chaos::ChaosNode n(1, std::ref(exec), healthy_get, no_post());
    BOOST_CHECK_THROW(n.pause(), docker_chaos::fault_unsupported);
}

BOOST_AUTO_TEST_CASE(pause_other_failure_is_runtime_error) {
    MockExec exec;
    exec.return_code = 1;
    exec.return_out = "Error: no such container";
    docker_chaos::ChaosNode n(1, std::ref(exec), healthy_get, no_post());
    try {
        n.pause();
        BOOST_FAIL("expected a throw");
    } catch (const docker_chaos::fault_unsupported&) {
        BOOST_FAIL("an ordinary failure must not read as unsupported");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find("no such container") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(cluster_takes_a_node_table) {
    MockExec exec;
    MockGet get;
    get.result = {200, R"({"status":"running"})"};
    auto table = docker_chaos::chaos5_node_table("t");
    docker_chaos::ChaosCluster cluster("five.yml", table, std::chrono::seconds{1}, std::ref(exec),
                                       std::ref(get), no_post());
    BOOST_TEST(cluster.all_nodes().size() == 5U);
    BOOST_TEST(cluster.node(5).container() == "t-5");
    BOOST_CHECK_THROW(cluster.node(6), std::invalid_argument);

    cluster.node(4).kill();
    BOOST_TEST(exec.calls.back() == docker_chaos::os::docker_kill_cmd("t-4"));
    (void)cluster.node(2).is_healthy();
    BOOST_TEST(get.called_urls.back() == "http://localhost:8282/health");
    (void)cluster.log_lines(3);
    BOOST_TEST(exec.calls.back() == docker_chaos::os::docker_logs_cmd("t-3", 200));

    BOOST_CHECK_THROW(docker_chaos::ChaosCluster("x.yml", std::map<int, docker_chaos::NodePorts>{},
                                                 std::chrono::seconds{1}, std::ref(exec),
                                                 std::ref(get), no_post()),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(default_cluster_still_uses_k_node_map) {
    MockExec exec;
    docker_chaos::ChaosCluster cluster("docker/docker-compose.yml", std::chrono::seconds{1},
                                       std::ref(exec), healthy_get, no_post());
    BOOST_TEST(cluster.all_nodes().size() == docker_chaos::k_node_map.size());
    BOOST_TEST(cluster.node(2).container() == "chaos_node_2");
}

// The 5-node compose file and chaos5_node_table() describe one layout; this
// reads the file and fails on any drift between them.
BOOST_AUTO_TEST_CASE(chaos5_compose_matches_node_table) {
    std::ifstream in(std::string(KYTHIRA_SOURCE_DIR) + "/docker/chaos-5-compose.yml");
    BOOST_REQUIRE(in.good());
    const std::string cluster_var = "${CHAOS5_CLUSTER:-kythira-chaos5}";
    auto table =
        docker_chaos::chaos5_node_table(std::string(docker_chaos::k_chaos5_default_cluster));

    std::map<int, std::string> containers;
    std::map<int, std::vector<int>> host_ports;
    int current = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("  c5node", 0) == 0 && line.back() == ':') {
            current = std::stoi(line.substr(8, line.size() - 9));
            continue;
        }
        if (current == 0) {
            continue;
        }
        if (auto pos = line.find("container_name: "); pos != std::string::npos) {
            auto name = line.substr(pos + 16);
            BOOST_REQUIRE(name.rfind(cluster_var, 0) == 0);
            containers[current] = std::string(docker_chaos::k_chaos5_default_cluster) +
                                  name.substr(cluster_var.size());
        }
        // - "${KYTHIRA_BIND_ADDRESS:-127.0.0.1}:7201:7000"
        if (auto pos = line.find("127.0.0.1}:"); pos != std::string::npos) {
            auto rest = line.substr(pos + 11);
            host_ports[current].push_back(std::stoi(rest.substr(0, rest.find(':'))));
        }
    }
    BOOST_REQUIRE(containers.size() == table.size());
    for (const auto& [id, ports] : table) {
        BOOST_TEST(containers[id] == ports.container_name);
        BOOST_TEST(host_ports[id] ==
                   (std::vector<int>{ports.rpc_port, ports.http_port, ports.fiu_port}));
    }
}

BOOST_AUTO_TEST_SUITE_END()
