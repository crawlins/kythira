# Phase 0 Spike Notes

Findings from vendoring a throwaway `stdexec` checkout and probing its real
API surface, ahead of committing to the Phase 2/3 implementation details in
`design.md`. Method: installed the `stdexec` vcpkg port to a scratch
directory outside this project's manifest (`vcpkg install stdexec --classic
--triplet x64-linux --x-install-root=<scratch>`), then compiled and ran a
small probe program (`probe.cpp`, not checked in) against the real installed
headers with both compilers this project's CI already uses.

## Pinned version

- vcpkg port: `stdexec`, port version-date `2026-05-25`
- Upstream commit: `NVIDIA/stdexec@fee4d651494014610a277540f209cae56011e47f`
  (pulled via `vcpkg_from_github` in `/opt/vcpkg/ports/stdexec/portfile.cmake`
  at spike time)
- This is the version Task 1 (`vcpkg.json`) and Requirement 13.5's version
  record should target, updated if the vcpkg registry's pinned commit moves
  before implementation actually lands.

## Compiler compatibility — CONFIRMED

Both compilers this project's CI matrix already uses compile and run the
probe cleanly against `-std=c++23` (this project's `CMAKE_CXX_STANDARD`):

- `g++-13` (13.3.0, Ubuntu 24.04) — compiles, links, runs correctly
- `clang++-18` (18.1.3, Ubuntu 24.04, `-stdlib=libstdc++`) — compiles,
  links, runs correctly

No minimum-version issue found for either compiler already pinned by this
project's CI (`.github/workflows/ci.yml`'s `matrix.compiler`). No new
compiler version requirement is introduced by adding `stdexec`.

## (b) First-completed-wins combinator — EXISTS

`exec::when_any(senders...)` (`exec/when_any.hpp`) is real, available, and
was exercised successfully in the probe: given a fast and a slow sender on
the same scheduler, `when_any` completes with the fast one's result.

**Design impact**: Task 20 (`collectAny`/`collectAnyWithoutException`)
should use `exec::when_any` directly. The `single_shot_channel`-based
fallback design.md sketches as a fallback is NOT needed for the base
`collectAny` case. It may still be needed for `collectAnyWithoutException`'s
"continue past failures" semantics, since `when_any` alone completes (and
cancels siblings) on the *first* completion regardless of whether it's a
success or failure — `collectAnyWithoutException` needs first-*success*,
which likely needs `when_any` wrapped per-input with an adaptor that turns a
failure into "never completes" (e.g. via `let_error` chaining into a sender
that never sets a value, letting `when_any` skip past it) rather than a
full hand-rolled channel. Confirm the exact composition during Task 20
implementation.

## (c) Timed scheduler — EXISTS

`exec::schedule_after`/`exec::schedule_at` (`exec/timed_scheduler.hpp`) are
real CPOs, and `exec::timed_thread_context` (`exec/timed_thread_scheduler.hpp`)
is a concrete, usable timed-scheduler implementation. The probe scheduled
work 10ms in the future via `exec::schedule_after(sched, 10ms) | then(...)`
and it completed correctly without blocking the calling thread for the
delay (the wait happens inside the timed thread context's own timer
machinery, not a `sleep_for` on the caller's thread).

**Design impact**: Task 18 (`delay`/`within`) should use
`exec::timed_thread_context`/`schedule_after` directly. The hand-rolled
timed single-shot-channel fallback is NOT needed.

## (d) Manual-reset-event / single-shot primitive — DOES NOT EXIST

Searched `exec/` exhaustively (`grep -rli "manual_reset\|single_shot\|async_manual"`)
— nothing. `exec::create.hpp` exists and is a plausible *building block* for
implementing `single_shot_channel` more tersely than raw sender/receiver
boilerplate (it converts a callback-shaped "start this, call the provided
completion function later" pattern into a sender), but does not itself
provide the "arbitrary external code fulfills this later, from any thread,
possibly before or after a receiver connects" semantics `single_shot_channel`
needs. `exec::async_scope`/`exec::split` are for different problems
(structured-concurrency scope tracking and multicasting one sender's result
to multiple receivers, respectively — not applicable here).

**Design impact**: Confirmed — `single_shot_channel<T>` must be hand-rolled
exactly as design.md's Phase 2 section describes. This remains the
highest-risk new code in the project, as design.md already anticipated.
`exec::create` is worth evaluating as an implementation detail during Task
10 (it may simplify the sender/operation-state boilerplate), but does not
change the state-machine design itself.

## New finding not anticipated by design.md: `just_error` alone is not `sync_wait`-able

A bare `stdexec::just_error(ex)` sender has *only* a `set_error_t(Error)`
completion signature — no `set_value_t(...)` at all. `stdexec::sync_wait`
requires the sender type to have *exactly one* successful-completion
signature and rejects anything else with a `static_assert` at compile time
(confirmed: this is a hard compile error, not a runtime behavior). This
matters directly for two places design.md did not flag:

- **Requirement 8.2 / Task 14**: `makeExceptionalFuture<T>(ex)` cannot
  simply wrap a bare `stdexec::just_error(ex)` and later `sync_wait` it from
  `Future<T>::get()` — the sender's type never declares `T` as its value
  type, so it won't type-check.
- **Requirement 7.1 / Task 12**: any `Future<T>` implementation that stores
  a type-erased sender member (needed anyway, since `Future<T>` must be a
  concrete class type comparable to `folly::Future<T>`, not a
  template-parameterized-on-every-continuation raw sender type) needs that
  erasure to declare `set_value_t(T)` explicitly regardless of which
  concrete sender is stored underneath.

**Resolution**: `exec::any_sender<exec::any_receiver<completion_signatures<...>>>`
(`exec/any_sender_of.hpp`, confirmed present; the file also contains a
`[[deprecated]]` `any_sender_of` alias template superseded by this
newer two-type form) provides exactly the type-erasure-with-explicit-
completion-signatures mechanism needed. `Future<T>`'s internal
representation should be `exec::any_sender<exec::any_receiver<
stdexec::completion_signatures<stdexec::set_value_t(T),
stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>>>`
(or equivalent), letting `makeFuture`, `makeExceptionalFuture`, and every
continuation/collector operation converge on one concrete, sync_wait-able
type regardless of which sender shape produced it. This should be folded
into design.md's "Phase 2: stdexec Backend" `Future<T>` description before
Task 12 is implemented, since it's a correction to an implicit assumption
(that a bare `just`/`just_error`/`then`-chain sender could be stored
directly) rather than an open question the original design deferred.

## `ensure()` mapping — CONFIRMED

`exec::finally` (`exec/finally.hpp`) exists in this vendored version.
Task 17 (`ensure(func)`) should use it directly rather than the
`then`+`upon_error` composition design.md offered as a fallback.

## Link-time dependency: `-ltbb` required for `exec::single_thread_context`

Compiling `#include <exec/single_thread_context.hpp>` and using
`exec::single_thread_context` produced undefined references to
`tbb::detail::r1::execution_slot` at link time, even though the vcpkg
`stdexec` port was installed with no `tbb` feature requested. Adding
`-ltbb` to the link line resolved it cleanly on both compilers; `libtbb`
was already present on this system (Ubuntu 24.04 ships it as a system
package). This is not necessarily true of every environment — Task 1
(vcpkg wiring) should account for it: either request the `tbb` vcpkg
feature explicitly so vcpkg provisions a matching `libtbb` regardless of
what the host system has, or confirm which specific `exec/` scheduler
headers pull this in and avoid them if an alternative (e.g.
`exec::static_thread_pool`, not yet probed) does not have the same
dependency. Recorded here rather than resolved, since it is a Task 1
(build wiring) decision, not a Phase 2 API question.

## Summary of design.md deltas to apply before/during implementation

1. Task 20: use `exec::when_any` directly for `collectAny`'s base case;
   only fall back to a hand-rolled channel for `collectAnyWithoutException`'s
   skip-failures behavior, and confirm the exact composition then.
2. Task 18: use `exec::timed_thread_context`/`schedule_after` directly, no
   fallback needed.
3. Task 17: use `exec::finally` directly, no fallback needed.
4. Task 12 (new, not previously called out): `Future<T>`'s internal storage
   must be a type-erased `exec::any_sender<exec::any_receiver<...>>` with
   explicit completion signatures, not a bare concrete sender type — this
   is what makes `makeExceptionalFuture<T>` and `get()` actually
   `sync_wait`-able. Surfaced here because it changes Task 8/12's
   implementation shape, not just a detail to figure out inline.
5. Task 1 (vcpkg wiring): decide `-ltbb` handling (request the `tbb`
   feature vs. avoid the headers that need it) before finalizing
   `vcpkg.json`.
6. `single_shot_channel` (Task 10) remains hand-rolled exactly as designed
   — no existing primitive replaces it. `exec::create` may simplify its
   implementation but does not change its design.
