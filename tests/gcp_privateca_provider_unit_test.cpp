// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE gcp_privateca_provider_unit_test
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_HAS_GCP_PRIVATECA

#include <raft/gcp_privateca_certificate_provider.hpp>
#include <raft/gcp_privateca_certificate_provider_impl.hpp>

#ifdef FIU_ENABLE
#include <fiu-control.h>
#endif

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

using namespace raft::testing;

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

#ifdef FIU_ENABLE
struct FiuInitFixture {
    FiuInitFixture() { fiu_init(0); }
};
BOOST_GLOBAL_FIXTURE(FiuInitFixture);
#endif

auto valid_config() -> gcp_privateca_certificate_provider_config {
    gcp_privateca_certificate_provider_config cfg;
    cfg.gcp.project_id = "test-project";
    cfg.location = "us-central1";
    cfg.ca_pool_id = "test-pool";
    return cfg;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(gcp_privateca_construction)

BOOST_AUTO_TEST_CASE(valid_config_constructs) {
    BOOST_CHECK_NO_THROW((gcp_privateca_certificate_provider{valid_config()}));
}

BOOST_AUTO_TEST_CASE(empty_project_throws) {
    auto cfg = valid_config();
    cfg.gcp.project_id.clear();
    BOOST_CHECK_THROW((gcp_privateca_certificate_provider{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_location_throws) {
    auto cfg = valid_config();
    cfg.location.clear();
    BOOST_CHECK_THROW((gcp_privateca_certificate_provider{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_ca_pool_throws) {
    auto cfg = valid_config();
    cfg.ca_pool_id.clear();
    BOOST_CHECK_THROW((gcp_privateca_certificate_provider{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(satisfies_certificate_provider_concept) {
    static_assert(certificate_provider<gcp_privateca_certificate_provider>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef FIU_ENABLE

BOOST_AUTO_TEST_SUITE(gcp_privateca_fault_injection)

BOOST_AUTO_TEST_CASE(get_certificate_authority_fault_returns_exceptional_future) {
    gcp_privateca_certificate_provider provider{valid_config()};
    fiu_enable("raft/gcp/privateca/get_certificate_authority", 1, nullptr, 0);
    auto fut = provider.root_certificate_pem();
    fiu_disable("raft/gcp/privateca/get_certificate_authority");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(create_certificate_fault_returns_exceptional_future) {
    gcp_privateca_certificate_provider provider{valid_config()};
    csr_signing_options opts;
    opts.dns_names = {"example.com"};
    fiu_enable("raft/gcp/privateca/create_certificate", 1, nullptr, 0);
    auto fut = provider.sign_csr("not a real csr", opts);
    fiu_disable("raft/gcp/privateca/create_certificate");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(revoke_certificate_fault_returns_exceptional_future) {
    gcp_privateca_certificate_provider provider{valid_config()};
    fiu_enable("raft/gcp/privateca/revoke_certificate", 1, nullptr, 0);
    auto fut = provider.revoke("projects/p/locations/l/caPools/pool/certificates/c");
    fiu_disable("raft/gcp/privateca/revoke_certificate");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // FIU_ENABLE

#else  // !KYTHIRA_HAS_GCP_PRIVATECA

BOOST_AUTO_TEST_CASE(skipped_no_gcp_privateca) {
    BOOST_TEST_MESSAGE("KYTHIRA_HAS_GCP_PRIVATECA not defined — skipping");
}

#endif  // KYTHIRA_HAS_GCP_PRIVATECA
