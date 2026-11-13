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
is a different mechanism, below.

### Multi-region strong consistency (task 27)

AWS's rules, from the developer guide's "How global tables work" page,
which this design follows:

- A table with `MultiRegionConsistency=STRONG` (MRSC) has exactly three
  regions: three replicas, or two replicas and a witness. A witness holds
  the data but serves no reads or writes, and is listed in
  `DescribeTable`'s `GlobalTableWitnesses`.
- It is made with `UpdateTable` on an existing **empty** table, adding the
  replicas (`ReplicaUpdates`) and at most one witness
  (`GlobalTableWitnessUpdates`). Replicas cannot be added later, a single
  replica or the witness cannot be removed, and the mode cannot change.
  Removing two replicas, or one and the witness, leaves a single-region
  table.
- A write returns once it is in at least one other region. Strongly
  consistent reads and conditions see the latest version in every region.
  GSI reads stay eventually consistent.
- A region without a second region to form a quorum serves only
  eventually consistent reads; writes and strongly consistent reads answer
  errors until it is back and caught up.
- Transactions, TTL and LSIs are not supported on MRSC tables. Streams are
  off by default, may be enabled per replica, and every replica's stream
  holds the same records in the same order.
- Billing mode, throughput and GSIs are kept the same on every replica.
  Deletion protection, PITR, tags, Contributor Insights, Kinesis
  destinations, resource policies and the stream setting are per replica.

**One log for all three regions.** An MRSC table's items live in Raft
groups whose voters are spread evenly over the three regions: `n` voters
in each (`n` is 3 by default, 1 for tests), so a group has `3n` voters and
a quorum of `floor(3n/2) + 1`. No region holds a quorum alone, so every
commit includes a second region (AWS's "in at least one other region"),
and any one region can be lost. A witness region runs ordinary voters for
these groups; it stores the data, as AWS's witness does, but its catalog
has no table, so its gateways answer `ResourceNotFoundException`.
Log-only witness voters would save its disk and are left for later.

Because all three regions apply one Raft log, nothing has to be resolved
afterwards: there is no last-writer-wins step, every replica's change log
and stream are the same, and conditions are evaluated in apply as for any
table. Writes are serialized by the log, so a concurrent write from
another region waits instead of failing; `ReplicatedWriteConflictException`
is never returned. That is allowed, since AWS documents it as retryable.

**A second multi-raft host.** Each node runs a second `multi_raft` host,
the strong host, beside the cluster's own. It holds only MRSC groups, with
its own Raft listener and everything under `<data dir>/strong/` (its
RocksDB database, Raft logs, change log, `layout.json` and `group-ids`),
and Raft timings for a wide-area network (election timeouts in seconds,
not hundreds of milliseconds), so cross-region latency never slows a
local group's elections. Its peers are every strong voter in the three
regions (`KYTHIRA_DYNAMODB_STRONG_PEERS`, `id=region@url`), with each
region's CA trusted by the others. Strong node ids are unique across the
three regions, and group ids come from each voter's range in the sorted
list of all strong voters, the per-voter scheme splits already use, so
two regions splitting at once never collide. Ops for a strong group's
leader go to its gateway at `/_kythira/dynamodb/strong-forward`, signed
with a secret all three regions share (`KYTHIRA_DYNAMODB_STRONG_SECRET`).

**The strong catalog.** The strong host bootstraps the same layout as a
cluster's host: a catalog group and one item group over `t/`. Its
catalog group, with voters in all three regions, is the strong catalog.
It holds what the regions must agree on: each MRSC table's strong id, its
shared settings (key schema, billing, throughput, GSIs), its regions and
witness, and its status. Strong table and index ids start at 2^62, so
they never meet a cluster's own ids, and a key's table id says which host
owns it. Each region's own catalog keeps the per-replica settings, and
its descriptor carries the strong ids. The gateway's backend routes by
table id: catalog commands and keys under a cluster id go to the
cluster's host, and keys under a strong id go to the strong host. The
handlers, the propagator, drop steps and range reads need no other
change. An `UpdateTable` that changes a shared setting is a strong
catalog entry, applied by each region's catalog leader to its local
descriptor.

**Making a table strong.**

1. `UpdateTable` in region A checks the request: three regions in total,
   each configured as a strong peer, no LSI, TTL off, the table empty.
   It answers `ValidationException` otherwise, with AWS's wording.
2. A's catalog seals the table (its groups refuse writes from here) and
   checks it is still empty; a write that slipped in before the seal fails
   the conversion and the replicas end `CREATION_FAILED`.
3. A proposes the table to the strong catalog, which creates the item
   groups (and GSI groups) in the strong host.
4. Each replica region's catalog leader sees the entry, creates its local
   descriptor (`CREATING`, then `ACTIVE` once its strong host has the
   groups), and the witness region records only the membership.
5. A's local ranges for the table are dropped. They were empty, which is
   why AWS requires an empty table: nothing has to be copied.

**Leaving strong mode.** Removing two replicas, or a replica and the
witness, is a membership change on every group of the table (joint
consensus, which the multi-raft host already has) down to the remaining
region's voters. The table stays in that region's strong host as a
single-region table with `MultiRegionConsistency` absent. Copying it back
into the local host would cost a full copy for no gain. `DeleteTable` on
one replica of a three-region table is refused, as AWS refuses removing a
single replica; deleting the last replica deletes the groups.

**Requests.**

- A write goes to the group's leader, which may be in another region.
  The forward link reaches leaders in other regions through their
  gateways, authenticated with the replication principal that global
  tables already use. A write from a region without the leader therefore
  costs about two cross-region round trips, against one on AWS. Spreading
  leaders over the regions, and moving a group's leadership toward the
  region that writes to it most, are later improvements.
- A strongly consistent read takes the read barrier on the leader, as
  today, so it is forwarded too. Follower read-index, which would answer
  from the local replica after one round trip to the leader, is a later
  improvement.
- An eventually consistent read is answered by a voter in the caller's
  region. It keeps working when the region is cut off.
- A region that cannot reach a quorum cannot commit or take a barrier.
  Writes and strongly consistent reads give up after the request deadline
  and answer the error AWS returns in that case. AWS does not document
  which code that is, so it must be recorded before it is chosen; until
  then the module answers `InternalServerError`.
- `TransactWriteItems`, `TransactGetItems` and PartiQL transactions naming
  an MRSC table, `UpdateTimeToLive` enabling TTL, and an LSI in
  `CreateTable` or `UpdateTable` are refused with AWS's error.

**Splits, GSIs and the change log.** Splits work as for any group: the cut
is a Raft entry, so every region's replica splits at the same index, and
stream shard lineage is identical everywhere. GSI groups also live in the
strong host, spread over the regions, so every region has its index; the
propagator runs on each base group's leader, and index reads stay
eventually consistent. Per-replica consumers of the change log (Streams,
Kinesis destinations, PITR, backups) read their own region's replica, so a
replica's stream setting and destinations stay local, as on AWS.

**What it costs.** Every write to an MRSC table pays at least one
cross-region round trip, and the strong host doubles each node's Raft
listeners and databases. Tables that are not MRSC are untouched: they
never enter the strong host.

### Backups and point-in-time recovery

A backup is an engine checkpoint of each of the table's ranges (base, LSI,
GSI) at one catalog-chosen applied index per group, uploaded with a manifest
to a configured object store. Point-in-time recovery adds the change log
archived to the same store; a restore loads the latest backup before the
target time and replays change records up to it into a new table id.
Exports write the AWS export layout (manifest files plus data files) from a
backup; imports create a table and bulk-ingest by building engine files.

A bulk put (Requirement 22) is an import into an existing table: a catalog
job reads the input files from the configured store and proposes ordinary
batched writes, rate-limited by `MaxWriteRequestUnits`, so conditions,
LSIs, GSIs and the change log see every item; its status and failure
report live in the catalog beside export and import jobs.

A vector index is a further asynchronous index fed by the change log, like a
GSI: each item group keeps an approximate nearest-neighbour graph (HNSW) of
its own items' vectors, and `SearchVectors` asks every group the table
spans for its `TopK`, merges the results by score and re-reads the winning
items so that a search never returns a deleted item. The choice of graph
library, and whether results must match AWS's ranking exactly or only its
documented distance functions, is open until the task starts.

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
   cross-cluster Raft group, or list it as unsupported? Decided in task
   25 (Clark, 2026-11-13): build it, as task 27, designed above.
2. Should `dynamodb_gateway_node` and `kinesis_gateway_node` also be
   offered as one combined binary for small deployments?
3. Which error does AWS return for a write or a strongly consistent read
   on an MRSC replica whose region has no quorum? Recording it needs a
   three-region MRSC table and an AWS FIS region isolation experiment,
   which is real spend and needs Clark's go.
