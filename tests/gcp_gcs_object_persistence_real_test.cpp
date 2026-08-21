// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE gcp_gcs_object_persistence_real_test
#include <boost/test/unit_test.hpp>

#include "object_persistence_real_cases.hpp"

#include <raft/gcp_gcs_client.hpp>

// Real-GCS object-persistence suite (cloud-object-persistence task 16).
// NEVER CTest-registered — see object_persistence_real_support.hpp.
//
// Requires: KYTHIRA_GCS_BUCKET. Credentials come from Application Default
// Credentials, which is how a node on GCE authenticates; GOOGLE_CLOUD_PROJECT
// is honoured when set but object operations name a bucket, never a project.

using namespace kythira;
namespace obj = kythira::object_real;

namespace {

using SignalTeardown = obj::signal_teardown_fixture;
BOOST_GLOBAL_FIXTURE(SignalTeardown);

constexpr const char* k_provider = "gcs";

struct RealGcsFixture {
    std::string bucket{obj::env("KYTHIRA_GCS_BUCKET")};
    std::string prefix{obj::unique_prefix()};

    RealGcsFixture() {
        obj::skip_unless_configured({{"KYTHIRA_GCS_BUCKET", bucket}});
        obj::skip_unless_reachable(client(), bucket, "kythira-real-test/");
        obj::register_prefix_teardown(client(), bucket, prefix);
    }

    [[nodiscard]] auto client() const -> gcp_gcs_client {
        gcp_client_config cfg;
        cfg.project_id = obj::env("GOOGLE_CLOUD_PROJECT");
        return gcp_gcs_client{cfg};
    }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(gcp_gcs_object_persistence_real)

BOOST_FIXTURE_TEST_CASE(fresh_engine_read_back, RealGcsFixture, *boost::unit_test::timeout(600)) {
    obj::case_fresh_engine_read_back(client(), bucket, prefix + "/node-1");
}

BOOST_FIXTURE_TEST_CASE(measured_latency, RealGcsFixture, *boost::unit_test::timeout(900)) {
    obj::case_measured_latency(client(), bucket, prefix + "/latency", k_provider);
}

BOOST_FIXTURE_TEST_CASE(fencing_race_with_negative_control, RealGcsFixture,
                        *boost::unit_test::timeout(600)) {
    obj::case_fencing_race_with_negative_control(client(), bucket, prefix + "/fenced");
}

BOOST_FIXTURE_TEST_CASE(backup_verify_restore_read_back, RealGcsFixture,
                        *boost::unit_test::timeout(900)) {
    obj::case_backup_verify_restore_read_back(client(), bucket, prefix + "/src",
                                              prefix + "/backups", prefix + "/restored");
}

BOOST_FIXTURE_TEST_CASE(list_after_write, RealGcsFixture, *boost::unit_test::timeout(900)) {
    obj::case_list_after_write(client(), bucket, prefix + "/law", k_provider);
}

BOOST_AUTO_TEST_SUITE_END()
