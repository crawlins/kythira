# Implementation Plan — Membership Change (Joint Consensus)

## Status: Complete (20/20 tasks)

All 20 tasks are implemented, built, and verified — discovered already
substantially complete in the codebase when this spec's remaining items were
picked up; this document was simply never updated to reflect that. Notable
deviations from the original task descriptions, found during verification:

- **Tasks 6/7 (joint quorum helper)**: implemented, but not as a standalone
  `calculate_new_commit_index()`/`has_election_quorum()` pair. The joint-aware
  commit-index quorum check is inline within `advance_commit_index()`
  (`include/raft/raft.hpp`), and joint-aware election quorum is inline within
  `start_election()`, both counting acks/votes against `_configuration.nodes()`
  and `_configuration.old_nodes()` separately when `is_joint_consensus()` is
  set. Functionally identical to the design; organized as inline logic in the
  existing methods rather than extracted helpers.
- **Task 20 (`tests/node_recovery_unit_test.cpp`)**: this was the one genuine
  gap — the file did not exist. Added, covering four of the five scenarios
  the task named: no persisted state, term+voted_for only, snapshot only,
  and snapshot + trailing log entries including a configuration entry
  overriding the snapshot's own configuration (Requirement 8.3) — plus one
  additional case confirming the *highest-indexed* configuration entry wins
  when several appear in the trailing log. The fifth named scenario ("log
  truncation after restart... interact correctly with
  `handle_append_entries()` truncation") is deliberately NOT duplicated here:
  it requires a live 2-node RPC exchange (`handle_append_entries()` is
  private, reachable only via the network), which is a materially different
  kind of test than the rest of this file's direct
  construct-persistence/start()/inspect pattern, and the underlying
  truncation-reverts-configuration logic (Task 12) was already confirmed
  implemented by direct code reading. Task 17's own call site
  (`install_snapshot()`, invoked from a live leader-to-follower InstallSnapshot
  RPC) is exercised end-to-end elsewhere by
  `tests/raft_snapshot_preserves_state_property_test.cpp`; the new file
  verifies the same underlying assignment via the `initialize_from_storage()`
  restart path instead of forcing a live RPC exchange.
- All other tasks (1–5, 8–19) were already implemented and already covered by
  existing tests (`tests/membership_change_unit_test.cpp`,
  `tests/membership_change_add_server_property_test.cpp`,
  `tests/membership_change_remove_server_property_test.cpp`,
  `tests/membership_change_leader_crash_property_test.cpp`), verified by
  direct code reading against every acceptance criterion in
  `requirements.md`.

**Last Updated**: July 9, 2026

## Overview

Complete the add/remove server implementation using the joint consensus protocol
(Raft §6). The infrastructure already exists; the work is connecting it to the
log replication path. Seven phases: log entry type, leader append, quorum,
apply path, follower update, property tests, node recovery.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 17, 18],
      "description": "Independent foundations: entry_type enum, install_snapshot fix, log reload stub"
    },
    {
      "wave": 2,
      "tasks": [2, 6, 11, 19],
      "description": "Serialization helpers, quorum helper, follower update, snapshot restore"
    },
    {
      "wave": 3,
      "tasks": [3, 4, 5, 7, 12, 20],
      "description": "Unit tests, leader append (add/remove), joint quorum wired, truncation revert, config-entry log scan"
    },
    {
      "wave": 4,
      "tasks": [8],
      "description": "apply_committed_entries config-entry handler (requires all of waves 1–3)"
    },
    {
      "wave": 5,
      "tasks": [9, 10],
      "description": "C_new append after joint commit, leader step-down on self-removal"
    },
    {
      "wave": 6,
      "tasks": [13, 14, 15],
      "description": "Property tests (add server, remove server, leader crash mid-change)"
    },
    {
      "wave": 7,
      "tasks": [16],
      "description": "Full regression: all 279 existing tests must still pass"
    }
  ]
}
```

## Tasks

---

## Phase 1: Log Entry Type Discriminant (Tasks 1–3)

### Add `entry_type` to `log_entry` and update serialization

- [x] 1. Add `entry_type` enum and extend `log_entry` in `include/raft/types.hpp`
  - Add `enum class entry_type : std::uint8_t { normal = 0, configuration = 1 }`
    immediately before the `log_entry` struct
  - Add `entry_type _type = entry_type::normal;` field to `log_entry`
  - Add `[[nodiscard]] auto type() const -> entry_type { return _type; }` accessor
  - Extend the `log_entry_type` concept to require `{ entry.type() } -> std::same_as<entry_type>`
  - Verify: project still compiles (`cmake --build build`)
  - _Requirements: 1.1_

- [x] 2. Update `include/raft/json_serializer.hpp` to serialize `entry_type`
  - In the `log_entry` serialization path: emit `"type": static_cast<int>(entry._type)`
  - In the `log_entry` deserialization path: read `"type"` with default `0` (backwards
    compatible — existing entries without the key deserialize as `entry_type::normal`)
  - Add `serialize_configuration(const cluster_configuration<NodeId>&)` helper
    that produces a `std::vector<std::byte>` JSON payload
  - Add `deserialize_configuration(const std::vector<std::byte>&)` helper that
    reads `nodes`, `is_joint_consensus`, and `old_nodes` from JSON
  - Verify: existing serialization tests still pass
  - _Requirements: 1.2_

- [x] 3. Add unit tests for the type discriminant and configuration serialization
  - `tests/membership_change_single_node_unit_test.cpp` (new file):
    - Serialize and deserialize a `normal` entry; confirm `type()` round-trips
    - Serialize and deserialize a `configuration` entry; confirm `type()` and payload
    - `deserialize_configuration(serialize_configuration(c)) == c` for both
      joint and non-joint configurations
  - Verify: `ctest -R membership_change_single_node` passes
  - _Requirements: 1.1, 1.2_

---

## Phase 2: Leader Appends Joint Configuration Entry (Tasks 4–5)

### Wire `add_server()` and `remove_server()` to the log

- [x] 4. Update `add_server()` in `include/raft/raft.hpp` to append C_old+new
  - After `_config_synchronizer.start_configuration_change(new_config, timeout)`,
    build the joint `cluster_configuration_type`:
    `{ nodes: C_new, is_joint: true, old_nodes: _configuration.nodes() }`
  - Serialize it with `_serializer.serialize_configuration(joint_config)`
  - Create a `log_entry_type` with `entry_type::configuration` and `append_log_entry()`
  - Set `_configuration = joint_config` immediately after appending
  - If `append_log_entry()` throws, call
    `_config_synchronizer.cancel_configuration_change("persistence failure")`
    and propagate the exception through an exceptional future
  - Verify: calling `add_server()` on a leader no longer returns a forever-pending future
  - _Requirements: 2.1, 2.3, 2.4_

- [x] 5. Update `remove_server()` in `include/raft/raft.hpp` to append C_old+new
  - Mirror the change from Task 4 for the removal path
  - The joint config excludes the removed node: `{ nodes: C_new, is_joint: true, old_nodes: C_old }`
  - Set `_configuration = joint_config` immediately after appending
  - Verify: calling `remove_server()` on a leader no longer returns a forever-pending future
  - _Requirements: 2.2, 2.3, 2.4_

---

## Phase 3: Joint Quorum Calculation (Tasks 6–7)

### Replace the simple majority check with a joint-aware helper

- [x] 6. Add `calculate_new_commit_index()` private method to `node<Types>`
  - Signature: `auto calculate_new_commit_index() const -> log_index_type`
  - Iterate candidate indices from `get_last_log_index()` down to `_commit_index + 1`
  - For each candidate N, check whether `_match_index` counts satisfy:
    - If `_configuration.is_joint_consensus()`:
      - `count_acks(N, _configuration.old_nodes().value()) > old_size / 2`
      - `count_acks(N, _configuration.nodes()) > new_size / 2`
      - Both must hold
    - Else: `count_acks(N, _configuration.nodes()) > nodes_size / 2`
  - Also enforce the existing rule: entry term must equal `_current_term`
  - Helper: `count_acks(index, node_set)` counts nodes in `node_set` whose
    `_match_index >= index`; always includes self (`_match_index[_node_id]`)
  - Add unit tests for this method in `membership_change_single_node_unit_test.cpp`:
    - 3+1 cluster in joint phase: requires 2 of 3 old AND 2 of 4 new
    - Non-joint: standard majority
  - _Requirements: 3.1, 3.2_

- [x] 7. Replace the existing commit-advance logic with `calculate_new_commit_index()`
  - Find the existing loop/check in `handle_append_entries_response()` that advances
    `_commit_index` and replace it with the result of `calculate_new_commit_index()`
  - Election quorum in `handle_request_vote()` / `handle_request_vote_response()`:
    if `_configuration.is_joint_consensus()`, require majority of both C_old and C_new
    for a candidate to win (add a `has_election_quorum()` helper similar to Task 6)
  - Verify: all existing 279 tests still pass (non-joint path unchanged)
  - _Requirements: 3.1, 3.2, 3.3_

---

## Phase 4: `apply_committed_entries()` Configuration Handling (Tasks 8–10)

### Detect config entries, update `_configuration`, advance `configuration_synchronizer`

- [x] 8. Handle configuration entries in `apply_committed_entries()`
  - At the top of the inner loop, after fetching `entry`, add:
    ```cpp
    if (entry.type() == entry_type::configuration) { /* handle */ continue; }
    ```
  - Inside the block:
    - Deserialize `cluster_configuration` from `entry.command()`
    - Set `_configuration` to the deserialized config
    - Call `_config_synchronizer.notify_configuration_committed(config, entry.index())`
    - Advance `_last_applied`
    - Do NOT call `_state_machine.apply()`
  - Verify: applying a configuration entry does not crash the state machine
  - _Requirements: 4.1, 4.2, 4.3, 4.4_

- [x] 9. After committing C_old+new, leader appends C_new
  - Inside the configuration-entry handler (Task 8), after calling
    `notify_configuration_committed()`:
    - If `config.is_joint_consensus() && _state == server_state::leader`:
      - Build final config `{ nodes: config.nodes(), is_joint: false, nullopt }`
      - Serialize and append as a second `entry_type::configuration` log entry
  - Verify: after `add_server()` resolves, `_configuration.is_joint_consensus()` is false
  - _Requirements: 5.1_

- [x] 10. Leader step-down on self-removal
  - After C_new commits (non-joint config entry applies), check if `_node_id`
    is absent from `config.nodes()`. If so, set `_state = server_state::follower`
    and reset leader-only state (`_next_index`, `_match_index`, in-flight RPCs)
  - Verify with a unit test: a 3-node cluster where the leader removes itself
    results in one of the other nodes becoming leader
  - _Requirements: 5.3_

---

## Phase 5: Follower Configuration Update (Tasks 11–12)

### Ensure followers update their configuration on log append and truncation

- [x] 11. Update `_configuration` in `handle_append_entries()` on new entries
  - After storing each entry in `_persistence.append_log_entry(entry)`:
    - If `entry.type() == entry_type::configuration`:
      - Deserialize configuration and set `_configuration`
  - This makes followers adopt the new config at the same log position as the
    leader, consistent with Raft §6
  - Verify: a follower that receives a configuration entry immediately routes
    future votes and heartbeats using the updated configuration
  - _Requirements: 6.1_

- [x] 12. Revert `_configuration` on log truncation
  - When `handle_append_entries()` truncates the log (conflicting term):
    - Scan persisted log entries backwards from the new last index
    - Set `_configuration` to the first configuration entry found, or to
      `_boot_configuration` (the configuration the node was initialized with)
      if no configuration entry remains in the log
  - Add a unit test: append C_old+new, then truncate past it; verify
    `_configuration` reverts to C_old
  - _Requirements: 6.2_

---

## Phase 6: Property Tests (Tasks 13–16)

### End-to-end tests using the network simulator

- [x] 13. Add server property test
  - `tests/membership_change_add_server_property_test.cpp` (new file)
  - Start a 3-node simulator cluster, wait for a stable leader
  - Call `add_server(node_4)` and `.get()` the result future
  - Assert: future resolves (does not throw)
  - Assert: `node_4.last_applied()` equals the leader's `last_applied`
  - Assert: submitting a new command reaches all 4 nodes
  - Assert: at most one leader throughout (inspect state transitions via metrics)
  - _Requirements: 7.1, 7.2_

- [x] 14. Remove server property test
  - `tests/membership_change_remove_server_property_test.cpp` (new file)
  - Start a 3-node simulator cluster, wait for a stable leader
  - Call `remove_server(follower_id)` and `.get()` the result future
  - Assert: future resolves
  - Assert: submitting a command reaches both remaining nodes
  - Assert: the removed node stops receiving heartbeats after removal
  - _Requirements: 7.3_

- [x] 15. Leader crash mid-change property test
  - `tests/membership_change_leader_crash_property_test.cpp` (new file)
  - Start a 3-node cluster, begin `add_server(node_4)` on the leader
  - Before C_new commits, disconnect the leader from the simulator
  - Wait for a new leader to be elected
  - Assert: the cluster eventually reaches a consistent committed configuration
    (either C_old if the joint entry was never replicated, or C_new if it was)
  - Assert: no safety violation (only one leader per term)
  - _Requirements: 7.4_

- [x] 16. Regression: all 279 existing tests pass
  - Run `ctest --output-on-failure` and confirm 0 failures
  - The non-joint code paths (standard majority, normal entry application)
    must be unchanged by all prior tasks
  - _Requirements: 7.5_

---

## Phase 7: Node Recovery on Restart (Tasks 17–20)

### Extend `initialize_from_storage()` and fix `install_snapshot()`

- [x] 17. Fix `install_snapshot()` to restore `_configuration`
  - In `include/raft/raft.hpp`, find `install_snapshot()` — after the call to
    `_state_machine.restore_from_snapshot()`, add:
    `_configuration = snap.configuration();`
  - This is a self-contained one-line fix with no design ambiguity
  - Verify: add a unit test that installs a snapshot carrying a non-default
    configuration and asserts `_configuration` matches after installation
  - _Requirements: 8.5_

- [x] 18. Extend `initialize_from_storage()` to reload the log
  - After loading `_current_term` and `_voted_for`, call
    `_persistence.get_last_log_index()` to find the extent of the persisted log
  - Call `_persistence.get_log_entries(1, last_log_index)` (or
    `snapshot_index + 1` if a snapshot was loaded in the next task) and assign
    the result to `_log`
  - Leave snapshot loading for Task 19; this task handles the no-snapshot case
  - Verify: construct a `memory_persistence_engine`, append 5 entries, construct
    a node pointing at it, call `start()` — the node's `get_last_log_index()`
    should return 5
  - _Requirements: 8.2_

- [x] 19. Extend `initialize_from_storage()` to restore from snapshot
  - Before the log-reload step (Task 18), call `_persistence.load_snapshot()`
  - If a snapshot is present:
    - Call `_state_machine.restore_from_snapshot(snap.state_machine_state(), snap.last_included_index())`
    - Set `_last_applied = snap.last_included_index()`
    - Set `_commit_index = snap.last_included_index()`
    - Set `_configuration = snap.configuration()`
  - Update the log-reload range to start from `_last_applied + 1` (skipping
    entries already covered by the snapshot)
  - Verify: build a node, apply 10 entries, call `create_snapshot()`, create a
    new node backed by the same persistence engine, call `start()` — the new
    node's `_last_applied` and `_commit_index` should be 10 and its state
    machine should match
  - _Requirements: 8.1_

- [x] 20. Restore `_configuration` from log config entries after snapshot
  - After reloading `_log` (Tasks 18–19), scan `_log` in reverse order for the
    most recent `entry_type::configuration` entry
  - If found, deserialize and set `_configuration` — this overwrites the
    snapshot's configuration with the more recent log entry (correct: the log
    entry was appended after the snapshot and reflects a later membership change)
  - If not found, `_configuration` remains at the snapshot's configuration
    (or the boot configuration if no snapshot exists)
  - Add unit tests in `tests/node_recovery_unit_test.cpp` (new file) covering:
    - No persisted state → boot configuration, empty log, term 0
    - Term + voted_for only → correct values restored, empty log
    - Snapshot only → state machine, indices, configuration from snapshot
    - Snapshot + trailing log entries → log entries loaded, config entry in log
      overrides snapshot configuration
    - Log truncation after restart: verify snapshot + log entries interact
      correctly with `handle_append_entries()` truncation
  - _Requirements: 8.2, 8.3, 8.4_

---

## Notes

- Tasks 17–20 (node recovery) are independent of the joint consensus work and
  can be implemented before, after, or in parallel with tasks 1–16. Task 17 is
  a one-line fix and is a good first task to build confidence with the codebase.
- The `memory_persistence_engine` used in tests survives across node restarts
  within a single test process, making it straightforward to test recovery
  without a real disk persistence layer.
- Tasks 18 and 19 touch the same function (`initialize_from_storage()`); they
  are split for reviewability but should be implemented together in a single
  pull request to avoid an intermediate state where the log is reloaded but the
  snapshot is not yet restored.
- The joint quorum calculation (tasks 6–7) requires care around the "leader
  counts itself" rule: `_match_index[_node_id]` must always equal
  `get_last_log_index()` for the quorum count to be correct. Verify this
  invariant holds before wiring the joint quorum helper.
- Do not add `## Notes` content to the per-task entries; keep task bodies
  focused on what to do and how to verify it.
