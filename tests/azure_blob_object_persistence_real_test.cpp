// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE azure_blob_object_persistence_real_test
#include <boost/test/unit_test.hpp>

#include "object_persistence_real_cases.hpp"

#include <raft/azure_blob_client.hpp>

// Real-Azure-Blob object-persistence suite (cloud-object-persistence task 16).
// NEVER CTest-registered — see object_persistence_real_support.hpp.
//
// Requires: KYTHIRA_AZURE_STORAGE_ACCOUNT and KYTHIRA_AZURE_BLOB_CONTAINER.
// The bearer token comes from the credential chain azure_client_config already
// carries — this client takes no storage-SDK dependency (task 0.6).

using namespace kythira;
namespace obj = kythira::object_real;

namespace {

using SignalTeardown = obj::signal_teardown_fixture;
BOOST_GLOBAL_FIXTURE(SignalTeardown);

constexpr const char* k_provider = "azure-blob";

struct RealBlobFixture {
    std::string account{obj::env("KYTHIRA_AZURE_STORAGE_ACCOUNT")};
    std::string bucket{obj::env("KYTHIRA_AZURE_BLOB_CONTAINER")};
    std::string prefix{obj::unique_prefix()};

    RealBlobFixture() {
        obj::skip_unless_configured(
            {{"KYTHIRA_AZURE_STORAGE_ACCOUNT", account}, {"KYTHIRA_AZURE_BLOB_CONTAINER", bucket}});
        obj::skip_unless_reachable(client(), bucket, "kythira-real-test/");
        obj::register_prefix_teardown(client(), bucket, prefix);
    }

    [[nodiscard]] auto client() const -> azure_blob_client {
        azure_blob_config cfg;
        cfg.account = account;
        return azure_blob_client{cfg};
    }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(azure_blob_object_persistence_real)

BOOST_FIXTURE_TEST_CASE(fresh_engine_read_back, RealBlobFixture, *boost::unit_test::timeout(600)) {
    obj::case_fresh_engine_read_back(client(), bucket, prefix + "/node-1");
}

BOOST_FIXTURE_TEST_CASE(measured_latency, RealBlobFixture, *boost::unit_test::timeout(900)) {
    obj::case_measured_latency(client(), bucket, prefix + "/latency", k_provider);
}

BOOST_FIXTURE_TEST_CASE(fencing_race_with_negative_control, RealBlobFixture,
                        *boost::unit_test::timeout(600)) {
    obj::case_fencing_race_with_negative_control(client(), bucket, prefix + "/fenced");
}

BOOST_FIXTURE_TEST_CASE(backup_verify_restore_read_back, RealBlobFixture,
                        *boost::unit_test::timeout(900)) {
    obj::case_backup_verify_restore_read_back(client(), bucket, prefix + "/src",
                                              prefix + "/backups", prefix + "/restored");
}

BOOST_FIXTURE_TEST_CASE(list_after_write, RealBlobFixture, *boost::unit_test::timeout(900)) {
    obj::case_list_after_write(client(), bucket, prefix + "/law", k_provider);
}

BOOST_AUTO_TEST_SUITE_END()
