#define BOOST_TEST_MODULE prometheus_metrics_scenario_test
#include <boost/test/unit_test.hpp>

#include "metrics_scenario_support.hpp"

#include <chrono>
#include <string>

// End-to-end proof for doc/TODO.md "Metrics Backends" (Prometheus entry,
// Docker tier): a real chaos_node exposes /metrics via
// prometheus_scrape_server, a real Prometheus container scrapes it, and the
// sample is read back through Prometheus's own query API — so the exposition
// was really parsed and ingested, not just rendered.

using namespace std::chrono_literals;

namespace {

constexpr int k_node_http_port = 8092;   // prometheus-metrics-compose.yml
constexpr int k_prometheus_port = 9096;  // host-mapped Prometheus API

auto compose() -> std::string {
    return metrics_scenario::compose_file("KYTHIRA_PROMETHEUS_METRICS_COMPOSE_FILE",
                                          "docker/prometheus-metrics-compose.yml");
}

}  // namespace

struct PrometheusFixture : metrics_scenario::compose_fixture {
    PrometheusFixture() : metrics_scenario::compose_fixture(compose(), k_node_http_port) {}
};

BOOST_AUTO_TEST_SUITE(prometheus_metrics_scenario)

BOOST_FIXTURE_TEST_CASE(scraped_sample_is_queryable, PrometheusFixture,
                        *boost::unit_test::timeout(120)) {
    drive_activity("prometheus-test-key");

    // raft_heartbeat_sent_total is emitted by every leader heartbeat tick,
    // so it exists within one scrape interval of leadership. The instant
    // query returns a non-empty result vector only for an ingested series.
    const std::string query =
        "/api/v1/query?query=raft_heartbeat_sent_total%7Bnode_id%3D%221%22%7D";
    require_eventually(
        [&] {
            auto body = metrics_scenario::http_get_body(k_prometheus_port, query);
            return body.find("\"status\":\"success\"") != std::string::npos &&
                   body.find("raft_heartbeat_sent_total") != std::string::npos;
        },
        30s, [&] { return metrics_scenario::http_get_body(k_prometheus_port, query); },
        "raft_heartbeat_sent_total{node_id=\"1\"} never became queryable in Prometheus");
}

BOOST_AUTO_TEST_SUITE_END()
