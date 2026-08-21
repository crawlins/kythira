// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file oci_certificates_provider_mock_test.cpp
/// @brief `oci_certificates_provider` against `oci_mock_server`
///        (`.kiro/specs/oci-cloud-provider/`, Requirements 12.x, 13.5-13.7).
///
/// The claims worth testing here are the ones that are invisible in the return
/// value:
///
/// - **What goes on the wire.** `sign_csr` must send
///   `MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA` with the caller's own CSR. The
///   mock rejects anything else with a 400, so "the private key never leaves the
///   caller" is enforced by the server rather than asserted about the client.
/// - **Caching is a call count.** Two identical PEMs prove nothing about
///   `root_certificate_pem` caching; the number of times the CA-bundle route was
///   hit does.
/// - **Idempotent revocation is an absence.** The mock answers 409 on a genuine
///   double-revoke, so a provider that re-issued the call fails rather than
///   silently passing.

#define BOOST_TEST_MODULE oci_certificates_provider_mock_test
#include <boost/test/unit_test.hpp>

#include <raft/oci_certificates_provider.hpp>

#include "oci_mock_server.hpp"
#include "test_timeout_scale.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

#ifdef FIU_ENABLE
#include <fiu-control.h>
#endif

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <chrono>
#include <cstdint>
#include <string>

namespace {

using raft::testing::csr_signing_options;
using raft::testing::oci_certificates_provider;
using raft::testing::oci_certificates_provider_config;

constexpr std::uint16_t test_port = 18323;

/// A CSR-shaped string. This tier never parses it — `CreateCertificate` forwards
/// the bytes and OCI's CA is what would reject a malformed one — so a real CSR
/// would buy nothing here but an RSA keygen per case.
constexpr const char* fake_csr_pem =
    "-----BEGIN CERTIFICATE REQUEST-----\nmock-csr-bytes\n-----END CERTIFICATE REQUEST-----\n";

auto generate_rsa_key_pem() -> std::string {
    EVP_PKEY* raw = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
    EVP_PKEY_keygen(ctx, &raw);
    EVP_PKEY_CTX_free(ctx);
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, raw, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);  // NOLINT(google-runtime-int) — OpenSSL's type
    std::string pem(data, static_cast<std::size_t>(len));
    BIO_free_all(bio);
    EVP_PKEY_free(raw);
    return pem;
}

auto shared_key_pem() -> const std::string& {
    static const std::string pem = generate_rsa_key_pem();
    return pem;
}

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

struct MockFixture {
    MockFixture() : server(test_port) {
        // Before start(): from here on the server verifies every signature
        // against the key the client uses, so a request whose sent headers
        // disagree with its signed ones fails here rather than at Oracle.
        server.set_signing_key_pem(shared_key_pem());
        server.start();
    }
    ~MockFixture() { server.stop(); }

    MockFixture(const MockFixture&) = delete;
    auto operator=(const MockFixture&) -> MockFixture& = delete;
    MockFixture(MockFixture&&) = delete;
    auto operator=(MockFixture&&) -> MockFixture& = delete;

    [[nodiscard]] auto config() const -> oci_certificates_provider_config {
        oci_certificates_provider_config cfg;
        cfg.oci.region = "us-phoenix-1";
        cfg.oci.tenancy_id = "ocid1.tenancy.oc1..aaaa";
        cfg.oci.user_id = "ocid1.user.oc1..bbbb";
        cfg.oci.fingerprint = "aa:bb:cc:dd";
        cfg.oci.private_key_pem = shared_key_pem();
        cfg.oci.endpoint_override = server.origin();
        cfg.oci.api_timeout = std::chrono::seconds{5};
        cfg.compartment_id = "ocid1.compartment.oc1..mock";
        cfg.certificate_authority_id = server.certificate_authority_id();
        cfg.poll_interval = std::chrono::milliseconds{20};
        return cfg;
    }

    kythira::testing::oci_mock_server server;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(oci_certificates_provider_mock_tests)

/// Requirement 12.4's required fields, refused before any network call.
BOOST_AUTO_TEST_CASE(construction_requires_a_compartment_and_a_certificate_authority) {
    MockFixture fixture;
    {
        auto cfg = fixture.config();
        cfg.compartment_id.clear();
        BOOST_CHECK_THROW(oci_certificates_provider{cfg}, std::invalid_argument);
    }
    {
        auto cfg = fixture.config();
        cfg.certificate_authority_id.clear();
        BOOST_CHECK_THROW(oci_certificates_provider{cfg}, std::invalid_argument);
    }
}

/// Requirement 13.7's `oci_certificates_issue_and_cache_root`.
///
/// The caching claim is asserted as a route hit count. Comparing the two
/// returned strings would pass identically against a provider that fetched
/// twice, which is the behaviour the requirement is actually about.
BOOST_AUTO_TEST_CASE(oci_certificates_issue_and_cache_root,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    oci_certificates_provider provider{fixture.config()};

    const auto first = std::move(provider.root_certificate_pem()).get();
    const auto second = std::move(provider.root_certificate_pem()).get();

    BOOST_CHECK_EQUAL(first, fixture.server.root_pem());
    BOOST_CHECK_EQUAL(second, first);
    BOOST_CHECK_EQUAL(fixture.server.hits("GET /20210224/certificateAuthorityBundles/{id}"), 1U);
}

/// Requirement 12.2: the CSR goes out unchanged under the externally-issued
/// config type, and the returned material carries no private key.
///
/// The mock 400s on any other `configType` and on a missing `csrPem`, so the
/// call succeeding *is* the assertion that the right request shape went out;
/// the checks below then confirm the response was unpacked from the right
/// fields.
BOOST_AUTO_TEST_CASE(sign_csr_forwards_the_callers_csr_and_returns_no_private_key,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    oci_certificates_provider provider{fixture.config()};

    csr_signing_options options;
    options.dns_names = {"node-1.cluster.local"};
    options.validity = std::chrono::hours(24 * 7);

    const auto material = std::move(provider.sign_csr(fake_csr_pem, options)).get();

    BOOST_CHECK(material.private_key_pem.empty());
    BOOST_CHECK(material.certificate_pem.find("mock-leaf-") != std::string::npos);
    BOOST_CHECK_EQUAL(material.chain_pem, fixture.server.root_pem());
    BOOST_CHECK_EQUAL(material.serial, 100001U);

    const auto certificates = fixture.server.certificates();
    BOOST_REQUIRE_EQUAL(certificates.size(), 1U);
    BOOST_CHECK_EQUAL(certificates.front().csr_pem, fake_csr_pem);
    BOOST_CHECK_EQUAL(certificates.front().issuer_certificate_authority_id,
                      fixture.server.certificate_authority_id());
    // The resource name is derived from the requested identity so an operator
    // reading the OCI console can tell the certificates apart.
    BOOST_CHECK_MESSAGE(certificates.front().name.starts_with("node-1-cluster-local-"),
                        "unexpected certificate name: " << certificates.front().name);
}

/// Requirement 12.2's poll: issuance is not instantaneous on real OCI, and the
/// provider must wait for `ACTIVE` rather than reading a bundle that is not
/// there yet.
///
/// Two `CREATING` answers before `ACTIVE`, so a provider that read
/// `currentVersion` from the first response would fail here — the mock includes
/// `currentVersion` in the `CREATING` responses too, precisely so that reading
/// it early is a *silent* mistake rather than a null dereference.
///
/// The count is exact rather than a lower bound: three is one call per
/// `CREATING` answer plus the `ACTIVE` one. Fewer means the state was ignored;
/// more means the loop kept polling after it had its answer.
BOOST_AUTO_TEST_CASE(sign_csr_waits_for_the_certificate_to_become_active,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    fixture.server.set_certificate_creating_polls(2);
    oci_certificates_provider provider{fixture.config()};

    const auto material = std::move(provider.sign_csr(fake_csr_pem, csr_signing_options{})).get();
    BOOST_CHECK(!material.certificate_pem.empty());
    BOOST_CHECK_EQUAL(fixture.server.hits("GET /20210224/certificates/{id}"), 3U);
}

/// Requirement 12.2's bound on that poll. `api_timeout` is the same knob that
/// bounds a single HTTP call, deliberately — see the header. Reaching it must
/// reject rather than hang, which is the difference between a slow bootstrap and
/// a wedged one.
BOOST_AUTO_TEST_CASE(sign_csr_gives_up_when_the_certificate_never_becomes_active,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    fixture.server.set_certificate_creating_polls(10000);
    auto cfg = fixture.config();
    cfg.oci.api_timeout = std::chrono::seconds{1};
    oci_certificates_provider provider{cfg};

    auto fut = provider.sign_csr(fake_csr_pem, csr_signing_options{});
    BOOST_CHECK_THROW(std::move(fut).get(), std::runtime_error);
}

/// Requirement 13.7's `oci_certificates_revoke_idempotent`, plus Requirement
/// 12.6's operation choice.
///
/// The serial is resolved back to a `(certificate OCID, version number)` pair by
/// searching the CA's certificates — a fresh provider instance does the second
/// revoke to prove that search, not a cache populated by `sign_csr`, is what
/// finds it. That distinction is the whole reason the search exists: a
/// revocation is most often needed after the process that issued the
/// certificate is gone.
BOOST_AUTO_TEST_CASE(oci_certificates_revoke_idempotent,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    oci_certificates_provider provider{fixture.config()};

    const auto material = std::move(provider.sign_csr(fake_csr_pem, csr_signing_options{})).get();
    const auto serial = std::to_string(material.serial);

    BOOST_CHECK_NO_THROW(std::move(provider.revoke(serial)).get());
    BOOST_CHECK_EQUAL(
        fixture.server.hits("POST /20210224/certificates/{id}/versions/{n}/actions/revoke"), 1U);
    {
        const auto certificates = fixture.server.certificates();
        BOOST_REQUIRE_EQUAL(certificates.size(), 1U);
        BOOST_REQUIRE_EQUAL(certificates.front().versions.size(), 1U);
        BOOST_CHECK(certificates.front().versions.front().revoked);
    }

    // A second provider, with no memory of having issued anything.
    oci_certificates_provider reopened{fixture.config()};
    BOOST_CHECK_NO_THROW(std::move(reopened.revoke(serial)).get());
    // Still one: the already-revoked version is recognised and not re-revoked.
    // The mock would answer 409 if it were, so this count is what makes the
    // idempotency claim non-vacuous.
    BOOST_CHECK_EQUAL(
        fixture.server.hits("POST /20210224/certificates/{id}/versions/{n}/actions/revoke"), 1U);

    // A serial the CA never issued resolves successfully and touches nothing.
    BOOST_CHECK_NO_THROW(std::move(reopened.revoke("999999")).get());
    BOOST_CHECK_EQUAL(
        fixture.server.hits("POST /20210224/certificates/{id}/versions/{n}/actions/revoke"), 1U);
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef FIU_ENABLE

BOOST_AUTO_TEST_SUITE(oci_certificates_provider_fault_injection)

/// Requirement 12.7's three points, each asserted to reject the Future *and* —
/// where it is observable — to fire before the call it guards.
BOOST_AUTO_TEST_CASE(the_create_certificate_fault_rejects_the_future,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    oci_certificates_provider provider{fixture.config()};

    fiu_enable("raft/oci/certificates/create_certificate", 1, nullptr, 0);
    auto fut = provider.sign_csr(fake_csr_pem, csr_signing_options{});
    fiu_disable("raft/oci/certificates/create_certificate");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
    BOOST_CHECK_EQUAL(fixture.server.hits("POST /20210224/certificates"), 0U);
}

BOOST_AUTO_TEST_CASE(the_get_bundle_fault_rejects_the_future,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    oci_certificates_provider provider{fixture.config()};

    fiu_enable("raft/oci/certificates/get_bundle", 1, nullptr, 0);
    auto fut = provider.root_certificate_pem();
    fiu_disable("raft/oci/certificates/get_bundle");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
    BOOST_CHECK_EQUAL(fixture.server.hits("GET /20210224/certificateAuthorityBundles/{id}"), 0U);

    // The failure must not have poisoned the cache — the next call succeeds.
    BOOST_CHECK_NO_THROW(std::move(provider.root_certificate_pem()).get());
}

BOOST_AUTO_TEST_CASE(the_revoke_fault_rejects_the_future,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    oci_certificates_provider provider{fixture.config()};

    fiu_enable("raft/oci/certificates/revoke", 1, nullptr, 0);
    auto fut = provider.revoke("100001");
    fiu_disable("raft/oci/certificates/revoke");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // FIU_ENABLE
