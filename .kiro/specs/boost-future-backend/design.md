# Design Document

## Overview

This design adds a third implementation of the `kythira`
future/promise/executor family, backed by `boost::thread`'s extended
future API, alongside the existing Folly-backed default
(`include/raft/future.hpp`) and `stdexec`-backed
(`include/raft/future_stdexec.hpp`) implementations. It does not touch any
existing production call site or change the default backend.

Where `.kiro/specs/stdexec-future-backend/`'s central design problem was
bridging a **pull**-model library (senders don't run until connected and
started) onto Raft's **push**-model transport code, this feature has no
equivalent bridge problem: `boost::promise<T>::set_value()`/`set_exception()`
are already push-model, callable from arbitrary code on any thread at any
later time — the same shape as `folly::Promise` and `std::promise`. The
`SemiPromise<T>`/`Promise<T>` wrapper here is therefore a **direct,
near-trivial wrap** of `boost::promise<T>`, not a hand-rolled primitive.

The real design work is elsewhere:

1. **`BOOST_THREAD_VERSION` is a project-wide ABI setting, not a per-file
   flag.** Boost.Thread's future/promise templates compile differently
   depending on this macro's value at each translation unit's compile
   time; passing a `boost::future`/`boost::promise` object across a TU
   boundary compiled with a different value is undefined behavior. This is
   solved by definition, not convention: `BOOST_THREAD_VERSION=4` is
   applied as a single `target_compile_definitions(network_simulator
   INTERFACE ...)`, the same mechanism `KYTHIRA_HAS_STDEXEC` already uses,
   so every consumer of `network_simulator` gets the identical value
   automatically.
2. **`boost::future<T>::then()` does not match Folly's `thenValue` shape
   directly** — it passes the *completed future itself* to the callback,
   not the unwrapped value, and does not auto-flatten a callback that
   returns another future. Both are handled inside the wrapper's own
   adaptor closures, not assumed away.
3. **No built-in timed-continuation combinator.** Unlike `stdexec`'s
   optional timed schedulers, Boost.Thread has nothing equivalent to
   `delay`/`within`. This design reuses `boost::asio` (also a required
   vcpkg dependency already) for a small, backend-owned timer instead of
   inventing a new primitive.
4. **`then()`/`when_all`/`when_any` are Boost's own `// EXTENSION`-labeled
   surface**, not standard `std::future` behavior — confirmed by direct
   inspection of the vendored Boost.Thread headers while writing this
   design (`boost/thread/future.hpp`), not assumed from documentation
   alone. A Phase 0 spike (mirroring `stdexec-future-backend`'s own Phase
   0) confirms the exact enabled surface against the actually-installed
   version before the rest of the design is finalized in detail.

## Architecture

```
┌───────────────────────────────────────────────────────────────────┐
│  Concept Layer (include/concepts/future.hpp)                       │
│  try_type / semi_promise / promise / executor / keep_alive /       │
│  future / future_factory / future_collector /                      │
│  future_continuation / future_transformable                        │
│  — already backend-neutral (std::exception_ptr, kythira::unit)     │
│    since .kiro/specs/stdexec-future-backend/'s Phase 1              │
└───────┬───────────────────────┬───────────────────────┬───────────┘
        │ satisfies              │ satisfies              │ satisfies
┌───────▼──────────┐  ┌──────────▼───────────┐  ┌─────────▼──────────────┐
│ Folly Backend     │  │ stdexec Backend       │  │ boost Backend (new)     │
│ (default)         │  │ include/raft/         │  │ include/raft/           │
│ include/raft/     │  │  future_stdexec.hpp    │  │  future_boost.hpp       │
│  future.hpp        │  │ namespace kythira::   │  │ namespace kythira::     │
│ namespace kythira  │  │  stdexec_backend       │  │  boost_backend          │
│  { ... }            │  │                        │  │                          │
│ Try/SemiPromise/   │  │ Try/SemiPromise/      │  │ Try/SemiPromise/        │
│ Promise/Future/    │  │ Promise/Future/       │  │ Promise/Future/         │
│ Executor/KeepAlive/│  │ single_shot_channel/  │  │ executor_shim/          │
│ FutureFactory/     │  │ scheduler_executor_   │  │ FutureFactory/          │
│ FutureCollector    │  │  shim/FutureFactory/  │  │ FutureCollector/        │
│ wrap folly::* types│  │  FutureCollector      │  │ timer (boost::asio)     │
│                     │  │ wrap stdexec senders  │  │ wrap boost::thread      │
└─────────────────────┘  └────────────────────────┘  └──────────────────────────┘
        │                          │                            │
 folly::Future/Promise    stdexec::sender/receiver,    boost::future/promise,
 folly::Executor           scheduler, when_all, then    boost::executors::executor,
                                                          boost::asio::steady_timer
```

`kythira::unit` and every concept remain as defined by
`.kiro/specs/stdexec-future-backend/`'s Phase 1 — this feature makes no
further change to `include/concepts/future.hpp`. Generic core code already
templated on a future type is unaffected by which backend is chosen; it
only depends on the concepts.

### Why a third namespace, not a third `#ifdef` branch inside the existing backends

Same rationale `stdexec-future-backend`'s design.md already established for
its own second namespace: a single `kythira::Future` selected by `#ifdef`
would make mixing backends within one build (or one test binary comparing
all three, per Requirement 10.4) impossible without macro gymnastics.
`kythira::boost_backend` follows the exact same shape as
`kythira::stdexec_backend` for the same reason.

## Components and Interfaces

### Phase 0: Spike (Requirement 9)

Before committing to exact adaptor code, confirm against the actually
vendored Boost.Thread version (not just the source inspection already done
while writing this document):
- `BOOST_THREAD_VERSION=4` alone enables `then()`, `when_all`, `when_any`,
  and the executor-taking `then(Ex&, F)` overload, with no additional macro
  needed (Requirement 1.2's fallback path if not).
- `then()`'s callback signature — confirmed by inspection to receive the
  completed future by value (`BOOST_THREAD_FUTURE<R>` moved in), not the
  unwrapped `R` — holds for the installed version.
- `boost::when_all`'s iterator-pair (runtime-sized) overload, not just the
  fixed-arity variadic one, is present and usable over a
  `std::vector<boost::future<T>>`.
- Record the exact Boost release/commit and minimum compiler versions
  validated, in `spike-notes.md`.

### Phase 1: `BOOST_THREAD_VERSION` Wiring (Requirement 1)

```cmake
# CMakeLists.txt, alongside the existing stdexec_FOUND block
#
# boost-future-backend spec: BOOST_THREAD_VERSION is a project-wide ABI
# setting (mixing TUs compiled with different values is undefined
# behavior for any boost::future/boost::promise object crossing a TU
# boundary), so it is applied exactly once here as an INTERFACE compile
# definition on network_simulator — the same mechanism KYTHIRA_HAS_STDEXEC
# already uses — rather than left to individual files to get right.
if(KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "boost" OR KYTHIRA_BUILD_BOOST_FUTURE_BACKEND)
    target_compile_definitions(network_simulator INTERFACE
        BOOST_THREAD_VERSION=4
        KYTHIRA_HAS_BOOST_FUTURE
    )
endif()
```

A separate `KYTHIRA_BUILD_BOOST_FUTURE_BACKEND` option (default `OFF`,
forced `ON` when `KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "boost"`) lets the
backend's own test suite build and run even when it is not the *default*
backend — mirroring how `stdexec_FOUND` alone (independent of
`KYTHIRA_DEFAULT_FUTURE_BACKEND`) already gates the `stdexec` backend's own
targets.

### Phase 2: `Try<T>` and `SemiPromise<T>`/`Promise<T>` (Requirements 2, 3)

**Try<T>**: a thin wrapper around `boost::future<T>`'s already-ready state
— constructed from a ready `boost::future<T>` (never held long-term itself,
matching the Folly backend's `Try<T>` being a snapshot, not a live handle):

```cpp
namespace kythira::boost_backend {

template<typename T>
class Try {
public:
    explicit Try(boost::future<T> f);  // precondition: f.is_ready()
    [[nodiscard]] auto hasValue() const -> bool;
    [[nodiscard]] auto hasException() const -> bool;
    auto value() -> T&;
    auto exception() const -> std::exception_ptr;
private:
    // get_exception_ptr() first (Requirement 2.2 — no throw/catch round
    // trip to inspect a result the caller may only want the exception
    // from), falling back to .get() only when the value path is taken.
    std::variant<T, std::exception_ptr> _state;
};

}  // namespace kythira::boost_backend
```

**SemiPromise<T>/Promise<T>**: direct wraps, no bridge primitive:

```cpp
namespace kythira::boost_backend {

template<typename T>
class SemiPromise {
public:
    auto setValue(T value) -> void { _p.set_value(std::move(value)); }
    auto setException(std::exception_ptr ex) -> void { _p.set_exception(ex); }
    [[nodiscard]] auto isFulfilled() const -> bool;  // tracked separately —
        // boost::promise has no direct query; wrapper tracks a bool set
        // alongside each setValue/setException call, matching how the
        // concept's isFulfilled() is used (post-hoc inspection, not a
        // pre-condition check racing the actual fulfillment).
protected:
    boost::promise<T> _p;
};

template<typename T>
class Promise : public SemiPromise<T> {
public:
    auto getFuture() -> Future<T> { return Future<T>{this->_p.get_future()}; }
    auto getSemiFuture() -> Future<T> { return getFuture(); }  // no
        // separate "semi future" type on this backend, same simplification
        // the concept already allows (semi_promise doesn't require
        // getFuture() to exist at all).
};

}  // namespace kythira::boost_backend
```

Double-fulfillment (Requirement 3.3) and broken-promise (Requirement 3.4)
both fall out of `boost::promise`'s own documented behavior
(`boost::promise_already_satisfied`, future ready with
`boost::broken_promise`) — the wrapper only needs to normalize these
Boost-specific exception types to `std::exception_ptr` at the boundary,
which they already are (both derive from `std::exception`).

### Phase 3: `Future<T>` and Continuations (Requirements 4, 6)

```cpp
namespace kythira::boost_backend {

template<typename T>
class Future {
public:
    auto get() && -> T { return std::move(_f).get(); }
    [[nodiscard]] auto isReady() const -> bool { return _f.is_ready(); }
    auto wait(std::chrono::milliseconds timeout) -> bool {
        return _f.wait_for(boost::chrono::milliseconds(timeout.count()))
               == boost::future_status::ready;
    }

    // thenValue: then()'s callback receives the completed
    // boost::future<T> itself, not T — the adaptor below unwraps it and
    // normalizes an exception path into thenError's own callback shape
    // (Requirement 6.1/6.2), and flattens Future<U>-returning callbacks
    // explicitly since then() itself does not (Requirement 6.3).
    template<typename F>
    auto thenValue(F&& func) -> Future<std::invoke_result_t<F, T>>;
    template<typename F>
    auto thenError(F&& func) -> Future<T>;
    template<typename F>
    auto ensure(F&& func) -> Future<T>;

    // via: then(Ex&, F) overload, requires BOOST_THREAD_PROVIDES_EXECUTORS
    // (auto-enabled at BOOST_THREAD_VERSION>=4, Requirement 1.2).
    auto via(boost::executors::executor& ex) -> Future<T>;

    // delay/within: no Boost.Thread primitive (Requirement 6.5) — backed
    // by a small boost::asio::steady_timer-driven promise (Phase 4).
    auto delay(std::chrono::milliseconds d) -> Future<T>;
    auto within(std::chrono::milliseconds timeout) -> Future<T>;

private:
    boost::future<T> _f;
};

}  // namespace kythira::boost_backend
```

### Phase 4: Timer-Backed `delay`/`within` (Requirement 6.5)

```cpp
namespace kythira::boost_backend::detail {

// One small, lazily-started io_context + thread, shared by every
// delay()/within() call in the process — not one thread per call. Owns
// no application state; only ever schedules a timer expiry that fulfills
// a boost::promise<T> (for delay, with the eventual value already known;
// for within, racing the original future's own completion).
class timer_service {
public:
    static auto instance() -> timer_service&;  // Meyers singleton
    auto schedule_after(std::chrono::milliseconds d, std::function<void()> cb) -> void;
private:
    boost::asio::io_context _ctx;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work;
    std::thread _thread{[this] { _ctx.run(); }};
};

}  // namespace kythira::boost_backend::detail
```

`within(timeout)` races the original future's completion against the timer:
whichever fulfills first wins, using the same `boost::promise` exactly-once
semantics Phase 2 already relies on — no separate cancellation mechanism is
needed since the loser's continuation, if it fires at all, is simply
discarded (the timer keeps a `weak_ptr` to the shared promise state so a
timer firing after the real completion is a harmless no-op, not a second
fulfillment attempt).

### Phase 5: `FutureFactory`/`FutureCollector` (Requirements 5, 7)

**FutureFactory**: direct forwarding to `boost::make_ready_future`/
`boost::make_exceptional_future` — no bridge needed, these are already the
pull-model-free, immediately-ready case.

**FutureCollector**:
- `collectAll` → `boost::when_all` over the input vector (iterator-pair
  overload, confirmed present per the Phase 0 spike), converting the
  resulting future-of-vector into `std::vector<Try<T>>` at the boundary.
- `collectAny` → `boost::when_any`, then scanning the returned collection
  for the one member that is ready, converting it to `(index, Try<T>)`.
- `collectAnyWithoutException` → `collectAny` in a loop, discarding
  failures and re-running over the remaining pool until a success is found
  or the pool is exhausted (Requirement 7.3 — Boost has no single
  primitive for this).
- `collectN` → repeated `collectAny`-style selection, removing each
  selected future from the pool before the next round, until N results are
  collected (Requirement 7.4 — same reasoning).

Both `collectAnyWithoutException` and `collectN`'s loop-based
implementations are the same shape `stdexec-future-backend`'s design.md
already used for its own `collectAny`/`collectN` fallback (there, over
`single_shot_channel`; here, over `boost::when_any`) — reusing an
already-reviewed pattern rather than inventing a new one.

### Backend Selection (Requirement 8)

```cpp
// CMakeLists.txt
set_property(CACHE KYTHIRA_DEFAULT_FUTURE_BACKEND PROPERTY STRINGS folly stdexec boost)
if(NOT KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "folly"
   AND NOT KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "stdexec"
   AND NOT KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "boost")
    message(FATAL_ERROR "KYTHIRA_DEFAULT_FUTURE_BACKEND must be \"folly\", \"stdexec\", or \"boost\", got \"${KYTHIRA_DEFAULT_FUTURE_BACKEND}\"")
endif()
if(KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "boost")
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_FUTURE_BACKEND_BOOST)
endif()
```

```cpp
// include/raft/future_default.hpp — extended, not replaced
#include "future.hpp"
#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)
#include "future_stdexec.hpp"
#elif defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include "future_boost.hpp"
#endif

namespace kythira {
#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)
template<typename T> using future_default = stdexec_backend::Future<T>;
#elif defined(KYTHIRA_FUTURE_BACKEND_BOOST)
template<typename T> using future_default = boost_backend::Future<T>;
#else
template<typename T> using future_default = Future<T>;
#endif
}  // namespace kythira
```

## Data Models

### Exception Representation

Same normalization point as both existing backends: `std::exception_ptr` at
the concept boundary. `boost::promise_already_satisfied` and
`boost::broken_promise` (both `std::exception`-derived) need no conversion
step beyond wrapping in `std::exception_ptr` the ordinary way
(`std::make_exception_ptr` or Boost's own already-thrown-and-caught path
inside `then()`/`get()`).

### Unit Representation

Reuses `kythira::unit` exactly as defined by the concept layer — no new
type. `boost::make_ready_future()`'s `void` overload converts to
`kythira::unit{}` at the same wrapper boundary point Requirement 5.3
specifies.

### `then()` Callback Adaptation

```
User calls:           future.thenValue(f)   // f: T -> U
Wrapper installs:      _f.then([f](boost::future<T> completed) -> U {
                            return f(completed.get());  // rethrows on
                                                          // exception,
                                                          // propagating
                                                          // through
                                                          // then()'s own
                                                          // future normally
                        })
```
`thenError`'s adaptor instead inspects `completed.has_exception()` before
calling `.get()`, calling the user's error callback with the normalized
`std::exception_ptr` on that path and passing the value through unchanged
on the success path. `ensure`'s adaptor runs its callback in both branches
before returning/rethrowing. All three are implemented as one shared
internal adaptor-building helper, not three independent `then()` calls
stacked on top of each other (which would each pay Boost's own future
allocation/scheduling overhead redundantly).

## Correctness Properties

*A property is a characteristic or behavior that should hold true across
all valid executions of a system.*

**Property 1: BOOST_THREAD_VERSION Single-Definition**
*For any* translation unit in the project that includes any Boost.Thread
future/promise header, either directly or transitively through
`network_simulator`, it should observe the same value of
`BOOST_THREAD_VERSION` as every other such translation unit in the same
build
**Validates: Requirement 1.1, 1.3, 1.4**

**Property 2: boost Try Fidelity**
*For any* ready `boost::future<T>` holding a value or an exception,
`Try<T>` constructed from it should report `hasValue()`/`hasException()`/
`value()`/`exception()` matching Folly `Try<T>` semantics for the
equivalent state
**Validates: Requirements 2.1, 2.2, 2.3, 2.4**

**Property 3: Promise Exactly-Once Fulfillment**
*For any* interleaving of `setValue`/`setException` calls (from any
thread) on a boost-backed `Promise<T>`, the associated future should
complete exactly once, and a second fulfillment attempt should surface as
a `std::exception_ptr`-carried error without corrupting the first result
**Validates: Requirements 3.2, 3.3**

**Property 4: Broken Promise Never Hangs**
*For any* promise destroyed before fulfillment while its future has
already been retrieved, the future should become ready with an error
rather than hang indefinitely
**Validates: Requirement 3.4**

**Property 5: boost Future Blocking Get Correctness**
*For any* boost-backed `Future<T>`, `get()` should block only until the
underlying `boost::future` completes and then return the value or rethrow
the exception, matching Folly `Future<T>::get()` for equivalent completions
**Validates: Requirement 4.1**

**Property 6: Continuation Callback Unwrapping**
*For any* `thenValue`/`thenError`/`ensure` chain, the user-supplied
callback should observe a plain unwrapped value or `std::exception_ptr` —
never a raw `boost::future<T>` — regardless of `then()`'s own
future-in-future-out native signature
**Validates: Requirements 6.1, 6.2, 6.6**

**Property 7: Continuation Flattening**
*For any* `thenValue` callback that itself returns a `Future<U>`, the
overall result should be `Future<U>`, never `Future<Future<U>>`
**Validates: Requirement 6.3**

**Property 8: Delay/Within Correctness**
*For any* `delay(d)` call, the resulting future should not become ready
before `d` has elapsed; *for any* `within(timeout)` call on a future that
does not complete in time, the result should be an error, and the timer
service should never leak a thread or fire a stale continuation after the
real completion already occurred
**Validates: Requirement 6.5**

**Property 9: Cross-Backend Concept Compliance**
*For any* concept in `include/concepts/future.hpp`, the boost-backed
wrapper types intended to satisfy it should do so, verified by
`static_assert`, exactly as already verified for the Folly and `stdexec`
backends
**Validates: Requirements 2.4, 3.5, 4.4, 5.4, 7.5, 10.1**

**Property 10: Collective Operation Fidelity**
*For any* collection of futures with a mix of successes, failures, and
completion orderings, `collectAll`/`collectAny`/`collectAnyWithoutException`/
`collectN` should produce results equivalent (same indices, same
values/exceptions, same ordering guarantees) to the Folly and `stdexec`
backends given the same inputs, even though internal scheduling mechanics
differ
**Validates: Requirements 7.1, 7.2, 7.3, 7.4, 10.4**

**Property 11: Backend Non-Interference**
*For any* attempt to combine a `boost_backend` type with a Folly- or
`stdexec`-backend-only type in the same expression, compilation should fail
**Validates: Requirement 8.5**

**Property 12: Backend Selection Isolation**
*For any* change to `KYTHIRA_DEFAULT_FUTURE_BACKEND` involving `boost`,
templated core code should compile and behave identically, and only
`kythira::future_default`-using call sites should change behavior
**Validates: Requirements 8.4, 8.6**

## Error Handling

### Exception Safety Guarantees

Same three-tier structure (basic/strong/no-throw) as both existing
backends. The `timer_service` (Phase 4) adds one new consideration: its
background thread must be joinable and joined cleanly at static-destruction
time — implemented via the `executor_work_guard` releasing and the
`io_context` being given a chance to drain in the singleton's destructor,
the same pattern any long-lived `boost::asio::io_context`-owning singleton
in this codebase would need, not specific to futures.

### Cancellation

Boost.Thread's futures have no first-class cancellation channel distinct
from Folly's (both lack `stdexec`'s "stopped" signal) — no new mapping is
needed here the way `stdexec-future-backend`'s design.md needed one for
`stdexec`'s stopped-signal-to-exception mapping. `within(timeout)`'s
"timed out" case surfaces as an ordinary `std::exception_ptr`-carried
timeout error, consistent with the Folly backend's own `within()` behavior.

### Broken Promise / Timer Cleanup

Covered by Property 4 (broken promise) and Property 8 (timer never leaks or
double-fires) above — both are `boost::promise`'s and the timer's own
exactly-once semantics respectively, not a new mechanism this design
invents.

## Testing Strategy

### Dual Testing Approach

Same as both existing backends: unit tests for specific examples/edge
cases, property-based tests (Boost.Test, minimum 100 iterations) for
universal properties, tagged `**Feature: boost-future-backend, Property N:
{property_text}**`.

### New Test Files (mirroring existing Folly/stdexec suite naming)

- `tests/boost_future_concept_compliance_property_test.cpp`
- `tests/boost_promise_concept_compliance_property_test.cpp`
- `tests/boost_semi_promise_concept_compliance_property_test.cpp`
- `tests/boost_try_concept_compliance_property_test.cpp`
- `tests/boost_future_collector_property_test.cpp` — `collectAll`/
  `collectAny`/`collectAnyWithoutException`/`collectN` fidelity
  (Property 10)
- `tests/boost_future_continuation_property_test.cpp` — `thenValue`/
  `thenError`/`ensure`/flattening (Properties 6, 7)
- `tests/boost_future_delay_within_property_test.cpp` — the new
  timer-backed primitives get their own dedicated suite given they're the
  highest-risk new code on this backend (Property 8), including specific
  cases: `delay` completing no earlier than requested, `within` racing a
  slow future to a timeout, `within` racing a fast future to a normal
  completion, repeated delay/within calls not leaking the shared timer
  thread.
- `tests/backend_non_interference_compile_fail_test.cpp` — extended
  (not duplicated) with `boost_backend` cases alongside the existing
  `stdexec_backend` ones (Property 11).

### Concept Validation

`static_assert` blocks at the end of the new `include/raft/future_boost.hpp`,
mirroring the pattern at the end of both existing backend headers, checked
for `int`, `void`, `std::string`, and a custom struct — the same type
matrix as the existing files, for direct comparability.

### Cross-Backend Fidelity

Extends the existing cross-backend comparison approach (already present for
Folly vs. `stdexec`) to include `boost` as a third column: the same
input/operation pair run through all three backends should produce
equivalent externally-observable results (Property 10).

### Execution

All new tests run exclusively through CTest, with labels `boost-future` and
`future-backend` so `ctest -L boost-future` runs just this suite, and
`ctest -LE boost-future` excludes it — mirroring the existing `stdexec`
label convention exactly.

## Non-Goals

- **Converting any existing production call site** from Folly or
  `stdexec` to `boost`.
- **Removing or making optional-only the Folly or `stdexec` dependency.**
- **`boost::fibers`, Boost.Cobalt, or any other Boost async facility**
  besides `boost::thread`'s future/promise extension surface.
- **Priority-based execution or work-stealing scheduling** — the
  `boost::executors::executor` shim is a thin `via()` target, not a new
  scheduling policy.
- **Treating `then()`/`when_all`/`when_any` as a permanently stable API** —
  Requirement 11.5 explicitly documents this as a known, accepted
  maintenance risk rather than pretending otherwise.
