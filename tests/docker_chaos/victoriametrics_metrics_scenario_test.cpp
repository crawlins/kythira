#define BOOST_TEST_MODULE victoriametrics_metrics_scenario_test
#include <boost/test/unit_test.hpp>

#include "metrics_scenario_support.hpp"

#include <chrono>
#include <string>

// End-to-end proof for doc/TODO.md "Metrics Backends" (VictoriaMetrics
// entry, Docker tier): a real chaos_node pushes its cumulative Prometheus
// exposition to a real VictoriaMetrics container's
// /api/v1/import/prometheus, and the sample is read back through VM's
// Prometheus-compatible query API — constant job/instance labels included,
// which only exist if the push path (not a scrape) delivered them.

using namespace std::chrono_literals;

namespace {

constexpr int k_node_http_port = 8093;  // victoriametrics-metrics-compose.yml
constexpr int k_vm_port = 8429;         // host-mapped VictoriaMetrics API

auto compose() -> std::string {
    return metrics_scenario::compose_file("KYTHIRA_VICTORIAMETRICS_METRICS_COMPOSE_FILE",
                                          "docker/victoriametrics-metrics-compose.yml");
}

}  // namespace

struct VictoriaMetricsFixture : metrics_scenario::compose_fixture {
    VictoriaMetricsFixture() : metrics_scenario::compose_fixture(compose(), k_node_http_port) {}
};

BOOST_AUTO_TEST_SUITE(victoriametrics_metrics_scenario)

BOOST_FIXTURE_TEST_CASE(pushed_sample_is_queryable_with_identity_labels, VictoriaMetricsFixture,
                        *boost::unit_test::timeout(120)) {
    drive_activity("victoriametrics-test-key");

    // job/instance arrive as the backend's constant_labels (main.cpp wires
    // job=kythira-chaos-node, instance=<NODE_ID>), so querying by them
    // proves the identity plumbing along with the data path. Pushes run on
    // a 5 s default interval; the poll rides that out.
    const std::string query =
        "/api/v1/query?query=raft_heartbeat_sent_total%7Bjob%3D%22kythira-chaos-node%22%2C"
        "instance%3D%221%22%7D";
    require_eventually(
        [&] {
            auto body = metrics_scenario::http_get_body(k_vm_port, query);
            return body.find("\"status\":\"success\"") != std::string::npos &&
                   body.find("raft_heartbeat_sent_total") != std::string::npos;
        },
        30s, [&] { return metrics_scenario::http_get_body(k_vm_port, query); },
        "raft_heartbeat_sent_total{job,instance} never became queryable in VictoriaMetrics");
}

BOOST_AUTO_TEST_SUITE_END()
