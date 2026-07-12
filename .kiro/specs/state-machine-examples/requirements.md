# Requirements Document

## Introduction

`doc/TODO.md` lists **State machine examples** — counter, register, replicated
log, and distributed lock — as an incomplete "Minor Enhancement," noting that
"counter and register already exist as test targets." An audit of the tree
confirms this precisely: `include/raft/examples/` contains all four headers,
but `replicated_log_state_machine.hpp` and `distributed_lock_state_machine.hpp`
are not `#include`d anywhere, have no test files, and have no CMake targets.
They compile in isolation (verified with `-fsyntax-only`) but are otherwise
unverified dead code, unlike `counter_state_machine.hpp` and
`register_state_machine.hpp`, which each have a Boost.Test unit test wired
into `tests/CMakeLists.txt`.

While auditing `distributed_lock_state_machine.hpp` for test coverage, a
correctness defect was found that must be fixed before the example can be
considered a valid demonstration: `acquire()` and `query()` call
`std::chrono::steady_clock::now()` inside `apply()`. Per the `state_machine`
concept (`include/raft/types.hpp`) and Raft's fundamental correctness
requirement, `apply()` must be **deterministic** — every replica applies the
same command at the same log index and must reach identical state. Wall-clock
time read inside `apply()` differs across replicas (different machines, clock
skew, GC pauses, applied-at-different-instants), so two replicas can diverge
on whether a lock has expired. Shipping this as a "best for distributed
coordination" example would teach an anti-pattern.

The goal of this spec is to bring the two untested examples to parity with
counter/register — test coverage, CMake wiring — while fixing the determinism
defect in distributed lock first, so the tests exercise correct, replicable
behavior rather than encoding a bug.

## Glossary

- **Example_State_Machine**: One of the four types in
  `include/raft/examples/` (`counter_state_machine`, `register_state_machine`,
  `replicated_log_state_machine`, `distributed_lock_state_machine`).
- **State_Machine_Concept**: The `kythira::state_machine<SM, LogIndex>`
  concept (`include/raft/types.hpp:168-181`) requiring `apply(command, index)
  -> vector<byte>`, `get_state() const -> vector<byte>`, and
  `restore_from_snapshot(state, index) -> void`.
- **Determinism**: The property that `apply(command, index)` produces
  identical resulting state and return value on every replica, given the same
  command and index — no dependency on wall-clock time, thread scheduling, or
  other replica-local state.
- **Log_Index_Based_Expiry**: The determinism-safe replacement design for
  distributed lock's timeout: expiry is expressed as a count of subsequent
  applied log entries (`expiry_index = acquire_index + timeout_entries`)
  rather than milliseconds of wall-clock time, since `index` is the one
  piece of data every replica is guaranteed to agree on for a given `apply`
  call.

## Requirements

### Requirement 1: Fix distributed lock determinism defect

**User Story:** As a library user studying the distributed lock example, I
want its `apply()` to be deterministic across replicas, so that the example
demonstrates a correct Raft state machine rather than a latent split-brain
bug.

#### Acceptance Criteria

1. WHEN `distributed_lock_state_machine::apply()` executes THEN it SHALL NOT
   call `std::chrono::steady_clock::now()`, `system_clock::now()`, or any
   other non-deterministic time source.
2. WHEN an `ACQUIRE <lock_id> <owner> <timeout_entries>` command is applied at
   log index `N` THEN the lock's expiry SHALL be recorded as `N +
   timeout_entries` (Log_Index_Based_Expiry), using the `index` parameter
   already passed into `apply()`.
3. WHEN a `QUERY <lock_id>` or `ACQUIRE` command is applied at index `M` and
   `M >= expiry_index` for an existing lock THEN the lock SHALL be treated as
   expired (released) before evaluating the command.
4. WHEN the same sequence of commands is applied at the same indices on two
   independent instances of `distributed_lock_state_machine` THEN both
   instances SHALL produce byte-identical `get_state()` output and identical
   `apply()` return values for every command.
5. WHEN the parameter name changes from `timeout_ms` to `timeout_entries`
   THEN `include/raft/examples/README.md`'s Distributed Lock section SHALL be
   updated to reflect the new semantics and a worked example using log
   indices.

### Requirement 2: Replicated log test coverage

**User Story:** As a maintainer, I want `replicated_log_state_machine` to have
the same test rigor as `counter_state_machine` and `register_state_machine`,
so that it is verified rather than merely present.

#### Acceptance Criteria

1. WHEN `tests/replicated_log_state_machine_test.cpp` is compiled and run
   THEN it SHALL cover: appending a single entry, appending multiple entries
   in order, `entry_count()` after N appends, rejecting a malformed command
   (missing `"APPEND "` prefix) with `std::invalid_argument`, and rejecting an
   empty command with `std::invalid_argument`.
2. WHEN a snapshot round trip is tested THEN `get_state()` on a machine with
   several appended entries, followed by `restore_from_snapshot()` on a fresh
   instance, SHALL reproduce the same `entry_count()` and the same entry data
   in the same order.
3. WHEN `restore_from_snapshot()` is called with an empty snapshot THEN the
   resulting machine SHALL have `entry_count() == 0`.
4. WHEN an entry containing embedded null bytes or non-ASCII bytes is
   appended THEN it SHALL round-trip through `get_state()` /
   `restore_from_snapshot()` unchanged (the length-prefixed encoding must not
   truncate at null bytes).
5. WHEN the test file is written THEN it SHALL follow the existing
   `make_command()` / Boost.Test structural pattern used in
   `tests/counter_state_machine_test.cpp` and
   `tests/register_state_machine_test.cpp`, including a
   `*boost::unit_test::timeout(10)` decorator on every test case.

### Requirement 3: Distributed lock test coverage

**User Story:** As a maintainer, I want `distributed_lock_state_machine` to
have test coverage for its lock/release/query/expiry semantics, so that the
Requirement 1 fix is verified and future changes cannot silently reintroduce
non-determinism or break lock correctness.

#### Acceptance Criteria

1. WHEN `tests/distributed_lock_state_machine_test.cpp` is compiled and run
   THEN it SHALL cover: `ACQUIRE` on a free lock returns `"OK"`; a second
   `ACQUIRE` by a different owner before expiry returns `"LOCKED"`; `RELEASE`
   by the owning client returns `"OK"` and frees the lock; `RELEASE` by a
   non-owning client returns `"NOT_OWNER"` and leaves the lock held; `QUERY`
   on a free lock returns `"FREE"`; `QUERY` on a held lock returns
   `"LOCKED:<owner>"`.
2. WHEN a lock's `expiry_index` has been reached (per Requirement 1) THEN a
   subsequent `QUERY` or `ACQUIRE` at or past that index SHALL treat the lock
   as free, and a new owner SHALL be able to acquire it.
3. WHEN a malformed command is applied (wrong argument count for `ACQUIRE` /
   `RELEASE`, or an unrecognized command verb) THEN `apply()` SHALL throw
   `std::invalid_argument`.
4. WHEN a snapshot round trip is tested THEN `get_state()` on a machine with
   one or more active locks, followed by `restore_from_snapshot()` on a fresh
   instance, SHALL reproduce identical `QUERY` responses for every lock ID
   that was present.
5. WHEN `restore_from_snapshot()` is called with an empty snapshot THEN
   `QUERY` for any lock ID on the resulting machine SHALL return `"FREE"`.
6. WHEN the determinism property (Requirement 1, Acceptance Criterion 4) is
   tested THEN the test file SHALL include a dedicated test case that applies
   the same command sequence to two separate instances and asserts their
   `get_state()` outputs are byte-identical after each command.

### Requirement 4: Build integration

**User Story:** As a developer, I want the two new test executables to build
and run via the existing `ctest` workflow, so they are exercised by CI and by
the local pre-commit coverage ratchet exactly like every other test.

#### Acceptance Criteria

1. WHEN `tests/CMakeLists.txt` is updated THEN it SHALL add
   `replicated_log_state_machine_test` and
   `distributed_lock_state_machine_test` executables immediately after the
   existing `register_state_machine_test` block, following the identical
   `add_executable` / `target_link_libraries` / `target_compile_features` /
   `add_test` / `set_tests_properties` pattern (linking `network_simulator`
   and `Boost::unit_test_framework`, `TIMEOUT 30`, `LABELS
   "unit;state_machine;example"`).
2. WHEN `cmake --build build --target <test_name>` is run for either new
   target THEN it SHALL compile without warnings under the project's existing
   compiler flags.
3. WHEN `ctest -R state_machine` is run THEN it SHALL include and pass all
   four example state machine tests (counter, register, replicated log,
   distributed lock).

### Requirement 5: Concept conformance verification

**User Story:** As a maintainer, I want each example state machine to
explicitly assert that it satisfies `kythira::state_machine`, so that a
future edit that accidentally breaks the required interface fails to compile
with a clear diagnostic instead of failing silently or only at Raft
integration time.

#### Acceptance Criteria

1. WHEN `replicated_log_state_machine_test.cpp` and
   `distributed_lock_state_machine_test.cpp` are compiled THEN each SHALL
   contain a `static_assert(kythira::state_machine<..., std::uint64_t>, ...)`
   verifying concept satisfaction, matching the pattern used in
   `tests/raft_state_machine_concept_test.cpp`.

### Requirement 6: Documentation reconciliation

**User Story:** As a contributor reading `doc/TODO.md`, I want the State
machine examples item to accurately reflect what's built and tested, so the
TODO list stays trustworthy.

#### Acceptance Criteria

1. WHEN this spec's tasks are complete THEN `doc/TODO.md`'s "State machine
   examples" line item SHALL be marked `[x]` and its parenthetical updated
   to note all four examples now have test targets.
2. WHEN the distributed lock parameter rename (Requirement 1) lands THEN
   `include/raft/examples/README.md` SHALL be updated so its distributed lock
   example and description no longer reference millisecond timeouts.
