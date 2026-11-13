# KCL lease coordination

KCL keeps one lease per shard (and a few of its own bookkeeping items) and
coordinates its workers by conditional writes on them. On AWS those leases
live in a DynamoDB table named after the application. A Kythira Kinesis
cluster can hold them itself, so a KCL application needs nothing but the
cluster (`.kiro/specs/kinesis-compatible-streams/` task 9, Requirement 15).

## Which backend supports which KCL version

| Lease backend | KCL 2.x | KCL 3.x |
|---|---|---|
| Kythira native lease layer (`java/kythira-kcl-leases`) | yes (built and tested against 2.7.4) | no: KCL 3.x hard-wires its DynamoDB DAOs (worker metrics, coordinator state), so a refresher alone cannot move it |
| Kythira's DynamoDB-compatible API (`dynamodb_gateway_node`) | yes (2.7.4) | yes (3.5.4) |
| DynamoDB Local | yes (2.7.4, against 3.3.2) | yes (3.5.4, against 3.3.2) |
| AWS DynamoDB | expected to work (KCL's own backend), not run here: a run is real-cloud spend and needs Clark's go | the same |

The DynamoDB rows are KCL's own lease management, unchanged: point its
DynamoDB client at the endpoint, and it creates and uses its lease table
there (KCL 3.x also its worker metrics and coordinator state tables, and
the lease table's `LeaseOwnerToLeaseKeyIndex` GSI). The container
scenario's `kcl_2_and_3_keep_their_leases_in_dynamodb` case runs all four
Kythira and DynamoDB Local combinations on one stream: every record is
processed, and after a restart each worker carries on from the checkpoints
its table kept (task 16). The sample consumer (`tests/kcl/sample`, and
`tests/kcl/sample3` for KCL 3.x) shows the wiring: `KCL_LEASES=dynamodb`
with `KCL_DYNAMODB_ENDPOINT`.

## The Lease API

REST on every `kinesis_gateway_node` listener, under
`/v1/leases/<application>/` (`include/kythira/kinesis/kinesis_lease_api.hpp`
has the full table):

- `GET`, `PUT`, `PATCH` and `DELETE` on `/v1/leases/<app>/<key>`, with
  `If-Match: <version>` and `If-None-Match: *` as the conditions;
- `GET /v1/leases/<app>/?prefix=&start-after=&limit=` lists in key order;
- `DELETE /v1/leases/<app>/` drops the application's leases.

A lease's version is the Raft log index of the write that produced it, so
it only grows, and comes back as the `ETag`. A failed condition answers 412
`ConditionalCheckFailedException` with the current version.

Leases live in the catalog group. Reads go through its log too, so a read
sees every write acknowledged before it on any node: KCL's lease taker
needs that, because it decides from what it reads whether a lease has
expired. A lease is at most 64 KiB; a list page at most 1000 leases or
1 MiB.

Requests are signed with SigV4 for the `kinesis` service and authorized
with the DynamoDB actions KCL needs on AWS, on the table it would create
there (`arn:<partition>:dynamodb:<region>:<account>:table/<application>`):
`GetItem` and `Scan` to read, `PutItem`, `UpdateItem` and `DeleteItem` to
write, `DeleteTable` to drop the application. An IAM policy written for KCL
on AWS works unchanged; with static keys, read-only keys read and
read-write keys write, and only admin keys drop an application.

## The Java lease layer

`services/kinesis/java/kythira-kcl-leases` is a Gradle library
(`io.kythira:kythira-kcl-leases`, Java 17 bytecode). KCL is a
`compileOnly` dependency: the application brings its own KCL 2.x.

- `KythiraLeaseRefresher` replaces KCL's `DynamoDBLeaseRefresher`. Leases
  are stored as the DynamoDB items KCL's own `LeaseSerializer` produces, so
  every field KCL keeps (owner, counter, checkpoints, parent shards, hash
  range) round-trips exactly. KCL's legacy `Expected` and
  `AttributeUpdates` conditions are checked against the stored item and
  applied client-side, then written with the version the item was read at;
  a lost race re-reads and re-checks.
- `KythiraLeaseManagementFactory` is KCL's `DynamoDBLeaseManagementFactory`
  with that refresher: lease coordinator, taker, renewer, shard sync and
  cleanup are all KCL's own. The checkpointer is KCL's too, writing through
  the refresher.
- `HttpLeaseApi` is the client: `java.net.http` with the AWS SDK's SigV4
  signer, credentials from any `AwsCredentialsProvider`. Reads retry; writes
  do not, because KCL retries them with its own conditions.

Install it on the `LeaseManagementConfig` before building the scheduler:

```java
ConfigsBuilder configs = new ConfigsBuilder(streamTracker, application, kinesis,
        dynamo, cloudWatch, workerId, processorFactory);
LeaseManagementConfig leases = configs.leaseManagementConfig();
LeaseApi api = new HttpLeaseApi(endpoint, application, credentials, region);
leases.leaseManagementFactory(KythiraLeaseManagementFactory.create(leases, api, false));
```

KCL still insists on a `DynamoDbAsyncClient` and a `CloudWatchAsyncClient`;
the lease layer never calls the first, and `NullMetricsFactory` keeps KCL
from calling the second. `tests/kcl/sample/` is a complete worker.

## Tests

- `kinesis_leases_test` covers the records and the REST API: conditions,
  versions, patches, pages, routing and authorization.
- `KythiraLeaseRefresherTest` (`gradle test`, the Kinesis Module
  workflow's java job) runs the refresher against an in-memory Lease API,
  including racing writers and KCL's own expected-value updates.
- The container scenario's `kcl_workers_keep_one_owner_per_shard_through_node_kills`
  case runs three sample workers on a four-shard stream while each node is
  killed in turn (so the catalog group's leader is killed while workers
  renew), and checks that every record arrives and that no shard has two
  owners for longer than the failover time.
