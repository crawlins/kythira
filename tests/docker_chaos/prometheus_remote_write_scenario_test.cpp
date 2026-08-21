// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE prometheus_remote_write_scenario_test
#include <boost/test/unit_test.hpp>

#include "metrics_scenario_support.hpp"

#include <chrono>
#include <string>

// End-to-end proof of the remote-write EXPORT path for doc/TODO.md's
// "Metrics Backends" cloud-vendor entries: one OTLP datapoint is posted at a
// real OpenTelemetry Collector running the UNMODIFIED example config
// docker/cloud-monitoring/alibaba-cloudmonitor-collector-config.yaml, whose
// prometheusremotewrite exporter delivers into a real open-source Prometheus
// started with --web.enable-remote-write-receiver, and the series is read
// back through Prometheus's own query API.
//
// ## Why this exists
//
// Four of the five cloud-vendor monitoring entries have no self-hostable
// emulator of their ingestion API, so their Docker tier was
// cloud_monitoring_config_validation_test.cpp — the Collector's own
// `validate`, which is a SCHEMA check. That leaves daylight between "the
// config parses" and "a well-formed remote-write request left the Collector
// and a series came back". This closes that gap, with no credentials and no
// cost.
//
// ## What a green here does NOT mean
//
// **It does not mean CloudMonitor works.** Open-source Prometheus is not an
// emulator of CloudMonitor the way LocalStack is an emulator of CloudWatch —
// it is the PROTOCOL CloudMonitor borrowed. Three things remain observable
// only against the real service, which is what
// scripts/real-cloud-monitoring/alibaba-cloudmonitor.sh is for:
//
//   1. **The path differs.** Prometheus's receiver serves /api/v1/write;
//      CloudMonitor serves /api/v3/write. The compose file points at v1
//      deliberately.
//   2. **The auth is never evaluated.** The config's basicauth/cms extension
//      is fed deliberately fake credentials, and this Prometheus does not
//      require Basic auth. A green proves the extension is WIRED, never that
//      a credential is valid — a party that validates against whatever you
//      send always agrees with you, which is the trap that cost this repo
//      two shipped OCI defects.
//   3. **Vendor-side metric-name, label and cardinality rules.**
//
// ## Why there is no chaos_node
//
// The node -> OTLP -> Collector leg is already proven by
// otlp_collector_test.cpp. Repeating it here would add wall clock and an
// image dependency without adding evidence; the probe is posted straight at
// the OTLP receiver, exactly as the real-cloud tier's own scripts do.

using namespace std::chrono_literals;

namespace {

constexpr int k_prometheus_port = 9097;  // prometheus-remote-write-compose.yml
constexpr int k_otlp_port = 4319;        // host-mapped Collector OTLP/HTTP
constexpr const char* k_probe_metric = "kythira_remote_write_tier_probe";

auto compose() -> std::string {
    return metrics_scenario::compose_file("KYTHIRA_PROMETHEUS_REMOTE_WRITE_COMPOSE_FILE",
                                          "docker/prometheus-remote-write-compose.yml");
}

/// One gauge datapoint as OTLP/HTTP JSON. A gauge is the shape every vendor
/// exporter in the example configs maps without temporality caveats — the
/// same choice scripts/real-cloud-monitoring/common.sh makes.
[[nodiscard]] auto post_probe() -> bool {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const std::string body =
        R"({"resourceMetrics":[{"resource":{"attributes":[)"
        R"({"key":"service.name","value":{"stringValue":"kythira-rw-tier-probe"}}]},)"
        R"("scopeMetrics":[{"scope":{"name":"kythira-ci"},"metrics":[{"name":")" +
        std::string(k_probe_metric) +
        R"(","gauge":{"dataPoints":[{"asDouble":42,"timeUnixNano":")" + std::to_string(now_ns) +
        R"("}]}}]}]}]})";

    httplib::Client client("127.0.0.1", k_otlp_port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    auto res = client.Post("/v1/metrics", body, "application/json");
    return res && res->status == 200;
}

/// Compose up/down with no node to wait on: readiness is the Collector's
/// OTLP receiver accepting a POST, which is also the first thing the test
/// needs to do anyway.
struct remote_write_fixture {
    std::string file{compose()};

    remote_write_fixture() {
        docker_chaos::os::try_exec(docker_chaos::os::real_exec,
                                   docker_chaos::os::compose_down_cmd(file));
        docker_chaos::os::checked_exec(docker_chaos::os::real_exec,
                                       docker_chaos::os::compose_up_cmd(file));
        // Both images are pulled on a cold machine, so the window is
        // generous; the probe POST itself is the readiness signal.
        if (!metrics_scenario::poll_until([] { return post_probe(); }, 120s)) {
            docker_chaos::os::try_exec(docker_chaos::os::real_exec,
                                       docker_chaos::os::compose_down_cmd(file));
            BOOST_FAIL("the Collector's OTLP receiver never accepted the probe datapoint");
        }
    }

    ~remote_write_fixture() {
        docker_chaos::os::try_exec(docker_chaos::os::real_exec,
                                   docker_chaos::os::compose_down_cmd(file));
    }

    remote_write_fixture(const remote_write_fixture&) = delete;
    auto operator=(const remote_write_fixture&) -> remote_write_fixture& = delete;
    remote_write_fixture(remote_write_fixture&&) = delete;
    auto operator=(remote_write_fixture&&) -> remote_write_fixture& = delete;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(prometheus_remote_write_scenario)

// The negative control, asserted FIRST because every other assertion in this
// file depends on it. If this Prometheus scraped anything, a queryable series
// would not be attributable to remote write at all — and the sibling
// pull-path test (prometheus_metrics_scenario_test.cpp) already covers
// scraping. docker/prometheus/prometheus-remote-write.yml has an empty
// scrape_configs precisely so this holds.
BOOST_FIXTURE_TEST_CASE(sink_scrapes_nothing, remote_write_fixture,
                        *boost::unit_test::timeout(240)) {
    auto body = metrics_scenario::http_get_body(k_prometheus_port, "/api/v1/targets");
    BOOST_TEST_REQUIRE(body.find("\"status\":\"success\"") != std::string::npos,
                       "Prometheus /api/v1/targets was not reachable: " << body);
    BOOST_TEST(body.find("\"activeTargets\":[]") != std::string::npos,
               "the remote-write sink has scrape targets, so a queryable series would not "
               "prove remote write delivered it: "
                   << body);
}

BOOST_FIXTURE_TEST_CASE(remote_written_series_is_queryable, remote_write_fixture,
                        *boost::unit_test::timeout(240)) {
    const std::string query = std::string("/api/v1/query?query=") + k_probe_metric;
    std::string last;
    const bool arrived = metrics_scenario::poll_until(
        [&] {
            last = metrics_scenario::http_get_body(k_prometheus_port, query);
            return last.find("\"status\":\"success\"") != std::string::npos &&
                   last.find(k_probe_metric) != std::string::npos;
        },
        60s);
    BOOST_TEST(arrived,
               "the probe series never arrived in Prometheus by remote write. "
               "Last query response: "
                   << last);

    // resource_to_telemetry_conversion is enabled in the example config, and
    // its whole purpose is that resource attributes become series labels —
    // the mechanism by which every Kythira backend labels series by node
    // identity. Asserting the label proves the exporter setting took effect,
    // not merely that some series with the right name exists.
    BOOST_TEST(last.find("service_name") != std::string::npos,
               "service.name did not become a series label, so "
               "resource_to_telemetry_conversion did not take effect: "
                   << last);
}

BOOST_AUTO_TEST_SUITE_END()
