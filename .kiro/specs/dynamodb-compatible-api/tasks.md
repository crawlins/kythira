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
    family and `SearchVectors`) were listed as unsupported pending a scope
    decision; Clark chose on November 10, 2026 to plan both (tasks 20 and
    26, Requirement 22).

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

- [x] 7. Control plane and single-item API
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
    - Done: the Raft-hosted backend (`raft_backend.hpp`): one
      `dynamo_group_state_machine` per `multi_raft` group (the catalog
      group, item groups over `t/`), requests on any node hopping to each
      group's leader as ops a peer serves, read barriers for consistent
      reads, range reads that walk groups page by page, and the catalog
      leader's follow-on work on a worker thread. Three in-process nodes
      cover it (`dynamodb_raft_backend_test`).
    - Done: `dynamodb_gateway_node` as a cluster node
      (`cmd/dynamodb_gateway_node/run_cluster.hpp`). `KYTHIRA_DYNAMODB_PEERS`
      turns it on; each node runs the `multi_raft` host on core's Beast
      transport with the bootstrap groups, and the gateway forwards an op
      to the group leader's gateway over HTTP(S) with a time-stamped
      HMAC-SHA256 (`forward_link.hpp`). `KYTHIRA_DYNAMODB_ENGINE=rocksdb`
      keeps the groups in one RocksDB database under the data directory
      when the build has the engine. Covered by three nodes in one
      process (`dynamodb_gateway_cluster_test`); boto3 against three
      processes ran create, puts through every node, consistent gets,
      Query, Scan and delete.
    - Left: warm throughput. The KCL sample is the KCL 2.x run in
      `kcl_on_kythira.sh` (task 11).
    - Closed in task 25's review: warm throughput had landed with the
      capacity work (`WarmThroughput` on `CreateTable` and `UpdateTable`
      and in `DescribeTable`, refused when not positive or below the
      current value; `dynamodb_api_test`), and the KCL runs moved to
      `clients_on_kythira.sh` (task 24), so the box is ticked.

- [x] 8. Query, Scan and batch operations
  - Range reads with every `Query` option; cross-shard `Scan` cursors;
    parallel segments; `BatchGetItem`, `BatchWriteItem` with unprocessed
    items
  - Differential test harness against DynamoDB Local, first op mix
  - _Requirements: 9.1-9.3, 10.1, 10.2, 21.1_
  - Done (November 10, 2026):
    - `query_api.hpp`: a key condition becomes one base-key range inside
      the item collection, and each item read is checked against the
      condition again. Scan segments are slices of the 2-byte partition
      hash, so they stay balanced. A cursor is a base key, so it does not
      depend on which group serves the range. Pages stop at `Limit` or
      before 1 MB, and either way return `LastEvaluatedKey`, as DynamoDB
      Local does.
    - `batch_api.hpp`: the whole batch is validated before anything runs.
      Each write is its own entry, so there is no atomicity across items.
      Keys past the 16 MB response limit come back in `UnprocessedKeys`
      together with their table's read settings.
    - `dynamo_backend::read_range` serves both. On `local_backend` it is a
      single group. The Raft backend (task 7) has to walk the groups the
      range covers in key order.
    - The harness is `tools/ops-corpus/`: 6 scenarios and 125 steps,
      recorded against DynamoDB Local 3.3.2 and replayed by
      `dynamodb_ops_corpus_test`. There are no mismatches. 16 steps are
      marked as divergences, and each is one of four kinds:
      - AWS's data-plane not-found message;
      - AWS's `Key` message ("The provided key element does not match
        the schema");
      - AWS's item key messages, which name the key;
      - the core codec's model-constraint messages, where DynamoDB Local
        has its own.
    - Added later (November 10, 2026): `doc/invariants.md`, 93 rules from
      the developer guide and API reference (limits, types, expressions,
      writes, Query, Scan, batches, tables, LSIs), each with its source and
      a case in `dynamodb_invariants_test`. The same cases ran against
      DynamoDB Local 3.3.2 and agree apart from three places Local departs
      from the documentation. The list found four gaps, now fixed: empty
      attribute names were accepted, `BatchGetItem` could pass 16 MB by one
      item, items could nest one level too deep, and a test expected the
      over-limit answer. Rules for later tasks are listed there as
      deferred.

### Phase 3: indexes and transactions (KCL 3.x works after task 11)

- [x] 9. Local secondary indexes
  - LSI entries in the same apply, projections, split veto inside item
    collections, the 10 GB limit and `ItemCollectionMetrics`
  - _Requirements: 11.1, 11.5_
  - Done (November 10, 2026):
    - `local_index.hpp`: an entry is keyed
      `<collection>\1<index no><enc(index sk)><enc(sk)>`, inside its item
      collection, and each collection's size is a meta-family counter at
      `<collection>\2`. Splits are already allowed only at partition
      prefixes, so a collection, its entries and its counter never part.
    - Each write command carries the table's LSIs, and `apply()` moves the
      item's entries and updates the counter in the entry's one batch. A
      write that would grow a collection past 10 GB fails with
      `ItemCollectionSizeLimitExceededException`; one that shrinks it
      always applies. `ReturnItemCollectionMetrics=SIZE` answers in AWS's
      documented shape (DynamoDB Local never returns it). A table drop
      clears entries and counters.
    - `CreateTable` takes `LocalSecondaryIndexes` with DynamoDB Local's
      checks and wording; `DescribeTable` lists them. `Query` and `Scan`
      take `IndexName`. Attributes the index does not project come from
      the table, a `Query` filter may name only projected attributes, and
      index cursors carry the table keys and the index sort key.
    - The ops corpus gained an LSI scenario and a CreateTable scenario: 8
      scenarios, 218 steps, all matching, 23 marked divergences (model
      limits, metrics, and DynamoDB Local accepting an index key of the
      wrong type on `UpdateItem`). Items with equal index keys are ordered
      by the table's sort key; DynamoDB promises no order for ties, so the
      corpus avoids them. `dynamodb_lsi_test` covers what the corpus
      cannot.
    - A `Scan` of an index walks the table's hash slice and skips the
      other keys; a cheaper walk would need a per-index prefix, which
      would separate entries from their collection.

- [x] 10. Transactions
  - `validate` hook in `transactional_state_machine` (multi-raft-transactions
    change, reviewed with that spec's owner)
  - `TransactWriteItems`, `ConditionCheck`, cancellation reasons, token
    idempotency; lock-based `TransactGetItems`
  - Chaos: leader killed mid-transaction
  - _Requirements: 12.1-12.5_
  - Done (November 11, 2026):
    - Core: an inner state machine may offer `validate(key, write)`
      (`txn_validating_state_machine`). `prepare` and `one_phase` call it
      for every write after the lock checks, so a condition is judged
      under the lock against the committed item. A refusal installs
      nothing and answers `validation_failed`; a verdict with detail
      answers `validated`. The coordinator aborts with
      `txn_abort_reason::validation_failed` and returns every shard's
      verdicts in `txn_outcome::_validations`. Inner machines without the
      hook behave as before.
    - Every group runs `dynamo_txn_group_state_machine`, the item and
      catalog machines under the wrapper, partitioned by base key. The
      item machine stages a write into an overlay and commits it in one
      batch, so `apply()`, `commit` and a plain write share one path. A
      `ConditionCheck` is a write that writes nothing, and a
      `TransactGetItems` read is a `read_lock` write whose verdict carries
      the item, so reads see no half-applied transaction.
    - `txn_api.hpp`: `TransactWriteItems` checks the request as DynamoDB
      Local does (at most 100 actions and 4 MB, one item once), authorizes
      each action, and answers `TransactionCanceledException` with one
      reason per action in Local's wording, the item where `ALL_OLD`
      asked for it. A plain write to a locked item answers
      `TransactionConflictException`. `ClientRequestToken` is a record in
      reserved table 0 keyed by a hash of the caller and the token,
      written by the transaction itself: a retry within 10 minutes answers
      success and changes nothing, a changed request is
      `IdempotentParameterMismatchException`, and a cancelled one runs
      again. Capacity is twice the single-item figures.
    - The Raft backend runs transactions through core's coordinator from
      the node that took the request, forwarding to each group's leader
      and retrying a stale route. Tests: `dynamodb_txn_test`; the
      corpus's transactions scenario (9 scenarios, 243 steps, 28 marked
      divergences; of the scenario's five, three are the known message
      kinds and two are DynamoDB Local cancelling with a `ValidationError`
      reason where Kythira answers `ValidationException`, for a key of the
      wrong type and for an update of a key attribute);
      in `dynamodb_raft_backend_test`, transactions across groups, a lock
      refusing plain writes, a leader lost before a transaction, and the
      chaos case: leaders killed and revived while transactions run back
      to back, after which both items always agree.
    - Left as documented limits: two writes to one item collection that
      each fit can together pass 10 GB; expired token records stay until
      TTL (task 13) removes them; no `ItemCollectionMetrics` on
      transactions. The chaos case runs in process; the Docker harness
      comes with the daemon's chaos suite.

- [x] 11. Global secondary indexes
  - GSI key spaces, leader-side propagator from the change log (needs task
    15's change log; until then the propagator reads a temporary in-engine
    queue with the same interface), versioned idempotent writes, lag metric
  - Backfill on `UpdateTable`, removal, projections, the synchronous switch
  - KCL 3.x lease, coordinator and worker-metrics tables on Kythira
  - _Requirements: 11.2-11.6_
  - Done so far (November 11, 2026): GSIs fixed at `CreateTable`.
    - Layout: a GSI's key space takes an id from the catalog's
      never-reused table id counter. An entry lives at
      `t/<index id>/<hash><index partition key>\0[index sort key]<table
      key>` in the data family, holding the projected item; its version
      stays in the meta family after a delete, so a late write cannot
      bring an entry back. On a table with GSIs every write bumps the
      item's version (deletes too, and a deleted item keeps it), so each
      change is newer than everything the indexes hold for the item.
    - Pending changes: a write that changes an item's entries records,
      in the same batch and in the item's own collection (so always in
      its group), the item's version and the entries the indexes held.
      Index writes (`index_put`, `index_delete`) route by entry key and
      apply only when newer than the entry; `index_ack` ends the record
      when nothing newer arrived, else keeps it with what the indexes
      now hold. `plan_propagation` (`gsi_propagator.hpp`) turns a record
      and the item into those writes. The local backend runs it under its
      lock after every write; the Raft backend runs it on each item
      group's leader from a worker thread, through the routed write path,
      so entries reach other groups. A new leader finds the record still
      there and carries it again. `raft_backend::gsi_lag()` is the lag
      (pending items on the groups this node leads). Until task 15 the
      records stand in for the change log: the set of pending keys is
      rebuilt by a meta scan on open.
    - API: `CreateTable` takes `GlobalSecondaryIndexes` with DynamoDB
      Local's checks and messages; `DescribeTable` shows them; `Query`
      and `Scan` take a GSI's `IndexName` (eventual only, projected
      attributes only, Local's messages for `ConsistentRead`, `Select`,
      projections and filters). Writes refuse a GSI key of the wrong type
      or empty. `DeleteTable` deletes each index's key space with a job
      of its own. Descriptor, catalog and item entry forms are now
      version 2; version 1 still decodes.
    - Tests: `dynamodb_gsi_test`; invariants l01-l04 (also against
      DynamoDB Local); in `dynamodb_raft_backend_test`, entries reaching
      other groups after a split and through an item leader's loss; two
      scenarios in the ops corpus (11 scenarios, 333 steps, 30 marked
      divergences), recorded against DynamoDB Local 3.3.2.
    - Known limits: entry and item versions of deleted items (tombstones)
      are never reclaimed; the Raft propagator sends an item's writes one
      at a time; `IndexSizeBytes` and `ItemCount` show 0, as the table's
      `ItemCount` does (the corpus does not compare them).
    - `UpdateTable` (November 11, 2026): `GlobalSecondaryIndexUpdates`
      creates, deletes or re-provisions an index, with DynamoDB Local's
      checks and messages (one index created or deleted at a time and
      none while another is CREATING or DELETING, attribute definitions
      that must agree with the table's, `Nothing to update`, and so on).
      The table stays ACTIVE, as in Local. A new index is CREATING with
      `Backfilling` true, as AWS shows it (Local shows false: a marked
      divergence in the corpus); writes maintain it at once
      and reads of it are refused with AWS's "Cannot read from
      backfilling global secondary index" (Local fails them with an
      internal error). The backfill marks every item of the table as a
      pending change (`index_mark`, one entry per group, a key a split
      moved marked again on its own) and the propagators carry the marks
      like any write; versions make marks that race writes harmless. The
      local backend backfills before `UpdateTable` returns. The Raft
      backend's catalog leader waits `gsi_backfill_delay` (by default the
      call deadline) first, so no write that read the table before the
      index existed can land after the backfill passed its item, then
      makes the index ACTIVE once every item is marked; the entries
      follow within the propagation lag. A deleted index is DELETING
      (its keys leave the AttributeDefinitions at once, as in Local)
      until its key space's delete job ends, then it goes.
    - Tests: in `dynamodb_gsi_test`, marks and the catalog's index
      transitions; invariant l05; in `dynamodb_raft_backend_test`, an
      index added to a split table while writes race the backfill, then
      deleted; a 26-step `UpdateTable` scenario in the ops corpus (12
      scenarios, 359 steps). The corpus test now also compares
      AttributeDefinitions and ignores the placeholder
      `OnDemandThroughput` of -1 units Local gives an index `UpdateTable`
      created.
    - Synchronous switch: on a table whose `gsi-consistency` is `sync`
      (the `kythira:compat:gsi-consistency` tag, or the cluster profile
      given to the APIs with `set_compat_profile`), every write
      (single-item, batch and transactional) carries its own pending
      change before it answers: `settle_item` reads the record and the
      item at the item group's leader and sends the index writes and the
      ack from the node that took the request, beside the group's own
      propagator (versions make the race harmless). A read of the index
      after the write therefore sees it. This differs from the design's
      wording (a tier-1 transaction over the base key and the old and new
      index keys): the entries depend on the item the write produced, so
      a transaction would have to read the item first and could not run
      condition and update expressions in one step; carrying the change
      after the base write gives the same read-your-writes for index
      reads, which are eventual in DynamoDB anyway, without that. The
      base write and its entries are not atomic to a concurrent reader.
      The local backend already propagates before answering. Test: in
      `dynamodb_raft_backend_test`, a synchronous and an asynchronous
      table with the propagators idle.
    - KCL 3.x tables: the KCL 3.5.4 evidence client runs end to end with
      its lease table on Kythira (and KCL 2.x with its own), the recording
      stub standing in for the stream: `tools/evidence/kcl_on_kythira.sh`
      starts the stub with `--dynamodb-upstream`, which passes every
      DynamoDB request unchanged to Kythira and records the answer, and
      `check_kcl_run.py` requires every refusal to be one the KCL expects
      (`ResourceNotFoundException` on `DescribeTable`, a failed condition),
      the lease table and a lease written, the `UpdateTable` adding
      `LeaseOwnerToLeaseKeyIndex` accepted and a `Query` on it finding the
      worker's lease. The `build` job of `dynamodb-module.yml` runs it on
      a three-node cluster of the daemon it built, in containers, under
      Docker and rootless Podman. KCL 3.5.4 asks for the
      `<app>-CoordinatorState` and `<app>-WorkerMetricStats` tables, finds
      none and keeps the coordinator, leader and worker-metrics entries in
      the lease table (`entityType` `CLIENT_VERSION_MIGRATION`, `LEADER`,
      `WORKER_METRIC_STATS`); DynamoDB Local 3.3.2 got the same
      conversation, request for request, in a run with the same client.
      The daemon gained `KYTHIRA_DYNAMODB_LOG_LEVEL` (default `info`): its
      Raft groups logged at `trace`.

- [x] 12. Capacity, throttling and consumed capacity
  - Consumed-capacity formulas; provisioned and on-demand modes; leader-side
    token buckets per table, index and partition; throttling errors
  - Throttling on by default; the capacity switch turns it off per cluster
    or per table, with a test for each setting
  - _Requirements: 3.5, 19.1-19.3_
  - Notes:
    - Formulas: a read is 4 KB units (half for an eventual read), a write
      1 KB units of the larger of the item before and after it (the stored
      item on a failed condition, which the state machine now reports as
      `item_result::charged_bytes`), a transaction twice that. A write's
      GSI share goes to each index it touches: a put to the indexes whose
      partition key the item carries, an update or delete to all of them.
    - Buckets (`capacity.hpp`): a provisioned table or index fills at its
      units a second and holds 300 seconds of them, as AWS's burst
      capacity; an on-demand one at 40,000 units a second, AWS's default
      account table limit; each partition at 3,000 read and 1,000 write
      units a second. A request is admitted while its buckets are not in
      debt and charged afterwards, so one large item can borrow; this is
      how AWS answers too.
    - Errors: `ProvisionedThroughputExceededException` with
      `ThrottlingReasons` for provisioned tables, `ThrottlingException`
      with `throttlingReasons` on demand, each reason
      `<Table|Index><Read|Write><ProvisionedThroughputExceeded|
      AccountLimitExceeded|KeyRangeThroughputExceeded>` with the table or
      index ARN. A throttled transaction is cancelled with
      `ThrottlingError`; a batch returns throttled entries unprocessed and
      fails only when all of them are.
    - Deviation: the buckets are per gateway node, each holding
      1/(voters) of the table's rate (`raft_backend_options::
      capacity_share`), not on the group leader. Every node admits
      without a hop, and a client spread over the nodes sees the table's
      rate; one pinned to a single node sees its share. Load-based splits
      for hot partitions (19.3) wait for the split policy (task 7).
    - The switch is `throttling` (`KYTHIRA_DYNAMODB_COMPAT=throttling=off`
      for the cluster, tag `kythira:compat:throttling` per table);
      `ConsumedCapacity` is reported either way. Tests:
      `dynamodb_capacity_test`; tests that probe the size limits turn
      throttling off.

### Phase 4: streams, TTL and PartiQL

- [x] 13. TTL
  - `UpdateTimeToLive`, `DescribeTimeToLive`, TTL index, leader-proposed
    conditional delete batches, delay switch
  - _Requirements: 15.1-15.3_
  - Notes:
    - The setting is the descriptor's `time_to_live` (catalog format 3,
      which still reads 2) and a `set_ttl` catalog command. Its refusals
      are DynamoDB Local's, word for word ("TimeToLive is already
      enabled", "... already disabled", "... active on a different
      AttributeName"), checked in Local's order. AWS's one change an hour
      ("Time to live has been modified multiple times within a fixed
      interval"), which Local does not keep, is kept under `ttl-delay=aws`.
      `DescribeTimeToLive` answers `ENABLED` or `DISABLED` at once, as
      Local does; AWS's `ENABLING` and `DISABLING` are not shown.
    - An item expires once its TTL attribute, a number of seconds since the
      epoch, has passed; a value that is not a number, or is more than five
      years old, never expires, as AWS documents. Expired items stay
      readable until deleted.
    - `ttl_sweeper` (`ttl.hpp`) walks each TTL table a slice at a time
      (1,000 items read, 100 deleted per pass) and deletes with the
      condition that the attribute still holds the value it read, so a
      rewritten item is kept. Deletes go through the backend as ordinary
      item entries, below the throttler (no capacity used, as on AWS), and
      take LSI and GSI entries with them. The gateway runs a pass a second
      on the catalog leader (`raft_backend::leads_catalog()`), or on the
      one node for `local_backend`.
    - `ttl-delay=aws` (the default) walks a table again at most every 10
      minutes; `prompt` walks it every pass and lifts the hourly limit.
    - Deviation: no TTL index. The sweeper pages through the table, so a
      walk costs the table's size, but nothing new is kept in the item
      groups or carried across splits. TTL deletes will reach streams with
      task 16.
    - Tests: `dynamodb_ttl_test`; `dynamodb_gateway_cluster_test` sweeps
      on a three-node cluster.

- [x] 14. PartiQL
  - Parser and compiler to the JSON API's requests; `ExecuteStatement`,
    `BatchExecuteStatement`, `ExecuteTransaction`
  - _Requirements: 13.1, 13.2_
  - Notes:
    - `partiql.hpp` parses a statement and compiles it into the JSON API
      request it stands for (paths into `ExpressionAttributeNames`,
      literals and `?` parameters into `ExpressionAttributeValues`), which
      the JSON API's handler then runs. Limits, conditions, capacity and
      transaction rules are therefore the JSON API's.
    - A `SELECT` with an equality (or `IN` list) on the partition key runs
      as `Query`, with a sort-key term as the key condition and the rest as
      a filter; any other `SELECT` is a `Scan`. `Limit`, `NextToken` (the
      last evaluated key, base64 JSON) and `ORDER BY <sk> DESC` work.
      `INSERT` is a `PutItem` with `attribute_not_exists`, `UPDATE` an
      `UpdateItem` with `attribute_exists`, `DELETE` a `DeleteItem`, and
      `EXISTS` a transaction `ConditionCheck`.
    - Answers and errors are DynamoDB Local 3.3's, statement for statement
      (`DuplicateItemException` for a duplicate `INSERT`, a duplicate in a
      transaction as a `ValidationError` reason, writes answering
      `{"Items":[]}`, nested projections named by their last name).
    - Each statement is authorized by its PartiQL action (`PartiQLSelect`,
      `PartiQLInsert`, `PartiQLUpdate`, `PartiQLDelete`) on its table or
      index, as AWS does; the JSON handlers then run without a second
      check.
    - Tests: `dynamodb_partiql_test`; authorization in
      `dynamodb_gateway_test`.

- [x] 15. Change log
  - Change records appended in `apply()` to the group's
    `record_segment_store`; the GSI propagator switched onto it
  - _Requirements: 14.1_
  - Notes:
    - `change_log.hpp`. A write to a logged table that changes its item
      appends one record (`INSERT`, `MODIFY`, `REMOVE`, both images, the
      TTL flag, the proposer's timestamp clamped never to go back) to the
      node's `record_segment_store` in the same apply, fsynced before the
      engine batch. A failed condition, a delete of a missing item and a
      write that leaves the item as it was append nothing, as Streams
      documents. Commands carry the fields in format 3, written only when
      set, so other writes stay format 2.
    - One store stream per group *epoch* (`ddb/<hex range start>/<epoch>`):
      a split or merge seals the open stream on every replica at the same
      entry and opens one per resulting group naming its parents, the
      lineage task 16 shows as shards. The group's `change_log_state`
      (epoch, sequence, segments, closed streams) sits at its range start
      plus byte 5 in the meta family, so it moves with snapshots, splits
      and merges. Opening truncates records past it (an entry the engine
      never made durable, replayed by Raft).
    - A checkpoint adds a file naming the open stream's active segment;
      `ingest` fetches that segment from the group's voters when this node
      lacks it (`raft_backend::fetch_change_segment` over the new `segment`
      op) and refuses without it, as Kinesis does. Sealed segments a node
      lacks are fetched by readers in task 16.
    - `table_descriptor::logs_changes()` is false until Streams (16),
      point-in-time recovery (19) and global tables (22) turn it on, so
      no table pays for the log before something reads it.
    - Deviation: GSIs stay on the pending records of task 11. Those live
      beside each item and move with it through splits; a per-group log
      would make every split child carry its parent's cursor and fetch
      the parent's segments, for no behaviour a client can see. Closed
      streams are kept until task 16's 24-hour retention trims them.
    - Tests: `dynamodb_change_log_test`; replicas, a split and a peer
      fetch on three nodes in `dynamodb_raft_backend_test`.

- [x] 16. DynamoDB Streams
  - [x] Stream shards over the change log, lineage across splits, merge veto,
    24-hour retention per stream shard, iterators, the four actions
  - [x] Streams Kinesis Adapter conformance with KCL
  - _Requirements: 14.2-14.5_
  - Notes:
    - `StreamSpecification` on `CreateTable` and `UpdateTable` keeps a list
      of the table's streams in the descriptor (format 4, still written
      as 3 for a table that never had one): label (the enable time, from
      the entry, unique per table), view type, disable time. A disabled
      stream stays listed for 24 hours, while its records can be read;
      `DescribeTable` drops the specification but keeps `LatestStreamArn`
      and `LatestStreamLabel`, as DynamoDB Local does. Enabling or
      disabling takes effect at once and the table stays ACTIVE.
    - A table with an enabled stream logs its changes, and every logged
      write carries the stream's label into its record (`stream_ms`), so a
      stream shows only its own table's records written while it was
      enabled, whatever the writer's and the catalog's clocks say. The
      store keeps both images; the view type picks what `GetRecords`
      shows.
    - `streams.hpp`. A shard is one store stream: one group in one epoch.
      A stream lists the open store stream of every group its table
      overlaps (a group with no logged write yet shows the one its first
      will open) and each closed one holding a record of the table
      written since the stream was enabled; each store stream records the
      tables it holds (`log_stream::tables`). `ParentShardId` names the
      closed stream a split continued. Sequence numbers are the epoch then
      the record's place, 21 digits; iterators carry the ARN, the store
      stream and the next position, and expire after 15 minutes.
    - `raft_backend` serves `shards` (an item group's streams, at its
      leader after a read barrier) and `changes` (a page of one store
      stream, at the leader of the group holding its range start, which
      every successor of a split does); a segment the group names but the
      node lacks is fetched from its voters first.
    - Retention: each item group's leader proposes `trim_change_log`
      (cutoff now minus 24 hours) when a sealed segment's last record is
      older, every minute; the state drops those segments and closed
      streams left empty, and the store removes the files after the
      engine batch. Segments roll hourly, so records live 24 to 25 hours.
      `local_backend` trims on writes, at most once a minute.
    - Merge veto (14.3): `raft_backend::merge_groups` refuses when either
      range overlaps a table with a stream, enabled or still listed; the
      node runs no automatic merges.
    - The memory engine's change log goes to a scratch directory removed
      at exit, so Streams work there too (items do not outlive the
      process either).
    - Not built: the two-readers-per-shard limit (deferred in
      `invariants.md`).
    - Tests: `dynamodb_streams_test` (DynamoDB Local's answers, view
      types, streams that never see each other's records, TTL identity,
      retention, expiry, stored forms); a split and the merge veto on
      three nodes in `dynamodb_raft_backend_test`.
    - Adapter conformance: `tools/evidence/kcl_on_kythira.sh` now runs a
      third client, `streams-adapter` (DynamoDB Streams Kinesis Adapter
      over KCL), against a Kythira node. It creates `adapter-source` with
      a `NEW_IMAGE` stream, writes half its items, starts the worker,
      writes the rest, and fails unless every item is seen exactly once;
      `check_kcl_run.py` also requires `GetRecords` answers with records.
      Native run 11-11: 60 of 60 items, none twice, with KCL 2.x and 3.x
      still passing. Not covered: a split during the run (there is no
      external split API yet); the three-node split test covers lineage.

- [x] 17. Kinesis streaming destinations
  - Publisher to Kythira Kinesis or AWS Kinesis; the four actions
  - _Requirements: 16.1, 16.2_
  - Notes:
    - `kinesis_destination.hpp`. A table's destinations live in its
      descriptor (format 5, still written as 3 or 4 for a table that never
      had one): stream ARN, status, precision, enable time, failure text
      and the publisher's cursors. One streams at a time, as on AWS; an
      enable answers `ENABLING`, a disable `DISABLING`, an update
      `UPDATING`, and the publisher completes each with a
      `destination_progress` entry. The stream ARN must be a Kinesis
      stream in the table's region. Refusal kinds follow AWS; their
      wording is Kythira's, since DynamoDB Local has no destinations to
      record and no AWS run was made.
    - A streaming destination turns the change log on (`logs_changes()`),
      and the publisher reads it through `change_feed_backend`, like
      DynamoDB Streams, keeping records of its table written since the
      enable. Records are AWS's documents (both images,
      `ApproximateCreationDateTime` in ms or µs, the µs ending in 000
      because the log keeps ms; a TTL delete's `userIdentity` in the
      lower-case form Lambda-style records use, unverified against AWS).
      The partition key is a hash of the item's key, so an item stays in
      one Kinesis shard; AWS does not document its own choice.
    - At least once, in order per item: a store stream continued by a
      split waits for its parent to drain; one `PutRecords` call holds at
      most one record per item; the cursor stops at the first failed
      record, and the records after it are resent. Cursors go to the
      catalog after each pass that moved them, so a new catalog leader
      resends at most one pass. Passes are bounded (20,000 records per
      destination).
    - The gateway runs a pass a second beside the TTL sweeper (on the
      catalog leader for `raft_backend`). `kinesis_http_sink` signs JSON
      1.1 calls with SigV4 (`DescribeStreamSummary` to enable,
      `PutRecords`), to AWS's endpoint for the stream's region or
      `KYTHIRA_DYNAMODB_KINESIS_ENDPOINT`.
    - Tests: `dynamodb_kinesis_destination_test` (13 cases). Also run once
      in the sandbox, not checked in because it would join the DynamoDB
      and Kinesis modules in one test: the publisher against an
      in-process Kythira Kinesis gateway with static-key SigV4 checks,
      50 of 50 records over two Kinesis shards, in per-item order, and an
      enable refused with Kinesis's own "not found" text before the stream
      existed.
    - Run once against AWS Kinesis on November 12, 2026, with Clark's go
      (under $1): the publisher with SigV4 to a real stream, 50 of 50
      records, in per-item order, in the documented record format; an
      enable of a stream that does not exist ended `ENABLE_FAILED` with
      AWS's own "Stream X under account N not found." text. Not checked
      in (it needs AWS credentials and a stream).

### Phase 5: data protection

- [x] 18. Backups and restore
  - Consistent multi-range checkpoints, manifests on the object stores,
    `CreateBackup`, `ListBackups`, `DescribeBackup`, `DeleteBackup`,
    `RestoreTableFromBackup`
  - _Requirements: 17.1_
  - Notes:
    - `backup.hpp`. A backup is a record (`backups/<id>/backup.json`:
      name, status, size, item count, the source descriptor and one entry
      per data object with its sha256) and data objects of DynamoDB JSON
      lines (`backups/<id>/data/000000.json`, at most 10,000 items or
      16 MiB each). Stores: a directory (`KYTHIRA_DYNAMODB_BACKUP_DIR`,
      written by rename), any Kythira object store through
      `key_object_store` (`KYTHIRA_DYNAMODB_BACKUP_S3_BUCKET` and friends
      in the gateway daemon, when built with the AWS SDK), or memory (a
      single node with neither). DIR and BUCKET together are refused at
      start. A cluster with no store refuses `CreateBackup` with a
      `ValidationException` naming the settings.
    - Deviation from the design: no checkpoint at one catalog-chosen
      applied index per group. The worker reads the table's whole range
      with linearizable reads made after the request, group by group, so
      every write acknowledged before `CreateBackup` is in the backup and
      a write racing it may or may not be (AWS documents the same for
      on-demand backups). Engine checkpoints would need a per-group
      snapshot export the engines do not offer yet; PITR (task 19) can
      revisit it.
    - Only the base table's items are stored. A restore creates a new
      table (new id, `RestoreSummary`, `CREATING` with
      `RestoreInProgress` true, writes refused) with the backup's
      indexes, or the subset named by the index overrides, and the
      billing and throughput overrides; the worker verifies each object's
      sha256, puts every item through the ordinary write path (so indexes
      are rebuilt, not copied) and sends `restore_done` (catalog format
      6), which makes the table `ACTIVE`; the status settler that turns new
      tables `ACTIVE` leaves a restoring one alone. A damaged or missing object stops
      the load; it is retried each pass and logged.
    - The worker runs in the gateway's background loop beside the TTL
      sweeper and the Kinesis publisher (on the catalog leader for
      `raft_backend`), so a long backup delays those passes. It lists the
      store when poked by `CreateBackup` or every 10 s; an unfinished
      backup or restore is taken up again by the next pass, here or on a
      new leader. A backup whose table was deleted before it was taken is
      dropped. `DeleteBackup` refuses a `CREATING` backup
      (`BackupInUseException`).
    - Refusal kinds follow AWS; the wording is Kythira's (DynamoDB Local
      has no backups, and no AWS run was made). Backup ARNs use AWS's
      form, `<table arn>/backup/<14 digits>-<8 hex>`.
    - Tests: `dynamodb_backup_test` (10 cases) and, in
      `dynamodb_raft_backend_test`, a backup of a table split over two
      groups taken by a worker on one node and restored through another.

- [x] 19. Point-in-time recovery
  - Change-log archive, `UpdateContinuousBackups`,
    `DescribeContinuousBackups`, `RestoreTableToPointInTime`
  - _Requirements: 17.2_
  - Notes:
    - `pitr.hpp`. The descriptor's `pitr` (catalog format 7, still
      written as 6 or older for a table that never had it) holds the
      setting (enabled, `RecoveryPeriodInDays` 1 to 35, enable time) and
      the archiver's progress (cursors per store stream, next object
      number, earliest and latest restorable times), moved by
      `pitr_progress` entries that never go back. Enabled recovery turns
      the change log on. A fresh enable starts a new archive under
      `pitr/<table id>/<enable time>/` in the task 18 backup store;
      changing only the period keeps it; a disable ends it.
    - The archiver runs in the gateway's background loop after the backup
      worker. Each pass copies the table's new change records into one
      log object of DynamoDB JSON lines (write time, key, new image or
      none for a delete), parents before split children as the Kinesis
      publisher does, so each item's records stay in order. A pass that
      caught up with every store stream sets `LatestRestorableDateTime`
      to its start (AWS's is about five minutes behind). A base (every
      item, read consistently, task 18's layout, with its start and end
      times and the first log object after its start) is taken at enable
      and each day.
    - A restore to T loads the newest base that ended by T and replays the
      archived records from its first log object on with write times up
      to T. A record the base already holds is replayed harmlessly, since
      replay keeps each item's order and ends at its last write by T.
      The table is created `CREATING` with a `RestoreSummary` naming the
      source and T, and becomes `ACTIVE` with `restore_done`.
    - `EarliestRestorableDateTime` is the later of the oldest kept base's
      end and the window's start. Bases that ended before the window go,
      bar the newest of them, with the log objects only they needed.
      Archives no enabled recovery owns (a disabled or re-enabled one's,
      a deleted table's) are swept every minute.
    - Deviations: a deleted table's recovery ends with it (AWS keeps a
      system backup for 35 days); the archive can only keep up while the
      change log still holds the records (24 hours, task 16), so a
      catalog leader stalled longer leaves a gap; restore times assume
      the nodes' clocks agree to well under a second (records carry the
      writer's time, bases and the latest time the catalog leader's);
      log objects carry no checksum (bases do). Refusal wording is
      Kythira's (DynamoDB Local has no point-in-time recovery, and no
      AWS run was made). Without a backup store,
      `UpdateContinuousBackups` answers
      `ContinuousBackupsUnavailableException`.
    - Tests: `dynamodb_pitr_test` (8 cases) and, in
      `dynamodb_raft_backend_test`, restores to times on both sides of a
      split with the archiver on another node.

- [x] 20. Export, import and bulk put
  - Full and incremental `ExportTableToPointInTime`, `ImportTable`, the
    describe and list calls; AWS file layouts
  - `StartBulkPutItems`, `DescribeBulkOperation`, `ListBulkOperations`,
    `CancelBulkOperation`: an import into an existing table through
    ordinary writes, rate-limited, with its report
  - _Requirements: 17.3, 22.1_
  - Notes:
    - `export_import.hpp`, `export_formats.hpp`. A request's S3 bucket
      maps to a store: a directory under `KYTHIRA_DYNAMODB_BUCKET_DIR`
      (default `<backup dir>/buckets`), the S3 bucket of that name on the
      backup store's endpoint, or memory for one node on the memory
      engine. Each export and import is a JSON record in the task 18
      backup store (`exports/<id>/`, `imports/<id>/`), so there is no
      catalog change; a worker in the gateway's background loop runs the
      IN_PROGRESS ones on the catalog leader, from the start again after
      a leader change. Records are listed for 90 days, client tokens hold
      for 8 hours (`ExportConflictException`, `ImportConflictException`).
    - Exports need point-in-time recovery, as on AWS, and write AWS's
      layout under `<S3Prefix>/AWSDynamoDB/<id>/`: `_started`, gzipped
      data files of `{"Item":...}` lines (DynamoDB JSON or Ion),
      `manifest-files.json` with each file's count, base64 MD5, ETag and
      key, `manifest-summary.json` and its `.md5`. A full export with no
      `ExportTime` reads the table now; one at T rebuilds it from the
      point-in-time archive (task 19's base and replay, now shared as
      `pitr_detail::replay`). An incremental export from F to T rebuilds
      the table at F, applies the records up to T, and writes one line per
      touched item: keys, last write time in µs, `OldImage` at F (with
      `NEW_AND_OLD_IMAGES`) and `NewImage` at T (none once deleted).
    - Imports create the table `CREATING` from `TableCreationParameters`,
      checked by `CreateTable`'s own code (now
      `table_api::descriptor_of_create`), with the import's ARN in its
      `table_restore` so it loads before it takes writes and shows no
      `RestoreSummary`. Every object under the prefix is decompressed
      (none, gzip, zstd) and read as DynamoDB JSON, Ion or CSV (header
      row or `HeaderList`, a delimiter, key attributes of the table and
      its GSIs typed as declared, other cells strings, empty cells left
      out); items go through the ordinary write path, so GSIs are built.
      Unreadable or refused items count in `ErrorCount` and are skipped.
      An import with no objects fails and its table is dropped.
    - Ion follows AWS's mapping (decimals for N, blobs for B, annotated
      lists for sets); the reader takes ints, decimals, floats, long
      strings, symbols, clobs and comments, and refuses timestamps,
      s-expressions, typed nulls and nan or infinities. zlib and libzstd
      are optional (`find_package`), present in vcpkg builds.
    - Deviations: a point-in-time or incremental export holds the rebuilt
      table in the catalog leader's memory; SSE settings are recorded but
      the stores add no encryption of their own; without zlib, data files
      are written uncompressed; the 15-minute minimum incremental period
      is an option (tests set it to 0). Refusal wording is Kythira's
      (DynamoDB Local has no exports or imports, and no AWS run was made).
    - Tests: `dynamodb_export_import_test` (18 cases) and, in
      `dynamodb_raft_backend_test`, an Ion export across a split imported
      back through another node.
    - Bulk put (`bulk_put.hpp`): `StartBulkPutItems` reads DynamoDB JSON
      or Ion records with the import reader and puts each `Item` into the
      existing table through the write path, so conditions, LSIs, GSIs,
      streams and the change log see every item. A record may carry
      `ConditionExpression` and its names and values, compiled as
      `PutItem` compiles them. Records live in the backup store
      (`bulk/<id>/`, ARN `.../bulk-operation/<id>`); progress is saved
      after each input object with the objects done, so a new leader
      resumes at the first object not done (only that object is put
      twice). `MaxWriteRequestUnits` drives a token bucket (one unit per
      started KB, one second of burst). `ERROR` and
      `CONDITION_CHECK_FAILURE` reports are JSON lines under
      `<prefix>/AWSDynamoDB/BulkOperations/<id>/` in the output bucket
      (Kythira's layout: AWS documents none). Status ends `COMPLETED`, or
      `COMPLETED_WITH_ERRORS` when a record or object was in error
      (condition failures are not errors). `CancelBulkOperation` writes a
      cancel marker and answers `CANCELLING`; the worker stops at its next
      check (every 256 records) and ends `CANCELLED`, keeping what it
      wrote. `ListBulkOperations` is newest first, 90 days.
      `PerformanceTier` is recorded only.
    - Tests: `dynamodb_bulk_put_test` (7 cases).

### Phase 6: the rest of the surface

- [x] 21. Resource policies and contributor insights
  - `PutResourcePolicy`, `GetResourcePolicy`, `DeleteResourcePolicy` through
    IAM's evaluator; contributor insights counters and calls
  - _Requirements: 5.3, 19.4_
  - Notes:
    - `resource_policy.hpp`. Policies live in the catalog descriptor
      (format 8: `policies` and `insights` after `pitr`, written only for
      a table that has either), one for the table and one per stream,
      changed by a `set_resource_policy` entry so `ExpectedRevisionId`
      (and `NO_POLICY`) is checked atomically. Revision ids are entry
      milliseconds, growing per resource; putting the attached document
      again keeps its revision; deleting without an expected revision is
      idempotent. `CreateTable` takes `ResourcePolicy`; restores and
      imports never copy one. Only the document's shape is checked
      (20 KB with whitespace, JSON, each statement's `Effect`, `Principal`
      and `Action`): what it grants is the authorizer's to decide.
    - The front end's `resource_lookup` (`resource_lookup_for()`) puts the
      table's policy and tags on every authorization of the table and its
      indexes, and the stream's policy on the stream's; with a lookup set
      it also names a foreign resource account. IAM's evaluator then
      decides as AWS does. `PutResourcePolicy` asks the authorizer whether
      the new policy still lets the caller put one and refuses a lockout
      without `ConfirmRemoveSelfResourceAccess`; the account root's
      `DeleteResourcePolicy` is authorized without the policy. Deviation:
      `dynamodb_gateway_node` still runs the static-key authorizer, which
      has no resource policies, so it sets no lookup; wiring IAM's
      authorizer into the daemon is the composition root's next step, not
      this task's. Tests use an authorizer that evaluates a policy's
      denials.
    - `insights.hpp`, `insights_api.hpp`. Counting happens where capacity
      is charged (`api_common.hpp`), with or without throttling, for
      enabled settings: `PKC` and `SKC` by consumed units, `PKT` and
      `SKT` by throttled requests (`THROTTLED_KEYS` only the latter).
      Each rule keeps a Space-Saving summary of 256 keys per minute for
      an hour, keyed by the enable time, so a re-enable starts afresh.
      Deviation: the requirement's "sampled counters" are bounded
      summaries that see every request; per gateway node like the token
      buckets (task 12). The update answers `ENABLING`/`DISABLING` and the
      setting reads `ENABLED`/`DISABLED` at once (there are no CloudWatch
      rules to wait for); rule names follow AWS's
      `DynamoDBContributorInsights-<code>-<table>[-<index>]-<ms>`, cut to
      128 characters. CloudWatch's rule report is replaced by a Kythira
      extension, a SigV4-signed `GET /kythira/contributor-insights`
      authorized as `DescribeContributorInsights`. Deleting a GSI drops
      its setting. `ListContributorInsights` with `TableName` lists the
      table and every GSI; without, every setting ever changed.

- [x] 26. Vector indexes and `SearchVectors` (added after the spec; done
  in this phase, numbered last so later tasks keep their numbers)
  - `VectorIndexes` on `CreateTable`, `VectorIndexUpdates` on
    `UpdateTable`, their descriptions
  - Per-group approximate nearest-neighbour graphs fed by the change log;
    `SearchVectors` fan-out, merge by score, re-read of the winners
  - Recorded against AWS only with Clark's go, as DynamoDB Local may not
    implement vector search
  - _Requirements: 22.2-22.4_
  - Notes:
    - `vector_index.hpp`. Indexes live in the catalog descriptor (format
      9: `vectors` after `insights`, written only for a table that has
      one) and change through `update_vectors`, which works like
      `update_gsis`: one create or delete at a time, CREATING then ACTIVE,
      DELETING then gone. They need no key space: graphs are in memory.
      `CreateTable` indexes are ACTIVE with the table; restores keep the
      backup's or take `VectorIndexOverride` (whole definitions). A table
      with a vector index logs its changes. Checks: 1-4096 dimensions, a
      vector attribute that is not a key, at most one `HASH`, no attribute
      twice, `NonKeyAttributes` only with `INCLUDE`, names unique across
      LSIs, GSIs and vector indexes, 20 per table.
    - `vector_search.hpp`. Deviation from "per-group graphs": each gateway
      node keeps graphs of the indexes it has searched, per `HASH` value,
      built from a consistent table read and then fed from every change
      log stream of the table (parents before children, as the
      point-in-time archiver reads them); records trimmed before they were
      read cause a rebuild. One node's graphs cover the whole table, so
      there is no fan-out or merge: the node ranks its candidates, reads
      the best again consistently, re-scores them from the item as it is,
      drops any that are gone or no longer match, and fetches more until
      it has `TopK`. So a search never returns an item that is not in the
      table (22.4), and maintenance follows writes asynchronously.
    - Graphs: HNSW through hnswlib (Clark, 2026-11-12, decision card:
      "hnswlib via vcpkg"), header-only in `vcpkg.json`; a partition under
      1000 entries is scored exactly, larger ones search HNSW with a breadth
      of at least 64 and twice `TopK`, and scores are always recomputed
      from the stored vector. A build without hnswlib scores everything
      exactly. Scores: `COSINE` 1 - cosine similarity (0 to 2, a zero
      vector at 1), `EUCLIDEAN` the distance, `DOT_PRODUCT` the dot
      product (largest first); ties by key.
    - `SearchConditionExpression` takes `attribute = :value` terms joined
      by `AND` on search schema attributes, and must give the `HASH`
      value. KEYS_ONLY results show the table keys, the vector and the
      search schema attributes; `ProjectionExpression` narrows further.
      Deviation: `VectorSearchRequestBytes` is the search vector's size as
      32-bit floats; AWS does not document how it counts. A search on an
      index that is not ACTIVE is a `ValidationException`.
    - The catalog leader's `vector_index_worker` (gateway sweep loop)
      builds its own graphs of a new index before marking it ACTIVE, and
      drops DELETING ones. `ItemCount` and `IndexSizeBytes` read 0, as for
      GSIs. Not recorded against AWS: that needs Clark's go, and DynamoDB
      Local may not implement vector search.

- [ ] 27. Multi-region strong consistency (added 2026-11-13: Clark chose
  to build it in task 25)
  - Design first: a strongly consistent global table's ranges in Raft
    groups whose voters span three clusters (or two and a witness), and
    what that means for placement, splits, the forward link, transactions
    and the change log
  - `MultiRegionConsistency=STRONG` on `CreateTable` and `UpdateTable`
    replica updates, `GlobalTableWitnessUpdates`, and their descriptions
  - Strongly consistent reads in every region; writes refused, as AWS does,
    when the region cannot reach a quorum
  - Tests across three linked clusters, including losing a region
  - _Requirements: 18.3_

- [x] 22. Global tables, last-writer-wins
  - Peer cluster config, replicator and receiver, replica updates in
    `UpdateTable`, replica status, conflict tests with clock skew
  - _Requirements: 18.1, 18.2_
  - Notes:
    - Stamps (`item_state_machine.hpp`): a global table's write carries
      its time and region (command format 4, written only for regional
      commands) and keeps them in the item's meta value after the
      version, tombstones included. A `replicate` command applies only
      over an older stamp, compared by time then region name; a local
      write is stamped past the stored stamp, so a write made after seeing
      another wins everywhere whatever the clocks say. Change records
      carry the origin region (record format 2, written only when set).
    - Catalog (format 10: `region` and `replicas` after `vectors`, only
      for a global table): `set_replicas` (one add or remove at a time;
      CREATING, ACTIVE, DELETING, CREATION_FAILED) and
      `replica_progress` (the replicator's cursors per store stream, which
      only move forward).
    - `global_tables.hpp`. The peer API is `POST /kythira/replication` on
      the gateway, SigV4-signed for the peer's region with a replication
      principal's keys and authorized per action (`CreateTableReplica`,
      `DeleteTableReplica`, `UpdateTable`, `PutItem`/`DeleteItem`); the
      design's mTLS option was not needed. Peers are
      `KYTHIRA_DYNAMODB_REPLICA_PEERS` with `_REPLICA_ACCESS_KEY_ID`,
      `_REPLICA_SECRET_ACCESS_KEY` and `_REPLICA_CA`.
    - Deviation from "a replicator on each base group's leader": one
      replicator runs on the catalog leader with the other background
      workers and reads every group's change log streams, parents first,
      as the point-in-time archiver does; its cursors live in the catalog,
      so a new leader resumes them.
    - A new replica is copied with each item's stamp (range pages can
      carry stamps), then the other ACTIVE replicas are told of it and
      start it at their cursors for the creating cluster, and the creating
      cluster sends its whole log since the copy, any origin, before the
      replica goes ACTIVE; after that each cluster sends only its own
      region's writes. Deleted items are not copied, so under clock skew a
      write a delete beat can land on a new replica that hears of it first.
    - Refused or differing (`doc/unsupported.md`): `ReplicaUpdates`
      `Update` and the per-replica overrides, combining `ReplicaUpdates`
      with other changes, `DeleteTable` of a table with replicas, and
      multi-region strong consistency (task 25). Streams are not required.
    - `dynamodb_global_table_test`: clusters on `local_backend` linked
      through each other's front ends (signed and authorized as over
      HTTP): backfill and statuses, both directions, clock skew, ties,
      deletes, a three-region mesh with a write in flight while a region
      joins, replica deletion, an unreachable peer, a refused creation,
      the refusals, and the stored forms.

- [x] 23. Coverage close-out
  - Every remaining operation in the pinned model implemented or listed in
    `unsupported.md`; coverage test green
  - _Requirements: 2.1-2.3_
  - Notes:
    - Every operation of both pinned models is implemented except eight,
      all out of scope: the two replica auto scaling calls (Application
      Auto Scaling) and the six global tables version 2017.11.29 (legacy)
      calls. Decision: the legacy calls are not mapped onto version
      2019.11.21 tables, as AWS asks new tables not to use them and its
      own legacy calls do not see 2019.11.21 tables; the requirements'
      out-of-scope list now names them.
    - `dynamodb_model_coverage_test` now also fails on a row that is not
      out of scope, so nothing can be left waiting for a task.
    - `doc/invariants.md`: the 20 KB policy limit moved to the checked
      table (task 21 tests it); "at most 2 readers per stream shard" is
      listed as not enforced, AWS wording it as advice with no error.

### Phase 7: conformance and docs

- [x] 24. Conformance and chaos
  - Conformance jobs: AWS CLI, boto3, aws-sdk-java-v2, lock client, KCL 2.x
    and 3.x, Streams adapter, Terraform `aws_dynamodb_table`
  - Chaos: leader kills mid-GSI-propagation and mid-split with streams on;
    no lost or duplicated writes, index entries or stream records (Docker
    and rootless Podman)
  - Optional recording run against real AWS DynamoDB (**Clark's go**)
  - _Requirements: 20.4, 21.1, 21.4-21.6_
  - Notes:
    - Conformance: `tools/evidence/clients_on_kythira.sh` (was
      `kcl_on_kythira.sh`) runs all eight clients against a three-node
      gateway through the recording stub, and `check_client_runs.py`
      checks every recording. Terraform is a new client (apply, empty
      second plan, capacity change, destroy). The run found two bugs in
      the clients' own scripts (boto3 used the reserved word `blob`
      unescaped, and its batch writer put and deleted one key in a batch);
      real DynamoDB refuses both too, so the scripts were fixed.
    - The AWS recording run was not made: no go was asked for, as the
      existing evidence recordings already cover the clients' calls.
    - Splits: there were none without an admin call, so the chaos case
      could not happen. The item state machine now answers the host's
      size policy (exact scan, cached 10 s; cuts are partition hash
      buckets, so no item collection is cut) and the daemon runs it,
      splits only (`KYTHIRA_DYNAMODB_SPLIT_BYTES`, `_SPLIT_KEYS`,
      `_SPLIT_INTERVAL_MS`). Deviation: a split makes two groups, not up
      to ten: ten-way splits under load made 72 groups on three nodes and
      Raft timed out.
    - Group ids: the daemon had no allocator for new groups. Each voter
      takes ids from its own range, `(ordinal + 1) << 48` onward, saved in
      `group-ids` on RocksDB (`group_ids.hpp`), so two leaders never pick
      the same id with no coordination.
    - Restart after a split (found by the chaos run): a group whose state
      machine keeps its applied index does not replay the split after a
      restart, so the node came back with only its bootstrap groups and
      halted on "key outside the group's range". The host now reports
      every replica's layout synchronously in the apply path
      (`multi_raft_config::on_local_layout_changed`, core), the daemon
      saves it durably as `layout.json` (`group_layout.hpp`), and starts
      from it.
    - Chaos: `tools/chaos/` (`run_chaos.sh compose|native`,
      `chaos_driver.py`, `compose.yml`). Three RocksDB nodes split at
      512 KiB; each is SIGKILLed twice while writers fill a table with a
      GSI and a stream. The driver fails on any lost, duplicated or
      phantom item, GSI entry or stream record, and when no split
      happened. Native runs here: 8,427 and 8,949 writes, 16 and 14
      shards, every check passed. CI runs it under Docker and rootless
      Podman in the dynamodb-module workflow (`ci_dynamodb_defconfig`
      adds RocksDB).
    - Left open (core, not DynamoDB): the group-scoped quorum manager
      logs `provision_node failed` on every heartbeat once a peer is
      silent for 30 s; an overloaded run wrote millions of such lines.

- [x] 25. Documentation and decisions
  - `doc/dynamodb.md`: model, profile switches, limits, operations,
    unsupported list, deployment
  - Decide multi-region strong consistency (Requirement 18.3) with Clark
  - _Requirements: 18.3_
  - Notes:
    - `doc/dynamodb.md` covers the model (catalog and item groups, key
      layout, LSIs, GSIs, transactions, the change log, splits and the
      saved layout), deployment, the compatibility switches, limits,
      operations, what is unsupported, and the conformance and chaos runs.
      The module README's stale "one node, no Raft" note now points to it.
    - Multi-region strong consistency: Clark chose to build it
      (2026-11-13), as task 27. Until then `MultiRegionConsistency=STRONG`
      and `GlobalTableWitnessUpdates` answer `ValidationException`, and the
      docs list it as planned.
