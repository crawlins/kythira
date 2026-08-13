#define BOOST_TEST_MODULE cloud_monitoring_config_validation_test
#include <boost/test/unit_test.hpp>

#include "os_faults.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Docker-tier validation for doc/TODO.md "Metrics Backends" cloud-vendor
// entries WITHOUT a self-hostable emulator of their ingestion API (Azure
// Monitor, GCP Cloud Monitoring, Alibaba Cloud CloudMonitor — and the
// CloudWatch config too, cheaply, though that one also gets a full
// LocalStack round-trip in cloudwatch_metrics_scenario_test.cpp): each
// example OpenTelemetry Collector config is validated by the real
// Collector binary's own `validate` command (the exact image the OTLP
// scenario test runs), so a config that references a nonexistent exporter,
// misspells a key, or breaks a component's config schema fails here — not
// in an operator's first deployment.
//
// The OCI entry routes via the OCI Management Agent's PrometheusEmitter
// (no OTel exporter for OCI Monitoring exists in collector-contrib), whose
// .properties config has no vendor validator to invoke — the test checks
// the vendor-documented required keys instead.
//
// Env-var placeholders (${env:...}) in the configs are supplied with
// syntactically valid dummies: `validate` checks config structure, not
// credentials, and no network I/O toward any vendor happens here.

namespace {

constexpr const char* k_collector_image = "otel/opentelemetry-collector-contrib:0.116.1";

auto config_dir() -> std::string {
    const char* env = std::getenv("KYTHIRA_CLOUD_MONITORING_CONFIG_DIR");
    if (env && *env) return env;
    return "docker/cloud-monitoring";  // repo-root relative; CI passes the abs path
}

/// Runs `<runtime> run --rm -v <config>:/cfg.yaml:ro [-e VAR=dummy...]
/// <collector image> validate --config=/cfg.yaml` and returns the result.
auto validate_config(const std::string& file_name, const std::vector<std::string>& dummy_env)
    -> docker_chaos::os::CmdResult {
    std::vector<std::string> cmd{docker_chaos::os::container_runtime(), "run", "--rm", "-v",
                                 config_dir() + "/" + file_name + ":/cfg.yaml:ro"};
    for (const auto& kv : dummy_env) {
        cmd.emplace_back("-e");
        cmd.emplace_back(kv);
    }
    cmd.emplace_back(k_collector_image);
    cmd.emplace_back("validate");
    cmd.emplace_back("--config=/cfg.yaml");
    return docker_chaos::os::real_exec(cmd);
}

void require_valid(const std::string& file_name, const std::vector<std::string>& dummy_env) {
    auto result = validate_config(file_name, dummy_env);
    BOOST_TEST_MESSAGE("validate " << file_name << " (exit " << result.code << "):\n"
                                   << result.out);
    BOOST_REQUIRE_MESSAGE(result.code == 0,
                          file_name << " failed the Collector's own validate — output above");
}

}  // namespace

BOOST_AUTO_TEST_SUITE(cloud_monitoring_config_validation)

BOOST_AUTO_TEST_CASE(cloudwatch_config_is_valid, *boost::unit_test::timeout(120)) {
    require_valid("cloudwatch-collector-config.yaml", {"KYTHIRA_CLOUDWATCH_ENDPOINT="});
}

BOOST_AUTO_TEST_CASE(azure_monitor_config_is_valid, *boost::unit_test::timeout(120)) {
    // A structurally valid (all-zero) instrumentation key: the exporter's
    // config validation parses the connection-string format.
    require_valid("azure-monitor-collector-config.yaml",
                  {"APPLICATIONINSIGHTS_CONNECTION_STRING="
                   "InstrumentationKey=00000000-0000-0000-0000-000000000000"});
}

BOOST_AUTO_TEST_CASE(gcp_cloud_monitoring_config_is_valid, *boost::unit_test::timeout(120)) {
    require_valid("gcp-cloud-monitoring-collector-config.yaml",
                  {"GCP_PROJECT_ID=kythira-validate-only"});
}

BOOST_AUTO_TEST_CASE(alibaba_cloudmonitor_config_is_valid, *boost::unit_test::timeout(120)) {
    require_valid("alibaba-cloudmonitor-collector-config.yaml",
                  {"KYTHIRA_CMS_REMOTE_WRITE_URL=https://cms.example.invalid/api/v3/write",
                   "ALIBABA_CLOUD_ACCESS_KEY_ID=validate-only",
                   "ALIBABA_CLOUD_ACCESS_KEY_SECRET=validate-only"});
}

// The OCI Management Agent PrometheusEmitter .properties file: no vendor
// validator exists to shell out to, so assert the vendor-documented
// required keys (url, namespace, compartmentId) are present and non-empty,
// and that url is the HTTP endpoint form the agent requires.
BOOST_AUTO_TEST_CASE(oci_prometheus_emitter_properties_have_required_keys) {
    std::ifstream in(config_dir() + "/oci-management-agent-prometheus-emitter.properties");
    BOOST_REQUIRE_MESSAGE(in.good(),
                          "could not open the OCI .properties example under " << config_dir());

    std::string url;
    std::string ns;
    std::string compartment;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        auto value = line.substr(eq + 1);
        if (key == "url") url = value;
        if (key == "namespace") ns = value;
        if (key == "compartmentId") compartment = value;
    }

    BOOST_TEST(url.rfind("http://", 0) == 0);  // agent scrapes HTTP only
    BOOST_TEST(!ns.empty());
    BOOST_TEST(compartment.rfind("ocid1.compartment.", 0) == 0);
}

BOOST_AUTO_TEST_SUITE_END()
