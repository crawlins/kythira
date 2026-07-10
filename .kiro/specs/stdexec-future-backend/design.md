# Design Document

## Overview

This design adds a second implementation of the `kythira` future/promise/executor family, backed by `stdexec` (NVIDIA's reference implementation of C++26's `std::execution`, P2300), alongside the existing Folly-backed implementation. It does not touch any production call site: `include/raft/future.hpp` (Folly) keeps its current public API and remains the default. The work has two parts that must happen in order:

1. **Regenericize the concepts** in `include/concepts/future.hpp` so they no longer name `folly::exception_wrapper` or `folly::Unit` directly. This is a prerequisite — a `stdexec` type cannot satisfy a concept that requires a Folly type in its signature no matter how it's implemented.
2. **Implement a `stdexec`-backed second family** (`Try`, `SemiPromise`, `Promise`, `Future`, `Executor`/scheduler shim, `FutureFactory`, `FutureCollector`) in a new header, satisfying the now-generic concepts, in a distinct namespace so the two backends can never be silently mixed.

The central design problem is that Folly's `Promise`/`Future` pair is a **push** model — `promise.setValue(x)` is called from arbitrary code, at an arbitrary later time, often from a different thread than the one that created the promise (e.g. a network I/O completion callback fulfilling an RPC response promise). `stdexec` senders are a **pull** model — a sender describes work; nothing happens until it is connected to a receiver and started, and the sender itself decides when to signal completion by calling into the receiver. There is no first-class "let external code complete this later" primitive shipped by `stdexec`. Bridging this (Requirement 6) is the crux of the project; every other requirement is comparatively mechanical sender-adaptor plumbing once that bridge exists.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Regenericized Concept Layer (include/concepts/future.hpp)   │
│  try_type / semi_promise / promise / executor / keep_alive / │
│  future / future_factory / future_collector /                │
│  future_continuation / future_transformable                  │
│  — expressed with std::exception_ptr and kythira::unit only  │
└───────────────┬───────────────────────────┬───────────────────┘
                │ satisfies                  │ satisfies
┌───────────────▼───────────────┐ ┌──────────▼────────────────────┐
│ Folly Backend (existing,      │ │ stdexec Backend (new)          │
│ include/raft/future.hpp)      │ │ include/raft/future_stdexec.hpp│
│ namespace kythira { ... }     │ │ namespace kythira::stdexec_    │
│                                │ │           backend { ... }      │
│ Try/SemiPromise/Promise/      │ │ Try/SemiPromise/Promise/       │
│ Future/Executor/KeepAlive/    │ │ Future/single_shot_channel/    │
│ FutureFactory/FutureCollector │ │ scheduler_executor_shim/       │
│ wrap folly::* types           │ │ FutureFactory/FutureCollector  │
│                                │ │ wrap stdexec senders           │
└────────────────────────────────┘ └─────────────────────────────────┘
                │                              │
        folly::Future/Promise         stdexec::sender/receiver,
        folly::Executor                scheduler, when_all, then, ...
```

`kythira::unit` (Requirement 2) lives in the concept layer so both backends can reference it without depending on each other. Generic core code that is already templated on a future type (from the prior `folly-concept-wrappers` conversion, Requirement 28 in that spec) is unaffected by which backend is chosen — it only depends on the concepts.

### Why a new namespace instead of a compile-time `#ifdef` swap of `kythira::Future`

An `#ifdef`-selected single `kythira::Future` would mean every translation unit in a build silently gets one backend, and mixing backends (e.g. a test that wants to compare both) becomes impossible without separate compilation units and macro gymnastics. Two concrete namespaces, both satisfying the same concepts, let generic template code accept either, let non-generic code pick one explicitly, and let a single test binary exercise both side by side — at the cost of two symbol sets to maintain. This tradeoff is worth it given Requirement 12 explicitly wants both backends validated against each other in the same test suite.

## Components and Interfaces

### Phase 1: Concept Regenericization

**Before** (current `include/concepts/future.hpp`, Folly-specific):
```cpp
concept semi_promise = requires(P p, const P cp) {
    { cp.isFulfilled() } -> std::convertible_to<bool>;
    { p.setException(std::declval<folly::exception_wrapper>()) } -> std::same_as<void>;
} && (std::is_void_v<T> ? requires(P p) {
                           { p.setValue(folly::Unit{}) } -> std::same_as<void>;
                       } : requires(P p, T value) {
                           { p.setValue(std::move(value)) } -> std::same_as<void>;
                       });
```

**After**:
```cpp
struct unit {
    constexpr bool operator==(const unit&) const = default;
};

concept semi_promise = requires(P p, const P cp) {
    { cp.isFulfilled() } -> std::convertible_to<bool>;
    { p.setException(std::declval<std::exception_ptr>()) } -> std::same_as<void>;
} && (std::is_void_v<T> ? requires(P p) {
                           { p.setValue(unit{}) } -> std::same_as<void>;
                       } : requires(P p, T value) {
                           { p.setValue(std::move(value)) } -> std::same_as<void>;
                       });
```

The same substitution (`folly::exception_wrapper` → `std::exception_ptr`, `folly::Unit` → `kythira::unit`) applies to `try_type`, `future_factory`, and every other concept in the file. `include/raft/future.hpp`'s existing wrapper types already expose both a `folly::exception_wrapper` overload and a `std::exception_ptr` overload for `setException` (see `SemiPromise::setException`), so the Folly backend continues to satisfy the regenericized concepts with no change — this was verified by reading the existing wrapper implementation before committing to this design, not assumed.

The `executor` and `keep_alive` concepts are **not** regenericized to a common shape (Requirement 3). Folly's `.add(std::function<void()>)` and `stdexec`'s `schedule()`-returns-a-sender model describe different things; forcing a shared syntactic concept over both would either weaken the concept until it's meaningless or push the sender/receiver model through a callback-shaped hole, defeating the point of adopting `stdexec`. Instead:
- `executor`/`keep_alive` remain as-is (still Folly-shaped, still useful for existing code).
- A new `scheduler_executor_shim` in the `stdexec` backend additionally satisfies `executor`/`keep_alive` by wrapping `stdexec::sync_wait(stdexec::schedule(scheduler) | stdexec::then(func))` inside `.add()`. This is documented as a compatibility path with real overhead (it blocks a thread per submitted callback) — new code should use the underlying scheduler directly via `via(scheduler)` instead of going through `.add()`.

### Phase 2: stdexec Backend

**Try<T>** (Requirement 5): built directly, not derived from a sender — it's a plain tagged union of `T` and `std::exception_ptr` (or `std::variant<T, std::exception_ptr>` internally), since `stdexec` has no `Try`-equivalent type to wrap. A free function `into_try(sender) -> Try<T>` connects the sender to a receiver whose `set_value`/`set_error`/`set_stopped` populate a `Try<T>`, for use inside `sync_wait`-based `get()` and inside `collectAll`.

**single_shot_channel<T>** (Requirement 6) — the core new primitive, with no Folly equivalent to wrap:
```cpp
namespace kythira::stdexec_backend::detail {

template<typename T>
class single_shot_channel {
public:
    // Shared state, heap-allocated, kept alive by both the promise side
    // and the operation-state side via a small internal refcount —
    // needed because the promise may be destroyed (fulfilled or not)
    // independently of when/whether a receiver ever connects.
    struct shared_state { /* variant<monostate, T, std::exception_ptr>,
                              a std::atomic<state> for the fulfilled/
                              connected/started transitions, and a single
                              type-erased continuation slot filled in when
                              a receiver starts before fulfillment */ };

    auto set_value(T value) -> void;       // called from arbitrary thread
    auto set_error(std::exception_ptr) -> void;

    // Sender: connecting produces an operation_state that, on start(),
    // either completes synchronously (already fulfilled) or registers
    // its receiver's completion in shared_state for the fulfilling
    // thread to invoke.
    auto get_sender() -> /* single_shot_sender<T> */;
};

} // namespace kythira::stdexec_backend::detail
```
This is the same shape as a manual-reset single-consumer event, a well-known pattern in the `stdexec`/`libunifex` community for exactly this bridge; it is being hand-rolled here rather than assumed to exist in `stdexec` proper (a Phase 2 spike task confirms whether a suitable primitive already exists in the `exec` extensions before this is hand-rolled — see tasks.md). `SemiPromise<T>`/`Promise<T>` are thin wrappers over `shared_ptr<shared_state>`; `Future<T>` wraps the sender.

**Future<T> continuations** (Requirement 9): `thenValue` → `stdexec::then`; `thenError` → `stdexec::upon_error` with a `std::exception_ptr`-to-`std::exception_ptr` (or value) adaptor at the boundary since `stdexec`'s native error channel is `std::exception_ptr`-friendly but not guaranteed to only ever carry that type — the wrapper normalizes any error channel type to `std::exception_ptr` before calling the user's callback, matching the Folly wrapper's existing normalization behavior; `ensure` → a `stdexec` adaptor that runs on both the value and error paths (composed from `then` + `upon_error`, or `stdexec::finally` if the selected `stdexec` version has it — confirmed during implementation, not assumed here); `via(scheduler)` → the `stdexec` transfer/`continue_on` adaptor; `delay`/`within` → a timed scheduler, with a hand-rolled timed single-shot channel as fallback if no timed combinator exists in the vendored `stdexec` version (mirrors the Requirement 6 primitive, reused rather than duplicated).

**FutureFactory** (Requirement 8): `makeFuture(value)` → `Future<T>` wrapping `stdexec::just(value)`; `makeExceptionalFuture<T>(ex)` → wrapping `stdexec::just_error(ex)`; `makeReadyFuture()` → `Future<kythira::unit>` wrapping `stdexec::just(unit{})`. These are direct, no bridge needed — they're the pull model's native case.

**FutureCollector** (Requirement 10): `collectAll` → `stdexec::when_all` over the input senders, each first wrapped with `into_try` so an individual failure doesn't cancel siblings and result ordering is preserved (matching Folly's `collectAll` semantics, where the whole operation succeeds and gives you a vector of `Try<T>` regardless of individual failures). `collectAny`/`collectAnyWithoutException`/`collectN` need first-completed-wins semantics; if no ready-made combinator covers this in the vendored `stdexec`, they are implemented on top of `single_shot_channel` (Requirement 6): connect and start all input senders, each completion attempts to fulfill a shared `single_shot_channel<std::pair<size_t, Try<T>>>`, first writer wins, others are discarded — the same pattern `folly::collectAny` uses internally.

### Backend Selection (Requirement 11)

```cpp
// CMakeLists.txt
option(KYTHIRA_DEFAULT_FUTURE_BACKEND "folly or stdexec" "folly")

// generated or hand-maintained small header, e.g. include/raft/future_default.hpp
#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)
namespace kythira { template<typename T> using future_default = stdexec_backend::Future<T>; }
#else
namespace kythira { template<typename T> using future_default = Future<T>; }
#endif
```
Templated core code (already generic over a future type per the prior spec) never references `future_default` — it takes whatever `Future`-satisfying type its caller instantiates it with. `future_default` exists only for the remaining non-templated call sites that want "the project's chosen backend" without spelling out which.

## Data Models

### Exception Representation

Both backends normalize to `std::exception_ptr` at the concept boundary (Requirement 1). The Folly backend keeps `folly::exception_wrapper` internally and converts at its edge, exactly as it does today (`detail::to_std_exception_ptr` / `detail::to_folly_exception_wrapper` in `include/raft/future.hpp` already do this — reused, not reinvented). The `stdexec` backend uses `std::exception_ptr` throughout with no intermediate type, since `stdexec`'s error channel is designed around arbitrary error types with `std::exception_ptr` as the common case.

### Unit Representation

```cpp
namespace kythira {
struct unit {
    constexpr bool operator==(const unit&) const = default;
};
}
```
Deliberately not `std::monostate` (which works too, but a named `kythira::unit` matching Folly's `Unit` naming keeps the vocabulary consistent and gives a natural place to grow, if ever needed, without touching `std::monostate`'s shared meaning elsewhere in the codebase).

### single_shot_channel State Machine

```
        set_value/set_error (any thread)
             │
  ┌──────────▼──────────┐
  │ empty                │──start() before fulfillment──▶ waiting (receiver stored)
  └──────────┬────────────┘                                     │
             │ start() after fulfillment                        │ set_value/set_error
             ▼                                                  ▼
       complete synchronously                          complete via stored receiver
```
Both transitions into "complete" happen under a single atomic compare-exchange to guarantee exactly-once completion (Requirement 6.2/6.3) regardless of which side — fulfillment or connection — arrives second. Double-fulfillment (`set_value` called twice) is detected by the same compare-exchange failing on the second call and throws, matching `folly::Promise`'s documented behavior.

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid executions of a system.*

**Property 1: Concept Regenericization Preserves Folly Compliance**
*For any* wrapper type in the existing Folly backend that satisfied a concept in `include/concepts/future.hpp` before this feature, it should continue to satisfy the regenericized version of that concept, with identical runtime behavior
**Validates: Requirements 1.4, 1.5, 2.4**

**Property 2: Concept-Layer Folly Independence**
*For any* compilation unit that includes only `include/concepts/future.hpp`, compilation should succeed without transitively including any Folly header
**Validates: Requirement 1.4**

**Property 3: Unit Type Equivalence**
*For any* void-valued operation, results obtained through `kythira::unit` on either backend should be observably equivalent to the pre-existing `folly::Unit`-based behavior of the Folly backend
**Validates: Requirements 2.1, 2.2, 2.3, 2.4**

**Property 4: Executor/Scheduler Shim Correctness**
*For any* function submitted through `scheduler_executor_shim::add()`, the function should execute exactly once, on the wrapped scheduler's execution context, before `add()` returns
**Validates: Requirements 3.2, 3.4**

**Property 5: Optional Dependency Isolation**
*For any* build configuration where `stdexec` is not found, configuration and build of all Folly-backed and backend-independent targets should succeed unaffected
**Validates: Requirements 4.2, 4.3**

**Property 6: stdexec Try Fidelity**
*For any* sender completing with a value, error, or stopped signal, `into_try` should produce a `Try<T>` whose `hasValue()`/`hasException()`/`value()`/`exception()` reflect that completion exactly, matching Folly `Try<T>` semantics for the equivalent state
**Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.5**

**Property 7: Single-Shot Channel Exactly-Once Completion**
*For any* interleaving of `set_value`/`set_error` calls (from any thread) and receiver connect/start calls, the channel should complete its receiver exactly once, with the value/error from whichever fulfillment call is accepted, and a second fulfillment attempt should throw without corrupting the first result
**Validates: Requirements 6.2, 6.3, 6.4**

**Property 8: Single-Shot Channel Broken-Promise Semantics**
*For any* promise destroyed before fulfillment while its future is already connected and started, the operation should complete with an error, never hang
**Validates: Requirement 6.5**

**Property 9: stdexec Future Blocking Get Correctness**
*For any* `stdexec`-backed `Future<T>`, `get()` should block only until the underlying sender completes and then return the value or rethrow the exception, matching Folly `Future<T>::get()` for equivalent completions
**Validates: Requirements 7.1, 7.2, 7.3**

**Property 10: Cross-Backend Concept Compliance**
*For any* concept in the regenericized `include/concepts/future.hpp`, both the Folly-backed and `stdexec`-backed wrapper types intended to satisfy it should do so, verified by `static_assert`
**Validates: Requirements 5.5, 6.6, 7.4, 7.5, 8.4, 10.5, 12.1**

**Property 11: Factory Operation Fidelity**
*For any* value or exception, `stdexec`-backed `FutureFactory` methods should produce futures immediately ready with that exact value or exception, matching Folly backend behavior for the same input
**Validates: Requirements 8.1, 8.2, 8.3**

**Property 12: Continuation and Transformation Fidelity**
*For any* chain of `thenValue`/`thenTry`/`thenError`/`ensure`/`via`/`delay`/`within` operations applied identically to both backends, the externally observable result (final value, exception, or timeout outcome) should be equivalent, even though internal scheduling mechanics differ
**Validates: Requirements 9.1, 9.2, 9.3, 9.4, 9.5, 9.6, 12.4**

**Property 13: Collective Operation Fidelity**
*For any* collection of futures with a mix of successes, failures, and completion orderings, `collectAll`/`collectAny`/`collectAnyWithoutException`/`collectN` should produce equivalent results (same indices, same values/exceptions, same ordering guarantees) on both backends
**Validates: Requirements 10.1, 10.2, 10.3, 10.4, 10.5, 12.4**

**Property 14: Backend Non-Interference**
*For any* attempt to combine a `stdexec_backend` type with a Folly-backend-only type in the same expression (e.g. `stdexec_backend::Future::via` given a Folly `Executor*`), compilation should fail
**Validates: Requirement 11.4**

**Property 15: Backend Selection Isolation**
*For any* change to `KYTHIRA_DEFAULT_FUTURE_BACKEND`, templated core code should compile and behave identically, and only `kythira::future_default`-using call sites should change behavior
**Validates: Requirements 11.2, 11.5**

## Error Handling

### Exception Safety Guarantees

Same three-tier guarantee structure as the Folly backend (basic / strong / no-throw for destructors and moves), extended to the new `single_shot_channel`:
- **No-throw**: `single_shot_channel` destruction and the atomic state transition itself never throw.
- **Strong**: `set_value`/`set_error` either fully install the result (and release any waiting receiver) or, on a losing race (already fulfilled), throw without having mutated shared state.
- **Basic**: A receiver that throws inside its own continuation does not corrupt the channel's internal state for any other observer — there is only ever one observer per single-shot channel by construction, so this reduces to "the channel is not reused after its one completion," enforced at compile time by the sender being single-shot (move-only, not copyable).

### Cancellation

`stdexec`'s "stopped" completion signal has no direct Folly equivalent (Folly futures don't have a first-class cancellation channel in the same sense). The design maps "stopped" to a project-defined `kythira::stdexec_backend::operation_cancelled` exception surfaced through the normal `std::exception_ptr` error channel, so code written against the concepts (which only know about value/exception, not a third "stopped" state) sees consistent behavior on both backends. This is a deliberate simplification: it discards `stdexec`'s richer cancellation semantics at the concept boundary in exchange for backend interchangeability. Code that wants full `stdexec` cancellation semantics should use the `stdexec_backend` types directly with native `stdexec` algorithms, bypassing the `kythira` concept wrappers.

### Broken Promise / Timeout Cleanup

Timeout paths (`wait(timeout)`, `within(timeout)`) must not leak the underlying `stdexec` operation state when the caller stops waiting before completion. The design keeps the operation state alive (via the same `shared_ptr<shared_state>` used for the single-shot channel) until the *later* of "caller stopped waiting" and "producer completed," discarding the result if the caller already left, rather than trying to synchronously tear down an in-flight operation state — the latter is a common source of use-after-free bugs in hand-rolled sender/receiver code and is explicitly avoided here.

## Testing Strategy

### Dual Testing Approach

Same as the `folly-concept-wrappers` spec: unit tests for specific examples/edge cases, property-based tests (Boost.Test, minimum 100 iterations) for universal properties, tagged `**Feature: stdexec-future-backend, Property N: {property_text}**`.

### New Test Files (mirroring existing Folly suite naming)

- `tests/stdexec_future_concept_compliance_property_test.cpp`
- `tests/stdexec_promise_concept_compliance_property_test.cpp`
- `tests/stdexec_semi_promise_concept_compliance_property_test.cpp`
- `tests/stdexec_executor_concept_compliance_property_test.cpp`
- `tests/stdexec_concept_wrappers_unit_test.cpp`
- `tests/stdexec_concept_wrappers_interoperability_property_test.cpp` — cross-backend fidelity (Properties 6, 9, 11, 12, 13)
- `tests/single_shot_channel_property_test.cpp` — the new primitive gets its own dedicated, higher-iteration-count suite given it's the highest-risk new code (Properties 7, 8), including specific interleavings: fulfillment-before-connect, connect-before-fulfillment, concurrent fulfillment-from-multiple-threads-races-detected, destroy-before-fulfill.
- `tests/backend_non_interference_compile_fail_test.cpp` — a small set of `static_assert(!requires{...})` checks (not a runtime test) verifying Property 14 rejects cross-backend mixing at compile time.

### Concept Validation

`static_assert` blocks at the end of the new `include/raft/future_stdexec.hpp`, mirroring the pattern at the end of the existing `include/raft/future.hpp`, checked against the regenericized concepts for `int`, `void`, `std::string`, and a custom struct — same type matrix as the existing file, for direct comparability.

### Execution

All new tests run exclusively through CTest, per `test-execution-standards.md`, with labels `stdexec` and `future-backend` added so `ctest -L stdexec` can run just this suite, and `ctest -LE stdexec` can exclude it entirely on machines without the optional `stdexec` dependency installed.
