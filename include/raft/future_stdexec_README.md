# `future_stdexec.hpp` — scope boundaries

Quick reference for what `.kiro/specs/stdexec-future-backend/` does and does
not do. See `future_stdexec.hpp`'s own file-level doc comment for the same
information inline, and `.kiro/specs/stdexec-future-backend/design.md` for
the full design.

- **No existing production call site is converted.** Every place in this
  codebase that uses `kythira::Future`/`Promise`/`Try` today (the Folly
  backend, `include/raft/future.hpp`) keeps doing so unchanged. This spec
  adds a second, independent implementation in its own namespace
  (`kythira::stdexec_backend`) — it does not touch, deprecate, or migrate
  any existing code path.
- **Folly is not removed or made optional-only.** It remains the default
  backend (`KYTHIRA_DEFAULT_FUTURE_BACKEND=folly`) and a required
  dependency for the rest of the project regardless of whether `stdexec` is
  available. `stdexec` is an *additional* optional dependency, following
  the same `find_package(... CONFIG QUIET)` + graceful-degradation pattern
  as every other optional dependency in this project (see
  `CMakeLists.txt`).
- **GPU execution (`nvexec`) and other non-CPU `stdexec` extensions are out
  of scope.** Only CPU scheduling is covered — `exec::single_thread_context`,
  `exec::timed_thread_context`, and equivalents.
- **Who this is for:** new code that specifically wants direct access to
  `stdexec` schedulers/algorithms without going through Folly's executor
  model. See `examples/stdexec-backend/migration_guide_example.cpp` for a
  side-by-side comparison of equivalent operations on both backends — it is
  a *comparison*, not a migration guide, since nothing is expected to
  migrate.
- **Version/compiler matrix validated during implementation:** see
  `.kiro/specs/stdexec-future-backend/spike-notes.md` (vcpkg port version,
  upstream `stdexec` commit, and the `g++`/`clang++` versions confirmed
  during the Phase 0 spike).
