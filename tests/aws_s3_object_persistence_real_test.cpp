// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE aws_s3_object_persistence_real_test
#include <boost/test/unit_test.hpp>

#include "object_persistence_real_cases.hpp"
#include "ordered_global_fixtures.hpp"

#include <raft/aws_s3_client.hpp>

#include <aws/core/Aws.h>

// Real-S3 object-persistence suite (cloud-object-persistence task 16).
//
// NEVER CTest-registered — run by name, by the real-cloud CI job, against a
// real bucket. Every case is in object_persistence_real_cases.hpp; this file
// supplies authentication and a bucket, which is all that differs between
// providers.
//
// Requires: KYTHIRA_AWS_S3_BUCKET, KYTHIRA_AWS_REGION. Credentials come from
// the SDK's own chain, exactly as the engine takes them.

using namespace kythira;
namespace obj = kythira::object_real;

namespace {

struct AwsSdkFixture {
    AwsSdkFixture() {
        Aws::SDKOptions opts;
        Aws::InitAPI(opts);
    }
    ~AwsSdkFixture() {
        Aws::SDKOptions opts;
        Aws::ShutdownAPI(opts);
    }
    AwsSdkFixture(const AwsSdkFixture&) = delete;
    auto operator=(const AwsSdkFixture&) -> AwsSdkFixture& = delete;
    AwsSdkFixture(AwsSdkFixture&&) = delete;
    auto operator=(AwsSdkFixture&&) -> AwsSdkFixture& = delete;
};

using SignalTeardown = obj::signal_teardown_fixture;

// One registration, constructed in this order and destroyed in reverse:
// separately registered global fixtures run in link-address order, not
// declaration order (see ordered_global_fixtures.hpp).
// The SDK is up before the signal teardown is armed, and still up
// when it runs.
using SuiteGlobalFixtures =
    kythira::testing::ordered_global_fixtures<AwsSdkFixture, SignalTeardown>;
BOOST_GLOBAL_FIXTURE(SuiteGlobalFixtures);

constexpr const char* k_provider = "s3";

struct RealS3Fixture {
    std::string bucket{obj::env("KYTHIRA_AWS_S3_BUCKET")};
    std::string region{obj::env("KYTHIRA_AWS_REGION")};
    std::string prefix{obj::unique_prefix()};

    RealS3Fixture() {
        obj::skip_unless_configured(
            {{"KYTHIRA_AWS_S3_BUCKET", bucket}, {"KYTHIRA_AWS_REGION", region}});
        obj::skip_unless_reachable(client(), bucket, "kythira-real-test/");
        obj::register_prefix_teardown(client(), bucket, prefix);
    }

    [[nodiscard]] auto client() const -> aws_s3_client {
        aws_client_config cfg;
        cfg.region = region;
        return aws_s3_client{cfg};
    }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(aws_s3_object_persistence_real)

BOOST_FIXTURE_TEST_CASE(fresh_engine_read_back, RealS3Fixture, *boost::unit_test::timeout(600)) {
    obj::case_fresh_engine_read_back(client(), bucket, prefix + "/node-1");
}

BOOST_FIXTURE_TEST_CASE(measured_latency, RealS3Fixture, *boost::unit_test::timeout(900)) {
    obj::case_measured_latency(client(), bucket, prefix + "/latency", k_provider);
}

BOOST_FIXTURE_TEST_CASE(fencing_race_with_negative_control, RealS3Fixture,
                        *boost::unit_test::timeout(600)) {
    obj::case_fencing_race_with_negative_control(client(), bucket, prefix + "/fenced");
}

BOOST_FIXTURE_TEST_CASE(backup_verify_restore_read_back, RealS3Fixture,
                        *boost::unit_test::timeout(900)) {
    obj::case_backup_verify_restore_read_back(client(), bucket, prefix + "/src",
                                              prefix + "/backups", prefix + "/restored");
}

BOOST_FIXTURE_TEST_CASE(list_after_write, RealS3Fixture, *boost::unit_test::timeout(900)) {
    obj::case_list_after_write(client(), bucket, prefix + "/law", k_provider);
}

// ── object-store-batched-layout task 10 ─────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(layout_migration_read_back, RealS3Fixture,
                        *boost::unit_test::timeout(600)) {
    obj::case_layout_migration_read_back(client(), bucket, prefix + "/migrate");
}

BOOST_FIXTURE_TEST_CASE(segment_truncate_compact, RealS3Fixture, *boost::unit_test::timeout(600)) {
    obj::case_segment_truncate_compact<obj::engine_for<decltype(client())>>(
        client(), bucket, prefix + "/segment", obj::batched_options());
}

BOOST_FIXTURE_TEST_CASE(fenced_segment_truncate_compact, RealS3Fixture,
                        *boost::unit_test::timeout(600)) {
    obj::case_segment_truncate_compact<obj::fenced_engine_for<decltype(client())>>(
        client(), bucket, prefix + "/segment-fenced", obj::batched_options("node-1"));
}

BOOST_FIXTURE_TEST_CASE(retired_vote_slot_with_negative_control, RealS3Fixture,
                        *boost::unit_test::timeout(600)) {
    obj::case_retired_vote_slot_with_negative_control(client(), bucket, prefix + "/retired");
}

BOOST_FIXTURE_TEST_CASE(batched_latency, RealS3Fixture, *boost::unit_test::timeout(900)) {
    obj::case_batched_latency(client(), bucket, prefix + "/batched-latency", k_provider);
}

BOOST_FIXTURE_TEST_CASE(batched_backup_verify_restore, RealS3Fixture,
                        *boost::unit_test::timeout(900)) {
    obj::case_batched_backup_verify_restore(client(), bucket, prefix + "/batched-src",
                                            prefix + "/batched-backups",
                                            prefix + "/batched-restored");
}

BOOST_FIXTURE_TEST_CASE(write_behind_commit_rate, RealS3Fixture, *boost::unit_test::timeout(900)) {
    obj::case_write_behind_commit_rate(client(), bucket, prefix + "/commit-rate", k_provider);
}

BOOST_AUTO_TEST_SUITE_END()
