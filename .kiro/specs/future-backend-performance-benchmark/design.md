# Design Document

## Overview

This design adds a benchmark suite that measures and reports the performance
of `kythira::Future<T>` (Folly backend, `include/raft/future.hpp`) against
`kythira::stdexec_backend::Future<T>` (`include/raft/future_stdexec.hpp`)
across a fixed catalog of scenarios. It touches no existing backend code. It
adds:

1. A backend-generic **benchmark harness** (scenario functions templated on
   the future/promise/factory/collector types, constrained by
   `include/concepts/future.hpp`'s concepts) so each scenario is written once.
2. A **CTest-registered regression suite** with hardware-independent sanity
   floors only (no backend-vs-backend assertions), gated behind
   `stdexec_FOUND` via the existing `add_stdexec_test` helper.
3. A **standalone comparison report generator**, modeled on the existing
   `examples/performance_benchmark_report.cpp`, that prints a side-by-side
   table and writes a machine-readable artifact to `test_results/`.
4. **Documentation** (`doc/future_backend_performance_comparison.md`)
   recording methodology, how to run it, and known structural asymmetries.

The reason a shared, templated harness is the load-bearing design decision
(rather than two separately-written benchmark files, one per backend) is the
same reason `tests/stdexec_concept_wrappers_interoperability_property_test.cpp`
runs identical logic against both backends: any hand-duplicated
"equivalent" scenario risks silently drifting (different iteration counts,
different payload construction, a `get()` in one and a `wait()` in the
other) until the numbers are comparing two different things rather than one
operation on two backends. A single template instantiated twice cannot drift
that way by construction.

## Architecture

```
┌───────────────────────────────────────────────────────────────────┐
│ Backend-Generic Scenario Templates                                 │
│ (examples/future-backend-benchmark/benchmark_harness.hpp)          │
│                                                                      │
│  template<future_backend_traits Traits>                            │
│  BenchmarkResult bench_creation_resolution(const BenchConfig&);     │
│  template<future_backend_traits Traits>                            │
│  BenchmarkResult bench_cross_thread_promise(const BenchConfig&);    │
│  ... (one function template per Requirement 2 scenario)             │
│                                                                      │
│  Constrained by include/concepts/future.hpp's future/promise/       │
│  future_factory/future_collector concepts; per-backend setup that   │
│  has no concept-level equivalent (scheduler construction) goes      │
│  through a small `Traits` type, not an #ifdef in the scenario body. │
└───────────────┬───────────────────────────────┬─────────────────────┘
                │ instantiated with               │ instantiated with
     ┌──────────▼───────────┐          ┌──────────▼─────────────────┐
     │ folly_backend_traits  │          │ stdexec_backend_traits      │
     │ (kythira:: types,      │          │ (kythira::stdexec_backend::│
     │  folly::CPUThreadPool  │          │  types, exec::single_      │
     │  Executor scheduler)   │          │  thread_context scheduler) │
     └──────────┬────────────┘          └──────────┬──────────────────┘
                │                                   │
     ┌──────────▼───────────────────────────────────▼──────────────┐
     │ Two consumers of the same scenario templates:                 │
     │  1. tests/future_backend_benchmark_test.cpp (CTest,           │
     │     sanity-floor assertions only, Requirement 4)               │
     │  2. examples/future-backend-benchmark/                        │
     │     benchmark_report.cpp (standalone report generator,        │
     │     Requirement 5)                                             │
     └─────────────────────────────────────────────────────────────┘
```

### Why the harness lives under `examples/` rather than `tests/`

The scenario templates are consumed by two different binaries with two
different jobs (a fast, CI-safe regression check vs. a slower, thorough
report generator). Neither `tests/` nor `examples/` alone is the natural
owner of shared code consumed by both, but this project already has
precedent for shared non-test headers under `examples/` (e.g.
`examples/stdexec-backend/migration_guide_example.cpp` lives beside the spec
it documents, not under `tests/`). Putting the harness header under
`examples/future-backend-benchmark/` and having
`tests/future_backend_benchmark_test.cpp` include it keeps the CTest binary
thin (just sanity-floor assertions over shared logic) without making
`examples/` depend on `tests/`.

## Components and Interfaces

### `BenchConfig` and `BenchmarkResult`

```cpp
// examples/future-backend-benchmark/benchmark_harness.hpp
namespace kythira::benchmark {

struct BenchConfig {
    std::size_t warmup_iterations = 1000;
    std::size_t measured_iterations = 100000;
    std::size_t fan_in_n = 100;        // collectAll/collectAny width
    std::size_t chain_depth = 5;       // thenValue chain depth
    std::chrono::milliseconds delay{10};
};

struct BenchmarkResult {
    std::string scenario_name;
    std::string backend_name;          // "folly" or "stdexec"
    std::size_t operations = 0;
    std::chrono::nanoseconds total_duration{0};
    double ops_per_second = 0.0;
    // Populated only for latency-style scenarios (Requirement 3.3);
    // left empty (size 0) for pure-throughput scenarios.
    std::chrono::nanoseconds p50{0};
    std::chrono::nanoseconds p95{0};
    std::chrono::nanoseconds p99{0};
    std::string notes;                 // e.g. gap documentation (Req 2.10)
};

} // namespace kythira::benchmark
```

`BenchmarkResult` deliberately mirrors the existing `BenchmarkResult` struct
in `examples/performance_benchmark_report.cpp` (name/operations/duration/
ops_per_second/notes) with two additions (`backend_name`, percentile
fields) rather than inventing a new shape — this keeps the report generator
recognizable to anyone already familiar with that file.

### Backend Traits

```cpp
namespace kythira::benchmark {

struct folly_backend_traits {
    template<typename T> using future_type = kythira::Future<T>;
    template<typename T> using promise_type = kythira::Promise<T>;
    using factory_type = kythira::FutureFactory;
    using collector_type = kythira::FutureCollector;
    static constexpr const char* name = "folly";

    // Owns a warmed, ready-to-use single worker thread for `via`
    // scenarios; constructed once per benchmark run, never inside a
    // timed region (Requirement 3.5).
    struct scheduler_handle {
        std::shared_ptr<folly::CPUThreadPoolExecutor> executor;
        auto get() const -> folly::Executor*;
    };
    static auto make_scheduler() -> scheduler_handle;
};

struct stdexec_backend_traits {
    template<typename T> using future_type = kythira::stdexec_backend::Future<T>;
    template<typename T> using promise_type = kythira::stdexec_backend::Promise<T>;
    using factory_type = kythira::stdexec_backend::FutureFactory;
    using collector_type = kythira::stdexec_backend::FutureCollector;
    static constexpr const char* name = "stdexec";

    struct scheduler_handle {
        std::shared_ptr<exec::single_thread_context> context;
        auto get() const -> exec::single_thread_context::scheduler_type;
    };
    static auto make_scheduler() -> scheduler_handle;
};

} // namespace kythira::benchmark
```

Only `stdexec_backend_traits` is compiled when `KYTHIRA_HAS_STDEXEC` is
defined (Requirement 1.4), matching the existing `#if defined(KYTHIRA_HAS_STDEXEC)`
gating already used elsewhere in this project's `stdexec`-conditional code.

### Scenario Template Shape (Requirement 1, 2)

Every scenario has the same shape: a function template taking a `Traits`
type and a `BenchConfig`, returning a `BenchmarkResult`. Example for the
highest-risk scenario (cross-thread promise fulfillment, the one most
likely to show a real structural difference per
`.kiro/specs/stdexec-future-backend/spike-notes.md` finding 3):

```cpp
template<typename Traits>
auto bench_cross_thread_promise(const BenchConfig& cfg) -> BenchmarkResult {
    using promise_t = typename Traits::template promise_type<int>;

    // Setup outside the timed region (Requirement 3.5).
    std::vector<std::chrono::nanoseconds> latencies;
    latencies.reserve(cfg.measured_iterations);

    auto run_once = [] {
        promise_t promise;
        auto future = promise.getFuture();
        auto start = std::chrono::steady_clock::now();
        std::thread fulfiller([&promise] { promise.setValue(42); });
        int value = future.get();          // sink: value consumed below
        auto end = std::chrono::steady_clock::now();
        fulfiller.join();
        assert(value == 42);               // Requirement 3.4 do-not-optimize sink
        return end - start;
    };

    for (std::size_t i = 0; i < cfg.warmup_iterations; ++i) { run_once(); }
    for (std::size_t i = 0; i < cfg.measured_iterations; ++i) {
        latencies.push_back(run_once());
    }

    return make_latency_result("cross_thread_promise", Traits::name, latencies);
}
```

Each scenario in Requirement 2 (creation/resolution, same-thread promise,
cross-thread promise, `thenValue` chain at depth 1/5/20, `thenError`, `via`,
`collectAll` at N=10/100/1000, `collectAny`, `delay`/`within`) is one such
template in `benchmark_harness.hpp`, following this same pattern: warm-up
loop discarded, measured loop recorded, a `make_throughput_result` or
`make_latency_result` helper converts raw samples into a `BenchmarkResult`
per Requirement 3.2/3.3.

Spawning a real `std::thread` per iteration in `bench_cross_thread_promise`
is deliberate, not an oversight: the scenario exists specifically to
measure the cross-thread wake-up path (Requirement 2.3), and thread-creation
overhead is common to both backends' measured samples equally, so it cancels
out of the *relative* comparison even though it inflates both backends'
absolute per-iteration latency. The doc (Requirement 6) calls this out
explicitly so a reader doesn't mistake "absolute latency includes a
`std::thread` spawn" for a backend-specific cost.

### Latency Percentile Computation

```cpp
inline auto make_latency_result(std::string name, std::string backend,
                                 std::vector<std::chrono::nanoseconds> samples)
    -> BenchmarkResult {
    std::sort(samples.begin(), samples.end());
    auto pick = [&](double q) { return samples[static_cast<std::size_t>(q * (samples.size() - 1))]; };
    BenchmarkResult r;
    r.scenario_name = std::move(name);
    r.backend_name = std::move(backend);
    r.operations = samples.size();
    r.total_duration = std::accumulate(samples.begin(), samples.end(), std::chrono::nanoseconds{0});
    r.ops_per_second = samples.empty() ? 0.0
        : (samples.size() * 1e9) / static_cast<double>(r.total_duration.count());
    r.p50 = pick(0.50);
    r.p95 = pick(0.95);
    r.p99 = pick(0.99);
    return r;
}
```

Nearest-rank percentile over sorted samples, not a fitted distribution —
simple, has no hidden assumptions about the shape of the latency
distribution, and matches how the rest of this project already reports
percentiles (`doc/raft_performance_benchmarking.md`'s P50/P95/P99 latency
sections use the same convention).

### CTest Regression Suite (Requirement 4)

`tests/future_backend_benchmark_test.cpp` includes
`benchmark_harness.hpp`, runs every scenario with a **reduced** iteration
count (fast enough for routine CI, not the full statistical-confidence
count the report generator uses), and asserts only sanity floors:

```cpp
BOOST_AUTO_TEST_CASE(folly_creation_resolution_sanity_floor, *boost::unit_test::timeout(30)) {
    auto result = kythira::benchmark::bench_creation_resolution<
        kythira::benchmark::folly_backend_traits>(ci_bench_config());
    BOOST_CHECK(result.ops_per_second > 10000);  // same floor as existing performance_benchmark_test.cpp
}

#if defined(KYTHIRA_HAS_STDEXEC)
BOOST_AUTO_TEST_CASE(stdexec_creation_resolution_sanity_floor, *boost::unit_test::timeout(30)) {
    auto result = kythira::benchmark::bench_creation_resolution<
        kythira::benchmark::stdexec_backend_traits>(ci_bench_config());
    BOOST_CHECK(result.ops_per_second > 10000);
}
#endif
```

No test asserts `folly_result.ops_per_second > stdexec_result.ops_per_second`
or vice versa (Requirement 4.4) — each backend's sanity-floor check is
independent. Registered via:

```cmake
# tests/CMakeLists.txt
add_network_test(future_backend_benchmark_test future_backend_benchmark_test.cpp)
set_tests_properties(future_backend_benchmark_test PROPERTIES
    LABELS "performance;benchmark;future-backend"
    TIMEOUT 120)
```

The `#if defined(KYTHIRA_HAS_STDEXEC)` guard inside one translation unit
(rather than a second `add_stdexec_test`-gated executable) is used here
because the Folly-only sanity floors must still run even when `stdexec` is
unavailable, and both sets share the same `ci_bench_config()` helper and
`BOOST_TEST_MODULE`; splitting into two binaries would duplicate that
shared setup for no benefit, unlike the concept-compliance suites (which
are entirely stdexec-specific end to end and correctly use
`add_stdexec_test`).

### Standalone Comparison Report Generator (Requirement 5)

`examples/future-backend-benchmark/benchmark_report.cpp`, modeled directly
on `examples/performance_benchmark_report.cpp`'s `PerformanceBenchmark`
class shape:

```cpp
class FutureBackendComparisonReport {
public:
    explicit FutureBackendComparisonReport(kythira::benchmark::BenchConfig cfg) : cfg_(cfg) {}

    void run_all_scenarios();   // calls every bench_* template for folly_backend_traits,
                                 // and, if KYTHIRA_HAS_STDEXEC, stdexec_backend_traits
    void print_comparison_table(std::ostream&) const;
    void write_csv(const std::filesystem::path&) const;

private:
    kythira::benchmark::BenchConfig cfg_;
    std::vector<kythira::benchmark::BenchmarkResult> folly_results_;
    std::vector<kythira::benchmark::BenchmarkResult> stdexec_results_;
};
```

`main()` parses `--iterations`/`--warmup` (Requirement 5.4), runs both
sets, prints the table, and writes
`test_results/future_backend_benchmark_<timestamp>.csv` (Requirement 5.2),
using the same `std::filesystem` + `std::chrono::system_clock`-based
timestamp formatting this project's `test_results/*_<timestamp>.txt` files
already use elsewhere (e.g. the naming pattern visible in
`test_results/raft_perf_benchmark_20260226_135952.txt`).

When `KYTHIRA_HAS_STDEXEC` is not defined, `print_comparison_table` omits
the `stdexec` column and the delta column entirely rather than printing
empty/zero placeholders (Requirement 5.3) — a reader should not mistake
"not measured" for "measured as zero".

### Build Wiring

```cmake
# examples/CMakeLists.txt
add_executable(future_backend_benchmark_report
    future-backend-benchmark/benchmark_report.cpp)
target_link_libraries(future_backend_benchmark_report PRIVATE network_simulator)
if(TARGET Folly::folly)
    target_link_libraries(future_backend_benchmark_report PRIVATE Folly::folly)
endif()
if(stdexec_FOUND)
    target_link_libraries(future_backend_benchmark_report PRIVATE STDEXEC::stdexec TBB::tbb)
    target_compile_definitions(future_backend_benchmark_report PRIVATE KYTHIRA_HAS_STDEXEC)
endif()
```

This mirrors the existing conditional-link pattern already used for other
optional-dependency examples/tests in this project (`if(TARGET Folly::folly)`,
`if(stdexec_FOUND)`), rather than introducing a new pattern.

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all
valid executions of a system. Here, "correctness" means the benchmark
methodology is sound, not that either backend's future implementation is
correct (that is validated by the `stdexec-future-backend` spec's own
property suite).*

**Property 1: Backend-Generic Scenario Reuse**
*For any* scenario in the catalog, the same function template's body,
uninstantiated, should contain no backend-specific branching (no `if
constexpr` or `#ifdef` keyed on which backend is running) inside the timed
region — only `Traits`-parameterized type aliases and the `Traits`-provided
scheduler handle differ between instantiations.
**Validates: Requirements 1.1, 1.2, 1.3, 7.3**

**Property 2: Warm-up Exclusion**
*For any* scenario run with `warmup_iterations > 0`, the reported
`operations`/`total_duration`/percentiles should reflect exactly
`measured_iterations` samples, never including any warm-up sample.
**Validates: Requirement 3.1**

**Property 3: Setup Exclusion From Timed Region**
*For any* scenario requiring a scheduler/thread pool, the timed region
should begin only after that scheduler is fully constructed and warmed,
verified by asserting the first measured sample is not a statistical
outlier relative to the rest of the measured distribution (no
one-time-startup spike inside the measured set).
**Validates: Requirements 3.5, 7.2**

**Property 4: Repeated-Run Stability**
*For any* scenario and backend, running the same scenario twice in
immediate succession with identical `BenchConfig` should produce
`ops_per_second` values (or, for latency scenarios, P50 values) within a
documented tolerance (e.g. within 25% of each other on the same otherwise-idle
machine) — a self-check catching gross harness non-determinism, not a
strict performance regression gate.
**Validates: Requirement 7.1**

**Property 5: No Cross-Backend Assertions in CI**
*For any* CTest-registered benchmark test, its only pass/fail assertions
should be sanity floors evaluated against a single backend's own result;
no assertion should compare one backend's result against the other's.
**Validates: Requirements 4.3, 4.4**

**Property 6: Optional-Dependency Report Degradation**
*For any* build where `stdexec_FOUND` is false, the report generator should
build, run, and print a Folly-only table (no `stdexec` rows, no delta
column, no build failure).
**Validates: Requirements 1.4, 5.3**

**Property 7: Dead-Code-Elimination Resistance**
*For any* scenario whose measured operation produces a value not otherwise
used by control flow, the harness should consume that value through an
explicit sink before the timed region ends, and the compiled benchmark
binary (built with optimizations) should show non-trivial measured
duration rather than a near-zero time consistent with the loop being
optimized away.
**Validates: Requirement 3.4**

## Error Handling

Benchmark scenarios are not expected to throw under normal operation except
where the scenario is specifically exercising the error path (`thenError`,
exceptional futures). Where a scenario's own setup can fail (e.g. worker
thread construction), the harness lets the exception propagate out of the
scenario function rather than swallowing it into a zeroed `BenchmarkResult`
— a silently-zeroed result reads as "measured near-instant" rather than
"failed to run," which would be a more misleading failure mode than a
crashed benchmark run.

## Testing Strategy

### Dual Consumption, Not Dual Implementation

Per the Architecture section, there is exactly one implementation of each
scenario (the templates in `benchmark_harness.hpp`), consumed by two
binaries with different jobs:

- `tests/future_backend_benchmark_test.cpp` — fast (small iteration counts,
  `timeout(30)`-class), CTest-registered, sanity-floor-only, runs in normal
  CI per Requirement 4.
- `examples/future-backend-benchmark/benchmark_report.cpp` — slow (full
  iteration counts for statistical confidence), not CTest-registered, run
  on demand by a developer per Requirement 5, produces the human/machine
  readable comparison artifacts.

### Harness Self-Check (Property 4)

A dedicated `BOOST_AUTO_TEST_CASE(harness_repeated_run_stability, ...)` in
`tests/future_backend_benchmark_test.cpp` runs one representative
throughput scenario (`bench_creation_resolution`) twice back-to-back for
the Folly backend and asserts the two `ops_per_second` values are within
the documented tolerance of each other — this is the one test in the suite
whose purpose is validating the *harness*, not either future backend.

### Execution

All CTest-registered targets run exclusively through CTest, per this
project's test execution standards, labeled `performance;benchmark;
future-backend` so `ctest -L benchmark` selects the full performance
suite (this benchmark plus the pre-existing Raft performance benchmarks)
and `ctest -LE stdexec` continues to exclude `stdexec`-dependent pieces on
machines without the optional dependency. The standalone report generator
is a plain executable (`./build/examples/future_backend_benchmark_report`),
run manually, not through CTest, matching how
`examples/performance_benchmark_report.cpp` is already run today.
