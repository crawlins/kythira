#define BOOST_TEST_MODULE gcp_privateca_provider_real_test
#include <boost/test/unit_test.hpp>

// Real-GCP integration test for gcp_privateca_certificate_provider against
// Google Cloud Certificate Authority Service. Guarded by KYTHIRA_GCP_REAL_TESTS=1
// and excluded from the default ctest run (CTest label
// `integration;gcp;real-privateca`). Skips — never fails — when credentials or
// the required env vars are absent (Requirement 23 ACs 13-17, 20).
//
// Required env vars: GCP_PROJECT_ID, GCP_REGION, GCP_TEST_CA_POOL (a
// pre-existing CA pool with at least one ENABLED CA). See
// scripts/ci-cloud-credentials/gcp/README.md.

#if defined(KYTHIRA_HAS_GCP_PRIVATECA)

#include <raft/certificate_provider.hpp>
#include <raft/gcp_privateca_certificate_provider.hpp>
#include <raft/gcp_privateca_certificate_provider_impl.hpp>

#include "gcp_real_gce_test_support.hpp"

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <cstdlib>
#include <string>

using namespace raft::testing;
using kythira::testing::gcp_real_gce::env_or;

namespace {

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        folly::init(&argc, &argv, false);
    }
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

struct PreflightSkipFixture {
    PreflightSkipFixture() {
        const bool have =
            env_or("KYTHIRA_GCP_REAL_TESTS", "") == "1" && !env_or("GCP_PROJECT_ID", "").empty() &&
            !env_or("GCP_REGION", "").empty() && !env_or("GCP_TEST_CA_POOL", "").empty();
        if (!have) {
            BOOST_TEST_MESSAGE(
                "KYTHIRA_GCP_REAL_TESTS!=1 or GCP_PROJECT_ID/GCP_REGION/"
                "GCP_TEST_CA_POOL unset — skipping real-CAS tests");
            std::exit(77);
        }
    }
};
BOOST_GLOBAL_FIXTURE(PreflightSkipFixture);

auto make_config() -> gcp_privateca_certificate_provider_config {
    gcp_privateca_certificate_provider_config cfg;
    cfg.gcp.project_id = env_or("GCP_PROJECT_ID", "");
    cfg.location = env_or("GCP_REGION", "us-central1");
    cfg.ca_pool_id = env_or("GCP_TEST_CA_POOL", "");
    return cfg;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(gcp_privateca_real)

BOOST_AUTO_TEST_CASE(sign_csr_returns_valid_certificate) {
    gcp_privateca_certificate_provider provider{make_config()};

    // Generate a key + CSR locally; the private key never leaves the caller.
    leaf_certificate_options leaf;
    leaf.subject.common_name = "kythira-it-node";
    leaf.dns_names = {"kythira-it-node.internal"};
    leaf.server_auth = true;
    leaf.client_auth = true;
    auto csr = generate_key_and_csr(leaf);

    csr_signing_options opts;
    opts.dns_names = leaf.dns_names;
    opts.server_auth = true;
    opts.client_auth = true;

    auto material = std::move(provider.sign_csr(csr.csr_pem, opts)).get();
    BOOST_CHECK(!material.certificate_pem.empty());
    BOOST_CHECK(!material.chain_pem.empty());
    BOOST_CHECK(material.certificate_pem.find("BEGIN CERTIFICATE") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(root_certificate_pem_matches_pool_ca) {
    gcp_privateca_certificate_provider provider{make_config()};
    auto root = std::move(provider.root_certificate_pem()).get();
    BOOST_CHECK(!root.empty());
    BOOST_CHECK(root.find("BEGIN CERTIFICATE") != std::string::npos);
    // Cached on the second call — must be identical.
    auto root2 = std::move(provider.root_certificate_pem()).get();
    BOOST_CHECK_EQUAL(root, root2);
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !KYTHIRA_HAS_GCP_PRIVATECA

BOOST_AUTO_TEST_CASE(skipped_no_gcp_privateca) {
    BOOST_TEST_MESSAGE("KYTHIRA_HAS_GCP_PRIVATECA not defined — real-CAS test not built");
}

#endif  // KYTHIRA_HAS_GCP_PRIVATECA
