# Implementation Plan

**Status (as of 2026-07-12)**: Phases 0–2 (Tasks 0–13) are implemented and
verified on the `feat/stdexec-future-backend` branch (commits `dd44707`,
`6ea1224`, `63caec5`) — not yet merged into `main`. Phases 3–6 (Tasks 14–35:
factory/continuations/collectors, executor shim, backend selection,
cross-backend test parity, documentation) have not been started on any
branch. Four sub-tasks within Phases 0–2 were not completed as distinct
deliverables — see the inline notes below — and are left unchecked
accordingly: **1.1** (optional-dependency-isolation CI/CMake check), the
second bullet of **5** (standalone-compile check was verified ad hoc but
never added as a persistent CMake/CTest target), **5.1**, and **6.1**/**6.2**
(no dedicated property-test files exist for Properties 1–3; the existing
regression suite incidentally exercises the same ground but was not written
to validate these properties specifically).

## Phase 0: Spike and Dependency Setup

- [x] 0. Spike: confirm stdexec API surface assumptions before committing to Phase 2 design details
  - Vendor a throwaway `stdexec` checkout and confirm: (a) minimum GCC/Clang versions that compile it cleanly against this project's C++23 setting, (b) whether `exec::when_any` or an equivalent first-completed-wins combinator exists, (c) whether a timed scheduler (`schedule_after`/`schedule_at`) ships in the vendored version, (d) whether an existing single-shot/manual-reset-event sender primitive exists in `exec` that could replace the hand-rolled `single_shot_channel`
  - Record findings in a short note in this spec directory (`spike-notes.md`) since design.md's Phase 2 section explicitly defers these questions to this spike
  - _Requirements: 13.5_

- [x] 1. Add stdexec as an optional vcpkg dependency
  - Add `stdexec` to `vcpkg.json` dependencies
  - Add `find_package(stdexec CONFIG QUIET)` to root `CMakeLists.txt`, mirroring the existing `find_package(folly QUIET)` pattern
  - Emit a warning (not an error) when `stdexec` is not found, matching the existing Folly-not-found warning
  - Gate all new `stdexec`-backend targets behind `if(stdexec_FOUND)`
  - _Requirements: 4.1, 4.2, 4.3, 4.4_

- [ ] 1.1 Write property test for optional dependency isolation
  - **Property 5: Optional Dependency Isolation**
  - **Validates: Requirements 4.2, 4.3**
  - Implemented as a CI/CMake-level check (configure with a build directory that has `stdexec` hidden from `find_package`, verify the rest of the project still configures and builds) rather than a C++ test

## Phase 1: Regenericize the Concept Layer

- [x] 2. Introduce `kythira::unit` in the concept layer
  - Add `kythira::unit` (trivial, default-constructible, `operator==`) to `include/concepts/future.hpp`
  - Do not remove `folly::Unit` usage from `include/raft/future.hpp` yet — this task only adds the new type
  - _Requirements: 2.1_

- [x] 3. Replace `folly::exception_wrapper` with `std::exception_ptr` in concept signatures
  - Update `try_type` concept's exception-access requirement
  - Update `semi_promise` concept's `setException` requirement
  - Update `future_factory` concept's `makeExceptionalFuture` requirement
  - Remove the `#include <folly/ExceptionWrapper.h>` from `include/concepts/future.hpp` once no concept references it
  - _Requirements: 1.1, 1.2_

- [x] 4. Replace `folly::Unit` with `kythira::unit` in concept signatures
  - Update `semi_promise` concept's void-specialization `setValue` requirement
  - Update `future_factory` concept's `makeReadyFuture` requirement
  - Remove the `#include <folly/Unit.h>` from `include/concepts/future.hpp` once no concept references it
  - _Requirements: 1.3, 2.1_

- [ ] 5. Verify concept-layer Folly independence
  - Compile `include/concepts/future.hpp` in isolation (a standalone translation unit including only this header) and confirm no Folly header is transitively included — **done** (verified ad hoc during Phase 1; commit `6ea1224`)
  - Add this standalone-compile check as a CMake/CTest target so regressions are caught automatically — **not done**, no such target exists in `tests/CMakeLists.txt`
  - _Requirements: 1.4_

- [ ] 5.1 Write property test for concept-layer Folly independence
  - **Property 2: Concept-Layer Folly Independence**
  - **Validates: Requirement 1.4**

- [x] 6. Verify the existing Folly backend still satisfies the regenericized concepts
  - Recompile `include/raft/future.hpp` against the updated `include/concepts/future.hpp`
  - Confirm the existing `static_assert` block at the end of `include/raft/future.hpp` still passes unmodified
  - Confirm the existing `setException(const std::exception_ptr&)` overload on `SemiPromise`/`Promise` already satisfies the new concept requirement (it should, per design.md's analysis) — if it does not, adjust the Folly wrapper's overload set, not the concept
  - Run the full existing test suite via CTest and confirm zero regressions
  - _Requirements: 1.5, 2.4_

- [ ] 6.1 Write property test for concept regenericization preserving Folly compliance
  - **Property 1: Concept Regenericization Preserves Folly Compliance**
  - **Validates: Requirements 1.4, 1.5, 2.4**

- [ ] 6.2 Write property test for unit type equivalence
  - **Property 3: Unit Type Equivalence**
  - **Validates: Requirements 2.1, 2.2, 2.3, 2.4**

- [x] 7. Checkpoint — Phase 1 complete
  - Ensure all pre-existing tests still pass with the regenericized concepts before starting Phase 2
  - Ask the user if any concept change turned out to require touching `include/raft/future.hpp` beyond the `setException` overload check in Task 6

## Phase 2: stdexec-Backed Try, Single-Shot Channel, and Future

- [x] 8. Create `include/raft/future_stdexec.hpp` skeleton
  - Set up the `kythira::stdexec_backend` namespace
  - Add `#include <stdexec/execution.hpp>` and the `exec/` extension headers identified as needed by the Phase 0 spike
  - Forward-declare `Try`, `SemiPromise`, `Promise`, `Future`, `FutureFactory`, `FutureCollector`, `scheduler_executor_shim`
  - _Requirements: 4.4_

- [x] 9. Implement `into_try` and the `stdexec`-backed `Try<T>`
  - Implement `Try<T>` as a value/`std::exception_ptr` tagged union (or `std::variant`), independent of any sender
  - Implement `into_try(sender) -> Try<T>` via a receiver whose `set_value`/`set_error`/`set_stopped` populate a `Try<T>`, mapping "stopped" to `operation_cancelled` per design.md's Error Handling section
  - Add `static_assert` confirming `stdexec_backend::Try<T>` satisfies the regenericized `try_type` concept for `int`, `void`, `std::string`, and a custom struct
  - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.5_

- [x] 9.1 Write property test for stdexec Try fidelity
  - **Property 6: stdexec Try Fidelity**
  - **Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.5**

- [x] 10. Implement `single_shot_channel<T>` shared state and atomic state machine
  - Implement the empty/waiting/complete state machine from design.md using a single atomic compare-exchange for the fulfill-vs-connect race
  - Implement double-fulfillment detection (throws on the losing `set_value`/`set_error` call)
  - Implement broken-promise completion when the shared state is destroyed with no fulfillment but a receiver already started
  - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5_

- [x] 10.1 Write property test for single-shot channel exactly-once completion
  - **Property 7: Single-Shot Channel Exactly-Once Completion**
  - **Validates: Requirements 6.2, 6.3, 6.4**
  - Cover interleavings explicitly: fulfill-before-connect, connect-before-fulfill, concurrent fulfillment attempts from multiple threads (only one should win, the other should throw), high-iteration stress test for the race window

- [x] 10.2 Write property test for single-shot channel broken-promise semantics
  - **Property 8: Single-Shot Channel Broken-Promise Semantics**
  - **Validates: Requirement 6.5**

- [x] 11. Implement `stdexec`-backed `SemiPromise<T>`/`Promise<T>` over `single_shot_channel`
  - `SemiPromise<T>`: `setValue`/`setException`/`isFulfilled` delegating to the shared `single_shot_channel`
  - `Promise<T>`: adds `getFuture()`/`getSemiFuture()` returning a `Future<T>` wrapping the channel's sender
  - Add `static_assert` confirming compliance with regenericized `semi_promise` and `promise` concepts
  - _Requirements: 6.6_

- [x] 12. Implement `stdexec`-backed `Future<T>::get()`, `isReady()`, `wait(timeout)`
  - `get()` via `stdexec::sync_wait` (or the equivalent identified in the Phase 0 spike), rethrowing on error
  - `isReady()` via a non-blocking poll of the underlying `single_shot_channel` state where the future is backed by one; for factory-created (already-ready) futures, always `true`
  - `wait(timeout)` without leaking the operation state on timeout, per design.md's Broken Promise / Timeout Cleanup section
  - Add `static_assert` confirming compliance with the regenericized `future` concept
  - _Requirements: 7.1, 7.2, 7.3, 7.4_

- [x] 12.1 Write property test for stdexec future blocking get correctness
  - **Property 9: stdexec Future Blocking Get Correctness**
  - **Validates: Requirements 7.1, 7.2, 7.3**

- [x] 13. Checkpoint — Phase 2 core primitives complete
  - Ensure Try, single_shot_channel, SemiPromise, Promise, and basic Future (get/isReady/wait only, no continuations yet) all pass their tests before adding continuation/transformation/collection operations on top
  - This checkpoint exists because every later task in Phase 2/3 builds on `single_shot_channel`; catching a design flaw here is far cheaper than after continuations are layered on

## Phase 3: stdexec-Backed Factory, Continuations, Transformations, Collections

- [x] 14. Implement `stdexec`-backed `FutureFactory`
  - `makeFuture(value)` via `stdexec::just`
  - `makeExceptionalFuture<T>(ex)` via `stdexec::just_error`
  - `makeReadyFuture()` via `stdexec::just(kythira::unit{})`
  - Add `static_assert` confirming compliance with the regenericized `future_factory` concept
  - _Requirements: 8.1, 8.2, 8.3, 8.4_

- [x] 14.1 Write property test for factory operation fidelity
  - **Property 11: Factory Operation Fidelity**
  - **Validates: Requirements 8.1, 8.2, 8.3**

- [x] 15. Implement `thenValue`/`thenTry` on `stdexec`-backed `Future<T>`
  - Compose via `stdexec::then`
  - Implement automatic flattening when the callback returns a `Future<U>`, matching the Folly backend's existing flattening overloads
  - Handle both void and non-void future types
  - _Requirements: 9.1, 9.3_

- [x] 16. Implement `thenError` on `stdexec`-backed `Future<T>`
  - Compose via `stdexec` error-handling adaptors, normalizing the error channel to `std::exception_ptr` at the callback boundary
  - Implement automatic flattening when the callback returns a `Future<T>`
  - _Requirements: 9.2, 9.3_

- [x] 16.1 Write property test for continuation and transformation fidelity (thenValue/thenTry/thenError)
  - **Property 12: Continuation and Transformation Fidelity** (partial — thenValue/thenTry/thenError only; via/delay/within/ensure covered by Task 18.1)
  - **Validates: Requirements 9.1, 9.2, 9.3**

- [x] 17. Implement `via(scheduler)` and `ensure(func)` on `stdexec`-backed `Future<T>`
  - `via` using the `stdexec` transfer/`continue_on` adaptor identified in the Phase 0 spike
  - `ensure` guaranteeing execution on both value and error paths
  - _Requirements: 9.4, 9.6_

- [x] 18. Implement `delay(duration)` and `within(timeout)` on `stdexec`-backed `Future<T>`
  - Use the vendored `stdexec`/`exec` timed scheduler if the Phase 0 spike found one; otherwise implement a minimal timed single-shot channel reusing the Task 10 primitive
  - Confirm neither operation blocks a thread for the duration of the delay/timeout (this is the entire motivation for adopting a non-blocking model — regressing to a blocking sleep anywhere here defeats the purpose)
  - _Requirements: 9.5_

- [x] 18.1 Write property test for continuation and transformation fidelity (via/delay/within/ensure)
  - **Property 12: Continuation and Transformation Fidelity** (remainder)
  - **Validates: Requirements 9.4, 9.5, 9.6, 12.4**

- [x] 19. Implement `stdexec`-backed `FutureCollector::collectAll`
  - Compose via `stdexec::when_all` over `into_try`-wrapped input senders so individual failures don't cancel siblings
  - Preserve input ordering in the result vector
  - _Requirements: 10.1_

- [x] 20. Implement `stdexec`-backed `FutureCollector::collectAny` and `collectAnyWithoutException`
  - Use the vendored first-completed-wins combinator if the Phase 0 spike found one; otherwise implement using `single_shot_channel<std::pair<size_t, Try<T>>>` per design.md
  - `collectAnyWithoutException` continues past individual failures to find the first success
  - _Requirements: 10.2, 10.3_

- [x] 21. Implement `stdexec`-backed `FutureCollector::collectN`
  - Return the first N completed results with original indices
  - _Requirements: 10.4_

- [x] 21.1 Write property test for collective operation fidelity
  - **Property 13: Collective Operation Fidelity**
  - **Validates: Requirements 10.1, 10.2, 10.3, 10.4, 12.4**

- [x] 22. Add `static_assert` compliance checks for all Phase 3 components
  - Confirm `FutureCollector` satisfies the regenericized `future_collector` concept
  - Confirm `Future<T>` (with continuations) satisfies `future_continuation` and `future_transformable`
  - _Requirements: 8.4, 10.5_

- [x] 22.1 Write property test for cross-backend concept compliance
  - **Property 10: Cross-Backend Concept Compliance**
  - **Validates: Requirements 5.5, 6.6, 7.4, 7.5, 8.4, 10.5, 12.1**

- [x] 23. Checkpoint — Phase 3 complete, stdexec backend feature-complete
  - Ensure all stdexec backend tests pass before starting executor-shim and backend-selection work
  - Ask the user if any operation from the Folly backend's API surface was found to have no reasonable stdexec mapping

## Phase 4: Executor Compatibility Shim and Backend Selection

- [ ] 24. Implement `scheduler_executor_shim`
  - Wrap a `stdexec` scheduler, implement `.add(func)` by connecting/starting `stdexec::schedule(scheduler) | stdexec::then(func)` and blocking until it completes
  - Document (in a header comment) the overhead and semantic differences from native scheduler usage, per Requirement 3.4
  - Add `static_assert` confirming it satisfies the existing (non-regenericized) `executor` and `keep_alive` concepts
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5_

- [ ] 24.1 Write property test for executor/scheduler shim correctness
  - **Property 4: Executor/Scheduler Shim Correctness**
  - **Validates: Requirements 3.2, 3.4**

- [ ] 25. Add `KYTHIRA_DEFAULT_FUTURE_BACKEND` CMake option and `kythira::future_default` alias
  - Add the CMake option (values `folly`/`stdexec`, default `folly`) to root `CMakeLists.txt`
  - Add `include/raft/future_default.hpp` defining `kythira::future_default<T>` per the selected backend
  - Confirm switching the option requires no changes to templated core code
  - _Requirements: 11.1, 11.2, 11.3, 11.5_

- [ ] 25.1 Write property test for backend selection isolation
  - **Property 15: Backend Selection Isolation**
  - **Validates: Requirements 11.2, 11.5**

- [ ] 26. Add compile-fail checks for backend non-interference
  - Add `tests/backend_non_interference_compile_fail_test.cpp` with `static_assert(!requires{...})` checks verifying `stdexec_backend` types cannot be combined with Folly-backend-only types (e.g. `stdexec_backend::Future::via` given a Folly `Executor*`)
  - _Requirements: 11.4_

- [ ] 26.1 Write property test for backend non-interference
  - **Property 14: Backend Non-Interference**
  - **Validates: Requirement 11.4**

## Phase 5: Test Suite Parity and Cross-Backend Validation

- [ ] 27. Port the Folly concept-compliance test suite structure to stdexec
  - Create `tests/stdexec_future_concept_compliance_property_test.cpp`, `tests/stdexec_promise_concept_compliance_property_test.cpp`, `tests/stdexec_semi_promise_concept_compliance_property_test.cpp`, `tests/stdexec_executor_concept_compliance_property_test.cpp`, `tests/stdexec_concept_wrappers_unit_test.cpp`
  - Mirror the existing `tests/folly_*` files' scenarios so coverage is comparable, not just present
  - Use two-argument `BOOST_AUTO_TEST_CASE` with appropriate timeouts throughout, per `test-execution-standards.md`
  - _Requirements: 12.2, 12.3_

- [ ] 28. Add cross-backend fidelity tests
  - Create `tests/stdexec_concept_wrappers_interoperability_property_test.cpp` running the same operation (value, exception, timeout, collection scenarios) through both backends and asserting equivalent externally observable results
  - _Requirements: 12.4_

- [ ] 29. Register all new test targets with CTest, labeled appropriately
  - Add `add_test` entries for every new test file
  - Apply `stdexec` and `future-backend` labels so `ctest -L stdexec` / `ctest -LE stdexec` work
  - Gate all new test targets behind `if(stdexec_FOUND)` in CMakeLists.txt
  - _Requirements: 12.5, 4.3_

- [ ] 30. Run full test suite and store results per project convention
  - `ctest --test-dir build --output-on-failure -j$(nproc) 2>&1 | tee test_results_<timestamp>.txt`
  - Confirm zero regressions in the pre-existing Folly suite and all new stdexec suite tests pass
  - _Requirements: 12.5_

- [ ] 31. Checkpoint — Phase 5 complete
  - Ensure all tests (existing + new) pass; ask the user if any cross-backend fidelity test reveals an intentional (documented) behavioral difference rather than a bug

## Phase 6: Documentation

- [ ] 32. Write the stdexec backend migration guide
  - Add `examples/stdexec-backend/migration_guide_example.cpp` modeled on the existing `examples/migration_guide_example.cpp`, showing Folly-backend and stdexec-backend code side by side for: basic future creation, chaining, promise/future pairs, error handling, collective operations
  - _Requirements: 13.4_

- [ ] 33. Document scope boundaries explicitly
  - State in the new header's file-level doc comment and in a short `include/raft/future_stdexec.hpp`-adjacent README-style note: no production call site is converted by this feature, Folly is not removed or made optional-only, GPU/`nvexec` is out of scope
  - _Requirements: 13.1, 13.2, 13.3_

- [ ] 34. Record validated stdexec version and compiler matrix
  - Record the specific `stdexec` version/commit and minimum GCC/Clang versions validated during this implementation (from the Phase 0 spike notes, updated with any findings from later phases)
  - _Requirements: 13.5_

- [ ] 35. Final checkpoint — complete validation
  - Ensure all tests pass, all `static_assert` compliance checks pass, documentation is complete
  - Ask the user if questions arise before considering this spec done
