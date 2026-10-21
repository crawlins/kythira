// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file object_persistence_real_cases_mock_test.cpp
/// @brief The shared real-tier cases run against the mock store.
///
/// This does not stand in for a live run: no task that needs one is checked off
/// against it. It exists so the cases themselves are known to be right before a
/// paid run spends money on them, and so they keep compiling in every PR's CI,
/// where the provider suites that include them are usually not built. A case
/// that fails here is a bug in the case; one that passes here and fails against
/// a service is a finding about that service.

#define BOOST_TEST_MODULE object_persistence_real_cases_mock_test

#include "mock_object_store.hpp"
#include "object_persistence_real_cases.hpp"

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <string>

namespace obj = kythira::object_real;

namespace {

constexpr const char* k_bucket = "bucket";
constexpr const char* k_provider = "mock";

using store_t = kythira::mock_object_store;

}  // namespace

BOOST_AUTO_TEST_SUITE(object_persistence_real_cases_on_the_mock)

BOOST_AUTO_TEST_CASE(layout_migration_read_back) {
    obj::case_layout_migration_read_back(store_t{}, k_bucket, "p/migrate");
}

BOOST_AUTO_TEST_CASE(segment_truncate_compact) {
    obj::case_segment_truncate_compact<obj::engine_for<store_t>>(store_t{}, k_bucket, "p/seg",
                                                                 obj::batched_options());
}

BOOST_AUTO_TEST_CASE(fenced_segment_truncate_compact) {
    obj::case_segment_truncate_compact<obj::fenced_engine_for<store_t>>(
        store_t{}, k_bucket, "p/seg-fenced", obj::batched_options("node-1"));
}

BOOST_AUTO_TEST_CASE(retired_vote_slot_with_negative_control) {
    obj::case_retired_vote_slot_with_negative_control(store_t{}, k_bucket, "p/retired");
}

BOOST_AUTO_TEST_CASE(batched_latency) {
    obj::case_batched_latency(store_t{}, k_bucket, "p/latency", k_provider, 3);
}

BOOST_AUTO_TEST_CASE(batched_backup_verify_restore) {
    obj::case_batched_backup_verify_restore(store_t{}, k_bucket, "p/src", "p/backups",
                                            "p/restored");
}

// A 20 ms PUT so the write-behind submitters actually queue behind a flush.
BOOST_AUTO_TEST_CASE(write_behind_commit_rate) {
    store_t store;
    store.state()->put_delay = std::chrono::milliseconds{20};
    obj::case_write_behind_commit_rate(store, k_bucket, "p/rate", k_provider);
}

BOOST_AUTO_TEST_SUITE_END()
