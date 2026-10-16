// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE gcp_privateca_provider_unit_test
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_HAS_GCP_PRIVATECA

#include <raft/gcp_privateca_certificate_provider.hpp>
#include <raft/gcp_privateca_certificate_provider_impl.hpp>
#include <raft/certificate_provider.hpp>

#include "wide_serial_test_support.hpp"

#ifdef FIU_ENABLE
#include <fiu-control.h>
#endif

#include "ordered_global_fixtures.hpp"

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#include <memory>
#endif

using namespace raft::testing;

namespace {

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        _init = std::make_unique<folly::Init>(&argc, &argv, false);
    }
    std::unique_ptr<folly::Init> _init;
};
#else
using FollyInitFixture = kythira::testing::no_global_fixture;
#endif

#ifdef FIU_ENABLE
struct FiuInitFixture {
    FiuInitFixture() { fiu_init(0); }
};
#else
using FiuInitFixture = kythira::testing::no_global_fixture;
#endif

// One registration, constructed in this order and destroyed in reverse:
// separately registered global fixtures run in link-address order, not
// declaration order (see ordered_global_fixtures.hpp).
// folly::Init first, then fiu_init.
using SuiteGlobalFixtures =
    kythira::testing::ordered_global_fixtures<FollyInitFixture, FiuInitFixture>;
BOOST_GLOBAL_FIXTURE(SuiteGlobalFixtures);

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

// .kiro/specs/cloud-composite-node-ids/ Requirement 12.5: a CAS certificate's
// serial, at full width, from the certificate itself or else from CAS's own
// hex_serial_number.
BOOST_AUTO_TEST_SUITE(gcp_privateca_serial)

BOOST_AUTO_TEST_CASE(serial_is_read_from_the_certificate_then_the_description) {
    certificate_authority ca;
    leaf_certificate_options leaf;
    leaf.subject.common_name = "cas-leaf";
    leaf.dns_names = {"cas-leaf.example.com"};
    auto csr = generate_key_and_csr(leaf);
    csr_signing_options signing;
    signing.dns_names = leaf.dns_names;
    const auto wide = kythira::testing::twenty_octet_serial();
    auto material = kythira::testing::sign_csr_with_serial(ca, csr.csr_pem, signing, wide);

    google::cloud::security::privateca::v1::Certificate certificate;
    certificate.set_pem_certificate(material.certificate_pem);
    // The PEM wins over a disagreeing description.
    certificate.mutable_certificate_description()
        ->mutable_subject_description()
        ->set_hex_serial_number("01");
    BOOST_TEST((gcp_privateca_detail::serial_of(certificate) == wide));

    certificate.clear_pem_certificate();
    // Upper case, as CAS may spell it.
    certificate.mutable_certificate_description()
        ->mutable_subject_description()
        ->set_hex_serial_number("808182838485868788898A8B8C8D8E8F90919214");
    BOOST_TEST((gcp_privateca_detail::serial_of(certificate) == wide));

    certificate.mutable_certificate_description()
        ->mutable_subject_description()
        ->set_hex_serial_number("not hex");
    BOOST_TEST(!gcp_privateca_detail::serial_of(certificate).has_value());
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

// Revoking by serial lists the pool first; a failure there rejects the
// future rather than silently revoking nothing.
BOOST_AUTO_TEST_CASE(revoke_by_serial_list_fault_returns_exceptional_future) {
    gcp_privateca_certificate_provider provider{valid_config()};
    fiu_enable("raft/gcp/privateca/list_certificates", 1, nullptr, 0);
    auto fut = provider.revoke(kythira::testing::twenty_octet_serial());
    fiu_disable("raft/gcp/privateca/list_certificates");
    // BOOST_CHECK_THROW wraps its statement in a loop, so a std::move(fut)
    // written inside it reads to clang-tidy as a move on every iteration.
    // Moving inside a lambda keeps the single move visible as single.
    auto wait = [&fut] { std::move(fut).get(); };
    BOOST_CHECK_THROW(wait(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // FIU_ENABLE

#else  // !KYTHIRA_HAS_GCP_PRIVATECA

BOOST_AUTO_TEST_CASE(skipped_no_gcp_privateca) {
    BOOST_TEST_MESSAGE("KYTHIRA_HAS_GCP_PRIVATECA not defined — skipping");
}

#endif  // KYTHIRA_HAS_GCP_PRIVATECA
