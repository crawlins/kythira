# On-demand capacity mode

Task 12 of `.kiro/specs/kinesis-compatible-streams/` (Requirement 10.3).

## UpdateStreamMode

`UpdateStreamMode` switches a stream between `PROVISIONED` and
`ON_DEMAND`. It is one catalog entry, `set_stream_mode_command`, and it
leaves the shards as they are.

The call is refused in these cases:

- **The stream is not ACTIVE.** This is `ResourceInUseException`, the same
  as the other control-plane calls.
- **A third switch within 24 hours.** AWS allows two switches in 24 hours.
  The catalog keeps the times of the last two switches, and a third switch
  inside the window gets `LimitExceededException`.
- **A switch to `ON_DEMAND` past the account's on-demand stream limit.**
  The limit is 50 by default, as on AWS, and the refusal is
  `LimitExceededException`.
- **A `StreamMode` outside the model's enum.** The front end refuses it with
  `ValidationException`.
- **`WarmThroughputMiBps`.** This gets `InvalidArgumentException`. AWS
  unlocks warm throughput with an account billing commitment, and Kythira
  has nothing like that. `UpdateStreamWarmThroughput` is unsupported for
  the same reason.

Asking for the mode the stream already has succeeds, changes nothing, and
does not count as a switch.

`SplitShard`, `MergeShards` and `UpdateShardCount` are for provisioned
streams only, as AWS documents. On an `ON_DEMAND` stream they get
`ValidationException`. The check runs inside the reshard, against the
catalog the reshard locked, so a mode switch cannot slip in between.

The exact wording of these messages, and the error code of the
provisioned-only refusal, are ours. They have not been checked against a
recording of AWS yet.

## The capacity controller

`kinesis_on_demand.hpp`'s `on_demand_controller` runs on every node in the
maintenance pass (`KINESIS_MAINTENANCE_INTERVAL_MS`).

1. **Sampling.** For each open shard of each ACTIVE on-demand stream, the
   node that leads the shard's group samples two values from the shard's
   state: the last counter, which gives records, and the retained bytes.
   This is the node where the shard's writes are admitted.
2. **Hot shards.** A rate is measured over a window of at least 10 seconds.
   A shard is hot when it takes half of a shard's write limit (512 KiB/s or
   500 records/s) or more.
3. **Splitting.** A shard that stays hot for 60 seconds is split at the
   middle of its hash range, through the same transaction a `SplitShard`
   uses (see [resharding.md](resharding.md)). Each split adds one shard's
   worth of write capacity.

The controller has these bounds:

- **One split per stream per pass.** If another node is resharding the
  same stream, the split meets the UPDATING lock and is tried again on a
  later pass.
- **A ceiling on shards.** There is no split past 200 open shards in a
  stream, or past the account's shard limit. AWS's default on-demand
  ceiling is 200 MiB/s of writes per stream.
- **No merges.** AWS sizes an on-demand stream for twice its peak over the
  last 30 days. Capacity that shrank as soon as traffic dropped would fall
  short of that.
- **Samples live in memory.** A new leader starts sampling from scratch,
  which costs one sustain period (60 seconds) before it splits.

With throttling on, a shard's accepted rate stops at its limit, and that
is above the hot threshold. So a throttled on-demand shard gets split
too, and the throttling stops once the children share its keys.

## Tests

`kinesis_on_demand_test` has 4 cases:

- the mode switch and its refusals;
- the provisioned-only resharding calls;
- the controller splitting a shard that stays hot, by records and by bytes,
  while it leaves quiet shards and provisioned streams alone;
- no split once a stream is back in provisioned mode.

A local three-node run with the catalog and the shards in separate groups
split a shard taking 700 records/s about 65 seconds into the load. That is
one 10-second window plus the 60-second sustain.
