/// @file oci_certificates_provider_real_test.cpp
/// @brief `oci_certificates_provider` against a **real OCI Certificate
///        Authority** (`.kiro/specs/oci-cloud-provider/`, Requirements
///        13.8-13.11).
///
/// Opt-in and never registered with CTest, like its quorum-manager sibling.
/// Unlike that one it launches no compute — the only billable thing in reach is
/// the CA's Vault key, which is an operator-provisioned prerequisite rather than
/// anything this suite creates.
///
/// **What this tier is for here specifically.** `oci_certificates_provider`
/// could not have reached OCI at all until recently: the endpoint domain was
/// wrong for both certificate services, and
/// `certificatesmanagement.{region}.oraclecloud.com` has no DNS record
/// (`spike-notes.md` Finding 10). The mock tier cannot see that, because
/// `endpoint_override` replaces the whole host. This suite is the only place
/// the *management* and *retrieval* planes are exercised as two distinct hosts.
///
/// Requirement 12.3 is load-bearing for what is NOT here: this provider never
/// creates or deletes a Certificate Authority, so neither does this suite. The
/// CA arrives as an env-supplied OCID (Requirement 13.11) — and getting one to
/// `ACTIVE` needs a Dynamic Group grant on its Vault key, which is documented
/// in `scripts/ci-cloud-credentials/oci/README.md` because it took five
/// attempts to discover.

#define BOOST_TEST_MODULE oci_certificates_provider_real_test
#include <boost/test/unit_test.hpp>

#include <raft/oci_certificates_provider.hpp>

#include "oci_real_test_support.hpp"
#include "test_timeout_scale.hpp"

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

using kythira::testing::oci_real::real_test_config;
using raft::testing::csr_signing_options;
using raft::testing::oci_certificates_provider;
using raft::testing::oci_certificates_provider_config;

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

constexpr int kSkipExitCode = 77;

/// A CSR generated locally, so the private key demonstrably never leaves this
/// process — which is the whole point of the
/// `MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA` config type this provider uses.
[[nodiscard]] auto make_csr(const std::string& common_name) -> std::string {
    raft::testing::leaf_certificate_options options;
    options.subject.common_name = common_name;
    options.dns_names = {common_name};
    // **RSA, not this project's ECDSA default.** OCI requires the CSR's key
    // algorithm to be in the same family as the issuing CA's, and a mismatch
    // is not rejected at request time — `CreateCertificate` succeeds and the
    // certificate lands in `lifecycleState: FAILED` minutes later with
    // "The key algorithm is in a different algorithm family from the issuing
    // certificate authority's algorithm family."
    //
    // The CA in `scripts/ci-cloud-credentials/oci/README.md` is created with an
    // RSA master key, because that is what OCI accepts for a CA. So a caller
    // whose nodes use this project's default ECDSA keys cannot use that CA —
    // a real constraint on deployment, not a test detail.
    options.algorithm = raft::testing::key_algorithm::rsa_2048;
    const auto material = raft::testing::generate_key_and_csr(options);
    return material.csr_pem;
}

struct RealCertificatesFixture {
    real_test_config cfg{real_test_config::from_environment()};
    std::optional<oci_certificates_provider> provider;

    RealCertificatesFixture() {
        if (!cfg.has_credentials() || cfg.compartment_id.empty() ||
            cfg.certificate_authority_id.empty()) {
            std::cerr << "[oci-real] SKIP: set the OCI credential env vars plus "
                         "KYTHIRA_OCI_COMPARTMENT_ID and "
                         "KYTHIRA_OCI_CERTIFICATE_AUTHORITY_ID. See "
                         "scripts/ci-cloud-credentials/oci/README.md.\n";
            std::exit(kSkipExitCode);
        }

        oci_certificates_provider_config provider_cfg;
        provider_cfg.oci = cfg.client_config();
        // Issuance is not instant, and this timeout bounds the ACTIVE poll as
        // well as each HTTP call.
        provider_cfg.oci.api_timeout = std::chrono::seconds{300};
        provider_cfg.compartment_id = cfg.compartment_id;
        provider_cfg.certificate_authority_id = cfg.certificate_authority_id;
        provider_cfg.poll_interval = std::chrono::milliseconds{5000};

        try {
            provider.emplace(provider_cfg);
        } catch (const std::exception& e) {
            std::cerr << "[oci-real] SKIP: could not construct the provider: " << e.what() << "\n";
            std::exit(kSkipExitCode);
        }
    }
};

}  // namespace

BOOST_FIXTURE_TEST_SUITE(oci_real_certificates, RealCertificatesFixture)

/// Requirement 12.5, and the retrieval-plane host.
///
/// `GetCertificateAuthorityBundle` is served from
/// `certificates.{region}.oci.oraclecloud.com` — a *different* host from the
/// management plane, and one that does not resolve at all without the `oci`
/// label. That distinction is invisible to the mock tier, so this is the only
/// place it is checked.
///
/// The caching claim is asserted as identity of the returned value plus a
/// second call completing promptly; the mock tier asserts it as a call count,
/// which is the stronger form but needs a server that counts.
BOOST_AUTO_TEST_CASE(root_certificate_is_fetched_and_cached,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(600))) {
    const auto first = std::move(provider->root_certificate_pem()).get();
    BOOST_REQUIRE_MESSAGE(!first.empty(), "the CA bundle came back empty");
    BOOST_CHECK_MESSAGE(first.find("-----BEGIN CERTIFICATE-----") != std::string::npos,
                        "the CA bundle is not PEM: " << first.substr(0, 80));

    const auto started = std::chrono::steady_clock::now();
    const auto second = std::move(provider->root_certificate_pem()).get();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    BOOST_CHECK_EQUAL(second, first);
    // A cached read is local. A generous bound, since what would fail it is a
    // second round trip to OCI, not a slow memcpy.
    BOOST_CHECK_MESSAGE(
        elapsed < std::chrono::seconds{2},
        "the second read took "
            << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
            << "ms, which looks like a second fetch rather than a cache hit");
}

/// Requirement 12.2: the caller's own CSR is submitted, and no private key
/// comes back.
///
/// This is the invariant `certificate_provider.hpp` exists to protect, and the
/// one OCI's *default* issuance flow (`ISSUED_BY_INTERNAL_CA`) violates by
/// generating the key itself into Vault. Confirming it against a real CA is
/// what distinguishes "we sent the right `configType`" from "OCI honoured it".
BOOST_AUTO_TEST_CASE(sign_csr_issues_from_the_callers_csr_and_returns_no_private_key,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(900))) {
    csr_signing_options options;
    options.dns_names = {"node-real-test.kythira.invalid"};
    options.validity = std::chrono::hours(24 * 7);

    const auto csr = make_csr("node-real-test.kythira.invalid");
    const auto material = std::move(provider->sign_csr(csr, options)).get();

    BOOST_CHECK_MESSAGE(material.private_key_pem.empty(),
                        "OCI returned a private key — the caller-supplied-CSR invariant is "
                        "broken, which would mean the wrong configType reached the service");
    BOOST_CHECK_MESSAGE(
        material.certificate_pem.find("-----BEGIN CERTIFICATE-----") != std::string::npos,
        "the issued certificate is not PEM: " << material.certificate_pem.substr(0, 80));
    BOOST_CHECK_MESSAGE(!material.chain_pem.empty(), "no chain was returned");
}

/// Requirement 12.6's idempotency, against a real CA.
///
/// A serial the CA never issued must resolve successfully rather than throwing
/// — the search simply finds nothing. Cheap: it issues no certificate.
BOOST_AUTO_TEST_CASE(revoking_an_unknown_serial_is_idempotent,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(600))) {
    BOOST_CHECK_NO_THROW(std::move(provider->revoke("999999999999")).get());
}

BOOST_AUTO_TEST_SUITE_END()
