# Future Backend Performance Comparison

**Last Updated**: July 13, 2026

## Overview

This guide documents the benchmark suite that compares the two `kythira`
future implementations:

- **Folly backend** — `kythira::Future<T>` (`include/raft/future.hpp`), the
  project's default.
- **stdexec backend** — `kythira::stdexec_backend::Future<T>`
  (`include/raft/future_stdexec.hpp`), an optional, sender/receiver-based
  alternative (see `.kiro/specs/stdexec-future-backend/`).

The suite exists to characterize performance, not to pick a winner: it
**does not** change `KYTHIRA_DEFAULT_FUTURE_BACKEND` (still `folly`) and
makes no recommendation to change it. It touches no existing backend code.

Every scenario in the catalog below is implemented exactly once, as a
function template parameterized on a small `Traits` type
(`examples/future-backend-benchmark/benchmark_harness.hpp`), instantiated
once per backend. This is a deliberate design choice: a hand-duplicated
"equivalent" scenario per backend risks silently drifting (different
iteration counts, a `get()` in one and a `wait()` in the other) until the
two backends' numbers are no longer describing the same operation. See
`.kiro/specs/future-backend-performance-benchmark/design.md` for the full
rationale and architecture.

## Quick Start

```bash
# Build with optimizations
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target future_backend_benchmark_test future_backend_benchmark_report

# Run the CI-fast regression suite (hardware-independent sanity floors only)
cd build
ctest -R future_backend_benchmark_test --output-on-failure
# or, to run it alongside the pre-existing Raft performance benchmarks:
ctest -L benchmark --output-on-failure

# Run the full, report-quality comparison generator (developer-run, not
# CTest-registered)
./examples/future_backend_benchmark_report
./examples/future_backend_benchmark_report --iterations 20000 --warmup 500
```

The report generator writes a timestamped CSV and JSON artifact to
`test_results/future_backend_benchmark_<timestamp>.{csv,json}`, following
this project's existing `test_results/*_<timestamp>.*` convention.

If `stdexec` was not found at configure time
(`vcpkg-overlays/stdexec/README.md`), both the CTest suite and the report
generator still build and run — the report simply omits the `stdexec` rows
and the delta column entirely rather than printing empty or zeroed
placeholders. No target disappears except the concept-specific `stdexec`-only
test suites elsewhere in this project.

## Scenario Catalog

Each scenario is one function template in `benchmark_harness.hpp`,
constrained by a `future_backend_traits` concept over this project's
backend-neutral concepts (`include/concepts/future.hpp`):

| Scenario | Kind | What it measures |
|---|---|---|
| `bench_creation_resolution` (int / string / struct) | throughput | `FutureFactory::makeFuture` + `get()` round trip for three payload shapes |
| `bench_same_thread_promise` | throughput | `Promise::getFuture()` → `setValue()` → `get()`, all on the calling thread |
| `bench_cross_thread_promise` | latency (P50/P95/P99) | Same round trip, but `setValue()` runs on a spawned `std::thread` — isolates the cross-thread wake-up path |
| `bench_thenvalue_chain` (depth 1 / 5 / 20) | throughput | `thenValue` chain construction + end-to-end resolution at three depths |
| `bench_thenerror` | throughput | Exceptional future construction + `thenError` recovery |
| `bench_via_scheduler` | throughput | `via(scheduler)` continuation hop, using a scheduler warmed once outside the timed region |
| `bench_collect_all` (N = 10 / 100 / 1000) | throughput | `FutureCollector::collectAll` fan-in at three widths |
| `bench_collect_any` | latency (P50/P95/P99) | `FutureCollector::collectAny` with one future arranged to resolve first |
| `bench_delay_within` (delay / within) | latency (P50/P95/P99, overhead) | Scheduling overhead of `delay()`/`within()` — elapsed time **minus** the nominal delay, not the delay duration itself |

No scenario gap was found: as of this spec's implementation, both backends
already implement the full concept set needed for every scenario above
(confirmed by the `stdexec-future-backend` spec's own completed property
suite), so Requirement 2.10's gap-documentation clause is a no-op here.

## Throughput vs. Latency Results

Most scenarios report **throughput** (`ops_per_second`, computed over the
whole measured run) because their cost is dominated by CPU-bound work with
low run-to-run variance. Three scenarios — `bench_cross_thread_promise`,
`bench_collect_any`, and `bench_delay_within` — instead report **P50/P95/P99
latency** per iteration, because they involve real OS-level waits
(thread spawn/join, a timer race) whose distribution shape matters more than
its mean. Percentiles use nearest-rank over sorted samples (the same
convention `doc/raft_performance_benchmarking.md` already uses), not a
fitted distribution.

Warm-up iterations (`BenchConfig::warmup_iterations`, run before the timed
region starts) are always excluded from both throughput and latency
figures.

## Known Structural Asymmetries

These are real, understood differences in how the two backends are built,
not benchmark bugs — call them out explicitly so a reader doesn't mistake
them for undocumented anomalies:

1. **`bench_cross_thread_promise`'s absolute latency includes a real
   `std::thread` spawn and join per iteration**, deliberately: the scenario
   exists specifically to exercise the cross-thread wake-up path (the
   stdexec backend's `single_shot_channel` uses a `std::mutex`; the Folly
   backend uses `folly::Promise`'s own internal core). Thread-spawn cost is
   common to both backends' samples, so it inflates both backends' absolute
   numbers roughly equally and washes out of the *relative* comparison, but
   it does mean neither backend's absolute per-iteration latency here is a
   pure measure of promise-fulfillment cost alone.
2. **GCC 13 at `-O2`/`-O3` requires `-fno-strict-aliasing`** on any target
   that links `STDEXEC::stdexec` (root `CMakeLists.txt`, applied to the
   `network_simulator` INTERFACE target and inherited transitively by both
   `future_backend_benchmark_test` and `future_backend_benchmark_report`).
   This works around a real miscompilation of `exec::any_sender`'s
   small-buffer-optimized move constructor, found and diagnosed during the
   `stdexec-future-backend` spec's Phase 3
   (`.kiro/specs/stdexec-future-backend/spike-notes.md`). It has no
   measurable effect on the numbers below beyond making the stdexec-backend
   binary correct in the first place.
3. **`within_overhead` measures as ~0 for both backends in the reference
   run below.** This is a real result, not a suppressed error: the
   original future in this scenario is already resolved before `within()`'s
   race even starts, so the "loser" is almost always the timer, and the
   measured overhead (elapsed time minus the nominal delay) reflects only
   the synchronous fast path, not a timeout ever actually firing.

## Reference Machine and Results

**Environment**:
- CPU: Intel(R) Core(TM) i5-6300U @ 2.40GHz (4 logical cores)
- OS: Ubuntu 24.04 (kernel 6.8.0-134-generic), x86_64
- Compiler: GCC 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1)
- Build type: Release (`-O2`/`-O3`)
- `stdexec` available (vcpkg overlay port; see `vcpkg-overlays/stdexec/README.md`)
- Report run with default `BenchConfig` (`--iterations`/`--warmup` not
  passed: 100,000 measured / 1,000 warm-up iterations for throughput
  scenarios; 200 measured / up to 200 warm-up for the three thread/timer-bound
  latency scenarios)

**Results** (`./build/examples/future_backend_benchmark_report`, artifacts
at `test_results/future_backend_benchmark_20260713_210814.{csv,json}`,
CTest run captured at
`test_results/future_backend_benchmark_ctest_20260713_210900.txt`):

```
Scenario                      Backend   Ops/sec           P50(ns)     P95(ns)     P99(ns)     Notes
------------------------------------------------------------------------------------------------------------------------
creation_resolution_int       folly     11332992          0           0           0
creation_resolution_string    folly     5600167           0           0           0
creation_resolution_struct    folly     9071253           0           0           0
same_thread_promise           folly     8497141           0           0           0
thenvalue_chain_depth1        folly     4397340           0           0           0
thenvalue_chain_depth5        folly     1007161           0           0           0
thenvalue_chain_depth20       folly     283912            0           0           0
thenerror                     folly     2693348           0           0           0
via_scheduler                 folly     11889110          0           0           0
collect_all_n10               folly     416266            0           0           0
collect_all_n100              folly     56755             0           0           0
collect_all_n1000             folly     3818              0           0           0
cross_thread_promise          folly     21353             39900       67693       130658      includes per-iteration std::thread spawn cost, common to both backends
collect_any_n100              folly     119927            7541        13224       22088
delay_overhead                folly     5700              132050      365825      541121      value is scheduling overhead atop the nominal delay, not the delay duration itself
within_overhead               folly     0                 0           0           0           value is scheduling overhead atop the nominal delay, not the delay duration itself
creation_resolution_int       stdexec   7123798           0           0           0
creation_resolution_string    stdexec   4007291           0           0           0
creation_resolution_struct    stdexec   3363852           0           0           0
same_thread_promise           stdexec   3140824           0           0           0
thenvalue_chain_depth1        stdexec   2701860           0           0           0
thenvalue_chain_depth5        stdexec   1057502           0           0           0
thenvalue_chain_depth20       stdexec   229132            0           0           0
thenerror                     stdexec   1838607           0           0           0
via_scheduler                 stdexec   258869            0           0           0
collect_all_n10               stdexec   165341            0           0           0
collect_all_n100              stdexec   20390             0           0           0
collect_all_n1000             stdexec   2084              0           0           0
cross_thread_promise          stdexec   22678             32834       71701       223121      includes per-iteration std::thread spawn cost, common to both backends
collect_any_n100              stdexec   6165              157350      237096      247256
delay_overhead                stdexec   5594              136207      372889      649265      value is scheduling overhead atop the nominal delay, not the delay duration itself
within_overhead               stdexec   0                 0           0           0           value is scheduling overhead atop the nominal delay, not the delay duration itself
------------------------------------------------------------------------------------------------------------------------

=== Delta (stdexec vs. folly, ops/sec) ===
creation_resolution_int       -37.1%
creation_resolution_string    -28.4%
creation_resolution_struct    -62.9%
same_thread_promise           -63.0%
thenvalue_chain_depth1        -38.6%
thenvalue_chain_depth5        +5.0%
thenvalue_chain_depth20       -19.3%
thenerror                     -31.7%
via_scheduler                 -97.8%
collect_all_n10               -60.3%
collect_all_n100              -64.1%
collect_all_n1000             -45.4%
cross_thread_promise          +6.2%
collect_any_n100              -94.9%
delay_overhead                -1.9%
within_overhead               +0.0%
```

### Reading these numbers

- **Folly is faster on most throughput scenarios** in this single run, most
  notably `via_scheduler` (-97.8%) and `collect_any_n100` (-94.9%). This is
  a plausible reflection of the stdexec backend's `via()` needing to hop
  through its own `scheduler_handle` type erasure and `collectAny` needing
  a shared-state/channel bookkeeping path (see
  `include/raft/future_stdexec.hpp`'s `FutureCollector` comments) rather
  than Folly's more direct executor model — but this is a single run on a
  4-core laptop-class CPU under normal desktop load, not a controlled,
  repeated-trial statistical comparison. Treat the *sign and rough
  magnitude* of these deltas as informative, not the precise percentage.
- **`cross_thread_promise` and `thenvalue_chain_depth5`/`delay_overhead`
  are roughly at parity** (within a few percent), consistent with those
  paths being dominated by cost common to both backends (thread spawn,
  timer scheduling) rather than backend-specific overhead.
- This suite makes **no CI-enforced cross-backend assertion** — see
  `tests/future_backend_benchmark_test.cpp`, whose only assertions are
  per-backend sanity floors (Requirement 4.3/4.4) — precisely so numbers
  like these can vary between machines and runs without breaking a build.

## Running the CTest Regression Suite

`tests/future_backend_benchmark_test.cpp` is a single Boost.Test binary
covering both backends (guarded internally with
`#if defined(KYTHIRA_HAS_STDEXEC)` for the stdexec-only cases), registered
via `add_network_test` with `LABELS "performance;benchmark;future-backend"`:

```bash
ctest -R future_backend_benchmark_test --output-on-failure
ctest -L future-backend --output-on-failure   # same suite, by label
ctest -L benchmark --output-on-failure         # this suite + Raft performance benchmarks
```

It uses much smaller iteration counts than the report generator (routine-CI
friendly — the full suite completes in a few seconds) and asserts only
hardware-independent sanity floors, one per backend per scenario, plus a
`harness_repeated_run_stability` self-check that validates the *harness*
(not either backend) by running one throughput scenario twice back-to-back
and checking the two results land within 25% of each other.

## Out of Scope

Per `.kiro/specs/future-backend-performance-benchmark/requirements.md`,
this spec does not: change either backend's implementation, change
`KYTHIRA_DEFAULT_FUTURE_BACKEND` (still `folly`) or recommend a new
default, cover GPU/`nvexec` scheduling, profile memory/allocation
behavior, or set up continuous historical performance tracking. It answers
"how do these two backends currently compare on this catalog of
operations," nothing more.

## See Also

- [Raft Performance Benchmarking Guide](raft_performance_benchmarking.md)
- `.kiro/specs/future-backend-performance-benchmark/` (requirements, design, tasks)
- `.kiro/specs/stdexec-future-backend/` (the stdexec backend itself)
- `include/raft/future_stdexec_README.md` (stdexec backend scope boundary)
