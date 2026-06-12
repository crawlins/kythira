# Requirements Document

## Introduction

This document specifies the requirements for completing the implementation of
dynamic cluster membership change in Kythira using the joint consensus protocol
described in Raft §6 (Ongaro & Ousterhout, 2014).

The `add_server()` and `remove_server()` public API already exists on
`kythira::node`, and the surrounding infrastructure (`configuration_synchronizer`,
`cluster_configuration`, `membership_manager_type`) is in place. What is missing
is the connection between that infrastructure and the core Raft log replication
path: no joint-configuration log entries are ever appended, `apply_committed_entries()`
never calls `notify_configuration_committed()`, and quorum calculations remain
single-configuration throughout. The result is that any call to `add_server()`
or `remove_server()` hangs indefinitely.

## Glossary

- **C_old**: The current cluster configuration before a membership change begins.
- **C_new**: The desired cluster configuration after the membership change completes.
- **C_old+new** (joint configuration): A transitional configuration that requires
  majorities in both C_old and C_new to commit any log entry during the transition.
- **Joint consensus phase**: The period between appending the C_old+new entry and
  committing it. The leader replicates to all nodes in both sets.
- **Final configuration phase**: The period between committing C_old+new and
  committing C_new. The leader appends C_new and reverts to single-configuration
  quorum once it commits.
- **Configuration entry**: A log entry that carries a `cluster_configuration`
  payload rather than a state machine command. Distinguished by an `entry_type`
  field on `log_entry`.

## Requirements

### Requirement 1: Log Entry Type Discriminant

**User Story:** As a Raft node, I need to distinguish configuration entries
from state machine command entries in the log so that I can apply them
differently when replaying or committing.

#### Acceptance Criteria

1. WHEN a `log_entry` is created THEN it SHALL carry an `entry_type` field
   with values `normal` (state machine command) and `configuration` (membership
   change).
2. WHEN a `log_entry` is serialized and deserialized THEN the `entry_type`
   field SHALL survive the round-trip unchanged.
3. WHEN an `entry_type::configuration` entry is applied by a node THEN the
   node SHALL NOT pass its payload to the state machine.

### Requirement 2: Joint Configuration Log Entry

**User Story:** As a cluster leader processing `add_server()` or
`remove_server()`, I need to append a C_old+new joint-configuration entry to
the log so that the membership change is durable and replicates to all peers.

#### Acceptance Criteria

1. WHEN a leader receives `add_server(new_node)` AND all preconditions pass
   THEN it SHALL append a `configuration` log entry carrying the joint config
   `{nodes: C_new, is_joint: true, old_nodes: C_old}` before returning a
   future to the caller.
2. WHEN a leader receives `remove_server(old_node)` AND all preconditions pass
   THEN it SHALL append a `configuration` log entry carrying the joint config
   `{nodes: C_new, is_joint: true, old_nodes: C_old}` before returning a
   future to the caller.
3. WHEN the leader appends the C_old+new entry THEN it SHALL immediately update
   its own `_configuration` to the joint config (per Raft §6: a server uses the
   latest configuration in its log regardless of commit state).
4. WHEN the leader appends the C_old+new entry THEN it SHALL begin replicating
   to all nodes in both C_old and C_new.

### Requirement 3: Joint Quorum Calculation

**User Story:** As a cluster leader in the joint consensus phase, I need to
require majorities from both C_old and C_new before advancing the commit index,
so that no two leaders can exist simultaneously during a membership change.

#### Acceptance Criteria

1. WHEN the leader is in joint consensus phase THEN a log entry SHALL be
   considered committed only when a majority of C_old nodes AND a majority of
   C_new nodes have acknowledged it.
2. WHEN the leader is NOT in joint consensus phase THEN the commit index SHALL
   advance using the standard single-configuration majority.
3. WHEN a node becomes a candidate during joint consensus phase THEN it SHALL
   require votes from a majority of both C_old and C_new to win the election.

### Requirement 4: Configuration Entry Application

**User Story:** As any Raft node (leader or follower) applying committed log
entries, I need to detect and handle configuration entries so that my live
configuration is updated at the correct commit point and the
`configuration_synchronizer` can advance through its phases.

#### Acceptance Criteria

1. WHEN `apply_committed_entries()` encounters a `configuration` entry THEN it
   SHALL call `_config_synchronizer.notify_configuration_committed()` with the
   entry's configuration and index.
2. WHEN a node commits a `configuration` entry THEN it SHALL update its live
   `_configuration` to match the committed entry.
3. WHEN a follower appends a configuration entry received via `AppendEntries`
   THEN it SHALL update its live `_configuration` immediately (before commit),
   matching the leader's behaviour described in Requirement 2.3.
4. WHEN `apply_committed_entries()` encounters a `configuration` entry THEN it
   SHALL NOT pass the entry's payload to `_state_machine.apply()`.

### Requirement 5: C_new Phase Completion

**User Story:** As a cluster leader whose joint configuration has been committed,
I need to append the final C_new configuration entry and commit it so that
the transitional joint configuration is replaced by the permanent new one.

#### Acceptance Criteria

1. WHEN the leader commits the C_old+new entry THEN it SHALL immediately append
   a new `configuration` log entry carrying C_new `{nodes: C_new, is_joint: false}`.
2. WHEN the C_new entry is committed THEN the `configuration_synchronizer`
   SHALL resolve the pending promise, fulfilling the future returned to the
   original `add_server()` / `remove_server()` caller.
3. WHEN the C_new entry is committed AND the removed node is the current leader
   THEN the leader SHALL step down after committing C_new (by converting to
   follower state).
4. WHEN the C_new entry is committed AND the removed node is a follower THEN
   the leader SHALL stop sending heartbeats to that node.

### Requirement 6: Follower Configuration Replication

**User Story:** As a follower, I need to receive and correctly apply
configuration entries propagated by the leader so that my view of the cluster
is consistent with the leader's.

#### Acceptance Criteria

1. WHEN a follower receives an `AppendEntries` containing a configuration entry
   THEN it SHALL store the entry and update its `_configuration` immediately.
2. WHEN a follower's log is truncated (due to a conflicting term) AND the
   truncated entries include a configuration entry THEN it SHALL revert its
   `_configuration` to the last configuration entry remaining in its log, or
   to the boot configuration if none remain.

### Requirement 7: Property Tests

**User Story:** As a developer, I need automated property tests for membership
changes so that regressions in the joint consensus implementation are caught
before they reach production.

#### Acceptance Criteria

1. A property test SHALL verify that at most one leader exists at any point
   during a single-server addition to a live cluster.
2. A property test SHALL verify that after `add_server()` resolves, the new
   node receives and applies subsequent log entries.
3. A property test SHALL verify that after `remove_server()` resolves, the
   removed node no longer influences quorum.
4. A property test SHALL verify that membership changes complete correctly
   when the leader crashes mid-change and a new leader is elected.
5. All existing 279 tests SHALL continue to pass after these changes.

### Requirement 8: Node Recovery on Restart

**User Story:** As a Raft node that has restarted after a crash, I need to
reload my durable state from the persistence engine so that I re-join the
cluster with a consistent view of the log, state machine, and configuration —
not as a blank slate.

Currently `initialize_from_storage()` loads only `current_term` and `voted_for`.
The log (`_log`), commit/apply indices, state machine contents, and cluster
configuration are all silently reset to empty defaults, making the node behave
as if it had never existed in the cluster.

#### Acceptance Criteria

1. WHEN a node initializes AND the persistence engine contains a saved snapshot
   THEN the node SHALL restore the state machine from the snapshot by calling
   `_state_machine.restore_from_snapshot()`, set `_last_applied` and
   `_commit_index` to the snapshot's `last_included_index`, and set
   `_configuration` to the snapshot's `configuration()`.
2. WHEN a node initializes AND the persistence engine contains log entries at
   indices greater than the snapshot's `last_included_index` (or greater than 0
   if no snapshot exists) THEN the node SHALL reload those entries into `_log`
   by calling `_persistence.get_log_entries()`.
3. WHEN a node initializes AND the reloaded log contains one or more
   `entry_type::configuration` entries THEN the node SHALL set `_configuration`
   to the configuration carried by the highest-indexed such entry.
4. WHEN a node initializes AND there is no snapshot AND there are no
   configuration entries in the log THEN `_configuration` SHALL remain at
   the value supplied to the constructor (the boot configuration).
5. WHEN a snapshot is installed on a running node (via `install_snapshot()`)
   THEN the node SHALL set `_configuration` to the snapshot's `configuration()`
   in addition to the existing state machine restore and index updates.
   (This is a related pre-existing gap: `install_snapshot()` does not currently
   restore `_configuration` even though the snapshot stores it.)
