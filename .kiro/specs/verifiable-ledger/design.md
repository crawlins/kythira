# Design Document

## Overview

A ledger is one Raft group whose state machine is a persistent state machine
(`.kiro/specs/persistent-state-machine/`). Its journal is applied state; the
Raft log only carries proposals and is compacted as usual.

```
client ──HTTP/mTLS──► ledger_node (any node)
                        │ forwards to the ledger group's leader
                        ▼
leader: txn session  ── reads at journal seq S (revisions CF, "newest ≤ S")
                     ── buffers writes, records read set
commit ──► Raft entry {txn id, ts, read set, write set, statements, blockHash}
                        ▼ every replica
ledger_state_machine::apply
   validate read set vs change markers  ── conflict → result occ_conflict, no block
   build revisions, entriesHash, blockHash ── mismatch with entry → stop group
   one engine batch: revisions, current, index entries, markers,
                     journal block, Merkle nodes, applied index
```

## Key design decisions

### One ledger per group, no cross-shard machinery

QLDB itself had one strand per ledger. Mapping a ledger to one group makes
every transaction single-shard, so the multi-raft transaction tiers, the
timestamp oracle and the tier-2 multi-version state machine are not needed.
The cost is a throughput ceiling of one group's commit rate per ledger; a
cluster scales by ledger count. A sharded ledger would need cross-group
proofs and is out of scope.

### Blocks live in the engine first, segments later

Writing the block to a file and the revisions to the engine would need a
two-store recovery protocol. Instead the block goes into the `journal`
column family in the same batch as everything else. A leader-proposed
`seal` entry later names a sequence range; every replica writes those blocks
to a segment file, fsyncs it, records `{range, file, sha256}` in the
`segments` key space and deletes the range from `journal` in one batch.
Because sealing is deterministic, replicas produce byte-identical segments,
and a crash mid-seal is redone on replay (the batch did not commit, so the
blocks are still in `journal`; a partial file is overwritten).

### Hashing

- Revision hash = `dot(ionhash(data), ionhash(metadata))`; a delete hashes
  metadata only. This matches what QLDB's docs and samples describe; the
  test vectors in task 2 pin it.
- Statement hash = `ionhash(statement)`, where a native statement is an Ion
  struct (`{op, table, id, value, ...}`).
- `entriesHash` = Merkle root with `dot` over statement hashes followed by
  revision hashes, in block order.
- `blockHash` = `dot(dot(entriesHash, ionhash({hashVersion, seq, txId,
  blockTimestamp})), previousBlockHash)`. Only `dot` sits above the Ion
  hashes, so a revision proof is one fold: the revision's path in the
  entries tree, the header hash, the previous block hash, then the
  accumulator path to the digest (Requirement 4.3, 4.6). An Ion hash over a
  header that contains entriesHash, as first written here, would break that
  chain at the block: no `dot` fold can pass through it. Changed in task 4,
  before any ledger existed.
- Ion hash is implemented in C++ over ion-c's reader (no C/C++
  implementation exists upstream); it is small and has public test vectors.

`hashVersion` (start at 1) lets a later QLDB-compatible layer, or a fix,
change the field list without breaking verification of old blocks.

### Merkle accumulator

Leaves are block hashes in sequence order; the tree is the standard
left-filled binary tree (a node at level k covers 2^k consecutive leaves).
Interior nodes are stored in `merkle/<level>/<index>` when complete; the
frontier (at most one incomplete node per level) is cached in memory and
rebuilt from stored nodes on open. The digest at tip t folds the frontier
for t with `dot`. Because `dot` is commutative, proofs carry no left/right
flags and verify with the stock QLDB procedure. A digest at an older tip
uses the stored complete nodes plus a recomputed frontier for t, O(log n).

### Read set and conflict markers

| Read | Recorded | Change marker checked at apply |
|---|---|---|
| Document by id | `(table, id, version)` | `current/<table>/<id>` version |
| Index lookup | `(table, index, ionhash(value))` | `ichg/<table>/<index>/<hash>` = last seq that added or removed a doc under that key |
| Full scan | `(table)` | `tchg/<table>` = last seq that changed any doc in the table |

Every write updates the markers it touches in its batch. Validation is
O(read set). Snapshot reads at S come from `rev/<table>/<id>/<version>` by
a reverse seek, so no separate MVCC layer exists.

### Sessions

A transaction token names the leader's term and a session id. Session state
(S, read set, buffered writes) lives only in leader memory with the
transaction timeout as its TTL. A request that reaches a follower is
forwarded; a token from an older term fails with `session_expired`, which
the client retries.

### Engine layout

All under the ledger group's range `l/<ledger id>/`:

| Key space | Key | Value |
|---|---|---|
| `meta` | tables, indexes, segment list, export jobs, last block | Ion |
| `current` | `<table>/<id>` | version, seq |
| `rev` | `<table>/<id>/<version>` | revision Ion, revision hash, seq |
| `idx` | `<table>/<index>/<value hash>/<id>` | empty |
| `ichg`, `tchg` | as above | seq |
| `hist` | `<table>/<seq>/<id>` | version (history by time range) |
| `journal` | `<seq>` | block Ion |
| `merkle` | `<level>/<index>` | 32 bytes |
| `segments` | `<first seq>` | range, file name, size, sha256, object key |

### Placement in the tree

`services/ledger/include/kythira/ledger/*.hpp`, `services/ledger/src/`,
`services/ledger/tests/`, `cmd/ledger_node/`, `cmd/ledger_ctl/`. A CMake
check (a regex over includes) fails the build if a ledger file includes a
`*_impl.hpp`. This keeps the module extractable to its own repo once
Kythira exports a complete package.

## Components

| Component | File | Notes |
|---|---|---|
| Ion hash | `services/ledger/include/kythira/ledger/ion_hash.hpp` | SHA-256 Ion hash over ion-c |
| `dot`, Merkle accumulator, proofs | `merkle.hpp` | Pure functions plus an engine-backed store |
| Block and revision types | `journal_types.hpp` | Ion (de)serialization, hashing |
| `ledger_state_machine` | `ledger_state_machine.hpp` | `persistent_state_machine`; apply, validation, seal, split veto |
| Session manager | `ledger_sessions.hpp` | Leader-side transactions |
| Query | `ledger_query.hpp` | Id get, index equality, bounded scan |
| Segment store and offload | `journal_segments.hpp` | Uses `object_store_persistence` clients |
| HTTP front end | `ledger_http_front_end.hpp` | Same server stack as `cache_http_front_end` |
| Client | `ledger_client.hpp` | OCC retry |
| Daemon and tool | `cmd/ledger_node`, `cmd/ledger_ctl` | |

## Error handling

- Hash mismatch on apply: stop the group (as an engine write failure does
  in persistent-state-machine), log both hashes, metric.
- A segment whose SHA-256 does not match on fetch: read error to the client,
  metric, and refetch from a peer replica's local copy if present.
- Disk floor reached: reject new transactions with `insufficient_storage`;
  sealing and offload keep running.

## Testing strategy

See requirements.md Requirement 12. Unit tests run on `memory_kv_engine`;
one CI leg runs the ledger suite on RocksDB.

## Open questions

1. Should the ledger catalog share the CA cluster's or IAM's style of a
   dedicated reserved group, or be an ordinary group with a fixed id?
2. Default for digest signing: off, or on whenever a CA is configured?
3. Is a QLDB journal-export importer wanted (scoping doc phase 1, optional)?
   Not in this spec's tasks until there is a real export to test with.
