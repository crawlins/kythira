# Design Document

## Overview

The object-store engine joins the barrier protocol `node` already speaks:

| | Today (`synchronous`) | `write_behind` (`barrier`) |
|---|---|---|
| `append_log_entry_sequenced` | (not provided) | stage, no request |
| `append_log_entry` | one PUT under `_mu` | stage + `barrier_through` |
| `barrier_through(seq)` | (not provided) | flush every staged entry through `seq` as one run |
| `durability()` | (not provided) | `barrier` |
| `node` releases `_mutex` around the barrier | no | yes |
| N concurrent `submit_command` | N sequential PUTs | at most two flushes in flight one after the other, each one run |

With `write_behind` off the engine also provides the four barrier members, but
`append_log_entry_sequenced` is a synchronous PUT, `barrier_through` returns at
once, and `durability()` answers the new `synchronous`, which `node` reads as
"cannot block" and so holds `_mutex` exactly as it does today.

## Why a new `durability_class`

`barrier_can_block()` (`raft.hpp`) decides whether `node` breaks its critical
section open around a barrier. It is `durability() == barrier`. A synchronous
engine must answer something that is *durable* (so
`describes_durable_storage` does not call an object-store node undurable) and
*not blocking* (so Requirement 1.2 holds). Neither existing value is both:
`buffered` is "not durable", and `barrier` blocks. `synchronous` is both, and
the only readers of the enum are `barrier_can_block`, which needs no change,
`describes_durable_storage`, which gains one comparison, and
`switchable_persistence`, which forwards whatever it is given.

## Engine state

```cpp
struct staged_entry { log_entry_t entry; write_sequence seq; };
std::deque<staged_entry> _staged;      // in index order, never sent
write_sequence _write_seq{0};          // last assigned
write_sequence _durable_seq{0};        // last published
bool _flush_running{false};
std::condition_variable _flush_cv;     // waits on _mu
std::uint64_t _flushes_issued{0};      // for Requirement 7.2's test
```

A staged entry is in `_log` (Requirement 3.4) and in `_staged`. A flushed
entry is in `_log` only, exactly as today.

## The flush

```
barrier_through(seq):
    lock _mu
    loop while _durable_seq < seq:
        if _flush_running: wait _flush_cv; continue
        if no staged entry has sequence ≤ seq:
            throw "sequence seq can never be made durable" (discarded, Req 5.4)
        run ← every staged entry          # not only those ≤ seq: they are free riders
        sampled ← run.back().seq          # BEFORE the first request
        _flush_running ← true
        clear_strays(keys of run's objects)   # under _mu, as every log write does
        build keys and bodies (chunks under segmented)
        unlock _mu
        outcomes ← put_log_objects(...)   # the workers never take _mu
        lock _mu
        _flush_running ← false
        k ← length of the acknowledged prefix of objects
        pop those entries from _staged; publish their last sequence (AFTER)
        record the rest that were sent as strays; leave them staged
        notify_all
        throw if k < number of objects (latch first on a refused precondition)
```

**Why the whole queue rides along.** A flush that took only the entries
through `seq` would leave later stagers to pay a request each. Taking
everything staged at sampling time is what makes the second flush, the one the
waiting callers start, carry everything that staged during the first.

**Why `clear_strays` stays under `_mu`.** Deleting strays changes the store
and the stray set together. It is at most one DELETE per earlier failure, and
it has to precede the PUTs (stray invariant I1 of the batched-layout spec).

**Concurrency with staging.** While the flush is unlocked, appends stage behind
it: their indices are above the run's, their sequences above `sampled`, and they
are not in the run. The run's objects are new keys above everything flushed, so
the mirror and the store cannot disagree about them while the flush is out.

**Concurrency with truncation and compaction.** Both take `_mu`, then wait on
`_flush_cv` while `_flush_running`, before touching anything (Requirement 6.1).
Waiting while holding `node::_mutex` is safe: the flush never takes it.

## Truncation and compaction

```
truncate_log(i):
    wait for no flush
    drop staged entries with index ≥ i (and from _log)
    then the batched-layout algorithm over flushed objects only
```

A staged entry dropped here has a sequence no flush will ever publish, so its
waiter (a follower's AppendEntries handler that released `node::_mutex`) gets
Requirement 5.4's exception and answers `false`. That is the file engine's
behaviour for a truncated batch, and the leader retries.

Compaction drops staged entries below `j` the same way. In practice there are
none: a snapshot covers only committed entries, which were durable on a
majority, and the leader counts itself only after its own barrier. But the rule
does not depend on that.

## Crash safety

Nothing staged is ever on the store, so a crash during staging changes nothing
on the store. A crash during a flush is a crash during an `append_log_entries`
run, which the batched-layout crash tables already cover: a prefix of the run
is readable, as the `extend_log` oracle allows. The waiters never returned, so
nothing they staged was advertised. That is the barrier contract.

## Fencing

The flush's PUTs are create-only under `compare_and_swap`, as every new log
object is. A refused one latches the engine and fails every waiter. A
transient failure leaves the sent objects as strays (Requirement 5.2) rather
than retrying the same create-only PUT, which would be refused by its own
landed object and latch a healthy node. That is the lost-response residual,
avoided here instead of stated.

## The ceiling, restated

With `write_behind`, a leader's commit rate is about `B / p50(flush)`, where `B`
is the entries per flush. `B` is roughly the commands that arrive during one
flush. Under `segmented` a flush of `B ≤ max_segment_entries` entries is one PUT
of `B` records, so `p50(flush) ≈ p50(segment PUT)`, which the batched-layout
spec's task 10 measures. Under `per_entry` it is `⌈B / append_concurrency⌉`
round trips of single-entry PUTs.

## Alternatives considered

1. **Change `node` to batch `submit_command` itself.** It would duplicate the
   barrier machinery that already exists, and it would change locking for
   every engine.
2. **Write-behind without the barrier**, flushing on a timer. That would
   advertise appends before they are durable, which is the defect the
   durable-append-barrier spec removed.
3. **Make write-behind the default.** It changes `node`'s locking for every
   object-store deployment (`_mutex` released around the barrier) and its
   request timing. Defaults stay byte- and lock-identical, as the
   batched-layout spec's Property 1 promises.
4. **Answer `barrier` with write-behind off.** Then `node` would release
   `_mutex` around a no-op barrier, which the durable-append-barrier spec
   rejected for the memory engine for the same reason: a new interleaving
   window for nothing.

## Residual gaps

1. The leader still persists before it sends (Ongaro §10.2.1 is a `node`
   change).
2. Each flush is one run, so one slow chunk delays every waiter of that flush.
3. Staged entries cost memory until flushed. There is no bound beyond the
   commands in flight, which `node` already bounds by its callers.
