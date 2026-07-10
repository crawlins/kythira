# Requirements Document

## Introduction

This specification defines the requirements for adding a second, `stdexec`-backed implementation of the `kythira::Future`/`Promise`/`Executor`/`Try` family alongside the existing Folly-backed implementation created by the `folly-concept-wrappers` spec. The existing wrapper classes in `include/raft/future.hpp` already isolate Folly behind the concepts in `include/concepts/future.hpp`, but those concepts currently name Folly types directly in their `requires` clauses (`folly::exception_wrapper`, `folly::Unit`), so satisfying them today requires including Folly headers. This feature does not remove Folly or convert existing call sites; it (1) regenericizes the concepts so a non-Folly type can satisfy them, (2) adds `stdexec` (the NVIDIA reference implementation of C++26's `std::execution` / P2300) as an optional dependency, and (3) implements a second backend that satisfies the regenericized concepts, so the two backends can coexist and be selected per translation unit or per template instantiation.

The `stdexec` sender/receiver model is a fundamentally different concurrency paradigm from Folly's callback-chaining futures: senders describe work that starts when connected to a receiver ("pull"), whereas Folly's `Promise::setValue` is fulfilled from arbitrary external code at an arbitrary later time ("push"). Raft's transport layer relies on the push model — an RPC response arriving on a network I/O completion handler fulfills a promise created earlier on a different call stack. Bridging this mismatch (a single-shot, externally-fulfillable sender) is the central technical risk of this project and is called out explicitly in its own requirement rather than assumed away.

## Glossary

- **stdexec**: NVIDIA's header-only reference implementation of C++26's `std::execution` (P2300), usable today with C++20/23 toolchains without waiting for native standard library support.
- **P2300**: The C++ standards proposal defining `std::execution`, accepted for C++26.
- **Sender**: A `stdexec` object describing an asynchronous computation that has not yet started; the async analogue of a lazy, composable "future template."
- **Receiver**: A `stdexec` object that consumes a sender's completion (`set_value`, `set_error`, or `set_stopped`).
- **Scheduler**: A `stdexec` object that can produce a sender to schedule work onto an execution context; the sender/receiver analogue of `folly::Executor`.
- **Operation State**: The object produced by connecting a sender to a receiver (`stdexec::connect`); calling `start()` on it begins execution.
- **sync_wait**: A `stdexec` algorithm that blocks the calling thread until a sender completes and returns its result — the sender/receiver analogue of `Future::get()`.
- **Single-Shot Channel**: A custom sender/operation-state pair, not provided out of the box by `stdexec`, that lets code outside the sender graph fulfill a pending completion later (needed to bridge Folly's push-style `Promise::setValue` semantics onto `stdexec`'s pull-style senders).
- **Regenericized Concept**: One of the concepts in `include/concepts/future.hpp` after this feature, expressed without naming any Folly type directly (e.g. using `std::exception_ptr` instead of `folly::exception_wrapper`).
- **Backend**: A complete implementation of the future/promise/executor concepts — either the existing Folly-backed one or the new `stdexec`-backed one.
- **Compatibility Shim**: A small adapter that lets a `stdexec` scheduler additionally satisfy the existing callback-shaped `executor` concept (`.add(std::function<void()>)`), for interoperability with code that has not been updated to use schedulers directly.
- **Concept Compliance Test Suite**: The `tests/folly_*_concept_compliance*` and `tests/kythira_*_concept_compliance*` tests that `static_assert` and runtime-verify that a backend's types satisfy the concepts; this feature adds a parallel `stdexec` suite.

## Requirements

### Requirement 1

**User Story:** As a library maintainer, I want the concepts in `include/concepts/future.hpp` to be expressed without naming Folly types directly, so that a non-Folly backend can satisfy them without including Folly headers.

#### Acceptance Criteria

1. WHEN the `try_type` concept is evaluated THEN the system SHALL express exception access using `std::exception_ptr` instead of `folly::exception_wrapper`
2. WHEN the `semi_promise` concept is evaluated THEN the system SHALL express exception submission using `std::exception_ptr` instead of `folly::exception_wrapper`
3. WHEN a concept needs a stand-in type for `void` THEN the system SHALL use a backend-neutral `kythira::unit` type instead of `folly::Unit`
4. WHEN `include/concepts/future.hpp` is compiled on its own THEN the system SHALL NOT require any Folly header to be included
5. WHEN the existing Folly-backed wrapper types in `include/raft/future.hpp` are compiled against the regenericized concepts THEN the system SHALL continue to satisfy every concept it satisfied before this change, with no behavioral change

### Requirement 2

**User Story:** As a library maintainer, I want `folly::Unit` replaced by a project-owned unit type in the concept layer, so that void-result handling is not tied to Folly's type.

#### Acceptance Criteria

1. WHEN a backend-neutral unit type is needed THEN the system SHALL define `kythira::unit` as a trivial, default-constructible, comparable empty struct
2. WHEN the Folly backend maps `void` internally THEN the system SHALL convert between `kythira::unit` and `folly::Unit` at the Folly wrapper boundary only
3. WHEN the `stdexec` backend maps `void` internally THEN the system SHALL use `kythira::unit` directly, since `stdexec` senders natively support zero-or-more-value completions without requiring a placeholder type
4. WHEN existing code constructs `kythira::Future<void>` or `kythira::Try<void>` THEN the system SHALL preserve the existing public API surface unchanged

### Requirement 3

**User Story:** As a library maintainer, I want an explicit, documented decision on how the `executor` and `keep_alive` concepts relate to `stdexec` schedulers, so that the mismatch between Folly's callback-submission model and `stdexec`'s scheduler model is not silently papered over.

#### Acceptance Criteria

1. WHEN the design is evaluated THEN the system SHALL document that `folly::Executor::add(std::function<void()>)` (push-a-callback) and `stdexec` scheduling (`stdexec::schedule` returning a sender to connect and start) are different shapes, not variations of the same interface
2. WHEN a `stdexec` scheduler is wrapped for compatibility with existing `executor`-concept-constrained code THEN the system SHALL provide a compatibility shim implementing `.add()` by connecting and starting a sender on that scheduler
3. WHEN new code is written against the `stdexec` backend directly THEN the system SHALL allow that code to use `stdexec` schedulers and sender adaptors natively, without going through the compatibility shim
4. WHEN the compatibility shim is used THEN the system SHALL document its overhead and semantic differences (e.g. loss of structured-concurrency cancellation) relative to native scheduler usage
5. WHERE a future capability requires priority-based execution THEN the system SHALL treat it as optional for both backends, consistent with the existing `executor` concept

### Requirement 4

**User Story:** As a build system maintainer, I want `stdexec` wired into the build as an optional dependency, so that projects that don't need the new backend are unaffected.

#### Acceptance Criteria

1. WHEN `vcpkg.json` is examined THEN the system SHALL declare a dependency on the `stdexec` vcpkg port
2. WHEN CMake configures the project THEN the system SHALL locate `stdexec` using the same optional, `QUIET`-find pattern already used for `folly` in the root `CMakeLists.txt`
3. WHEN `stdexec` is not found THEN the system SHALL emit a warning and disable the `stdexec` backend and its targets, without failing configuration of the rest of the project
4. WHEN both `folly` and `stdexec` are found THEN the system SHALL build both backends and both concept-compliance test suites
5. WHEN a new compose file, container, or CI runner is not involved THEN this requirement SHALL NOT be read as implying any change to `tests/docker_chaos` container-runtime handling — this feature is header/library-only and has no container surface

### Requirement 5

**User Story:** As a developer, I want a `stdexec`-backed `Try<T>` wrapper, so that synchronous success/error results can be represented consistently with the Folly backend.

#### Acceptance Criteria

1. WHEN a `stdexec` sender completes with a value THEN the system SHALL construct a `Try<T>` holding that value
2. WHEN a `stdexec` sender completes with an error THEN the system SHALL construct a `Try<T>` holding the corresponding `std::exception_ptr`
3. WHEN a `stdexec` sender completes with the "stopped" (cancellation) signal THEN the system SHALL construct a `Try<T>` holding an `std::exception_ptr` wrapping a project-defined cancellation exception type
4. WHEN `Try<T>::hasValue()`, `hasException()`, `value()`, and `exception()` are called on the `stdexec`-backed `Try<T>` THEN the system SHALL behave identically to the Folly-backed `Try<T>` for equivalent states
5. WHEN the `stdexec`-backed `Try<T>` is evaluated against the regenericized `try_type` concept THEN it SHALL satisfy the concept

### Requirement 6

**User Story:** As a Raft transport developer, I want a `stdexec`-backed single-shot channel that can be fulfilled from arbitrary external code (e.g. a network I/O completion handler), so that Folly's push-style `Promise`/`Future` pattern used throughout the transport layer has a working `stdexec` equivalent.

#### Acceptance Criteria

1. WHEN a `SemiPromise<T>`/`Promise<T>` is constructed on the `stdexec` backend THEN the system SHALL produce an associated sender that has not yet completed
2. WHEN `setValue`/`setException` is called on the `stdexec`-backed promise from any thread, including one unrelated to the thread that created the promise THEN the system SHALL complete the associated sender exactly once with that value or error
3. WHEN `setValue`/`setException` is called more than once on the same `stdexec`-backed promise THEN the system SHALL behave the same as the Folly backend's double-fulfillment handling (throwing, per `folly::Promise` semantics)
4. WHEN the associated sender is connected to a receiver and started before `setValue`/`setException` is called THEN the system SHALL suspend the operation state until fulfillment occurs, without busy-waiting
5. WHEN the promise is destroyed before being fulfilled and its future has already been connected/started THEN the system SHALL complete the operation with an error (broken-promise semantics), matching `folly::Promise` behavior
6. WHEN this channel is evaluated against the regenericized `semi_promise` and `promise` concepts THEN it SHALL satisfy both

### Requirement 7

**User Story:** As a developer, I want the `stdexec`-backed `Future<T>` to satisfy the same concepts as the Folly-backed one, so that generic code written against the concepts works with either backend.

#### Acceptance Criteria

1. WHEN `Future<T>::get()` is called on the `stdexec` backend THEN the system SHALL block the calling thread using `stdexec::sync_wait` (or equivalent) and return the value or rethrow the exception
2. WHEN `Future<T>::isReady()` is called on the `stdexec` backend THEN the system SHALL report completion status without blocking
3. WHEN `Future<T>::wait(timeout)` is called on the `stdexec` backend THEN the system SHALL return whether the future became ready within the timeout, without leaking the underlying operation state on timeout
4. WHEN the `stdexec`-backed `Future<T>` is evaluated against the regenericized `future` concept THEN it SHALL satisfy the concept
5. WHEN the `stdexec`-backed `Future<T>` is evaluated against the regenericized `future_transformable` and `future_continuation` concepts THEN it SHALL satisfy both, per Requirement 9's mapping

### Requirement 8

**User Story:** As a developer, I want `stdexec`-backed factory functions equivalent to `FutureFactory`, so that ready and exceptional futures can be created without going through a `Promise`.

#### Acceptance Criteria

1. WHEN `FutureFactory::makeFuture(value)` is called on the `stdexec` backend THEN the system SHALL return a `Future<T>` wrapping `stdexec::just(value)` (or the backend's equivalent immediately-ready sender)
2. WHEN `FutureFactory::makeExceptionalFuture<T>(ex)` is called on the `stdexec` backend THEN the system SHALL return a `Future<T>` wrapping an immediately-failed sender carrying that `std::exception_ptr`
3. WHEN `FutureFactory::makeReadyFuture()` is called on the `stdexec` backend THEN the system SHALL return a `Future<void>`-equivalent (using `kythira::unit` per Requirement 2) that is immediately ready
4. WHEN the `stdexec`-backed `FutureFactory` is evaluated against the regenericized `future_factory` concept THEN it SHALL satisfy the concept

### Requirement 9

**User Story:** As a developer, I want `stdexec`-backed continuation and transformation operations (`thenValue`, `thenTry`, `thenError`, `ensure`, `via`, `delay`, `within`), so that existing chaining code patterns work the same way regardless of backend.

#### Acceptance Criteria

1. WHEN `thenValue` is called on the `stdexec` backend THEN the system SHALL compose it using the `stdexec::then` sender adaptor
2. WHEN `thenError` is called on the `stdexec` backend THEN the system SHALL compose it using `stdexec` error-handling adaptors (e.g. `upon_error`/`let_error`), converting `std::exception_ptr` at the boundary
3. WHEN `thenValue`/`thenTry`/`thenError` are given a callback that itself returns a `Future<U>` THEN the system SHALL automatically flatten to `Future<U>` (no `Future<Future<U>>`), mirroring the Folly backend's existing flattening behavior
4. WHEN `via(scheduler)` is called on the `stdexec` backend THEN the system SHALL reschedule the continuation onto the given scheduler using the appropriate `stdexec` transfer/continue-on adaptor
5. WHEN `delay(duration)` or `within(timeout)` is called on the `stdexec` backend THEN the system SHALL implement it using a timed scheduler from the `stdexec`/`exec` extensions; IF no such combinator exists in the selected `stdexec` version THEN the system SHALL implement a minimal timed-completion sender rather than blocking a thread
6. WHEN `ensure(func)` is called on the `stdexec` backend THEN the system SHALL guarantee `func` runs on both the success and error paths, matching Folly's `ensure` semantics

### Requirement 10

**User Story:** As a developer, I want `stdexec`-backed collective operations equivalent to `FutureCollector`, so that fan-out/fan-in patterns (`collectAll`, `collectAny`, `collectAnyWithoutException`, `collectN`) work the same way regardless of backend.

#### Acceptance Criteria

1. WHEN `collectAll` is called on the `stdexec` backend THEN the system SHALL implement it using `stdexec::when_all` (or the closest available combinator), preserving input order in the result
2. WHEN `collectAny` is called on the `stdexec` backend THEN the system SHALL return the index and `Try<T>` of the first future to complete, using an available `stdexec`/`exec` first-to-complete combinator; IF no such combinator exists in the selected `stdexec` version THEN the system SHALL implement one using the single-shot channel from Requirement 6
3. WHEN `collectAnyWithoutException` is called on the `stdexec` backend THEN the system SHALL return the first successfully-completed result, continuing past futures that fail
4. WHEN `collectN` is called on the `stdexec` backend THEN the system SHALL return the first N completed results with their original indices
5. WHEN any collective operation is evaluated against the regenericized `future_collector` concept THEN it SHALL satisfy the concept

### Requirement 11

**User Story:** As a developer choosing a backend, I want a documented, explicit selection mechanism, so that I know which backend a given `kythira::Future` instantiation uses and the two backends are never silently mixed.

#### Acceptance Criteria

1. WHEN the `stdexec` backend is implemented THEN the system SHALL place it in a distinct namespace (e.g. `kythira::stdexec_backend`) rather than reusing the unqualified `kythira::Future` name used by the Folly backend
2. WHEN generic core code (already templated on a future type per the prior `folly-concept-wrappers` conversion) is instantiated THEN the system SHALL accept either backend's `Future<T>` as the template argument, since both satisfy the same regenericized concepts
3. WHEN a project or translation unit wants a single default backend for non-templated call sites THEN the system SHALL provide exactly one CMake option (e.g. `KYTHIRA_DEFAULT_FUTURE_BACKEND`, values `folly` or `stdexec`, default `folly`) controlling a `kythira::future_default` alias
4. WHEN code from the two backends is combined in the same expression (e.g. chaining a `stdexec_backend::Future` continuation with a `folly_backend`-only `Executor`) THEN the system SHALL fail to compile rather than silently producing incorrect behavior
5. WHEN a developer changes `KYTHIRA_DEFAULT_FUTURE_BACKEND` THEN the system SHALL NOT require changes to templated core code, only to non-templated call sites using the `kythira::future_default` alias

### Requirement 12

**User Story:** As a developer, I want the `stdexec` backend validated with the same rigor as the Folly backend, so that I can trust its concept compliance and behavior.

#### Acceptance Criteria

1. WHEN the `stdexec` backend's wrapper types are compiled THEN the system SHALL `static_assert` their compliance with every regenericized concept they are meant to satisfy, mirroring the assertions at the end of `include/raft/future.hpp`
2. WHEN the existing Folly concept-compliance test files (`tests/folly_*_concept_compliance*`) are used as a template THEN the system SHALL add a parallel `tests/stdexec_*_concept_compliance*` suite covering the same behaviors for the `stdexec` backend
3. WHEN property-based tests are written for the `stdexec` backend THEN the system SHALL follow this project's existing property-test tagging convention (`**Feature: stdexec-future-backend, Property N: ...**`) and `BOOST_AUTO_TEST_CASE` timeout requirements
4. WHEN both backends implement the same operation (e.g. `collectAll` on ten futures with one exception) THEN the system SHALL produce equivalent externally-observable results (value/exception content and ordering) even though internal scheduling differs
5. WHEN the `stdexec` backend test suite is run THEN the system SHALL execute exclusively through CTest, per this project's test execution standards

### Requirement 13

**User Story:** As a developer, I want documentation of what this feature does and does not do, so that expectations about the scope of `stdexec` adoption are accurate.

#### Acceptance Criteria

1. WHEN documentation for this feature is written THEN the system SHALL state explicitly that no existing production call site is converted from Folly to `stdexec` by this feature
2. WHEN documentation for this feature is written THEN the system SHALL state explicitly that the Folly dependency is not removed or made optional-only as a result of this feature
3. WHEN documentation for this feature is written THEN the system SHALL state explicitly that GPU execution (`nvexec`) and other `stdexec` extensions beyond CPU scheduling are out of scope
4. WHEN documentation for this feature is written THEN the system SHALL provide a migration guide, modeled on the existing `examples/migration_guide_example.cpp`, showing Folly-backend and `stdexec`-backend code side by side for the same operation
5. WHEN documentation for this feature is written THEN the system SHALL record the specific `stdexec` version/commit and minimum compiler versions validated during implementation, since `stdexec` is still evolving alongside the not-yet-finalized C++26 standard
