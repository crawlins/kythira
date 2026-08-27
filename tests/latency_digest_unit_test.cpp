// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file latency_digest_unit_test.cpp
/// @brief The windowed latency estimator behind `_p99_read_latency` and
///        `_p99_apply_latency` (task 36 of `.kiro/specs/multi-raft/`).
///
/// The property that earns this file its keep is **decay**. A lifetime
/// percentile never comes down, so a shard that was slow once reads as slow
/// forever and any latency-derived policy becomes permanently sticky. Bucket
/// accuracy is the obvious thing to test and the least interesting: a threshold
/// comparison does not spend the precision, which is why the estimator is a
/// coarse fixed-bucket histogram in the first place.
///
/// The second property is that a reported percentile is never *lower* than the
/// truth. Rounding a latency down is the direction that hides a problem.

#define BOOST_TEST_MODULE latency_digest_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/latency_digest.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using kythira::latency_digest;
using kythira::latency_histogram;
using namespace std::chrono_literals;

namespace {

/// Instrumented global allocation counter, for the "no allocation on the record
/// path" assertion. A counter rather than a timing measurement, deliberately:
/// timing would be a flaky proxy for the thing actually being claimed.
std::atomic<std::size_t> g_allocations{0};

}  // namespace

void* operator new(std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n)) {
        return p;
    }
    throw std::bad_alloc{};
}

void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

BOOST_AUTO_TEST_SUITE(latency_digest_unit)

// ── bucket layout ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(small_values_are_exact_and_larger_ones_are_within_the_stated_error) {
    // Below the linear region every nanosecond has its own bucket.
    for (std::uint64_t v = 0; v < latency_histogram::k_linear; ++v) {
        BOOST_CHECK_EQUAL(latency_histogram::bucket_upper_bound(latency_histogram::bucket_index(v)),
                          v);
    }

    // Above it, the reported value is the bucket's upper bound — never below
    // the sample — and within `k_relative_error` of it.
    const std::uint64_t probes[] = {16, 17, 100, 999, 1'000'000, 250'000'000, 1'000'000'000};
    for (auto v : probes) {
        const auto reported =
            latency_histogram::bucket_upper_bound(latency_histogram::bucket_index(v));
        BOOST_CHECK_GE(reported, v);
        const double error = static_cast<double>(reported - v) / static_cast<double>(v);
        BOOST_CHECK_MESSAGE(
            error <= latency_histogram::k_relative_error,
            "relative error " << error << " exceeds the documented bound for v=" << v);
    }
}

BOOST_AUTO_TEST_CASE(bucket_indices_are_monotonic_and_contiguous) {
    // A gap or an inversion here would silently mis-order the quantile walk,
    // which reads buckets in index order and assumes index order is value
    // order.
    std::size_t previous = 0;
    for (std::uint64_t v = 0; v < 100'000; ++v) {
        const auto index = latency_histogram::bucket_index(v);
        BOOST_REQUIRE_GE(index, previous);
        BOOST_REQUIRE_LT(index, latency_histogram::k_bucket_count);
        previous = index;
    }
}

BOOST_AUTO_TEST_CASE(values_past_the_top_octave_clamp_rather_than_overflow) {
    // A latency of ten minutes and a latency of ten hours call for the same
    // action, so the top bucket absorbing both costs nothing — and an
    // out-of-range index would be a buffer overrun.
    const auto huge = latency_histogram::bucket_index(std::uint64_t{1} << 62);
    BOOST_CHECK_EQUAL(huge, latency_histogram::k_bucket_count - 1);
}

// ── percentiles ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_known_sample_set_yields_the_expected_p99) {
    latency_digest digest{1};
    // 99 samples at 1 ms, one at 500 ms. p99 of 100 samples is the 99th, which
    // is still 1 ms; p100 is the outlier.
    for (int i = 0; i < 99; ++i) {
        digest.record(1ms);
    }
    digest.record(500ms);

    BOOST_CHECK_EQUAL(digest.count(), 100U);

    const auto p99 = digest.p99();
    BOOST_CHECK_GE(p99.count(), std::chrono::nanoseconds{1ms}.count());
    // Within the bucket error of 1 ms, and nowhere near the 500 ms outlier.
    BOOST_CHECK_LT(p99.count(), std::chrono::nanoseconds{2ms}.count());

    const auto p100 = digest.quantile(1.0);
    BOOST_CHECK_GE(p100.count(), std::chrono::nanoseconds{500ms}.count());
}

BOOST_AUTO_TEST_CASE(an_empty_digest_reports_zero) {
    const latency_digest digest{4};
    BOOST_CHECK_EQUAL(digest.count(), 0U);
    BOOST_CHECK_EQUAL(digest.p99().count(), 0);
}

BOOST_AUTO_TEST_CASE(a_sub_millisecond_sample_is_not_recorded_as_zero) {
    // The exact failure the truncation fix in `node::read_state` addresses: a
    // percentile built from millisecond-truncated durations reports zero for
    // every read that took less than a millisecond, which is most of them.
    latency_digest digest{1};
    digest.record(std::chrono::nanoseconds{250});
    BOOST_CHECK_GT(digest.p99().count(), 0);
    BOOST_CHECK_LT(digest.p99().count(), 1000);
}

// ── the window: decay ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_burst_then_silence_decays_to_idle_within_one_window) {
    // The point of the whole design. A lifetime percentile would still be
    // reporting the burst an hour later.
    constexpr std::size_t windows = 4;
    latency_digest digest{windows};

    for (int i = 0; i < 1000; ++i) {
        digest.record(200ms);
    }
    BOOST_CHECK_GE(digest.p99().count(), std::chrono::nanoseconds{200ms}.count());

    // Silence. Each rotation retires one sub-window; after a full lap the burst
    // is gone entirely rather than merely diluted.
    for (std::size_t i = 0; i < windows; ++i) {
        digest.rotate();
    }
    BOOST_CHECK_EQUAL(digest.count(), 0U);
    BOOST_CHECK_EQUAL(digest.p99().count(), 0);
}

BOOST_AUTO_TEST_CASE(samples_inside_the_window_survive_rotation) {
    // Decay must not become amnesia: a percentile that forgot everything on
    // each tick would be a per-tick maximum wearing a percentile's name.
    constexpr std::size_t windows = 4;
    latency_digest digest{windows};

    for (std::size_t i = 0; i < windows - 1; ++i) {
        for (int j = 0; j < 100; ++j) {
            digest.record(5ms);
        }
        digest.rotate();
    }
    BOOST_CHECK_EQUAL(digest.count(), 100U * (windows - 1));
    BOOST_CHECK_GE(digest.p99().count(), std::chrono::nanoseconds{5ms}.count());
}

BOOST_AUTO_TEST_CASE(a_single_window_digest_is_legal_if_coarse) {
    // Degenerates to "everything since the last rotation" — a legitimate
    // configuration, and the one a caller gets by asking for zero.
    latency_digest digest{0};
    BOOST_CHECK_EQUAL(digest.window_count(), 1U);
    digest.record(1ms);
    BOOST_CHECK_EQUAL(digest.count(), 1U);
    digest.rotate();
    BOOST_CHECK_EQUAL(digest.count(), 0U);
}

// ── the record path ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(recording_allocates_nothing) {
    // Asserted with an instrumented allocator rather than a timing measurement,
    // because "fast" is a proxy and "allocated zero times" is the claim.
    latency_digest digest{4};
    digest.record(1ms);  // warm any lazy initialisation

    const auto before = g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < 10'000; ++i) {
        digest.record(std::chrono::nanoseconds{i * 37 + 1});
    }
    digest.rotate();
    const auto after = g_allocations.load(std::memory_order_relaxed);

    BOOST_CHECK_EQUAL(after - before, 0U);
}

BOOST_AUTO_TEST_CASE(concurrent_recorders_lose_no_samples) {
    // `record` runs on whichever thread served the request; `quantile` runs on
    // the policy tick. The counters are relaxed atomics, so the two race by
    // construction — but a race must not lose a count.
    latency_digest digest{2};
    constexpr int threads = 4;
    constexpr int per_thread = 5000;

    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&digest] {
            for (int i = 0; i < per_thread; ++i) {
                digest.record(std::chrono::nanoseconds{1000});
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    BOOST_CHECK_EQUAL(digest.count(), static_cast<std::uint64_t>(threads) * per_thread);
}

BOOST_AUTO_TEST_CASE(histograms_merge) {
    latency_histogram a;
    latency_histogram b;
    for (int i = 0; i < 50; ++i) {
        a.record(1ms);
        b.record(1ms);
    }
    a.merge(b);
    BOOST_CHECK_EQUAL(a.count(), 100U);
    BOOST_CHECK_GE(a.quantile(0.5).count(), std::chrono::nanoseconds{1ms}.count());
}

BOOST_AUTO_TEST_SUITE_END()
