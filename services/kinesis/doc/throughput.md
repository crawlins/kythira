# Throughput limits and hot shards per group

Spec task 8 and Requirement 12. This page covers how the limits are
enforced and what one Raft group sustained at the `aws` write limit.

## Enforcement

The `throttling` switch of the compatibility profile turns enforcement on
or off. It is on by default; a stream's tag overrides the cluster setting.

| Limit | Default | Where the bucket lives | Error |
| --- | --- | --- | --- |
| Writes, bytes | 1 MiB/s per shard (data plus partition key) | the group leader that proposes the append | `ProvisionedThroughputExceededException` |
| Writes, records | 1000 records/s per shard | the same leader | the same |
| `GetRecords` calls | 5/s per shard | the node that serves the call | the same |
| `GetRecords` bytes | 2 MiB/s per shard; one call may overdraw it, and the shard then answers throttled until the debt clears | the node that serves the call | the same |
| `GetShardIterator` | 5/s per shard | the node that serves the call | the same |

Each bucket holds one second of capacity and refills continuously, on a
monotonic clock. Buckets idle for a minute are dropped.

Writes:
- A record is admitted while the shard's byte bucket is still positive,
  and is then charged in full, so it can overdraw the bucket and later
  writes wait until the debt clears. Otherwise a 1 MiB record with its
  partition key could never fit a 1 MiB bucket, and AWS accepts one even
  on a shard written to in the last second (seen in the differential
  recordings, [differential.md](differential.md)).
- `PutRecords` admits the longest prefix of each shard's records that fits.
  The rest answer `ProvisionedThroughputExceededException` in place, with
  AWS's message: `Rate exceeded for shard shardId-… in stream S under
  account A.`
- `PutRecord` answers 400 with the same error when its record does not fit.
- A node that is not the leader forwards the append with the admission
  mode. The leader charges its own buckets, and a node that charged before
  learning it was not leader refunds the tokens.
- Buckets live in memory and reset on leader change, so a new leader can
  admit up to one extra second of traffic. AWS documents a similar burst
  tolerance.

Reads are node-local, because any replica serves `GetRecords`. A consumer
that spreads its calls over N nodes can therefore read up to N times the
per-shard read limit. The `GetRecords` message is AWS's: `Rate exceeded
for Shard - A:S:shardId-…`.

Not enforced yet: the per-account control-plane call rates (for example,
`DescribeStream` at 10 per second), which AWS answers with
`LimitExceededException`. `LimitExceededException` is returned for the
account's shard limit.

## Measurement: hot shards in one group

The question is how many shards one Raft group can hold at the full `aws`
write limit, with batching and group commit.

**Setup** (2026-11-10):
- Three `kinesis_gateway_node` processes, built at `-O1`, in one cloud
  sandbox: 4 vCPUs, 16 GB, one shared virtual disk.
- Plain HTTP and one Raft group (no `KYTHIRA_SHARD_CUTS`).
- Throttling on.
- One boto3 process per shard offers 1.25 MiB/s to its shard: 25 records
  of 10 KiB every 200 ms, `ExplicitHashKey` pinned to the shard, each call
  sent to the next node in turn.
- The driver is not in the tree. Each run lasts 30 s. The buckets start
  full, so an unconstrained shard reads 1.033 MiB/s over 30 s.

| Shards in the group | Accepted per shard (min / mean MiB/s) | Group total MiB/s | `PutRecords` p50 / p99 |
| --- | --- | --- | --- |
| 1 (15 s run) | 1.053 / 1.053 | 1.05 | 19 / 69 ms |
| 2 | 1.027 / 1.027 | 2.05 | 22 / 98 ms |
| 4 | 1.026 / 1.027 | 4.11 | 24 / 242 ms |
| 8 | 0.928 / 0.934 | 7.47 | 43 / 1138 ms |
| 16 | 0.424 / 0.445 | 7.12 | 115 / 4865 ms |

**Result:** one group holds **4 hot shards** at the `aws` write limit on
this machine. At 8 shards it delivers 93% of the limit per shard. Past
that, the group total stays near 7.5 MiB/s and latency grows.
- The ceiling is the disk, not the CPU. During the 8-shard run the host
  showed about 24% I/O wait and 50% idle CPU.
- All three replicas fsync the Raft log and their segments to the same
  virtual disk, so each accepted MiB costs about six MiB of synchronous
  writes on this host.
- Nodes on separate disks should hold more. That needs a re-run on real
  instances, which is real-cloud spend and waits for approval.
- Leader memory peaked at about 1.1 GB RSS during these runs and stayed
  there in a later run, so it is allocator high-water, not growth.

The tiling guidance follows from this: plan for **at most 4 shards at the
full write limit per group**, cut with `KYTHIRA_SHARD_CUTS` today and with
the elastic split policy once resharding (task 11) lands. Shards below the
limit share a group in proportion.
