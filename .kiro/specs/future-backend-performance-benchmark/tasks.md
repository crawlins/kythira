# Implementation Plan

## Phase 0: Harness Foundation

- [ ] 0. Spike: confirm timing/scheduler-construction assumptions before writing scenario bodies
  - Confirm `std::chrono::steady_clock` resolution on the CI/dev machines already used (per
    `.kiro/specs/stdexec-future-backend/spike-notes.md`'s compiler matrix) is fine-grained enough
    for the smallest scenario (single creation/resolution) to produce a non-degenerate sample
  - Confirm whether `exec::single_thread_context` construction cost is large enough to matter if
    accidentally placed inside a timed region (informs Requirement 3.5's warm-scheduler rule)
  - Confirm the `-fno-strict-aliasing` flag already applied to `stdexec`-dependent targets (per
    spike-notes.md finding 2) is inherited by any new benchmark target automatically via existing
    CMake target properties, or must be re-applied explicitly
  - Record findings inline in this spec's design.md if any assumption in the Architecture section
    turns out to be wrong; otherwise no separate note file is needed (unlike the prior spec's
    Phase 0, this spike is narrowly scoped and low-risk)
  - _Requirements: 3.5, 7.2_

- [ ] 1. Create `examples/future-backend-benchmark/benchmark_harness.hpp` skeleton
  - Define `BenchConfig` and `BenchmarkResult` per design.md's Components section
  - Define `folly_backend_traits` (always compiled) and `stdexec_backend_traits` (gated behind
    `#if defined(KYTHIRA_HAS_STDEXEC)`)
  - Define `make_throughput_result`/`make_latency_result` helper functions
  - _Requirements: 1.1, 1.3, 1.4_

- [ ] 2. Implement `folly_backend_traits::make_scheduler()` and `stdexec_backend_traits::make_scheduler()`
  - Folly: construct a single-thread `folly::CPUThreadPoolExecutor`
  - stdexec: construct a single-thread `exec::single_thread_context`, exposing its scheduler
  - Confirm neither construction path is accidentally invoked from within any scenario's timed
    region (Requirement 3.5) once scenarios are written in later tasks
  - _Requirements: 1.3, 3.5_

## Phase 1: Throughput Scenarios

- [ ] 3. Implement `bench_creation_resolution<Traits>` (Requirement 2.1)
  - Cover `int`, `std::string`, and a multi-field struct payload as three sub-scenarios or three
    parameterized calls
  - Apply the do-not-optimize sink pattern from design.md to the `get()` result
  - _Requirements: 2.1, 3.4_

- [ ] 4. Implement `bench_same_thread_promise<Traits>` (Requirement 2.2)
  - `getFuture()` then `setValue` then `get()`, all on the calling thread
  - _Requirements: 2.2_

- [ ] 5. Implement `bench_thenvalue_chain<Traits>` at configurable depth (Requirement 2.4)
  - Parameterize chain depth via `BenchConfig::chain_depth`; run at minimum depths 1, 5, 20
  - Measure both chain construction and end-to-end resolution, not construction alone
  - _Requirements: 2.4_

- [ ] 6. Implement `bench_thenerror<Traits>` (Requirement 2.5)
  - Build a future via `makeExceptionalFuture`, attach `thenError`, measure resolution
  - _Requirements: 2.5_

- [ ] 7. Implement `bench_via_scheduler<Traits>` (Requirement 2.6)
  - Use each `Traits::make_scheduler()` handle constructed once outside the timed region
  - Measure `via(scheduler)` + resolution round trip
  - _Requirements: 2.6, 3.5_

- [ ] 8. Implement `bench_collect_all<Traits>` at N = 10, 100, 1000 (Requirement 2.7)
  - Build N ready or near-simultaneously-resolving futures, measure `collectAll` fan-in
  - _Requirements: 2.7_

- [ ] 9. Checkpoint — Phase 1 throughput scenarios complete
  - Confirm every scenario so far compiles and runs for both `folly_backend_traits` and (when
    `stdexec_FOUND`) `stdexec_backend_traits` with no `#ifdef` inside any scenario body
    (Property 1)
  - This is the natural checkpoint because Phase 2's latency scenarios reuse the same helper
    functions and sink patterns established here

## Phase 2: Latency Scenarios

- [ ] 10. Implement `bench_cross_thread_promise<Traits>` (Requirement 2.3)
  - Follow design.md's exact shape: promise created and `get()`-blocked on the calling thread,
    `setValue` called from a spawned `std::thread`
  - Record per-iteration latency (not aggregate throughput) via `make_latency_result`
  - Document in the scenario's own comment that per-iteration `std::thread` spawn cost is
    included in absolute numbers and cancels out of the relative comparison (per design.md)
  - _Requirements: 2.3, 3.3_

- [ ] 11. Implement `bench_collect_any<Traits>` (Requirement 2.8)
  - N sibling futures where exactly one is arranged to resolve first (e.g. via a short sleep on
    the others, or a pre-fulfilled promise for the "winner")
  - _Requirements: 2.8_

- [ ] 12. Implement `bench_delay_within<Traits>` (Requirement 2.9)
  - Measure scheduling overhead of `delay(cfg.delay)`/`within(cfg.delay)`, i.e. time from call to
    completion minus the nominal delay itself, not the delay duration
  - _Requirements: 2.9_

- [ ] 13. Document any scenario gap found during Phase 1/2 implementation (Requirement 2.10)
  - As of spec authorship, both backends implement the full concept set needed for every listed
    scenario (per the `stdexec-future-backend` spec's Phase 3 completion) — if implementation
    reveals an actual gap, record it in `doc/future_backend_performance_comparison.md` (Task 20)
    rather than silently dropping the scenario
  - _Requirements: 2.10_

## Phase 3: CTest Regression Suite

- [ ] 14. Create `tests/future_backend_benchmark_test.cpp`
  - Include `benchmark_harness.hpp`; define `ci_bench_config()` returning a `BenchConfig` with
    reduced iteration/warm-up counts suitable for routine CI (not the report generator's full
    counts)
  - Add one `BOOST_AUTO_TEST_CASE` per scenario per backend, asserting only a sanity floor
    (Requirement 4.3), with `#if defined(KYTHIRA_HAS_STDEXEC)` guarding the `stdexec` cases
  - Add `harness_repeated_run_stability` self-check (Property 4 / Requirement 7.1)
  - Confirm no test compares Folly's result against stdexec's result (Property 5)
  - _Requirements: 3.2, 3.3, 4.3, 4.4, 4.5, 7.1_

- [ ] 15. Register the test target with CTest
  - Add via `add_network_test(future_backend_benchmark_test future_backend_benchmark_test.cpp)`
    in `tests/CMakeLists.txt` (single binary handles both backends via internal `#if`, per
    design.md's rationale for not using `add_stdexec_test` here)
  - Apply `LABELS "performance;benchmark;future-backend"` and an appropriate `TIMEOUT`
  - _Requirements: 4.1, 4.2_

- [ ] 16. Verify CI behavior in both configurations
  - Run `ctest -L benchmark` with `stdexec_FOUND` true and confirm both backends' sanity floors
    pass
  - Run (or simulate, e.g. via a build directory with `stdexec` hidden from `find_package`) with
    `stdexec_FOUND` false and confirm the Folly-only sanity floors still pass and no `stdexec`
    target exists
  - _Requirements: 1.4, 4.1_

## Phase 4: Standalone Comparison Report Generator

- [ ] 17. Implement `examples/future-backend-benchmark/benchmark_report.cpp`
  - `FutureBackendComparisonReport` class per design.md, running every scenario for
    `folly_backend_traits` and, when available, `stdexec_backend_traits`
  - `print_comparison_table` omitting the `stdexec` column/delta when unavailable
    (Requirement 5.3 / Property 6)
  - `--iterations`/`--warmup` CLI options (Requirement 5.4)
  - _Requirements: 5.1, 5.3, 5.4_

- [ ] 18. Implement CSV/JSON artifact writing to `test_results/`
  - Timestamped filename following this project's existing `test_results/*_<timestamp>.*` naming
    convention
  - _Requirements: 5.2_

- [ ] 19. Wire `future_backend_benchmark_report` into `examples/CMakeLists.txt`
  - Conditional link against `Folly::folly` and (`stdexec_FOUND`) `STDEXEC::stdexec`/`TBB::tbb`,
    defining `KYTHIRA_HAS_STDEXEC` only in the latter case, per design.md's Build Wiring section
  - Confirm the example builds with `stdexec_FOUND` false (Property 6)
  - _Requirements: 1.4, 5.3_

## Phase 5: Documentation

- [ ] 20. Write `doc/future_backend_performance_comparison.md`
  - Model on `doc/raft_performance_benchmarking.md`'s structure: quick start, scenario catalog
    description, how to run the CTest suite vs. the report generator, how to interpret percentile
    vs. throughput results
  - Record the machine/OS/compiler/build-type the reference numbers were captured on
    (Requirement 6.2)
  - Call out the known structural asymmetries: `single_shot_channel`'s mutex on the cross-thread
    path, the `std::thread`-spawn cost folded into `bench_cross_thread_promise`'s absolute
    numbers, and the GCC `-fno-strict-aliasing` mitigation already applied to `stdexec` targets
    (Requirement 6.3)
  - State explicitly that this spec does not change `KYTHIRA_DEFAULT_FUTURE_BACKEND` and
    recommends no default (Requirement 6.4)
  - Record any scenario-gap finding from Task 13, if one occurred
  - _Requirements: 6.1, 6.2, 6.3, 6.4_

- [ ] 21. Run the full suite once end to end and capture reference numbers
  - Run `ctest -L benchmark --output-on-failure` and store output following this project's
    existing `test_results/` convention
  - Run `./build/examples/future_backend_benchmark_report` with report-quality iteration counts
    and paste the resulting comparison table into `doc/future_backend_performance_comparison.md`
  - _Requirements: 6.1, 6.2_

- [ ] 22. Final checkpoint — complete validation
  - Confirm all CTest-registered benchmark tests pass, the report generator builds and runs in
    both `stdexec_FOUND` configurations, and documentation is complete
  - Ask the user whether any measured result was surprising enough to warrant a follow-up
    investigation spec (out of scope for this spec itself, per Requirements' Out of Scope section)
