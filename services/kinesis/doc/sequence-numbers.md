# Sequence numbers and arrival timestamps

This page covers Requirements 8.1-8.4 of
`.kiro/specs/kinesis-compatible-streams/`. The code is in
`include/kythira/kinesis/kinesis_sequence.hpp` and
`include/kythira/kinesis/kinesis_stream_state_machine.hpp`.

## Where a sequence number comes from

Each Kinesis shard has a counter in the stream state machine, stored under
the shard's routing key `k/<stream id>/<ordinal>`. `apply()` hands each
record of an append entry the next value. The counter is not the Raft log
index, so it does not change when Kythira splits, merges or moves the group
that holds the shard. The record segment store uses the same counter as the
record's sequence, so a read position and a store position are the same
number.

A shard opened by `CreateStream` starts at counter 0, so its first record
gets 1. A child shard from resharding starts at whatever counter the
resharding entry gives it. Children are new ordinals, so their numbers
never collide with the parent's.

## How it is printed

The profile chooses the form.

| Profile | Form | Example (ordinal 3, counter 42) |
|---|---|---|
| `aws` (default) | 56 decimal digits: `49`, ordinal (10 digits), counter (20), sub-record index (5), then 19 zeros | `49000000000300000000000000000042000000000000000000000000` |
| `compact` | the counter in decimal | `42` |

The `aws` form is the length AWS prints, so clients that check the length
or store the number in a fixed-width column keep working. It has a fixed
width, so comparing two numbers as strings gives the same answer as
comparing them as integers. Within a shard, that order is the record order.

The 19 trailing zeros are reserved. A number with anything else in that
part is refused as `InvalidArgumentException`. That includes a real AWS
sequence number pasted into a request, which is refused rather than misread
as a position here.

The sub-record index is 0 today. It is there for KPL-aggregated records,
which keep their aggregate's sequence number and add a sub-sequence number.

## Arrival timestamps and the clock assumption

`ApproximateArrivalTimestamp` is stamped by the group leader from its wall
clock when it proposes the append. All records of one entry share it.
`apply()` never reads a clock. It keeps the shard's latest timestamp and
uses the larger of that and the stamped one, so within a shard the
timestamp never goes backwards.

What this assumes:

- **The skew between leaders' clocks is bounded.** Kythira nodes are
  expected to run NTP or chrony.
- **A leader change can repeat a timestamp but never reverse one.** If the
  new leader's clock is behind the old one, the new leader's records take
  the last stamped time until its own clock passes it. Several records, or
  several seconds of records, can therefore share one timestamp after a
  failover. Their order is still exact, because order comes from sequence
  numbers.
- **A clock far ahead pulls every later record forward.** One leader with a
  clock an hour fast stamps an hour ahead. Every later record in that shard
  keeps that time until real time catches up. `AT_TIMESTAMP` iterators and
  `MillisBehindLatest` are off by the same amount during that window. This
  is the price of never going backwards, and clock monitoring is the fix.

Each timestamp lands in the record segment store as the record's
timestamp, so segment age rolls and `AT_TIMESTAMP` lookups see the same
value `GetRecords` returns.
