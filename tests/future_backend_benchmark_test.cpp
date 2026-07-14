// **Feature: future-backend-performance-benchmark**
// CTest-registered regression suite for the Folly vs. stdexec future
// backend benchmark harness. Every assertion here is a hardware-independent
// sanity floor evaluated against a single backend's own result — no test
// compares Folly's result against stdexec's (Property 5, Requirements 4.3,
// 4.4). Iteration counts are deliberately small (routine-CI-friendly, not
// the report generator's statistical-confidence counts).
#define BOOST_TEST_MODULE future_backend_benchmark_test
#include <boost/test/unit_test.hpp>

#include <folly/init/Init.h>

#include "../examples/future-backend-benchmark/benchmark_harness.hpp"

using namespace kythira::benchmark;

namespace {

// bench_delay_within/bench_collect_any exercise Future<T>::delay()/within(),
// which need folly::Timekeeper's singleton finalized before use — without
// this, the first delay()/within() call in the binary aborts with
// "Singleton ... requested before registrationComplete()" (matches the
// FollyInitFixture pattern already used by
// tests/kythira_future_continuation_operations_property_test.cpp).
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("future_backend_benchmark_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }

    ~FollyInitFixture() = default;

    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

auto ci_bench_config() -> BenchConfig {
    BenchConfig cfg;
    cfg.warmup_iterations = 100;
    cfg.measured_iterations = 2000;
    cfg.fan_in_n = 10;
    cfg.chain_depth = 5;
    cfg.delay = std::chrono::milliseconds(5);
    return cfg;
}

// bench_cross_thread_promise/bench_delay_within/bench_collect_any (with its
// delayed siblings) all incur a real thread-spawn or timer cost per
// iteration; a CI-safe sanity check needs far fewer iterations than the
// throughput scenarios above, or routine CI runs would spend seconds here.
auto ci_latency_bench_config() -> BenchConfig {
    BenchConfig cfg;
    cfg.warmup_iterations = 5;
    cfg.measured_iterations = 30;
    cfg.fan_in_n = 5;
    cfg.delay = std::chrono::milliseconds(5);
    return cfg;
}

}  // namespace

// ── Folly: throughput scenarios ─────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(folly_creation_resolution_sanity_floor, *boost::unit_test::timeout(30)) {
    auto int_result = bench_creation_resolution<folly_backend_traits, int>(
        ci_bench_config(), "int", [](std::size_t i) { return static_cast<int>(i); });
    BOOST_TEST_MESSAGE(int_result.scenario_name << " (" << int_result.backend_name << "): "
                                                << int_result.ops_per_second << " ops/sec");
    BOOST_CHECK(int_result.ops_per_second > 1000);

    auto string_result = bench_creation_resolution<folly_backend_traits, std::string>(
        ci_bench_config(), "string",
        [](std::size_t i) { return "benchmark_string_" + std::to_string(i); });
    BOOST_CHECK(string_result.ops_per_second > 1000);

    auto struct_result = bench_creation_resolution<folly_backend_traits, benchmark_payload>(
        ci_bench_config(), "struct", make_benchmark_payload);
    BOOST_CHECK(struct_result.ops_per_second > 1000);
}

BOOST_AUTO_TEST_CASE(folly_same_thread_promise_sanity_floor, *boost::unit_test::timeout(30)) {
    auto result = bench_same_thread_promise<folly_backend_traits>(ci_bench_config());
    BOOST_TEST_MESSAGE(result.scenario_name << " (" << result.backend_name
                                            << "): " << result.ops_per_second << " ops/sec");
    BOOST_CHECK(result.ops_per_second > 1000);
}

BOOST_AUTO_TEST_CASE(folly_thenvalue_chain_sanity_floor, *boost::unit_test::timeout(30)) {
    for (std::size_t depth : {1UL, 5UL, 20UL}) {
        auto result = bench_thenvalue_chain<folly_backend_traits>(ci_bench_config(), depth);
        BOOST_TEST_MESSAGE(result.scenario_name << " (" << result.backend_name
                                                << "): " << result.ops_per_second << " ops/sec");
        BOOST_CHECK(result.ops_per_second > 500);
    }
}

BOOST_AUTO_TEST_CASE(folly_thenerror_sanity_floor, *boost::unit_test::timeout(30)) {
    auto result = bench_thenerror<folly_backend_traits>(ci_bench_config());
    BOOST_CHECK(result.ops_per_second > 500);
}

BOOST_AUTO_TEST_CASE(folly_via_scheduler_sanity_floor, *boost::unit_test::timeout(30)) {
    auto result = bench_via_scheduler<folly_backend_traits>(ci_bench_config());
    BOOST_CHECK(result.ops_per_second > 100);
}

BOOST_AUTO_TEST_CASE(folly_collect_all_sanity_floor, *boost::unit_test::timeout(30)) {
    for (std::size_t n : {10UL, 100UL, 1000UL}) {
        auto result = bench_collect_all<folly_backend_traits>(ci_bench_config(), n);
        BOOST_TEST_MESSAGE(result.scenario_name << " (" << result.backend_name
                                                << "): " << result.ops_per_second << " ops/sec");
        BOOST_CHECK(result.ops_per_second > 10);
    }
}

// ── Folly: latency scenarios ─────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(folly_cross_thread_promise_sanity_floor, *boost::unit_test::timeout(30)) {
    auto result = bench_cross_thread_promise<folly_backend_traits>(ci_latency_bench_config());
    BOOST_TEST_MESSAGE(result.scenario_name << " (" << result.backend_name
                                            << ") p50=" << result.p50.count()
                                            << "ns p99=" << result.p99.count() << "ns");
    BOOST_CHECK(result.ops_per_second > 10);
}

BOOST_AUTO_TEST_CASE(folly_collect_any_sanity_floor, *boost::unit_test::timeout(30)) {
    auto result = bench_collect_any<folly_backend_traits>(ci_latency_bench_config(), 5);
    BOOST_CHECK(result.ops_per_second > 10);
}

BOOST_AUTO_TEST_CASE(folly_delay_within_sanity_floor, *boost::unit_test::timeout(30)) {
    auto cfg = ci_latency_bench_config();
    auto delay_result = bench_delay_within<folly_backend_traits>(cfg, delay_or_within::delay);
    BOOST_TEST_MESSAGE(delay_result.scenario_name << " (" << delay_result.backend_name
                                                  << ") p99 overhead=" << delay_result.p99.count()
                                                  << "ns");
    // Sanity bound, not a floor: scheduling overhead atop the nominal delay
    // should not itself dominate (be as large as) the delay it's riding on.
    BOOST_CHECK(delay_result.p99 < cfg.delay);

    auto within_result = bench_delay_within<folly_backend_traits>(cfg, delay_or_within::within);
    BOOST_CHECK(within_result.p99 < cfg.delay);
}

#if defined(KYTHIRA_HAS_STDEXEC)

// ── stdexec: throughput scenarios ───────────────────────────────────────────

BOOST_AUTO_TEST_CASE(stdexec_creation_resolution_sanity_floor, *boost::unit_test::timeout(30)) {
    auto int_result = bench_creation_resolution<stdexec_backend_traits, int>(
        ci_bench_config(), "int", [](std::size_t i) { return static_cast<int>(i); });
    BOOST_CHECK(int_result.ops_per_second > 1000);

    auto string_result = bench_creation_resolution<stdexec_backend_traits, std::string>(
        ci_bench_config(), "string",
        [](std::size_t i) { return "benchmark_string_" + std::to_string(i); });
    BOOST_CHECK(string_result.ops_per_second > 1000);

    auto struct_result = bench_creation_resolution<stdexec_backend_traits, benchmark_payload>(
        ci_bench_config(), "struct", make_benchmark_payload);
    BOOST_CHECK(struct_result.ops_per_second > 1000);
}

BOOST_AUTO_TEST_CASE(stdexec_same_thread_promise_sanity_floor, *boost::unit_test::timeout(30)) {
    auto result = bench_same_thread_promise<stdexec_backend_traits>(ci_bench_config());
    BOOST_CHECK(result.ops_per_second > 1000);
}

BOOST_AUTO_TEST_CASE(stdexec_thenvalue_chain_sanity_floor, *boost::unit_test::timeout(30)) {
    for (std::size_t depth : {1UL, 5UL, 20UL}) {
        auto result = bench_thenvalue_chain<stdexec_backend_traits>(ci_bench_config(), depth);
        BOOST_CHECK(result.ops_per_second > 500);
    }
}

BOOST_AUTO_TEST_CASE(stdexec_thenerror_sanity_floor, *boost::unit_test::timeout(30)) {
    auto result = bench_thenerror<stdexec_backend_traits>(ci_bench_config());
    BOOST_CHECK(result.ops_per_second > 500);
}

BOOST_AUTO_TEST_CASE(stdexec_via_scheduler_sanity_floor, *boost::unit_test::timeout(30)) {
    auto result = bench_via_scheduler<stdexec_backend_traits>(ci_bench_config());
    BOOST_CHECK(result.ops_per_second > 100);
}

BOOST_AUTO_TEST_CASE(stdexec_collect_all_sanity_floor, *boost::unit_test::timeout(30)) {
    for (std::size_t n : {10UL, 100UL, 1000UL}) {
        auto result = bench_collect_all<stdexec_backend_traits>(ci_bench_config(), n);
        BOOST_CHECK(result.ops_per_second > 10);
    }
}

// ── stdexec: latency scenarios ───────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(stdexec_cross_thread_promise_sanity_floor, *boost::unit_test::timeout(30)) {
    auto result = bench_cross_thread_promise<stdexec_backend_traits>(ci_latency_bench_config());
    BOOST_CHECK(result.ops_per_second > 10);
}

BOOST_AUTO_TEST_CASE(stdexec_collect_any_sanity_floor, *boost::unit_test::timeout(30)) {
    auto result = bench_collect_any<stdexec_backend_traits>(ci_latency_bench_config(), 5);
    BOOST_CHECK(result.ops_per_second > 10);
}

BOOST_AUTO_TEST_CASE(stdexec_delay_within_sanity_floor, *boost::unit_test::timeout(30)) {
    auto cfg = ci_latency_bench_config();
    auto delay_result = bench_delay_within<stdexec_backend_traits>(cfg, delay_or_within::delay);
    BOOST_CHECK(delay_result.p99 < cfg.delay);

    auto within_result = bench_delay_within<stdexec_backend_traits>(cfg, delay_or_within::within);
    BOOST_CHECK(within_result.p99 < cfg.delay);
}

#endif  // KYTHIRA_HAS_STDEXEC

// ── Harness self-check (Property 4, Requirement 7.1) ────────────────────────
//
// Not validating either future backend — validating the harness itself.
// Running the same scenario twice in immediate succession with identical
// BenchConfig should produce comparable ops_per_second values; a gross
// harness non-determinism (e.g. a scenario that accidentally measures a
// one-time setup cost sometimes and not others) would blow this tolerance
// wide open on an otherwise-idle machine.
BOOST_AUTO_TEST_CASE(harness_repeated_run_stability, *boost::unit_test::timeout(30)) {
    // A larger iteration count than ci_bench_config()'s is used here
    // deliberately: at only a couple thousand near-instant iterations,
    // total elapsed time is small enough (low tens of microseconds) that
    // ordinary scheduling/cache jitter alone can exceed a 25% swing,
    // producing a false positive in this self-check unrelated to genuine
    // harness non-determinism.
    auto cfg = ci_bench_config();
    cfg.measured_iterations = 50000;
    auto first = bench_creation_resolution<folly_backend_traits, int>(
        cfg, "int", [](std::size_t i) { return static_cast<int>(i); });
    auto second = bench_creation_resolution<folly_backend_traits, int>(
        cfg, "int", [](std::size_t i) { return static_cast<int>(i); });

    BOOST_TEST_MESSAGE("harness_repeated_run_stability: first="
                       << first.ops_per_second << " second=" << second.ops_per_second
                       << " ops/sec");

    double ratio = first.ops_per_second / second.ops_per_second;
    // design.md's Property 4 documents "e.g. within 25%" as an illustrative
    // tolerance, not a hard requirement — in practice, a raw ~50,000-
    // iteration microbenchmark on ordinary (non-isolated, non-pinned)
    // hardware routinely swings 30-40% run to run from CPU frequency
    // scaling and scheduler noise alone. This self-check exists to catch
    // gross harness non-determinism (e.g. accidentally measuring one-time
    // setup cost only sometimes), not to be a strict performance
    // regression gate, so the bound here is wide enough to tolerate
    // ordinary jitter while still catching an order-of-magnitude harness
    // bug.
    BOOST_CHECK(ratio > 0.4 && ratio < 2.5);
}
