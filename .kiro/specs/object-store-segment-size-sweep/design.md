# Design Document

## Overview

| | Today | After this spec |
|---|---|---|
| `max_segment_bytes` default | 1 MiB, reasoned | the S Requirement 8 picks, measured |
| Segment sizes measured | two (100 small entries; ~0.88 MiB), S3 only, n = 10 | seven, every provider Clark approves, n ≥ 30 |
| Operations measured against size | one PUT | one PUT, a follower's run, a truncation rewrite, recovery, a write-behind flush |
| Bound the default is judged against | none written down | Requirement 7's, backed by a mock-tier Raft test |

The sweep is four new real-tier cases and a reporting line. The engine does
not change. `node` does not change. The workflow gains one environment
variable per provider step.

## Why size matters, operation by operation

What one segment's size buys or costs, read from the engine as it stands
(`include/raft/object_store_persistence.hpp`):

| Operation | Requests | Effect of a larger S |
|---|---|---|
| Follower `append_log_entries` of one AppendEntries | one PUT per chunk from `chunk_run`, up to `append_concurrency` (8) in flight | fewer PUTs, but each slower, and less parallelism. A 100-entry run that fits in ≤ 8 segments already runs in one parallel wave, so past that point a larger S can only lengthen the wave |
| Leader write-behind flush | the same `append_log_entries` | as above, for the staged run |
| Leader synchronous append | one PUT per entry | none. A single small entry never fills a segment |
| `truncate_log` inside a segment | one rewrite of that segment (`If-Match` under `compare_and_swap`) plus deletes above it | the rewrite is a PUT of up to S |
| Recovery | one List, then one GET per log object, **sequential** (the load loop calls `get_object` per key) | fewer GETs of larger bodies; for a fixed log size, fewer round trips |
| Request bill | per request, not per byte, on every provider | fewer requests per GiB of log |

So the default trades recovery time and request cost (favouring large S)
against follower answer time and truncation cost (favouring small S, with a
knee where the run stops fitting in one parallel wave). The two S3 points
cannot locate that knee, and the knee depends on entry size, which is why
Requirement 3 runs two.

## Requirement 7: the liveness bound

A follower's AppendEntries handler resets its election timer **on receipt**
(`raft.hpp`, "Reset election timer on valid AppendEntries (§5.2)"), before
Rule 5's `persist_and_append_run`. Under `write_behind = false` the engine
answers `synchronous`, `barrier_can_block()` is false, and the run's PUTs are
made with `node::_mutex` held. While they run, the election-timer tick and
the leader's next AppendEntries both wait on `_mutex`.

Task 1 settled what happens next on the mock
(`tests/raft_object_store_segment_liveness_test.cpp`): a three-node cluster
whose follower holds one run's PUT for 1.5 s against a 300–600 ms election
timeout, five rounds, with a healthy leader heartbeating every 50 ms.

- **The follower's timer fires.** The tick takes the mutex before the queued
  heartbeat in every round, sees `now − _last_heartbeat` past its timeout,
  and starts a round. The time a follower spends in its own persistence does
  count against it.
- **The term never moves.** The round is a PreVote round
  (`.kiro/specs/raft-pre-vote/`), and the other two nodes refuse it because
  they heard from the leader within their own timeouts. No `RequestVote` is
  sent, the leader keeps its term, and the queued heartbeats are answered
  straight after. Every shipped transport implements PreVote (the simulator,
  TCP, TLS-TCP, the three HTTP clients, gRPC and the three CoAP backends), and
  there is no runtime switch to turn it off, so this holds for every
  deployment; the test `static_assert`s it so a transport without PreVote
  cannot silently invalidate it.
- **The leader does not resend.** No log key was PUT twice: a run answered
  inside `append_entries_timeout` is not retried.

So the bound is the second of the two this section used to offer:

> tail(run persist time) < `append_entries_timeout` (5 s by default)

and `election_timeout_min` is unaffected by segment size. Past that bound the
leader's RPC times out and its retry policy resends the run; the follower's
second call writes chunks the first already wrote, create-only keys are
refused, and under `compare_and_swap` that latches the engine. That is the
hard ceiling the sweep reports run persist time against. The provider's
`combined` election-timeout floor (`doc/cloud_object_persistence.md`,
"Election-timeout sizing") no longer enters the rule, because the bound does
not depend on it.

The one cost the test does show is a wasted PreVote round per run that
overruns the follower's timeout: two small RPCs, refused, with no effect on
the cluster. The sweep need not price it. The test stays in CTest as a
regression guard: a change that let a follower's own persistence time move
its term would fail it.

## The cases

All five live in `tests/object_persistence_real_cases.hpp`, beside cases
1–11, and take the same `(store, bucket, prefix, provider)` arguments plus a
`sweep_grid` describing the sizes and samples:

```cpp
struct sweep_grid {
    std::vector<std::size_t> segment_bytes{64 * k_kib, 256 * k_kib, 512 * k_kib, 1 * k_mib,
                                           2 * k_mib, 4 * k_mib, 8 * k_mib};
    std::size_t samples{30};
    std::size_t recovery_samples{5};
    std::size_t recovery_log_bytes{16 * k_mib};
};
```

The mock test passes `{ {4 * k_kib, 16 * k_kib, 64 * k_kib}, 3, 1, 256 * k_kib }`.

| Case | Requirement | Each sample |
|---|---|---|
| 12 `case_segment_sweep_put` | 2 | one `append_log_entries` of `k` equal entries whose segment body is within 5% below S; `max_segment_entries = 1000` |
| 13 `case_segment_sweep_follower_run` | 3 | one 100-entry run of 4 KiB, then of 32 KiB, entries; records the chunk count by listing the new keys |
| 14 `case_segment_sweep_truncate` | 4 | write one full segment, `truncate_log` at its middle index, then a fresh engine reads the truncated tail back (a correctness check, untimed) |
| 15 `case_segment_sweep_recovery` | 5 | per S, write 16 MiB of 128 KiB-payload entries once; then time `recovery_samples` fresh engine constructions |

Requirement 6 does not need a case of its own: case 11's driver becomes a
function taking options and a payload size, which case 11 calls exactly as
today and the sweep calls once per S with 64 KiB payloads and
`write_behind = true`. Case 11's output line is unchanged so the S3 numbers
from run 39727417123 stay comparable.

Round-robin (Requirement 1.3) is per case: each case opens one engine per S on
its own sub-prefix up front, then loops `samples` rounds over the grid. Case 15
is the exception, since a sample is a whole engine construction; it still
alternates S between samples.

### Picking entry sizes to hit a body size

`chunk_run`'s body is JSON with base64 payloads, so the case computes the body
of a candidate run with the engine's own encoding rather than estimating
4/3. The helper encodes `k` entries of payload `p`, takes the largest `k` with
`p = 96 KiB` (the existing case 9 size) whose body is ≤ S, then grows `p` until
the body is within 5% of S. For S = 64 KiB that is one entry, a singleton,
which is reported as such: below one entry's size a "segment" is a singleton
and the S knob is no longer what is being measured.

### Reporting

One greppable line per sample set, in the style of `KYTHIRA_LATENCY` and
`KYTHIRA_COMMIT_RATE`:

```
KYTHIRA_SEGMENT_SWEEP provider=s3 arch=x64 op=follower_run entry_payload=32768
  segment_bytes=1048576 body_bytes=4467712 chunks=5 samples=30
  p50_ms=… p90_ms=… max_ms=… measured=client-side-around-engine-call
```

`arch` comes from `uname -m` at runtime rather than an environment variable,
so a local run reports truthfully too. No field is named `p99`
(Requirement 1.4).

## Opt-in

`real-cloud-tests.yml` has 25 `workflow_dispatch` inputs, GitHub's maximum, so
the sweep cannot be a dispatch input without removing one. It is a repository
variable, `REAL_CLOUD_SEGMENT_SWEEP`, read the way the bundle toggles' repo
variables are, and passed into each object-persistence bundle step as
`KYTHIRA_SEGMENT_SWEEP: ${{ vars.REAL_CLOUD_SEGMENT_SWEEP }}`. Each sweep case
begins:

```cpp
if (!sweep_enabled()) {   // KYTHIRA_SEGMENT_SWEEP == "true"
    BOOST_TEST_MESSAGE("segment sweep not enabled (REAL_CLOUD_SEGMENT_SWEEP)");
    return;
}
```

A pass, not exit 77: the suite's other cases ran, and 77 means "could not
run", which the workflow turns into an error for an enabled bundle. The Boost
timeout on each sweep case is 1800 s.

The variable is set for one dispatch and cleared after, and the run plan job's
"Report what this run will do" step prints it so a reader of the run sees the
sweep was on.

## Cost and duration

Upper bounds for one provider and one architecture at the full grid
(sizes sum to ~15.8 MiB):

| Case | PUTs | Bytes up | GETs | Bytes down |
|---|---|---|---|---|
| 12 | 210 | ~475 MiB | 0 | 0 |
| 13 | ≤ 42,000 (100 per run at 64 KiB, fewer above) | ~1 GiB | 0 | 0 |
| 14 | ~420 | ~715 MiB | ~210 | ~240 MiB |
| 15 | ~700 | ~112 MiB | ≤ 9,000 | ~560 MiB |
| 6 via 11 | ≤ 900 | ~30 MiB | 64 | ~4 MiB |
| **Total** | **≤ ~44,000** | **~2.3 GiB** | **≤ ~9,300** | **~0.8 GiB** |

At list prices (S3 PUT $0.005 per 1,000; internet egress ~$0.09/GB; the other
four providers are within a factor of two on requests) that is about **$0.30
per provider per architecture**, under Requirement 9.4's dollar. Ingress is
free everywhere. Storage is deleted within the run.

Duration is dominated by case 13 at small S and case 15's sequential GETs:
on the x64 S3 runner (~160 ms p50) about 15–25 minutes per architecture. That
fits inside the job's existing one-hour credential session, which the S3 step
already relies on.

## Decision procedure

After each approved run the numbers go into a table in
`doc/cloud_object_persistence.md` (one row per provider × arch × S × op).
Requirement 8 is then applied mechanically:

1. Mark each S that passes 8.1 on every provider row.
2. Of those, the largest is the candidate. Check 8.2 for every smaller passing
   S against it.
3. If the result is not 1 MiB, the default changes in its own commit
   (Requirement 8.4), with `doc/CHANGELOG.md` and the limits table.

`max_segment_bytes ≤ max_object_bytes` is already validated at construction,
and loading never consults `max_segment_bytes`, so a change in either direction
is safe for existing buckets and needs no migration.

## Alternatives considered

1. **A finer grid or more samples.** Each extra size costs a full row of
   cases 12–15; doubling samples doubles the bill and the duration. Seven
   sizes over two decades at n = 30 locates a knee to within a factor of two,
   which is as precise as a default needs to be.
2. **Sweeping inside the existing case 9.** It would run on every bundle and
   multiply the routine run's cost and duration for a question that is asked
   once.
3. **Choosing S per provider at runtime.** The engine does not know which
   provider it is over, by design (it is a template over a store concept), and
   a placement-dependent default would make the same configuration behave
   differently by region. Requirement 8.5's documented table is the fallback.
4. **A synthetic benchmark outside the engine (raw client PUTs).** Cheaper to
   write, but it would not measure `chunk_run`'s splitting, the engine's
   concurrency, or recovery's sequential load, which are what the default
   actually governs.

## Open questions

1. ~~If task 1 shows a follower can campaign against a healthy leader while
   blocked in its own persistence, does Clark want that fixed in `node`?~~
   Settled by task 1: it cannot. Its timer fires, but the PreVote round is
   refused and the term never moves (see "Requirement 7: the liveness bound").
2. Whether the arm64 and x64 runners' very different S3 latencies (35 ms vs
   160 ms p50 for one small PUT) reflect runner placement rather than
   architecture. The sweep reports both; the decision uses the worse.
