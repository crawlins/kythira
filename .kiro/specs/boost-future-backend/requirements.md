# Requirements Document

## Introduction

This specification defines the requirements for adding a **third**
implementation of the `kythira::Future`/`Promise`/`Executor`/`Try` family,
backed by `boost::thread`'s extended future API, alongside the existing
Folly-backed default (`include/raft/future.hpp`) and the `stdexec`-backed
implementation (`include/raft/future_stdexec.hpp`,
`.kiro/specs/stdexec-future-backend/`). `boost-thread` is already a required
vcpkg dependency of this project (`vcpkg.json`) and already linked into the
`network_simulator` interface target (`Boost::thread`, root `CMakeLists.txt`)
— today for `Boost::system`/threading primitives elsewhere in the codebase,
not for `boost::future`/`boost::promise` themselves, which this feature is
the first to actually use.

Unlike `stdexec`, `boost::thread`'s future/promise pair is **already a push
model**: `boost::promise<T>::set_value()` can be called from arbitrary code
on any thread at any later time, exactly like `folly::Promise::setValue()`
and `std::promise::set_value()`. This makes the boost backend structurally
the *simplest* of the three to bridge onto Raft's existing push-style
transport code — there is no `stdexec`-style pull/push mismatch to solve,
and no hand-rolled single-shot-channel primitive is needed the way
`.kiro/specs/stdexec-future-backend/` needed one for Requirement 6 there.
The real risks here are different in kind, not degree:

1. **Continuations are an unstable extension, not standard `std::future`
   behavior.** `boost::future<T>::then()`, `boost::when_all`, and
   `boost::when_any` are only available when `BOOST_THREAD_VERSION` is
   defined to `4` or `5` before `<boost/thread/future.hpp>` is first
   included (default is `2` if undefined at all, which compiles a
   `std::future`-equivalent surface with none of the extensions this
   feature needs). Every `then()` overload in the installed Boost.Thread
   headers is annotated `// EXTENSION` in-source — Boost's own documented
   signal that this surface is not part of the C++ standard and has
   changed shape across Boost releases historically. This feature commits
   to a specific `BOOST_THREAD_VERSION` and records exactly which Boost
   release was validated against (Requirement 9), rather than assuming
   perpetual API stability.
2. **`BOOST_THREAD_VERSION` is a project-wide ABI hazard, not a
   per-file compile flag.** Boost.Thread's future/promise template
   instantiations differ in layout and behavior depending on which
   version was active when a given translation unit was compiled;
   mixing translation units built with different values of
   `BOOST_THREAD_VERSION` that pass the same `boost::future`/
   `boost::promise` object across a TU boundary is undefined behavior,
   not merely a style inconsistency. Nothing in this codebase defines
   `BOOST_THREAD_VERSION` today (confirmed by inspection before writing
   this document — grep across `CMakeLists.txt`, `include/`, `cmd/`,
   `src/`, `tests/` found zero references), so there is no existing
   value to conflict with, but the requirements below make the value a
   single, project-wide `INTERFACE` compile definition rather than a
   convention someone could violate in one file.

This feature does not remove Folly or `stdexec`, does not convert any
existing production call site, and does not change the default backend.

## Glossary

- **Boost.Thread**: The `boost-thread` vcpkg package/Boost library
  providing `boost::thread`, `boost::future`, `boost::promise`, and the
  `boost::executors::` namespace; a required (non-optional) dependency of
  this project already, per `vcpkg.json`.
- **`BOOST_THREAD_VERSION`**: A Boost.Thread configuration macro
  (`include/boost/thread/detail/config.hpp` in the vendored headers)
  selecting which API surface is compiled; must be `4` or `5` for
  `then()`/`when_all`/`when_any` to exist at all. Defining it to `4`
  auto-defines `BOOST_THREAD_PROVIDES_FUTURE_CONTINUATION` and
  `BOOST_THREAD_PROVIDES_EXECUTORS` unless the build already defined
  either explicitly.
- **Extension API**: Any `boost::future`/`boost::promise` member or free
  function marked `// EXTENSION` in the Boost.Thread source — not part of
  the C++ standard `std::future` surface, documented by Boost itself as
  outside the standard's stability guarantees.
- **`boost::executors::executor`**: Boost.Thread's own abstract executor
  base class (`boost/thread/executors/executor.hpp`), with concrete
  implementations including `basic_thread_pool`, `serial_executor`, and
  `inline_executor` — Boost's analogue of `folly::Executor`, distinct in
  interface shape.
- **Backend**: A complete implementation of the future/promise/executor
  concepts in `include/concepts/future.hpp` — the existing Folly-backed
  one, the existing `stdexec`-backed one, or this feature's new
  `boost::thread`-backed one.
- **Shared Future**: `boost::shared_future<T>`, a copyable, multi-consumer
  counterpart to the move-only `boost::future<T>`; relevant only where
  `when_all`/`when_any`'s own signatures require it internally, not part
  of this feature's public wrapper surface (Requirement 4.5).
- **Concept Compliance Test Suite**: The `tests/folly_*_concept_compliance*`
  and `tests/stdexec_*_concept_compliance*` tests that `static_assert` and
  runtime-verify a backend's types satisfy `include/concepts/future.hpp`;
  this feature adds a parallel `tests/boost_*_concept_compliance*` suite.

## Requirements

### Requirement 1

**User Story:** As a library maintainer, I want `BOOST_THREAD_VERSION` and
its dependent macros defined exactly once, project-wide, as a build-system
setting rather than a per-file convention, so that no translation unit can
compile Boost.Thread futures/promises against a different API surface than
any other.

#### Acceptance Criteria

1. WHEN the `boost` future backend is enabled THEN the system SHALL define
   `BOOST_THREAD_VERSION=4` as a `target_compile_definitions(... INTERFACE
   ...)` on `network_simulator`, mirroring exactly how `KYTHIRA_HAS_STDEXEC`
   is already applied for the `stdexec` backend, so every consuming
   translation unit in the project — not just the new backend's own header
   — receives the identical value automatically
2. WHEN `BOOST_THREAD_VERSION=4` is defined THEN the system SHALL rely on
   Boost.Thread's own auto-definition of `BOOST_THREAD_PROVIDES_FUTURE_CONTINUATION`
   and `BOOST_THREAD_PROVIDES_EXECUTORS` rather than defining either
   redundantly, UNLESS the Phase 0 spike (Requirement 9) finds the
   installed Boost.Thread version does not auto-define one of them, in
   which case the system SHALL define it explicitly alongside
   `BOOST_THREAD_VERSION` and record why in `spike-notes.md`
3. WHEN any existing translation unit that already transitively includes
   `<boost/thread/future.hpp>` (directly or via another Boost.Thread
   header) is compiled after this feature lands THEN the system SHALL
   compile it identically to before, since `BOOST_THREAD_VERSION=4` is a
   strict superset of version 2's surface for any code that does not use
   the new extension APIs
4. WHEN the `boost` backend is not enabled (Requirement 8) THEN the system
   SHALL NOT define `BOOST_THREAD_VERSION` at all, leaving Boost.Thread's
   own default (`2`) in effect for the rest of the project exactly as
   today

### Requirement 2

**User Story:** As a developer, I want a `boost::thread`-backed `Try<T>`
wrapper, so that synchronous success/error results can be represented
consistently with the Folly and `stdexec` backends.

#### Acceptance Criteria

1. WHEN a `boost::future<T>` is ready and holds a value THEN the system
   SHALL construct a `Try<T>` holding that value
2. WHEN a `boost::future<T>` is ready and holds an exception THEN the
   system SHALL construct a `Try<T>` holding the corresponding
   `std::exception_ptr` (`boost::future<T>::get_exception_ptr()` or
   equivalent, not a rethrow-and-catch — avoiding an unnecessary
   throw/catch round trip on the hot path of inspecting an already-failed
   result)
3. WHEN `Try<T>::hasValue()`, `hasException()`, `value()`, and
   `exception()` are called on the boost-backed `Try<T>` THEN the system
   SHALL behave identically to the Folly-backed `Try<T>` for equivalent
   states
4. WHEN the boost-backed `Try<T>` is evaluated against the `try_type`
   concept (`include/concepts/future.hpp`, already backend-neutral per
   `.kiro/specs/stdexec-future-backend/` Requirement 1) THEN it SHALL
   satisfy the concept without modification to the concept itself

### Requirement 3

**User Story:** As a Raft transport developer, I want a `boost::thread`-backed
`SemiPromise<T>`/`Promise<T>` that can be fulfilled from arbitrary external
code (e.g. a network I/O completion handler), so that Folly's push-style
`Promise`/`Future` pattern used throughout the transport layer has a direct
`boost::thread` equivalent.

#### Acceptance Criteria

1. WHEN a `SemiPromise<T>`/`Promise<T>` is constructed on the `boost`
   backend THEN the system SHALL wrap a `boost::promise<T>` directly — no
   custom shared-state primitive is needed, since `boost::promise`/
   `boost::future` already implement exactly-once, cross-thread,
   arbitrary-timing fulfillment natively
2. WHEN `setValue`/`setException` is called on the boost-backed promise
   from any thread, including one unrelated to the thread that created the
   promise THEN the system SHALL complete the associated future exactly
   once with that value or error, delegating to `boost::promise::set_value`/
   `set_exception`
3. WHEN `setValue`/`setException` is called more than once on the same
   boost-backed promise THEN the system SHALL surface
   `boost::promise`'s own `promise_already_satisfied` condition as a
   `std::exception_ptr`-carried exception at the concept boundary, matching
   the Folly backend's double-fulfillment behavior in externally observable
   effect (throws), even though the concrete exception type differs
4. WHEN the promise is destroyed before being fulfilled and its future has
   already been retrieved THEN the system SHALL rely on
   `boost::promise`'s own broken-promise behavior (the future becomes ready
   with a `boost::broken_promise`-derived exception), matching `folly::Promise`'s
   behavior in externally observable effect
5. WHEN this wrapper is evaluated against the `semi_promise` and `promise`
   concepts THEN it SHALL satisfy both

### Requirement 4

**User Story:** As a developer, I want the `boost::thread`-backed
`Future<T>` to satisfy the same concepts as the Folly and `stdexec`
backends, so that generic code written against the concepts works with any
of the three.

#### Acceptance Criteria

1. WHEN `Future<T>::get()` is called on the `boost` backend THEN the
   system SHALL block the calling thread using `boost::future<T>::get()`
   and return the value or rethrow the exception
2. WHEN `Future<T>::isReady()` is called on the `boost` backend THEN the
   system SHALL report completion status via `boost::future<T>::is_ready()`
   without blocking
3. WHEN `Future<T>::wait(timeout)` is called on the `boost` backend THEN
   the system SHALL use `boost::future<T>::wait_for()` and return whether
   the future became ready within the timeout
4. WHEN the boost-backed `Future<T>` is evaluated against the `future`
   concept THEN it SHALL satisfy the concept
5. WHEN a `boost::future<T>`-returning Boost.Thread API internally requires
   a `boost::shared_future<T>` (e.g. as an implementation detail of
   `when_all`/`when_any` composition) THEN the system SHALL keep that
   conversion entirely internal to the wrapper's implementation — the
   public `kythira::boost_backend::Future<T>` type SHALL remain move-only,
   matching the Folly and `stdexec` backends' `Future<T>` semantics, never
   exposing `boost::shared_future` in any public signature

### Requirement 5

**User Story:** As a developer, I want `boost`-backed factory functions
equivalent to `FutureFactory`, so that ready and exceptional futures can be
created without going through a `Promise`.

#### Acceptance Criteria

1. WHEN `FutureFactory::makeFuture(value)` is called on the `boost` backend
   THEN the system SHALL return a `Future<T>` wrapping
   `boost::make_ready_future(value)`
2. WHEN `FutureFactory::makeExceptionalFuture<T>(ex)` is called on the
   `boost` backend THEN the system SHALL return a `Future<T>` wrapping
   `boost::make_exceptional_future<T>(ex)`
3. WHEN `FutureFactory::makeReadyFuture()` is called on the `boost` backend
   THEN the system SHALL return a `Future<kythira::unit>`-equivalent that
   is immediately ready, using `boost::make_ready_future()`'s `void`
   overload internally and converting to `kythira::unit` at the wrapper
   boundary (the same conversion point the Folly backend already uses for
   `folly::Unit` — see `include/raft/future.hpp`)
4. WHEN the boost-backed `FutureFactory` is evaluated against the
   `future_factory` concept THEN it SHALL satisfy the concept

### Requirement 6

**User Story:** As a developer, I want `boost`-backed continuation and
transformation operations (`thenValue`, `thenTry`, `thenError`, `ensure`,
`via`, `delay`, `within`), so that existing chaining code patterns work the
same way regardless of backend.

#### Acceptance Criteria

1. WHEN `thenValue` is called on the `boost` backend THEN the system SHALL
   compose it using `boost::future<T>::then()`, unwrapping the
   `boost::future<T>` argument `then()`'s callback receives (Boost's
   `then()` passes the *completed future itself* to the callback, not the
   unwrapped value — the wrapper SHALL call `.get()` inside its own
   adaptor closure to present a plain-value callback signature matching
   the Folly backend's `thenValue`)
2. WHEN `thenError` is called on the `boost` backend THEN the system SHALL
   compose it using the same `then()` mechanism, inspecting the completed
   future for an exception inside the adaptor closure and normalizing it
   to `std::exception_ptr` before invoking the user's callback — Boost.Thread
   has no separate `thenError`-shaped primitive, unlike its distinct
   value/error handling in some other future libraries
3. WHEN `thenValue`/`thenTry`/`thenError` are given a callback that itself
   returns a `Future<U>` THEN the system SHALL automatically flatten to
   `Future<U>` (no `Future<Future<U>>`), mirroring the Folly backend's
   existing flattening behavior — `boost::future<T>::then()` itself does
   NOT auto-flatten (unlike `folly::Future::thenValue`), so this flattening
   SHALL be implemented explicitly in the wrapper, not assumed to come free
4. WHEN `via(scheduler)` is called on the `boost` backend THEN the system
   SHALL reschedule the continuation onto the given `boost::executors::executor`
   using the `then(Ex&, F)` overload (requires
   `BOOST_THREAD_PROVIDES_EXECUTORS`, auto-enabled per Requirement 1.2)
5. WHEN `delay(duration)` or `within(timeout)` is called on the `boost`
   backend THEN the system SHALL implement it using `boost::asio`
   (`boost-asio` is already a required vcpkg dependency of this project,
   per `vcpkg.json`) — a `boost::asio::steady_timer` posted onto a small,
   backend-owned `io_context`/thread, fulfilling a `boost::promise` on
   expiry — rather than a busy-wait or a blocking sleep on the calling
   thread; Boost.Thread itself provides no built-in timed-continuation
   combinator, unlike `stdexec`'s (optional) timed schedulers
6. WHEN `ensure(func)` is called on the `boost` backend THEN the system
   SHALL guarantee `func` runs on both the success and error paths inside
   the same `then()`-based adaptor used for Acceptance Criteria 1-2,
   matching Folly's `ensure` semantics

### Requirement 7

**User Story:** As a developer, I want `boost`-backed collective operations
equivalent to `FutureCollector`, so that fan-out/fan-in patterns
(`collectAll`, `collectAny`, `collectAnyWithoutException`, `collectN`) work
the same way regardless of backend.

#### Acceptance Criteria

1. WHEN `collectAll` is called on the `boost` backend THEN the system
   SHALL implement it using `boost::when_all`, converting the resulting
   single future-of-tuple (or future-of-vector, for the iterator-pair
   overload) into a `std::vector<Try<T>>` at the wrapper boundary, one
   `Try<T>` per input future in its original order — matching Folly's
   `collectAll` semantics, where the whole operation succeeds and gives
   you a vector of `Try<T>` regardless of individual failures
2. WHEN `collectAny` is called on the `boost` backend THEN the system
   SHALL implement it using `boost::when_any`, converting the resulting
   future-of-tuple (which carries every input future, exactly one of them
   ready) into the `(index, Try<T>)` pair shape the concept expects by
   scanning for the ready one
3. WHEN `collectAnyWithoutException` is called on the `boost` backend THEN
   the system SHALL return the first successfully-completed result,
   continuing past futures that fail — `boost::when_any` alone does not
   distinguish "first to complete" from "first to *succeed*", so this
   SHALL be built on top of `collectAny` plus a retry-past-failures loop,
   not assumed to be a single Boost primitive
4. WHEN `collectN` is called on the `boost` backend THEN the system SHALL
   return the first N completed results with their original indices —
   Boost.Thread has no `when_n`/`collectN` primitive, so this SHALL be
   implemented by repeated application of `collectAny`-style
   first-to-complete selection over the shrinking remainder, removing each
   selected future from the pool before the next round
5. WHEN any collective operation is evaluated against the
   `future_collector` concept THEN it SHALL satisfy the concept

### Requirement 8

**User Story:** As a developer choosing a backend, I want the existing
backend-selection mechanism extended to include `boost`, so that I know
which backend a given `kythira::Future` instantiation uses and no two
backends are ever silently mixed.

#### Acceptance Criteria

1. WHEN the `boost` backend is implemented THEN the system SHALL place it
   in a distinct namespace, `kythira::boost_backend`, mirroring
   `kythira::stdexec_backend`'s existing precedent — never reusing the
   unqualified `kythira::Future` name the Folly backend uses
2. WHEN generic core code (already templated on a future type, per the
   prior `folly-concept-wrappers` and `stdexec-future-backend` work) is
   instantiated THEN the system SHALL accept any of the three backends'
   `Future<T>` as the template argument, since all three satisfy the same
   concepts
3. WHEN `KYTHIRA_DEFAULT_FUTURE_BACKEND` (root `CMakeLists.txt`, currently
   accepting `folly` or `stdexec`) is examined THEN the system SHALL extend
   its accepted values to include `boost`, updating both the
   `set_property(... STRINGS ...)` list and the
   `if/elseif/else message(FATAL_ERROR ...)` validation to cover all three
   values instead of two
4. WHEN `KYTHIRA_DEFAULT_FUTURE_BACKEND=boost` is selected THEN the system
   SHALL define `KYTHIRA_FUTURE_BACKEND_BOOST` (mirroring
   `KYTHIRA_FUTURE_BACKEND_STDEXEC`'s existing pattern) and
   `include/raft/future_default.hpp` SHALL resolve
   `kythira::future_default<T>` to `kythira::boost_backend::Future<T>` in
   that configuration
5. WHEN code from two different backends is combined in the same
   expression (e.g. chaining a `boost_backend::Future` continuation with a
   Folly-only `Executor*`, or a `stdexec_backend`-only construct) THEN the
   system SHALL fail to compile rather than silently producing incorrect
   behavior, extending the existing
   `tests/backend_non_interference_compile_fail_test.cpp` coverage to
   include the new backend rather than adding a separate file
6. WHEN a developer changes `KYTHIRA_DEFAULT_FUTURE_BACKEND` to or from
   `boost` THEN the system SHALL NOT require changes to templated core
   code, only to `kythira::future_default`-using call sites, identical to
   the existing guarantee for switching between `folly` and `stdexec`

### Requirement 9

**User Story:** As a developer, I want the boost backend's dependence on
Boost's own `// EXTENSION`-labeled API surface investigated and recorded
before implementation begins, so that the design does not assume API
stability Boost itself does not guarantee.

#### Acceptance Criteria

1. WHEN implementation begins THEN the system SHALL first record, in a
   `spike-notes.md` in this spec's directory (mirroring
   `.kiro/specs/stdexec-future-backend/spike-notes.md`'s precedent): the
   exact installed Boost version and whether `BOOST_THREAD_VERSION=4`
   alone is sufficient to enable `then()`/`when_all`/`when_any`/executor
   overloads on it, or whether any macro needs defining explicitly
   (Requirement 1.2)
2. WHEN the spike is performed THEN the system SHALL confirm empirically
   (a throwaway compile, not documentation-reading alone) that
   `boost::future<T>::then()`'s callback receives the completed future
   itself (not the unwrapped value) as documented in Requirement 6.1,
   since this shape has differed across Boost versions historically
3. WHEN the spike is performed THEN the system SHALL confirm whether
   `boost::when_all`'s iterator-pair overload (for a runtime-sized
   collection, needed by `collectAll`/`collectN` over an arbitrary-length
   `std::vector<Future<T>>`) is available in the installed version, as
   distinct from the variadic overload (fixed at compile time) — both
   exist in the Boost.Thread source inspected while writing this document
   (`boost/thread/future.hpp`), but the spike SHALL confirm this against
   the actual vendored vcpkg version, not this document's own inspection
4. WHEN the spike concludes THEN the system SHALL record the specific
   Boost release and minimum compiler versions validated, matching
   `.kiro/specs/stdexec-future-backend/` Requirement 13.5's precedent for
   `stdexec`

### Requirement 10

**User Story:** As a developer, I want the `boost` backend validated with
the same rigor as the Folly and `stdexec` backends, so that I can trust its
concept compliance and behavior.

#### Acceptance Criteria

1. WHEN the `boost` backend's wrapper types are compiled THEN the system
   SHALL `static_assert` their compliance with every concept they are
   meant to satisfy, mirroring the assertions at the end of
   `include/raft/future.hpp` and `include/raft/future_stdexec.hpp`
2. WHEN the existing Folly and `stdexec` concept-compliance test files are
   used as a template THEN the system SHALL add a parallel
   `tests/boost_*_concept_compliance*` suite covering the same behaviors
   for the `boost` backend
3. WHEN property-based tests are written for the `boost` backend THEN the
   system SHALL follow this project's existing property-test tagging
   convention (`**Feature: boost-future-backend, Property N: ...**`) and
   `BOOST_AUTO_TEST_CASE` timeout requirements
4. WHEN all three backends implement the same operation (e.g. `collectAll`
   on ten futures with one exception) THEN the system SHALL produce
   equivalent externally-observable results (value/exception content and
   ordering) across all three, even though internal scheduling mechanics
   differ
5. WHEN the `boost` backend test suite is run THEN the system SHALL
   execute exclusively through CTest, per this project's test execution
   standards, labeled `boost-future` so `ctest -L boost-future` can run
   just this suite

### Requirement 11

**User Story:** As a developer, I want documentation of what this feature
does and does not do, so that expectations about the scope of the `boost`
backend are accurate.

#### Acceptance Criteria

1. WHEN documentation for this feature is written THEN the system SHALL
   state explicitly that no existing production call site is converted
   from Folly (or `stdexec`) to `boost` by this feature
2. WHEN documentation for this feature is written THEN the system SHALL
   state explicitly that neither the Folly nor the `stdexec` dependency is
   removed or made optional-only as a result of this feature
3. WHEN documentation for this feature is written THEN the system SHALL
   state explicitly that `boost::fibers`, Boost.Cobalt (Boost 1.84+'s
   coroutine-based async library), and any other Boost async facility
   besides `boost::thread`'s future/promise extension surface are out of
   scope
4. WHEN documentation for this feature is written THEN the system SHALL
   provide a migration guide entry, modeled on the existing
   `examples/migration_guide_example.cpp`, showing Folly-backend and
   `boost`-backend code side by side for the same operation
5. WHEN documentation for this feature is written THEN the system SHALL
   explicitly flag, in both `spike-notes.md` and the top-of-file comment
   in the new backend header, that `then()`/`when_all`/`when_any` are
   Boost's own `// EXTENSION`-labeled surface, not standard `std::future`
   behavior, and that a future Boost upgrade changing or removing this
   surface is a real (if currently unlikely) maintenance risk this project
   is knowingly accepting by using it
