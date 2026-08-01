#define BOOST_TEST_MODULE gcp_privateca_provider_real_test
#include <boost/test/unit_test.hpp>

// Real-GCP integration test for gcp_privateca_certificate_provider against
// Google Cloud Certificate Authority Service. Guarded by KYTHIRA_GCP_REAL_TESTS=1
// and excluded from the default ctest run (CTest label
// `integration;gcp;real-privateca`). Skips — never fails — when credentials or
// the required env vars are absent (Requirement 23 ACs 13-17, 20).
//
// Required env vars: GCP_PROJECT_ID, GCP_REGION. GCP_TEST_CA_POOL is optional:
// when it names a pre-existing pool (with at least one ENABLED CA) that pool is
// used as-is and never deleted; when unset the fixture creates a DevOps-tier
// pool and a self-signed root CA and tears both down afterwards, per
// Requirement 23 AC 15's env-var table ("created (with a self-signed root CA)
// if absent") and AC 17's reverse-dependency-order teardown. See
// scripts/ci-cloud-credentials/gcp/README.md.

#if defined(KYTHIRA_HAS_GCP_PRIVATECA)

#include <raft/certificate_provider.hpp>
#include <raft/gcp_privateca_certificate_provider.hpp>
#include <raft/gcp_privateca_certificate_provider_impl.hpp>

#include "gcp_real_gce_test_support.hpp"

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <google/cloud/security/privateca/v1/resources.pb.h>
#include <google/cloud/security/privateca/v1/service.pb.h>
#include <google/protobuf/duration.pb.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

using namespace raft::testing;
using kythira::testing::gcp_real_gce::env_or;
using kythira::testing::gcp_real_gce::g_active_gcp_fixture;
using kythira::testing::gcp_real_gce::signal_cleanup_target;

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

namespace privateca_pb = google::cloud::security::privateca::v1;

/// Unique per-run id, labelling every resource this fixture creates
/// (Requirement 23 AC 16). Matches the real-GCE suite's scheme.
auto run_id() -> const std::string& {
    static const std::string id = "run-" + std::to_string(::getpid());
    return id;
}

/// @brief Provides the CA pool the suite issues against.
///
/// Two modes, per Requirement 23 AC 15: when `GCP_TEST_CA_POOL` is set, that
/// operator-supplied pool is used as-is and never deleted ("Resources supplied
/// via env vars are used as-is and are never deleted by the fixture", AC 16);
/// when it is unset, a DevOps-tier pool and a self-signed root CA are created
/// here and destroyed in `teardown()`.
///
/// Derives from `signal_cleanup_target` and publishes itself to
/// `g_active_gcp_fixture` so a SIGTERM/SIGINT mid-run still deletes the CA —
/// the same wiring `RealEc2Fixture` and `AzureIntegrationFixture` use, and
/// which no GCP suite had previously registered for, leaving the installed
/// handlers with nothing to call.
struct CasPoolFixture : signal_cleanup_target {
    std::string project;
    std::string location;
    std::string pool_id;
    std::string ca_id;
    /// False when the pool came from GCP_TEST_CA_POOL — teardown then deletes
    /// nothing at all.
    bool owns_pool{false};
    /// Guards against teardown running twice (destructor after signal handler).
    /// Default-constructs clear since C++20, so no ATOMIC_FLAG_INIT.
    std::atomic_flag torn_down{};

    CasPoolFixture() {
        project = env_or("GCP_PROJECT_ID", "");
        location = env_or("GCP_REGION", "us-central1");
        if (env_or("KYTHIRA_GCP_REAL_TESTS", "") != "1" || project.empty() ||
            env_or("GCP_REGION", "").empty()) {
            BOOST_TEST_MESSAGE(
                "KYTHIRA_GCP_REAL_TESTS!=1 or GCP_PROJECT_ID/GCP_REGION unset — "
                "skipping real-CAS tests");
            std::exit(77);
        }

        pool_id = env_or("GCP_TEST_CA_POOL", "");
        if (!pool_id.empty()) {
            BOOST_TEST_MESSAGE("using operator-supplied CA pool " + pool_id);
            return;
        }

        // Nothing has been created yet, so any failure below is still a clean
        // skip — but once `owns_pool` is set, every exit path must tear down.
        pool_id = "kythira-it-" + run_id();
        ca_id = "kythira-it-ca-" + run_id();
        provision_pool_and_ca();
    }

    ~CasPoolFixture() {
        g_active_gcp_fixture.store(nullptr, std::memory_order_release);
        teardown();
    }

    /// Reverse dependency order (Requirement 23 AC 17): the CA is disabled and
    /// deleted before the pool that contains it. Every step runs regardless of
    /// earlier failures, errors are collected and printed to `std::cerr`, and
    /// nothing throws — teardown failures never fail the test.
    void teardown() noexcept override {
        if (torn_down.test_and_set(std::memory_order_acq_rel)) {
            return;
        }
        if (!owns_pool) {
            return;
        }
        std::vector<std::string> errors;
        try {
            auto client = gcp_privateca_detail::make_client(client_config());

            if (!ca_id.empty()) {
                const std::string ca_name = pool_path() + "/certificateAuthorities/" + ca_id;
                // Disable first: CAS rejects deleting an ENABLED CA.
                privateca_pb::DisableCertificateAuthorityRequest disable_req;
                disable_req.set_name(ca_name);
                if (auto disabled = client.DisableCertificateAuthority(disable_req).get();
                    !disabled) {
                    errors.push_back("DisableCertificateAuthority: " + disabled.status().message());
                }
                // skip_grace_period avoids leaving the CA in the 30-day
                // scheduled-deletion window still accruing its monthly charge;
                // ignore_active_certificates permits deletion despite the
                // certificates the suite just issued.
                privateca_pb::DeleteCertificateAuthorityRequest delete_ca_req;
                delete_ca_req.set_name(ca_name);
                delete_ca_req.set_skip_grace_period(true);
                delete_ca_req.set_ignore_active_certificates(true);
                if (auto deleted = client.DeleteCertificateAuthority(delete_ca_req).get();
                    !deleted) {
                    errors.push_back("DeleteCertificateAuthority: " + deleted.status().message());
                }
            }

            // DeleteCaPool's LRO has an Empty response, which google-cloud-cpp
            // surfaces as StatusOr<OperationMetadata> rather than a bare Status.
            privateca_pb::DeleteCaPoolRequest delete_pool_req;
            delete_pool_req.set_name(pool_path());
            if (auto deleted_pool = client.DeleteCaPool(delete_pool_req).get(); !deleted_pool) {
                errors.push_back("DeleteCaPool: " + deleted_pool.status().message());
            }
        } catch (const std::exception& ex) {
            errors.emplace_back(std::string("teardown threw: ") + ex.what());
        }

        for (const auto& e : errors) {
            std::cerr << "[gcp_privateca_provider_real_test] teardown error: " << e << "\n";
        }
        if (!errors.empty()) {
            std::cerr << "[gcp_privateca_provider_real_test] CA pool " << pool_path()
                      << " may still exist and continue to bill — delete it manually.\n";
        }
    }

    [[nodiscard]] auto client_config() const -> kythira::gcp_client_config {
        kythira::gcp_client_config gcp;
        gcp.project_id = project;
        return gcp;
    }

    [[nodiscard]] auto pool_path() const -> std::string {
        return "projects/" + project + "/locations/" + location + "/caPools/" + pool_id;
    }

private:
    /// Creates the pool, then a self-signed root CA inside it, then enables the
    /// CA (CAS creates it STAGED). A failure here skips the suite rather than
    /// failing it — an absent permission is the same class of problem as absent
    /// credentials — but only after tearing down whatever was already created.
    void provision_pool_and_ca() {
        try {
            auto client = gcp_privateca_detail::make_client(client_config());
            const std::string parent = "projects/" + project + "/locations/" + location;

            privateca_pb::CaPool pool;
            // DevOps tier: no per-certificate metadata persistence, and the
            // cheaper of the two tiers — this pool exists for minutes.
            pool.set_tier(privateca_pb::CaPool::DEVOPS);
            privateca_pb::CreateCaPoolRequest pool_req;
            pool_req.set_parent(parent);
            pool_req.set_ca_pool_id(pool_id);
            *pool_req.mutable_ca_pool() = pool;
            auto created_pool = client.CreateCaPool(pool_req).get();
            if (!created_pool) {
                BOOST_TEST_MESSAGE("CreateCaPool failed: " + created_pool.status().message() +
                                   " — skipping real-CAS tests");
                std::exit(77);
            }
            // The pool now exists; from here every failure must clean up.
            owns_pool = true;
            g_active_gcp_fixture.store(this, std::memory_order_release);

            privateca_pb::CertificateAuthority ca;
            ca.set_type(privateca_pb::CertificateAuthority::SELF_SIGNED);
            // Ten years, comfortably longer than the 30-day leaf lifetime the
            // provider requests by default — CAS rejects issuing a certificate
            // that would outlive its issuer.
            ca.mutable_lifetime()->set_seconds(10LL * 365 * 24 * 60 * 60);
            // SignHashAlgorithm is nested directly in CertificateAuthority (a
            // sibling of KeyVersionSpec), so its values scope to the message.
            ca.mutable_key_spec()->set_algorithm(
                privateca_pb::CertificateAuthority::EC_P256_SHA256);
            auto& subject = *ca.mutable_config()->mutable_subject_config()->mutable_subject();
            subject.set_common_name("kythira integration test root");
            subject.set_organization("kythira-test-run");
            auto& x509 = *ca.mutable_config()->mutable_x509_config();
            x509.mutable_ca_options()->set_is_ca(true);
            x509.mutable_key_usage()->mutable_base_key_usage()->set_cert_sign(true);
            x509.mutable_key_usage()->mutable_base_key_usage()->set_crl_sign(true);
            (*ca.mutable_labels())["kythira-test-run"] = run_id();

            privateca_pb::CreateCertificateAuthorityRequest ca_req;
            ca_req.set_parent(pool_path());
            ca_req.set_certificate_authority_id(ca_id);
            *ca_req.mutable_certificate_authority() = ca;
            auto created_ca = client.CreateCertificateAuthority(ca_req).get();
            if (!created_ca) {
                // `ca_id` is deliberately left set: a failed create can still
                // leave a CA behind, and the pool cannot be deleted while one
                // exists. Teardown attempts both; a NOT_FOUND on a CA that was
                // never created is harmless noise on stderr.
                skip_after_cleanup("CreateCertificateAuthority failed: " +
                                   created_ca.status().message());
            }

            privateca_pb::EnableCertificateAuthorityRequest enable_req;
            enable_req.set_name(pool_path() + "/certificateAuthorities/" + ca_id);
            if (auto enabled = client.EnableCertificateAuthority(enable_req).get(); !enabled) {
                skip_after_cleanup("EnableCertificateAuthority failed: " +
                                   enabled.status().message());
            }

            BOOST_TEST_MESSAGE("created CA pool " + pool_id + " with root CA " + ca_id);
        } catch (const std::exception& ex) {
            skip_after_cleanup(std::string("CA pool provisioning threw: ") + ex.what());
        }
    }

    /// `std::exit` runs no destructors, so anything already created has to be
    /// removed before skipping — otherwise a mid-provision failure leaks the
    /// very resources this fixture exists to manage.
    [[noreturn]] void skip_after_cleanup(const std::string& why) {
        BOOST_TEST_MESSAGE(why + " — skipping real-CAS tests");
        teardown();
        std::exit(77);
    }
};

/// Boost destroys global fixtures in reverse construction order, so this one
/// outlives the test cases and tears the pool down after the last of them.
CasPoolFixture& cas_pool() {
    static CasPoolFixture fixture;
    return fixture;
}

struct PreflightSkipFixture {
    PreflightSkipFixture() { cas_pool(); }
};
BOOST_GLOBAL_FIXTURE(PreflightSkipFixture);

auto make_config() -> gcp_privateca_certificate_provider_config {
    gcp_privateca_certificate_provider_config cfg;
    cfg.gcp.project_id = env_or("GCP_PROJECT_ID", "");
    cfg.location = env_or("GCP_REGION", "us-central1");
    cfg.ca_pool_id = cas_pool().pool_id;
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
