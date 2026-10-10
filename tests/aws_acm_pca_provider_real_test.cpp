// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE aws_acm_pca_provider_real_test
#include <boost/test/unit_test.hpp>

// Real-AWS integration test for aws_acm_pca_provider (certificate-authority
// task 11, .kiro/specs/acm-pca-ephemeral-ca/). Opt-in, because it creates
// billable CAs: it runs only with KYTHIRA_ACM_PCA_REAL_TESTS=1 or
// $KYTHIRA_TEST_ACM_PCA_ARN set, and otherwise exits 77 (ctest "Not Run")
// before the SDK initializes. Labelled `slow`, so the default CI run
// excludes it.
//
// The suite creates the two root CAs it needs, one with OCSP and one with no
// revocation configuration, and deletes them at the end of the run, on a
// trappable signal (a ctest TIMEOUT included), and on a provisioning failure,
// which then skips the suite. Both CAs use short-lived mode. A CA named by
// $KYTHIRA_TEST_ACM_PCA_ARN stands in for whichever kind it is and is never
// modified or deleted; only the other kind is created. Every created CA is
// tagged kythira:suite=acm-pca-real-test, which is what
// scripts/aws-acm-pca-leaks.sh and the CI role's acm-pca bundle key on.
//
// The fixture, not the provider, creates the CAs: certificate-authority
// Requirement 10.2 keeps CA provisioning out of aws_acm_pca_provider.
//
// Region: the supplied ARN's, else $AWS_REGION, else us-west-2. Credentials
// come from the default AWS chain. Cost per run is printed as an [aws-cost]
// block; see doc/aws_acm_pca_test_cost_estimate.md.

#ifdef KYTHIRA_HAS_AWS_ACM_PCA

#include <raft/aws_acm_pca_provider.hpp>
#include <raft/aws_acm_pca_provider_impl.hpp>

#include "aws_acm_pca_test_support.hpp"
#include "aws_real_test_support.hpp"

#include <aws/core/Aws.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace raft::testing;
namespace acm_pca = kythira::testing::acm_pca;
using kythira::testing::aws_real_ec2::BilledResource;
using kythira::testing::aws_real_ec2::CostSummaryFixture;
using kythira::testing::aws_real_ec2::g_active_aws_fixture;
using kythira::testing::aws_real_ec2::g_cost_accumulator;
using kythira::testing::aws_real_ec2::install_aws_signal_handlers;
using kythira::testing::aws_real_ec2::signal_cleanup_target;
using kythira::testing::aws_real_ec2::TestCostReport;

namespace {

constexpr const char* SUITE_TAG_KEY = "kythira:suite";
constexpr const char* SUITE_TAG_VALUE = "acm-pca-real-test";

// us-east-1 list prices (Requirement 4.2): a short-lived-mode CA is $50 a
// month, billed per hour; every certificate, the self-issued roots included,
// is $0.058.
constexpr double SHORT_LIVED_CA_HOURLY = 50.0 / 730.0;
constexpr double CERTIFICATE_USD = 0.058;

auto env(const char* name) -> std::string {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string{};
}

auto supplied_arn() -> std::string {
    return env("KYTHIRA_TEST_ACM_PCA_ARN");
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

auto region() -> std::string {
    if (!supplied_arn().empty()) {
        return region_from_arn(supplied_arn());
    }
    auto r = env("AWS_REGION");
    return r.empty() ? std::string("us-west-2") : r;
}

/// `run-<pid>-<epoch seconds>`, tagged on every CA this process creates.
auto run_id() -> const std::string& {
    static const std::string id = "run-" + std::to_string(::getpid()) + "-" +
                                  std::to_string(static_cast<long long>(std::time(nullptr)));
    return id;
}

// Registered first so a run that is not enabled exits before the SDK
// initializes (Requirement 6.1).
struct PreflightSkipFixture {
    PreflightSkipFixture() {
        if (env("KYTHIRA_ACM_PCA_REAL_TESTS") != "1" && supplied_arn().empty()) {
            std::cerr << "SKIP: neither KYTHIRA_ACM_PCA_REAL_TESTS=1 nor "
                         "KYTHIRA_TEST_ACM_PCA_ARN is set; real ACM Private CA tests, which "
                         "create billable CAs, not run\n";
            std::exit(77);
        }
        if (!supplied_arn().empty() && region_from_arn(supplied_arn()).empty()) {
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
        _init = std::make_unique<folly::Init>(&argc, &argv, false);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

// Global fixtures are torn down in reverse, so the cost summary prints after
// the SDK shuts down, which is after the CAs are deleted and costed below.
BOOST_GLOBAL_FIXTURE(CostSummaryFixture);

struct AwsSdkFixture {
    Aws::SDKOptions opts;
    AwsSdkFixture() { Aws::InitAPI(opts); }
    ~AwsSdkFixture() { Aws::ShutdownAPI(opts); }
    AwsSdkFixture(const AwsSdkFixture&) = delete;
    auto operator=(const AwsSdkFixture&) -> AwsSdkFixture& = delete;
};
BOOST_GLOBAL_FIXTURE(AwsSdkFixture);

auto make_config(const std::string& arn) -> aws_acm_pca_provider_config {
    aws_acm_pca_provider_config cfg;
    cfg.certificate_authority_arn = arn;
    cfg.aws.region = region();
    cfg.aws.api_timeout = std::chrono::seconds(60);
    return cfg;
}

/// The revocable (OCSP) and bare (no revocation) CAs every case runs against,
/// each either created here or supplied. Registered with the signal handler
/// the moment the first CA exists (Requirement 2.4, 2.5).
struct RealCaFixture : signal_cleanup_target {
    std::string revocable_arn;
    std::string bare_arn;
    /// Leaf certificates issued, for the cost report.
    std::atomic<int> leaves_issued{0};

    RealCaFixture() {
        // Classify the supplied CA first: whichever kind it is, only the other
        // one is created, and the supplied one is never touched again.
        bool need_revocable = true;
        bool need_bare = true;
        if (!supplied_arn().empty()) {
            bool revocable = false;
            try {
                aws_acm_pca_provider provider{make_config(supplied_arn())};
                revocable = std::move(provider.revocation_configured()).get();
            } catch (const std::exception& ex) {
                std::cerr << "SKIP: cannot describe KYTHIRA_TEST_ACM_PCA_ARN " << supplied_arn()
                          << ": " << ex.what() << "\n";
                std::exit(77);
            }
            (revocable ? revocable_arn : bare_arn) = supplied_arn();
            (revocable ? need_revocable : need_bare) = false;
            std::cerr << "[acm-pca-real] using supplied " << (revocable ? "revocable" : "bare")
                      << " CA " << supplied_arn() << "\n";
        }

        install_aws_signal_handlers();
        try {
            if (need_revocable) {
                revocable_arn = provision(_revocable, acm_pca::ca_revocation::ocsp,
                                          "kythira acm-pca real test root (ocsp)");
            }
            if (need_bare) {
                bare_arn = provision(_bare, acm_pca::ca_revocation::none,
                                     "kythira acm-pca real test root (no revocation)");
            }
        } catch (const acm_pca::ephemeral_ca_unavailable& ex) {
            // Requirement 2.6: a missing permission or quota is a skip, like
            // missing credentials, but never a leak.
            std::cerr << "SKIP: could not provision an ACTIVE root CA: " << ex.what() << "\n";
            teardown();
            std::exit(77);
        }
    }

    ~RealCaFixture() { teardown(); }

    RealCaFixture(const RealCaFixture&) = delete;
    auto operator=(const RealCaFixture&) -> RealCaFixture& = delete;
    RealCaFixture(RealCaFixture&&) = delete;
    auto operator=(RealCaFixture&&) -> RealCaFixture& = delete;

    /// Deletes the owned CAs, bare first, then files the cost report. Runs
    /// once; never throws. The supplied CA, if any, is never touched.
    void teardown() noexcept override {
        if (_torn_down.test_and_set(std::memory_order_acq_rel)) {
            return;
        }
        // On the normal path, stop the signal handler reaching a fixture that
        // is mid-teardown or already gone.
        g_active_aws_fixture.store(nullptr, std::memory_order_release);
        try {
            TestCostReport report;
            report.test_name = "aws_acm_pca_provider_real_test";
            for (auto* ca : {_bare.get(), _revocable.get()}) {
                if (ca == nullptr || !ca->created()) {
                    continue;
                }
                ca->teardown();
                BilledResource billed;
                billed.label = "short-lived CA " + ca->arn().substr(ca->arn().rfind('/') + 1);
                billed.hourly_rate = SHORT_LIVED_CA_HOURLY;
                billed.start = ca->created_at();
                billed.stop = ca->deleted_at();
                billed.finalize();
                report.resources.push_back(billed);
                if (ca->root_issued()) {
                    report.resources.push_back(BilledResource{
                        .label = "root certificate (self-issued)", .fixed_usd = CERTIFICATE_USD});
                }
            }
            for (int i = 0; i < leaves_issued.load(); ++i) {
                report.resources.push_back(
                    BilledResource{.label = "leaf certificate", .fixed_usd = CERTIFICATE_USD});
            }
            for (auto& r : report.resources) {
                r.finalize();
            }
            g_cost_accumulator.add(std::move(report));
        } catch (...) {
            std::cerr << "[acm-pca-real] teardown: could not file the cost report\n";
        }
    }

private:
    std::unique_ptr<acm_pca::ephemeral_acm_pca> _revocable;
    std::unique_ptr<acm_pca::ephemeral_acm_pca> _bare;
    std::atomic_flag _torn_down{};

    /// The instance is stored before `provision()` makes any AWS call, so
    /// `teardown()` can reach a CA whose bootstrap is still running.
    auto provision(std::unique_ptr<acm_pca::ephemeral_acm_pca>& slot,
                   acm_pca::ca_revocation revocation, const std::string& common_name)
        -> std::string {
        acm_pca::ephemeral_acm_pca::options opts;
        opts.client.region = region();
        opts.client.requestTimeoutMs = 60000;
        opts.client.connectTimeoutMs = 10000;
        opts.revocation = revocation;
        opts.common_name = common_name;
        opts.tags = {{SUITE_TAG_KEY, SUITE_TAG_VALUE}, {"kythira:run-id", run_id()}};
        opts.on_created = [this](const std::string& arn) {
            g_active_aws_fixture.store(this, std::memory_order_release);
            std::cerr << "[acm-pca-real] created " << arn << "\n";
        };
        slot = std::make_unique<acm_pca::ephemeral_acm_pca>(std::move(opts));
        slot->provision();
        std::cerr << "[acm-pca-real] " << slot->arn() << " is ACTIVE\n";
        return slot->arn();
    }
};

auto cas() -> RealCaFixture& {
    static RealCaFixture fixture;
    return fixture;
}

/// Creates the CAs after the SDK is up, and deletes them before it shuts
/// down: `cas()` is a function-local static, whose own destructor would run
/// only after main returns, by which point ShutdownAPI has already run. That
/// later destructor call is then a no-op.
struct CaLifetimeFixture {
    CaLifetimeFixture() { cas(); }
    ~CaLifetimeFixture() { cas().teardown(); }
    CaLifetimeFixture(const CaLifetimeFixture&) = delete;
    auto operator=(const CaLifetimeFixture&) -> CaLifetimeFixture& = delete;
};
BOOST_GLOBAL_FIXTURE(CaLifetimeFixture);

/// One leaf per CA per run, shared by every case that needs it: the bare CA
/// needs its own certificate to fail to revoke (design, "Test cases").
auto issue_leaf(const std::string& arn, const std::string& common_name) -> pem_material {
    aws_acm_pca_provider provider{make_config(arn)};
    auto request = acm_pca::make_leaf_request(common_name);
    // Counted on request, before the outcome is known: a certificate AWS
    // issued but the provider failed to fetch is still billed, and an
    // over-count beats an unreported charge.
    cas().leaves_issued.fetch_add(1);
    return std::move(provider.sign_csr(request.csr.csr_pem, request.options)).get();
}

auto revocable_leaf() -> const pem_material& {
    static const pem_material material =
        issue_leaf(cas().revocable_arn, "kythira-acm-pca-real-revocable");
    return material;
}

auto bare_leaf() -> const pem_material& {
    static const pem_material material = issue_leaf(cas().bare_arn, "kythira-acm-pca-real-bare");
    return material;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(acm_pca_real)

BOOST_AUTO_TEST_CASE(root_certificate_pem_is_cached) {
    aws_acm_pca_provider provider{make_config(cas().revocable_arn)};
    auto root = std::move(provider.root_certificate_pem()).get();
    BOOST_TEST(root.find("BEGIN CERTIFICATE") != std::string::npos);
    BOOST_TEST(std::move(provider.root_certificate_pem()).get() == root);
}

// Exercises IssueCertificate and the asynchronous GetCertificate poll,
// including its REQUEST_IN_PROGRESS backoff, which ACM Private CA returns for
// the first poll after almost every issuance.
BOOST_AUTO_TEST_CASE(sign_csr_issues_certificate_chaining_to_root) {
    aws_acm_pca_provider provider{make_config(cas().revocable_arn)};
    const auto& material = revocable_leaf();
    BOOST_TEST(material.certificate_pem.find("BEGIN CERTIFICATE") != std::string::npos);
    BOOST_TEST(material.chain_pem.starts_with(material.certificate_pem));

    auto root = std::move(provider.root_certificate_pem()).get();
    BOOST_TEST(acm_pca::chain_verifies(material.chain_pem, root));
}

BOOST_AUTO_TEST_CASE(revocation_configured_distinguishes_cas) {
    aws_acm_pca_provider revocable{make_config(cas().revocable_arn)};
    aws_acm_pca_provider bare{make_config(cas().bare_arn)};
    BOOST_TEST(std::move(revocable.revocation_configured()).get());
    BOOST_TEST(!std::move(bare.revocation_configured()).get());
}

// certificate-authority Requirement 10.7: revocation works when the CA has
// OCSP (or a CRL) configured...
BOOST_AUTO_TEST_CASE(revoke_on_revocable_ca_succeeds) {
    aws_acm_pca_provider provider{make_config(cas().revocable_arn)};
    auto serial = acm_pca::hex_serial(revocable_leaf().certificate_pem);
    BOOST_CHECK_NO_THROW(std::move(provider.revoke(serial, "KEY_COMPROMISE")).get());
}

// ...and AWS accepts it even when the CA has neither. The first real run
// (Real Cloud Tests run 38065882804, 2026-10-10, both architectures)
// showed RevokeCertificate succeeding on a CA with no CRL or OCSP: AWS
// records the revocation but has nowhere to publish it, so no relying party
// would ever see it. That is why ca_service checks revocation_configured()
// and answers 501 before it calls revoke(); this case pins the AWS behavior
// that gate exists for.
BOOST_AUTO_TEST_CASE(revoke_on_bare_ca_is_accepted_by_aws) {
    aws_acm_pca_provider provider{make_config(cas().bare_arn)};
    auto serial = acm_pca::hex_serial(bare_leaf().certificate_pem);
    BOOST_CHECK_NO_THROW(std::move(provider.revoke(serial, "KEY_COMPROMISE")).get());
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !KYTHIRA_HAS_AWS_ACM_PCA

BOOST_AUTO_TEST_CASE(skipped_no_acm_pca_sdk_component) {
    BOOST_TEST_MESSAGE("KYTHIRA_HAS_AWS_ACM_PCA not defined — skipping");
}

#endif  // KYTHIRA_HAS_AWS_ACM_PCA
