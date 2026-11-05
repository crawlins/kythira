// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Real-cloud OIDC federation (`.kiro/specs/iam-compatible-auth/` task 5,
// Requirement 2.5). One ca_cluster_node is the issuer. It publishes its
// discovery document and JWKS to a public S3 prefix (--oidc-publish-url), a
// workload certificate from it is exchanged for a Kythira JWT, and the JWT is
// exchanged with each cloud's token service for credentials that read one
// test object, through the same SDK clients the persistence engine uses.
//
// The trust on the cloud side (an IAM OIDC provider and role, a GCP workload
// identity pool and provider, each allowed to read only the test object) is
// set up once by scripts/ci-cloud-credentials/*/provision-oidc-federation-test.sh,
// because the CI roles may not administer IAM. Each run therefore creates
// only the two published objects, and deletes them at the end.
//
// Configuration, all from the environment (.github/workflows/oidc-federation.yml):
//   KYTHIRA_OIDC_FED_ISSUER_URL   https://<bucket>.s3.<region>.amazonaws.com/<prefix>
//   KYTHIRA_OIDC_FED_PUBLISH_URL  s3://<bucket>/<prefix>
//   KYTHIRA_OIDC_FED_SUBJECT_URI  the workload URI the trust names
//   KYTHIRA_OIDC_FED_EXPECTED     the test objects' content
//   AWS:  KYTHIRA_OIDC_FED_AWS_ROLE_ARN, _AWS_REGION, _AWS_BUCKET, _AWS_KEY
//   GCP:  KYTHIRA_OIDC_FED_GCP_PROVIDER (the provider's full resource name,
//         //iam.googleapis.com/projects/...), _GCS_BUCKET, _GCS_KEY
// With no issuer URL the binary exits 77 (ctest "skipped"). A cloud whose
// variables are missing is skipped with a message; at least one must be set.

#define BOOST_TEST_MODULE oidc_federation_real_test

#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>
#include <raft/cloud_credential_exchange.hpp>

#ifdef KYTHIRA_HAS_AWS_SDK
#include <raft/aws_s3_client.hpp>
#include <raft/aws_web_identity_credentials.hpp>

#include <aws/core/Aws.h>
#endif
#ifdef KYTHIRA_HAS_GCP_STORAGE
#include <raft/gcp_gcs_client.hpp>
#endif

#include "ca_cluster_node_harness.hpp"

#include <httplib.h>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace raft::testing;
using namespace ca_cluster_harness;
namespace cc = kythira::cloud_credentials;

namespace {

constexpr const char* k_auth_token = "oidc-federation-test-token";
constexpr const char* k_aws_audience = "sts.amazonaws.com";

auto env(const char* name) -> std::string {
    const char* v = std::getenv(name);
    return v == nullptr ? std::string{} : std::string(v);
}

/// Skips the whole module, with ctest's skip code, when not configured.
struct require_configuration {
    require_configuration() {
        if (env("KYTHIRA_OIDC_FED_ISSUER_URL").empty()) {
            std::fprintf(stderr,
                         "oidc_federation_real_test: KYTHIRA_OIDC_FED_ISSUER_URL is not set; "
                         "skipped\n");
            // _Exit, not exit: nothing set up yet needs tearing down, and
            // running static destructors from inside Boost.Test's setup is
            // what segfaulted on CI (arm64, clang++-18) when this check ran
            // after InitAPI.
            std::fflush(stderr);
            std::_Exit(77);
        }
    }
};

#ifdef KYTHIRA_HAS_AWS_SDK
struct aws_sdk {
    Aws::SDKOptions options;
    aws_sdk() { Aws::InitAPI(options); }
    ~aws_sdk() { Aws::ShutdownAPI(options); }
    aws_sdk(const aws_sdk&) = delete;
    auto operator=(const aws_sdk&) -> aws_sdk& = delete;
};
#endif

// One registration, so the order is fixed: Boost.Test runs separately
// registered global fixtures in link-address order, which put InitAPI ahead
// of the skip check (see ca_cluster_node_real_ec2_test.cpp for the same).
struct suite_fixtures {
    require_configuration preflight;
#ifdef KYTHIRA_HAS_AWS_SDK
    aws_sdk sdk;
#endif
};
BOOST_TEST_GLOBAL_FIXTURE(suite_fixtures);

/// Splits "https://host/path" into ("host", "/path").
auto split_https(const std::string& url) -> std::pair<std::string, std::string> {
    const std::string rest = url.substr(std::string("https://").size());
    const auto slash = rest.find('/');
    return {rest.substr(0, slash), slash == std::string::npos ? "" : rest.substr(slash)};
}

/// GET over the public internet with the system trust store.
auto public_get(const std::string& url) -> std::optional<std::string> {
    const auto [host, path] = split_https(url);
    httplib::SSLClient c(host, 443);
    c.enable_server_certificate_verification(true);
    c.set_connection_timeout(10, 0);
    c.set_read_timeout(20, 0);
    auto res = c.Get(path);
    if (!res || res->status != 200) {
        return std::nullopt;
    }
    return res->body;
}

auto kid_set(const std::string& jwks) -> std::vector<std::string> {
    std::vector<std::string> kids;
    // Named, not iterated in place: before C++23's P2718 (clang 19), a
    // range-for does not extend the parse() temporary, and the loop would
    // walk a freed array.
    const auto doc = boost::json::parse(jwks);
    for (const auto& k : doc.at("keys").as_array()) {
        kids.emplace_back(k.as_object().at("kid").as_string());
    }
    std::sort(kids.begin(), kids.end());
    return kids;
}

}  // namespace

BOOST_AUTO_TEST_CASE(federates_to_each_configured_cloud, *boost::unit_test::timeout(900)) {
    const std::string issuer = env("KYTHIRA_OIDC_FED_ISSUER_URL");
    const std::string publish = env("KYTHIRA_OIDC_FED_PUBLISH_URL");
    const std::string subject_uri = env("KYTHIRA_OIDC_FED_SUBJECT_URI");
    const std::string expected = env("KYTHIRA_OIDC_FED_EXPECTED");
    BOOST_REQUIRE_MESSAGE(issuer.starts_with("https://") && !publish.empty() &&
                              !subject_uri.empty() && !expected.empty(),
                          "set KYTHIRA_OIDC_FED_PUBLISH_URL, _SUBJECT_URI and _EXPECTED too");
    const std::string prefix = subject_uri.substr(0, subject_uri.rfind('/') + 1);

    const std::string aws_role = env("KYTHIRA_OIDC_FED_AWS_ROLE_ARN");
    const std::string gcp_provider = env("KYTHIRA_OIDC_FED_GCP_PROVIDER");
    BOOST_REQUIRE_MESSAGE(!aws_role.empty() || !gcp_provider.empty(),
                          "configure at least one cloud");

    lone_node_env dir("oidc_federation");
    certificate_authority listener_ca;
    const std::string listener_root = dir.tmp_root + "/listener_root.pem";
    std::ofstream(listener_root) << listener_ca.root_certificate_pem();
    {
        leaf_certificate_options o;
        o.subject.common_name = "ca-node-1";
        o.ip_addresses = {"127.0.0.1"};
        auto leaf = listener_ca.issue(o);
        std::ofstream(dir.tmp_root + "/cert.pem") << leaf.chain_pem;
        std::ofstream(dir.tmp_root + "/key.pem") << leaf.private_key_pem;
    }

    std::vector<std::string> args{"--rpc-address",
                                  "127.0.0.1",
                                  "--http-address",
                                  "127.0.0.1",
                                  "--tls-cert",
                                  dir.tmp_root + "/cert.pem",
                                  "--tls-key",
                                  dir.tmp_root + "/key.pem",
                                  "--peer-tls-ca",
                                  listener_root,
                                  "--oidc-issuer-url",
                                  issuer,
                                  "--oidc-publish-url",
                                  publish,
                                  "--oidc-workload-uri-prefix",
                                  prefix};
    if (!aws_role.empty()) {
        args.insert(args.end(), {"--oidc-audience", k_aws_audience});
    }
    if (!gcp_provider.empty()) {
        args.insert(args.end(), {"--oidc-audience", gcp_provider});
    }

    const int rpc_port = find_free_port();
    const int http_port = find_free_port();
    const std::string peers = "1:127.0.0.1:" + std::to_string(rpc_port) +
                              "@https://127.0.0.1:" + std::to_string(http_port);
    cluster_node_process node(1, rpc_port, http_port, dir.tmp_root + "/node1", dir.unseal_key_file,
                              k_auth_token, peers, /*bootstrap=*/true, args);

    auto client = [&] {
        auto c = std::make_unique<httplib::SSLClient>("127.0.0.1", http_port);
        c->set_ca_cert_path(listener_root);
        c->enable_server_certificate_verification(true);
        c->set_connection_timeout(5, 0);
        c->set_read_timeout(30, 0);
        return c;
    };
    const httplib::Headers bearer{{"Authorization", std::string("Bearer ") + k_auth_token}};

    // The workload certificate whose subject the cloud trust names.
    leaf_certificate_options wl;
    wl.subject.common_name = "oidc-federation-test";
    auto csr = generate_key_and_csr(wl);
    const boost::json::object issue_body{
        {"csr_pem", csr.csr_pem},
        {"uri_names", boost::json::array{boost::json::string(subject_uri)}},
        {"client_auth", true},
        {"server_auth", false}};
    std::string cert_pem;
    std::string local_jwks;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (std::chrono::steady_clock::now() < deadline &&
           (cert_pem.empty() || local_jwks.empty())) {
        if (cert_pem.empty()) {
            auto res = client()->Post("/v1/certificates", bearer,
                                      boost::json::serialize(issue_body), "application/json");
            if (res && res->status == 200) {
                cert_pem = std::string(boost::json::parse(res->body).at("chain_pem").as_string());
            }
        }
        if (local_jwks.empty()) {
            auto res = client()->Get("/.well-known/jwks.json");
            if (res && res->status == 200 && !kid_set(res->body).empty()) {
                local_jwks = res->body;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    BOOST_REQUIRE_MESSAGE(!cert_pem.empty(), "the node never issued the workload certificate");
    BOOST_REQUIRE_MESSAGE(!local_jwks.empty(), "the node never generated a signing key");
    const std::string wl_cert = dir.tmp_root + "/wl_cert.pem";
    const std::string wl_key = dir.tmp_root + "/wl_key.pem";
    std::ofstream(wl_cert) << cert_pem;
    std::ofstream(wl_key) << csr.private_key_pem;

    // The public copies: the same keys, and a discovery document naming the
    // issuer, at the URLs the clouds will fetch.
    const auto local_kids = kid_set(local_jwks);
    bool published = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
    while (!published && std::chrono::steady_clock::now() < deadline) {
        auto discovery = public_get(issuer + "/.well-known/openid-configuration");
        auto jwks = public_get(issuer + "/.well-known/jwks.json");
        if (discovery && jwks) {
            const auto d = boost::json::parse(*discovery).as_object();
            published = d.at("issuer").as_string() == issuer &&
                        d.at("jwks_uri").as_string() == issuer + "/.well-known/jwks.json" &&
                        kid_set(*jwks) == local_kids;
        }
        if (!published) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
    BOOST_REQUIRE_MESSAGE(published, "the issuer documents never appeared at " << issuer);
    BOOST_TEST_MESSAGE("issuer documents are public at " << issuer);

    cc::oidc_token_source_config token_cfg;
    token_cfg.ca_url = "https://127.0.0.1:" + std::to_string(http_port);
    token_cfg.client_cert_path = wl_cert;
    token_cfg.client_key_path = wl_key;
    token_cfg.ca_bundle_path = listener_root;
    auto tokens = std::make_shared<cc::kythira_oidc_token_source>(token_cfg);

    if (aws_role.empty()) {
        BOOST_TEST_MESSAGE("AWS: not configured, skipped");
    } else {
#ifdef KYTHIRA_HAS_AWS_SDK
        const std::string region = env("KYTHIRA_OIDC_FED_AWS_REGION");
        cc::aws_web_identity_config role;
        role.role_arn = aws_role;
        role.role_session_name = "kythira-oidc-federation-test";
        role.duration = std::chrono::seconds(900);
        role.sts_endpoint = "https://sts." + region + ".amazonaws.com";
        auto sts = std::make_shared<cc::aws_web_identity_source>(tokens, role);
        const auto creds = sts->current();
        BOOST_TEST(creds.access_key_id.starts_with("ASIA"));

        kythira::aws_client_config s3cfg;
        s3cfg.region = region;
        s3cfg.credentials_provider = cc::make_web_identity_provider_chain(sts);
        const kythira::aws_s3_client s3{s3cfg};
        const auto got =
            s3.get_object(env("KYTHIRA_OIDC_FED_AWS_BUCKET"), env("KYTHIRA_OIDC_FED_AWS_KEY"));
        BOOST_REQUIRE_MESSAGE(got.has_value(), "AWS: the test object is absent");
        BOOST_TEST(got->body == expected);
        BOOST_TEST_MESSAGE("AWS: read the test object through AssumeRoleWithWebIdentity");
#else
        BOOST_FAIL("AWS is configured but this binary was built without the AWS SDK");
#endif
    }

    if (gcp_provider.empty()) {
        BOOST_TEST_MESSAGE("GCP: not configured, skipped");
    } else {
#ifdef KYTHIRA_HAS_GCP_STORAGE
        cc::gcp_workload_identity_config wif;
        wif.provider_audience = gcp_provider;
        auto source = std::make_shared<cc::gcp_workload_identity_source>(tokens, wif);
        kythira::gcp_client_config gcfg;
        gcfg.access_token = [source] { return source->current(); };
        const kythira::gcp_gcs_client gcs{gcfg};
        const auto got =
            gcs.get_object(env("KYTHIRA_OIDC_FED_GCS_BUCKET"), env("KYTHIRA_OIDC_FED_GCS_KEY"));
        BOOST_REQUIRE_MESSAGE(got.has_value(), "GCP: the test object is absent");
        BOOST_TEST(got->body == expected);
        BOOST_TEST_MESSAGE("GCP: read the test object through the STS token exchange");
#else
        BOOST_FAIL("GCP is configured but this binary was built without google-cloud-cpp storage");
#endif
    }
}
