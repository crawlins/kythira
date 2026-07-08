#define BOOST_TEST_MODULE aws_acm_pca_provider_unit_test
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_HAS_AWS_ACM_PCA

#include <raft/aws_acm_pca_provider.hpp>
#include <raft/aws_acm_pca_provider_impl.hpp>

#include <aws/core/Aws.h>

#ifdef FIU_ENABLE
#include <fiu-control.h>
#endif

#include <folly/init/Init.h>

using namespace raft::testing;

namespace {

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        folly::init(&argc, &argv, false);
    }
};

struct AwsSdkFixture {
    AwsSdkFixture() {
        Aws::SDKOptions opts;
        Aws::InitAPI(opts);
#ifdef FIU_ENABLE
        fiu_init(0);
#endif
    }
    ~AwsSdkFixture() {
        Aws::SDKOptions opts;
        Aws::ShutdownAPI(opts);
    }
};

}  // namespace

BOOST_GLOBAL_FIXTURE(FollyInitFixture);
BOOST_GLOBAL_FIXTURE(AwsSdkFixture);

BOOST_AUTO_TEST_SUITE(acm_pca_construction)

BOOST_AUTO_TEST_CASE(valid_config_constructs) {
    aws_acm_pca_provider_config cfg;
    cfg.certificate_authority_arn =
        "arn:aws:acm-pca:us-east-1:123456789012:certificate-authority/test";
    cfg.aws.region = "us-east-1";
    BOOST_CHECK_NO_THROW((aws_acm_pca_provider{cfg}));
}

BOOST_AUTO_TEST_CASE(empty_arn_throws) {
    aws_acm_pca_provider_config cfg;
    cfg.aws.region = "us-east-1";
    BOOST_CHECK_THROW((aws_acm_pca_provider{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(satisfies_certificate_provider_concept) {
    static_assert(certificate_provider<aws_acm_pca_provider>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef FIU_ENABLE

BOOST_AUTO_TEST_SUITE(acm_pca_fault_injection)

namespace {
auto make_provider() -> aws_acm_pca_provider_config {
    aws_acm_pca_provider_config cfg;
    cfg.certificate_authority_arn =
        "arn:aws:acm-pca:us-east-1:123456789012:certificate-authority/test";
    cfg.aws.region = "us-east-1";
    cfg.aws.api_timeout = std::chrono::seconds(2);
    return cfg;
}
}  // namespace

BOOST_AUTO_TEST_CASE(get_certificate_authority_certificate_fault_returns_exceptional_future) {
    aws_acm_pca_provider provider{make_provider()};

    fiu_enable("raft/aws/acm_pca/get_certificate_authority_certificate", 1, nullptr, 0);
    auto fut = provider.root_certificate_pem();
    fiu_disable("raft/aws/acm_pca/get_certificate_authority_certificate");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(issue_certificate_fault_returns_exceptional_future) {
    aws_acm_pca_provider provider{make_provider()};

    csr_signing_options opts;
    opts.dns_names = {"example.com"};

    fiu_enable("raft/aws/acm_pca/issue_certificate", 1, nullptr, 0);
    auto fut = provider.sign_csr("not a real csr", opts);
    fiu_disable("raft/aws/acm_pca/issue_certificate");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(revoke_certificate_fault_returns_exceptional_future) {
    aws_acm_pca_provider provider{make_provider()};

    fiu_enable("raft/aws/acm_pca/revoke_certificate", 1, nullptr, 0);
    auto fut = provider.revoke("01");
    fiu_disable("raft/aws/acm_pca/revoke_certificate");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // FIU_ENABLE

#else  // !KYTHIRA_HAS_AWS_ACM_PCA

BOOST_AUTO_TEST_CASE(skipped_no_acm_pca_sdk_component) {
    BOOST_TEST_MESSAGE("KYTHIRA_HAS_AWS_ACM_PCA not defined — skipping");
}

#endif
