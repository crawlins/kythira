// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// snapshot_pacer (.kiro/specs/file-install-snapshot/ Requirement 6.3): a
// token bucket that runs into debt and says how long to wait.

#define BOOST_TEST_MODULE snapshot_pacer_test
#include <boost/test/unit_test.hpp>

#include <raft/snapshot_pacer.hpp>

#include <chrono>

using kythira::snapshot_pacer;
using namespace std::chrono_literals;

BOOST_AUTO_TEST_CASE(zero_rate_is_unlimited) {
    snapshot_pacer p(0);
    BOOST_TEST(p.reserve(1ULL << 40).count() == 0);
}

BOOST_AUTO_TEST_CASE(a_burst_goes_at_once_then_bytes_wait_their_turn) {
    const auto t0 = snapshot_pacer::clock::now();
    snapshot_pacer p(1000, 1000);
    BOOST_TEST(p.reserve(600, t0).count() == 0);
    BOOST_TEST(p.reserve(400, t0).count() == 0);
    // 500 bytes of debt at 1000 B/s.
    BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(p.reserve(500, t0)).count() ==
               500);
    // Another 500 queue behind them.
    BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(p.reserve(500, t0)).count() ==
               1000);
    // Two seconds later the debt is paid and the bucket is full again.
    BOOST_TEST(p.reserve(1000, t0 + 3s).count() == 0);
}

BOOST_AUTO_TEST_CASE(a_chunk_larger_than_the_burst_still_goes) {
    const auto t0 = snapshot_pacer::clock::now();
    snapshot_pacer p(100, 10);
    BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(p.reserve(110, t0)).count() ==
               1000);
}

BOOST_AUTO_TEST_CASE(the_bucket_never_holds_more_than_its_burst) {
    const auto t0 = snapshot_pacer::clock::now();
    snapshot_pacer p(100, 50);
    BOOST_TEST(p.reserve(50, t0 + 1h).count() == 0);
    BOOST_TEST(
        std::chrono::duration_cast<std::chrono::milliseconds>(p.reserve(10, t0 + 1h)).count() ==
        100);
}
