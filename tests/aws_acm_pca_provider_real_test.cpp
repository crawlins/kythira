// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE aws_acm_pca_provider_real_test
#include <boost/test/unit_test.hpp>

// Real-AWS integration test for aws_acm_pca_provider against an existing ACM
// Private CA (certificate-authority task 11). Opt-in: it needs
// $KYTHIRA_TEST_ACM_PCA_ARN naming an ACTIVE CA and exits 77 (ctest "Not Run")
// when that is unset. Labelled `slow`, so the default CI ctest run excludes it.
//
// Requirement 10.2 keeps CA provisioning out of this component, so the suite
// never creates or deletes a CA. It issues exactly one certificate per run
// (the per-certificate charge is the only cost it adds; see
// doc/aws_acm_pca_test_cost_estimate.md) and revokes it again when the CA has
// a CRL or OCSP configuration.
//
// Region comes from the ARN; credentials from the default AWS chain.

#ifdef KYTHIRA_HAS_AWS_ACM_PCA

#include <raft/aws_acm_pca_provider.hpp>
#include <raft/aws_acm_pca_provider_impl.hpp>

#include "aws_acm_pca_test_support.hpp"

#include <aws/core/Aws.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

using namespace raft::testing;
namespace acm_pca = kythira::testing::acm_pca;

namespace {

auto ca_arn() -> std::string {
    const char* v = std::getenv("KYTHIRA_TEST_ACM_PCA_ARN");
    return v != nullptr ? std::string(v) : std::string{};
}

/// `arn:aws:acm-pca:<region>:<account>:certificate-authority/<id>`.
auto region_from_arn(const std::string& arn) -> std::string {
    std::size_t pos = 0;
    for (int field = 0; field < 3; ++field) {
        pos = arn.find(':', pos);
        if (pos == std::string::npos) {
            return {};
        }
        ++pos;
    }
    auto end = arn.find(':', pos);
    return end == std::string::npos ? std::string{} : arn.substr(pos, end - pos);
}

// Registered first so an unconfigured run exits before the SDK initializes.
struct PreflightSkipFixture {
    PreflightSkipFixture() {
        if (ca_arn().empty()) {
            std::cerr << "SKIP: KYTHIRA_TEST_ACM_PCA_ARN unset — real ACM Private CA tests "
                         "not run\n";
            std::exit(77);
        }
        if (region_from_arn(ca_arn()).empty()) {
            std::cerr << "SKIP: KYTHIRA_TEST_ACM_PCA_ARN is not an acm-pca ARN\n";
            std::exit(77);
        }
    }
};
BOOST_GLOBAL_FIXTURE(PreflightSkipFixture);

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

struct AwsSdkFixture {
    Aws::SDKOptions opts;
    AwsSdkFixture() { Aws::InitAPI(opts); }
    ~AwsSdkFixture() { Aws::ShutdownAPI(opts); }
};
BOOST_GLOBAL_FIXTURE(AwsSdkFixture);

auto make_config() -> aws_acm_pca_provider_config {
    aws_acm_pca_provider_config cfg;
    cfg.certificate_authority_arn = ca_arn();
    cfg.aws.region = region_from_arn(ca_arn());
    cfg.aws.api_timeout = std::chrono::seconds(60);
    return cfg;
}

/// The one certificate this run issues, shared by every case that needs it.
auto issued() -> const pem_material& {
    static const pem_material material = [] {
        aws_acm_pca_provider provider{make_config()};
        auto request = acm_pca::make_leaf_request("kythira-acm-pca-real-test");
        return std::move(provider.sign_csr(request.csr.csr_pem, request.options)).get();
    }();
    return material;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(acm_pca_real)

BOOST_AUTO_TEST_CASE(root_certificate_pem_is_cached) {
    aws_acm_pca_provider provider{make_config()};
    auto root = std::move(provider.root_certificate_pem()).get();
    BOOST_TEST(root.find("BEGIN CERTIFICATE") != std::string::npos);
    BOOST_TEST(std::move(provider.root_certificate_pem()).get() == root);
}

// Exercises IssueCertificate and the asynchronous GetCertificate poll,
// including its REQUEST_IN_PROGRESS backoff, which ACM Private CA returns for
// the first poll after almost every issuance.
BOOST_AUTO_TEST_CASE(sign_csr_issues_certificate_chaining_to_root) {
    aws_acm_pca_provider provider{make_config()};
    const auto& material = issued();
    BOOST_TEST(material.certificate_pem.find("BEGIN CERTIFICATE") != std::string::npos);
    BOOST_TEST(material.chain_pem.starts_with(material.certificate_pem));

    auto root = std::move(provider.root_certificate_pem()).get();
    BOOST_TEST(acm_pca::chain_verifies(material.chain_pem, root));
}

// Requirement 10.7: revocation works when the CA has CRL/OCSP configured, and
// surfaces the AWS error, not a local fallback, when it has neither.
BOOST_AUTO_TEST_CASE(revoke_matches_revocation_configuration) {
    aws_acm_pca_provider provider{make_config()};
    const bool configured = std::move(provider.revocation_configured()).get();
    BOOST_TEST_MESSAGE("CA revocation configured: " << std::boolalpha << configured);

    auto serial = acm_pca::hex_serial(issued().certificate_pem);
    auto revoked = provider.revoke(serial, "SUPERSEDED");
    if (configured) {
        BOOST_CHECK_NO_THROW(std::move(revoked).get());
    } else {
        BOOST_CHECK_THROW(std::move(revoked).get(), std::runtime_error);
    }
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !KYTHIRA_HAS_AWS_ACM_PCA

BOOST_AUTO_TEST_CASE(skipped_no_acm_pca_sdk_component) {
    BOOST_TEST_MESSAGE("KYTHIRA_HAS_AWS_ACM_PCA not defined — skipping");
}

#endif  // KYTHIRA_HAS_AWS_ACM_PCA
