// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE oci_object_storage_persistence_real_test
#include <boost/test/unit_test.hpp>

#include "object_persistence_real_cases.hpp"

#include <raft/oci_object_storage_client.hpp>

// After the client header: oci_real_test_support.hpp names kythira::oci_client_config
// without including it itself.
#include "oci_real_test_support.hpp"

// Real-OCI object-persistence suite (cloud-object-persistence task 16).
// NEVER CTest-registered — see object_persistence_real_support.hpp.
//
// Requires KYTHIRA_OCI_OBJECT_BUCKET plus the credentials every OCI suite
// needs. Those are read through oci_real::real_test_config, NOT re-read here:
// that type exists precisely so the OCI suites cannot drift on which auth
// fields they forward, and forwarding a different set from this file would
// reproduce the bug its comment warns about — a fixture that skips-or-401s only
// in CI, where nobody runs a debugger.
//
// The namespace is deliberately left empty and resolved once at construction
// via `GET /n/` (task 0.8), so an operator does not need to know it.
//
// ── READ THIS BEFORE TRUSTING A RESULT FROM THIS SUITE ──────────────────────
//
// The CI tenancy intermittently declines VALID requests with `404
// BucketNotFound` — measured at 3-16%, across PUT/GET/DELETE/LIST, body-
// carrying or not (spike-notes.md). It resembles a policy `where` clause
// declining a different principal's Object Storage requests.
//
// Until that is understood, a failure here is ambiguous in a way the other four
// providers' failures are not: it may be a defect or it may be the tenancy. The
// suite is therefore run and reported like the others, but its result is
// **weaker evidence**, and the honest response to a red run is to investigate
// the decline's opc-request-id in the compartment audit log — not to re-run
// until it is green, which would launder a real regression into a flake.

using namespace kythira;
namespace obj = kythira::object_real;

namespace {

using SignalTeardown = obj::signal_teardown_fixture;
BOOST_GLOBAL_FIXTURE(SignalTeardown);

constexpr const char* k_provider = "oci-objectstorage";

struct RealOciObjectFixture {
    kythira::testing::oci_real::real_test_config oci{
        kythira::testing::oci_real::real_test_config::from_environment()};
    std::string bucket{obj::env("KYTHIRA_OCI_OBJECT_BUCKET")};
    std::string prefix{obj::unique_prefix()};

    RealOciObjectFixture() {
        obj::skip_unless_configured({{"KYTHIRA_OCI_OBJECT_BUCKET", bucket},
                                     {"KYTHIRA_OCI_REGION", oci.region},
                                     {"KYTHIRA_OCI_PRIVATE_KEY_PEM", oci.private_key_pem}});
        // Either the federated session token (CI's WIF exchange) or the full
        // API-key triple (a developer's ~/.oci/config). Named as the choice it
        // is, rather than as four independently-missing values, because
        // reporting all four would tell an operator to set values they must
        // not set.
        if (!oci.has_credentials()) {
            std::cout << "SKIP: OCI needs either KYTHIRA_OCI_SECURITY_TOKEN, or all three of"
                         " KYTHIRA_OCI_TENANCY_ID / KYTHIRA_OCI_USER_ID /"
                         " KYTHIRA_OCI_FINGERPRINT\n";
            std::cout << "SKIP: not run\n";
            std::cout.flush();
            std::exit(77);
        }
        obj::skip_unless_reachable(client(), bucket, "kythira-real-test/");
        obj::register_prefix_teardown(client(), bucket, prefix);
    }

    [[nodiscard]] auto client() const -> oci_object_storage_client {
        oci_object_storage_config cfg;
        cfg.oci = oci.client_config();
        return oci_object_storage_client{cfg};
    }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(oci_object_storage_persistence_real)

BOOST_FIXTURE_TEST_CASE(fresh_engine_read_back, RealOciObjectFixture,
                        *boost::unit_test::timeout(600)) {
    obj::case_fresh_engine_read_back(client(), bucket, prefix + "/node-1");
}

BOOST_FIXTURE_TEST_CASE(measured_latency, RealOciObjectFixture, *boost::unit_test::timeout(900)) {
    obj::case_measured_latency(client(), bucket, prefix + "/latency", k_provider);
}

BOOST_FIXTURE_TEST_CASE(fencing_race_with_negative_control, RealOciObjectFixture,
                        *boost::unit_test::timeout(600)) {
    obj::case_fencing_race_with_negative_control(client(), bucket, prefix + "/fenced");
}

BOOST_FIXTURE_TEST_CASE(backup_verify_restore_read_back, RealOciObjectFixture,
                        *boost::unit_test::timeout(900)) {
    obj::case_backup_verify_restore_read_back(client(), bucket, prefix + "/src",
                                              prefix + "/backups", prefix + "/restored");
}

BOOST_FIXTURE_TEST_CASE(list_after_write, RealOciObjectFixture, *boost::unit_test::timeout(900)) {
    obj::case_list_after_write(client(), bucket, prefix + "/law", k_provider);
}

BOOST_AUTO_TEST_SUITE_END()
