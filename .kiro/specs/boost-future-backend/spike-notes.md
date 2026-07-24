# Spike Notes — Boost Future Backend

**Date**: 2026-07-23
**Boost version**: 1.89.0 (`BOOST_VERSION 108900`, vcpkg `boost-thread`/`boost-asio`)
**Compiler validated**: `clang++-18`, `-std=c++23`, `-O3 -DNDEBUG` (this
project's actual `certificate_authority` target flags, extracted from
`build-clang/CMakeFiles/certificate_authority.dir/flags.make` rather than
guessed, after an initial ad hoc flag set produced an unrelated
`boost::mpl`/`na` parse failure — see "False start" below)

## Method

A throwaway standalone `.cpp` (not part of the CMake build), compiled and
run directly against the vendored `vcpkg_installed/x64-linux` Boost
headers/static libs, confirming each design.md assumption empirically
rather than from source inspection alone.

## Findings

### 1. `BOOST_THREAD_PROVIDES_EXECUTORS` is NOT auto-defined at `BOOST_THREAD_VERSION=4` — correction to design.md

**This is the one real design correction from the spike.** Requirements.md/
design.md assumed (from source inspection alone, before this spike)
that `BOOST_THREAD_VERSION=4` auto-defines both
`BOOST_THREAD_PROVIDES_FUTURE_CONTINUATION` and
`BOOST_THREAD_PROVIDES_EXECUTORS`. A direct preprocessor check
(`#if defined BOOST_THREAD_PROVIDES_EXECUTORS`, printed via
`#pragma message`) after `#include <boost/thread/future.hpp>` with only
`BOOST_THREAD_VERSION` defined to `4` shows:

- `BOOST_THREAD_PROVIDES_FUTURE_CONTINUATION`: **defined** ✓ (matches
  design.md's assumption)
- `BOOST_THREAD_USES_MOVE`: **defined** ✓
- `BOOST_THREAD_PROVIDES_EXECUTORS`: **NOT defined** ✗

Re-reading `boost/thread/detail/config.hpp` with this evidence in hand:
the auto-definition block for `BOOST_THREAD_PROVIDES_EXECUTORS` is
actually gated on `#if BOOST_THREAD_VERSION>=5` (line 339), not `>=4` —
my original source read (while writing design.md) mis-attributed which
`#if` block enclosed it, having correctly found the `#define
BOOST_THREAD_PROVIDES_EXECUTORS` line but not walked far enough up to see
its real guard was a *separate* `>=5` block, not the `>=4` block just
above it that encloses `BOOST_THREAD_USES_MOVE` and several other
`PROVIDES_*` macros.

**Resolution**: keeps `BOOST_THREAD_VERSION=4` (not `5` — no other reason
found to need version 5's changes, and staying at the lower, longer-track-
recorded version is the more conservative choice) and additionally defines
`BOOST_THREAD_PROVIDES_EXECUTORS` explicitly, exactly as
Requirements.md 1.2's fallback clause already anticipated for exactly this
possibility. `include/raft/future_boost.hpp`'s macro block becomes:
```cpp
#define BOOST_THREAD_VERSION 4
#define BOOST_THREAD_PROVIDES_EXECUTORS
```
and the CMake wiring (design.md's Phase 1) defines both as
`target_compile_definitions`, not just `BOOST_THREAD_VERSION`.

### 2. `then()`'s callback receives the completed `boost::future<T>` itself — confirmed

```cpp
auto f3 = f2.then([](boost::future<int> completed) -> int {
    static_assert(std::is_same_v<decltype(completed), boost::future<int>>, ...);
    return completed.get() + 1;
});
```
Compiles and the `static_assert` passes. Confirms design.md's Requirement
6.1 assumption exactly — the wrapper's `thenValue` adaptor must unwrap
this itself, `then()` gives no shortcut.

### 3. `boost::when_all`/`boost::when_any` iterator-pair (runtime-sized) overloads — confirmed present and usable

Both compile and run correctly over a `std::vector<boost::future<int>>`
built at runtime (3 elements), confirming the overload needed for
`collectAll`/`collectN` over an arbitrary-length input exists in this
version, distinct from the fixed-arity variadic overload.

### 4. Executor-taking `then(Ex&, F)` overload — confirmed, once `BOOST_THREAD_PROVIDES_EXECUTORS` is explicitly defined

Works correctly against `boost::basic_thread_pool` once Finding 1's
correction is applied (`basic_thread_pool.hpp`'s own class definition is
itself gated on `BOOST_THREAD_PROVIDES_FUTURE_CONTINUATION`,
`BOOST_THREAD_PROVIDES_EXECUTORS`, **and** `BOOST_THREAD_USES_MOVE` all
three being defined — without the executors fix, `basic_thread_pool`
doesn't exist at all, not merely lacking the `then(Ex&, F)` overload).

### 5. `make_ready_future`/`make_exceptional_future`, `wait_for`/`is_ready` — confirmed, exactly as documented

All behave as design.md's Phase 5/Requirement 4 describe. No surprises.

## False start (recorded for future reference, not a real finding)

An initial ad hoc compile (before extracting this project's actual
`certificate_authority` flags) used `-stdlib=libstdc++` explicitly and
plain `-I` instead of `-isystem` for the vcpkg include path, and failed
deep inside `boost/mpl/if.hpp` with `'na' does not refer to a value` —
a `boost::date_time`/`boost::mpl` chain reached via
`boost/thread/thread_time.hpp`. Switching to this project's actual
verified flags (`-O3 -DNDEBUG -std=c++23`, `-isystem` for the vcpkg
include dir, no explicit `-stdlib`) made this disappear entirely with no
other change. Not investigated further since it doesn't reproduce under
the real build configuration — recorded here only so a future
implementer hitting the same `na`/`mpl` error from a manual/ad hoc
compile knows to check their invocation against
`build-clang/CMakeFiles/<any target>/flags.make` before assuming it's a
real Boost/C++23 incompatibility.

### 6. `boost::exception_ptr` is NOT `std::exception_ptr`, and the naive bridge silently breaks rethrow — found during Phase 2, folded back in here

Not anticipated by design.md at all (design.md's "Exception Representation"
section assumed a `std::make_exception_ptr`-style direct conversion would
just work, without having checked). `boost::promise<T>::set_exception`
takes `boost::exception_ptr` (`boost/exception/detail/exception_ptr.hpp`),
a distinct type with an *implicit* converting constructor from
`std::exception_ptr` — but using that implicit conversion directly
(`promise.set_exception(a_std_exception_ptr)`) compiles fine and then
silently does the wrong thing at runtime: the future's `get()` throws a
`boost::exception_detail::clone_impl<std::exception_ptr>` wrapper object
instead of the original exception, which no ordinary
`catch (const std::exception&)` catches (reproduced: an uncaught
`clone_impl` terminates the process instead of being caught).

The correct bridge in both directions is a genuine catch-and-rethrow, not
a direct conversion:
```cpp
// std::exception_ptr -> boost::promise::set_exception
void set_exception_from_std(boost::promise<T>& p, std::exception_ptr ep) {
    try { std::rethrow_exception(ep); }
    catch (...) { p.set_exception(boost::current_exception()); }
}

// boost::future::get_exception_ptr() -> std::exception_ptr
auto to_std_exception_ptr(const boost::exception_ptr& bep) -> std::exception_ptr {
    try { boost::rethrow_exception(bep); }
    catch (...) { return std::current_exception(); }
    return nullptr;  // bep was empty
}
```
Verified end to end: `set_exception_from_std` into a promise, `wait()`,
`get_exception_ptr()`, `to_std_exception_ptr()`, `rethrow_exception()` —
the original `std::runtime_error` and its message survive the full round
trip intact, caught by its concrete type (`catch (const
std::runtime_error&)`), not degraded to a generic wrapper. This is the
same catch-and-rethrow shape `include/raft/future.hpp`'s own
`detail::to_std_exception_ptr`/`to_folly_exception_wrapper` already use
for the Folly backend's equivalent boundary — consistent with existing
project precedent, not a new pattern. `include/raft/future_boost.hpp`
uses exactly these two functions at every `Try<T>`/`SemiPromise<T>`
exception boundary; no code path may use the implicit
`boost::exception_ptr(std::exception_ptr)` constructor directly.

### 7. `then()` does not auto-flatten a future-returning callback, and discarding a `then()` call's return value does not cancel it — both confirmed

Two more design.md assumptions checked empirically during Phase 3/5 rather
than trusted from documentation: a `then()` callback returning
`boost::future<U>` produces `boost::future<boost::future<U>>>`, never
auto-unwrapped — confirming `thenValue`'s Future-returning overload really
does need to hand-build the flattening bridge (`extract_boost_future()` +
a nested `then()` fulfilling a bridging `Promise<U>`, not
`inner.thenValue()/thenError()` directly, which is ill-formed when
`U=void`). Separately, calling `.then(...)` and discarding the returned
`boost::future` still lets that continuation run to completion — confirmed
by observing the discarded continuation's side effect fire — which is what
makes the recursive, fire-and-forget `.then()` chains in
`FutureCollector::collectAnyWithoutException`/`collectN` and the
`thenValue` flattening overloads safe.

### 8. `boost_backend::Future::via()`'s templated executor parameter defeats SFINAE-based cross-backend detection — found while writing `backend_non_interference_compile_fail_test.cpp`

Unlike the Folly and stdexec backends' `via()` (each fixed to one concrete
executor/scheduler-handle type), `boost_backend::Future<T>::via()` is
`template<typename Ex> auto via(Ex& ex)` — a deliberate design choice
(Finding 4 above: Boost.Thread's own concrete executors like
`basic_thread_pool` don't inherit from a common `executor` base, so
`then(Ex&, F)` itself is duck-typed). Consequence: a
`static_assert(!requires(F f, Arg arg) { f.via(arg); })`-style check
compiles as **well-formed** for *any* `Arg` — including a Folly
`Executor&` or a `stdexec_backend::scheduler_handle&` — because template
argument deduction for `Ex` always succeeds at the call site; the real
mismatch only hard-errors once `via()`'s body (`_f.then(ex, ...)`) is
instantiated, which an unevaluated `requires`-expression never triggers
(function bodies aren't instantiated in an unevaluated operand). Confirmed
directly: the two `can_via<boost_backend::Future<int>, kythira::Executor>`-
style assertions failed to compile (evaluated true, not false) when first
added. Not a real backend-interference bug — passing the wrong executor to
`boost_backend::Future::via()` still fails to compile if anyone actually
writes it — just not detectable through this particular SFINAE idiom, so
`backend_non_interference_compile_fail_test.cpp` omits `can_via` checks
for the boost backend and keeps only the `FutureCollector::collectAll`
(genuinely deduction-based) and `std::is_convertible_v` checks for it.

## Conclusion

Every design.md assumption holds except Finding 1, which is a small,
already-anticipated correction (explicit `BOOST_THREAD_PROVIDES_EXECUTORS`
alongside `BOOST_THREAD_VERSION=4`, not relying on auto-definition).
Findings 6-8 were discovered during implementation/test-writing rather
than the initial Phase 0 spike proper, but are recorded here as the same
"verify empirically, don't assume" evidence trail continues to apply.

The full `include/raft/future_boost.hpp` implementation passed a 20/20
throwaway runtime smoke test, then a full CMake/CTest round trip: two new
property-test binaries (`boost_future_concept_compliance_property_test`,
`boost_future_continuation_and_collector_property_test`, 31 test cases
total) plus the extended `backend_non_interference_compile_fail_test`, all
green under `-DKYTHIRA_DEFAULT_FUTURE_BACKEND=boost` and — for
`backend_non_interference_compile_fail_test`, deliberately unconditional —
also under a Folly-only configuration with both optional backends absent.
