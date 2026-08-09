#pragma once

/// @file test_timeout_scale.hpp
/// @brief Scales per-test-case Boost timeouts by the build's measured slowdown.
///
/// `*boost::unit_test::timeout(N)` raises SIGALRM after N seconds of wall
/// clock, which is a fixed budget applied to a build whose speed is not fixed.
/// A coverage build (`-DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug`) runs the
/// coap suite several times slower than Release, so budgets sized for Release
/// expire mid-test and the case fails with SIGALRM rather than an assertion.
///
/// Measured on GitHub runners, full suite, same selection, 10-12 runs each
/// (`.github/workflows/coap-flake-measure.yml`, `profile=release` vs
/// `profile=coverage`):
///
/// | test                                  | Release | Coverage | ratio |
/// |---------------------------------------|--------:|---------:|------:|
/// | coap_get_joined_multicast_groups_test | 21.4s   | 63.7s    | 3.0x  |
/// | coap_future_resolution_property_test  | 38.9s   | 96.7s    | 2.5x  |
/// | coap_thread_safety_property_test      | 12.0s   | 23.5s    | 2.0x  |
/// | coap_duplicate_detection_property_test| 76.9s   | 133.9s   | 1.7x  |
///
/// Median 1.1x, p90 3.0x. Those figures average passing runs only — a test
/// that timed out contributes nothing — so they understate the true ratio for
/// exactly the tests that matter. Hence the default scale of 4 rather than 3.
///
/// This is not "raise the timeout until it passes", which has been tried four
/// times on this suite (92d824b, bc39d04, 9727d38, 5a9c5ff) without a
/// measurement behind it. The budget is unchanged for the Release builds those
/// numbers were sized against; it is widened only for a build measured to be
/// slower, by a factor taken from that measurement. See
/// doc/coap-flake-investigation.md.

#include <chrono>
#include <cstdlib>

#ifndef KYTHIRA_TEST_TIMEOUT_SCALE
#define KYTHIRA_TEST_TIMEOUT_SCALE 1
#endif

namespace kythira::testing {

/// @brief Per-case Boost timeout in seconds, scaled for the current build.
///
/// Consteval-friendly and cheap: the scale is a compile-time constant set by
/// CMake, so this is a multiply the optimiser folds away.
[[nodiscard]] constexpr auto scaled_timeout(unsigned seconds) noexcept -> unsigned {
    return seconds * static_cast<unsigned>(KYTHIRA_TEST_TIMEOUT_SCALE);
}

/// @brief An in-test deadline in milliseconds, scaled for the current build.
///
/// The sibling of scaled_timeout() for deadlines a test passes *into* the code
/// under test -- an RPC timeout argument, a condition-variable wait -- rather
/// than the case's own SIGALRM budget. Both need scaling for the same reason:
/// a fixed millisecond budget is being applied to a build and a machine whose
/// speed is not fixed.
///
/// This exists because proxygen_transport_test hardcoded a 3000ms RPC deadline
/// in nine places and failed CI with `ingress timeout, streamID=1,
/// timeout=3000ms`. Scaling the case timeout alone would not have helped
/// there: the deadline that expired was the one handed to send_request_vote(),
/// not Boost's.
[[nodiscard]] constexpr auto scaled_deadline(unsigned milliseconds) noexcept
    -> std::chrono::milliseconds {
    return std::chrono::milliseconds{milliseconds *
                                     static_cast<unsigned>(KYTHIRA_TEST_TIMEOUT_SCALE)};
}

/// @brief How much longer than nominal an observed sleep may take before the
///        test calls it a backoff defect.
///
/// Scheduler jitter, not build slowness. A retry delay is a real wall-clock
/// sleep, so how long it takes does not depend on whether the binary is
/// instrumented -- which is why this is *not* multiplied by
/// KYTHIRA_TEST_TIMEOUT_SCALE, unlike everything else in this header. What it
/// absorbs is the runner declining to schedule the thread the instant the
/// timer fires.
///
/// The value is additive because the delay it models is additive: a thread
/// waiting on a 50ms timer and one waiting on a 200ms timer are both handed
/// back late by roughly the same amount, having waited on the same kind of
/// timer on the same loaded machine.
inline constexpr std::chrono::milliseconds scheduler_jitter_allowance{50};

/// @brief Upper bound for an observed delay whose nominal value is `nominal`.
///
/// Backoff assertions were written as a bare percentage of nominal -- 20% --
/// which is the wrong shape for what actually goes wrong. A percentage band
/// narrows in absolute terms as the delay shrinks, so it is tightest on the
/// *first* retry, which is the shortest one and therefore the one every
/// exponential schedule starts with. At 50ms nominal, 20% is 10ms of headroom:
/// less than a scheduling quantum on a contended runner.
///
/// Observed on GitHub runners, all three from the same week of CI:
///
/// | test                                    | nominal | observed | old limit |
/// |-----------------------------------------|--------:|---------:|----------:|
/// | error_handler_async_retry (delay1)      |    50ms |     62ms |      60ms |
/// | error_handler_async_retry (delay2)      |   100ms |    122ms |     120ms |
/// | raft_timeout_classification (delay2)    |   100ms |    127ms |     120ms |
///
/// Every one of those is 12-27ms of jitter on a schedule that was otherwise
/// exactly right, failing a bound it missed by single-digit milliseconds.
///
/// Keeping the multiplicative factor *and* adding a fixed allowance leaves the
/// assertion sharp where it matters. At the 50/100/200ms schedule these tests
/// use, the bounds become 110/170/290ms, which still separates every mistake
/// the test exists to catch: a first delay that jumped straight to the cap
/// (200 > 110), a multiplier of 4 rather than 2 (200 > 170), and a cap that was
/// never applied (400 > 290).
[[nodiscard]] constexpr auto delay_upper_bound(std::chrono::milliseconds nominal,
                                               double factor = 1.2) noexcept
    -> std::chrono::milliseconds {
    return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(
               static_cast<double>(nominal.count()) * factor)} +
           scheduler_jitter_allowance;
}

}  // namespace kythira::testing
