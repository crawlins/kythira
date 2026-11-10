# Tags, resource policies, monitoring and account calls

Task 15 of `.kiro/specs/kinesis-compatible-streams/` (Requirements 2.1,
14.2-14.5 and 16.2). With it, every operation in the pinned model is either
implemented (`../implemented-operations.txt`) or listed in
[unsupported.md](unsupported.md) with its reason and error. The ones still
listed are:

- the Channel operations, which are not in the spec;
- enhanced fan-out, which is task 13;
- encryption, which is task 14;
- `UpdateStreamWarmThroughput`.

## Tags

There are two forms, and both change the stream's tags through one catalog
entry, `tag_stream_command`:

- **The stream form** names the stream with `StreamName` or `StreamARN`:
  `AddTagsToStream`, `RemoveTagsFromStream` and `ListTagsForStream`.
- **The resource form** names it with `ResourceARN`: `TagResource`,
  `UntagResource` and `ListTagsForResource`.

The rules:

- **Adding overwrites.** Adding a key that is already there replaces its
  value. Removing a key that is not there does nothing.
- **At most 50 tags per stream.** This is AWS's limit. A change that would
  leave more gets `LimitExceededException`. The state machine checks the
  limit again when the entry applies, so two racing calls cannot both get
  past it.
- **Paging.** `ListTagsForStream` returns tags in key order, starting after
  `ExclusiveStartTagKey`, up to `Limit` of them (10 when it is absent). It
  sets `HasMoreTags` when more remain. `ListTagsForResource` returns them
  all.
- **Profile switches.** A `kythira:compat:*` tag is a profile switch. It is
  checked as it is at `CreateStream`, and it takes effect on the stream's
  next request.
- **Consumer ARNs.** A consumer ARN in `ResourceARN` gets
  `ResourceNotFoundException` until enhanced fan-out (task 13) creates
  consumers.
- **Stream status.** The stream must be ACTIVE or UPDATING.

## Resource policies

`PutResourcePolicy`, `GetResourcePolicy` and `DeleteResourcePolicy` store a
stream's policy in its catalog record (`set_policy_command`).

- **Storing a policy.** The policy has to be a JSON object with a
  `Statement`, and at most 20 KiB. It is stored as given.
- **Applying it.** The front end's resource lookup hands the policy, and
  the stream's tags, to the authorizer on every later request that names
  the stream. With IAM, that is IAM's evaluator, which applies the
  cross-account and explicit-deny rules. `static_key_authorizer` ignores
  policies.
- **Getting it.** `GetResourcePolicy` returns `{}` for a stream with no
  policy.
- **Deleting it.** `DeleteResourcePolicy` on a stream with no policy gets
  `ResourceNotFoundException`.

## Enhanced monitoring

`EnableEnhancedMonitoring` and `DisableEnhancedMonitoring` switch shard-level
metrics on and off. `ALL` stands for all seven. The answer carries the
switches before and after the call (`CurrentShardLevelMetrics`,
`DesiredShardLevelMetrics`). `DescribeStream` and `DescribeStreamSummary`
show them under `EnhancedMonitoring`.

## Metrics

With `KINESIS_METRICS=true`, each gateway serves its metrics on
`GET /metrics` in the Prometheus text format. The endpoint has no
authentication, like `/healthz`, so that a scraper can reach it. It names
streams and accounts, which is why it is off by default.

- **What a node counts.** Each node counts what it served, so a cluster's
  totals are the sum over its nodes. A write forwarded to the leader is
  counted once, on the node the client called.
- **Stream-level series** are always there:
  - bytes and records in, and bytes and records out;
  - write and read throttles;
  - iterator age;
  - `GetRecords` latency;
  - segment bytes, local and tiered;
  - fan-out subscriptions, which are 0 until task 13.
- **Shard-level series.** The traffic series appear per shard only for the
  metrics switched on for the stream, as CloudWatch's shard-level metrics
  do. Segment bytes are always per shard.

`kinesis_metrics.hpp` lists every series.

## Account calls

Kythira has no billing commitment:

- `DescribeAccountSettings` reports `MinimumThroughputBillingCommitment` as
  `DISABLED`.
- `UpdateAccountSettings` accepts `DISABLED` and refuses `ENABLED` with
  `InvalidArgumentException`.
- `UpdateMaxRecordSize` accepts 1024 KiB, the default and the only size
  this endpoint stores. Any other size gets `InvalidArgumentException`.

## Not yet checked against AWS

The wording of these messages, the default `Limit` of `ListTagsForStream`,
and `GetResourcePolicy`'s answer for a stream without a policy follow AWS's
documentation as we read it. None of them has been checked against a
recording of AWS yet.

## Tests

`kinesis_tags_test` has 6 cases:

- stream tags, with paging, the limit and profile tags;
- resource tags, with the tags reaching authorization;
- policies stored and handed to authorization;
- the monitoring switches;
- the account calls and record size;
- the metrics, following the switches.
