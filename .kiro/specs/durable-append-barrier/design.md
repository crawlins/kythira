# Design Document

## Overview

Move the durability barrier from `multi_raft::tick()`'s persist phase, where it
covers a fifth of the log, to the boundary where a node **advertises** an
append — and make that affordable with group commit.

The measurement this design exists to answer is
`.kiro/specs/multi-raft-performance/` task 19: 19.9% and 24.5% coverage in two
runs, with a controller supplied exactly as documented.

## The mistake in the current placement, stated precisely

`tick()` is the wrong place for a reason that has nothing to do with how it is
written. A batch is a property of a *thread's* sequence of writes: `begin_batch`
sets `_batching`, appends accumulate into `_batch_lines`, `commit_batch` flushes
and fsyncs. `tick()` runs on the host's driver thread. The appends run on the
client's thread (`submit_command`) and on the transport's handler threads
(`handle_append_entries`). A batch opened on one thread cannot capture writes
made on another except by accident of timing — which is exactly what the 20–25%
figure is measuring.

Worse, it is *unsafe* rather than merely incomplete: `file_persistence_engine`'s
`_batching` flag is guarded by `_mu`, so an append on an RPC thread arriving
while the tick thread holds a batch open is silently buffered into that batch
and becomes durable only when the tick commits — after `handle_append_entries`
has already returned success. The window is small and the failure is total.

## The boundary that is correct

Raft's requirement is not "every append is fsynced immediately". It is that an
append is durable **before the node acts on it as though it were**. Two places:

- **Leader.** Before the entry counts toward `match_index` and therefore toward
  the commit index. `raft.hpp:1638` appends; the barrier belongs between that
  and the point the leader treats itself as having the entry.
- **Follower.** Before `handle_append_entries` returns success.
  `append_entries_with_consistency_check` appends at `raft.hpp:4300`/`4304`;
  the barrier belongs before its caller returns.

Both are *response boundaries*, not append sites. That distinction is what makes
group commit possible: several appends may pile up between the write and the
response, and one barrier can serve all of them.

## Group commit

The mechanism, and the reason it is the only affordable shape:

```
append(entry):
    seq = log_writer.write(entry)      # bytes into the file, no fsync
    barrier.wait_for(seq)              # blocks until some fsync covered seq
    # only now may the caller advertise
```

`barrier.wait_for(seq)` is where the coalescing happens. A single barrier thread
(or the first waiter, promoted) performs one `fsync`, records the highest
sequence it covered, and wakes every waiter at or below it.

**The correctness rule Requirement 3.4 asks to be made impossible rather than
unlikely**: a waiter must never be woken by a barrier that *started* before its
own bytes were written. Recording "the highest sequence durable" and comparing
`durable >= seq` gets this right only if the sequence is assigned under the same
lock that appends the bytes, and if the barrier samples the sequence
**before** it calls `fsync` and publishes it **after**. Sampling after would
credit the barrier with writes that raced in during the syscall.

That is three lines of ordering and it is the whole of the design's subtlety, so
it is stated here and must be restated in the code.

## What this does to `tick_batch_controller`

Requirement 4 forces a decision. The recommendation is **removal**.

Its documented purpose — "one durability barrier spanning every ready group's
persist phase" — is not achievable from `tick()` for the reason above, and its
own header already concedes the narrower half: a single barrier for N groups
requires a store spanning N groups, and this codebase gives each group an
independent `file_persistence_engine` at its own directory. What remains after
both concessions is a hook that fsyncs whatever happens to be buffered on one
thread at one moment, which is not a durability mechanism and should not look
like one.

Removing it also removes the `batched_persistence_engine` concept's only
consumer. The concept itself should stay: the group-commit writer needs
"flush these bytes, then one barrier" from the engine, which is what
`commit_batch` already is. What changes is who calls it and when.

## Where the counter goes

Requirement 2.3 keeps it out of production code, and there is already a
precedent to follow rather than a new place to invent:
`tests/multi_raft_transport_harness.hpp`'s `benchmark_persistence_engine` is a
handle over either engine that counts what it forwards, and it already carries
`_entries`, `_entries_batched` and the `barriered_fraction()` those feed. It
needs one change: today it infers coverage from "was a batch open when the
append passed through", which is the right question for the current design and
the wrong one for this one. Under group commit the question becomes "did a
barrier whose covered sequence is at least this entry's complete before the
advertise", which the wrapper can observe because it sees both calls.

## Rollout

The change is not separable from correctness, so it cannot be feature-flagged
in the usual sense — a half-applied barrier is the current defect. It *is*
separable by engine: `memory_persistence_engine`'s barrier is a no-op
(Requirement 5.1), so every existing test and every Tier A/B row is unaffected
by construction, and the blast radius is exactly the file-backed path that is
currently not durable anyway.

## Testing strategy

- **A coverage test, first and failing.** Before any production change, a test
  that appends N entries through a file-backed engine and asserts every one was
  covered by a barrier. It must fail against today's code — a test for this
  that passes before the fix is testing the wrong thing.
- **A crash test.** Append, kill the process without a clean shutdown, restart,
  assert the entries the node advertised are present. `fiu` is already wired
  into `file_persistence` for fault injection and is the tool.
- **A group-commit test.** N concurrent appends, assert fewer than N barriers
  and that every append was still covered — the two halves that together mean
  the coalescing is real and safe.
- **An ordering test for the rule above**, driving the barrier and the writer
  from separate threads with an injected delay inside the `fsync`, asserting no
  waiter is released by a barrier that began before its write.
- **The existing suite, unmodified** (Requirement 5.2).

## Cost, stated in advance

`.kiro/specs/multi-raft-performance/` task 19 measured the file-backed,
no-barrier path at ~33% below memory, and the partially-barriered path a further
~13% below that. A fully-barriered path will be slower than both, and the honest
expectation is that the loss is bounded by the volume's `fsync` rate divided by
the achieved entries-per-barrier. On a gp3 volume at 3000 IOPS with group commit
working, that is not the bottleneck; with group commit broken, it is.

Stating it here means the first Tier D row can be read against a prediction
rather than against a hope.
