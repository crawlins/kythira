# Kythira Kinesis

Task 18 of `.kiro/specs/kinesis-compatible-streams/` (Requirements 8.4 and
15.4). This page covers running the Kinesis Data Streams-compatible service
and what to expect from it. The other pages in this directory go deeper on
one subject each; this one links to them.

## What it is

`kinesis_gateway_node` is a daemon that speaks Kinesis Data Streams' API.
Unmodified clients point their endpoint at any node of a cluster:

- the AWS CLI;
- the AWS SDKs, over JSON or CBOR;
- KPL;
- KCL 2.x, with the lease layer in [leases.md](leases.md);
- the Flink AWS connector's Kinesis source and sink (5.x).

Every node takes every call. A write is forwarded to the leader of its
shard's Raft group; a read is served from the receiving node's own replica.

## The model

- **Streams and shards.** A stream is a catalog entry. A shard is a
  routing key in one of the cluster's Raft groups, not a group of its own,
  so a cluster with a few groups holds thousands of shards. Hash keys are
  routed by MD5, as on AWS.
- **Records.** Each record is proposed through its shard's group, and is
  stored once committed in the core record segment store, outside the Raft
  snapshot. Every replica writes byte-identical segments.
- **Sequence numbers.** They come from the shard's counter in the log, so
  they are dense, ordered and identical on every replica. See
  [sequence-numbers.md](sequence-numbers.md).
- **Resharding.** `SplitShard`, `MergeShards` and `UpdateShardCount` close
  parents and open children in one cross-group transaction. See
  [resharding.md](resharding.md).
- **Retention.** The leader trims sealed segments past the stream's
  retention, and can move older segments to S3. See
  [retention.md](retention.md).

## The clock assumption

`ApproximateArrivalTimestamp` comes from the shard leader's wall clock, and
never goes backwards within a shard. Run NTP or chrony on every node. A
leader whose clock runs ahead pulls later records' timestamps forward until
real time catches up; `AT_TIMESTAMP` iterators and `MillisBehindLatest` are
off by as much in that window. Order is never affected, since it comes from
sequence numbers. The details are in
[sequence-numbers.md](sequence-numbers.md#arrival-timestamps-and-the-clock-assumption).

## Compatibility switches

By default the service behaves as AWS does. An operator can relax five
behaviours for the whole cluster with `KINESIS_COMPAT` (`k=v,...`), and a
stream can override the cluster with a tag `kythira:compat:<switch>`:

| Switch | AWS value | Other value | What it changes |
|---|---|---|---|
| `throttling` | `on` | `off` | per-shard write and read rates, and fan-out pacing |
| `limits` | `aws` | `relaxed` | the 5 MiB `PutRecords` batch and the 10 MiB `GetRecords` page (64 MiB relaxed) |
| `count-limits` | `aws` | `relaxed` | the account's shard and on-demand stream counts, and 20 consumers per stream (1000 relaxed) |
| `iterator-lifetime` | `aws` | `relaxed` | shard iterators expire after 5 minutes (a day relaxed) |
| `id-format` | `aws` | `compact` | the printed form of sequence numbers and shard ids |

No switch changes an error code, message or status. An unknown switch or
value is refused when the tag is set, rather than ignored.

## Limits

| Limit | Value | Notes |
|---|---|---|
| Record | 1 MiB, data plus partition key | always |
| `PutRecords` | 500 records; 5 MiB | the count always (the model's bound); the size with `limits` |
| `GetRecords` | 10 000 records; 10 MiB | the count always (the model's bound); the size with `limits` |
| Writes per shard | 1 MiB/s and 1000 records/s | `throttling`; enforced by the shard's leader |
| `GetRecords` per shard | 5 calls/s, 2 MiB/s | `throttling`; enforced per node |
| `GetShardIterator` per shard | 5 calls/s | `throttling`; enforced per node |
| Fan-out per consumer and shard | 2 MiB/s | `throttling` |
| Open shards per account | 500 in us-east-1, us-west-2 and eu-west-1; 200 elsewhere | `count-limits` |
| On-demand streams per account | 50 | `count-limits` |
| Consumers per stream | 20 | `count-limits` |
| Tags | 50 per stream or consumer | always |
| Resource policy | 20 KiB | always |
| Retention | 24 hours to 365 days | always |
| Shard iterator | 5 minutes | `iterator-lifetime` |
| Subscription | 5 minutes | always |

Read limits are kept per node, since any replica serves reads. A client
that spreads its reads over N nodes can read up to N times a shard's rate.
The per-account control-plane call rates (for example `DescribeStream` at
10 per second) are not enforced. See [throughput.md](throughput.md).

## Lease backends by KCL version

KCL coordinates its workers through a lease table, which KCL keeps in
DynamoDB. These are the choices:

| Lease backend | KCL 2.x | KCL 3.x |
|---|---|---|
| Kythira's native lease layer (`java/kythira-kcl-leases`) | yes, tested against 2.7.4 | no: KCL 3.x reaches DynamoDB directly for more than leases |
| Kythira's DynamoDB-compatible API | yes, tested against 2.7.4 | yes, tested against 3.5.4 |
| DynamoDB Local | yes, tested against 2.7.4 | yes, tested against 3.5.4 |
| AWS DynamoDB | yes, run against AWS with 2.7.4 | yes, run against AWS with 3.5.4 |

Both polling and enhanced fan-out work with the native lease layer. With
a DynamoDB backend KCL's own lease management runs unchanged, pointed at
that endpoint. See [leases.md](leases.md) for installing the lease layer
and for the DynamoDB backends, and [fan-out.md](fan-out.md) for enhanced
fan-out.

## Deploying

### A cluster

Run one `kinesis_gateway_node` per host, three or five of them. Each needs:

- its Raft identity and peers: `KYTHIRA_NODE_ID`, and `KYTHIRA_PEERS` as
  `id=http://host:port,...` listing every voter, itself included;
- a data directory on a local disk, `KINESIS_DATA_DIR`, for the Raft state
  and the record segments;
- the API listener, `KINESIS_LISTEN` (`0.0.0.0:4567` by default), with
  `KINESIS_TLS_CERT` and `KINESIS_TLS_KEY`;
- access keys, `KINESIS_STATIC_KEYS_FILE`, one line per key:
  `id secret account name role`, where the role is `read-only`,
  `read-write` or `admin`;
- a secret the gateways share, `KINESIS_FORWARD_SECRET` or
  `KINESIS_FORWARD_SECRET_FILE`, which signs forwarded writes and, unless
  `KINESIS_ITERATOR_KEYS_FILE` names keys of their own, shard iterators.

`--help` lists every setting, and `--check-config` prints what the
environment resolves to. Set `KYTHIRA_SHARD_CUTS` to split the key space
into several groups from the start. `k/` puts the catalog in one group and
every shard in another.

### Networking

- **The API.** Clients reach every node's API listener. Nodes reach each
  other's API listeners too, for forwarded writes and segment fetches:
  `KINESIS_PEER_GATEWAYS` maps each node id to its address when it is not
  the Raft host with the same port. `KINESIS_PEER_CA` names the CA that
  signed the peers' certificates.
- **Raft.** Raft RPC is plaintext. Keep it on a private network, and set
  `KYTHIRA_ALLOW_PLAINTEXT_RAFT=true` to say so when it is not on
  loopback.
- **HTTP/2.** The API listener also speaks HTTP/2, which the Java SDK
  uses for `SubscribeToShard`: through ALPN over TLS, or with prior
  knowledge in plaintext. A load balancer in front must pass HTTP/2
  through.
- **Plaintext.** `KINESIS_ALLOW_PLAINTEXT=true` serves the API without
  TLS. It is for tests: signatures and data cross the network in the clear.

### Optional features

| Feature | Settings | Page |
|---|---|---|
| Prometheus metrics on `GET /metrics` (unauthenticated) | `KINESIS_METRICS=true` | [tags-policies-metrics.md](tags-policies-metrics.md) |
| Encryption at rest | `KINESIS_KMS=local` with `KINESIS_KMS_PASSPHRASE[_FILE]`, or `KINESIS_KMS=aws` with `KINESIS_KMS_REGION` and AWS credentials | [encryption.md](encryption.md) |
| Tiering to S3 | `KINESIS_TIER_AFTER_HOURS` (or `_MS`), `KINESIS_TIER_S3_BUCKET`, `_PREFIX`, `_REGION`, `_ENDPOINT` | [retention.md](retention.md) |
| Disk floor | `KINESIS_DISK_FLOOR_BYTES` (1 GiB by default, 0 turns it off) | [retention.md](retention.md) |

### Containers

`docker/kinesis_gateway_node/Dockerfile` packages a host-built daemon.
`tests/docker/compose.yml` is a working three-node example: nodes
addressed by service name, no static addresses, no privileges. It runs
under Docker and rootless Podman.

### Operating it

- **A node lost.** The others keep taking writes and serving reads while a
  majority of each group is up. A node that comes back catches up from its
  peers, including the segments it missed.
- **Disks.** Below `KINESIS_DISK_FLOOR_BYTES` of free space a node refuses
  writes it would propose or lead, with an error that says why.
- **Clients.** Point clients at all nodes, or at a load balancer over
  them. Any node answers any call.
- **KPL.** KPL calls STS `GetCallerIdentity` before its first write, to
  build the `StreamARN` it sends. Set `StsEndpoint` and `StsPort` to a
  node, as well as `KinesisEndpoint` and `KinesisPort`: every node answers
  `GetCallerIdentity` (signed for `sts`) from the caller's own signature,
  and refuses every other STS action. Left unset, KPL asks
  `sts.<region>.amazonaws.com` even with a custom Kinesis endpoint. KPL
  speaks HTTPS only, so the nodes need TLS; the container scenario puts a
  TLS front before plaintext nodes instead (`tests/kpl/kpl_produce.sh`).
  KCL deaggregates what KPL aggregates, through any node.

## Not supported

`doc/unsupported.md` lists every operation of the pinned model that is not
implemented, with the error it returns (Requirement 2.1). Today that is
the five Channel operations, which deliver records to S3 and are outside
the spec, and `UpdateStreamWarmThroughput`. `tools/check_coverage.py`
keeps the list complete. Other differences from AWS are noted on each
subject's page under "Limits and differences" or "Not yet checked against
AWS".
