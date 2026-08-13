#define BOOST_TEST_MODULE cloudwatch_metrics_scenario_test
#include <boost/test/unit_test.hpp>

#include "metrics_scenario_support.hpp"

#include <chrono>
#include <string>
#include <vector>

// End-to-end proof for doc/TODO.md "Metrics Backends" (AWS CloudWatch
// entry, Docker tier): a real chaos_node emits OTLP to a real OpenTelemetry
// Collector running the UNMODIFIED example config
// (docker/cloud-monitoring/cloudwatch-collector-config.yaml), whose
// awsemf/awscloudwatchlogs exporters deliver into a LocalStack CloudWatch —
// so the example config's routing really drove PutLogEvents against the
// CloudWatch ingestion API, not just a schema check. CloudWatch is the one
// cloud-vendor monitoring entry with a self-hostable emulator of its
// ingestion API (LocalStack, mirroring aws_quorum_manager_localstack_
// test.cpp's choice of emulator); the other four vendors get config
// validation instead (cloud_monitoring_config_validation_test.cpp).
//
// Assertions run through `exec awslocal` inside the LocalStack container —
// reading back via the same public CloudWatch Logs API a real consumer
// would use, without needing an AWS CLI on the host.

using namespace std::chrono_literals;

namespace {

constexpr int k_node_http_port = 8096;  // cloudwatch-localstack-compose.yml
constexpr const char* k_localstack_container = "cloudwatch-localstack-test-localstack";

auto compose() -> std::string {
    return metrics_scenario::compose_file("KYTHIRA_CLOUDWATCH_METRICS_COMPOSE_FILE",
                                          "docker/cloudwatch-localstack-compose.yml");
}

/// filter-log-events against LocalStack's CloudWatch Logs API for one log
/// group, via awslocal inside the container. Returns combined output; a
/// nonexistent group (exporter hasn't created it yet) returns the error
/// text, which simply fails the caller's contains-check for another poll
/// round.
auto filter_log_events(const std::string& log_group) -> std::string {
    auto result = docker_chaos::os::real_exec(
        {docker_chaos::os::container_runtime(), "exec", k_localstack_container, "awslocal", "logs",
         "filter-log-events", "--log-group-name", log_group, "--limit", "50"});
    return result.out;
}

}  // namespace

struct CloudWatchFixture : metrics_scenario::compose_fixture {
    CloudWatchFixture() : metrics_scenario::compose_fixture(compose(), k_node_http_port) {}
};

BOOST_AUTO_TEST_SUITE(cloudwatch_metrics_scenario)

// Metrics leg: awsemf renders each OTLP export as an Embedded Metric Format
// document and PutLogEvents it into the metrics log group. The EMF
// envelope's "CloudWatchMetrics" directive is what real CloudWatch uses to
// extract metrics server-side, so its presence alongside a known Kythira
// counter proves a real, extraction-ready metric event arrived through the
// ingestion API. The counter is raft_heartbeat_sent — its OTLP wire name;
// the _total suffix the Prometheus scenario test asserts on is a
// Prometheus-exposition rendering detail that does not exist on this path
// (this assertion's first CI run failed by expecting it).
BOOST_FIXTURE_TEST_CASE(emf_metric_events_reach_cloudwatch_logs, CloudWatchFixture,
                        *boost::unit_test::timeout(180)) {
    drive_activity("cloudwatch-test-key");

    const std::string group = "/kythira/chaos-node/metrics";
    require_eventually(
        [&] {
            auto body = filter_log_events(group);
            return body.find("CloudWatchMetrics") != std::string::npos &&
                   body.find("raft_heartbeat_sent") != std::string::npos;
        },
        90s, [&] { return filter_log_events(group); },
        "no EMF document containing raft_heartbeat_sent ever arrived in LocalStack");
}

// Logs leg: the node's diagnostic_logger output (otlp_logger) must land in
// the logs log group via the awscloudwatchlogs exporter. "Starting Raft
// node" is logged at startup, so it exists as soon as delivery works.
BOOST_FIXTURE_TEST_CASE(node_logs_reach_cloudwatch_logs, CloudWatchFixture,
                        *boost::unit_test::timeout(180)) {
    drive_activity("cloudwatch-logs-test-key");

    const std::string group = "/kythira/chaos-node/logs";
    require_eventually(
        [&] {
            auto body = filter_log_events(group);
            return body.find("Raft") != std::string::npos;
        },
        90s, [&] { return filter_log_events(group); },
        "the node's log lines never arrived in LocalStack's CloudWatch Logs");
}

BOOST_AUTO_TEST_SUITE_END()
