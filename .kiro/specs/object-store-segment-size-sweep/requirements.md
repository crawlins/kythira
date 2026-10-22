# Requirements Document

## Introduction

`object_persistence_options::max_segment_bytes` bounds the body of one log
segment under `log_layout::segmented`
(`.kiro/specs/object-store-batched-layout/`). Its default, **1 MiB**, was
reasoned rather than tuned: the batched-layout design picked it as "the largest
rewrite truncation can cost and the largest GET recovery pays per object", and
said no segment-sized PUT had been measured. `doc/cloud_object_persistence.md`
still says so in its limits table and its "Future work" list.

The S3-only real-cloud run of batched-layout task 10 (Actions run
39727417123, October 22, 2026, `main` at 747227c) measured two segment sizes
and nothing between them. Client-side, around the engine call, ten samples
each:

| Runner | `append_log_entry` p50 / p99 | 100 small entries, one segment p50 / p99 | ~0.88 MiB segment (7 × 96 KiB entries) p50 / p99 |
|---|---|---|---|
| x64 | 160.7 / 178.8 ms | 175.4 / 185.8 ms | **230.3 / 700.0 ms** |
| arm64 | 35.0 / 42.1 ms | 40.1 / 44.1 ms | **89.4 / 313.2 ms** |

Three things follow, and none of them settles the default:

- **Size costs latency, and mostly in the tail.** A near-1 MiB body added
  ~55 ms to the x64 p50 but ~515 ms to its p99. With ten samples a "p99" is the
  maximum, so that tail is one sample, not a distribution.
- **There are two points on the curve.** Whether 256 KiB is nearly free or
  4 MiB is nearly as cheap as 1 MiB is unknown on every provider.
- **The run measured one PUT, not the operations the bound exists for.** A
  follower answers AppendEntries after its whole run is durable, possibly
  several segments in flight at once (`append_concurrency`); truncation
  rewrites one segment; recovery GETs every segment one after another;
  write-behind flushes chunk by the same bound. None was measured against
  segment size.

This spec adds an **opt-in segment-size sweep** to the real tier, runs it, and
uses the result to keep or change the default, with the reasoning written down
in the operator documentation either way.

**Scope**: new real-tier cases in `tests/object_persistence_real_cases.hpp`,
their mock-tier smoke run, one Raft-level mock test of the liveness bound the
sweep is judged against, the workflow wiring that opts a run in, the
`max_segment_bytes` default and its documentation.

**Out of scope**: `max_segment_entries` (it stays tied to
`max_entries_per_append`, 100); per-provider defaults chosen at runtime;
multipart upload; any change to the on-bucket format, to `node`, or to
`append_concurrency`'s default. If the sweep shows `append_concurrency` should
move, that is recorded as a finding, not changed here.

## Glossary

- **S** — a value of `max_segment_bytes` under test.
- **Body bytes** — the encoded size of a segment object as PUT, which is what a
  provider sees. Entries are base64 inside the JSON segment, so body bytes are
  about 4/3 of the payload bytes.
- **Run persist time** — wall time of one `append_log_entries` call: what a
  follower spends before it can answer one AppendEntries.
- **Sweep** — the cases of Requirements 2–6, run at every S of Requirement 1.
- **Tail** — the highest sample. With fewer than 100 samples no figure in this
  spec is called a p99.

## Requirements

### Requirement 1: The sweep grid

1. THE sweep SHALL measure S ∈ {64 KiB, 256 KiB, 512 KiB, 1 MiB, 2 MiB,
   4 MiB, 8 MiB}, all below `max_object_bytes` (64 MiB).
2. EVERY measurement SHALL record the S it ran at, the body bytes actually
   sent, the provider, the runner architecture, and that it was measured
   client-side around the engine call.
3. SIZES SHALL be visited round-robin, one sample of each S per round, so that
   drift in the service or the network over the run spreads across every S
   rather than landing on whichever ran last.
4. EACH S SHALL get at least 30 samples per measured operation, and the report
   SHALL give p50, p90 and the maximum, and SHALL NOT label any of them p99.

### Requirement 2: Single segment PUT latency

1. FOR each S, the sweep SHALL time one `append_log_entries` call whose run
   is exactly one segment of body bytes within 5% below S, with
   `max_segment_entries` raised so that the byte bound is the one that binds.

### Requirement 3: Follower run persist time

1. THE sweep SHALL time `append_log_entries` of one full AppendEntries run of
   100 entries at each of two entry sizes, 4 KiB and 32 KiB of payload (about
   0.5 MiB and 4.3 MiB of body per run), at every S.
2. EACH sample SHALL record how many segments the run became, so a reader can
   tell when `append_concurrency` (default 8) ran them in parallel and when
   they queued.

### Requirement 4: Truncation rewrite

1. FOR each S, the sweep SHALL time `truncate_log` at an index in the middle
   of a full segment, which rewrites that segment in place, and SHALL run it
   on the fenced engine where the provider fences (an `If-Match` overwrite) and
   unfenced otherwise.

### Requirement 5: Recovery

1. FOR each S, the sweep SHALL write a log of the same total payload, 16 MiB,
   and time a fresh engine's construction over it, recording the number of
   log objects loaded.
2. THE recovery measurement SHALL take at least 5 samples per S; its cost is
   GET egress, and Requirement 9 bounds it.

### Requirement 6: Write-behind with large commands

1. FOR each S, the sweep SHALL run `case_write_behind_commit_rate`'s driver
   with 16 submitters and 64 commands of 64 KiB payload under
   `write_behind = true`, and report commits/s, log PUTs, `B`, and how many
   flushes were split into more than one segment.

### Requirement 7: The liveness bound the sweep is judged against

1. A Raft-level mock test SHALL establish what a follower does when one run's
   persist time exceeds its election timeout: whether, on return, it campaigns
   against a healthy leader or processes the queued AppendEntries first.
2. THE design SHALL state the bound that test supports — run persist time
   against `election_timeout_min`, and against `append_entries_timeout`
   (default 5 s), past which the leader resends the run — and the decision
   rule of Requirement 8 SHALL use it.

### Requirement 8: The decision

1. THE default SHALL be the largest S that, on every provider measured,
   keeps the tail of Requirement 3's run persist time (both entry sizes) inside
   the Requirement 7 bound at that provider's documented `combined`
   election-timeout floor.
2. AMONG sizes that satisfy 8.1, a smaller S SHALL be preferred only if it
   lowers the p50 run persist time of the 32 KiB case by at least 20% on at
   least three providers and raises it by no more than 10% on any.
3. IF 1 MiB satisfies 8.1 and no S passes 8.2, the default SHALL stay 1 MiB,
   and the documentation SHALL say it is now measured.
4. A changed default SHALL be a separate commit whose body cites the run ids
   and the numbers it rests on, and SHALL be listed in `doc/CHANGELOG.md`.
   Existing buckets need nothing: segment size is a write-side bound, and any
   segment the engine can parse loads at any setting.
5. IF providers disagree enough that no single S satisfies 8.1 on all of them,
   the default SHALL be the largest S that does on the providers the
   documentation calls well placed, and the limits section SHALL carry a
   per-provider recommendation table.

### Requirement 9: Opt-in, cost, and permission

1. THE sweep SHALL NOT run in the existing object-persistence bundles unless
   opted in. The opt-in SHALL be a repository variable, because
   `real-cloud-tests.yml` already has 25 `workflow_dispatch` inputs, GitHub's
   maximum.
2. WITHOUT the opt-in, each sweep case SHALL pass with a logged skip message,
   so a bundle run is unchanged in duration and cost.
3. NO real-cloud sweep run SHALL be dispatched without Clark's go, and the
   first SHALL be S3 only, matching how batched-layout task 10 was run.
4. THE design SHALL state the sweep's request count and bytes per provider and
   an upper bound on its cost, and the bound SHALL be under one US dollar per
   provider per architecture.
5. THE sweep SHALL clean up every object it writes, as the existing cases do.

### Requirement 10: Tests and records

1. `object_persistence_real_cases_mock_test` SHALL run every sweep case on the
   mock store with a reduced grid and sample count, so the cases are known to
   work before a paid run. That SHALL NOT tick any real-cloud task.
2. RESULTS SHALL be recorded in `doc/cloud_object_persistence.md` beside the
   existing latency tables, with run ids, and the limits table and "Future
   work" entry for `max_segment_bytes` SHALL be rewritten from them.
3. No task requiring a live run SHALL be checked off against the mock tier.
