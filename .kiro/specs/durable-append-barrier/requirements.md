# Requirements Document

## Introduction

Kythira's log is not durable, and the measurement that establishes it is in
`.kiro/specs/multi-raft-performance/` task 19: with a `tick_batch_controller`
supplied exactly as `include/raft/multi_raft.hpp` documents, a durability
barrier covered **19.9% and 24.5%** of appended log entries in two independent
runs. The remaining three quarters reached the operating system's page cache
and no `fsync` ever reached them.

This is not a tuning problem and it is not a bug in the controller. It is a
placement problem. `multi_raft::tick()` opens the batch, runs the persist phase
and commits it, all inside one `tick()` call — but the appends it is meant to
cover do not happen there:

| Where an entry is appended | Thread | Inside `tick()`'s batch? |
|---|---|---|
| `raft.hpp:1638`, the leader's own `submit_command` | the **caller's** thread | no |
| `raft.hpp:2210`, an administration entry | the caller's thread | no |
| `raft.hpp:4300`/`4304`, a follower in `append_entries_with_consistency_check` | the **RPC handler's** thread | no |
| the six configuration-change sites (3017, 3169, 3298, 3379, 3508, 5388, 6485) | the caller's thread | no |

`file_persistence_engine::append_log_entry` outside a batch calls
`append_to_log_file`, which flushes the `ofstream` and stops.
`sync_log_and_directory()` — the only `fsync` on the log path — is reached from
`commit_batch()` and from nowhere else. So a multi-group host with a
file-backed log and no controller issues **no barrier at all**, and one *with*
a controller issues barriers that mostly miss.

The consequence is the one Raft's safety argument does not survive: a node can
tell a leader "I have this entry" and then lose it to a power cut, and the
leader can count that acknowledgement toward a commit. Requirement 3.5 of the
multi-raft-performance spec already insists that a file-backed log which does
not fsync be labelled **not durable** wherever it appears; this spec is the
work that makes the label unnecessary.

**Scope**: where the durability barrier is taken relative to an append, and the
mechanism that keeps taking it affordable. The persistence engines themselves
(`file_persistence_engine`, `memory_persistence_engine`) and the
`persistence_engine` concept are in scope only where the barrier boundary
forces a change.

**Out of scope**: the storage format (the log stays JSON lines; whether that is
the right format is
`.kiro/specs/multi-raft-performance/` H4's question, not this one), snapshot
durability, the object-store persistence tiers, and any change to which entries
Raft chooses to append.

**Explicitly out of scope: making the benchmark faster.** A correct
implementation of this spec will make the durable configuration *slower* than
today's, because today's is not doing the work. The multi-raft-performance
spec's Tier D rows are the beneficiary of this work, not its justification.

## Glossary

- **Barrier** — a durability barrier: the point at which previously written log
  bytes are guaranteed to survive a power loss. On `file_persistence_engine`
  this is `sync_log_and_directory()`, an `fsync` of the log file and of its
  containing directory.
- **Advertise** — for a leader, to count an append toward `match_index` and
  therefore toward the commit index; for a follower, to return success from
  `handle_append_entries`. Both are promises to the rest of the cluster that
  the entry is held.
- **Group commit** — coalescing the barriers of several concurrent appends into
  one, so that N appends cost one `fsync` rather than N.
- **Covered** — an appended entry is covered when a barrier was taken after it
  was written and before the node advertised it.

## Requirement 1 — The barrier is taken before the append is advertised

**User Story:** As an operator, I want a node that says it holds an entry to
still hold it after a power cut, so that Raft's safety argument applies to the
system I am actually running.

### Acceptance Criteria

1. WHEN a leader appends an entry to its own log THEN the system SHALL take a
   durability barrier covering that entry **before** the entry contributes to
   `match_index` or to any commit-index computation
2. WHEN a follower appends entries in `append_entries_with_consistency_check`
   THEN the system SHALL take a durability barrier covering them **before**
   `handle_append_entries` returns success
3. WHEN a configuration-change entry is appended at any of the six sites named
   in the Introduction THEN the system SHALL treat it exactly as Requirement
   1.1 treats a command entry — a configuration entry that is lost is worse
   than a command that is lost, not better
4. WHEN `save_current_term` or `save_voted_for` is called THEN the system SHALL
   NOT defer it into a batch, preserving the behaviour
   `file_persistence_engine` already documents: Raft requires both durable
   before the node responds to the RPC that changed them
5. IF a barrier fails THEN the system SHALL NOT advertise the append, and SHALL
   surface the failure to the caller rather than logging it and continuing

## Requirement 2 — Coverage is measurable, and measured

**User Story:** As a maintainer, I want "the log is durable" to be a number I
can see rather than a property I believe, because the current defect was
invisible for exactly as long as nobody counted.

### Acceptance Criteria

1. WHEN the durability instrumentation is present THEN the system SHALL be able
   to report the fraction of appended entries that a barrier covered
2. WHEN that fraction is anything other than 1.0 in a configuration that claims
   durability THEN the system SHALL be treated as failing, not as slow
3. WHEN coverage is measured THEN the counter SHALL live outside production
   code, as
   `.kiro/specs/multi-raft-performance/` Requirement 8.2 requires — the
   existing `benchmark_persistence_engine` wrapper is the precedent and the
   place
4. WHEN an entry is appended while no barrier is pending THEN the system SHALL
   count it as uncovered rather than assuming a later barrier will reach it

## Requirement 3 — Group commit, so that correctness is affordable

**User Story:** As an operator, I want durability to cost one `fsync` per batch
of concurrent appends rather than one per entry, because the second is the
difference between a usable system and a correct one nobody deploys.

### Acceptance Criteria

1. WHEN several appends are in flight concurrently THEN the system SHALL be
   able to satisfy them with a single barrier
2. WHEN an append is waiting for a barrier THEN the system SHALL NOT hold a
   lock that prevents other appends from joining the same barrier
3. WHEN the system reports entries per barrier THEN that figure SHALL count
   only entries a barrier actually covered, so the ratio cannot be inflated by
   entries no barrier reached — this is the defect
   `.kiro/specs/multi-raft-performance/` task 19 found in its own first draft
4. WHEN a group commit is used THEN the design SHALL state whether a waiter can
   be woken by a barrier that began before its own write landed, and SHALL make
   that impossible rather than unlikely

## Requirement 4 — `tick_batch_controller`'s future is decided, not left

**User Story:** As a reader of `multi_raft.hpp`, I want the batching hook to
either do what it says or not exist, because a hook that half-works is how this
defect survived.

### Acceptance Criteria

1. WHEN this spec is delivered THEN `tick_batch_controller` SHALL either be
   removed, or retained with its documented contract narrowed to what it
   actually provides
2. IF it is retained THEN the system SHALL NOT allow it to be the only
   durability mechanism, since it demonstrably cannot cover the appends that
   matter
3. WHEN the decision is made THEN the reasoning SHALL be recorded in the header
   beside the type, replacing the comment that this spec's Introduction quotes

## Requirement 5 — No silent regression of what already works

**User Story:** As a maintainer, I want the memory-backed path and the existing
test suite to be unaffected, so that a durability change does not become a
rewrite.

### Acceptance Criteria

1. WHEN `memory_persistence_engine` is in use THEN the barrier SHALL be a no-op
   and SHALL cost nothing measurable, since there is nothing to make durable
2. WHEN the change lands THEN every existing test SHALL pass unmodified, or
   each modification SHALL be justified in the task that makes it
3. WHEN the change lands THEN `multi_raft_regression_tier` SHALL still complete
   inside the budget `.kiro/specs/multi-raft-performance/` Requirement 12.4
   states, since it runs on `memory_persistence_engine` and Requirement 5.1
   makes it free
4. WHEN a persistence engine does not implement a barrier THEN the system SHALL
   refuse to describe a configuration using it as durable, rather than
   defaulting to optimism
