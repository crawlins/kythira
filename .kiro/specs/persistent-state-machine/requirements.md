# Requirements Document

## Introduction

Every Kythira state machine is RAM-resident and is moved as one byte buffer.
The `state_machine` concept (`include/raft/types.hpp`) is

```cpp
{ sm.apply(command, index) }               -> std::vector<std::byte>;
{ const_sm.get_state() }                   -> std::vector<std::byte>;
{ sm.restore_from_snapshot(bytes, index) } -> void;
```

and `node<Types>` and `multi_raft` lean on that shape at eight sites:

| Site | What it does with the whole state |
|---|---|
| `raft.hpp` `create_snapshot_locked` | serializes it to compact the log |
| `raft.hpp` `initialize_from_storage` | rebuilds it from the saved snapshot on every restart |
| `raft.hpp` `handle_install_snapshot` → `install_snapshot` | rebuilds it from a received buffer |
| `raft.hpp` `send_install_snapshot_chunk` | slices the leader's in-memory buffer |
| `raft.hpp` `serve_read_after_apply` | **serializes it on every linearizable read** and returns it |
| `multi_raft_impl.hpp` split apply | `split_state` copies each child's share out as bytes |
| `multi_raft_impl.hpp` derived child | `restore_from_snapshot` of the derived share |
| `multi_raft_impl.hpp` merge apply | `get_state` of the source, `absorb` into the target |

So a group's data must fit in memory on every replica, a restart reloads all
of it, a split copies it, and a snapshot holds a second full copy in memory
while it is built and sent. The disk-backed build cache
(`.kiro/specs/disk-backed-build-cache/`) moved value bytes out of RAM but kept
the index in RAM and kept every one of these paths.

This spec adds a second kind of state machine whose state **lives on disk and
is never serialized whole**: the shape TiKV uses (one LSM engine per node, each
Raft group owning a key range in it). It is the storage half of the
"Disk-backed state machine" section of the DynamoDB scoping doc (2026-11-02).
The transfer half, sending such a state to a follower, is
`.kiro/specs/file-install-snapshot/`.

**Scope**: the `persistent_state_machine` and `kv_engine` concepts, an
in-memory and a RocksDB engine, `node<Types>` and `multi_raft` integration
(restart, compaction, reads, split, merge), the transaction wrapper's state on
disk, one reference state machine, and the crash tests. **Out of scope**:
the wire format for snapshot transfer (the companion spec), moving the Raft
log into the engine, object-store backup of an engine's files (Requirement
10), and any DynamoDB or Kinesis API.

## Glossary

- **Engine**: one ordered, durable key-value store per node process, shared by
  every group on that node that uses a persistent state machine.
- **Group range**: the half-open key range `[start, end)` a group owns in the
  engine. It is the group's shard range, prefixed by a per-host namespace.
- **Applied index**: the index of the last log entry whose effects are in the
  engine, stored in the engine under the group's metadata key.
- **Durable applied index**: the highest applied index the engine guarantees
  survives a power cut (after its WAL sync or memtable flush).
- **Checkpoint**: an immutable, point-in-time view of a group range as a set
  of files, cheap to take (hard links on RocksDB).

## Requirements

### Requirement 1: Concepts

**User Story:** As a state-machine author, I want a contract that lets my
state live on disk, so that its size is bounded by disk, not RAM.

#### Acceptance Criteria

1. A `kv_engine` concept SHALL provide: point get, ordered iteration over a
   range in both directions, an atomic write batch (put, delete,
   delete-range), a durable-sequence query and a wait/sync for it, checkpoint
   of a range to a directory, ingest of a checkpoint's files into a range, and
   drop of a range.
2. A `persistent_state_machine<SM>` concept SHALL require: `apply(command,
   index)` that writes its effects and the applied index in one batch;
   `applied_index()`; `durable_applied_index()`; `open(engine, range)`;
   `checkpoint(dir) -> snapshot_manifest`; `ingest(dir, manifest, index)`;
   and `drop()`. It SHALL NOT require `get_state` or `restore_from_snapshot`.
3. A `persistent_splittable_state_machine<SM>` concept SHALL add range-only
   split and merge: `split_at(keys) -> vector<range>` and `adopt(range)`, with
   no byte copies.
4. `node<Types>` SHALL select the persistent path with `if constexpr` on
   `persistent_state_machine<state_machine_type>`. A state machine that
   satisfies only `state_machine` SHALL compile and behave exactly as today;
   no existing state machine, serializer or test SHALL change.
5. A state machine SHALL NOT satisfy both `state_machine`'s snapshot pair and
   `persistent_state_machine` in a way that makes the choice ambiguous; the
   concept check SHALL prefer the persistent path and document it.

### Requirement 2: Atomic apply and idempotent replay

**User Story:** As a replica, I must never apply an entry twice or skip one,
whatever crashes in between.

#### Acceptance Criteria

1. Each `apply(command, index)` SHALL commit the entry's effects and the new
   applied index in **one** engine write batch.
2. After a crash, `applied_index()` SHALL equal the index of the last batch
   the engine recovered, and the node SHALL replay from `applied_index() + 1`.
3. `apply` SHALL be a no-op returning the cached result kind for an index at
   or below `applied_index()`, so a replay that overlaps recovered state is
   harmless.
4. Engine writes on apply SHALL NOT need their own fsync. Durability comes
   from the Raft log, so a lost batch is replayed; Requirement 3 keeps the log
   long enough for that.

### Requirement 3: Compaction bound

**User Story:** As an operator, I need the log compacted, but never past state
the engine could still lose.

#### Acceptance Criteria

1. For a persistent group, log compaction SHALL truncate only through
   `durable_applied_index()`, never through `last_applied`.
2. Taking a snapshot for compaction SHALL save **snapshot metadata only**
   (last included index and term, configuration, and an `external_state`
   flag) through the persistence engine, with an empty state buffer.
3. The compaction trigger SHALL be the bytes applied since the last
   compaction (today's `snapshot_threshold_bytes`), with an optional engine
   sync before compaction when the durable index lags by more than a
   configured bound.
4. A metric SHALL report the gap between `last_applied` and
   `durable_applied_index()` per group.

### Requirement 4: Restart

**User Story:** As an operator, I want a node with 100 GB of state to restart
in seconds, not reload it.

#### Acceptance Criteria

1. `initialize_from_storage` SHALL, for a persistent group, open the state
   machine on its range and set `_last_applied` to `applied_index()` without
   reading any state into memory.
2. It SHALL set the snapshot index and term from the saved metadata, and SHALL
   refuse to start (with a clear error) if `applied_index()` is below the
   snapshot index, because the entries between were compacted.
3. If `applied_index()` is above the log's last index (state ahead of a lost
   log tail), the node SHALL start, treat `applied_index()` as committed, and
   log a warning; this matches Raft's guarantee that an applied entry was
   committed.
4. Restart time SHALL be measured and recorded for a group of at least 10 GB.

### Requirement 5: Reads without serialization

**User Story:** As a client, I want a linearizable read that costs a key
lookup, not a copy of the shard.

#### Acceptance Criteria

1. For a persistent group, `serve_read_after_apply` SHALL NOT serialize the
   state machine. `read_state` SHALL confirm the read index and then return
   an empty buffer; the caller reads through `with_state_machine` or
   `with_leader_state_machine`, as `multi_raft` hosts already do.
2. Every in-tree caller that uses `read_state`'s returned bytes SHALL be
   listed in the design and either be left on non-persistent groups or moved
   to a query callback.
3. Engine iteration under `with_state_machine` SHALL be bounded by the caller
   (a limit or a byte budget) so a scan cannot hold the node mutex for
   seconds.

### Requirement 6: Split and merge without copies

**User Story:** As the split policy, I want splitting a 50 GB shard to cost a
metadata entry.

#### Acceptance Criteria

1. When the state machine satisfies `persistent_splittable_state_machine`,
   split apply SHALL hand each child its sub-range of the engine and SHALL NOT
   call `split_state`, `restore_from_snapshot` or copy any key.
2. Merge apply on a colocated target SHALL adopt the source's range in the
   same engine and SHALL NOT call `get_state` or `absorb`.
3. Merge when the source is NOT colocated SHALL keep today's refusal path (the
   placement driver colocates first); this spec does not add a remote merge.
4. Split and merge SHALL keep their existing admin entries, idempotence rules
   and failpoints (`raft/multiraft/merge/after_absorb_before_destroy` and
   friends), with persistent-path equivalents.
5. The split veto/hint hook (multi-raft Requirement 11) SHALL keep working.

### Requirement 7: Engines

**User Story:** As a developer, I want a fast in-memory engine for tests and a
real one for deployments.

#### Acceptance Criteria

1. `memory_kv_engine` SHALL implement `kv_engine` with a simulated durable
   sequence that tests can hold back, to exercise Requirement 3 and crash
   cases deterministically.
2. `rocksdb_kv_engine` SHALL implement it on RocksDB: one DB per node with a
   column family for data and one for transaction state, `WriteBatch` per
   apply, `Checkpoint` for checkpoints, `IngestExternalFile` for ingest,
   `DeleteRange` for drop, and the latest durable sequence from the WAL.
3. RocksDB SHALL be a vcpkg feature behind a Kconfig option
   (`KYTHIRA_ENGINE_ROCKSDB`, default off). With it off, no RocksDB header or
   library SHALL be built or linked.
4. Both engines SHALL pass one shared conformance test suite.
5. The added build time with the option on SHALL be measured on CI and
   recorded in the task.

### Requirement 8: Transaction state on disk

**User Story:** As a transactional host, I want locks and records to be as
durable and as large as the data.

#### Acceptance Criteria

1. `transactional_state_machine` over a persistent inner state machine SHALL
   keep `_intents`, `_records` and `_settled` in the engine (transaction
   column family, under the group range) and write them in the same batch as
   the inner effects.
2. Its snapshot SHALL then be the engine checkpoint; the `kythira-txn-sm`
   blob format stays for in-memory hosts only.
3. Split and merge SHALL partition transaction state by key range exactly as
   `split_state`/`absorb` do today (`txn` Requirement 6), by range.
4. The bank workload (`multi-raft-transactions` task 8) SHALL pass on a
   persistent host.

### Requirement 9: Reference state machine and crash tests

**User Story:** As a reviewer, I want proof on a real workload and real
crashes.

#### Acceptance Criteria

1. A `persistent_kv_state_machine` SHALL implement the Redis gateway's command
   set (`redis_kv_commands.hpp`) on the engine, so `redis_gateway_node` can
   run with either state machine selected at build time.
2. Failpoints SHALL exist between batch commit and engine sync, and between
   checkpoint and compaction; tests SHALL kill there and assert no lost or
   doubled entry.
3. A docker-chaos scenario SHALL `SIGKILL` replicas of a persistent group
   under write load and check the Redis linearizability checker passes. It
   SHALL run under Docker and rootless Podman.
4. A performance row SHALL compare the in-memory and RocksDB state machines
   on the existing multi-raft bench (proposals/s, p99, restart time).

### Requirement 10: Unsupported combinations fail loudly

**User Story:** As an operator, I want a configuration that cannot work to be
refused, not to lose data.

#### Acceptance Criteria

1. A persistent group with a persistence engine that restores state from its
   own snapshot store (the object-store engines, `raft_object_backup`) SHALL
   be rejected at configuration validation until object-store export of
   checkpoints is specified, because a fresh node restored from objects would
   have metadata and no state.
2. Until `.kiro/specs/file-install-snapshot/` lands, a persistent group SHALL
   refuse to send `InstallSnapshot` and SHALL keep its log (compaction off),
   with that limitation logged once at start.
