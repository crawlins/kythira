// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE telegraf_metrics_scenario_test
#include <boost/test/unit_test.hpp>

#include "metrics_scenario_support.hpp"

#include <chrono>
#include <string>

// End-to-end proof for doc/TODO.md "Metrics Backends" (Telegraf entry,
// Docker tier): a real chaos_node emits InfluxDB line protocol over real UDP
// to a real Telegraf socket_listener, and the metric is read back from
// Telegraf's file output — which unparseable input never reaches, so the
// lines were really parsed, tags and all.

using namespace std::chrono_literals;

namespace {

constexpr int k_node_http_port = 8094;  // telegraf-metrics-compose.yml
constexpr const char* k_telegraf_container = "telegraf-metrics-test-telegraf";
constexpr const char* k_output_path = "/tmp/telegraf-out.influx";

auto compose() -> std::string {
    return metrics_scenario::compose_file("KYTHIRA_TELEGRAF_METRICS_COMPOSE_FILE",
                                          "docker/telegraf-metrics-compose.yml");
}

// Read Telegraf's output file via `<runtime> exec` rather than a host bind
// mount — works identically under Docker and rootless Podman (the OTLP
// scenario test's own pattern).
auto read_telegraf_output() -> std::string {
    auto result = docker_chaos::os::real_exec({docker_chaos::os::container_runtime(), "exec",
                                               k_telegraf_container, "cat", k_output_path});
    return result.out;
}

}  // namespace

struct TelegrafFixture : metrics_scenario::compose_fixture {
    TelegrafFixture() : metrics_scenario::compose_fixture(compose(), k_node_http_port) {}
};

BOOST_AUTO_TEST_SUITE(telegraf_metrics_scenario)

BOOST_FIXTURE_TEST_CASE(lines_are_parsed_and_reach_an_output, TelegrafFixture,
                        *boost::unit_test::timeout(120)) {
    drive_activity("telegraf-test-key");

    // Telegraf re-serializes what it parsed, so the measurement name and the
    // node_id tag surviving to the file output proves the full parse.
    // raft.hpp emits snake_case names — raft_heartbeat_sent fires on every
    // leader heartbeat tick.
    require_eventually(
        [&] {
            auto out = read_telegraf_output();
            return out.find("raft_heartbeat_sent") != std::string::npos &&
                   out.find("node_id=1") != std::string::npos;
        },
        30s, [&] { return "output file tail:\n" + read_telegraf_output().substr(0, 4000); },
        "raft_heartbeat_sent with node_id=1 never appeared in Telegraf's file output");
}

// The logging leg (telegraf_logger, TELEGRAF_LOGS=on in the compose file):
// log events ride the same socket_listener as line-protocol records, so the
// same file output proves the parse — measurement, level tag, and the msg
// string field surviving re-serialization.
BOOST_FIXTURE_TEST_CASE(log_events_are_parsed_and_reach_an_output, TelegrafFixture,
                        *boost::unit_test::timeout(120)) {
    drive_activity("telegraf-logs-test-key");

    require_eventually(
        [&] {
            auto out = read_telegraf_output();
            return out.find("kythira_log") != std::string::npos &&
                   out.find("level=info") != std::string::npos &&
                   out.find("msg=") != std::string::npos;
        },
        30s, [&] { return "output file tail:\n" + read_telegraf_output().substr(0, 4000); },
        "kythira_log events never appeared in Telegraf's file output");
}

BOOST_AUTO_TEST_SUITE_END()
