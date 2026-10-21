# Requirements Document

## Introduction

`object_store_persistence_engine` caps a cluster's commit rate at about
`1 / p50(PUT)`: a leader's `submit_command` makes one synchronous PUT per
command, and nothing else can append while it is in flight. The
object-store-batched-layout spec named this as its residual gap 1 and left
"leader group commit" to its own spec. Its open question 1, scoped on October
21, 2026, found the gap smaller than that spec assumed:

- **`node` already does group commit.** `.kiro/specs/durable-append-barrier/`
  moved the durability boundary off the append site. `submit_command` writes
  its entry with `append_log_entry_sequenced` under `_mutex`, releases
  `_mutex`, and waits in `settle_barrier` → `barrier_through(seq)` before it
  replicates or counts itself. The follower does the same before it answers
  AppendEntries. The file engine turns N waiters into one `fsync`.
- **The object-store engine opts out.** It is not a
  `barriered_persistence_engine`, so `persist_append` takes the non-barriered
  arm, `append_log_entry`, which is one PUT made under `_mutex` and under the
  engine's own `_mu`. No second command can even be *staged* while that PUT is
  in flight.

So this spec is an **engine** change: the object-store engine becomes a
barriered engine whose append *stages* an entry and whose barrier *flushes*
every staged entry as one `append_log_entries` run. Under the segmented log
(`log_layout::segmented`) that run is one PUT per segment. Under `per_entry` it
is up to `append_concurrency` PUTs in flight. No `node` code changes, and the
concepts change by one enumerator.

**Scope**: `object_store_persistence_engine`'s append, barrier, truncation and
compaction paths; one `durability_class` value and the two places that read
it; tests; the operating envelope.

**Out of scope**: the file and memory engines; persisting in parallel with
sending (Ongaro §10.2.1, the leader sending AppendEntries while its own write is
in flight), which is a `node` change; pipelining client commands beyond what
the barrier already allows; any change to the on-bucket format.

## Glossary

- **Stage** — record an entry in the engine's mirror and its staging queue,
  with no request. The entry is not durable.
- **Flush** — write every staged entry through a sequence as one
  `append_log_entries` run.
- **Write sequence** — `kythira::write_sequence`, assigned per staged entry,
  monotonic per engine (the barrier concept's).
- **Write-behind** — the engine configuration in which appends stage and the
  barrier flushes.
- **Synchronous** — today's configuration: every append is a PUT that has
  returned before `append_log_entry` does.

## Requirements

### Requirement 1: Opt-in, and today's behaviour by default

1. `object_persistence_options` SHALL gain `write_behind` (default `false`).
2. WITH `write_behind == false` the engine SHALL send exactly today's requests
   for every call sequence, and `node` SHALL hold exactly the locks it holds
   today (no `_mutex` released around a barrier that cannot block).
3. THE engine SHALL satisfy `barriered_persistence_engine` at every setting, so
   the setting is a runtime option and not a second type.
4. `write_behind` SHALL be accepted under either `log_layout` and either
   `fencing_mode`.

### Requirement 2: Durability classes

1. `durability_class` SHALL gain `synchronous`: every append is durable when it
   returns, and a barrier never blocks.
2. `describes_durable_storage` SHALL answer `true` for `synchronous` and for
   `barrier`, and `false` otherwise.
3. `node::barrier_can_block` SHALL stay `durability() == barrier`, so a
   `synchronous` engine keeps today's locking (Requirement 1.2).
4. THE object-store engine SHALL answer `synchronous` with `write_behind` off
   and `barrier` with it on.

### Requirement 3: Staging

1. WITH `write_behind`, `append_log_entry_sequenced` SHALL validate the entry
   (the size cap, Requirement 6's refusals), add it to the mirror, stage it,
   and return its sequence, sending no request.
2. `append_log_entry` SHALL be `append_log_entry_sequenced` followed by
   `barrier_through` of that sequence, so a caller that does not know about the
   barrier still gets a durable append.
3. `append_log_entries` SHALL stage the run and flush through its last
   sequence before returning, keeping `bulk_append_persistence_engine`'s
   all-or-nothing contract.
4. Reads (`get_log_entry`, `get_log_entries`, `get_last_log_index`) SHALL see
   staged entries: the mirror is what the node believes, not what the store
   holds.

### Requirement 4: The barrier

1. `barrier_through(seq)` SHALL return only once every staged entry with a
   sequence ≤ `seq` has been acknowledged by the store.
2. AT most one flush SHALL be in flight per engine. A caller that arrives while
   one is in flight SHALL wait for it and then flush whatever is still staged,
   so N concurrent callers cost at most two flushes, not N.
3. THE flush SHALL sample the highest sequence it will claim before its first
   request and publish it after its last acknowledgement — the
   durable-append-barrier ordering rule — and SHALL release `_mu` across the
   requests so that appends can stage while it runs.
4. A flush SHALL write its entries as one `append_log_entries` run: one
   object per entry under `per_entry`, the run's chunks under `segmented`.
5. `durable_through()` SHALL report the highest published sequence.

### Requirement 5: Failure

1. A flush that fails SHALL throw from `barrier_through` to every caller whose
   sequence it was to cover, and SHALL publish only the contiguous prefix of
   the run whose objects were acknowledged.
2. Every object a failed flush sent beyond that prefix SHALL be recorded as a
   stray, and the entries SHALL stay staged, so the next barrier deletes the
   strays and retries them. A transient failure SHALL NOT latch.
3. UNDER `compare_and_swap` a refused create-only PUT SHALL latch, as it does
   today, and SHALL fail every waiter.
4. A sequence whose entry was discarded before any flush reached it
   (Requirement 6) SHALL make `barrier_through` throw rather than wait,
   as the file engine does.

### Requirement 6: Truncation and compaction with staged entries

1. `truncate_log(i)` and `delete_log_entries_before(j)` SHALL wait for an
   in-flight flush to finish before they act.
2. `truncate_log(i)` SHALL discard staged entries at or above `i` without a
   request, and act on the store only for flushed objects.
3. `delete_log_entries_before(j)` SHALL discard staged entries below `j`
   without a request.
4. EVERY crash table of the object-store-batched-layout spec SHALL still hold:
   a staged entry is never on the store, so a crash at any request reads a
   state that spec's oracle allows for the flushed log, with staged entries
   simply absent.

### Requirement 7: What `node` sees

1. NO code in `raft.hpp` SHALL change.
2. A leader with `write_behind` and N concurrent `submit_command` callers SHALL
   make fewer than N PUTs per N commands once N > 1, measured by the mock's
   request log.
3. A follower SHALL still answer AppendEntries success only after the barrier
   covering its entries returned (durable-append-barrier Requirement 1.2).

### Requirement 8: Tests and envelope

1. THE object-store conformance suite SHALL pass unmodified at
   `write_behind == false`, and SHALL be instantiated with `write_behind` on,
   over both log layouts, unfenced and fenced.
2. A crash-injection case SHALL cover a flush of a multi-chunk staged run at
   every request, with appends staging concurrently.
3. A failure case SHALL cover a partially acknowledged flush: prefix published,
   remainder strayed and retried, no latch.
4. A raft-level case SHALL run a three-node cluster on write-behind mock
   engines with concurrent submitters, and assert both coalescing
   (Requirement 7.2) and that no entry was counted before its barrier.
5. `doc/cloud_object_persistence.md` SHALL state the ceiling with write-behind
   as `B / p50(flush)`, with `B` the measured entries per flush, and SHALL say
   that the default is unchanged.
6. THE object-store-batched-layout spec's real-cloud task SHALL measure a
   write-behind leader's commit rate beside the synchronous one.
