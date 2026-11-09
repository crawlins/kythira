# Persistent state machines

A Raft group normally keeps its state machine in memory and snapshots it as
one byte buffer. A **persistent** state machine keeps its state in a per-node
key-value engine instead and is never serialized as a whole. A group's state
can then be far larger than memory, and a restart takes seconds no matter how
much state there is. Specs: `.kiro/specs/persistent-state-machine/` and
`.kiro/specs/file-install-snapshot/`.

## Choosing the kind

`node<Types>` picks the path at compile time from `Types::state_machine_type`.
A type that satisfies `persistent_state_machine`
(`include/raft/persistent_state_machine.hpp`) runs on the persistent path.
Every existing byte-buffer state machine is unchanged.

| Engine | Header | When |
|---|---|---|
| `memory_kv_engine` | `include/raft/memory_kv_engine.hpp` | Tests. It can hold durability back and simulate a crash. |
| `rocksdb_kv_engine` | `include/raft/rocksdb_kv_engine.hpp` | Deployments. Opt-in: Kconfig `ENGINE_ROCKSDB` and the vcpkg feature `rocksdb`. Link `raft_rocksdb_engine`. |

The RocksDB engine opens one database per node, with column families `data`,
`txn` and `meta`. Every group on the node lives in its own key range of that
database. Writes go to the WAL without fsync, and `sync` makes them durable.

## What the node does differently

- **Restart:** the node starts at the engine's applied index and reloads
  nothing.
- **Snapshots:** a snapshot is metadata only: index, term, configuration, and
  a fixed marker instead of state bytes.
- **Compaction:** the log is truncated only up to the state machine's
  durable applied index, never past what a power cut could lose.
- **Reads:** a linearizable read confirms the read index and returns no bytes.
  Read the data through `with_state_machine`.
- **Restrictions:** object-store persistence engines are refused at compile
  time, because a node restored from objects would have metadata but no state.

## Catching up a follower

If a follower falls behind the leader's compacted log, the leader sends a
**format-1** InstallSnapshot instead of a byte buffer:

1. The leader checkpoints the group's range of its engine into a directory
   under `_snapshot_staging_directory`. With RocksDB that is one SST file per
   non-empty column family.
2. It streams the files as offset-addressed chunks of `_snapshot_chunk_size`
   bytes. The first chunk carries a manifest listing each file's name, size
   and SHA-256.
3. The follower stages the chunks in its own `_snapshot_staging_directory`.
   It fsyncs each file and checks it against the manifest, then the state
   machine ingests the files. RocksDB ingests all column families in one
   atomic call, so a range is never left half replaced.
4. The follower saves a metadata snapshot and continues by AppendEntries.

Followers that need the same snapshot point share one checkpoint. A
checkpoint no transfer is using is kept for
`_snapshot_checkpoint_max_unused_age` (10 minutes by default). It is deleted
sooner when a newer checkpoint is taken or the node steps down.

**Resume:** if a transfer breaks, the leader sends the same checkpoint again.
For that checkpoint (same index, term and manifest digest), the follower
answers `offset_mismatch` with `bytes_received`, and the leader continues
from there. A follower that restarts deletes what it had staged, so its
transfer starts again from zero.

**Pacing:** `_snapshot_pacer` is a `snapshot_pacer` shared by every group on
a node. It caps snapshot bytes per second, so a large transfer does not crowd
out heartbeats. It is unlimited when unset.

## Configuration

All fields are on `raft_configuration`.

| Field | Default | Meaning |
|---|---|---|
| `_persistent_log_compaction` | `true` | Compact persistent groups' logs. Off keeps the whole log (logged once at start). |
| `_persistent_sync_lag_entries` | `1` | Before compacting, sync the engine when the durable index lags by more than this many entries. `0` never syncs, which stops an engine like RocksDB from ever compacting. |
| `_snapshot_threshold_bytes` | 10 MB | Command bytes applied between compactions. |
| `_snapshot_staging_directory` | empty | Where checkpoints and incoming transfers are staged. Required for format-1 transfers. Put it on the engine's filesystem so ingest can hard-link instead of copying. |
| `_snapshot_disk_margin_bytes` | 64 MiB | Free space a follower keeps beyond a transfer's size before accepting it. |
| `_snapshot_checkpoint_max_unused_age` | 10 min | How long an unused checkpoint is kept for reuse. |
| `_snapshot_chunk_size` | 1 MB | Bytes per InstallSnapshot chunk. |
| `_snapshot_pacer` | none | Node-wide snapshot rate limit. |

## Refusals

Every refused InstallSnapshot carries a reason code:

- `stale_term`
- `stale_snapshot`
- `offset_mismatch`, together with `bytes_received` when the follower can resume
- `missing_first_chunk`
- `format_mismatch`
- `manifest_invalid`
- `disk_space`
- `checksum`
- `io_error`

The leader counts refusals in `raft_install_snapshot_refused`, labelled by
reason, and starts the transfer again on the next heartbeat that finds the
follower behind.

## Metrics

| Metric | What it reports |
|---|---|
| `raft_persistent_apply_lag` | Last applied index minus the durable applied index. |
| `raft_snapshot_checkpoints` | Checkpoints the leader holds. |
| `raft_snapshot_checkpoint_bytes` | Bytes in the checkpoints the leader holds. |
| `raft_install_snapshot_success` | Completed transfers. |
| `raft_install_snapshot_failed` | Failed transfers. |
| `raft_install_snapshot_refused` | Refused transfers, by reason. |
| `raft_install_snapshot_resumed` | Transfers a follower resumed. |

## Fault points

Builds with `FIU_ENABLE` (see `persistent_node_fault_test`) can arm these
fault points:

| Fault point | Effect |
|---|---|
| `raft/snapshot/corrupt_chunk` | Flips a byte of an incoming chunk. The transfer is refused with `checksum`. |
| `raft/snapshot/fsync_file` | Fails a staged file's fsync. The transfer is refused with `io_error`. |
| `raft/snapshot/crash_after_ingest` | Throws between ingest and the metadata save. On restart the node takes the applied index from the engine. |

## Tests

- `persistent_node_integration_test` runs on the memory engine.
- `persistent_node_rocksdb_test` runs the same suite on RocksDB, plus a 1 GB
  catch-up whose memory growth must stay under 64 MB.
- `persistent_node_fault_test` adds the fault point cases.
- `rocksdb_kv_engine_test` runs the engine conformance suite.
