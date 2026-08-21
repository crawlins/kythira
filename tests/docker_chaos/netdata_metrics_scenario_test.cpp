// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE netdata_metrics_scenario_test
#include <boost/test/unit_test.hpp>

#include "metrics_scenario_support.hpp"

#include <chrono>
#include <string>

// End-to-end proof for doc/TODO.md "Metrics Backends" (NetData entry, Docker
// tier): a real chaos_node emits StatsD datagrams over real UDP to a real
// NetData container's statsd.plugin, and the metric is read back through
// NetData's own REST API — a metric only appears in /api/v1/allmetrics once
// the plugin parsed it into a chart.

using namespace std::chrono_literals;

namespace {

constexpr int k_node_http_port = 8095;  // netdata-metrics-compose.yml
constexpr int k_netdata_port = 19999;   // host-mapped NetData REST API

auto compose() -> std::string {
    return metrics_scenario::compose_file("KYTHIRA_NETDATA_METRICS_COMPOSE_FILE",
                                          "docker/netdata-metrics-compose.yml");
}

}  // namespace

struct NetdataFixture : metrics_scenario::compose_fixture {
    NetdataFixture() : metrics_scenario::compose_fixture(compose(), k_node_http_port) {}
};

BOOST_AUTO_TEST_SUITE(netdata_metrics_scenario)

BOOST_FIXTURE_TEST_CASE(statsd_metric_becomes_a_chart, NetdataFixture,
                        *boost::unit_test::timeout(120)) {
    drive_activity("netdata-test-key");

    // NetData exposes every statsd-derived chart in /api/v1/allmetrics;
    // raft_heartbeat_sent fires on every leader heartbeat tick. shell format
    // is the cheapest to substring-search and is always enabled.
    require_eventually(
        [&] {
            auto body =
                metrics_scenario::http_get_body(k_netdata_port, "/api/v1/allmetrics?format=shell");
            return body.find("RAFT_HEARTBEAT_SENT") != std::string::npos ||
                   body.find("raft_heartbeat_sent") != std::string::npos;
        },
        30s,
        [&] {
            return "allmetrics head:\n" + metrics_scenario::http_get_body(
                                              k_netdata_port, "/api/v1/allmetrics?format=shell")
                                              .substr(0, 4000);
        },
        "raft_heartbeat_sent never appeared in NetData's /api/v1/allmetrics");
}

BOOST_AUTO_TEST_SUITE_END()
