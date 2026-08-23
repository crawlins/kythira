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
// ── THE TENANCY FLAKE: RESOLVED 2026-08-22, BUT READ THIS IF RED RETURNS ────
//
// This suite passes: 5/5 cases, 3/3 dispatched CI runs on 2026-08-23 under the
// least-privilege WIF principal (spike-notes.md Finding 27). It is reported
// exactly like the other four providers'.
//
// It was not always so. From 2026-08-12 to 2026-08-22 this tenancy declined
// VALID Object Storage requests from the CI principal with `404
// BucketNotFound`, EPISODICALLY -- twelve identical bursts read 0.00% to
// 13.40%. It stopped on its own after ~86,000 clean requests, correlating to
// within minutes with a billing-account upgrade (trial -> pay-as-you-go). That
// correlation is plausible but was never confirmed and is no longer testable,
// so THE FAULT MAY RETURN.
//
// If it does, do not re-investigate from scratch. Findings 24-27 already
// eliminated:
//
//   * THIS CLIENT. A declined ListObjects had a byte-identical twin that
//     succeeded 6.7 s earlier in the same job -- same URI, UPST, principal and
//     bucketId. No client-shaped explanation survives that comparison.
//   * TENANCY POLICY. The `Oracle-Tags` `where` clause was removed, measured
//     and restored: 81/5000 declines with it, 122/5000 without. Exonerated.
//   * THE CREDENTIAL TYPE. It reproduced under a plain API key, not only the
//     CI's token-exchange UPST.
//
// and established that it needed all three of a non-administrator principal,
// Object Storage, and concurrency: serial 0.30% vs 16-way 11.70% for the same
// principal and request; an Administrator clean across 5000 at that same
// concurrency; the same principal's Compute calls in the same compartment
// clean.
//
// Reproduce it with scripts/ci-cloud-credentials/oci/
// reproduce-oci-object-storage-404.py, read the data-plane log with
// read-object-storage-log.sh. NOT the audit log -- data-plane operations are
// not audited by default, and neither log records an authorization decision
// at all.
//
// TWO RULES THAT OUTLIVE THE FAULT:
//
//   * Never re-run a red run hoping for green. That is how a real regression
//     is laundered into a flake, and it is why this suite stayed red for ten
//     days rather than being quietly retried.
//   * Quote a range, never a point estimate, for anything episodic. A single
//     run's rate is not a parameter of anything.
//
// And one temptation refused, recorded so it is refused deliberately if ever
// taken: the engine retries writes, but this suite's pre-flight LIST is the
// one request in a run with no retry, which makes the suite more fragile than
// the engine it tests. Giving it a retry would have turned red runs green
// while hiding the fault. That is a decision about tolerating red, not a fix.

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
