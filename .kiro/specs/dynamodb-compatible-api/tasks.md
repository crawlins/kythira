# Implementation Plan

Depends on:
- `.kiro/specs/iam-compatible-auth/` task 6 (SigV4) for task 2, and task 13
  (the authorizer hook) for task 4. Clark placed this spec after Z task 13.
- `.kiro/specs/persistent-state-machine/` tasks 1-4 (concepts, in-memory
  engine, node integration, copy-free split/merge) for task 6, its task 5
  (RocksDB) for the RocksDB CI leg, and `.kiro/specs/file-install-snapshot/`
  for follower catch-up.
- `.kiro/specs/redis-gateway-module/` tasks 2-3 (core INTERFACE targets,
  boundary check, `services/README.md`) for task 1.
- `.kiro/specs/kinesis-compatible-streams/` task 3 (the core
  `record_segment_store`) for task 15. Task 2 here builds the shared JSON
  codec that Kinesis task 2 extends with CBOR.

Each task is one PR unless noted. Recommended interleave with Kinesis:
DynamoDB 1-2, Kinesis 1-3, then both in parallel.

### Phase 0: evidence

- [x] 1. Module skeleton and evidence tables
  - `DYNAMODB` Kconfig option (default off), `services/dynamodb/` CMake
    targets, boundary allow list, path-filtered `dynamodb-module.yml`
  - Pinned botocore `dynamodb` and `dynamodbstreams` models; generated
    operation list and the coverage test (Requirement 2.2)
  - A recording stub, and checked-in wire evidence for the AWS CLI, boto3,
    aws-sdk-java-v2, KCL 2.x and 3.x, the lock client and the Streams adapter
  - Off-by-default build proved unchanged
  - _Requirements: 1.1-1.5, 2.1, 2.2, 2.4_
  - Done: botocore 1.43.126 models (62 DynamoDB and 4 Streams operations)
    with checked-in shape tables and `--check` ctests; `doc/unsupported.md`
    lists every operation not built yet with its task, and the coverage
    test holds the implemented and unsupported lists to the model. No
    boundary allow-list entries were needed. Evidence: all seven clients
    recorded against `tools/evidence/recording_stub.py`, and
    `dynamodb_wire_evidence_test` validates every DynamoDB and Streams
    request against the strict codec. `dynamodb-module.yml` re-records under
    Docker and rootless Podman and runs the test on the fresh recording; the
    module-off build is checked as `iam-module.yml` checks its module. Five
    operations added to the model after the spec was written (the bulk-put
    family and `SearchVectors`) are listed as unsupported pending a scope
    decision.

### Phase 1: protocol and auth

- [x] 2. AWS JSON protocol codec (core)
  - `include/raft/aws_json_protocol.hpp`: JSON 1.0 and 1.1 framing,
    `X-Amz-Target` dispatch, shape (de)serialization generated from the
    pinned model, `__type` errors, `x-amz-crc32`, request ids
  - Request limits per `http-server-request-limits`
  - _Requirements: 4.1-4.3_
  - Done: the codec, `scripts/gen_aws_json_shapes.py` (botocore model to
    `constexpr` shape tables; a module registers its `--check` ctest so a
    model upgrade carries its regenerated header), and
    `tests/aws_json_protocol_test.cpp` on a fixture model. Built before
    task 1 so Kinesis task 2 could start on it; the DynamoDB and Kinesis
    models were generated and compiled against it locally. Validation runs
    AWS's two passes (type faults as `SerializationException`, then every
    constraint in one `ValidationException`). Error names, the message
    member's spelling and the statuses for protocol-level faults are kept
    in one `error_style` per service family so task 1's recordings and the
    task 8 differential job can correct them in one place.

- [x] 3. Decimal and item codec
  - `dynamo_decimal` with exact arithmetic, AWS range and rounding, the
    order-preserving key encoding; vector suite on x86-64 and arm64
  - Item codec, attribute validation, size formula
  - _Requirements: 6.1-6.4, 21.2_
  - Done: `decimal.hpp` and `item_codec.hpp`. The mantissa is a 38-digit
    array rather than two 64-bit words: arithmetic runs on a digit grid
    wide enough for the whole range, so cancellation across a 39th digit
    is exact, and nothing depends on `__int128`. Results that need a 39th
    digit or leave the range are refused, never rounded. Numbers format in
    plain notation. Messages are AWS's where published; task 8's
    differential pins the rest. The size formula charges a byte per `L`/`M`
    element, which AWS's prose omits but the service charges. The wire
    evidence test also runs every recorded item and key through the codec.

- [x] 4. Front end, SigV4 and authorization
  - `dynamo_front_end` on the shared Beast host, forwarding to shard
    leaders, `DescribeEndpoints`
  - SigV4 through the core verifier; `dynamodb` action table with condition
    keys; IAM authorizer path and the static-key fallback
  - Compatibility profile resolution (cluster config plus table tags)
  - _Requirements: 3.1-3.4, 4.4, 5.1-5.4_
  - Done: `front_end.hpp` (transport-free; SigV4 before the body,
    unsupported operations before validation, the same-named action on the
    resource the input names, `call_context::authorize` for transactions,
    PartiQL and item-level keys), `DescribeEndpoints`, the `dynamodb` action
    table generated from AWS's machine-readable Service Authorization
    Reference (`tools/gen_dynamodb_actions.py`, `--check` ctest), and
    `compat_profile.hpp`. The front end takes any `aws_request_authorizer`:
    IAM's, or core's new `static_key_authorizer` (shared with Kinesis, roles
    derived from the reference's annotations). Mounting it on the shared
    Beast host is the gateway binary's job, which arrives with task 7 when
    there are tables to serve; forwarding to shard leaders is the item
    handlers' (task 7).

### Phase 2: tables and items (KCL 2.x leases work after task 7)

- [x] 5. Expression language
  - Parser for condition, update, key condition, filter and projection
    expressions; substitution; nested paths; every function and operator
  - Legacy parameter translation into the same tree
  - Versioned tree serialization; fuzzing; error parity with DynamoDB Local
  - _Requirements: 8.3-8.5, 21.3_
  - Done: `expr*.hpp` under `services/dynamodb/include/kythira/dynamodb/`;
    2985 cases recorded against DynamoDB Local 3.3.2
    (`tools/expr-corpus/record.sh`) all replay identically, apart from 14
    marked divergences where Kythira keeps AWS's number messages or
    DynamoDB Local fails internally. With three or more clashing paths,
    DynamoDB Local's overlap message may name an unrelated "path two";
    Kythira names the clashing pair, and the test compares only the kind
    of clash there.

- [x] 6. Catalog and item state machine
  - Catalog range, descriptors, status transitions, table ids, background
    jobs with progress entries
  - `dynamo_item_state_machine` on `kv_engine`: atomic apply with the
    applied index, item versions, condition evaluation in `apply()`
  - Disk guard
  - _Requirements: 7.2-7.4, 8.2, 20.1, 20.2_
  - Done: `key_layout.hpp`, `item_state_machine.hpp`, `catalog.hpp` and
    `disk_guard.hpp`, tested on `memory_kv_engine`. Item versions live in
    the meta column family under the base item's key rather than a `v/`
    prefix, so a group's range holds an item and its version together.
    Item groups split only at partition boundaries. A deleted table is
    removed by bounded `drop_table_step` entries, each recorded in the
    catalog as a `job_progress` entry. The disk guard runs at the gateway
    before a write is proposed, because free space differs between
    replicas and so cannot be checked in `apply()`.

- [ ] 7. Control plane and single-item API
  - `CreateTable`, `DescribeTable`, `ListTables`, `UpdateTable` (basic),
    `DeleteTable`, `DescribeLimits`, tags, deletion protection, table class,
    warm throughput
  - `GetItem`, `PutItem`, `UpdateItem`, `DeleteItem` with every parameter;
    consistent and eventual reads
  - `dynamodb_gateway_node` daemon
  - KCL 2.x sample consumer with leases on Kythira (Docker and rootless
    Podman), against a Kythira or test Kinesis stream
  - _Requirements: 7.1, 8.1, 8.6, 20.3_
  - Progress (November 10, 2026):
    - Done: the control plane and single-item handlers (`table_api.hpp`,
      `item_api.hpp`) over the `dynamo_backend` concept, with
      `local_backend` applying commands in-process (`backend.hpp`);
      `dynamodb_gateway_node` on core's Beast HTTPS host with static keys,
      one node on `memory_kv_engine` (`gateway.hpp`). boto3 and the AWS CLI
      ran create, waiters, put, update, get, conditions, tags and delete
      against it.
    - Left: the Raft-hosted backend on the RocksDB engine (after
      persistent-state-machine task 5), warm throughput, and the KCL
      sample.

- [ ] 8. Query, Scan and batch operations
  - Range reads with every `Query` option; cross-shard `Scan` cursors;
    parallel segments; `BatchGetItem`, `BatchWriteItem` with unprocessed
    items
  - Differential test harness against DynamoDB Local, first op mix
  - _Requirements: 9.1-9.3, 10.1, 10.2, 21.1_

### Phase 3: indexes and transactions (KCL 3.x works after task 11)

- [ ] 9. Local secondary indexes
  - LSI entries in the same apply, projections, split veto inside item
    collections, the 10 GB limit and `ItemCollectionMetrics`
  - _Requirements: 11.1, 11.5_

- [ ] 10. Transactions
  - `validate` hook in `transactional_state_machine` (multi-raft-transactions
    change, reviewed with that spec's owner)
  - `TransactWriteItems`, `ConditionCheck`, cancellation reasons, token
    idempotency; lock-based `TransactGetItems`
  - Chaos: leader killed mid-transaction
  - _Requirements: 12.1-12.5_

- [ ] 11. Global secondary indexes
  - GSI key spaces, leader-side propagator from the change log (needs task
    15's change log; until then the propagator reads a temporary in-engine
    queue with the same interface), versioned idempotent writes, lag metric
  - Backfill on `UpdateTable`, removal, projections, the synchronous switch
  - KCL 3.x lease, coordinator and worker-metrics tables on Kythira
  - _Requirements: 11.2-11.6_

- [ ] 12. Capacity, throttling and consumed capacity
  - Consumed-capacity formulas; provisioned and on-demand modes; leader-side
    token buckets per table, index and partition; throttling errors
  - Throttling on by default; the capacity switch turns it off per cluster
    or per table, with a test for each setting
  - _Requirements: 3.5, 19.1-19.3_

### Phase 4: streams, TTL and PartiQL

- [ ] 13. TTL
  - `UpdateTimeToLive`, `DescribeTimeToLive`, TTL index, leader-proposed
    conditional delete batches, delay switch
  - _Requirements: 15.1-15.3_

- [ ] 14. PartiQL
  - Parser and compiler to the JSON API's requests; `ExecuteStatement`,
    `BatchExecuteStatement`, `ExecuteTransaction`
  - _Requirements: 13.1, 13.2_

- [ ] 15. Change log
  - Change records appended in `apply()` to the group's
    `record_segment_store`; the GSI propagator switched onto it
  - _Requirements: 14.1_

- [ ] 16. DynamoDB Streams
  - Stream shards over the change log, lineage across splits, merge veto,
    24-hour retention per stream shard, iterators, the four actions
  - Streams Kinesis Adapter conformance with KCL
  - _Requirements: 14.2-14.5_

- [ ] 17. Kinesis streaming destinations
  - Publisher to Kythira Kinesis or AWS Kinesis; the four actions
  - _Requirements: 16.1, 16.2_

### Phase 5: data protection

- [ ] 18. Backups and restore
  - Consistent multi-range checkpoints, manifests on the object stores,
    `CreateBackup`, `ListBackups`, `DescribeBackup`, `DeleteBackup`,
    `RestoreTableFromBackup`
  - _Requirements: 17.1_

- [ ] 19. Point-in-time recovery
  - Change-log archive, `UpdateContinuousBackups`,
    `DescribeContinuousBackups`, `RestoreTableToPointInTime`
  - _Requirements: 17.2_

- [ ] 20. Export and import
  - Full and incremental `ExportTableToPointInTime`, `ImportTable`, the
    describe and list calls; AWS file layouts
  - _Requirements: 17.3_

### Phase 6: the rest of the surface

- [ ] 21. Resource policies and contributor insights
  - `PutResourcePolicy`, `GetResourcePolicy`, `DeleteResourcePolicy` through
    IAM's evaluator; contributor insights counters and calls
  - _Requirements: 5.3, 19.4_

- [ ] 22. Global tables, last-writer-wins
  - Peer cluster config, replicator and receiver, replica updates in
    `UpdateTable`, replica status, conflict tests with clock skew
  - _Requirements: 18.1, 18.2_

- [ ] 23. Coverage close-out
  - Every remaining operation in the pinned model implemented or listed in
    `unsupported.md`; coverage test green
  - _Requirements: 2.1-2.3_

### Phase 7: conformance and docs

- [ ] 24. Conformance and chaos
  - Conformance jobs: AWS CLI, boto3, aws-sdk-java-v2, lock client, KCL 2.x
    and 3.x, Streams adapter, Terraform `aws_dynamodb_table`
  - Chaos: leader kills mid-GSI-propagation and mid-split with streams on;
    no lost or duplicated writes, index entries or stream records (Docker
    and rootless Podman)
  - Optional recording run against real AWS DynamoDB (**Clark's go**)
  - _Requirements: 20.4, 21.1, 21.4-21.6_

- [ ] 25. Documentation and decisions
  - `doc/dynamodb.md`: model, profile switches, limits, operations,
    unsupported list, deployment
  - Decide multi-region strong consistency (Requirement 18.3) with Clark
  - _Requirements: 18.3_
