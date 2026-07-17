#define BOOST_TEST_MODULE otlp_collector_test
#include <boost/test/unit_test.hpp>

#include "os_faults.hpp"

#include <httplib.h>
#include <boost/json.hpp>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

// End-to-end proof for .kiro/specs/otlp-telemetry-backend/ (Requirement 7):
// a real chaos_node, configured via OTLP_ENDPOINT, POSTs real OTLP/HTTP JSON
// to a real OpenTelemetry Collector container, which really parses it — not
// just that the JSON this project constructs matches what it believes the
// OTLP schema to be.

using namespace std::chrono_literals;
namespace json = boost::json;

namespace {

constexpr int k_node_http_port = 8091;
constexpr const char* k_collector_container = "otlp-collector-test-collector";
constexpr const char* k_export_path = "/data/otlp-export.jsonl";

std::string compose_file() {
    const char* env = std::getenv("KYTHIRA_OTLP_COLLECTOR_COMPOSE_FILE");
    if (env && *env) return env;
    return "docker/otlp-collector-compose.yml";
}

bool is_healthy() {
    httplib::Client cli("localhost", k_node_http_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(3);
    auto res = cli.Get("/health");
    return res && res->status == 200;
}

bool wait_all_healthy(std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (is_healthy()) return true;
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

// Submits {"key":k,"value":v} to node1's HTTP control plane. A single-node
// cluster's majority is 1, so it self-elects leader and this succeeds once
// it has — no peer coordination needed for this test's purposes.
bool submit_command(const std::string& key, const std::string& value) {
    httplib::Client cli("localhost", k_node_http_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);
    json::object body{{"key", key}, {"value", value}};
    auto res = cli.Post("/command", json::serialize(body), "application/json");
    return res && res->status == 200;
}

bool wait_for_leader_and_submit(const std::string& key, const std::string& value,
                                std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (submit_command(key, value)) return true;
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

// Reads the Collector's exported file via `<runtime> exec` rather than a
// host bind-mount, so this works identically under Docker and rootless
// Podman without depending on host-vs-container UID mapping.
std::string read_collector_export() {
    auto result = docker_chaos::os::real_exec(
        {docker_chaos::os::container_runtime(), "exec", k_collector_container, "cat",
         k_export_path});
    return result.out;
}

}  // namespace

struct OtlpCollectorFixture {
    std::string file{compose_file()};

    OtlpCollectorFixture() {
        docker_chaos::os::try_exec(docker_chaos::os::real_exec,
                                   docker_chaos::os::compose_down_cmd(file));
        docker_chaos::os::checked_exec(docker_chaos::os::real_exec,
                                       docker_chaos::os::compose_up_cmd(file));
        if (!wait_all_healthy(60s)) {
            docker_chaos::os::try_exec(docker_chaos::os::real_exec,
                                       docker_chaos::os::compose_down_cmd(file));
            BOOST_FAIL("otlp-collector-test node did not become healthy within 60 s");
        }
    }

    ~OtlpCollectorFixture() {
        try {
            docker_chaos::os::real_exec(docker_chaos::os::compose_down_cmd(file));
        } catch (...) {
        }
    }
};

BOOST_AUTO_TEST_SUITE(otlp_collector)

BOOST_FIXTURE_TEST_CASE(node_becomes_healthy, OtlpCollectorFixture, *boost::unit_test::timeout(90)) {
    BOOST_TEST(is_healthy());
}

// The direct test of Requirement 7.3: drive real activity, then assert the
// Collector really received and parsed real OTLP metrics and logs from this
// node.
BOOST_FIXTURE_TEST_CASE(real_metrics_and_logs_reach_the_collector, OtlpCollectorFixture,
                        *boost::unit_test::timeout(120)) {
    BOOST_REQUIRE_MESSAGE(wait_for_leader_and_submit("otlp-test-key", "otlp-test-value", 30s),
                          "node1 never became leader / accepted a command within 30 s");

    // Give the exporter's batching (default flush_interval) and the
    // Collector's own file-exporter flush a moment to land.
    std::this_thread::sleep_for(8s);

    std::string exported = read_collector_export();
    BOOST_REQUIRE_MESSAGE(!exported.empty(), "Collector's export file is empty");

    BOOST_TEST(exported.find("\"resourceMetrics\"") != std::string::npos);
    BOOST_TEST(exported.find("\"resourceLogs\"") != std::string::npos);
    BOOST_TEST(exported.find("\"service.instance.id\"") != std::string::npos);
    // service_instance_id is set to the node's own NODE_ID ("1") — Requirement 4.3.
    BOOST_TEST(exported.find(R"("stringValue":"1")") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
