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
#include <cstdlib>
#include <optional>
#include <string>

namespace obj = kythira::object_real;

namespace {

constexpr const char* k_bucket = "bucket";
constexpr const char* k_provider = "mock";

using store_t = kythira::mock_object_store;

/// The sweep's reduced grid (segment-size-sweep Requirement 10.1): a handful of
/// samples, and three sizes: one below a 4 KiB entry's body (every run is
/// singletons), one where case 12's fill is one grown entry, and 512 KiB,
/// where it is several 96 KiB entries in one segment and a 4 KiB follower run
/// splits into two.
auto mock_grid() -> obj::sweep_grid {
    return {.segment_bytes = {4 * obj::k_kib, 64 * obj::k_kib, 512 * obj::k_kib},
            .samples = 3,
            .recovery_samples = 1,
            .recovery_log_bytes = 256 * obj::k_kib,
            .recovery_entry_payload = 16 * obj::k_kib};
}

/// Sets `KYTHIRA_SEGMENT_SWEEP` for one test case and restores it after.
struct sweep_opt_in {
    explicit sweep_opt_in(const char* value) {
        if (const char* old = std::getenv("KYTHIRA_SEGMENT_SWEEP"); old != nullptr) {
            _old = old;
        }
        if (value != nullptr) {
            ::setenv("KYTHIRA_SEGMENT_SWEEP", value, 1);
        } else {
            ::unsetenv("KYTHIRA_SEGMENT_SWEEP");
        }
    }
    ~sweep_opt_in() {
        if (_old.has_value()) {
            ::setenv("KYTHIRA_SEGMENT_SWEEP", _old->c_str(), 1);
        } else {
            ::unsetenv("KYTHIRA_SEGMENT_SWEEP");
        }
    }
    sweep_opt_in(const sweep_opt_in&) = delete;
    auto operator=(const sweep_opt_in&) -> sweep_opt_in& = delete;
    sweep_opt_in(sweep_opt_in&&) = delete;
    auto operator=(sweep_opt_in&&) -> sweep_opt_in& = delete;

private:
    std::optional<std::string> _old;
};

/// Every object under @p prefix, so a case that skipped can be shown to have
/// written nothing.
auto objects_under(const store_t& store, const std::string& prefix) -> std::size_t {
    return store.list_keys(k_bucket, prefix).size();
}

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

// ── Segment-size sweep (segment-size-sweep task 2) ──────────────────────────

BOOST_AUTO_TEST_CASE(segment_sweep_put) {
    const sweep_opt_in on{"true"};
    obj::case_segment_sweep_put(store_t{}, k_bucket, "p/sweep", k_provider, mock_grid());
}

BOOST_AUTO_TEST_CASE(segment_sweep_follower_run) {
    const sweep_opt_in on{"true"};
    obj::case_segment_sweep_follower_run(store_t{}, k_bucket, "p/sweep", k_provider, mock_grid());
}

BOOST_AUTO_TEST_CASE(segment_sweep_truncate) {
    const sweep_opt_in on{"true"};
    obj::case_segment_sweep_truncate<obj::engine_for<store_t>>(store_t{}, k_bucket, "p/sweep",
                                                               k_provider, "", mock_grid());
}

BOOST_AUTO_TEST_CASE(fenced_segment_sweep_truncate) {
    const sweep_opt_in on{"true"};
    obj::case_segment_sweep_truncate<obj::fenced_engine_for<store_t>>(
        store_t{}, k_bucket, "p/sweep-fenced", k_provider, "node-1", mock_grid());
}

BOOST_AUTO_TEST_CASE(segment_sweep_recovery) {
    const sweep_opt_in on{"true"};
    obj::case_segment_sweep_recovery(store_t{}, k_bucket, "p/sweep", k_provider, mock_grid());
}

// A 5 ms PUT so the write-behind submitters queue behind a flush.
BOOST_AUTO_TEST_CASE(segment_sweep_write_behind) {
    const sweep_opt_in on{"true"};
    store_t store;
    store.state()->put_delay = std::chrono::milliseconds{5};
    obj::case_segment_sweep_write_behind(store, k_bucket, "p/sweep", k_provider, mock_grid());
}

// Without the opt-in every sweep case passes at once and writes nothing
// (Requirement 9.2), so an ordinary bundle run is unchanged.
BOOST_AUTO_TEST_CASE(segment_sweep_skips_without_the_opt_in) {
    const sweep_opt_in off{nullptr};
    BOOST_TEST(!obj::sweep_enabled());
    const store_t store;
    obj::case_segment_sweep_put(store, k_bucket, "p/off", k_provider);
    obj::case_segment_sweep_follower_run(store, k_bucket, "p/off", k_provider);
    obj::case_segment_sweep_truncate<obj::engine_for<store_t>>(store, k_bucket, "p/off", k_provider,
                                                               "");
    obj::case_segment_sweep_recovery(store, k_bucket, "p/off", k_provider);
    obj::case_segment_sweep_write_behind(store, k_bucket, "p/off", k_provider);
    BOOST_TEST(objects_under(store, "p/off") == 0U);

    const sweep_opt_in other{"1"};
    BOOST_TEST(!obj::sweep_enabled());
}

BOOST_AUTO_TEST_SUITE_END()
