# Design Document

## Overview

A persistent state machine keeps its state in a per-node ordered key-value
engine and never moves it as one buffer. `node<Types>` keeps every
consensus rule it has today; what changes is how it restarts, compacts,
reads, snapshots and splits when the state machine says its state is on disk.

```
multi_raft<Types, Key, GroupId>                   one per node process
  ├─ kv_engine  (memory_kv_engine | rocksdb_kv_engine)   one per node
  │     cf "data": <ns><group range keys>  → values
  │     cf "txn" : <ns><key>/intent|record|settled
  │     cf "meta": <ns>/group/<id>/applied_index, range, epoch
  └─ groups
        node<Types>  ──► SM = transactional_state_machine<persistent_kv_sm>
                            apply(entry, i) → one WriteBatch{effects, applied=i}
                            checkpoint(dir) / ingest(dir) / drop()
                            split_at(keys) / adopt(range)       (no copies)
```

## Key design decisions

### One engine per node, groups as key ranges

A RocksDB instance per group would give each group its own WAL, compaction
threads and file handles; with hundreds of groups per node that is the
overhead TiKV moved away from. One engine per node with each group owning a
range means a split or merge is a change of ownership of keys already on
disk. Every routing key is already the group's shard key, so the engine key is
`<host namespace><shard key>`; the namespace keeps two hosts in one process
(tests) apart.

The cost is shared fate: one engine's stall or corruption affects every group
on the node. That is the same failure domain as the node's disk, which those
groups already share.

### No fsync on apply

The Raft log is already durable (`durable-append-barrier`). An apply batch
that is lost in a power cut is replayed from the log, because the batch also
carried the applied index, so the recovered applied index is older too. The
only rule that makes this safe is Requirement 3.1: never compact the log past
what the engine has made durable. RocksDB reports that through the WAL's
synced sequence; we map sequence numbers to applied indices by recording, per
group, the sequence of each batch in a small in-memory ring (rebuilt as
`applied_index` on restart, where the two coincide).

Syncing on every apply would cost one fsync per entry per group, which is
exactly the cost group commit and the barrier spec worked to avoid.

### Snapshot = checkpoint plus metadata

A persistent group's "snapshot" for compaction is just metadata saved through
the existing persistence engine: `{last_included_index, term, configuration,
external_state = true}`. As built, the flag is a fixed marker
(`kythira-persistent-snapshot/1`) carried in `state_machine_state`, so no
persistence engine's snapshot encoding changes. The state itself
is already on disk. For sending to a follower, the state machine produces a
checkpoint directory and a `snapshot_manifest`
(`.kiro/specs/file-install-snapshot/` carries it on the wire). The checkpoint
is taken at `applied_index()` under the node mutex, which is the same point
`create_snapshot_locked` captures today.

### Reads

`serve_read_after_apply` returns `get_state()` today, which is fine for a
test state machine and fatal for a 50 GB one. For a persistent group the
function confirms the read index exactly as now and returns an empty buffer.
Callers that need data use `with_state_machine` after the read index, which is
what `multi_raft` and the Redis gateway already do (`redis_gateway_impl.hpp`:
"Never `node::read_state()`"). `multi_raft_impl.hpp:3373` is the one host path
that forwards `read_state` bytes; on a persistent host it returns the empty
buffer and is documented as a read-index barrier.

### Split and merge

| Step | Today | Persistent |
|---|---|---|
| Split apply | `split_state(keys)` returns blobs; derived child `restore_from_snapshot(blob)` | `split_at(keys)` returns child ranges; each child `open(engine, range)`; parent's meta entry narrowed in the same batch |
| Merge apply (colocated) | `source.get_state()` then `target.absorb(blob, range)` | `target.adopt(source_range)` and `source.drop_meta()` in one batch; keys stay where they are |
| Idempotence | range `covers` check | same check; adopt of an already-owned range is a no-op |

The applied index of a split child starts at the split entry's index, as the
derived child's does today.

As built (task 4): the source's metadata is folded into the target's by
`adopt` itself, in its one batch, so there is no separate `drop_meta`. A
split child's applied index is the parent's at the split entry, which
`split_at` writes into the child's metadata, and its seed snapshot is the
metadata-only marker at that index. `adopt` takes a neighbour on either side.

### Transaction state

The wrapper's three maps become key spaces in the `txn` column family:
`<ns><key>/i` (intent), `<ns><primary>/r/<txn_id>` (record),
`<ns><key>/s/<txn_id>` (settled). They are written in the same batch as the
inner effects, so a crash cannot separate a commit from its intent removal.
`intents_older_than` (the resolver's scan) needs a secondary index by
deadline: `<ns>/deadline/<ms>/<key>`, maintained in the same batch. Tier 2
MVCC, when it lands, maps onto versioned data keys (`key + inverted
commit_ts`), which this layout leaves room for.

## Components

| Component | File | Notes |
|---|---|---|
| `kv_engine`, `persistent_state_machine`, `persistent_splittable_state_machine` | `include/raft/persistent_state_machine.hpp` | Concepts plus `snapshot_manifest` (file name, size, SHA-256, and the engine's format tag) |
| `memory_kv_engine` | `include/raft/memory_kv_engine.hpp` | `std::map` per column family; a held-back durable sequence for tests; checkpoint writes one file per range |
| `rocksdb_kv_engine` | `include/raft/rocksdb_kv_engine.hpp`, `src/rocksdb_kv_engine.cpp` | Behind `KYTHIRA_ENGINE_ROCKSDB`; compiled in a `.cpp` so RocksDB headers stay out of every TU |
| `persistent_kv_state_machine` | `services/redis/include/kythira/redis/persistent_kv_state_machine.hpp` | Redis command set on the engine; TTL deadlines and sweeps as `redis-compatible-kv` Requirement 8 |
| Node integration | `include/raft/raft.hpp` | `if constexpr` branches at the eight sites in requirements.md |
| Host integration | `include/raft/multi_raft_impl.hpp` | Engine ownership, split/merge branches, namespace |
| Wrapper integration | `include/raft/txn_state_machine.hpp` | Persistent specialisation of the map storage |

## Error handling

- An engine write failure in `apply` is fatal for the group (the node stops
  the group and reports it), as a persistence failure is today; continuing
  would diverge from the log.
- Engine open failure or a meta key that disagrees with the shard map at
  start refuses the group, with both values logged.
- Ingest verifies the manifest's checksums before touching the live range.

## Testing strategy

- Engine conformance suite run against both engines.
- Property tests: random apply/crash/replay sequences on `memory_kv_engine`
  with held-back durability, asserting the state equals a reference applied
  from the log.
- Existing split/merge integration tests parameterised over both state
  machine kinds.
- Bank workload on a persistent host.
- Failpoint kill tests and one docker-chaos scenario (Docker and rootless
  Podman, service-name addressing).

## Open questions

1. RocksDB options: start from RocksDB defaults plus `level_compaction_dynamic_level_bytes`
   and a shared block cache sized from config, or tune now?
2. Should the Raft log move into the engine later (one WAL instead of two)?
   Not in this spec; the measurement in task 9 decides whether to ask.
3. LMDB as a second engine for read-heavy, small deployments (Raspberry Pi
   dogfood box)? The concept allows it; no task here.
