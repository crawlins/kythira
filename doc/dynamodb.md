# DynamoDB

Kythira's DynamoDB module (`services/dynamodb/`,
`.kiro/specs/dynamodb-compatible-api/`) speaks the DynamoDB and DynamoDB
Streams APIs (AWS JSON 1.0, API version 2012-08-10). The AWS SDKs, the AWS
CLI, the DynamoDB lock client, KCL 2.x and 3.x with the Streams adapter, and
Terraform's `aws_dynamodb_table` work against it unchanged with
`--endpoint-url`. Tables live in Kythira's Raft groups, which split as they
grow.

This page covers the model, running a cluster, the compatibility switches,
limits, what is implemented and what is not. The module's layout, file by
file, is in `services/dynamodb/README.md`.

## Model

A cluster is three or five `dynamodb_gateway_node` voters. Each runs a
`multi_raft` host with one RocksDB database for every group it holds, and
serves the API on every node: a request can go to any node, which hands it
to the right group's leader over the forward link.

- **The catalog group** holds table descriptors, their status, table and
  index ids (never reused), and background jobs: deletes, index backfills,
  exports, imports, restores.
- **Item groups** each own a contiguous key range. An item's key is
  `t/<table id>/<h16(partition key)><partition key><sort key>`, where `h16`
  is two bytes of the partition key's SHA-256. Partitions spread evenly
  over the hash space, and one partition's items (an item collection) stay
  together, so a `Query` is one range read on one group and parallel `Scan`
  segments are hash slices.
- **Local secondary index** entries sit inside the item collection, so they
  are written in the item's own Raft entry and are always consistent.
- **Global secondary indexes** have their own key spaces, usually in other
  groups. A write leaves a pending record beside the item, and the
  propagator carries it to the index's group, so index reads
  are eventually consistent, as on AWS.
- **Transactions** (`TransactWriteItems`, `TransactGetItems`) run across
  groups with Kythira's multi-Raft transactions, with AWS's conflict and
  idempotency rules.
- **The change log** records every write in its group. It feeds DynamoDB
  Streams, Kinesis streaming destinations, point-in-time recovery, global
  table replication and vector index graphs. A split closes the parent's
  stream and opens one per child, which DynamoDB Streams shows as shard
  lineage.

A consistent read takes a read barrier on the group leader. An eventually
consistent read may be answered by any replica. Conditions and update
expressions are evaluated when the entry applies, so they see every write
before them.

### Splits

A group splits when it passes 96 MiB or 960,000 keys, at most once an hour
per group by default. The cut is a hash bucket boundary, so it never splits
an item collection. Each split halves a group: one cut per split keeps the
group count growing gradually under load. Merges are off.

Each voter allocates new group ids from its own range (`(ordinal + 1) <<
48` onward, ordinal being its place in the sorted peer list), so two
leaders splitting at once never collide. Each node saves its group layout
(`<data dir>/layout.json`) in the apply path of every split and restarts
from it. Group state survives a restart in RocksDB, so nothing replays the
splits; without the file a restarted node would come back with only its
bootstrap groups.

## Deployment

Build with `CONFIG_DYNAMODB=y` and `CONFIG_ENGINE_ROCKSDB=y`
(`configs/ci_dynamodb_defconfig` has both). Every node takes its settings
from the environment; `dynamodb_gateway_node --help` lists them all.

| Setting | What |
|---|---|
| `KYTHIRA_DYNAMODB_NODE_ID` | this node's id |
| `KYTHIRA_DYNAMODB_PEERS` | `id=http://host:port,...`: every voter's Raft address, this node's included |
| `KYTHIRA_DYNAMODB_RAFT_BIND`, `_RAFT_PORT` | the Raft listener (default port 7000) |
| `KYTHIRA_DYNAMODB_PEER_GATEWAYS` | `id=host:port,...`: every other voter's API address, for forwarded requests |
| `KYTHIRA_DYNAMODB_FORWARD_SECRET` | the HMAC key gateways sign forwarded requests with (16 or more characters, the same on every node) |
| `KYTHIRA_DYNAMODB_ENGINE=rocksdb`, `KYTHIRA_DYNAMODB_DATA_DIR` | where the database, the Raft logs, `layout.json` and `group-ids` live |
| `KYTHIRA_DYNAMODB_TLS_CERT`, `_TLS_KEY` | TLS for the API (port 8000 by default) |
| `KYTHIRA_DYNAMODB_STATIC_KEYS_FILE` | access keys, `id:secret:account:name:role` per line, when IAM is not used |
| `KYTHIRA_DYNAMODB_REGION`, `_PARTITION` | the region and ARN partition answers carry (`us-east-1`, `aws`) |
| `KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES` | writes are refused below this much free space (1 GiB) |
| `KYTHIRA_DYNAMODB_SPLIT_BYTES`, `_SPLIT_KEYS`, `_SPLIT_INTERVAL_MS` | the split policy; both thresholds 0 turns splits off |
| `KYTHIRA_DYNAMODB_COMPAT` | the cluster's compatibility profile (below) |

Without `PEERS` the daemon runs one node with no Raft on the memory
engine, like DynamoDB Local's `-inMemory`: good for tests, nothing kept.

Backups, exports and imports need a store every node can reach:
`KYTHIRA_DYNAMODB_BACKUP_DIR` (a shared directory) or, in a build with the
AWS SDK, `KYTHIRA_DYNAMODB_BACKUP_S3_BUCKET` (with `_PREFIX`, `_REGION`,
`_ENDPOINT`). Kinesis streaming destinations publish to AWS Kinesis or to
`KYTHIRA_DYNAMODB_KINESIS_ENDPOINT` (a Kythira `kinesis_gateway_node`, for
example). Global tables need `KYTHIRA_DYNAMODB_REPLICA_PEERS` and a
replication principal's keys; `services/dynamodb/README.md` shows the
setup.

`services/dynamodb/tools/chaos/compose.yml` is a working three-node
cluster on RocksDB under Docker or rootless Podman.

## Compatibility profile

By default every behaviour is AWS's. An operator relaxes a behaviour for
the cluster with `KYTHIRA_DYNAMODB_COMPAT=switch=value,...`, or for one
table with the tag `kythira:compat:<switch>`; the table's value wins. No
switch changes an error code, message or status: it only changes whether
the limit or delay applies.

| Switch | AWS value | Relaxed value | Covers |
|---|---|---|---|
| `limits` | `aws` | `relaxed` | item, key, page, batch and transaction sizes |
| `throttling` | `on` | `off` | provisioned capacity enforcement and on-demand throttling |
| `capacity-reporting` | `on` | `off` | `ConsumedCapacity` and `ItemCollectionMetrics` in answers |
| `gsi-consistency` | `async` | `sync` | GSI propagation |
| `count-limits` | `aws` | `relaxed` | table and index counts |
| `ttl-delay` | `aws` | `prompt` | the TTL deletion window |

An unknown switch or value is refused, so a typo in a tag fails at
`TagResource` or `CreateTable` instead of silently keeping AWS's
behaviour.

## Limits

AWS's quotas apply as documented: 400 KB items, 2048-byte partition keys
and 1024-byte sort keys, 1 MB `Query` and `Scan` pages, 100 items per
transaction and per `BatchGetItem`, 25 per `BatchWriteItem`, 16 MB batch
answers, 10 GB item collections on tables with LSIs, 20 GSIs and 5 LSIs per
table, and nesting up to 32 levels. Every rule this module checks is listed
with its AWS source page and its test in
`services/dynamodb/doc/invariants.md`; the expectations were also run
against DynamoDB Local 3.3.2. Throttling is on, at a provisioned table's
capacity and at AWS's on-demand defaults; each gateway node enforces its
share (one over the number of voters).

## Operations

54 DynamoDB operations and the four DynamoDB Streams operations are
implemented; `services/dynamodb/include/kythira/dynamodb/operations.hpp`
lists them. That covers the control plane, items, `Query` and `Scan`,
batches, transactions, PartiQL, TTL, Streams, Kinesis streaming
destinations, backups, point-in-time recovery, exports and imports, bulk
puts, resource policies, Contributor Insights, vector indexes with
`SearchVectors`, and global tables (version 2019.11.21) with
last-writer-wins replication.

Every request is SigV4-signed and authorized per action and resource, by
IAM's evaluator or the static-key roles. Answers match AWS's wording.
Expressions were checked against about 3000 cases recorded from DynamoDB
Local, and the operations corpus against recorded scenarios.

## What is not supported

`services/dynamodb/doc/unsupported.md` is the full list. Eight operations
are out of scope and answer `UnknownOperationException`:

- the six global tables version 2017.11.29 (legacy) calls;
- the two replica auto scaling calls, which belong to Application Auto
  Scaling.

Parts of supported operations are also refused. Among them are
`ReplicaUpdates` `Update` and the per-replica overrides.

### Multi-region strong consistency

`MultiRegionConsistency=STRONG` and `GlobalTableWitnessUpdates` are planned
(Requirement 18.3, task 27; decided with Clark on 2026-11-13). Until that
lands they answer `ValidationException`, and global tables are eventually
consistent across regions with last-writer-wins.

What it takes: each write must commit in a quorum that spans clusters, as
AWS does with three regions (or two and a witness), so every write pays a
cross-region round trip. In Kythira that means Raft groups whose voters sit
in different clusters for a strongly consistent table's ranges. Group
placement, splits, the forward link and transactions all assume today that
a group's voters are in one cluster, so task 27 starts with a design.

## Testing it

- The module's unit and in-process cluster tests run in CI
  (`dynamodb_*_test`).
- `services/dynamodb/tools/evidence/clients_on_kythira.sh` runs the eight
  conformance clients (the AWS CLI, boto3, aws-sdk-java-v2, the lock
  client, KCL 2.x and 3.x, the Streams adapter and Terraform) against a
  three-node cluster and checks every exchange.
- `services/dynamodb/tools/chaos/run_chaos.sh` SIGKILLs each node of a
  RocksDB cluster in turn while writers fill a table that has a GSI and a
  stream, and groups split. It fails on any lost or duplicated write,
  index entry or stream record.

The `dynamodb-module` workflow runs both, under Docker and under rootless
Podman.
