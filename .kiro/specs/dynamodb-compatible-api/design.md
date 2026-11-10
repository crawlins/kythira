# Design Document

## Overview

```
AWS SDK / CLI / KCL / lock client / Terraform
        │ HTTPS, AWS JSON 1.0, SigV4
        ▼
dynamodb_gateway_node (any node)
  aws_json_protocol codec ── sigv4_verifier ── aws_request_authorizer
  (core)                     (core, Z task 6)  (IAM learner replica, or static keys)
  dynamo_front_end: parse request, compile expressions to trees,
                    route by key to the shard, forward to its leader
        │
        ▼ one multi_raft host per node
  groups over the routing key space
   ├─ \0meta/dynamodb/…        catalog group(s)
   ├─ t/<table id>/…           base items + LSI entries
   └─ t/<index id>/…           GSI entries
  each group: transactional_state_machine<dynamo_item_state_machine>
              on the node's kv_engine (persistent-state-machine)
              ── change log (record_segment_store, kinesis-compatible-streams)
                   ├─► GSI propagator (leader-side)
                   ├─► DynamoDB Streams API
                   ├─► Kinesis streaming destination publisher
                   ├─► global-table replicator
                   └─► point-in-time recovery archive
```

## Key design decisions

### Items live in the persistent engine from the first task

The scoping doc considered a RAM-resident first release. Clark left table
size to this implementation, and items X and Y will land before this spec
starts (both are earlier on the priority list). So `dynamo_item_state_machine`
is written against the `kv_engine` concept from day one: unit tests use
`memory_kv_engine`, and production uses RocksDB. There is never a
whole-table byte-buffer snapshot to migrate away from, and splits copy no
data.

### Key layout

All keys are ordered byte strings inside the engine:

| Key space | Key | Value |
|---|---|---|
| Catalog | `\0meta/dynamodb/table/<name>` | table descriptor (versioned) |
| Base item | `t/<table id>/<h16(pk)><enc(pk)>\x00<enc(sk)>` | encoded item |
| LSI entry | `t/<table id>/<h16(pk)><enc(pk)>\x01<lsi no><enc(lsi sk)><enc(base sk)>` | projected item |
| GSI entry | `t/<index id>/<h16(gsi pk)><enc(gsi pk)>\x00<enc(gsi sk)><enc(base key)>` | projected item, base version |
| Item version | the base item key, in the meta column family | 64-bit version (for GSI and replication ordering); kept with the item so a group's range covers both |
| TTL index | `x/<table id>/<expiry><base key>` | empty (derived, rebuilt on restore if missing) |

`h16` is a 2-byte hash of the encoded partition key, so partitions spread
over groups and parallel `Scan` segments are slices of the hash space. The
encoded partition key after it keeps an item collection contiguous, so
`Query` is one range read. Table and index ids are 64-bit and never reused.

### Expression trees, not text, in the log

The gateway parses every expression (and every legacy parameter, and every
PartiQL statement) into one tree type, `dynamo_expr`, validates it against
the table's key schema, and serializes it with a version byte into the Raft
entry. `apply()` evaluates the tree on the committed item. This keeps the
parser out of `apply()`, makes replicas agree even if a later release
changes the parser, and gives one evaluation path for the JSON API, legacy
parameters and PartiQL.

### Decimal

`dynamo_decimal`: sign, 38-digit unsigned mantissa held in two 64-bit words,
and a base-10 exponent. Parsing, formatting, `+`, `-` and comparison are
exact; anything that would need more than 38 digits fails with AWS's
`ValidationException` text. The key encoding is sign byte, biased exponent,
then mantissa digits as base-100 bytes, so byte order equals numeric order.
No floating point anywhere.

### Conditions in apply(); transactions through the wrapper

A single-item write is one Raft entry; `apply()` reads the current item,
evaluates the condition, applies the update, bumps the item version, updates
LSI entries, writes the TTL index, appends the change record, and writes the
applied index, all in one engine batch.

`TransactWriteItems` uses the tier-1 coordinator. The wrapper gets one new
optional hook on the inner state machine:

```cpp
template <typename SM>
concept txn_validating_state_machine = requires(const SM& sm,
    const std::string& key, const std::vector<std::byte>& write) {
    { sm.validate(key, write) } -> std::same_as<txn_validation>;  // ok | condition_failed{reason}
};
```

`prepare` and `one_phase` call it while installing the intent, so the
condition is checked under the lock. A `ConditionCheck` is an intent with an
empty write. The transaction id is derived from `(principal, table set,
ClientRequestToken)` when a token is given, so a retry finds the first
attempt's record; records with a token are kept 10 minutes past settlement.

### GSIs: asynchronous by default, from the change log

The base group's leader runs a propagator per GSI that tails the change
log and proposes `gsi_put(index key, base key, base version, projection)` /
`gsi_delete(...)` to the GSI's groups through the multi-raft client. `apply()`
on the GSI side keeps the entry with the highest base version, so a replayed
or reordered proposal never regresses it. The propagator's position is
stored in the base group's state, so a new leader resumes from it. Lag is
`last change seq - propagated seq` per index.

The synchronous switch instead turns a base write that changes an indexed
attribute into a tier-1 transaction over the base key and the old and new
index keys.

### Change log and Streams

The change log is the core `record_segment_store` built by
`kinesis-compatible-streams` task 3: append in `apply()`, segments outside
the snapshot, peer fetch, leader-proposed trim, optional object-store
tiering. DynamoDB uses one log per group. A stream shard is one group for
one epoch: a split closes the parent's stream shard at the split index and
opens one per child naming the parent; merges are vetoed for groups with a
stream-enabled table. Stream sequence numbers are `(epoch, log seq)` encoded
as AWS-shaped decimal strings. Retention is trimmed per stream shard, 24
hours.

### Catalog

The catalog is a reserved range in the same multi-raft host, small enough
for one group. A table operation is a catalog entry; follow-on work (create
ranges, backfill, background delete) is driven by the catalog leader and
recorded as progress entries, so a leader change resumes it. Status
transitions are catalog entries, which is what SDK waiters poll.

### Global tables

A replicator on each base group's leader tails the change log and sends
records to the peer cluster's `dynamodb_gateway_node` over an internal
authenticated API (mTLS with the peer's Kythira CA, or SigV4 with a
replication principal). The receiver applies a record only if its
`(timestamp, region id)` is newer than the item's, AWS's last-writer-wins
rule. Item timestamps come from the leader's clamped wall clock, the same
assumption Kinesis arrival timestamps use. Multi-region strong consistency
needs a cross-cluster quorum and is left to task 25's decision.

### Backups and point-in-time recovery

A backup is an engine checkpoint of each of the table's ranges (base, LSI,
GSI) at one catalog-chosen applied index per group, uploaded with a manifest
to a configured object store. Point-in-time recovery adds the change log
archived to the same store; a restore loads the latest backup before the
target time and replays change records up to it into a new table id.
Exports write the AWS export layout (manifest files plus data files) from a
backup; imports create a table and bulk-ingest by building engine files.

### Compatibility profile

`dynamo_compat_profile` is a struct of switches resolved per request from
cluster config overlaid with the table's `kythira:compat:*` tags. The
default `aws` profile sets every switch to AWS's behaviour, and it is the
default everywhere, standalone and test clusters included: throttling is on
unless an operator turns the capacity switch off (Clark, 2026-11-03). The
front end checks limits; the leader's throttler reads the capacity
switches; the propagator reads the GSI switch; the TTL sweeper reads the
delay switch.

### Placement in the tree

```
services/dynamodb/
  Kconfig, CMakeLists.txt, boundary-allow.txt
  include/kythira/dynamodb/   decimal, item codec, expr parser + evaluator,
                              item state machine, catalog, propagator,
                              streams, ttl, partiql, backup, replication,
                              front end, action table, compat profile
  cmd/dynamodb_gateway_node/
  tests/  tests/docker/  docker/  doc/
core additions (include/raft/):
  aws_json_protocol.hpp       JSON 1.0/1.1 codec, error shapes, CRC32 (shared with Kinesis)
  txn_validating hook         in transactional_state_machine (multi-raft-transactions)
```

## Components

| Component | Location | Notes |
|---|---|---|
| AWS JSON codec | core `aws_json_protocol.hpp` | Shapes generated from the pinned model; shared with Kinesis |
| Decimal | `decimal.hpp` | Exact, order-preserving encoding |
| Item codec | `item_codec.hpp` | Compact binary item form; size formula |
| Expressions | `expr_parser.hpp`, `expr_eval.hpp` | One tree for expressions, legacy parameters and PartiQL |
| Item state machine | `item_state_machine.hpp` | `persistent_splittable_state_machine`, `validate` hook, split vetoes |
| Catalog | `catalog.hpp` | Descriptors, status machine, background jobs |
| Query/Scan | `query.hpp` | Range reads, cursors across shards |
| GSI propagator | `gsi_propagator.hpp` | Leader-side, versioned, resumable |
| Streams | `streams.hpp` | Stream shards over the change log |
| TTL | `ttl_sweeper.hpp` | Leader-proposed conditional deletes |
| PartiQL | `partiql.hpp` | Compiles to the same requests |
| Backup, PITR, export, import | `backup.hpp`, `export.hpp` | Object-store manifests |
| Replication | `global_tables.hpp` | Peer clusters, last-writer-wins |
| Kinesis destination | `kinesis_destination.hpp` | Publishes to Kythira or AWS Kinesis |
| Throttler, insights | `capacity.hpp`, `insights.hpp` | Token buckets, sampled top-k |
| Front end | `front_end.hpp` | Beast host shared with `cache_http_front_end` |
| Action table | `services/action_table.hpp` | Registered with `aws_request_authorizer` |
| Daemon | `cmd/dynamodb_gateway_node` | Flags, TLS, forwarding, health |

## Error handling

- A condition failure is a normal result of `apply()`, returned to the
  client; it never stops the group.
- An expression tree with an unknown version (a newer leader, older
  follower) stops the group with a clear log line, so a rolling upgrade must
  upgrade before enabling new expression features. The tree version is
  bumped only for new syntax.
- Propagator, replicator and publisher failures retry with backoff and raise
  lag metrics; they never block base writes.
- Disk floor reached: writes return `InternalServerError` with a Kythira
  message naming the disk guard; reads continue.

## Testing strategy

See Requirement 21. Unit tests run on `memory_kv_engine`; the module CI runs
the integration suite on RocksDB. DynamoDB Local runs as a container in the
differential job (Docker and rootless Podman). The evidence tables for KCL,
the lock client and the Streams adapter are recorded once against a stub, as
`redis-compatible-kv` did for sccache, and checked in.

## Open questions

1. Multi-region strong consistency for global tables: build it on a
   cross-cluster Raft group, or list it as unsupported?
2. Should `dynamodb_gateway_node` and `kinesis_gateway_node` also be
   offered as one combined binary for small deployments?
