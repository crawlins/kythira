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

- [x] 2. CBOR and event-stream framing (core)
  - CBOR request and response encoding in `aws_json_protocol.hpp`
    (JSON 1.1 framing is already there from DynamoDB task 2)
  - `aws_eventstream.hpp`: prelude, headers, CRCs; AWS's published vectors
  - Done: `cbor_to_json`/`json_to_cbor` map CBOR to and from the JSON
    form under the operation's shapes (blobs as byte strings, timestamps
    as epoch milliseconds under tag 1, indefinite-length containers as
    Jackson writes them); `codec_options::accept_cbor` turns it on and
    every response, error or not, goes back in the request's encoding.
    `include/raft/aws_eventstream.hpp` encodes and stream-decodes
    messages with aws-c-event-stream's limits and builds event, exception
    and error messages. Tests `aws_json_cbor_test` (RFC 8949 Appendix A,
    malformed bodies, shapes, codec and dispatcher) and
    `aws_eventstream_test` (the published empty-message vector, and
    vectors botocore's parser accepts). The millisecond timestamp unit is
    the Java SDK's; task 7's recordings confirm it, and
    `cbor_timestamps` switches it in one place if they do not.
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

- [x] 4. Front end, SigV4, authorization and profile
  - `kinesis_front_end` on the shared Beast host, forwarding, replica reads
  - SigV4; `kinesis` action table; IAM path and static-key fallback
  - Compatibility profile resolution
  - _Requirements: 3.1-3.4, 4.4, 5.1-5.4_
  - Done: `kinesis_front_end.hpp` (transport-free, like DynamoDB's: SigV4
    before the body, JSON and CBOR, StreamName/StreamARN agreement,
    same-named action on the input's resource with request tags, resource
    tags and policy through a lookup hook, handler authorization with a
    safety net, `replica_read_ok` from the action table for routing);
    `kinesis_actions.hpp` generated by `tools/gen_kinesis_actions.py` from
    the pinned Service Authorization Reference, with the static-key roles;
    `kinesis_compat_profile.hpp`; `model/kinesis_model.hpp` from core's
    shape generator. Mounting on the Beast host (with `Expect:
    100-continue`) and forwarding to group leaders come with the daemon and
    the first handlers in task 6.

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

- [x] 6. Stream lifecycle and writes
  - `CreateStream`, `DeleteStream`, `DescribeStream`,
    `DescribeStreamSummary`, `ListStreams`, `ListShards`, `DescribeLimits`
  - `PutRecord`, `PutRecords` with MD5 routing, explicit hash keys, ordering,
    per-record results; group commit across shards of one group
  - `kinesis_gateway_node` daemon
  - _Requirements: 6.1, 7.1-7.5, 8.2, 16.1_
  - Done: the seven lifecycle operations and both writes
    (`kinesis_service.hpp`, over the `stream_cluster` interface); the
    multi-raft cluster (`kinesis_raft_cluster.hpp`) forwarding writes to the
    group leader's gateway, signed with the shared secret
    (`kinesis_forward.hpp`); the listener's routes and the peer client with
    verified TLS (`kinesis_gateway.hpp`); segment fetch between gateways for
    snapshot restore; `kinesis_gateway_node` configured from the
    environment like `redis_gateway_node`; `Expect: 100-continue` in the
    shared Beast HTTPS server. Checked locally on a three-node cluster with
    boto3, through full restarts, a hard kill and a leader loss, with
    byte-identical segments on every replica. Group commit: `PutRecords`
    gathers the shards that share a Raft group into one
    `append_batch_command`, answered per part, so a batch costs one
    proposal per group rather than one per shard.

- [ ] 7. Reads and iterators
  - HMAC iterator tokens with key rotation; all five iterator types;
    `GetRecords` with `MillisBehindLatest` and `ChildShards`
  - AWS CLI, boto3, aws-sdk-cpp producer and polling consumer end to end
    (Docker and rootless Podman)
  - _Requirements: 9.1-9.4_
  - Progress: iterators are HMAC-SHA256 tokens naming the stream (with its
    catalog id), shard, position and issue time, checked by any node
    (`kinesis_iterators.hpp`); the key ring rotates with an overlap, read
    from `KINESIS_ITERATOR_KEYS_FILE` or derived from the forwarding secret.
    All five iterator types; `AT_TIMESTAMP` resolves in `GetRecords`.
    `GetRecords` reads the local replica's segments
    (`kinesis_records.hpp`), fetching a missing sealed segment from a peer
    and failing with InternalFailure rather than skipping one; a trimmed
    position starts at the trim horizon; `MillisBehindLatest`; expiry per
    the `iterator-lifetime` switch (5 minutes, or a day relaxed). The
    container scenario (`tests/docker/`, `docker-kinesis-node-tests`, the
    Kinesis Module workflow's container job) runs three nodes by service
    name with the AWS CLI v2 and a boto3 producer and polling consumer,
    through a stopped node and its catch-up; it passes under Docker
    locally. Open: the aws-sdk-cpp producer and consumer, which needs the
    `kinesis` feature of vcpkg's aws-sdk-cpp; `ChildShards` is in place but
    untested until resharding (task 11) closes a shard.

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
