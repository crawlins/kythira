// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Cloud credentials from a Kythira OIDC token (`.kiro/specs/iam-compatible-auth/`
// task 3, Requirements 2.1-2.3): the shared refresh rule, the per-audience
// token source (including its mTLS fetch across a follower's redirect), and
// the AWS, GCP, Azure, Alibaba and OCI exchanges (tasks 3 and 4) against
// local mock endpoints that check the exact request each cloud expects, and
// the Alibaba and OCI signers reading the exchanged credentials.

#define BOOST_TEST_MODULE cloud_credential_exchange_test

#include <boost/test/unit_test.hpp>

#include <raft/alibaba_oss_client.hpp>
#include <raft/alibaba_signing.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/cloud_credential_exchange.hpp>
#include <raft/oci_signing.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace cc = kythira::cloud_credentials;
using namespace std::chrono_literals;

namespace {

/// A clock the test moves by hand.
struct fake_clock {
    std::shared_ptr<std::atomic<cc::clock::rep>> ticks =
        std::make_shared<std::atomic<cc::clock::rep>>(
            cc::clock::from_time_t(1'800'000'000).time_since_epoch().count());
    [[nodiscard]] auto now() const -> cc::clock::time_point {
        return cc::clock::time_point(cc::clock::duration(ticks->load()));
    }
    void advance(cc::clock::duration d) const { *ticks += d.count(); }
    [[nodiscard]] auto fn() const -> std::function<cc::clock::time_point()> {
        auto t = ticks;
        return [t] { return cc::clock::time_point(cc::clock::duration(t->load())); };
    }
};

/// A plain-HTTP mock server on a free loopback port, run on its own thread.
struct mock_server {
    httplib::Server server;
    std::thread thread;
    int port{0};

    void start() {
        port = server.bind_to_any_port("127.0.0.1");
        BOOST_REQUIRE(port > 0);
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    [[nodiscard]] auto url() const -> std::string {
        return "http://127.0.0.1:" + std::to_string(port);
    }
    ~mock_server() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }
};

auto fixed_token_source(std::string jwt) -> std::shared_ptr<cc::kythira_oidc_token_source> {
    return std::make_shared<cc::kythira_oidc_token_source>(
        [jwt](const std::string&) { return cc::oidc_token{jwt, cc::clock::now() + 1h}; });
}

}  // namespace

// ---------------------------------------------------------------------------
// Refresh rule
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(refreshes_after_eighty_percent_of_the_lifetime) {
    fake_clock clk;
    int fetches = 0;
    cc::refreshing_value<int> v(
        [&] {
            ++fetches;
            return cc::refreshing_value<int>::timed{fetches, clk.now() + 100s};
        },
        {}, clk.fn());
    BOOST_TEST(v.get() == 1);
    clk.advance(79s);
    BOOST_TEST(v.get() == 1);  // still fresh
    BOOST_TEST(fetches == 1);
    clk.advance(1s);  // 80 s of 100: refresh
    BOOST_TEST(v.get() == 2);
    BOOST_TEST(fetches == 2);
}

BOOST_AUTO_TEST_CASE(a_failed_refresh_keeps_serving_until_expiry_then_throws) {
    fake_clock clk;
    bool fail = false;
    int fetches = 0;
    cc::refreshing_value<int> v(
        [&] {
            ++fetches;
            if (fail) {
                throw cc::exchange_error("sts down");
            }
            return cc::refreshing_value<int>::timed{fetches, clk.now() + 100s};
        },
        {.refresh_at_fraction = 0.8, .first_retry = 2s, .max_retry = 8s}, clk.fn());
    BOOST_TEST(v.get() == 1);
    fail = true;
    clk.advance(85s);
    BOOST_TEST(v.get() == 1);  // refresh failed, old value still valid
    BOOST_TEST(fetches == 2);
    BOOST_TEST(v.consecutive_failures() == 1);
    clk.advance(1s);
    BOOST_TEST(v.get() == 1);  // inside the 2 s backoff: no new attempt
    BOOST_TEST(fetches == 2);
    clk.advance(1s);
    BOOST_TEST(v.get() == 1);  // backoff over: tries again, fails again
    BOOST_TEST(fetches == 3);
    clk.advance(20s);  // past expiry
    BOOST_CHECK_THROW((void)v.get(), cc::exchange_error);
    const int after_throw = fetches;
    BOOST_CHECK_THROW((void)v.get(), cc::exchange_error);  // backing off: no new attempt
    BOOST_TEST(fetches == after_throw);
    fail = false;
    clk.advance(10s);
    BOOST_TEST(v.get() == fetches);  // recovers
    BOOST_TEST(v.consecutive_failures() == 0);
}

BOOST_AUTO_TEST_CASE(one_refresh_in_flight_and_callers_keep_the_valid_value) {
    std::atomic<int> fetches{0};
    std::atomic<bool> release{false};
    fake_clock clk;
    cc::refreshing_value<int> v(
        [&] {
            const int n = ++fetches;
            if (n > 1) {
                while (!release.load()) {
                    std::this_thread::sleep_for(1ms);
                }
            }
            return cc::refreshing_value<int>::timed{n, clk.now() + 100s};
        },
        {}, clk.fn());
    BOOST_TEST(v.get() == 1);
    clk.advance(90s);  // refresh due, value still valid
    std::thread refresher([&] { (void)v.get(); });
    while (fetches.load() < 2) {
        std::this_thread::sleep_for(1ms);
    }
    // While the refresh is stuck, other callers get the old value at once.
    for (int i = 0; i < 20; ++i) {
        BOOST_TEST(v.get() == 1);
    }
    BOOST_TEST(fetches.load() == 2);
    release = true;
    refresher.join();
    BOOST_TEST(v.get() == 2);
}

BOOST_AUTO_TEST_CASE(cold_callers_wait_for_the_single_fetch) {
    std::atomic<int> fetches{0};
    cc::refreshing_value<int> v([&] {
        ++fetches;
        std::this_thread::sleep_for(50ms);
        return cc::refreshing_value<int>::timed{7, cc::clock::now() + 1h};
    });
    std::vector<std::thread> threads;
    std::atomic<int> sevens{0};
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            if (v.get() == 7) {
                ++sevens;
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    BOOST_TEST(fetches.load() == 1);
    BOOST_TEST(sevens.load() == 8);
}

// ---------------------------------------------------------------------------
// Token source
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(token_source_caches_per_audience) {
    std::map<std::string, int> calls;
    cc::kythira_oidc_token_source src([&](const std::string& aud) {
        ++calls[aud];
        return cc::oidc_token{"jwt-for-" + aud, cc::clock::now() + 1h};
    });
    BOOST_TEST(src.token("sts.amazonaws.com") == "jwt-for-sts.amazonaws.com");
    BOOST_TEST(src.token("sts.amazonaws.com") == "jwt-for-sts.amazonaws.com");
    BOOST_TEST(src.token("api://AzureADTokenExchange") == "jwt-for-api://AzureADTokenExchange");
    BOOST_TEST(calls["sts.amazonaws.com"] == 1);
    BOOST_TEST(calls["api://AzureADTokenExchange"] == 1);
}

BOOST_AUTO_TEST_CASE(parses_the_token_endpoint_response) {
    const auto now = cc::clock::from_time_t(1'800'000'000);
    auto t = cc::parse_oidc_token_response(
        R"({"token":"abc","token_type":"urn:ietf:params:oauth:token-type:jwt","expires_at":"x","expires_in":600})",
        now);
    BOOST_TEST(t.token == "abc");
    BOOST_TEST((t.expires_at == now + 600s));
    BOOST_CHECK_THROW((void)cc::parse_oidc_token_response("{}", now), cc::exchange_error);
    BOOST_CHECK_THROW((void)cc::parse_oidc_token_response(R"({"token":"a","expires_in":0})", now),
                      cc::exchange_error);
}

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
// The real fetch over mTLS, against two TLS servers standing in for a
// follower (which redirects) and the leader (which checks the client
// certificate and the form body). httplib's own redirect would drop the
// client certificate on the way to another port; the source must not.
BOOST_AUTO_TEST_CASE(fetch_presents_the_certificate_through_a_redirect) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("cc_exchange_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    raft::testing::certificate_authority ca;
    std::ofstream(dir / "root.pem") << ca.root_certificate_pem();
    {
        raft::testing::leaf_certificate_options o;
        o.subject.common_name = "server";
        o.ip_addresses = {"127.0.0.1"};
        auto leaf = ca.issue(o);
        std::ofstream(dir / "server.pem") << leaf.chain_pem;
        std::ofstream(dir / "server.key") << leaf.private_key_pem;
    }
    {
        raft::testing::leaf_certificate_options o;
        o.subject.common_name = "node";
        o.dns_names = {"ca-cluster-node-1"};
        o.server_auth = false;
        o.client_auth = true;
        auto leaf = ca.issue(o);
        std::ofstream(dir / "client.pem") << leaf.chain_pem;
        std::ofstream(dir / "client.key") << leaf.private_key_pem;
    }
    const auto p = [&](const char* f) { return (dir / f).string(); };

    httplib::SSLServer leader(p("server.pem").c_str(), p("server.key").c_str(),
                              p("root.pem").c_str());
    std::atomic<int> leader_hits{0};
    leader.Post("/v1/oidc/token", [&](const httplib::Request& req, httplib::Response& res) {
        ++leader_hits;
        BOOST_TEST(req.ssl != nullptr);
        BOOST_TEST(req.get_param_value("audience") == "sts.amazonaws.com");
        BOOST_TEST(req.get_param_value("lifetime") == "900");
        res.set_content(R"({"token":"signed.jwt.here","expires_in":900})", "application/json");
    });
    const int leader_port = leader.bind_to_any_port("127.0.0.1");
    std::thread lt([&] { leader.listen_after_bind(); });
    leader.wait_until_ready();

    httplib::SSLServer follower(p("server.pem").c_str(), p("server.key").c_str(),
                                p("root.pem").c_str());
    follower.Post("/v1/oidc/token", [&](const httplib::Request&, httplib::Response& res) {
        res.status = 308;
        res.set_header("Location",
                       "https://127.0.0.1:" + std::to_string(leader_port) + "/v1/oidc/token");
    });
    const int follower_port = follower.bind_to_any_port("127.0.0.1");
    std::thread ft([&] { follower.listen_after_bind(); });
    follower.wait_until_ready();

    cc::oidc_token_source_config cfg;
    cfg.ca_url = "https://127.0.0.1:" + std::to_string(follower_port);
    cfg.client_cert_path = p("client.pem");
    cfg.client_key_path = p("client.key");
    cfg.ca_bundle_path = p("root.pem");
    cfg.lifetime = 900s;
    auto token = cc::fetch_oidc_token(cfg, "sts.amazonaws.com");
    BOOST_TEST(token.token == "signed.jwt.here");
    BOOST_TEST(leader_hits.load() == 1);

    // Without a client certificate the leader's TLS layer refuses the
    // handshake, so the fetch fails loudly.
    cfg.ca_url = "https://127.0.0.1:" + std::to_string(leader_port);
    cfg.client_cert_path.clear();
    cfg.client_key_path.clear();
    BOOST_CHECK_THROW((void)cc::fetch_oidc_token(cfg, "sts.amazonaws.com"), cc::exchange_error);

    leader.stop();
    follower.stop();
    lt.join();
    ft.join();
    fs::remove_all(dir);
}
#endif

// ---------------------------------------------------------------------------
// AWS
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(aws_assume_role_with_web_identity_wire_format) {
    mock_server sts;
    std::atomic<int> hits{0};
    sts.server.Post("/", [&](const httplib::Request& req, httplib::Response& res) {
        ++hits;
        BOOST_TEST(req.get_header_value("Content-Type") == "application/x-www-form-urlencoded");
        BOOST_TEST(!req.has_header("Authorization"));  // the call is unsigned
        BOOST_TEST(req.get_param_value("Action") == "AssumeRoleWithWebIdentity");
        BOOST_TEST(req.get_param_value("Version") == "2011-06-15");
        BOOST_TEST(req.get_param_value("RoleArn") == "arn:aws:iam::123456789012:role/kythira-node");
        BOOST_TEST(req.get_param_value("RoleSessionName") == "node-1");
        BOOST_TEST(req.get_param_value("WebIdentityToken") == "hdr.payload.sig");
        BOOST_TEST(req.get_param_value("DurationSeconds") == "900");
        res.set_content(
            R"(<AssumeRoleWithWebIdentityResponse xmlns="https://sts.amazonaws.com/doc/2011-06-15/">
  <AssumeRoleWithWebIdentityResult>
    <SubjectFromWebIdentityToken>node:1</SubjectFromWebIdentityToken>
    <Credentials>
      <SessionToken>FwoGZX&amp;token</SessionToken>
      <SecretAccessKey>test-fixture-secret-access-key</SecretAccessKey>
      <Expiration>2027-01-15T09:00:00Z</Expiration>
      <AccessKeyId>ASIA-TEST-FIXTURE-KEY-ID</AccessKeyId>
    </Credentials>
  </AssumeRoleWithWebIdentityResult>
</AssumeRoleWithWebIdentityResponse>)",
            "text/xml");
    });
    sts.start();

    cc::aws_web_identity_config cfg;
    cfg.role_arn = "arn:aws:iam::123456789012:role/kythira-node";
    cfg.role_session_name = "node-1";
    cfg.duration = 900s;
    cfg.sts_endpoint = sts.url();
    fake_clock clk;  // 2027-01-15T08:00:00Z, an hour before Expiration
    cc::aws_web_identity_source src(fixed_token_source("hdr.payload.sig"), cfg, {}, clk.fn());
    auto creds = src.current();
    BOOST_TEST(creds.access_key_id == "ASIA-TEST-FIXTURE-KEY-ID");
    BOOST_TEST(creds.secret_access_key == "test-fixture-secret-access-key");
    BOOST_TEST(creds.session_token == "FwoGZX&token");
    BOOST_TEST((creds.expires_at == cc::clock::from_time_t(1'800'003'600)));
    (void)src.current();
    BOOST_TEST(hits.load() == 1);  // cached
}

BOOST_AUTO_TEST_CASE(aws_error_response_names_the_sts_code) {
    try {
        (void)cc::parse_assume_role_with_web_identity(
            403, R"(<ErrorResponse><Error><Type>Sender</Type><Code>InvalidIdentityToken</Code>
<Message>Couldn't retrieve verification key from your identity provider</Message></Error></ErrorResponse>)");
        BOOST_FAIL("expected an exchange_error");
    } catch (const cc::exchange_error& e) {
        BOOST_TEST(std::string(e.what()).find("InvalidIdentityToken") != std::string::npos);
    }
    BOOST_CHECK_THROW(
        (void)cc::parse_assume_role_with_web_identity(200, "<Credentials></Credentials>"),
        cc::exchange_error);
}

// ---------------------------------------------------------------------------
// GCP
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(gcp_token_exchange_and_impersonation_wire_format) {
    const std::string provider =
        "//iam.googleapis.com/projects/123/locations/global/workloadIdentityPools/kythira/"
        "providers/ca";
    mock_server sts;
    sts.server.Post("/v1/token", [&](const httplib::Request& req, httplib::Response& res) {
        BOOST_TEST(req.get_param_value("grant_type") ==
                   "urn:ietf:params:oauth:grant-type:token-exchange");
        BOOST_TEST(req.get_param_value("audience") == provider);
        BOOST_TEST(req.get_param_value("scope") ==
                   "https://www.googleapis.com/auth/cloud-platform");
        BOOST_TEST(req.get_param_value("requested_token_type") ==
                   "urn:ietf:params:oauth:token-type:access_token");
        BOOST_TEST(req.get_param_value("subject_token") == "kythira.jwt.token");
        BOOST_TEST(req.get_param_value("subject_token_type") ==
                   "urn:ietf:params:oauth:token-type:jwt");
        res.set_content(
            R"({"access_token":"federated-token","issued_token_type":"urn:ietf:params:oauth:token-type:access_token","token_type":"Bearer","expires_in":3599})",
            "application/json");
    });
    sts.start();
    mock_server iam;
    iam.server.Post(
        R"(/v1/projects/-/serviceAccounts/(.+):generateAccessToken)",
        [&](const httplib::Request& req, httplib::Response& res) {
            // httplib matches on the decoded path; the escaped form is on the wire.
            BOOST_TEST(req.matches[1].str() == "storage-writer@proj.iam.gserviceaccount.com");
            BOOST_TEST(req.target.find("storage-writer%40proj") != std::string::npos);
            BOOST_TEST(req.get_header_value("Authorization") == "Bearer federated-token");
            auto body = boost::json::parse(req.body).as_object();
            BOOST_TEST(body.at("scope").as_array().at(0).as_string() ==
                       "https://www.googleapis.com/auth/cloud-platform");
            BOOST_TEST(body.at("lifetime").as_string() == "1800s");
            res.set_content(
                R"({"accessToken":"ya29.impersonated","expireTime":"2027-01-15T08:30:00Z"})",
                "application/json");
        });
    iam.start();

    cc::gcp_workload_identity_config cfg;
    cfg.provider_audience = provider;
    cfg.sts_endpoint = sts.url();
    cfg.iamcredentials_endpoint = iam.url();
    fake_clock clk;

    // Federated token used directly.
    std::string audience_asked;
    auto tokens = std::make_shared<cc::kythira_oidc_token_source>([&](const std::string& aud) {
        audience_asked = aud;
        return cc::oidc_token{"kythira.jwt.token", cc::clock::now() + 1h};
    });
    cc::gcp_workload_identity_source direct(tokens, cfg, {}, clk.fn());
    auto t1 = direct.current();
    BOOST_TEST(t1.access_token == "federated-token");
    BOOST_TEST((t1.expires_at == clk.now() + 3599s));
    BOOST_TEST(audience_asked == provider);  // the OIDC audience is the provider name

    // Then impersonating a service account.
    cfg.service_account_email = "storage-writer@proj.iam.gserviceaccount.com";
    cfg.impersonation_lifetime = 1800s;
    cc::gcp_workload_identity_source imp(tokens, cfg, {}, clk.fn());
    auto t2 = imp.current();
    BOOST_TEST(t2.access_token == "ya29.impersonated");
    BOOST_TEST((t2.expires_at == cc::clock::from_time_t(1'800'001'800)));
}

BOOST_AUTO_TEST_CASE(gcp_errors_carry_the_service_message) {
    const auto now = cc::clock::now();
    try {
        (void)cc::parse_gcp_sts_exchange(
            400,
            R"({"error":"invalid_grant","error_description":"The audience in ID Token does not match"})",
            now);
        BOOST_FAIL("expected an exchange_error");
    } catch (const cc::exchange_error& e) {
        BOOST_TEST(std::string(e.what()).find("audience in ID Token") != std::string::npos);
    }
    try {
        (void)cc::parse_gcp_generate_access_token(
            403,
            R"({"error":{"code":403,"message":"Permission 'iam.serviceAccounts.getAccessToken' denied"}})");
        BOOST_FAIL("expected an exchange_error");
    } catch (const cc::exchange_error& e) {
        BOOST_TEST(std::string(e.what()).find("getAccessToken") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(rfc3339_parsing) {
    BOOST_TEST((*cc::detail::parse_rfc3339_utc("2027-01-15T08:00:00Z") ==
                cc::clock::from_time_t(1'800'000'000)));
    BOOST_TEST((*cc::detail::parse_rfc3339_utc("2027-01-15T08:00:00.123456Z") ==
                cc::clock::from_time_t(1'800'000'000)));
    BOOST_TEST(!cc::detail::parse_rfc3339_utc("2027-01-15T08:00:00+01:00").has_value());
    BOOST_TEST(!cc::detail::parse_rfc3339_utc("2027-01-15 08:00:00Z").has_value());
}

// ---------------------------------------------------------------------------
// Azure (task 4)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(azure_client_assertion_wire_format_and_per_scope_cache) {
    mock_server entra;
    std::atomic<int> hits{0};
    entra.server.Post(
        "/tenant-guid/oauth2/v2.0/token", [&](const httplib::Request& req, httplib::Response& res) {
            ++hits;
            BOOST_TEST(req.get_header_value("Content-Type") == "application/x-www-form-urlencoded");
            BOOST_TEST(!req.has_header("Authorization"));  // the assertion is the proof
            BOOST_TEST(req.get_param_value("grant_type") == "client_credentials");
            BOOST_TEST(req.get_param_value("client_id") == "app-guid");
            BOOST_TEST(req.get_param_value("client_assertion_type") ==
                       "urn:ietf:params:oauth:client-assertion-type:jwt-bearer");
            BOOST_TEST(req.get_param_value("client_assertion") == "kythira.jwt.token");
            const auto scope = req.get_param_value("scope");
            // The v2.0 endpoint sends a number; a string is accepted too.
            res.set_content(
                scope == "https://vault.azure.net/.default"
                    ? R"({"token_type":"Bearer","expires_in":"1800","access_token":"vault-token"})"
                    : R"({"token_type":"Bearer","expires_in":3599,"access_token":"storage-token"})",
                "application/json");
        });
    entra.start();

    cc::azure_federated_config cfg;
    cfg.tenant_id = "tenant-guid";
    cfg.client_id = "app-guid";
    cfg.authority = entra.url();
    std::string audience_asked;
    auto tokens = std::make_shared<cc::kythira_oidc_token_source>([&](const std::string& aud) {
        audience_asked = aud;
        return cc::oidc_token{"kythira.jwt.token", cc::clock::now() + 1h};
    });
    fake_clock clk;
    cc::azure_federated_source src(tokens, cfg, {}, clk.fn());

    const auto storage = src.current();
    BOOST_TEST(storage.access_token == "storage-token");
    BOOST_TEST((storage.expires_at == clk.now() + 3599s));
    BOOST_TEST(audience_asked == "api://AzureADTokenExchange");

    const auto vault = src.token("https://vault.azure.net/.default");
    BOOST_TEST(vault.access_token == "vault-token");
    BOOST_TEST((vault.expires_at == clk.now() + 1800s));

    (void)src.current();
    (void)src.token("https://vault.azure.net/.default");
    BOOST_TEST(hits.load() == 2);  // one exchange per scope, then cached
}

BOOST_AUTO_TEST_CASE(azure_errors_carry_the_aadsts_line) {
    try {
        (void)cc::parse_azure_token(
            400,
            R"({"error":"invalid_client","error_description":"AADSTS70021: No matching federated identity record found for presented assertion.\r\nTrace ID: abc\r\nTimestamp: 2026-11-05"})",
            cc::clock::now());
        BOOST_FAIL("expected an exchange_error");
    } catch (const cc::exchange_error& e) {
        const std::string what = e.what();
        BOOST_TEST(what.find("AADSTS70021") != std::string::npos);
        BOOST_TEST(what.find("Trace ID") == std::string::npos);
    }
    BOOST_CHECK_THROW((void)cc::parse_azure_token(200, R"({"access_token":"t"})", cc::clock::now()),
                      cc::exchange_error);
}

// ---------------------------------------------------------------------------
// Alibaba (task 4)
// ---------------------------------------------------------------------------

namespace {

auto alibaba_sts_mock(mock_server& sts, std::atomic<int>& hits) -> void {
    sts.server.Post("/", [&](const httplib::Request& req, httplib::Response& res) {
        ++hits;
        BOOST_TEST(!req.has_header("Authorization"));  // the call is anonymous
        BOOST_TEST(req.get_param_value("Action") == "AssumeRoleWithOIDC");
        BOOST_TEST(req.get_param_value("Format") == "JSON");
        BOOST_TEST(req.get_param_value("Version") == "2015-04-01");
        BOOST_TEST(req.get_param_value("Timestamp") == "2027-01-15T08:00:00Z");
        // The call's own parameters travel in the body, the common ones in
        // the query.
        BOOST_TEST(req.target.find("OIDCToken") == std::string::npos);
        BOOST_TEST(req.body.find("Action=") == std::string::npos);
        BOOST_TEST(req.get_param_value("RoleArn") == "acs:ram::1234567890:role/kythira-node");
        BOOST_TEST(req.get_param_value("OIDCProviderArn") ==
                   "acs:ram::1234567890:oidc-provider/kythira");
        BOOST_TEST(req.get_param_value("OIDCToken") == "kythira.jwt.token");
        BOOST_TEST(req.get_param_value("RoleSessionName") == "node-1");
        BOOST_TEST(req.get_param_value("DurationSeconds") == "900");
        res.set_content(
            R"({"RequestId":"r-1","Credentials":{"AccessKeyId":"STS.TEST-FIXTURE-KEY-ID","AccessKeySecret":"test-fixture-key-secret","SecurityToken":"test-fixture-security-token","Expiration":"2027-01-15T09:00:00Z"},"AssumedRoleUser":{"AssumedRoleId":"3:node-1","Arn":"acs:ram::1234567890:role/kythira-node/node-1"}})",
            "application/json");
    });
}

auto alibaba_role_config(const std::string& endpoint) -> cc::alibaba_oidc_role_config {
    cc::alibaba_oidc_role_config cfg;
    cfg.role_arn = "acs:ram::1234567890:role/kythira-node";
    cfg.oidc_provider_arn = "acs:ram::1234567890:oidc-provider/kythira";
    cfg.role_session_name = "node-1";
    cfg.duration = 900s;
    cfg.sts_endpoint = endpoint;
    return cfg;
}

}  // namespace

BOOST_AUTO_TEST_CASE(alibaba_assume_role_with_oidc_wire_format) {
    mock_server sts;
    std::atomic<int> hits{0};
    alibaba_sts_mock(sts, hits);
    sts.start();

    std::string audience_asked;
    auto tokens = std::make_shared<cc::kythira_oidc_token_source>([&](const std::string& aud) {
        audience_asked = aud;
        return cc::oidc_token{"kythira.jwt.token", cc::clock::now() + 1h};
    });
    fake_clock clk;  // 2027-01-15T08:00:00Z
    cc::alibaba_oidc_role_source src(tokens, alibaba_role_config(sts.url()), {}, clk.fn());
    const auto creds = src.current();
    BOOST_TEST(creds.access_key_id == "STS.TEST-FIXTURE-KEY-ID");
    BOOST_TEST(creds.access_key_secret == "test-fixture-key-secret");
    BOOST_TEST(creds.security_token == "test-fixture-security-token");
    BOOST_TEST((creds.expires_at == cc::clock::from_time_t(1'800'003'600)));
    BOOST_TEST(audience_asked == "sts.aliyuncs.com");
    (void)src.current();
    BOOST_TEST(hits.load() == 1);  // cached
}

BOOST_AUTO_TEST_CASE(alibaba_signers_use_the_exchanged_keys) {
    mock_server sts;
    std::atomic<int> hits{0};
    alibaba_sts_mock(sts, hits);
    sts.start();
    fake_clock clk;
    auto src = std::make_shared<cc::alibaba_oidc_role_source>(
        fixed_token_source("kythira.jwt.token"), alibaba_role_config(sts.url()),
        cc::refresh_policy{}, clk.fn());

    kythira::alibaba_client_config cfg;
    cfg.region = "cn-hangzhou";
    cfg.credentials = [src] { return src->current(); };

    // Control plane (ACS3).
    const auto acs = kythira::alibaba_signing::sign_request(cfg, "POST", "ess.aliyuncs.com", "/",
                                                            {}, "DescribeScalingGroups",
                                                            "2014-08-28", "", clk.now(), "nonce-1");
    BOOST_TEST(acs.at("x-acs-security-token") == "test-fixture-security-token");
    BOOST_TEST(acs.at("Authorization")
                   .starts_with("ACS3-HMAC-SHA256 Credential=STS.TEST-FIXTURE-KEY-ID,"));

    // OSS (V4).
    cfg.endpoint_override = "http://127.0.0.1:1";
    const kythira::alibaba_oss_client oss{cfg};
    const auto o = oss.prepared_headers("bucket", "key", "GET", {}, clk.now());
    BOOST_TEST(o.at("x-oss-security-token") == "test-fixture-security-token");
    BOOST_TEST(
        o.at("Authorization").starts_with("OSS4-HMAC-SHA256 Credential=STS.TEST-FIXTURE-KEY-ID/"));
    BOOST_TEST(hits.load() == 1);  // both signers read the one cached set
}

BOOST_AUTO_TEST_CASE(alibaba_errors_carry_the_sts_code) {
    try {
        (void)cc::parse_alibaba_assume_role_with_oidc(
            400,
            R"({"RequestId":"r","HostId":"sts.aliyuncs.com","Code":"AuthenticationFail.OIDCToken.Invalid","Message":"The OIDC token is invalid."})");
        BOOST_FAIL("expected an exchange_error");
    } catch (const cc::exchange_error& e) {
        BOOST_TEST(std::string(e.what()).find("AuthenticationFail.OIDCToken.Invalid") !=
                   std::string::npos);
    }
    BOOST_CHECK_THROW((void)cc::parse_alibaba_assume_role_with_oidc(200, R"({"RequestId":"r"})"),
                      cc::exchange_error);
}

// ---------------------------------------------------------------------------
// OCI (task 4)
// ---------------------------------------------------------------------------

// The source needs the session key generator, which needs TLS support.
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
namespace {

/// An unsigned JWT-shaped UPST whose only claim that matters is exp.
auto fake_upst(std::int64_t exp) -> std::string {
    const auto b64url = [](const std::string& in) {
        static constexpr char tbl[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string out;
        std::uint32_t buf = 0;
        int bits = 0;
        for (unsigned char c : in) {
            buf = (buf << 8) | c;
            bits += 8;
            while (bits >= 6) {
                bits -= 6;
                out.push_back(tbl[(buf >> bits) & 0x3f]);
            }
        }
        if (bits > 0) {
            out.push_back(tbl[(buf << (6 - bits)) & 0x3f]);
        }
        return out;
    };
    return b64url(R"({"alg":"RS256"})") + "." +
           b64url(R"({"sub":"workload","exp":)" + std::to_string(exp) + "}") + ".sig";
}

}  // namespace

BOOST_AUTO_TEST_CASE(oci_upst_exchange_wire_format_and_signing) {
    mock_server domain;
    std::atomic<int> hits{0};
    std::string submitted_public_key;
    const auto upst = fake_upst(1'800'003'600);
    domain.server.Post("/oauth2/v1/token", [&](const httplib::Request& req,
                                               httplib::Response& res) {
        ++hits;
        // HTTP Basic with the confidential app's id and secret.
        BOOST_TEST(req.get_header_value("Authorization") ==
                   httplib::make_basic_authentication_header("app-id", "app-secret").second);
        BOOST_TEST(req.get_param_value("grant_type") ==
                   "urn:ietf:params:oauth:grant-type:token-exchange");
        BOOST_TEST(req.get_param_value("requested_token_type") == "urn:oci:token-type:oci-upst");
        BOOST_TEST(req.get_param_value("subject_token_type") == "jwt");
        BOOST_TEST(req.get_param_value("subject_token") == "kythira.jwt.token");
        submitted_public_key = req.get_param_value("public_key");
        res.set_content(R"({"token":")" + upst + R"("})", "application/json");
    });
    domain.start();

    cc::oci_upst_config cfg;
    cfg.domain_url = domain.url();
    cfg.client_id = "app-id";
    cfg.client_secret = "app-secret";
    std::string audience_asked;
    auto tokens = std::make_shared<cc::kythira_oidc_token_source>([&](const std::string& aud) {
        audience_asked = aud;
        return cc::oidc_token{"kythira.jwt.token", cc::clock::now() + 1h};
    });
    fake_clock clk;
    auto src = std::make_shared<cc::oci_upst_source>(tokens, cfg, cc::refresh_policy{}, clk.fn());

    const auto session = src->current();
    BOOST_TEST(session.security_token == upst);
    BOOST_TEST((session.expires_at == cc::clock::from_time_t(1'800'003'600)));
    BOOST_TEST(audience_asked == "https://cloud.oracle.com");
    // The key submitted is the public half of the key handed out.
    BOOST_TEST(submitted_public_key ==
               kythira::oci_federation::public_key_der_base64(session.private_key_pem));

    // The signer uses the session: keyId ST$<upst>, and a signature that
    // verifies under the submitted public key is implied by the matching key.
    kythira::oci_client_config oci;
    oci.region = "us-phoenix-1";
    oci.session_credentials = [src] { return src->current(); };
    const auto headers = kythira::oci_signing::sign_request(
        oci, "GET", "/n/ns/b/bucket/o/key", "objectstorage.us-phoenix-1.oraclecloud.com", "");
    BOOST_TEST(headers.at("authorization").find("keyId=\"ST$" + upst + "\"") != std::string::npos);
    BOOST_TEST(hits.load() == 1);  // cached
}

#endif  // CPPHTTPLIB_OPENSSL_SUPPORT

BOOST_AUTO_TEST_CASE(oci_errors_carry_the_domain_message) {
    try {
        (void)cc::parse_oci_upst_exchange(
            400, R"({"error":"invalid_grant","error_description":"No rules matched the token."})");
        BOOST_FAIL("expected an exchange_error");
    } catch (const cc::exchange_error& e) {
        BOOST_TEST(std::string(e.what()).find("No rules matched") != std::string::npos);
    }
    BOOST_CHECK_THROW((void)cc::parse_oci_upst_exchange(200, R"({"expires_in":3600})"),
                      cc::exchange_error);
}
