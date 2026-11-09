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

Because RocksDB syncs only when asked, a state machine on it should offer
`sync_durable()`, which makes every batch it has applied durable. The node
calls it before compacting (`_persistent_sync_lag_entries`). Without it the
durable applied index never moves and the group's log is never compacted.
`persistent_kv_state_machine`, the test machines and
`transactional_state_machine` (when its inner machine has one) all do.

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

## Transactions

`transactional_state_machine` can wrap a persistent state machine that also
satisfies `persistent_staged_state_machine`. Both state machines that ship
with the engine satisfy it. The wrapper keeps its locks, transaction records
and settled entries in the engine's `txn` column family, one value per key.
Each log entry's writes and its transaction state are committed in one
batch, so a crash cannot keep a commit and lose its lock release. Splits and
merges move transaction state with the key range.

The wrapper also keeps that state in memory, and reads it back from the
engine on restart. Memory therefore grows with the number of open
transactions and with records not yet cleaned up, not with the data.
Versioned (`mvcc::on`) hosts are not supported on the persistent path yet.

## The Redis gateway

`redis_gateway_node` built with `CONFIG_REDIS_GATEWAY_ENGINE=y` and started
with `KYTHIRA_CACHE_PERSISTENCE=engine` runs its shards on
`persistent_kv_state_machine` over RocksDB. See *Engine storage* in
`doc/redis-gateway.md` for the data directory layout, the disk budget and
`KYTHIRA_CACHE_SNAPSHOT_BYTES_PER_SEC`, which sets the node's snapshot pacer.

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
| `raft/persistent/before_sync` | Fails compaction just before the node syncs the engine, leaving applied batches that a power cut loses. The log replays them on restart. |
| `raft/persistent/before_compact` | Fails compaction after the snapshot point is saved and before the log is truncated. Entries the engine already holds do not apply again. |

## Measurements

The `persistence` axis of `multi_raft_performance_report` compares the Redis
gateway's state machine in RAM (`redis_kv_state_machine`) with the engine one
(`persistent_kv_state_machine`) over `memory_kv_engine` and over
`rocksdb_kv_engine`. Every arm keeps its Raft logs in the fsynced
`binary_file_persistence_engine`, so a difference between rows comes from the
state machine. The rows are in
`services/redis/tests/multi_raft_persistence_bench_rows.hpp`.

```
multi_raft_performance_report --axis persistence --data-dir /mnt/disk \
    --state-bytes 10000000000
```

- **Write rows**: three hosts and two shards on the in-process fabric, with
  closed-loop SETs, 16 in flight, of 128 B and 4 KiB values.
- **Restart rows**: one host and two shards, loaded with 64 KiB values up to
  `--state-bytes` (256 MiB by default), then stopped and started again over
  the same directories five times. Each time is measured to the moment
  every shard has a leader, has committed a new write, and returns the last
  value loaded. The log also shows the storage part of each restart, which
  covers building and starting the host, before the election.

Measured November 9, 2026 on a 4-vCPU Xeon 2.1 GHz cloud VM with 15 GiB of
RAM and ext4 on a virtio disk, built with g++ 13 at `-O2` (Boost futures):

| State machine | State | Load | Restart (median of 5) | Storage part | First restart after the load | Peak RSS |
|---|---|---|---|---|---|---|
| In memory | 1 GiB | 10 s | 4.5 s | 3.9 s | 6.3 s | |
| In memory | 2 GiB | 102 s | 8.6 s | 8.0 s | 12.0 s | 12.1 GiB |
| In memory | 4 GiB | out of memory during the load | | | | over 15 GiB |
| RocksDB | 1 GiB | 10 s | 0.44 s | 12 ms | 1.3 s | |
| RocksDB | 10 GB (9.3 GiB), two runs | 109-113 s | 0.92 s, 1.02 s | 0.43 s | 1.09 s, 1.26 s | 303 MiB |

- The in-memory machine's restart grows with its state, at about 4 s per
  GiB, because it restores the whole snapshot. It also needs about six
  times its state in RAM while it takes snapshots.
- The RocksDB machine restarts in about a second at 10 GB, in 303 MiB of
  RAM for the whole process. Most of
  that second is the election timeout, 300-600 ms here. The storage part at
  10 GB is mostly the log tail since the last compaction (about 64 MiB per
  shard at `_snapshot_threshold_bytes`) plus opening the database's files.
- The first restart after a write burst also replays RocksDB's WAL. The
  engine caps the WAL at 256 MiB (`rocksdb_kv_engine_options::max_total_wal_bytes`).
  RocksDB's default kept about 1 GiB of WAL alive behind the `meta` column
  family's small writes, and the first restart after a 1 GiB load took 2.4 s
  to open instead of 0.9 s.
- Write rows with 128 B values ran at 1,900-2,050 SETs/s, with a p99 of 15-17
  ms, on all three arms: the fsynced log sets the pace. Measured with 1,000
  operations per window, so the spreads (6-21%) are above the report's 10%
  stability bar.

### Limits

- A restarted node is ready once the election finishes and an entry of its
  term commits. With only the election left, the election timeout is the
  floor.
- Restart loads the log entries kept since the last compaction, so a large
  `_snapshot_threshold_bytes` makes restarts slower. It reads none of the
  engine's state.
- The engine shares one database per node. Every group's state counts
  against one disk budget (see *Engine storage* in `doc/redis-gateway.md`).
- `memory_kv_engine` has no restart row, because its state does not outlive
  the process.

## Tests

- `persistent_node_integration_test` runs on the memory engine.
- `persistent_node_rocksdb_test` runs the same suite on RocksDB, plus a 1 GB
  catch-up whose memory growth must stay under 64 MB.
- `persistent_node_fault_test` adds the fault point cases.
- `rocksdb_kv_engine_test` runs the engine conformance suite.
- `txn_state_machine_persistent_test` holds the persistent transaction
  wrapper to the in-memory one, and `multi_raft_txn_bank_persistent_test`
  runs the bank workload on a persistent host.
- `docker-engine-chaos-tests` (`services/redis/tests/docker/engine_chaos_test.cpp`)
  runs three gateway replicas on RocksDB in containers. It SIGKILLs them under
  load and checks the history for linearizability, and it kills a lagging
  replica in the middle of a snapshot transfer. It runs under Docker and
  rootless Podman.
- `multi_raft_performance_report_persistence` runs the `persistence` axis at
  a tenth of its budget, as a liveness check of each arm's write and restart
  path.
