#define BOOST_TEST_MODULE harness_unit_test
#include <boost/test/unit_test.hpp>

#include "harness.hpp"
#include "os_faults.hpp"

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
