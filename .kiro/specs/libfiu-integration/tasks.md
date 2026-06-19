# Implementation Plan — Chaos Testing with libfiu

## Status: Complete

**Last Updated**: June 12, 2026

## Overview

Integrate fault injection chaos testing using libfiu. The work is divided into
five phases: build integration, chaos wrapper infrastructure, fault profiles,
safety/liveness property tests, and documentation. Phases 1–3 establish the
foundations; phases 4–5 are where correctness regressions are caught.

Chaos tests are isolated in `tests/chaos/` and compiled only when libfiu is
detected at configure time. No production code changes are required.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 2, 3, 4],
      "description": "Build integration: libfiu detection, CMake targets, smoke test"
    },
    {
      "wave": 2,
      "tasks": [5, 6, 7, 8, 9],
      "description": "Chaos wrapper infrastructure: decorators, chaos_raft_types, fault profiles, safety assertions"
    },
    {
      "wave": 3,
      "tasks": [10],
      "description": "Fault profile scenario tests (depends on all wave 2 infrastructure)"
    },
    {
      "wave": 4,
      "tasks": [11, 12, 13, 14, 15, 16, 17, 18],
      "description": "Safety and liveness property tests (depend on wave 2–3 infrastructure)"
    },
    {
      "wave": 5,
      "tasks": [19, 20, 21],
      "description": "Documentation and end-to-end verification"
    }
  ]
}
```

## Tasks

---

## Phase 1: Build Integration (Tasks 1–4)

### Wire libfiu detection and chaos test targets into CMake

- [x] 1. Verify libfiu availability and document the dependency
  - Install `libfiu-dev` on the build machine:
    `sudo apt install libfiu-dev`
  - Confirm `pkg-config --modversion fiu` prints a version ≥ 0.6
  - Confirm `/usr/include/fiu.h` and `/usr/include/fiu-control.h` exist
  - Add libfiu to `DEPENDENCIES.md` as a **test-only optional** dependency
    with install instructions
  - _Requirements: 1.1, 1.5_

- [x] 2. Add `pkg_check_modules(FIU QUIET fiu)` block to root `CMakeLists.txt`
  - Place the detection block after the clang-tidy detection block for
    consistency
  - Set `CHAOS_TESTS_ENABLED` based on result
  - Emit `message(STATUS ...)` in both the found and not-found cases; use
    `STATUS` (not `WARNING`) so the build stays clean
  - Verify: configure with libfiu installed → `chaos test targets enabled`
  - Verify: configure with libfiu removed → `chaos test targets disabled`
  - _Requirements: 1.1–1.3_

- [x] 3. Create `tests/chaos/` directory and stub `CMakeLists.txt`
  - `tests/chaos/CMakeLists.txt` guards everything with
    `if(CHAOS_TESTS_ENABLED)` and is included from the root `CMakeLists.txt`
    (following the pattern of how `tests/CMakeLists.txt` is structured)
  - Define the `chaos-tests` custom target:
    - If enabled: depends on all chaos test executables
    - If disabled: prints actionable error and exits non-zero
  - Add `LABELS "chaos"` property on all chaos test cases for `ctest` filtering
  - Verify: `cmake --build build --target chaos-tests` succeeds with libfiu
    and fails with an actionable message without it
  - _Requirements: 1.3, 7.2, 7.3_

- [x] 4. Add a minimal smoke test to confirm the build pipeline works end-to-end
  - `tests/chaos/chaos_smoke_test.cpp`: calls `fiu_init(0)`,
    `fiu_enable("raft/smoke", 1, nullptr, 0)`, asserts `fiu_fail("raft/smoke")`
    returns non-zero, then `fiu_disable_all()` and asserts
    `fiu_fail("raft/smoke")` returns zero
  - This test verifies that libfiu is linked correctly and the basic API works
  - `cmake --build build --target chaos-tests && ctest --label-regex chaos`
    should pass with this single test
  - _Requirements: 1.4, 7.1, 7.4_

---

## Phase 2: Chaos Wrapper Infrastructure (Tasks 5–9)

### Implement the decorator types and chaos_raft_types

- [x] 5. Write `tests/chaos/chaos_wrappers.hpp`
  - `chaos_network_client<T>`: intercepts `send_request_vote`,
    `send_append_entries`, `send_install_snapshot`; checks the corresponding
    `fiu_fail()` before delegating
  - `chaos_persistence_engine<T>`: intercepts all five write operations
    (`save_current_term`, `save_voted_for`, `append_log_entry`, `truncate_log`,
    `save_snapshot`); reads pass through without fault checks (see design
    trade-offs)
  - `chaos_state_machine<T>`: intercepts `apply`; `get_snapshot` and
    `restore_from_snapshot` pass through without fault checks
  - Each check: `if (fiu_fail("raft/<layer>/<op>")) { throw ...; }`
  - _Requirements: 2.1–2.5, 3.1–3.4_

- [x] 6. Verify that the wrapped types satisfy the kythira concepts
  - Add static assertions to `chaos_wrappers.hpp`:
    ```cpp
    static_assert(network_client<chaos_network_client<base_network_client>, ...>);
    static_assert(persistence_engine<chaos_persistence_engine<base_persistence>, ...>);
    ```
  - Alternatively, compile a `static_assert` in the smoke test file
  - If concept satisfaction fails, the wrapper is missing a required method —
    add it as a pass-through following the pattern for reads
  - _Requirements: 2.6_

- [x] 7. Write `tests/chaos/chaos_types.hpp`
  - Defines `kythira::chaos::chaos_raft_types` by composing wrapped components
    over `simulator_network_client`, `memory_persistence_engine`, and
    `test_key_value_state_machine`
  - `network_server_type` is the plain (unwrapped) `simulator_network_server` —
    receive-side faults are out of scope (see design trade-offs)
  - Verify: `kythira::node<chaos_raft_types>` compiles without errors
  - _Requirements: 2.6_

- [x] 8. Write `tests/chaos/fault_profiles.hpp`
  - `fault_profile` base: RAII handle with `enable_always`, `enable_random`,
    `enable_once` helpers; `disable()` and destructor clear all active points
  - Four concrete profiles (see design doc):
    `network_partition_profile`, `disk_degradation_profile`,
    `leader_crash_profile`, `state_machine_fault_profile`
  - Verify each profile's RAII guarantee: create a profile in a block scope;
    after the scope exits, confirm `fiu_fail()` returns 0 for all its points
  - _Requirements: 4.1–4.6_

- [x] 9. Write `tests/chaos/safety_assertions.hpp`
  - `assert_election_safety(nodes)`: checks no two nodes share leadership in
    the same term by examining each node's `current_term()` and `is_leader()`
  - `assert_log_matching(nodes)`: iterates up to `min(last_applied)` across
    all nodes and asserts log entries at each index match across all nodes
  - `assert_state_machine_safety(machines, up_to_index)`: compares the
    `applied_commands` history of each `chaos_state_machine` wrapper up to
    the given index
  - Note: this task may require adding a `debug_state()` accessor to
    `kythira::node` that exposes `_current_term`, `_commit_index`,
    `_last_applied`, and `_log` read-only. Coordinate this as a minimal,
    non-breaking addition: `[[nodiscard]] auto debug_state() const noexcept`
  - _Requirements: 5.5_

---

## Phase 3: Fault Profile Scenario Tests (Task 10)

- [x] 10. Write integration smoke tests for each profile in
  `tests/chaos/chaos_profiles_test.cpp`
  - For each profile: create a 3-node `chaos_raft_types` cluster, apply the
    profile to one or all nodes, confirm that the targeted operations throw,
    disable the profile, confirm the operations succeed
  - These tests do NOT verify Raft correctness — they only verify that the
    fault injection machinery works as specified
  - _Requirements: 4.1–4.6_

---

## Phase 4: Safety and Liveness Property Tests (Tasks 11–18)

### One file per property; each file is its own test executable

- [x] 11. `tests/chaos/chaos_election_safety_test.cpp`
  - Property: at most one leader per term, even under network partition
  - Scenario: 3-node cluster; after leader election, apply
    `network_partition_profile` to one follower; trigger election timeouts;
    call `assert_election_safety`; disable profile; verify recovery
  - Run 10 iterations with randomised election timeout jitter
  - _Requirements: 5.1_

- [x] 12. `tests/chaos/chaos_log_matching_test.cpp`
  - Property: if any two nodes share a (term, index) pair their log prefixes
    are identical
  - Scenario: 5-node cluster; submit 20 commands with `disk_degradation_profile`
    active on two followers (10% failure rate); send heartbeats; call
    `assert_log_matching` after fault removal
  - _Requirements: 5.2_

- [x] 13. `tests/chaos/chaos_leader_completeness_test.cpp`
  - Property: a committed entry must appear in all future leaders' logs
  - Scenario: commit one command; apply `network_partition_profile` to the
    current leader (isolating it); wait for a new leader to be elected; verify
    the committed entry appears in the new leader's log
  - _Requirements: 5.3_

- [x] 14. `tests/chaos/chaos_state_machine_safety_test.cpp`
  - Property: two nodes that have applied to the same index have applied the
    same commands
  - Scenario: 5-node cluster with `disk_degradation_profile` on two nodes
    (20% network failure rate on AppendEntries); let 30 commands be committed;
    call `assert_state_machine_safety` across all nodes that have applied
    at least one entry
  - _Requirements: 5.4_

- [x] 15. `tests/chaos/chaos_leader_election_recovery_test.cpp`
  - Liveness: after a partition heals, a leader is elected within the timeout
  - Scenario: isolate the minority with `network_partition_profile`; wait
    2× `election_timeout_max`; disable profile; verify a leader is elected
    within another 2× `election_timeout_max`
  - _Requirements: 6.1_

- [x] 16. `tests/chaos/chaos_commit_recovery_test.cpp`
  - Liveness: a pending command commits after network faults stop
  - Scenario: submit a command with `network_partition_profile` active on a
    minority; disable profile; verify commit within 10× `heartbeat_interval`
  - _Requirements: 6.2_

- [x] 17. `tests/chaos/chaos_persistence_degradation_recovery_test.cpp`
  - Liveness: cluster recovers log replication after disk degradation ends
  - Scenario: apply `disk_degradation_profile` (30% failure) to all nodes;
    submit 10 commands; disable profile; verify all commands commit
  - _Requirements: 6.3_

- [x] 18. `tests/chaos/chaos_state_machine_recovery_test.cpp`
  - Liveness: a node with state machine faults resumes applying entries once
    the fault is removed
  - Scenario: apply `state_machine_fault_profile` (100%) to one follower;
    commit 5 commands; disable profile; verify the follower catches up
    (its `last_applied` reaches the cluster `commit_index`)
  - _Requirements: 6.4_

---

## Phase 5: Documentation (Tasks 19–21)

- [x] 19. Update `README.md`
  - Add "Chaos Testing" section after "Static Analysis"
  - Content: install libfiu, `cmake --build build --target chaos-tests`,
    `ctest --label-regex chaos`, Fault_Point naming convention, reference to
    `design.md` catalogue
  - _Requirements: 8.1, 8.2_

- [x] 20. Update `doc/TODO.md`
  - Add `chaos testing with libfiu` to the Remaining Work list (or mark done
    when tasks are complete)
  - _Requirements: 8.3_

- [x] 21. End-to-end verification
  - `cmake --build build --target chaos-tests` → all executables compile
  - `ctest --label-regex chaos` → all 9 tests (smoke + profiles + safety +
    liveness) pass
  - Remove libfiu, reconfigure → `chaos test targets disabled` message, no
    build errors, `cmake --build build --target chaos-tests` exits non-zero
    with actionable message
  - `cmake --build build` (full build, no libfiu) → clean, no errors
  - _Requirements: all_

---

## Summary

| Phase | Tasks | Status |
|-------|-------|--------|
| 1 | 1–4   | Complete |
| 2 | 5–9   | Complete |
| 3 | 10    | Complete |
| 4 | 11–18 | Complete |
| 5 | 19–21 | Complete |

**Total**: 21 tasks

## Notes

- Election timeout constants in `chaos_leader_election_recovery_test.cpp` are
  set to [150ms, 200ms] — must exceed the `execute_with_retry` initial delay
  ceiling of 110ms (100ms ± 10% jitter) so retries always fire before
  `collect_majority`'s `within()` deadline.
- A 400ms drain sleep after `node::stop()` in the election safety and recovery
  tests prevents Folly timer callbacks from firing after node destruction.
- `retrieve_message` in the network simulator uses an immediate-throw pattern
  (no blocking/CV) to avoid deadlocks with `check_election_timeout()` which
  holds the node mutex during the synchronous retry setup.
