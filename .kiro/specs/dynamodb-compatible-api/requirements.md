# Requirements Document

## Introduction

This spec adds a **DynamoDB-compatible service** to Kythira. Unmodified AWS
SDKs, the AWS CLI, KCL, the DynamoDB lock client, the DynamoDB Streams
Kinesis Adapter, Terraform and other DynamoDB tools can point
`--endpoint-url` at a Kythira cluster and work.

It follows the "DynamoDB-compatible API on Kythira" scoping doc
(2026-11-02) and Clark's answers to its open questions (2026-11-03):

- **Scope.** A generally useful service for every use case AWS DynamoDB
  supports today, not only KCL leases.
- **Fidelity.** Full AWS behaviour compatibility is available as options
  the user selects (Requirement 3).
- **KCL.** KCL 2.x and 3.x must both work, 2.x first. Every lease backend is
  supported: this service, AWS DynamoDB, DynamoDB Local, and the native
  Kythira lease layer from `.kiro/specs/kinesis-compatible-streams/`.
- **Table size.** Left to this implementation. Items live in the
  disk-backed engine from `.kiro/specs/persistent-state-machine/` (item X),
  with file snapshots from `.kiro/specs/file-install-snapshot/` (item Y), so
  a table is bounded by disk, not RAM.
- **Priority.** Built after `.kiro/specs/iam-compatible-auth/` (item Z)
  reaches task 13, whose authorizer hook this service uses.

**Scope**: tables and items, the expression language, Query and Scan, batch
operations, secondary indexes, transactions, PartiQL, DynamoDB Streams, TTL,
backups and point-in-time recovery, export to and import from object
storage, Kinesis streaming destinations, global tables between Kythira
clusters, tags, resource policies, contributor insights, capacity modes, the
wire protocol and SigV4 authentication, and the tests.

**Out of scope**: DAX (a separate AWS service with its own protocol),
Application Auto Scaling (a separate service; provisioned capacity values
are stored and enforced, not auto-scaled by AWS's API), and global tables
that include real AWS regions.

## Glossary

- **Pinned model**: the botocore `dynamodb` and `dynamodbstreams`
  `service-2.json` files at a version pinned in the repo. The set of actions,
  shapes, limits and error codes this service must answer is derived from it.
- **Compatibility profile**: the set of behaviour switches in Requirement 3.
  `aws` is full AWS behaviour.
- **Item**: a map of attribute names to typed values, identified by its
  primary key (partition key, optional sort key).
- **Item collection**: all items, and their LSI entries, with one partition
  key value.
- **Change log**: the per-group append-only log of item mutations that GSIs,
  Streams and Kinesis streaming destinations consume. Its storage is the
  record segment store from `kinesis-compatible-streams`.
- **Catalog**: the replicated table descriptors (key schema, indexes,
  stream spec, TTL attribute, billing mode, status, tags, policies).
- **Decimal**: DynamoDB's `N` type, 38 significant digits, exponent range
  -130 to +125, exact arithmetic.

## Requirements

### Requirement 1: Opt-in module

**User Story:** As a maintainer, I want DynamoDB built only when asked for
and kept separable from the core.

#### Acceptance Criteria

1. The service SHALL live under `services/dynamodb/` (headers, sources,
   tests, container scenarios, `Kconfig`, `CMakeLists.txt`) and follow the
   module pattern of `services/README.md` (`redis-gateway-module`).
2. It SHALL be built only with a Kconfig option `DYNAMODB` (default off),
   which requires a persistent engine (`KYTHIRA_ENGINE_ROCKSDB` or the
   in-memory engine). It needs IAM's core headers
   (`aws_request_authorizer.hpp`, `sigv4_verifier.hpp`) but not the `IAM`
   module itself.
3. Module code SHALL include only core public headers;
   `scripts/check_module_boundaries.py` SHALL enforce this.
4. With the option off, no existing target, test or binary SHALL change.
5. A path-filtered `.github/workflows/dynamodb-module.yml` SHALL build and
   test the module under Docker and rootless Podman.

### Requirement 2: Coverage of the AWS API

**User Story:** As a user, I want every DynamoDB action I use today to work.

#### Acceptance Criteria

1. Every operation in the pinned model SHALL be implemented, or listed in
   `services/dynamodb/doc/unsupported.md` with the reason and the exact error
   it returns. The only entries allowed at spec close are the out-of-scope
   items above.
2. A generated test SHALL fail if the pinned model gains an operation that
   is neither implemented nor listed.
3. Request and response shapes SHALL follow the pinned model, including
   optional members, and unknown request members SHALL be rejected with
   `ValidationException` as AWS does.
4. Upgrading the pinned model SHALL be a reviewed change with the generated
   diff of new operations and members in the PR.

### Requirement 3: Selectable compatibility

**User Story:** As an operator, I want AWS's exact behaviour by default and
the freedom to relax it where my workload needs it.

#### Acceptance Criteria

1. The profile `aws` SHALL be the default and SHALL reproduce AWS behaviour
   for every switch below.
2. Each switch SHALL be settable per cluster in config and per table through
   a Kythira tag (`kythira:compat:<switch>`), with the per-table value
   winning:
   - item, key, page, batch and transaction size limits;
   - provisioned-capacity enforcement and on-demand throttling;
   - `ConsumedCapacity` and `ItemCollectionMetrics` reporting;
   - GSI consistency (asynchronous as AWS, or synchronous);
   - table and index count limits;
   - TTL deletion delay (AWS's "within a few days" window, or prompt).
3. A relaxed switch SHALL be visible in `DescribeTable` under a Kythira
   extension field that AWS SDKs ignore, and in metrics.
4. Error codes, messages and HTTP statuses SHALL always match AWS; no switch
   changes them.
5. Throttling SHALL be on by default in every deployment, standalone and
   test clusters included, and SHALL be one of the AWS behaviour switches
   above that an operator can turn off per cluster or per table.

### Requirement 4: Wire protocol

#### Acceptance Criteria

1. The service SHALL speak AWS JSON 1.0 on `POST /` with
   `X-Amz-Target: DynamoDB_20120810.<Action>` and
   `DynamoDBStreams_20120810.<Action>`, through a core
   `include/raft/aws_json_protocol.hpp` codec shared with the Kinesis module.
2. Errors SHALL be JSON with `__type` and `message` as AWS sends them, and
   every response SHALL carry `x-amz-crc32` and `x-amzn-RequestId`.
3. Request limits SHALL follow `http-server-request-limits`, set no lower
   than AWS's limits.
4. `DescribeEndpoints` SHALL return the node's own advertised endpoint.

### Requirement 5: Authentication and authorization

#### Acceptance Criteria

1. Every request SHALL be authenticated with SigV4 through the core
   `sigv4_verifier` (`iam-compatible-auth` task 6), in header and presigned
   forms.
2. Authorization SHALL go through the core `aws_request_authorizer` concept
   with a `dynamodb` action table (actions, resource ARNs, condition keys
   including `dynamodb:LeadingKeys` and `dynamodb:Attributes`) registered
   from this module's composition root.
3. With IAM configured, decisions SHALL be IAM's, including resource
   policies (`PutResourcePolicy`) evaluated as AWS does. Without IAM, a
   static access-key table mapped to read-only, read-write and admin roles
   SHALL be available for standalone and test clusters.
4. ARNs SHALL use the account id from IAM (or a configured one) and the
   configured partition.

### Requirement 6: Data types and item encoding

#### Acceptance Criteria

1. All attribute types (S, N, B, BOOL, NULL, M, L, SS, NS, BS) SHALL be
   supported with AWS's validation rules (empty strings and binaries allowed
   in non-key attributes, sets non-empty and duplicate-free, nesting depth
   32).
2. `N` SHALL be an exact decimal with 38 significant digits and AWS's range,
   rounding and overflow errors; arithmetic and comparison SHALL give
   identical results on x86-64 and arm64.
3. Key attributes SHALL use an order-preserving byte encoding: S and B in
   unsigned byte order, N in numeric order.
4. Item size SHALL be computed with AWS's published formula and checked
   against the 400 KB limit (Requirement 3).

### Requirement 7: Tables and the control plane

#### Acceptance Criteria

1. `CreateTable`, `DescribeTable`, `ListTables`, `UpdateTable`,
   `DeleteTable`, `DescribeLimits`, tags, deletion protection, table classes
   and warm throughput SHALL behave as AWS, with status transitions
   (`CREATING`, `UPDATING`, `DELETING`, `ACTIVE`) that SDK waiters observe.
2. The catalog SHALL be linearizable: kept in a reserved catalog key range
   (`\0meta/dynamodb/`) and changed only through Raft entries.
3. `DeleteTable` SHALL tombstone the catalog entry and then delete the
   table's key ranges in the background, bounded per Raft entry.
4. Table ids SHALL never be reused, so a recreated table never sees an old
   table's data, indexes or stream records.

### Requirement 8: Items, conditions and expressions

#### Acceptance Criteria

1. `GetItem`, `PutItem`, `UpdateItem` and `DeleteItem` SHALL support every
   parameter in the pinned model, including `ReturnValues` and
   `ReturnValuesOnConditionCheckFailure`.
2. Conditions SHALL be evaluated in `apply()` against the committed item,
   never at the gateway.
3. The expression language SHALL be complete: condition, update, key
   condition, filter and projection expressions, attribute name and value
   substitution, nested paths, every function and operator AWS documents,
   and AWS's validation errors for each misuse.
4. Expressions SHALL be parsed at the gateway into a versioned, serialized
   tree carried in the Raft entry, so every replica evaluates the same tree.
5. The legacy parameters (`Expected`, `AttributeUpdates`, `KeyConditions`,
   `QueryFilter`, `ScanFilter`, `AttributesToGet`, `ConditionalOperator`)
   SHALL translate into the same tree. KCL 2.x depends on them.
6. `GetItem` without `ConsistentRead` MAY be served by a follower;
   `ConsistentRead=true` SHALL use the linearizable read path.

### Requirement 9: Query and Scan

#### Acceptance Criteria

1. `Query` SHALL read one partition as a range, forward or reverse, with
   `Limit`, the 1 MB page, `ExclusiveStartKey`/`LastEvaluatedKey`, then
   filter and projection after the read, and `Select` including `COUNT`.
2. `Scan` SHALL walk a table or index shard by shard with a cursor that
   survives splits, merges and leader changes; `Segment`/`TotalSegments`
   SHALL divide the key space into balanced slices.
3. Both SHALL support `ConsistentRead` on base tables and LSIs, and SHALL
   reject it on GSIs as AWS does.

### Requirement 10: Batch operations

#### Acceptance Criteria

1. `BatchGetItem` (100 keys) and `BatchWriteItem` (25 requests) SHALL group
   by shard, with no atomicity across items, and return
   `UnprocessedKeys`/`UnprocessedItems` on partial failure or throttling.
2. Duplicate keys in one batch SHALL be rejected as AWS does.

### Requirement 11: Secondary indexes

#### Acceptance Criteria

1. LSIs SHALL be stored beside their item collection and updated in the same
   `apply()`. A group holding an LSI table SHALL veto splits inside an item
   collection, and an item collection over 10 GB SHALL fail writes with
   `ItemCollectionSizeLimitExceededException`.
2. GSIs SHALL be separate key spaces maintained from the change log by a
   leader-side propagator that proposes idempotent, versioned index writes,
   so replay or reordering cannot regress an entry.
3. GSI propagation lag SHALL be bounded under normal load, exported as a
   metric per index, and visible to tests.
4. `UpdateTable` adding a GSI SHALL backfill in the background with
   `IndexStatus=CREATING` and `Backfilling=true`, then become `ACTIVE`.
   Removing one SHALL delete its range in the background.
5. Projections (`KEYS_ONLY`, `INCLUDE`, `ALL`) SHALL control what is copied.
6. The synchronous switch (Requirement 3) SHALL maintain GSIs with tier-1
   transactions instead, for tables that ask for read-your-writes.

### Requirement 12: Transactions

#### Acceptance Criteria

1. `TransactWriteItems` (100 actions, 4 MB) SHALL run on the tier-1
   transaction coordinator. Each participant SHALL evaluate its action's
   condition against the committed item while installing the intent, through
   a new optional `validate` hook in the transaction wrapper.
2. `ConditionCheck` SHALL lock and validate an item without writing it.
3. Failures SHALL return `TransactionCanceledException` with per-action
   `CancellationReasons`; a plain write meeting an intent SHALL return
   `TransactionConflictException`.
4. `ClientRequestToken` SHALL make a retry within 10 minutes return the
   first attempt's outcome, and a changed request SHALL return
   `IdempotentParameterMismatchException`.
5. `TransactGetItems` SHALL be serializable with the items it reads:
   lock-based first, and a tier-2 snapshot read where the tables are
   multi-versioned.

### Requirement 13: PartiQL

#### Acceptance Criteria

1. `ExecuteStatement`, `BatchExecuteStatement` and `ExecuteTransaction`
   SHALL support the PartiQL subset AWS documents for DynamoDB (`SELECT`,
   `INSERT`, `UPDATE`, `DELETE`, `EXISTS`, functions, `WHERE` on keys and
   attributes, pagination by `NextToken`).
2. Statements SHALL compile to the same operations and expression trees as
   the JSON API, so behaviour and authorization are identical.

### Requirement 14: Change log and DynamoDB Streams

#### Acceptance Criteria

1. Every item mutation SHALL append one change record (`INSERT`, `MODIFY`,
   `REMOVE`, with the images the stream view type asks for, and the TTL
   service identity for TTL deletes) to the group's change log in the same
   apply.
2. `ListStreams`, `DescribeStream`, `GetShardIterator` and `GetRecords` SHALL
   serve stream shards with AWS's lineage rules: a Kythira split closes the
   parent stream shard and opens one per child, each naming its parent.
3. A group holding a stream-enabled table SHALL veto merges (a stream shard
   has one parent).
4. Records SHALL stay readable for 24 hours after they are written,
   including from closed parent shards after a split.
5. The DynamoDB Streams Kinesis Adapter with KCL SHALL consume a stream with
   no record lost or duplicated across splits.

### Requirement 15: TTL

#### Acceptance Criteria

1. `UpdateTimeToLive` and `DescribeTimeToLive` SHALL behave as AWS.
2. Expired items SHALL be deleted by leader-proposed, bounded batches that
   delete only if the attribute still holds the observed value.
3. Expired items SHALL stay readable until deleted, as AWS documents; the
   deletion delay is a switch (Requirement 3).

### Requirement 16: Kinesis streaming destinations

#### Acceptance Criteria

1. `EnableKinesisStreamingDestination`, `DisableKinesisStreamingDestination`,
   `DescribeKinesisStreamingDestination` and
   `UpdateKinesisStreamingDestination` SHALL publish change records to a
   Kythira Kinesis stream (`kinesis-compatible-streams`) or to AWS Kinesis.
2. Delivery SHALL be at least once, in order per item, with the record
   format AWS uses.

### Requirement 17: Backups, point-in-time recovery, export and import

#### Acceptance Criteria

1. `CreateBackup`, `DescribeBackup`, `ListBackups`, `DeleteBackup` and
   `RestoreTableFromBackup` SHALL take consistent table backups from engine
   checkpoints and store them in any Kythira object store.
2. `UpdateContinuousBackups` and `RestoreTableToPointInTime` SHALL provide
   point-in-time recovery over a configurable window (AWS: 35 days) from
   backups plus the change log.
3. `ExportTableToPointInTime` (full and incremental) and
   `ImportTable` SHALL read and write the AWS export formats
   (DynamoDB JSON, Ion, CSV for import) on any Kythira object store, with
   `s3://` URIs mapped to configured stores.

### Requirement 18: Global tables

#### Acceptance Criteria

1. `UpdateTable` with replica updates (version 2019.11.21) SHALL replicate a
   table to tables of the same name in other Kythira clusters, configured as
   peer regions.
2. Replication SHALL be asynchronous from the change log, with AWS's
   last-writer-wins conflict rule, and `ReplicaStatus` reported as AWS does.
3. Multi-region strong consistency SHALL be available as an option where
   the peers allow a cross-cluster quorum, or listed as unsupported with the
   reason (Requirement 2.1).

### Requirement 19: Capacity, throttling and insights

#### Acceptance Criteria

1. `PAY_PER_REQUEST` and `PROVISIONED` modes SHALL both work; consumed
   capacity SHALL be computed with AWS's formulas.
2. With enforcement on, leader-side token buckets per table, index and
   partition SHALL return `ProvisionedThroughputExceededException` and
   `ThrottlingException` as AWS does.
3. Hot partitions SHALL be relieved by Kythira's load-based splits, within
   the LSI veto.
4. Contributor Insights (`UpdateContributorInsights`,
   `DescribeContributorInsights`, `ListContributorInsights`) SHALL report the
   most-accessed and most-throttled keys from sampled counters.

### Requirement 20: Storage and operations

#### Acceptance Criteria

1. Items, LSI entries, GSI entries, transaction state and the catalog SHALL
   live in the node's persistent engine under the group's key range; groups
   SHALL snapshot by file manifest and split or merge without copying data.
2. A disk guard SHALL refuse writes with a clear error before the data
   directory's free space falls below a floor.
3. The default binary SHALL be a separate `dynamodb_gateway_node`, following
   `redis_gateway_node` for flags, TLS, forwarding and health.
4. Metrics SHALL report per table: requests and errors by operation,
   latency, consumed capacity, throttles, GSI lag, stream lag, TTL deletes,
   item count and size estimates.

### Requirement 21: Testing and conformance

#### Acceptance Criteria

1. A differential test SHALL run randomized operation mixes against Kythira
   and DynamoDB Local and compare every response, error type included.
2. The decimal type SHALL pass a vector suite covering AWS's documented
   rounding, range and comparison cases.
3. The expression parser SHALL be fuzzed, and every rejection SHALL match
   DynamoDB Local's error.
4. Conformance jobs SHALL run the AWS CLI, boto3 and aws-sdk-java-v2
   examples, the DynamoDB lock client, KCL 2.x and 3.x with leases on
   Kythira, the DynamoDB Streams Kinesis Adapter, and Terraform's
   `aws_dynamodb_table`.
5. Chaos scenarios SHALL kill leaders mid-transaction, mid-GSI-propagation
   and mid-split, and check that no acknowledged write, index entry or
   stream record is lost or duplicated, under Docker and rootless Podman with
   service-name addressing.
6. A recording run against real AWS DynamoDB, to settle behaviour DynamoDB
   Local does not reproduce, SHALL need Clark's go in his own words.
