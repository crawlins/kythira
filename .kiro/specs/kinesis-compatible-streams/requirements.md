# Requirements Document

## Introduction

This spec adds a **Kinesis Data Streams-compatible service** to Kythira.
Unmodified AWS SDKs, the AWS CLI, KPL producers, KCL consumers, enhanced
fan-out consumers, the Flink and Spark Kinesis connectors and other Kinesis
tools can point `--endpoint-url` at a Kythira cluster and work.

It follows the "Kinesis on Kythira" scoping doc (2026-11-02) and Clark's
answers to its open questions (2026-11-03):

- **Scope.** A generally useful service for every use case AWS Kinesis Data
  Streams supports today, including KCL consumers and enhanced fan-out.
- **Fidelity.** Full AWS behaviour compatibility is available as options
  the user selects (Requirement 3).
- **Retention.** The same retention options AWS offers, 24 hours up to 365
  days, so cold record segments are tiered to object storage.
- **Binary.** A separate `kinesis_gateway_node` by default.
- **KCL.** 2.x and 3.x, 2.x first. Lease coordination supports every
  option: the native Kythira lease layer (a Java `LeaseRefresher` against a
  Kythira lease API) first, for initial testing; then Kythira's own
  DynamoDB (`.kiro/specs/dynamodb-compatible-api/`), AWS DynamoDB and
  DynamoDB Local. KCL's DynamoDB is either Kythira's or AWS's.
- **Priority.** Built after `.kiro/specs/iam-compatible-auth/` (item Z)
  reaches task 13, whose authorizer hook this service uses.

**Scope**: streams and shards, the data plane, resharding, retention and
tiering, enhanced fan-out, encryption at rest, tags, resource policies,
on-demand and provisioned capacity, enhanced monitoring, the wire protocols
and SigV4, the native KCL lease layer, the core record segment store (also
used by DynamoDB's change log), and the tests.

**Out of scope**: Amazon Data Firehose, Kinesis Video Streams, Managed
Service for Apache Flink and Lambda event-source mappings. They are separate
AWS services that consume Kinesis Data Streams through the same API, so they
work against Kythira only where the user runs them pointed at it.

## Glossary

- **Pinned model**: the botocore `kinesis` `service-2.json` at a version
  pinned in the repo. The set of actions, shapes, limits and error codes
  this service must answer is derived from it.
- **Kinesis shard**: a user-visible shard of a stream, owning a range of the
  128-bit MD5 hash-key space. Distinct from a Kythira Raft group.
- **Routing key**: the multi-raft key of a Kinesis shard's records,
  `k/<stream id>/<shard ordinal>`. All records of one Kinesis shard live in
  one Raft group.
- **Record segment store**: a core component that appends records to local
  segment files outside the Raft snapshot, fetches missing segments from
  peers, trims by retention, and tiers cold segments to object storage.
- **Sequence number**: a decimal string, strictly increasing per Kinesis
  shard, that KCL compares as an integer.
- **Compatibility profile**: the behaviour switches in Requirement 3. `aws`
  is full AWS behaviour.
- **Lease API**: a small Kythira HTTP API of conditional writes and listing,
  used by the native KCL lease layer.

## Requirements

### Requirement 1: Opt-in module

**User Story:** As a maintainer, I want Kinesis built only when asked for
and kept separable from the core.

#### Acceptance Criteria

1. The service SHALL live under `services/kinesis/` and follow the module
   pattern of `services/README.md` (`redis-gateway-module`).
2. It SHALL be built only with a Kconfig option `KINESIS` (default off). It
   needs IAM's core headers (`aws_request_authorizer.hpp`,
   `sigv4_verifier.hpp`) but not the `IAM` module itself.
3. Module code SHALL include only core public headers;
   `scripts/check_module_boundaries.py` SHALL enforce this.
4. With the option off, no existing target, test or binary SHALL change.
5. A path-filtered `.github/workflows/kinesis-module.yml` SHALL build and
   test the module under Docker and rootless Podman.

### Requirement 2: Coverage of the AWS API

#### Acceptance Criteria

1. Every operation in the pinned model SHALL be implemented, or listed in
   `services/kinesis/doc/unsupported.md` with the reason and the exact error
   it returns.
2. A generated test SHALL fail if the pinned model gains an operation that
   is neither implemented nor listed.
3. Request and response shapes SHALL follow the pinned model.
4. Upgrading the pinned model SHALL be a reviewed change with the generated
   diff in the PR.

### Requirement 3: Selectable compatibility

#### Acceptance Criteria

1. The profile `aws` SHALL be the default and SHALL reproduce AWS behaviour
   for every switch below.
2. Each switch SHALL be settable per cluster in config and per stream
   through a Kythira tag (`kythira:compat:<switch>`), the stream value
   winning:
   - per-shard throughput limits (write 1 MB/s or 1000 records/s, read
     2 MB/s, 5 `GetRecords` calls/s) and their throttling;
   - record size, `PutRecords` batch and `GetRecords` page limits;
   - shard, stream and consumer count limits;
   - iterator lifetime (5 minutes);
   - sequence number and shard id formats (AWS's lengths and shapes, or
     Kythira's compact form).
3. A relaxed switch SHALL be visible through a Kythira extension field in
   `DescribeStreamSummary` and in metrics.
4. Error codes, messages and HTTP statuses SHALL always match AWS.
5. Throttling SHALL be on by default in every deployment, standalone and
   test clusters included, and SHALL be one of the AWS behaviour switches
   above that an operator can turn off per cluster or per stream.

### Requirement 4: Wire protocols

#### Acceptance Criteria

1. The service SHALL accept AWS JSON 1.1 and CBOR (`application/x-amz-cbor-1.1`)
   on `POST /` with `X-Amz-Target: Kinesis_20131202.<Action>`, through the
   core `aws_json_protocol` codec (built by the DynamoDB spec) extended with
   CBOR.
2. `SubscribeToShard` SHALL stream `application/vnd.amazon.eventstream`
   frames over HTTP/2, with AWS's event types and error events.
3. Errors SHALL use AWS's `__type` shapes in both encodings.
4. Request limits SHALL follow `http-server-request-limits`, set no lower
   than AWS's limits.

### Requirement 5: Authentication and authorization

#### Acceptance Criteria

1. Every request SHALL be authenticated with SigV4 through the core
   `sigv4_verifier`, including the HTTP/2 `SubscribeToShard` request.
2. Authorization SHALL go through `aws_request_authorizer` with a `kinesis`
   action table (actions, stream and consumer ARNs, condition keys).
3. With IAM configured, decisions SHALL be IAM's, including resource
   policies on streams and consumers. Without IAM, a static access-key table
   with read-only, read-write and admin roles SHALL be available.
4. Stream and consumer ARNs SHALL use the account id from IAM (or a
   configured one) and the configured partition.

### Requirement 6: Streams and shard metadata

#### Acceptance Criteria

1. `CreateStream`, `DeleteStream` (with `EnforceConsumerDeletion`),
   `DescribeStream`, `DescribeStreamSummary`, `ListStreams`, `ListShards`
   (all filter types) and `DescribeLimits` SHALL behave as AWS, with status
   transitions (`CREATING`, `ACTIVE`, `UPDATING`, `DELETING`) that SDK
   waiters observe.
2. Stream metadata (shards, hash ranges, lineage, starting and ending
   sequence numbers, mode, retention, encryption, tags, policies,
   consumers) SHALL be kept in a reserved catalog range
   (`\0meta/kinesis/`) and changed only through Raft entries.
3. Stream ids SHALL never be reused, so a recreated stream never sees an
   old stream's records.
4. A Kinesis shard's routing key SHALL never be cut by a Kythira split: the
   stream state machine vetoes split points inside a routing key.

### Requirement 7: Writing records

#### Acceptance Criteria

1. `PutRecord` and `PutRecords` SHALL hash the partition key (or use
   `ExplicitHashKey`) with MD5, pick the open shard owning the hash, and
   propose each shard's records as one Raft entry.
2. `SequenceNumberForOrdering` SHALL be honoured.
3. `PutRecords` SHALL return per-record results, with failed records
   (throttled or internal) and successful ones in one response, as AWS does.
4. An acknowledged record SHALL never be lost or duplicated by a leader
   change; a client retry after an ambiguous failure MAY write a second
   record, as with AWS.
5. Records written through KPL aggregation SHALL be stored unchanged.

### Requirement 8: Sequence numbers and timestamps

#### Acceptance Criteria

1. Sequence numbers SHALL be strictly increasing per Kinesis shard, derived
   from a per-shard counter kept in the stream state machine (not the raw
   Raft index), so they survive group splits, merges and moves.
2. In the `aws` profile they SHALL be decimal strings of AWS's length and
   SHALL compare as integers in record order.
3. Each record SHALL carry `ApproximateArrivalTimestamp` stamped by the
   leader from its wall clock, clamped never to go backwards within a shard.
4. The clock assumption (bounded skew between leaders; a leader change may
   repeat a timestamp but never reverse one) SHALL be documented.

### Requirement 9: Reading records

#### Acceptance Criteria

1. `GetShardIterator` SHALL support `AT_SEQUENCE_NUMBER`,
   `AFTER_SEQUENCE_NUMBER`, `TRIM_HORIZON`, `LATEST` and `AT_TIMESTAMP`.
2. Iterators SHALL be stateless HMAC-signed tokens naming stream, shard,
   position and expiry, valid on any node, expiring after the profile's
   lifetime with `ExpiredIteratorException`.
3. `GetRecords` SHALL return records, `NextShardIterator` (absent after a
   closed shard is exhausted), `MillisBehindLatest` and `ChildShards`, and
   MAY be served by any replica from committed segments.
4. Reading a trimmed position SHALL behave as AWS does (start at the trim
   horizon).

### Requirement 10: Resharding and capacity modes

#### Acceptance Criteria

1. `SplitShard`, `MergeShards` and `UpdateShardCount` (uniform scaling)
   SHALL close parents at an ending sequence number and open children with
   lineage, atomically across the Raft groups involved, using tier-1
   transactions when the shards live in different groups.
2. Closed shards SHALL stay readable until retention expires.
3. `UpdateStreamMode` SHALL switch between `PROVISIONED` and `ON_DEMAND`.
   In on-demand mode, a leader-side controller SHALL split hot shards and
   scale capacity from observed write rates, within AWS's on-demand limits.
4. Kythira's own group splits and moves SHALL be independent of Kinesis
   resharding and invisible to clients.

### Requirement 11: Retention, storage and tiering

#### Acceptance Criteria

1. Records SHALL be stored in the core record segment store: appended to
   local segment files in `apply()`, fsynced with the applied index, never
   in the Raft snapshot. A snapshot SHALL carry the stream state and segment
   index, and a replica missing a segment SHALL fetch it from a peer.
2. `IncreaseStreamRetentionPeriod` and `DecreaseStreamRetentionPeriod` SHALL
   support 24 hours to 8760 hours; trimming SHALL be a leader-proposed entry
   so every replica drops the same segments.
3. Segments older than a configurable age SHALL be uploaded to any Kythira
   object store with a SHA-256 recorded in the stream state, dropped
   locally, and fetched back transparently on read. Long-retention reads
   SHALL work with the local copy gone.
4. A disk guard SHALL refuse writes with a clear error before free space
   falls below a floor.
5. The store SHALL be a core component (`include/raft/record_segment_store.hpp`)
   usable by other modules; DynamoDB's change log is its second user.

### Requirement 12: Throughput limits

#### Acceptance Criteria

1. With enforcement on, the shard's leader SHALL apply token buckets per
   shard for writes and per shard and consumer for reads, returning
   `ProvisionedThroughputExceededException` and `LimitExceededException` as
   AWS does.
2. A measurement row SHALL show one Raft group sustaining the `aws`
   per-shard write limit for at least the configured number of hot shards
   per group, with batching and group commit.

### Requirement 13: Enhanced fan-out

#### Acceptance Criteria

1. `RegisterStreamConsumer`, `DeregisterStreamConsumer`,
   `DescribeStreamConsumer` and `ListStreamConsumers` SHALL manage consumers
   in stream metadata with AWS's limits and status transitions.
2. `SubscribeToShard` SHALL push records over HTTP/2 for up to 5 minutes per
   subscription, with per-consumer 2 MB/s per shard, continuation sequence
   numbers and child-shard events.
3. A subscription SHALL survive a leader change of the shard's group by
   resuming from the last sent sequence number, or end with the error event
   AWS uses so the client resubscribes.

### Requirement 14: Encryption, tags, policies and monitoring

#### Acceptance Criteria

1. `StartStreamEncryption` and `StopStreamEncryption` SHALL encrypt new
   segments at rest with AES-256-GCM data keys from a `kms_key_provider`
   concept: a local provider sealed like the CA key, and optional adapters
   for AWS KMS and other clouds' key services.
2. Tag operations (stream and resource forms) SHALL behave as AWS.
3. `PutResourcePolicy`, `GetResourcePolicy` and `DeleteResourcePolicy` SHALL
   store policies that IAM's evaluator applies.
4. `EnableEnhancedMonitoring` and `DisableEnhancedMonitoring` SHALL switch
   per-shard metrics on and off.
5. Account-level calls in the pinned model (for example
   `DescribeAccountSettings`, if present) SHALL be implemented over cluster
   config.

### Requirement 15: KCL lease coordination

#### Acceptance Criteria

1. A native lease layer SHALL be provided: a Lease API on
   `kinesis_gateway_node` (conditional put, update and delete on a version
   field; list by prefix; linearizable reads) and a Java library
   (`services/kinesis/java/kythira-kcl-leases`) implementing KCL 2.x's
   `LeaseRefresher`, `LeaseManagementFactory` and checkpointer interfaces
   against it.
2. The Java library SHALL be built and tested with Gradle in the module's CI
   against pinned KCL 2.x versions.
3. KCL 2.x and 3.x SHALL also run with leases in Kythira's DynamoDB, AWS
   DynamoDB and DynamoDB Local, each covered by a conformance job (AWS
   DynamoDB needs Clark's go, as real-cloud spend).
4. The documentation SHALL say which backend supports which KCL version
   (the native layer is 2.x only, because KCL 3.x hard-wires its DynamoDB
   DAOs).

### Requirement 16: Daemon and operations

#### Acceptance Criteria

1. The default binary SHALL be a separate `kinesis_gateway_node`, following
   `redis_gateway_node` for flags, TLS, forwarding and health.
2. Metrics SHALL report per stream and shard: incoming and outgoing bytes
   and records, throttles, iterator age, `GetRecords` latency, segment bytes
   local and tiered, and fan-out subscriptions.

### Requirement 17: Testing and conformance

#### Acceptance Criteria

1. A differential test SHALL run randomized operation mixes against Kythira
   and a reference implementation (an open-source Kinesis emulator, or
   recorded AWS responses where the emulator diverges), comparing every
   response and error type.
2. Conformance jobs SHALL run the AWS CLI, boto3 and aws-sdk-java-v2 and
   aws-sdk-cpp producers and consumers, KPL, KCL 2.x and 3.x with each lease
   backend, an enhanced fan-out consumer, and the Flink Kinesis connector.
3. Chaos scenarios SHALL kill leaders during `PutRecords`, resharding,
   segment tiering and fan-out, and check that no acknowledged record is
   lost, duplicated or reordered within a shard, under Docker and rootless
   Podman with service-name addressing.
4. A recording run against real AWS Kinesis, to settle behaviour no
   emulator reproduces, SHALL need Clark's go in his own words.
