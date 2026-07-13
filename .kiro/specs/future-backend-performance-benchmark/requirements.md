# Requirements Document

## Introduction

This specification defines the requirements for a benchmark suite that
quantitatively compares the performance of the two `kythira` future
implementations added by prior specs: the Folly-backed `kythira::Future<T>`
(`include/raft/future.hpp`) and the `stdexec`-backed
`kythira::stdexec_backend::Future<T>` (`include/raft/future_stdexec.hpp`,
`feat/stdexec-future-backend`). Both backends satisfy the same regenericized
concepts in `include/concepts/future.hpp` (from the `stdexec-future-backend`
spec), which is what makes a like-for-like comparison possible: the same
benchmark scenario can be instantiated against either backend through a
single templated harness rather than two hand-duplicated implementations.

This is a **measurement-only** feature. It does not alter either backend's
implementation, does not change `KYTHIRA_DEFAULT_FUTURE_BACKEND`, and does
not declare a winner. Its output is a reproducible, documented set of numbers
and a methodology, so that a future decision about backend choice (for a
specific call site, or for the project default) can be made from evidence
rather than intuition. Where the two backends have structurally different
costs (e.g. `stdexec`'s cross-thread fulfillment goes through the hand-rolled
`single_shot_channel`'s mutex, per `.kiro/specs/stdexec-future-backend/spike-notes.md`
finding 3, whereas Folly's core has its own internal synchronization), the
benchmark is expected to surface that difference, not normalize it away.

## Glossary

- **Scenario**: A single named operation exercised identically on both
  backends (e.g. "creation and immediate resolution", "cross-thread
  promise fulfillment", "10-deep `thenValue` chain").
- **Harness**: The shared, backend-generic benchmark driver code that runs a
  scenario against a `Future`-concept-satisfying type parameter, so scenario
  logic is written once and instantiated twice (once per backend).
- **Warm-up iterations**: Iterations run before timing begins, discarded from
  the reported statistics, to let branch predictors, allocators, and (for the
  `stdexec` backend) any lazily-initialized scheduler/thread-pool state reach
  steady state.
- **Measured iterations**: The iterations whose timings are actually
  aggregated into the reported result.
- **Throughput benchmark**: A scenario reported as operations/second over a
  fixed iteration count.
- **Latency benchmark**: A scenario reported as a distribution (P50/P95/P99)
  of per-operation duration, used where individual-operation latency (not
  aggregate throughput) is the property of interest (e.g. cross-thread
  wake-up latency).
- **Sanity floor**: A conservative, hardware-independent lower bound
  (ops/sec or upper-bound latency) asserted in the CTest-registered
  regression variant of a benchmark, in the same spirit as the existing
  `> 10000 ops/sec` assertion in `tests/performance_benchmark_test.cpp`.
  Distinct from a **comparison assertion** (backend A must beat backend B),
  which this spec deliberately does not introduce, since relative
  performance is expected to shift across hardware, compiler, and `stdexec`
  version and should not gate CI.
- **Comparison report**: A human-readable table (and optionally a
  machine-readable CSV/JSON artifact) produced by a standalone report
  program, listing both backends' results per scenario side by side with a
  relative delta, following the existing convention of
  `examples/performance_benchmark_report.cpp`.
- **Backend-generic scenario function**: A function template parameterized
  on a future/promise/factory/collector type set satisfying the concepts in
  `include/concepts/future.hpp`, instantiated once for `kythira::` (Folly)
  types and once for `kythira::stdexec_backend::` types.

## Requirements

### Requirement 1

**User Story:** As a library maintainer, I want a shared, backend-generic
benchmark harness, so that comparing the two backends measures actual
backend behavior rather than accidental differences between two
independently-written benchmark implementations.

#### Acceptance Criteria

1. WHEN a benchmark scenario is implemented THEN the system SHALL express
   its logic once, as a function template parameterized on the future/
   promise/factory/collector types, constrained by the concepts in
   `include/concepts/future.hpp`
2. WHEN a scenario is run for the Folly backend and the `stdexec` backend
   THEN the system SHALL instantiate the same template with
   `kythira::` types and `kythira::stdexec_backend::` types respectively,
   without duplicating the scenario's control flow
3. WHEN a scenario requires backend-specific setup that has no shared
   concept-level equivalent (e.g. constructing a worker-thread scheduler:
   `folly::CPUThreadPoolExecutor` vs. `exec::single_thread_context`) THEN
   the system SHALL isolate that setup behind a small per-backend traits
   type, keeping the timed region itself backend-generic
4. WHEN the harness is compiled THEN the system SHALL build correctly with
   `stdexec_FOUND` false (Folly-only benchmarks still run; `stdexec`
   benchmarks and their targets do not exist), consistent with the existing
   optional-dependency pattern

### Requirement 2

**User Story:** As a developer evaluating the two backends, I want a fixed
catalog of comparable scenarios covering the operations both backends
implement, so that the comparison covers the paths actually used by Raft's
transport layer and general chaining code, not just the cheapest case.

#### Acceptance Criteria

1. WHEN the scenario catalog is defined THEN the system SHALL include
   creation-and-immediate-resolution (`FutureFactory::makeFuture` +
   `get()`) for `int`, `std::string`, and a multi-field struct payload
2. WHEN the scenario catalog is defined THEN the system SHALL include
   same-thread promise/future round trip (`Promise::getFuture()` then
   `setValue` then `get()` on the same thread that created the promise)
3. WHEN the scenario catalog is defined THEN the system SHALL include
   cross-thread promise/future round trip (promise created on one thread,
   `setValue` called from a second thread after the first thread has
   already called `get()` and is blocked), isolating the exact bridge
   mechanism called out in the Introduction (Folly's internal core vs.
   `stdexec`'s `single_shot_channel`)
4. WHEN the scenario catalog is defined THEN the system SHALL include a
   `thenValue` continuation chain at configurable depth (at minimum depths
   1, 5, and 20) measuring both construction and end-to-end resolution cost
5. WHEN the scenario catalog is defined THEN the system SHALL include
   `thenError` triggered by an exceptional future
6. WHEN the scenario catalog is defined THEN the system SHALL include
   `via(scheduler)` rescheduling onto a single worker thread
7. WHEN the scenario catalog is defined THEN the system SHALL include
   `collectAll` fan-in over N sibling futures, at minimum N of 10, 100, and
   1000
8. WHEN the scenario catalog is defined THEN the system SHALL include
   `collectAny` over N sibling futures where exactly one resolves first
9. WHEN the scenario catalog is defined THEN the system SHALL include
   `delay`/`within` scheduling a completion after a fixed short duration,
   measuring scheduling overhead rather than the duration itself
10. WHEN a scenario has no reasonable equivalent on one backend (none are
    currently known, since both backends implement the full concept set)
    THEN the system SHALL document the gap explicitly rather than silently
    omitting the scenario from the report

### Requirement 3

**User Story:** As a developer reading benchmark output, I want each
scenario reported with statistically meaningful numbers, so that a single
noisy run doesn't get mistaken for a real performance difference.

#### Acceptance Criteria

1. WHEN a scenario is run THEN the system SHALL execute a configurable
   number of warm-up iterations (excluded from reported statistics) before
   the measured iterations
2. WHEN a throughput scenario completes THEN the system SHALL report total
   operations, total measured duration, and derived operations/second
3. WHEN a latency scenario completes THEN the system SHALL report P50, P95,
   and P99 per-operation duration, not only a mean
4. WHEN a scenario's measured loop contains a call whose result is unused
   by subsequent code (e.g. a `get()` return value only checked, not
   consumed) THEN the system SHALL prevent the compiler from eliding the
   call through dead-code elimination, using an explicit sink (e.g.
   `BOOST_CHECK`/`assert` on the value, or a documented do-not-optimize
   helper), so measured numbers reflect real work
5. WHEN a scenario involves a background thread pool or scheduler THEN the
   system SHALL construct and warm that pool before the timed region
   begins, so one-time thread-startup cost is not attributed to the first
   measured iteration

### Requirement 4

**User Story:** As a CI maintainer, I want the benchmark suite integrated
into CTest without making CI flaky or gating on relative backend
performance, so that hardware variance across CI runners doesn't produce
false failures.

#### Acceptance Criteria

1. WHEN benchmark scenarios are registered as CTest targets THEN the system
   SHALL gate every `stdexec`-backend benchmark target behind
   `if(stdexec_FOUND)`, using the existing `add_stdexec_test` helper in
   `tests/CMakeLists.txt`
2. WHEN a benchmark test target is registered THEN the system SHALL apply
   the labels `performance`, `benchmark`, and `future-backend`, so
   `ctest -L benchmark` selects this suite and `ctest -LE stdexec` still
   excludes the `stdexec`-only portion on machines without the optional
   dependency
3. WHEN the CTest-registered variant of a scenario asserts a pass/fail
   condition THEN the system SHALL assert only a hardware-independent
   sanity floor (e.g. "completes in under N seconds", "achieves at least
   some conservative minimum ops/sec"), consistent with the existing
   pattern in `tests/performance_benchmark_test.cpp`
4. WHEN the CTest-registered variant runs THEN the system SHALL NOT assert
   that one backend outperforms the other, since relative performance is
   expected to vary across hardware, compiler, and `stdexec` version and
   must not gate CI
5. WHEN the benchmark suite is built in a `Debug` configuration THEN the
   system SHALL still compile and run correctly, but the CTest-registered
   sanity-floor assertions SHALL account for (or skip enforcing tight
   floors in) unoptimized builds, since absolute timing is not meaningful
   without optimization

### Requirement 5

**User Story:** As a developer deciding between backends for new code, I
want a standalone comparison report generator, so that I can run a single
program and get a side-by-side table of both backends' numbers for every
scenario, independent of CI's pass/fail constraints.

#### Acceptance Criteria

1. WHEN the report generator is built THEN the system SHALL produce an
   executable, following the existing convention of
   `examples/performance_benchmark_report.cpp`, that runs every scenario in
   the catalog against both backends and prints a side-by-side comparison
   table (scenario, Folly result, `stdexec` result, relative delta)
2. WHEN the report generator completes THEN the system SHALL also write a
   machine-readable artifact (CSV or JSON) to `test_results/`, following
   this project's existing convention of timestamped files under
   `test_results/` (e.g. `test_results/future_backend_benchmark_<timestamp>.csv`)
3. WHEN `stdexec_FOUND` is false THEN the system SHALL build and run a
   Folly-only report (omitting `stdexec` rows and the delta column) rather
   than failing to build
4. WHEN the report generator is invoked THEN the system SHALL accept
   command-line options for iteration count and warm-up count, so a
   developer can trade run time for statistical confidence without editing
   source

### Requirement 6

**User Story:** As a developer relying on this spec's output, I want the
methodology and results documented, so that numbers are interpreted with
their caveats rather than taken as universal truths.

#### Acceptance Criteria

1. WHEN documentation for this feature is written THEN the system SHALL
   add `doc/future_backend_performance_comparison.md`, modeled on the
   existing `doc/raft_performance_benchmarking.md`, describing how to run
   both the CTest suite and the standalone report generator
2. WHEN documentation for this feature is written THEN the system SHALL
   record the specific machine/OS/compiler/build-type the reference numbers
   in the doc were captured on, since absolute (and often relative) numbers
   are not portable across environments
3. WHEN documentation for this feature is written THEN the system SHALL
   call out known structural asymmetries between the backends that are
   expected to show up in results (e.g. `stdexec`'s cross-thread
   fulfillment path taking a mutex in `single_shot_channel`, per
   `.kiro/specs/stdexec-future-backend/spike-notes.md` finding 3; GCC's
   `-fno-strict-aliasing` mitigation already applied to
   `stdexec`-dependent targets per the same spike notes) so a reader does
   not mistake a known, already-understood cost for a new discovery
4. WHEN documentation for this feature is written THEN the system SHALL
   state explicitly that this spec does not change
   `KYTHIRA_DEFAULT_FUTURE_BACKEND` and does not recommend a default —
   it documents evidence for a future, separate decision

### Requirement 7

**User Story:** As a library maintainer, I want the benchmark suite itself
validated for methodological soundness, so that its own bugs don't produce
misleading numbers that get trusted as fact.

#### Acceptance Criteria

1. WHEN the harness is evaluated THEN the system SHALL provide a
   self-check confirming that a scenario run twice in immediate succession
   (same backend, same parameters) produces throughput/latency results
   within a documented tolerance of each other, catching gross
   non-determinism in the harness itself (e.g. warm-up not actually
   excluded, timer misuse)
2. WHEN a benchmark scenario's timed region is reviewed THEN the system
   SHALL confirm it excludes one-time setup (future/promise/scheduler
   construction outside the measured operation) and dead-code elimination
   risk per Requirement 3.4
3. WHEN the harness is evaluated against the concepts in
   `include/concepts/future.hpp` THEN the system SHALL confirm the
   backend-generic scenario templates compile against both backends
   without any `#ifdef`-based branching inside a scenario's timed body

## Out of Scope

- Changing either backend's implementation to improve its measured numbers.
- Changing `KYTHIRA_DEFAULT_FUTURE_BACKEND` or recommending a new default.
- GPU execution (`nvexec`) or any non-CPU `stdexec` extension.
- Memory/allocation profiling (heap profiling tools like `valgrind --massif`
  are already documented in `doc/raft_performance_benchmarking.md` and are
  not duplicated here); this spec measures wall-clock time only.
- Continuous historical tracking/dashboards of benchmark results over time;
  this spec produces one comparison report per run, not a trend system.
