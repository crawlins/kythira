# Implementation Plan — Boost Future Backend

## Status: Not Started

**Last Updated**: July 23, 2026

## Overview

Adds a third `kythira::Future`/`Promise`/`Try`/`Executor` implementation,
backed by `boost::thread`'s extended future API (`then()`/`when_all`/
`when_any`, gated behind `BOOST_THREAD_VERSION=4`), alongside the existing
Folly (default) and `stdexec` backends. `boost-thread` is already a
required vcpkg dependency and already linked into `network_simulator` —
this spec is the first to actually use `boost::future`/`boost::promise`
rather than just other Boost.Thread facilities.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [0],
      "description": "Spike: confirm the exact enabled Boost.Thread extension surface before committing to adaptor code — everything else depends on this"
    },
    {
      "wave": 2,
      "tasks": [1],
      "description": "BOOST_THREAD_VERSION project-wide wiring (CMake) — needed before any code using then()/when_all/when_any can compile"
    },
    {
      "wave": 3,
      "tasks": [2, 3],
      "description": "Try<T> and SemiPromise<T>/Promise<T> — no dependency on continuations, can proceed in parallel with each other"
    },
    {
      "wave": 4,
      "tasks": [4, 5, 6],
      "description": "Future<T> core (get/isReady/wait) depends on wave 3's Promise::getFuture(); thenValue/thenError/ensure adaptor and via() both depend on Future<T> core existing first"
    },
    {
      "wave": 5,
      "tasks": [7, 8],
      "description": "timer_service, then delay()/within() built on it — depends on wave 4's Future<T> core"
    },
    {
      "wave": 6,
      "tasks": [9, 10],
      "description": "FutureFactory (depends on wave 4's Future<T> core only) and FutureCollector (depends on wave 4 and, for collectAnyWithoutException/collectN, the same loop pattern) — independent of each other"
    },
    {
      "wave": 7,
      "tasks": [11],
      "description": "Backend selection wiring (KYTHIRA_DEFAULT_FUTURE_BACKEND, future_default.hpp) — depends on every wrapper type existing"
    },
    {
      "wave": 8,
      "tasks": [12, 13, 14, 15, 16],
      "description": "Concept-compliance static_asserts and the full test suite — depends on all implementation waves; individual test files are independent of each other"
    },
    {
      "wave": 9,
      "tasks": [17, 18],
      "description": "Documentation and migration guide — depends on the implementation being stable enough to demonstrate"
    }
  ]
}
```

## Tasks

## Phase 0: Spike (Task 0)

- [ ] 0. Spike: confirm Boost.Thread extension API surface before committing to design details
  - Confirm `BOOST_THREAD_VERSION=4` alone (no other macro) enables
    `then()`, `when_all`, `when_any`, and the executor-taking
    `then(Ex&, F)` overload against the actually-installed vcpkg
    `boost-thread` version — not just the source inspection already
    recorded in design.md
  - Confirm `then()`'s callback receives the completed `boost::future<T>`
    itself, via a throwaway compile, not documentation alone
  - Confirm `boost::when_all`'s iterator-pair (runtime-sized) overload is
    present and usable over `std::vector<boost::future<T>>`, distinct from
    the fixed-arity variadic overload
  - Record the exact Boost release/commit and minimum compiler versions
    validated
  - Record findings in `spike-notes.md` in this spec directory
  - _Requirements: 9.1, 9.2, 9.3, 9.4_

## Phase 1: BOOST_THREAD_VERSION Wiring (Task 1)

- [ ] 1. Wire `BOOST_THREAD_VERSION=4` as a project-wide `INTERFACE` compile definition
  - Add `KYTHIRA_BUILD_BOOST_FUTURE_BACKEND` CMake option (default `OFF`)
  - `target_compile_definitions(network_simulator INTERFACE
    BOOST_THREAD_VERSION=4 KYTHIRA_HAS_BOOST_FUTURE)` guarded on
    `KYTHIRA_BUILD_BOOST_FUTURE_BACKEND OR KYTHIRA_DEFAULT_FUTURE_BACKEND
    STREQUAL "boost"` — mirroring exactly how `KYTHIRA_HAS_STDEXEC` is
    applied today
  - Confirm (build both ways) that with the option `OFF`, nothing in the
    existing codebase that already transitively includes any
    `<boost/thread/...>` header changes behavior — `BOOST_THREAD_VERSION`
    stays undefined (Boost's own default, `2`) exactly as before this task
  - _Requirements: 1.1, 1.3, 1.4_

## Phase 2: `Try<T>` and `SemiPromise<T>`/`Promise<T>` (Tasks 2-3)

- [ ] 2. Implement `kythira::boost_backend::Try<T>`
  - New header `include/raft/future_boost.hpp`, `namespace
    kythira::boost_backend`
  - Construct from a ready `boost::future<T>`; `hasValue()`/
    `hasException()`/`value()`/`exception()` per design.md's Phase 2 —
    `exception()` uses `get_exception_ptr()`, not a throw/catch round trip
  - `static_assert` against the `try_type` concept for `int`, `void`,
    `std::string`, and a custom struct (same matrix as the existing two
    backends)
  - _Requirements: 2.1, 2.2, 2.3, 2.4_

- [ ] 3. Implement `kythira::boost_backend::SemiPromise<T>`/`Promise<T>`
  - Direct wrap of `boost::promise<T>` — `setValue`/`setException`
    forward to `set_value`/`set_exception`; `isFulfilled()` tracked via a
    wrapper-owned flag set alongside each call (`boost::promise` has no
    direct query)
  - `getFuture()`/`getSemiFuture()` both return the same `Future<T>` (no
    separate semi-future type needed, matching what the `semi_promise`
    concept already allows)
  - Verify double-fulfillment surfaces as a `std::exception_ptr`-carried
    error (via `boost::promise_already_satisfied`) and a
    destroyed-before-fulfilled-with-retrieved-future promise leaves its
    future ready with a `boost::broken_promise`-derived error
  - `static_assert` against `semi_promise` and `promise` for the same type
    matrix
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5_

## Phase 3: `Future<T>` Core and Continuations (Tasks 4-6)

- [ ] 4. Implement `kythira::boost_backend::Future<T>` core
  - `get()` (via `boost::future<T>::get()`), `isReady()` (via
    `is_ready()`), `wait(timeout)` (via `wait_for()`, converting
    `std::chrono::milliseconds` to `boost::chrono::milliseconds` at the
    call boundary)
  - `static_assert` against the `future` concept for the same type matrix
  - _Requirements: 4.1, 4.2, 4.3, 4.4_

- [ ] 5. Implement `thenValue`/`thenError`/`ensure` continuation adaptors
  - One shared internal adaptor-building helper (per design.md's "then()
    Callback Adaptation" section) used by all three, rather than three
    independent `then()` calls stacked — installs a closure over
    `boost::future<T>::then()` that unwraps the completed future
    (`.get()`, rethrowing on the value path; `.get_exception_ptr()` on the
    error path) before calling the user's callback
  - Explicit flattening: if the user's callback returns `Future<U>`, the
    adaptor SHALL NOT return `Future<Future<U>>` — chain a second `.then()`
    internally to unwrap one level, mirroring the Folly backend's existing
    flattening behavior
  - `ensure`'s callback runs on both the value and error paths
  - _Requirements: 6.1, 6.2, 6.3, 6.6_

- [ ] 6. Implement `via(executor)`
  - Uses `then(Ex&, F)`, requiring `BOOST_THREAD_PROVIDES_EXECUTORS`
    (confirmed auto-enabled in Task 0's spike, or defined explicitly per
    Requirement 1.2's fallback)
  - Accepts `boost::executors::executor&` — document in the header comment
    that this is Boost's own executor abstraction, distinct in shape from
    `folly::Executor` (the existing `executor` concept is not extended to
    cover it, matching `stdexec-future-backend`'s Requirement 3 precedent
    of leaving `executor`/`keep_alive` Folly-shaped)
  - _Requirements: 6.4_

## Phase 4: Timer-Backed `delay`/`within` (Tasks 7-8)

- [ ] 7. Implement `timer_service` (Meyers singleton, `boost::asio`-backed)
  - One shared `io_context` + background thread + `executor_work_guard`
    for the whole process, not one thread per `delay()`/`within()` call
  - `schedule_after(duration, callback)` posts a `steady_timer` expiry
    onto the shared `io_context`
  - Clean shutdown at static-destruction time (release the work guard,
    join the thread) — verified with a test that constructs/destroys many
    `delay()`/`within()` calls and confirms no thread leak
  - _Requirements: 6.5_

- [ ] 8. Implement `delay(duration)` and `within(timeout)`
  - `delay`: fulfills a new `Promise<T>` with the original future's
    eventual result once both (a) the original future completes and (b)
    at least `duration` has elapsed, whichever resolves order — value/error
    passed through unchanged, only the *timing* of readiness is delayed
  - `within`: races the original future's completion against a
    `timer_service` expiry; whichever fulfills the shared `Promise<T>`
    first wins (exactly-once, per Phase 2's `Promise` semantics); the
    loser's continuation (if it fires at all) is a no-op via a `weak_ptr`
    to the shared state, not a second fulfillment attempt
  - _Requirements: 6.5_

## Phase 5: `FutureFactory`/`FutureCollector` (Tasks 9-10)

- [ ] 9. Implement `kythira::boost_backend::FutureFactory`
  - `makeFuture(value)` → `boost::make_ready_future(value)`;
    `makeExceptionalFuture<T>(ex)` → `boost::make_exceptional_future<T>(ex)`;
    `makeReadyFuture()` → `boost::make_ready_future()`'s `void` overload,
    converted to `kythira::unit` at the wrapper boundary
  - `static_assert` against `future_factory`
  - _Requirements: 5.1, 5.2, 5.3, 5.4_

- [ ] 10. Implement `kythira::boost_backend::FutureCollector`
  - `collectAll` → `boost::when_all` (iterator-pair overload, confirmed in
    Task 0), converted to `std::vector<Try<T>>` preserving input order
  - `collectAny` → `boost::when_any`, scanning the result for the ready
    member, converted to `(index, Try<T>)`
  - `collectAnyWithoutException` → `collectAny` in a loop over the
    shrinking remainder, discarding failures, per design.md's Phase 5
  - `collectN` → repeated `collectAny`-style selection removing each
    selected future from the pool, per design.md's Phase 5
  - `static_assert` against `future_collector`
  - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5_

## Phase 6: Backend Selection Wiring (Task 11)

- [ ] 11. Extend `KYTHIRA_DEFAULT_FUTURE_BACKEND` and `future_default.hpp` for `boost`
  - `set_property(CACHE KYTHIRA_DEFAULT_FUTURE_BACKEND PROPERTY STRINGS
    folly stdexec boost)`, extend the `FATAL_ERROR` validation to accept
    all three
  - `KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "boost"` also forces
    `KYTHIRA_BUILD_BOOST_FUTURE_BACKEND` on and defines
    `KYTHIRA_FUTURE_BACKEND_BOOST`
  - `include/raft/future_default.hpp`: add the `#elif
    defined(KYTHIRA_FUTURE_BACKEND_BOOST)` branch resolving
    `kythira::future_default<T>` to `kythira::boost_backend::Future<T>`
  - Confirm switching `KYTHIRA_DEFAULT_FUTURE_BACKEND` to/from `boost`
    requires no change to any templated core code (spot-check against at
    least one existing generic call site already exercised by the
    `stdexec` switch)
  - _Requirements: 8.3, 8.4, 8.6_

## Phase 7: Testing (Tasks 12-16)

- [ ] 12. Write `boost` concept-compliance test suite
  - `tests/boost_try_concept_compliance_property_test.cpp`,
    `tests/boost_semi_promise_concept_compliance_property_test.cpp`,
    `tests/boost_promise_concept_compliance_property_test.cpp` (folded
    together if the semi_promise/promise split is thin, matching however
    the `stdexec` suite ultimately organized this),
    `tests/boost_future_concept_compliance_property_test.cpp` — mirroring
    the existing Folly/`stdexec` suites' structure and property-test
    tagging convention
  - _Requirements: 2.4, 3.5, 4.4, 10.1, 10.2, 10.3, 10.5_

- [ ] 13. Write `tests/boost_future_collector_property_test.cpp`
  - **Property 10: Collective Operation Fidelity**
  - **Validates: Requirements 7.1, 7.2, 7.3, 7.4, 10.4**
  - `collectAll` with a mix of successes/failures preserves order and
    per-item `Try<T>` fidelity; `collectAny`/`collectAnyWithoutException`/
    `collectN` return correct indices and results across varied completion
    orderings

- [ ] 14. Write `tests/boost_future_continuation_property_test.cpp`
  - **Property 6: Continuation Callback Unwrapping**
  - **Property 7: Continuation Flattening**
  - **Validates: Requirements 6.1, 6.2, 6.3, 6.6**
  - Confirms `thenValue`/`thenError`/`ensure` present plain
    values/`std::exception_ptr` to callbacks (never a raw
    `boost::future<T>`), and that a `Future<U>`-returning `thenValue`
    callback flattens correctly

- [ ] 15. Write `tests/boost_future_delay_within_property_test.cpp`
  - **Property 8: Delay/Within Correctness**
  - **Validates: Requirement 6.5**
  - Highest-iteration-count suite given this is the newest, highest-risk
    code on this backend: `delay` never completes early, `within` racing a
    slow future produces a timeout error, `within` racing a fast future
    passes the real result through unchanged, repeated delay/within calls
    do not leak the shared `timer_service` thread (verified via thread
    count before/after a loop of many calls)

- [ ] 16. Extend `tests/backend_non_interference_compile_fail_test.cpp`
  - Add `boost_backend`-vs-Folly and `boost_backend`-vs-`stdexec_backend`
    `static_assert(!requires{...})` cases alongside the existing
    `stdexec_backend`-vs-Folly ones — same file, not a new one
  - **Property 11: Backend Non-Interference**
  - **Validates: Requirement 8.5**

## Phase 8: Documentation (Tasks 17-18)

- [ ] 17. Write `spike-notes.md`'s final form and header-comment risk disclosure
  - Finalize `spike-notes.md` with Task 0's findings plus anything
    discovered during implementation that changed assumptions
  - Add a top-of-file comment in `include/raft/future_boost.hpp`
    explicitly flagging `then()`/`when_all`/`when_any` as Boost's own
    `// EXTENSION`-labeled surface, not standard `std::future` behavior,
    and the accepted maintenance risk this represents
  - _Requirements: 9.1, 9.4, 11.5_

- [ ] 18. Write migration guide entry and update `doc/TODO.md`
  - Extend `examples/migration_guide_example.cpp` (or add a sibling file,
    matching however the `stdexec` migration content was structured) with
    Folly-backend vs. `boost`-backend code side by side for the same
    representative operations (a `thenValue` chain, a `collectAll`, a
    `within` timeout)
  - State explicitly (in the migration guide and/or this spec's own
    README-equivalent) that no production call site is converted, neither
    Folly nor `stdexec` is removed, and `boost::fibers`/Boost.Cobalt are
    out of scope
  - Update `doc/TODO.md`'s Pending Specifications table once this spec
    reaches completion, and add a `doc/CHANGELOG.md` entry, matching this
    project's established documentation-update convention for completed
    specs
  - _Requirements: 11.1, 11.2, 11.3, 11.4_

## Notes

- Depends on nothing landing first — unlike `discovery-nodes-host-build`'s
  dependency on `chaos-node-host-build`, this spec's prerequisite
  (backend-neutral `include/concepts/future.hpp`) already landed as part
  of `.kiro/specs/stdexec-future-backend/` Phase 1 and needs no further
  change.
- No new vcpkg dependency: `boost-thread` and `boost-asio` are both
  already required dependencies of this project (`vcpkg.json`) for
  reasons unrelated to this spec. This spec is the first to actually use
  `boost::future`/`boost::promise`, and the first to use `boost::asio`
  for anything future-related, but adds no new external dependency.
- `include/concepts/future.hpp` itself is not modified by this spec — it
  is already backend-neutral. Any future concept change should be
  validated against all three backends, not just the two that existed
  when that change is made.
- `tests/docker_chaos/CMakeLists.txt`, every compose file, and
  `docker/bind9/Dockerfile` are unaffected — this spec is header/library-
  only, with no container surface, mirroring
  `.kiro/specs/stdexec-future-backend/` Requirement 4.5's identical
  scoping note.
