# Resharding

Task 11 of `.kiro/specs/kinesis-compatible-streams/` (Requirements 10.1,
10.2 and 10.4). `SplitShard`, `MergeShards` and `UpdateShardCount`
(`UNIFORM_SCALING`) close parent shards and open children with lineage.
The catalog and the shards involved can live in different Raft groups,
so a reshard commits as one tier-1 transaction across them.

## What a reshard does

1. **The stream goes UPDATING.** This is a compare-and-set on the catalog
   record: only an ACTIVE stream can make the move, so only one reshard
   runs at a time. A second call meets `ResourceInUseException`, the same
   as on AWS.
2. **The plan is made from a linearizable read** of the catalog, taken
   after the stream went UPDATING. A replica that is behind therefore
   cannot hand the plan a stale shard table, and nothing else can change
   the table until the reshard ends.
3. **One transaction** writes three things:
   - `reshard_catalog_command` adds the children (new ordinals, with
     `ParentShardId` and, for a merge, `AdjacentParentShardId`) and marks
     the parents *closing*;
   - `close_shard_command` closes each parent;
   - `open_shard_command` opens each child at counter 0.

   An append that reaches a parent before the transaction lands in the
   parent. One that arrives during it waits out the transaction's key
   locks: the host's state machine is wrapped in
   `transactional_state_machine`, and the cluster retries
   `key_locked_exception` with backoff. One that arrives after finds the
   parent closed, and the service routes it again to the child that now
   owns its hash key. So no record is lost, and none lands out of order
   within a hash key.
4. **Ending sequence numbers.** The service proposes `close_shard_command`
   to each parent again. On a closed shard this changes nothing and
   answers the counter the shard ended at. `set_ending_counters_command`
   then writes those counters into the catalog, and the stream returns to
   ACTIVE. `ListShards` and `DescribeStream` show the parents with an
   `EndingSequenceNumber` from then on: one past the parent's last record,
   so no record carries it, as on AWS.

The children are new ordinals, which means new routing keys. Kythira's own
group splits and moves (`multi_raft`'s elastic policy) work on routing keys
and never see Kinesis lineage, and resharding never moves a group. The two
are independent (Requirement 10.4).

## Closed shards

A closed parent keeps its records and its segments. `GetRecords` serves
them until retention trims them (Requirement 10.2), the same as for an open
shard. A read that reaches the parent's end gets no `NextShardIterator` and
gets `ChildShards`, so KCL moves on to the children once it has finished
the parent.

## UpdateShardCount

`UNIFORM_SCALING` runs one reshard at a time towards the target:

- **Below the target:** split the widest open shard at its midpoint.
- **Above it:** merge the adjacent pair with the smallest combined width.

The open shards tile the hash key space after every step. AWS's bounds
apply: at most double and at least half the current count, and no more
than the account's shard limit.

## A reshard the caller abandons

The node that ran the reshard can die at any point. Every node's
maintenance pass calls `repair_reshards`, which looks for two cases:

- **A stream that has been UPDATING for a minute.** The reshard's
  transaction has settled by then, either committed or aborted. The pass
  calls `complete_reshard`, which fills in any ending counters and returns
  the stream to ACTIVE.
- **An ACTIVE stream with parents still marked closing.** This happens
  when the ending counters were never written. The pass writes them.

Both steps are idempotent, so two nodes repairing at once do no harm.
Before the transaction commits, nothing has changed apart from the status.
After it commits, the shards are already closed and opened, and only the
catalog's ending counters are left to write.

## Refusals

The service sends AWS's messages for each case, though the wording has
not been checked against a recording of AWS yet:

- a shard that does not exist;
- a shard that has already been split or merged;
- a `NewStartingHashKey` that is not strictly inside the parent's range;
- shards that are not adjacent;
- a stream that is not ACTIVE;
- an `UpdateShardCount` target outside half to double the current count.

## Tests

`kinesis_reshard_test` (5 cases) runs on the in-process cluster. It covers:

- splits and merges, with lineage, routing and readable parents;
- the refusals;
- `UpdateShardCount` reaching its target with the key space tiled;
- the repair finishing an abandoned reshard.

The container chaos case kills a leader in the middle of a reshard and
checks that no record is lost or reordered.
