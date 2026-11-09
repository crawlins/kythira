# Implementation Plan

Depends on:
- `.kiro/specs/iam-compatible-auth/` task 6 (SigV4) for task 4, and task 13
  (the authorizer hook) for task 4's IAM path. Clark placed this spec after
  Z task 13.
- `.kiro/specs/redis-gateway-module/` tasks 2-3 for task 1.
- `.kiro/specs/dynamodb-compatible-api/` task 2 (the core JSON codec) for
  task 2 here, which adds CBOR. Task 3 here builds the core
  `record_segment_store` that DynamoDB task 15 uses.
- `.kiro/specs/dynamodb-compatible-api/` task 7 for KCL 2.x on Kythira
  DynamoDB, and task 11 for KCL 3.x (task 16 here).
- `.kiro/specs/multi-raft-transactions/` tier 1 (done) for resharding
  across groups.

Each task is one PR unless noted. Recommended interleave with DynamoDB:
DynamoDB 1-2, Kinesis 1-3, then both in parallel. KCL 2.x with the native
lease layer (task 9) is the first end-to-end consumer milestone, as Clark
asked.

### Phase 0: evidence

- [ ] 1. Module skeleton and evidence tables
  - `KINESIS` Kconfig option (default off), `services/kinesis/` CMake
    targets, boundary allow list, path-filtered `kinesis-module.yml`
  - Pinned botocore `kinesis` model, generated operation list and the
    coverage test
  - Recording stub and checked-in wire evidence for the AWS CLI, boto3,
    aws-sdk-java-v2, aws-sdk-cpp, KPL, KCL 2.x and 3.x, the Flink connector
  - Off-by-default build proved unchanged
  - _Requirements: 1.1-1.5, 2.1, 2.2, 2.4_
  - Part done (November 9, 2026): the module skeleton (`KINESIS`, CMake,
    empty boundary allow list, `kinesis_gateway_node` placeholder),
    `configs/ci_no_kinesis_defconfig` and `kinesis-module.yml` (boundary,
    module-on tests, module-off assertion), the pinned model (botocore
    1.43.100), the generated operation table with its `--check` test, and
    the coverage test with every one of the 44 operations listed in
    `services/kinesis/doc/unsupported.md`. The recording stub and wire
    evidence for the AWS CLI v2, boto3, aws-sdk-java-v2 and KPL are in
    `services/kinesis/tests/evidence/` (README there: CBOR from the Java
    SDK, KPL's `Expect: 100-continue`, STS `GetCallerIdentity` and
    `StreamARN`). Moved to later tasks, with reasons in that README:
    aws-sdk-cpp (task 7), KCL 2.x and 3.x (tasks 9 and 16, they need a
    lease table first), the Flink connector (task 17) and `SubscribeToShard`
    (task 13), plus the workflow's container job, which joins with task 7
    when there is a daemon to run. The model's five Channel
    operations (delivery to S3 and S3 Tables) are not in the spec and are
    listed as unplanned pending the owner's call.

### Phase 1: core pieces

- [ ] 2. CBOR and event-stream framing (core)
  - CBOR request and response encoding in `aws_json_protocol.hpp`
    (JSON 1.1 framing is already there from DynamoDB task 2)
  - `aws_eventstream.hpp`: prelude, headers, CRCs; AWS's published vectors
  - _Requirements: 4.1-4.3_

- [x] 3. Record segment store (core)
  - `record_segment_store.hpp`: append with durable barrier, roll, read,
    peer fetch with SHA-256, seal, trim, restart truncation of unacknowledged
    tails
  - Conformance suite reusable by DynamoDB's change log
  - Done: `include/raft/record_segment_store.hpp`,
    `tests/record_segment_store_conformance.hpp` (17 cases, macro
    `KYTHIRA_RECORD_SEGMENT_CONFORMANCE`), `tests/record_segment_store_test.cpp`
    runs it for Kinesis-shaped and change-log-shaped streams. Rolls depend
    only on record sizes and timestamps, so replicas write byte-identical
    segments and an active segment's prefix digest is fetchable and
    verifiable after a snapshot restore. Tiering (task 10) and encryption
    (task 14) build on it.
  - _Requirements: 11.1, 11.5_

### Phase 2: core data plane

- [ ] 4. Front end, SigV4, authorization and profile
  - `kinesis_front_end` on the shared Beast host, forwarding, replica reads
  - SigV4; `kinesis` action table; IAM path and static-key fallback
  - Compatibility profile resolution
  - _Requirements: 3.1-3.4, 4.4, 5.1-5.4_

- [x] 5. Catalog and stream state machine
  - Catalog range, stream ids, status transitions, shard tables with hash
    ranges, the split hook that protects routing keys
  - `stream_state_machine`: per-shard counters, arrival timestamps, segment
    index, snapshots without record bytes
  - Done: `services/kinesis/include/kythira/kinesis/` `kinesis_hash.hpp`,
    `kinesis_sequence.hpp`, `kinesis_catalog.hpp` and
    `kinesis_stream_state_machine.hpp` (the design's `catalog.hpp` and
    `stream_state_machine.hpp`, named with the module prefix as IAM and
    ledger headers are); tests `kinesis_catalog_test`,
    `kinesis_stream_state_machine_test`; clock assumption in
    `services/kinesis/doc/sequence-numbers.md`. The record segment store is
    shared by a node's groups, so splits and merges move no bytes; a
    snapshot carries each shard's active-segment descriptor, and a
    restoring replica fetches that prefix through a `segment_fetcher` hook
    before it appends. Throttle configuration in the shard state comes with
    task 8.
  - _Requirements: 6.2-6.4, 8.1, 8.3, 8.4_

- [ ] 6. Stream lifecycle and writes
  - `CreateStream`, `DeleteStream`, `DescribeStream`,
    `DescribeStreamSummary`, `ListStreams`, `ListShards`, `DescribeLimits`
  - `PutRecord`, `PutRecords` with MD5 routing, explicit hash keys, ordering,
    per-record results; group commit across shards of one group
  - `kinesis_gateway_node` daemon
  - _Requirements: 6.1, 7.1-7.5, 8.2, 16.1_

- [ ] 7. Reads and iterators
  - HMAC iterator tokens with key rotation; all five iterator types;
    `GetRecords` with `MillisBehindLatest` and `ChildShards`
  - AWS CLI, boto3, aws-sdk-cpp producer and polling consumer end to end
    (Docker and rootless Podman)
  - _Requirements: 9.1-9.4_

- [ ] 8. Throughput limits and the measurement row
  - Leader-side token buckets for writes and reads, throttling errors
  - Throttling on by default; the throughput switch turns it off per
    cluster or per stream, with a test for each setting
  - Measurement: hot shards per group at the `aws` write limit, recorded in
    `doc/`
  - _Requirements: 3.5, 12.1, 12.2_

### Phase 3: KCL 2.x with the native lease layer (first consumer milestone)

- [ ] 9. Lease API and the Java lease layer
  - Lease API on `kinesis_gateway_node`
  - `java/kythira-kcl-leases` (Gradle): `LeaseRefresher`,
    `LeaseManagementFactory`, checkpointer, SigV4 signing
  - KCL 2.x sample consumer on Kythira Kinesis with leases on Kythira, under
    Docker and rootless Podman; leader killed during lease renewal, no shard
    processed by two workers past the failover time
  - _Requirements: 15.1, 15.2, 15.4_

### Phase 4: durability and lifecycle

- [ ] 10. Retention and tiering
  - Retention increase and decrease up to 8760 hours; leader-proposed trim
  - Tiering of sealed segments to the object stores with SHA-256, transparent
    fetch-back, disk guard
  - _Requirements: 11.2-11.4_

- [ ] 11. Resharding
  - `SplitShard`, `MergeShards`, `UpdateShardCount` with lineage, ending
    sequence numbers, tier-1 transactions across groups; closed shards
    readable until retention
  - Chaos: leader kill mid-reshard, no lost or reordered records
  - _Requirements: 10.1, 10.2, 10.4_

- [ ] 12. On-demand mode
  - `UpdateStreamMode`; leader-side controller that splits hot shards and
    scales from observed write rates within AWS's on-demand limits
  - _Requirements: 10.3_

### Phase 5: enhanced fan-out

- [ ] 13. Consumers and `SubscribeToShard`
  - Consumer registration calls and status transitions
  - HTTP/2 event-stream subscriptions with per-consumer limits,
    continuation, child-shard events, resubscribe on replica loss
  - Enhanced fan-out consumer (KCL 2.x and SDK) end to end
  - _Requirements: 13.1-13.3, 4.2, 5.1_

### Phase 6: the rest of the surface

- [ ] 14. Encryption at rest
  - `kms_key_provider` concept, local sealed provider, AWS KMS adapter;
    `StartStreamEncryption`, `StopStreamEncryption`; encrypted segments
  - _Requirements: 14.1_

- [ ] 15. Tags, resource policies, monitoring and account calls
  - Tag operations (both forms), resource policies through IAM's evaluator,
    enhanced monitoring switches, account-level calls; coverage close-out
    (every pinned operation implemented or listed)
  - _Requirements: 2.1-2.3, 14.2-14.5, 16.2_

### Phase 7: KCL 3.x and every lease backend

- [ ] 16. KCL on DynamoDB backends
  - KCL 2.x and 3.x conformance with leases in Kythira DynamoDB (needs
    DynamoDB tasks 7 and 11) and DynamoDB Local
  - AWS DynamoDB lease backend run (**Clark's go**, real-cloud spend)
  - _Requirements: 15.3, 15.4_

### Phase 8: conformance and docs

- [ ] 17. Conformance and chaos
  - Differential test against the chosen emulator; conformance jobs for
    every client in Requirement 17.2, including KPL and the Flink connector
  - Chaos: leader kills during `PutRecords`, tiering and fan-out
  - Optional recording run against real AWS Kinesis (**Clark's go**)
  - _Requirements: 17.1-17.4_

- [ ] 18. Documentation
  - `doc/kinesis.md`: model, profile switches, limits, clock assumption,
    lease backends by KCL version, deployment, unsupported list
  - _Requirements: 8.4, 15.4_
