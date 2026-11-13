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

- [x] 7. Reads and iterators
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
    locally. The aws-sdk-cpp producer and polling consumer
    (`docker/kinesis_sdk_cpp/`) are built in their own image against
    aws-sdk-cpp 1.11.665 (kinesis only, static) rather than through
    vcpkg, and join the scenario: they produce to a two-shard stream
    through one node and consume it across all three, with boto3 reading
    the same records back. `ChildShards` is covered by
    `kinesis_reshard_test` and the scenario's resharding case.

- [ ] 8. Throughput limits and the measurement row
  - Leader-side token buckets for writes and reads, throttling errors
  - Throttling on by default; the throughput switch turns it off per
    cluster or per stream, with a test for each setting
  - Measurement: hot shards per group at the `aws` write limit, recorded in
    `doc/`
  - _Requirements: 3.5, 12.1, 12.2_
  - Progress: token buckets (`kinesis_throttle.hpp`), one second deep,
    refilled on a monotonic clock. Writes are admitted on the proposing
    leader: `PutRecords` takes the longest prefix of each shard's records
    that fits and answers the rest throttled in place, a forwarded append
    carries its admission mode, and a node that learns it is not leader
    refunds. Reads (`GetRecords` calls and bytes, with debt;
    `GetShardIterator`) are limited on the serving node. AWS's messages for
    both. The switch is tested on by default, off per cluster, off per
    stream, and with the stream winning. Measurement in
    `services/kinesis/doc/throughput.md`: one group held 4 hot shards at
    the write limit on a shared-disk sandbox (disk-bound past that).
    Open: per-account control-plane call rates (`LimitExceededException`);
    a re-run on separate disks.

### Phase 3: KCL 2.x with the native lease layer (first consumer milestone)

- [ ] 9. Lease API and the Java lease layer
  - Lease API on `kinesis_gateway_node`
  - `java/kythira-kcl-leases` (Gradle): `LeaseRefresher`,
    `LeaseManagementFactory`, checkpointer, SigV4 signing
  - KCL 2.x sample consumer on Kythira Kinesis with leases on Kythira, under
    Docker and rootless Podman; leader killed during lease renewal, no shard
    processed by two workers past the failover time
  - _Requirements: 15.1, 15.2, 15.4_
  - Progress: the Lease API (`kinesis_lease_api.hpp`, records in
    `kinesis_leases.hpp`): REST under `/v1/leases/<app>/`, versions are
    the Raft log index of the write, reads go through the catalog group's
    log, SigV4 for `kinesis` and DynamoDB actions on the application's
    table ARN. The Java layer (`java/kythira-kcl-leases`, KCL 2.7.4):
    `KythiraLeaseRefresher` and `KythiraLeaseManagementFactory`, leases
    stored as KCL's own DynamoDB items, checkpoints through KCL's
    checkpointer; tested by `gradle test` in the Kinesis Module
    workflow's java job. The sample consumer (`tests/kcl/sample`) runs as
    three workers in the container scenario on a four-shard stream while
    each node is killed in turn (so the catalog leader dies while workers
    renew): every record arrives and no shard has two owners past the
    failover time. Passes under Docker locally; the container job runs it
    under Docker and rootless Podman. Backends by KCL version are in
    `doc/leases.md`.

### Phase 4: durability and lifecycle

- [ ] 10. Retention and tiering
  - Retention increase and decrease up to 8760 hours; leader-proposed trim
  - Tiering of sealed segments to the object stores with SHA-256, transparent
    fetch-back, disk guard
  - _Requirements: 11.2-11.4_
  - Progress: `IncreaseStreamRetentionPeriod` / `DecreaseStreamRetentionPeriod`
    (24-8760 hours, AWS's refusals, one catalog entry). The maintenance
    pass (`kinesis_maintenance.hpp`, every node, acting only on shards it
    leads) proposes `seal_shard_command` for an idle active segment,
    `trim_shard_command` past retention and `tier_segment_command` after
    uploading a sealed segment under its deterministic object key
    (`kinesis_tiering.hpp`); replicas drop tiered local copies, reads fetch
    them back through the node's fetcher (peers, then the object store,
    verified by `install_segment`) and the pass evicts fetched copies after
    ten idle minutes; trimming deletes the objects. The disk guard
    (`kinesis_disk_guard.hpp`) refuses appends below
    `KINESIS_DISK_FLOOR_BYTES` on the proposing node and the leader. The
    daemon tiers to S3 or an S3-compatible endpoint (`tier_s3.cpp`, built
    with the AWS SDK's s3 component). `kinesis_retention_test`, 12 cases;
    `doc/retention.md`. Open: the retention refusal messages are AWS's as
    documented, not yet checked against a recording; other object stores
    need only their `make_tier_store` call in the daemon.

- [ ] 11. Resharding
  - `SplitShard`, `MergeShards`, `UpdateShardCount` with lineage, ending
    sequence numbers, tier-1 transactions across groups; closed shards
    readable until retention
  - Chaos: leader kill mid-reshard, no lost or reordered records
  - _Requirements: 10.1, 10.2, 10.4_
  - Progress: `SplitShard`, `MergeShards` and `UpdateShardCount`
    (`UNIFORM_SCALING`, one reshard per step) in `kinesis_service.hpp`.
    The stream goes UPDATING (a compare-and-set, one reshard at a time),
    the plan comes from a linearizable catalog read, and
    `reshard_catalog_command`, the parents' close and the children's open
    commit as one tier-1 transaction (`raft_stream_cluster::transact`, a
    `transaction_coordinator` over the gateways' new signed
    `/_kythira/kinesis/v1/txn` endpoint; the daemon wraps its state machine
    in `transactional_state_machine`). Ending sequence numbers follow in
    `set_ending_counters_command`; every node's maintenance pass completes
    a reshard abandoned for a minute (`repair_reshards`). AWS's refusals.
    `kinesis_reshard_test`, 5 cases on the in-process cluster;
    `doc/resharding.md`. The container scenario's resharding case
    (three reshards, a node killed as each starts, catalog and shards in
    separate groups, a producer writing throughout) passes under Docker
    locally; tick once the CI Podman leg passes too.

- [x] 12. On-demand mode
  - `UpdateStreamMode`; leader-side controller that splits hot shards and
    scales from observed write rates within AWS's on-demand limits
  - _Requirements: 10.3_
  - Progress: `UpdateStreamMode` (`set_stream_mode_command`; ACTIVE only,
    two switches per 24 hours, the on-demand stream limit;
    `WarmThroughputMiBps` refused). `SplitShard`, `MergeShards` and
    `UpdateShardCount` refuse on-demand streams, as AWS documents. The
    controller (`kinesis_on_demand.hpp`, every node's maintenance pass,
    acting on the shards it leads) samples each on-demand shard's counter
    and bytes and splits one at its middle once it has taken half a
    shard's write limit for 60 seconds; at most 200 open shards; never
    merges. `kinesis_on_demand_test`, 4 cases; `doc/on-demand.md`; a local
    three-node run split a shard at 700 records/s. Open: the refusal
    wording is not checked against a recording of AWS.

### Phase 5: enhanced fan-out

- [x] 13. Consumers and `SubscribeToShard`
  - Consumer registration calls and status transitions
  - HTTP/2 event-stream subscriptions with per-consumer limits,
    continuation, child-shard events, resubscribe on replica loss
  - Enhanced fan-out consumer (KCL 2.x and SDK) end to end
  - _Requirements: 13.1-13.3, 4.2, 5.1_
  - Progress: consumers live in the stream's catalog entry (20 at most,
    one replicated `consumer_command` per change) and are ACTIVE at once;
    their ARNs take tags and policies, and `DeleteStream` honours
    `EnforceConsumerDeletion`. A subscription reads the receiving node's
    replica like `GetRecords`, so a leader change does not touch it; it
    paces itself with a 2 MiB/s token bucket, sends continuation numbers,
    ends after 5 minutes, ends a closed shard with its `ChildShards`, and
    ends with an exception event when the consumer, stream or replica goes.
    The front end hands a streaming answer back as an `event_source` the
    transport pulls. HTTP/2 came from nghttp2, not Proxygen, which would
    have pulled folly into the module's front end: it is a core addition
    (`include/raft/h2_server_session.hpp`) that the HTTPS server uses for
    ALPN "h2" and h2c with prior knowledge, beside HTTP/1.1 chunked
    streaming. Checked by `kinesis_fanout_test` (10 cases),
    `h2_server_test` (7 cases), a local three-node run with boto3 (HTTP/1.1)
    and an unmodified KCL 2.x worker in fan-out mode (HTTP/2, all 40
    records). The container scenario gains a KCL fan-out case through a
    node kill, which runs in CI. Subscriptions are counted per node, so two nodes could each take
    one for the same consumer and shard. See `services/kinesis/doc/fan-out.md`.

### Phase 6: the rest of the surface

- [x] 14. Encryption at rest
  - `kms_key_provider` concept, local sealed provider, AWS KMS adapter;
    `StartStreamEncryption`, `StopStreamEncryption`; encrypted segments
  - _Requirements: 14.1_
  - Progress: records are sealed (AES-256-GCM, random nonce, the routing
    key and data key generation as additional data) by the gateway that
    takes the write, so the log, every replica's segments, peer fetches
    and tiered objects hold ciphertext. The design's per-block encryption
    became per-record, since sealing must happen before the log and the
    store builds blocks only on apply. Data keys are wrapped by a
    `kms_key_provider` and kept per generation in the catalog record;
    stopping keeps them, so older records stay readable. Providers:
    `local_kms_provider` (PBKDF2-derived like the CA key,
    `KINESIS_KMS=local`) and `aws_kms_provider` (KMS JSON protocol with
    SigV4, no SDK component, `KINESIS_KMS=aws`). `kinesis_encryption_test`,
    7 cases; `doc/encryption.md`. Open: the AWS adapter has run only
    against a fake KMS (a real run needs a KMS key and Clark's go); the
    refusals' wording and `StopStreamEncryption`'s edge cases are
    unverified against a recording of AWS; data key generations are never
    pruned.

- [x] 15. Tags, resource policies, monitoring and account calls
  - Tag operations (both forms), resource policies through IAM's evaluator,
    enhanced monitoring switches, account-level calls; coverage close-out
    (every pinned operation implemented or listed)
  - _Requirements: 2.1-2.3, 14.2-14.5, 16.2_
  - Progress: the stream and resource tag operations (one
    `tag_stream_command`; 50 per stream, checked again in `apply()`;
    profile tags validated), `Put`/`Get`/`DeleteResourcePolicy` (stored in
    the catalog record and handed with the tags to the authorizer through
    the front end's resource lookup), `Enable`/`DisableEnhancedMonitoring`,
    `DescribeAccountSettings` / `UpdateAccountSettings` (no billing
    commitment) and `UpdateMaxRecordSize` (1024 KiB only). Per-stream and
    per-shard metrics in the Prometheus text format on `GET /metrics`
    (`KINESIS_METRICS`, off by default; shard-level traffic series follow
    the monitoring switches). Coverage: every pinned operation implemented
    or listed; what remains listed is channels (not in the spec), fan-out
    (task 13), encryption (task 14) and warm throughput.
    `kinesis_tags_test`, 6 cases; `doc/tags-policies-metrics.md`. Open:
    message wording, `ListTagsForStream`'s default `Limit` and
    `GetResourcePolicy` without a policy are unverified against a
    recording of AWS.

### Phase 7: KCL 3.x and every lease backend

- [x] 16. KCL on DynamoDB backends
  - KCL 2.x and 3.x conformance with leases in Kythira DynamoDB (needs
    DynamoDB tasks 7 and 11) and DynamoDB Local
  - AWS DynamoDB lease backend run (**Clark's go**, real-cloud spend)
  - _Requirements: 15.3, 15.4_
  - Progress:
    - Done: the sample consumer takes `KCL_LEASES=dynamodb` and
      `KCL_DYNAMODB_ENDPOINT` and then runs KCL's own lease management
      against that endpoint. `tests/kcl/sample3` builds the same source
      against KCL 3.5.4; only `KythiraLeases`, the one class that uses the
      lease layer (KCL 2.x interfaces), is its own. The KCL image carries
      both (`/opt/kcl-sample`, `/opt/kcl3-sample`).
    - Done: the container scenario's
      `kcl_2_and_3_keep_their_leases_in_dynamodb` case: KCL 2.x and 3.x,
      each with its lease table on Kythira's DynamoDB (`ddb-leases`, a
      `dynamodb_gateway_node` staged into the node image) and on DynamoDB
      Local 3.3.2 (`ddb-local`). Four applications read one stream; each
      must process every record, and after every worker restarts each
      must read the next records and none of the first again, so its
      checkpoints were kept. A native run here (one Kinesis node) passed
      all four: 300 of 300 records each, nothing re-read after the
      restart. Four applications polling two shards exceed 5 GetRecords
      calls a second per shard, so KCL meets and retries
      `ProvisionedThroughputExceededException`, as it would on AWS.
    - Done: the AWS DynamoDB run (Clark's go, 2026-11-13). The sample
      takes `KCL_DYNAMODB_PROFILE` for the lease calls alone, and the
      scenario adds `kcl2-aws-ddb` and `kcl3-aws-ddb` with
      `KINESIS_SCENARIO_AWS_LEASES=1`. On Clark's machine, one Kythira
      node with leases in us-east-1: KCL 2.7.4 and 3.5.4 each read d0 and
      d1 100/100, then after a restart d2 100/100 with nothing of d0 or d1
      again. KCL 3.x waited about 16 minutes for its lease-table GSI on
      AWS. The run's four tables were deleted.

### Phase 8: conformance and docs

- [x] 17. Conformance and chaos
  - Differential test against the chosen emulator; conformance jobs for
    every client in Requirement 17.2, including KPL and the Flink connector
  - Chaos: leader kills during `PutRecords`, tiering and fan-out
  - Optional recording run against real AWS Kinesis (**Clark's go**)
  - _Requirements: 17.1-17.4_
  - Progress: the container scenario's chaos case kills every node in turn
    while `batch-produce` writes 6000 records with PutRecords and
    `fan-out-consume` follows both shards with SubscribeToShard; every
    record arrives, in order under each key, pushed and polled (a local
    three-node run passed: 6000/6000, 8 duplicates from a retried batch
    whose answer was lost). Leader kills during resharding are task 11's
    case; the KCL fan-out case kills a node too. Tiering chaos: the
    scenario's last case recreates the nodes tiering to the in-repo S3 mock
    (`cmd/chaos_s3_mock`, staged into the node image) five seconds after a
    segment's last record (`KINESIS_TIER_AFTER_MS`), slows uploads, fails
    the first six, and kills every node in turn under a producer; every
    failure must fire, segments must reach the store, and every record
    must read back in order under each key through every node. It needs
    the AWS SDK's s3 component, so it runs only in CI. Differential:
    Clark chose recordings over an emulator. `tests/differential/` runs a
    seeded operation mix, normalises the answers, and compares them step
    by step; two recordings made against real AWS Kinesis in us-east-1
    on 2026-11-10 (seeds 1 and 2, 177 and 181 steps) are checked in, and
    the scenario's `differential_replay_matches_the_aws_recordings`
    replays them against a node after a self-check across two others.
    The first replay found one difference: a reshard of a closed shard
    answered ResourceInUseException where AWS answers
    InvalidArgumentException. With that fixed both recordings replay
    with no failures; error messages are compared as notes only, and two
    kinds still differ in wording (ListShards' invalid NextToken, and
    PutRecords' validation message, which AWS prints as Java toString).
    aws-sdk-cpp runs in the container scenario since task
    7. Flink: a Flink 1.20.3 job on an in-process mini cluster
    (`tests/flink`), with connector 5.1.0-1.20's KinesisStreamsSource
    (stream by ARN, polling from the trim horizon) and
    KinesisStreamsSink, copies one stream into another; the scenario's
    Flink case writes 300 records before the job starts and 200 while it
    runs, and every one must reach the output stream once (a native run
    passed: 500 of 500). KPL: every
    node answers STS `GetCallerIdentity` from the caller's signature
    (KPL's first call), and the scenario's KPL case writes 500 user
    records through KPL 0.15.12 with aggregation on, behind a socat TLS
    front, through one node; they must land in fewer than half as many
    Kinesis records, and a KCL 2.x worker on another node must
    deaggregate every one. A native run passed (500 user records in 2
    Kinesis records, all 500 read back).

- [x] 18. Documentation
  - `doc/kinesis.md`: model, profile switches, limits, clock assumption,
    lease backends by KCL version, deployment, unsupported list
  - _Requirements: 8.4, 15.4_
  - Progress: `services/kinesis/doc/kinesis.md` is the operator's page and
    links to the per-subject pages. Its lease-backend table marks the
    DynamoDB backends as planned until task 16 lands; update it then.
