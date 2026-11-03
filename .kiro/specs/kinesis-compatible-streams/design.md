# Design Document

## Overview

```
producers (SDK, CLI, KPL)      consumers (GetRecords, KCL, EFO, Flink)
        │ HTTPS JSON 1.1 / CBOR, SigV4          │ HTTP/2 event stream (EFO)
        ▼                                       ▼
kinesis_gateway_node (any node)
  aws_json_protocol (+CBOR) ── sigv4_verifier ── aws_request_authorizer
  kinesis_front_end: hash partition key → open shard → routing key
                     forward writes to the group leader; reads from any replica
  lease_api (native KCL lease layer)
        │
        ▼ one multi_raft host per node
  \0meta/kinesis/…          catalog: streams, shard tables, consumers, policies
  k/<stream id>/<ordinal>   one routing key per Kinesis shard
  each group: stream_state_machine (small, byte-buffer snapshot)
                 └─ record_segment_store (core): local segments, peer fetch,
                    retention trim, object-store tiering, encryption
```

## Key design decisions

### A Kinesis shard is a routing key, not a Raft group

Kinesis shards are user-visible, have lineage, and stay readable after they
close. Kythira groups split, merge and move on their own. Mapping each
Kinesis shard to one routing key `k/<stream id>/<ordinal>` lets many Kinesis
shards share a group, keeps every record of a shard in one group, and lets
Kythira rebalance groups without touching Kinesis semantics. The stream state
machine's split hook only offers split points between routing keys.

Kinesis resharding is logical: `SplitShard` writes the parent's ending
sequence number and creates two new ordinals with their hash ranges and
parent links, in one catalog entry plus one entry per affected group. When
the parent and children land in different groups, the update runs as a
tier-1 transaction.

### Stream state is small; record bytes are not in the snapshot

`stream_state_machine` keeps per shard: the next sequence counter, the last
arrival timestamp, open/closed state, the segment index (id, first and last
sequence, first and last timestamp, size, SHA-256, local or tiered, object
key), the trim horizon, and throttle configuration. That fits the existing byte-buffer state machine contract, so
Kinesis does not need the persistent engine.

Record bytes go to the record segment store during `apply()`. The applied
index and the segment append are made durable together: the store fsyncs
the active segment before `apply()` returns for entries the node
acknowledges, using the same barrier as the binary file persistence engine.
On restart, a segment tail beyond the snapshot's index is truncated and the
log tail replayed.

### Record segment store (core)

`include/raft/record_segment_store.hpp`, generalised from the build cache's
chunk store and fetcher:

- `append(stream key, records) -> segment position`; segments roll at a size
  or age bound.
- `read(stream key, from position, max bytes)` on any replica; a missing
  local segment is fetched from a peer (`cache_chunk_fetcher` pattern) or
  from object storage if tiered.
- `seal(segment)` and `tier(segment, store)` driven by leader-proposed
  entries so every replica agrees on what is local and what is tiered.
- `trim(before)` by leader-proposed retention entries.
- Encryption: segments for an encrypted stream are AES-256-GCM per block,
  with a data key wrapped by `kms_key_provider`; the wrapped key sits in the
  segment header and the stream state.

DynamoDB's change log is the second user, so the store has no Kinesis
types in its interface.

### Sequence numbers

AWS sequence numbers are long decimal strings. Ours encode
`(shard epoch, per-shard counter, sub-record index)` into a fixed-width
decimal of AWS's length in the `aws` profile. The counter lives in the
stream state machine and increments in `apply()`, so it is independent of
the Raft index and of which group holds the shard. A compact form (just the
counter) is available as a profile switch.

### Iterators

Iterators are `HMAC-SHA256(cluster iterator key, stream id, shard ordinal,
position, expiry, principal)` tokens, base64. The iterator key is generated
at cluster bootstrap, sealed like the CA key, and rotated with an overlap
window. No iterator state is replicated, and any node can serve `GetRecords`.

### Writes and throughput

`PutRecords` is split by target shard; each shard's records become one Raft
entry. The leader batches concurrent entries for a group (group commit), so
one group serves several hot shards. Token buckets live on the leader in
memory and reset on leader change, which can briefly admit up to one extra
bucket's worth of traffic; that matches AWS's documented burst tolerance and
is noted in the docs.

### Enhanced fan-out

`SubscribeToShard` is served over HTTP/2 by the node that receives it. It
reads from the local replica's committed segments and pushes
`SubscribeToShardEvent` frames, with per-consumer token buckets. On leader
change nothing breaks, because reads are replica-local; if the local
replica is removed, the stream ends with the error event AWS uses and the
client resubscribes with its continuation sequence number. HTTP/2 comes from
the Proxygen or nghttp2 stack already in the tree; the choice is made in
task 13 by which one the module may link under the boundary rules.

### Native KCL lease layer

Lease API on `kinesis_gateway_node` under `/v1/leases/<application>/`:
`PUT` with `If-Match: <version>` or `If-None-Match: *`, `PATCH` with a
version, `DELETE` with a version, `GET` (linearizable) and `LIST` by prefix
with pagination. Leases live in the catalog range under
`\0meta/kinesis/leases/<application>/`. The Java library
`kythira-kcl-leases` implements `LeaseRefresher` (all methods),
`LeaseManagementFactory` and a `Checkpointer` against it, signs requests
with SigV4 using the SDK's signer, and is published as a Gradle artifact
from the module's CI. It supports KCL 2.x only; KCL 3.x uses a DynamoDB
endpoint (Kythira's or AWS's).

### Compatibility profile

`kinesis_compat_profile` mirrors DynamoDB's: switches resolved per request
from cluster config overlaid with the stream's `kythira:compat:*` tags.
Default `aws` everywhere, standalone and test clusters included, so
throttling is on unless an operator turns the throughput switch off (Clark,
2026-11-03). Throttling is an ordinary profile switch, not a special mode.

### Placement in the tree

```
services/kinesis/
  Kconfig, CMakeLists.txt, boundary-allow.txt
  include/kythira/kinesis/   stream state machine, catalog, front end,
                             sequence numbers, iterators, resharding,
                             on-demand controller, fan-out, lease api,
                             action table, compat profile, kms providers
  cmd/kinesis_gateway_node/
  java/kythira-kcl-leases/   Gradle project
  tests/  tests/docker/  docker/  doc/
core additions (include/raft/):
  record_segment_store.hpp   shared with DynamoDB's change log
  aws_json_protocol.hpp      CBOR added (JSON built by the DynamoDB spec)
  aws_eventstream.hpp        event-stream framing (CRC, headers)
```

## Components

| Component | Location | Notes |
|---|---|---|
| Record segment store | core `record_segment_store.hpp` | Segments, peer fetch, trim, tiering, encryption |
| CBOR and event stream | core `aws_json_protocol.hpp`, `aws_eventstream.hpp` | Shared AWS protocol pieces |
| Stream state machine | `stream_state_machine.hpp` | Counters, segment index, split hook |
| Catalog | `catalog.hpp` | Streams, shards, consumers, leases, policies |
| Front end | `front_end.hpp` | Beast host, forwarding, replica reads |
| Iterators | `iterators.hpp` | HMAC tokens, key rotation |
| Resharding | `resharding.hpp` | Split, merge, uniform scaling, lineage |
| On-demand controller | `on_demand.hpp` | Rate observation, split decisions |
| Fan-out | `fan_out.hpp` | HTTP/2 subscriptions |
| Lease API | `lease_api.hpp` | Conditional writes for the Java layer |
| KMS providers | `kms_key_provider.hpp` | Local sealed keys; cloud adapters |
| Action table | `services/action_table.hpp` | Registered with `aws_request_authorizer` |
| Daemon | `cmd/kinesis_gateway_node` | Flags, TLS, forwarding, health |
| Java lease layer | `java/kythira-kcl-leases` | KCL 2.x |

## Error handling

- A segment failing its SHA-256 on fetch: try another peer, then object
  storage; if none is good, `GetRecords` returns `InternalFailure` and a
  metric fires. The segment is never silently skipped.
- Disk floor reached: writes return `InternalFailure` with a Kythira message
  naming the disk guard; reads and tiering continue, and tiering is
  prioritised to free space.
- Iterator key rotation: tokens signed by the previous key stay valid for
  the iterator lifetime.

## Testing strategy

See Requirement 17. The reference for differential tests is an open-source
Kinesis emulator run as a container; where it diverges from AWS, recorded
AWS responses (Requirement 17.4) settle it. Evidence tables of what the
SDKs, KPL, KCL 2.x/3.x and the Flink connector put on the wire are recorded
once against a stub and checked in.

## Open questions

1. Which open-source emulator to use as the differential reference
   (LocalStack's community edition now needs a licence for some services,
   which blocked an earlier S3 tier)?
2. Should the native lease layer also be offered for other languages
   through the MultiLangDaemon? It works there automatically, because the
   daemon runs Java KCL; the question is whether to document and test it.
