# Implementation Plan — Complete State Machine Examples

## Status: Complete (6/6 tasks)

Implemented in commit `acea06f` (`feat(raft): complete state machine examples
per kiro spec`). Verified directly against the working tree: the determinism
fix, both test files (with every case this spec lists), the CMake wiring, and
both doc updates are all present, and `ctest --test-dir build -R
state_machine` passes 19/19 (including `replicated_log_state_machine_test`
and `distributed_lock_state_machine_test`).

## Overview

Bring `replicated_log_state_machine` and `distributed_lock_state_machine` to
parity with `counter_state_machine` / `register_state_machine`: fix a
determinism defect in distributed lock, add test coverage for both, wire
both into `tests/CMakeLists.txt`, and reconcile `doc/TODO.md`.

The determinism fix must land before the distributed-lock tests are written,
since the tests assert index-based expiry behavior that doesn't exist yet —
writing tests against the current wall-clock implementation would either be
flaky or would encode the bug as "expected" behavior.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1],
      "description": "Fix distributed lock determinism defect (blocks task 3)"
    },
    {
      "wave": 2,
      "tasks": [2, 3],
      "description": "Write test files for replicated log and distributed lock"
    },
    {
      "wave": 3,
      "tasks": [4],
      "description": "Wire both test executables into tests/CMakeLists.txt"
    },
    {
      "wave": 4,
      "tasks": [5],
      "description": "Full build + test verification"
    },
    {
      "wave": 5,
      "tasks": [6],
      "description": "Documentation reconciliation"
    }
  ]
}
```

## Tasks

---

## Phase 1: Determinism Fix (Task 1)

- [x] 1. Replace wall-clock expiry with Log_Index_Based_Expiry in
  `include/raft/examples/distributed_lock_state_machine.hpp`
  - Rename `lock_info::expiry_ms` → `lock_info::expiry_index`
  - Thread the `index` parameter from `apply()` down into `acquire()` and
    `query()` (both currently take no index; add it to their signatures)
  - `acquire()`: grant when `lock.owner.empty() || index >= lock.expiry_index`;
    set `lock.expiry_index = index + timeout_entries`
  - `query()`: treat as expired when `index >= it->second.expiry_index`
  - `release()` is unchanged (pure ownership check, no time dependency)
  - `get_state()` / `restore_from_snapshot()`: update the serialized field
    name conceptually (wire format is still `lock_id:owner:number;`, just a
    different number) — no format change needed, only the value's meaning
  - Remove the `<chrono>` include; it's no longer used
  - Verify: `g++ -std=c++23 -fsyntax-only` on the header still compiles
  - _Requirements: 1.1, 1.2, 1.3_

---

## Phase 2: Test Coverage (Tasks 2-3)

- [x] 2. Write `tests/replicated_log_state_machine_test.cpp`
  - Structure: `BOOST_TEST_MODULE`, `make_command()` helper, one
    `BOOST_AUTO_TEST_CASE` per case below, `*boost::unit_test::timeout(10)`
    on each — mirror `tests/counter_state_machine_test.cpp`
  - Cases: single append; multiple appends in order + `entry_count()`;
    malformed command (no `"APPEND "` prefix) throws `std::invalid_argument`;
    empty command throws `std::invalid_argument`; snapshot round trip with
    several entries; empty-snapshot restore yields `entry_count() == 0`;
    entry containing embedded null/non-ASCII bytes round-trips unchanged
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5_

- [x] 3. Write `tests/distributed_lock_state_machine_test.cpp`
  - Structure: same pattern as task 2
  - Cases: `ACQUIRE` on free lock → `"OK"`; second `ACQUIRE` by different
    owner before expiry → `"LOCKED"`; `RELEASE` by owner → `"OK"` and lock
    freed; `RELEASE` by non-owner → `"NOT_OWNER"`, lock still held; `QUERY`
    on free lock → `"FREE"`; `QUERY` on held lock → `"LOCKED:<owner>"`;
    lock expiry — acquire with `timeout_entries=N`, then `QUERY`/`ACQUIRE`
    at `index >= acquire_index + N` sees it as free; malformed `ACQUIRE`
    (wrong arg count), malformed `RELEASE`, unknown verb all throw
    `std::invalid_argument`; snapshot round trip with active locks; empty
    snapshot → `QUERY` returns `"FREE"` for any id; determinism test —
    apply the identical command sequence (including at least one expiry) to
    two separate instances, assert `get_state()` matches after every command
  - This task depends on task 1 (needs `timeout_entries` semantics to exist)
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6_

Both task 2 and task 3 test files additionally include, near the top:

```cpp
#include <raft/types.hpp>
static_assert(kythira::state_machine<replicated_log_state_machine, std::uint64_t>, "...");
// (distributed-lock equivalent in task 3's file)
```

- _Requirements: 5.1_

---

## Phase 3: Build Integration (Task 4)

- [x] 4. Add both executables to `tests/CMakeLists.txt`
  - Insert after the existing `register_state_machine_test` block (~line
    1671), before the "State machine integration test" comment
  - Identical five-call shape as counter/register: `add_executable` →
    `target_link_libraries` (`network_simulator`, `Boost::unit_test_framework`)
    → `target_compile_features(... cxx_std_23)` → `add_test` →
    `set_tests_properties(... TIMEOUT 30 LABELS "unit;state_machine;example")`
  - Verify: `cmake --build build --target replicated_log_state_machine_test`
    and `... --target distributed_lock_state_machine_test` both compile
    clean
  - _Requirements: 4.1, 4.2_

---

## Phase 4: Verification (Task 5)

- [x] 5. End-to-end verification
  - `cmake --build build -j$(nproc)` — full build succeeds, no new warnings
  - `ctest --test-dir build -R state_machine --output-on-failure` — all four
    example state machine tests pass (counter, register, replicated log,
    distributed lock)
  - `ctest --test-dir build -L example --output-on-failure` — confirms label
    filtering picks up both new tests
  - Re-run the distributed-lock determinism test case specifically a handful
    of times (`--repeat until-fail:5`) to catch any residual flakiness before
    calling Requirement 1.4 satisfied
  - _Requirements: 3.6, 4.3_

---

## Phase 5: Documentation (Task 6)

- [x] 6. Reconcile documentation
  - `doc/TODO.md`: mark the "State machine examples" line `[x]`; update the
    parenthetical from "(counter and register already exist as test
    targets)" to note all four now have test targets
  - `include/raft/examples/README.md`: update the Distributed Lock section's
    `ACQUIRE` example and description to use `timeout_entries` (log entries)
    instead of `timeout_ms`, with a short note on why (determinism — see
    design.md)
  - _Requirements: 1.5, 6.1, 6.2_

---

## Summary

| Phase | Tasks | Status |
|-------|-------|--------|
| 1 | 1     | Complete |
| 2 | 2-3   | Complete |
| 3 | 4     | Complete |
| 4 | 5     | Complete |
| 5 | 6     | Complete |

**Total**: 6 tasks

## Notes

- `doc/OPTIONAL_TASKS_STATUS.md` already marks "Tasks 600-603" (the four
  example headers) as complete — that claim was accurate for "header
  exists" but not for "tested and wired into the build." This spec doesn't
  touch that file; its claims are about header existence, which remains
  true. If a future pass wants task-level granularity there, that's a
  separate documentation cleanup, not blocking this spec.
- Task 1 is the only production-code change in this spec; everything else
  is additive (new test files, new CMake entries, doc edits).
