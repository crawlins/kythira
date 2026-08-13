#define BOOST_TEST_MODULE alibaba_http_client_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/alibaba_http_client.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

// Unit coverage for the Alibaba control-plane HTTP client
// (.kiro/specs/alibaba-cloud-services/ Requirement 16.3): endpoint
// derivation, endpoint_override's global takeover, throttling single-retry,
// vendor error unwrapping, and empty-body handling.
//
// The HTTP layer has no injection seam by design (the oci_http_client
// precedent): tests point `endpoint_override` at a local httplib::Server,
// which is also what proves the override actually replaces every derived
// host.

using namespace kythira;
using namespace std::chrono_literals;

namespace {

/// A local server on an ephemeral port, torn down with the fixture.
struct MockServer {
    httplib::Server server;
    std::thread thread;
    int port{0};
    std::atomic<int> request_count{0};

    MockServer() = default;

    void start() {
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
    }

    ~MockServer() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    MockServer(const MockServer&) = delete;
    auto operator=(const MockServer&) -> MockServer& = delete;
    MockServer(MockServer&&) = delete;
    auto operator=(MockServer&&) -> MockServer& = delete;

    [[nodiscard]] auto origin() const -> std::string {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

auto config_for(const MockServer& mock) -> alibaba_client_config {
    alibaba_client_config cfg;
    cfg.region = "cn-hangzhou";
    cfg.access_key_id = "test-ak";
    cfg.access_key_secret = "test-secret";
    cfg.endpoint_override = mock.origin();
    cfg.api_timeout = 5s;
    return cfg;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(alibaba_http_client_endpoints)

// Central services take the bare host; ECS is region-qualified.
BOOST_AUTO_TEST_CASE(host_derivation_per_service) {
    alibaba_client_config cfg;
    cfg.region = "cn-hangzhou";
    cfg.access_key_id = "id";
    cfg.access_key_secret = "secret";
    const alibaba_http_client client{cfg};

    BOOST_TEST(client.host_for("ess") == "ess.aliyuncs.com");
    BOOST_TEST(client.host_for("sts") == "sts.aliyuncs.com");
    BOOST_TEST(client.host_for("ecs") == "ecs.cn-hangzhou.aliyuncs.com");
}

// The single-mock-server hook: the override replaces every service's host at
// once, which is what lets one test server stand in for ESS, ECS and STS.
BOOST_AUTO_TEST_CASE(endpoint_override_wins_for_every_service) {
    alibaba_client_config cfg;
    cfg.region = "cn-hangzhou";
    cfg.access_key_id = "id";
    cfg.access_key_secret = "secret";
    cfg.endpoint_override = "http://127.0.0.1:9999";
    const alibaba_http_client client{cfg};

    BOOST_TEST(client.host_for("ess") == "http://127.0.0.1:9999");
    BOOST_TEST(client.host_for("ecs") == "http://127.0.0.1:9999");
    BOOST_TEST(client.host_for("sts") == "http://127.0.0.1:9999");
}

// The signed-versus-sent invariant, assertable without a network: the Host
// header the request carries is the bare hostname that went into the
// signature — no scheme, no port.
BOOST_AUTO_TEST_CASE(host_header_is_the_bare_signed_hostname) {
    alibaba_client_config cfg;
    cfg.region = "cn-hangzhou";
    cfg.access_key_id = "id";
    cfg.access_key_secret = "secret";
    const alibaba_http_client client{cfg};

    const auto headers =
        client.prepared_headers("https://ess.aliyuncs.com", "DescribeRegions", "2014-08-28", {}, "",
                                std::chrono::system_clock::now(), "nonce");
    BOOST_TEST(headers.at("Host") == "ess.aliyuncs.com");
    BOOST_TEST(headers.at("host") == "ess.aliyuncs.com");
    BOOST_TEST(headers.at("Authorization").find("ACS3-HMAC-SHA256 Credential=id") == 0U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(alibaba_http_client_requests)

BOOST_AUTO_TEST_CASE(successful_call_parses_json) {
    MockServer mock;
    mock.server.Post("/", [](const httplib::Request& req, httplib::Response& res) {
        // The action rides in a signed header, and the parameters in the
        // query string — both assertable from the server side.
        BOOST_TEST(req.get_header_value("x-acs-action") == "DescribeScalingGroups");
        BOOST_TEST(req.get_param_value("ScalingGroupId") == "asg-1");
        res.set_content(R"({"TotalCount":1})", "application/json");
    });
    mock.start();

    const alibaba_http_client client{config_for(mock)};
    const auto response =
        client.rpc("ess", "DescribeScalingGroups", "2014-08-28", {{"ScalingGroupId", "asg-1"}});
    BOOST_TEST(response.at("TotalCount").as_int64() == 1);
}

BOOST_AUTO_TEST_CASE(empty_body_parses_as_null_not_throw) {
    MockServer mock;
    mock.server.Post("/", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
        res.set_content("", "application/json");
    });
    mock.start();

    const alibaba_http_client client{config_for(mock)};
    const auto response = client.rpc("ess", "Whatever", "2014-08-28", {});
    BOOST_TEST(response.is_null());
}

// Exactly one retry on 429, and only on 429 — the bounded policy the header
// documents. Two requests total, and the second one's answer is returned.
BOOST_AUTO_TEST_CASE(throttling_is_retried_exactly_once) {
    MockServer mock;
    mock.server.Post("/", [&mock](const httplib::Request&, httplib::Response& res) {
        const int n = ++mock.request_count;
        if (n == 1) {
            res.status = 429;
            res.set_header("retry-after", "1");
            res.set_content(R"({"Code":"Throttling"})", "application/json");
        } else {
            res.set_content(R"({"ok":true})", "application/json");
        }
    });
    mock.start();

    const alibaba_http_client client{config_for(mock)};
    const auto response = client.rpc("ess", "DescribeScalingGroups", "2014-08-28", {});
    BOOST_TEST(response.at("ok").as_bool());
    BOOST_TEST(mock.request_count.load() == 2);
}

BOOST_AUTO_TEST_CASE(persistent_throttling_throws_after_the_single_retry) {
    MockServer mock;
    mock.server.Post("/", [&mock](const httplib::Request&, httplib::Response& res) {
        ++mock.request_count;
        res.status = 429;
        res.set_content(R"({"Code":"Throttling","Message":"too fast"})", "application/json");
    });
    mock.start();

    const alibaba_http_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.rpc("ess", "A", "2014-08-28", {}), std::runtime_error);
    BOOST_TEST(mock.request_count.load() == 2);
}

// Vendor error fields (capitalised, unlike OCI's) reach the exception text —
// the difference between a debuggable failure and "HTTP 400".
BOOST_AUTO_TEST_CASE(vendor_error_fields_surface_in_the_exception) {
    MockServer mock;
    mock.server.Post("/", [](const httplib::Request&, httplib::Response& res) {
        res.status = 400;
        res.set_content(
            R"({"Code":"InvalidScalingGroupId.NotFound","Message":"no such group","RequestId":"REQ-1"})",
            "application/json");
    });
    mock.start();

    const alibaba_http_client client{config_for(mock)};
    try {
        (void)client.rpc("ess", "DescribeScalingGroups", "2014-08-28", {});
        BOOST_FAIL("expected a runtime_error");
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        BOOST_TEST(what.find("InvalidScalingGroupId.NotFound") != std::string::npos);
        BOOST_TEST(what.find("no such group") != std::string::npos);
        BOOST_TEST(what.find("REQ-1") != std::string::npos);
        BOOST_TEST(what.find("400") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(unparseable_2xx_body_throws) {
    MockServer mock;
    mock.server.Post("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("not json at all", "application/json");
    });
    mock.start();

    const alibaba_http_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.rpc("ess", "A", "2014-08-28", {}), std::runtime_error);
}

// A dead port: transport failure must surface as an exception naming the
// action, not hang or return a fabricated empty response.
BOOST_AUTO_TEST_CASE(transport_failure_throws) {
    alibaba_client_config cfg;
    cfg.region = "cn-hangzhou";
    cfg.access_key_id = "id";
    cfg.access_key_secret = "secret";
    cfg.endpoint_override = "http://127.0.0.1:1";
    cfg.api_timeout = 2s;

    const alibaba_http_client client{cfg};
    BOOST_CHECK_THROW(client.rpc("ess", "DescribeScalingGroups", "2014-08-28", {}),
                      std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
