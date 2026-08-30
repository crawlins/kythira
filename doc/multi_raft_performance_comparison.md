# Multi-Raft Performance Comparison

## Overview

This document answers one question — *how close is this implementation's
multi-Raft write and read path to published numbers* — and it spends most of its
length on why the honest answer is narrower than the question.

It reports what `tests/multi_raft_http_benchmark_test` measured, on two machines,
under the deployment tiers `.kiro/specs/multi-raft-performance/requirements.md`
Requirement 3 defines. It carries the external comparison register in full, and
it keeps like-for-like comparisons and indicative ones in separate tables that
are never interleaved.

**The headline is a negative one, and it is the most important sentence here:
there is currently no like-for-like comparison in this document, and there
cannot be one yet.** Requirement 3.3 forbids publishing a like-for-like claim
from any tier below C, and every row measured to date is Tier A or Tier B — all
hosts in one process, over loopback, with `memory_persistence_engine`. Every
external number in the register was measured on a cluster of separate machines
with a real disk under it. What follows is therefore an *indicative* comparison
throughout, plus a set of structural findings that do not depend on the
comparison at all and are the substance of the work.

The second headline is about the measuring instrument rather than the system:
**a benchmark row of this suite became quotable for the first time on cloud
hardware.** Every throughput row taken on the development machine carries
`UNSTABLE` or `machine was not quiet at start`; on one `c5.2xlarge` every row of
a five-case sweep came back `stable` with a 0.8–3.9% spread, twice. Several
findings below exist only because that happened.

## Quick Start

```sh
# Build (Release; the numbers below are Release, folly future backend)
cmake -B build-default -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DKYTHIRA_KCONFIG=configs/ci_full_defconfig \
    -DCMAKE_PREFIX_PATH=$PWD/vcpkg_installed/x64-linux
cmake --build build-default --target multi_raft_http_benchmark_test

# The CI regression tier: ~15 s, ratios only, no rate assertions
ctest --test-dir build-default -R multi_raft_regression_tier --output-on-failure

# One axis of the full matrix (the whole matrix is hours)
./build-default/tests/multi_raft_http_benchmark_test \
    --run_test='multi_raft_http_benchmark/write_throughput_by_value_size' \
    --log_level=test_suite

# The subset-selecting report generator, writing CSV + JSON to an out-dir
./build-default/tests/multi_raft_performance_report --list
./build-default/tests/multi_raft_performance_report --axis smoke --out-dir test_results

# One cloud row, provisioned and torn down (spends money; see the cost estimate)
./scripts/perf-cloud/run-aws-shape-1.sh \
    --binary build-default/tests/multi_raft_http_benchmark_test \
    --instance-type c5.2xlarge --region us-east-1 --repeat 2
```

## Deployment tiers, and which ones exist

Requirement 3.1's five tiers, and their delivery status. Requirement 3.7 asks
that an undelivered tier be recorded as undelivered *with the reason*, rather
than the comparison claim being quietly narrowed to the tiers that were run.

| Tier | What it is | Delivered | Comparable to an external number |
|---|---|---|---|
| A | All hosts in one process over the in-process fabric, memory persistence | yes | **never** (Requirement 3.1) |
| B | In-process hosts, real HTTP transport over loopback, memory persistence | yes | no (Requirement 3.3) |
| C | One host process per node on one machine | **no** | would be, for a no-fsync number |
| D | Tier C plus `file_persistence` in `barrier` mode against a real volume | **no** — but the durability half is built and measured at Tier B, see below | the only tier comparable to a durable number |
| E | One host per machine or container | **no** | yes |

**Why C, D and E are not delivered.** All three need a process that hosts
`multi_raft` and accepts client traffic — a binary in `cmd/`, of which
`cmd/chaos_node` is the nearest precedent and not a substitute. That binary is
Appendix B's third open question and it has not been built. Tier D additionally
needs `file_persistence` plus `tick_batch_controller` wired into the benchmark
harness, whose `persistence_engine_type` is currently a fixed
`memory_persistence_engine` in `tests/multi_raft_transport_harness.hpp`. Neither
is blocked on anything measured here; both are work that was not done.

**One thing reading for Tier D did turn up, and it belongs in front of anyone
sizing a durable deployment.** `tick_batch_controller`'s documentation used to
say that without a controller, `tick()` falls back to per-group batching and
pays one durability barrier per ready group. It does not: nothing in `include/`
or `src/` calls `begin_batch()` except a conditional forwarder that nothing
calls either, so the caller-supplied controller is the only thing in this
codebase that opens a batch. **Without a controller there is no batching at
all** — and for `file_persistence_engine` that is not merely slower, it is not
durable: `append_log_entry` outside a batch flushes the `ofstream` and stops,
and `sync_log_and_directory()` is reached only from `commit_batch()`. A
multi-group host with a file-backed log and no controller writes to the page
cache and issues no barrier at all, which is precisely the `buffered` mode
Requirement 3.5 insists be labelled "not durable" wherever it appears. The
comment is corrected; the fallback is not built, because that is a production
behaviour change and this is a measurement spec.

The consequence is stated once, plainly, and then assumed throughout: **the
like-for-like table below is empty, and it is empty for a structural reason
rather than because nothing was measured.**

## Reference machines

Two, and the difference between them is a finding rather than a footnote.

| | Development machine | Cloud instance (Shape 1) |
|---|---|---|
| CPU | Intel Core i5-6300U @ 2.40 GHz | Intel Xeon Platinum 8124M @ 3.00 GHz |
| vCPU | 4 | 8 |
| Memory | — | 15,502 MiB |
| Kernel | — | 7.0.0-1011-aws |
| Provider | none (workstation) | AWS `c5.2xlarge`, `us-east-1a`, shared tenancy |
| Network (stated) | — | Up to 10 Gigabit |
| Root volume | — | gp3, 3000 IOPS |
| Quiet? | **no, never** | yes |

Machine provenance for every cloud row is captured on the instance itself by
`scripts/perf-cloud/capture-provenance.sh` and travels with the artifact.
Unavailable fields are emitted as `null` rather than omitted or guessed.

**8 vCPU was chosen to match the development machine's core count rather than to
exceed it.** The point of Requirement 18.7 is to remove the hardware confound; a
16-vCPU cloud row against a 4-core local one replaces it with a larger one.

## Results

Every number below is the median of five repetitions of a row, with the
per-repetition spread reported. A row whose throughput spread exceeds 10% is
marked `UNSTABLE` by `repeated_result` and gets **no headline** — that is the
statistical method of Requirement 6 and it is why several development-machine
rows appear here as ratios only.

### The cloud row, and what it changed

Tier B, Beast transport, JSON on the wire, 3 nodes, 4 groups, 16 operations in
flight, 2 ms tick. Two independent runs on one `c5.2xlarge`, five repetitions
per row.

| value size | ops/sec r1 | r2 | ratio vs 16 B | AE/commit r1 | r2 | round interval | entry-sends/commit |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 B | 3636.3 | 3643.0 | 1.00x | 2.77 | 2.78 | 0.79 / 0.78 ms | 13.36 / 13.25 |
| 128 B | 3477.4 | 3515.8 | 0.96 / 0.97x | 2.82 | 2.81 | 0.81 ms | 13.44 / 13.57 |
| 1024 B | 2801.8 | 2782.8 | 0.77 / 0.76x | 2.99 | 2.96 | 0.95 / 0.96 ms | 13.41 / 13.61 |
| 2048 B | 2124.7 | 2110.1 | 0.58x | 3.26 | 3.28 | 1.14 ms | 14.84 / 14.85 |
| 4096 B | 1280.5 | 1218.5 | 0.35 / 0.33x | 3.73 | 3.93 | 1.63 / 1.64 ms | 17.06 / 17.46 |

The same sweep on the development machine, from
`.kiro/specs/multi-raft-performance/tasks.md` task 11a:

| value size | ops/sec r1 | r2 | ratio vs 16 B | AE/commit r1 | r2 |
|---:|---:|---:|---:|---:|---:|
| 16 B | 1289 | 1324 | 1.00x | 3.75 | 3.62 |
| 128 B | 1203 | 1204 | 0.93 / 0.91x | 3.89 | 3.79 |
| 1024 B | 727 | 760 | 0.56 / 0.57x | 4.87 | 4.58 |
| 2048 B | 392 | 414 | 0.30 / 0.31x | 6.97 | 6.38 |
| 4096 B | 61 | 52 | **0.047 / 0.039x** | 30.96 | 35.69 |

**The 4 KiB cliff is a property of the four-core development machine, not of
this implementation.** On four cores the 4 KiB row collapses to about a
twentieth of the 16 B row and entry-bearing rounds per commit rise 8.3–9.9x. On
eight modern cores the same sweep declines smoothly to about a third, and rounds
per commit rise 1.35–1.41x. Both measurements are correct; only one of them is
about the code. This is the clearest thing the cloud row bought, and it is
exactly the confound Requirement 18.7 exists to remove.

**What survives the machine change.** Entries per AppendEntries is 4.44–4.83
across the whole 256-fold value range on the cloud instance, against 4.03–4.89
locally — the batch is invariant to payload size on both machines, at
essentially the same value. That is a structural constant. Write amplification
does not vanish either: **every committed entry still crosses the wire 6.6 to
8.7 times per commit on the fast machine**, against a floor of two (one per
follower). The blow-up to 62–76x is local; the baseline redundancy of roughly
6.6x is not.

### The tick cadence

16 operations in flight, 128 B values.

| tick | cloud ops/sec r1 | r2 | cloud p50 | cloud AE/commit | cloud round interval | local ops/sec r1 | local AE/commit |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 ms | 2997.4 | 3013.7 | 5203 / 5173 us | 3.57 / 3.53 | 0.75 ms | 683.0 | 5.41 |
| 2 ms | 3517.8 | 3488.5 | 4487 / 4514 us | 2.81 / 2.86 | 0.81 / 0.80 ms | 954.6 | 4.16 |
| 5 ms | 3941.5 | 3914.9 | 4017 / 4040 us | 2.34 / 2.36 | 0.87 ms | 1304.2 | 2.85 |
| 20 ms | 4207.4 | 4203.4 | 3760 / 3747 us | 2.10 | 0.91 ms | 1586.8 | 2.23 |

**H6's refutation replicates on different hardware, and its magnitude does
not.** A slower clock still makes everything faster and no percentile shows a
floor at any cadence — the p50 at a 20 ms tick is 3.75 ms, well under a fifth of
the tick period. But the effect is 1.40x on eight cores against 2.3x on four.

**The asymptote is structural and the approach to it is hardware.** RPCs per
committed entry converges to 2.10 on the cloud instance and 2.23 locally, both
approaching the floor of two — one AppendEntries per follower. The 1 ms row
differs sharply between machines (3.53–3.57 against 5.41–6.05) because that is
where the redundant rounds live, and a machine that answers faster issues fewer
of them.

**The per-stream inter-round interval tracks the machine, not the clock.**
0.75–0.91 ms on the cloud instance against 1.89–2.26 ms locally, in each case
flat to within about 20% across a twentyfold change in the tick period. This is
the strongest evidence yet for the response-driven pacing hypothesis: the
interval moved when the machine's RPC round trip moved, and did not move when
the clock moved. It remains a hypothesis — nothing has instrumented the leader's
send path — but it now has two points of support instead of one.

### The wire serializer

128 B values, 16 in flight, Beast transport.

| serializer | cloud ops/sec r1 | r2 | spread |
|---|---:|---:|---:|
| `application/json` | 3532.1 | 3468.9 | 1.9 / 1.3% |
| `application/cbor` | 3608.5 | 3608.1 | 1.9 / 1.4% |
| `application/x-protobuf` | 3604.9 | 3643.5 | 2.6 / 1.0% |

At 128 B the three encodings are within 2.2% of each other and every row is
`stable`. That is H3's "much smaller effect at small values", now measured on a
machine where a 2% difference is inside the noise rather than lost in it. The
Ion row did not run: `KYTHIRA_BENCH_HAS_ION` is undefined without
`CONFIG_ION_SERIALIZER`, and the row says so rather than being absent.

### The encoding knee

| value | cloud JSON ampl | cloud CBOR ampl | cloud CBOR/JSON | local CBOR/JSON |
|---:|---:|---:|---:|---:|
| 1 KiB | 6.83 / 6.71 | 6.72 / 6.63 | 0.99 / 0.99 | 0.87 / 0.85 |
| 2 KiB | 7.38 / 7.46 | 6.97 / 7.06 | 0.94 / 0.95 | 0.70 / 0.71 |
| 4 KiB | 8.71 / 8.58 | 8.00 / 8.15 | 0.92 / 0.95 | 0.30 / 0.58 |

The case's own decision rule is printed beside the table: *a ratio at or near
1.00x says the knee is not about encoded size; materially below 1.00x says it
is.* On the development machine the ratio falls to 0.30–0.58 and the rule says
"encoded size". On the cloud instance it stays between 0.92 and 0.99 and the
rule says "not encoded size".

**Both readings are correct, and the synthesis is the finding.** There is no
knee on the cloud instance for encoding to remove — the JSON amplification curve
there rises a smooth 1.3x over a fourfold value range. Encoded size governs the
*severity of the knee* rather than the knee's existence, and the knee itself is
a CPU-starvation effect: on four cores a larger encoded payload costs more CPU
per round, which lengthens the round, which enlarges the outstanding window the
leader re-sends, which costs more CPU. Remove the starvation and the loop has
nothing to amplify. **Task 11b's conclusion holds on the machine it was measured
on and does not generalise, and the correction is this document's, not a later
reader's.**

### Durability

Three persistence configurations at 128 B, 16 in flight, a 2 ms tick, on the
development machine. Two runs of five repetitions each.

| mode | ops/sec r1 | r2 | p50 r1 | r2 | fsync/sec/host | entries/fsync | barriers | empty batches | entries |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| memory | 1137.2 | 1163.5 | 13564 us | 13214 us | 0.00 | 0.00 | 0 | 0 | 1200 |
| file, buffered — **NOT DURABLE** | 821.1 | 839.0 | 18569 us | 18366 us | 0.00 | 0.00 | 0 | 0 | 1200 |
| file, barrier | 706.3 | 714.0 | 21647 us | 21493 us | 74.8 / 84.5 | 9.45 / 8.45 | 127 / 142 | 1070 / 833 | 1200 |

**This is not Tier D.** Requirement 3.1 defines Tier D as *Tier C plus* file
persistence, and all three hosts here are in one process. The fsync is real and
the process separation is not, so nothing in this table may be compared to an
external durable number.

**The middle row's zeroes are the measurement, not a gap in it.** 1200 entries
were appended and no durability barrier was issued, on both runs, because
`file_persistence_engine::append_log_entry` outside a batch flushes the
`ofstream` and stops. That is the mode Requirement 3.5 insists be labelled not
durable, and it is the configuration anyone gets who wires a file-backed log
into a multi-group host without also supplying a `tick_batch_controller`.

**The cost splits cleanly into two halves.** The JSON-line append alone —
buffered against memory, no fsync in either — is **28% of throughput**. The
barrier on top of it is a further **15%**. Anyone deciding whether durability
is affordable here should note that more than half the cost is the log format
rather than the disk.

**The report generator reproduces these numbers independently.** Its
`--axis durability` rows come from the same shared header through a different
program, and returned 142 barriers, 8.45 entries per fsync and 82.75 fsyncs per
second per host against the suite's 127/142, 9.45/8.45 and 74.8/84.5. That
agreement is the property the shared measurement header exists to have.

**`entries/fsync` of 8.5–9.5, not 1, is why the fsync rate is bearable.** Four
groups at a 2 ms tick would be 2000 barriers per second per host if every tick
flushed; the measurement is 75–85, because 833–1070 of the batches closed with
nothing buffered. A group flushes about once every twelve ticks. Counting those
empty batches as barriers would have reported a system fsyncing twenty-five
times more often, and twenty-five times more cheaply, than it does.

### Reads

Measured on the development machine only; the cloud sweep did not include the
read cases. 1000 preloaded 128 B keys at stride 100, 8 in flight, Tier B.

| kind | consistency | ops/sec | spread | p50 | bytes/op | RPCs/read |
|---|---|---:|---:|---:|---:|---:|
| `read_state` | linearizable (heartbeat quorum) | 2260.2 | 52.1% UNSTABLE | 3268.5 us | 36508 | 3.03 |
| `GET` through the log | linearizable (through the log) | 1298.2 | 4.9% stable | 5939.8 us | 128 | 3.69 |
| local stale | **NOT LINEARIZABLE** | 870369.6 | 46.2% UNSTABLE | **0.8 us** | 128 | **0.00** |

Two of those three rows are `UNSTABLE` and carry no quotable rate. The ratios
are what the rows support: linearizability costs about 7400x in p50 against a
local stale read, and the local read's 332 RPCs across 200,000 reads (all
background heartbeats) is the structural proof of its own "no consensus" label.

`read_state`'s cost against shard size, all three rows stable:

| keys | ops/sec | spread | bytes/key |
|---:|---:|---:|---:|
| 100 | 2448.8 | 6.7% | **146.08** |
| 1000 | 1670.8 | 2.5% | **146.01** |
| 5000 | 597.2 | 5.8% | **146.00** |

### Cost attribution

One committed operation, 128 B, 16 in flight, decomposed by tier difference
(Requirement 8.1). Tier A appears only as the subtrahend and is never comparable
to anything external.

| component | cloud r1 | r2 | share (cloud) | local r1 | share (local) |
|---|---:|---:|---:|---:|---:|
| total, Tier B by key | 4530.0 us | 4518.9 us | — | 16376.3 us | — |
| routing | **NOT RESOLVED** | **NOT RESOLVED** | bound: ≤135.7 / ≤162.9 us | **NOT RESOLVED** | bound: ≤5502.5 us |
| transport + wire serialization | 449.6 us | 403.1 us | 8.9–9.9% | 9395.0 us | 57–58% |
| durability barrier | 0.0 us | 0.0 us | 0% | 0.0 us | 0% |
| consensus core | 4084.5 us | 4099.0 us | 90.2–90.7% | 6719.1 us | 41–42% |

At one operation in flight — the concurrency at which a p50 *is* one operation —
the routing bound tightens further: **under 38.2–39.2 us, at most 11.9–13.3% of
one committed operation**, against 212.8–299.4 us locally. Routing is not where
the time goes, and after four local runs and two cloud runs the bound is the
result rather than a placeholder for one.

**The shift in the transport share is mostly Little's law and should not be read
as "transport got cheap".** At 16 in flight the p50 is dominated by queueing
behind fifteen other operations, so the "consensus core" residual is largely
`concurrency ÷ throughput`. What is real is that transport plus serialization
fell 21x between the two machines while the total fell 3.6x.

## Known structural asymmetries

These are properties of the implementation that any comparison has to be read
against. Each is measured, and none is a bug report — nothing here establishes
that any of them is avoidable.

1. **Write amplification of roughly 6.6x per commit at the small end, on the
   fast machine**, against a floor of two entry-sends per commit. Locally it is
   about 9x. Every throughput claim this system makes has to be read against
   that number.
2. **The batch is set by proposals outstanding per group and by nothing else.**
   Invariant to the tick (twentyfold sweep), invariant to value size (256-fold
   sweep), tracking in-flight per group from 1.03 at one to 48.7 at sixty-four
   in the hot-group arm. Nothing configures it; the coalescing is incidental.
3. **`read_state` returns the whole shard**, at a measured 146.0 bytes per
   stored key, constant to three figures over a fifty-fold range.
4. **A `GET` is a proposal.** It pays a log entry and a replication round, which
   is why the whole-store read beats this system's own point read at a 250-key
   shard.
5. **`node<Types>` serializes on one mutex per group**, and single-group
   throughput never rises with concurrency — it falls monotonically from one
   operation in flight.
6. **`max_concurrent_connections` is inert in all three HTTP transports.**
   Declared, documented as an accept-time counter, read by nothing.

## Structural hypotheses H1–H7

Requirement 11.3. Each verdict, and the number behind it.

| | Hypothesis | Verdict | The number |
|---|---|---|---|
| H1 | No proposal batching — one call, one entry, one round | **REFUTED as stated** | 1.03 entries/AppendEntries at one in flight; 29.5 at 64 uniform, 48.7 Zipfian. Batching is real; nothing configures it. The mechanism task 8 named for it ("tick-driven") is itself refuted — see H6 |
| H2 | Replication rounds are not deduplicated | **REFUTED on concurrency, CONFIRMED on the tick and on value size** | 3.74–6.64 RPCs/commit across a 64-fold concurrency range (does not track N); 5.41 → 2.23 over a twentyfold tick sweep; 6.6–8.7 entry-sends per commit on cloud hardware and 9–76x locally |
| H3 | JSON on the wire costs, a little at small values | **CONFIRMED, with a hardware caveat this document adds** | Encodings within 2.2% at 128 B on both machines. CBOR is 2.5–5.3x faster at 4 KiB *on four cores* and 1.4–1.5x on eight. The effect is real; its size is a property of the machine |
| H4 | The log is JSON lines and lives entirely in memory | **HALF CONFIRMED — the encode cost is measured, the memory growth is not** | A file-backed log with no fsync anywhere in it costs **28% of throughput** against memory persistence (1137/1163 → 821/839 ops/sec, two runs) and 39% more p50. That is the JSON-line append, priced. The `std::map` the same class keeps alongside it is untouched by any measurement here |
| H5 | Reads transfer whole state | **CONFIRMED, linear to three figures** | 146.08 / 146.01 / 146.00 bytes per key at 100 / 1000 / 5000 keys, independently corroborated at 146.03 by a differently-configured row |
| H6 | The tick sets a latency floor | **REFUTED, in the opposite direction** | Every percentile *falls* as the clock slows, on both machines. p50 at a 20 ms tick is 9.4 ms locally and 3.75 ms on cloud hardware — in both cases well under the tick period. No floor at any cadence |
| H7 | Per-group locking is coarse | **CONFIRMED, and the cap is lower than the wording suggests** | Hot-group throughput never rises: 1460.5 → 1403.9 → 1221.1 → 513.1 ops/sec from 1 to 64 in flight. At one in flight the hot group *beats* the spread cluster; the ordering inverts by 64 |

## The comparison

### Like-for-like table

**Empty, and this is a finding rather than an omission.**

Requirement 3.3 forbids a like-for-like comparison from any tier below C under
any circumstances. Every row in this document is Tier A or Tier B. Every
external number in the register was taken on a multi-machine cluster with a real
disk. There is no pair of numbers here that a like-for-like table could
truthfully hold.

Requirement 9.8 asks that a metric with no possible like-for-like comparison say
so explicitly rather than substituting the closest indicative one. Doing that
metric by metric:

| Metric | Like-for-like comparison possible? | Why not, and what it would take |
|---|---|---|
| Committed write throughput | **No** | Needs Tier D: a durable log on a real volume, one host process per node. Every external write number is fsync-durable; ours has no disk in the path at all (`durability barrier: 0.0 us, exactly`) |
| Write latency p99 | **No** | Same, and worse: our p99 is frequently `n/a` because a 400–600-operation measured window does not contain enough samples to estimate it. The window would have to grow as well as the tier |
| `read_state` (whole-shard read) | **No** | No external source in the register publishes a whole-store read at all. The metric has no counterpart, not merely no matching configuration |
| Linearizable point read | **No** | etcd's linearizable read uses ReadIndex; ours is submitted as a proposal through the log. These are different mechanisms with the same consistency label, which is an indicative comparison at best |
| Non-linearizable read | **No** | etcd's serializable read is a replica read like ours, which makes it the *closest* pair in this document — and it is still one process against a cluster |
| Entries per AppendEntries | **No** | No external source in the register states it. This is a ratio we can measure and nobody publishes |
| RPCs per committed entry | **No** | Same |

### Indicative table

Every row is indicative, for at least the two reasons stated in every cell: the
tier mismatch (3.3) and, where the external system is a database, the storage
engine included in its number (9.5). Requirement 9.6 forbids a bare multiplier
anywhere, including here, so each gap is stated with its tier, its class and its
mismatched axes.

| Ours | Theirs | Gap, with its axes |
|---|---|---|
| 3,477–3,516 ops/sec committed writes, 128 B values, Tier B, 3 nodes, 4 groups, 16 in flight, no durability, one 8-vCPU machine | etcd 3.2.0: 44,341 QPS, 256 B values, 3 GCE machines of 8 vCPU each, 100 connections / 1,000 clients, durability not stated | About **an eighth of etcd's rate at roughly a sixtieth of the client concurrency**, and that pairing is not a like-for-like: theirs is three machines and a storage engine, ours is one process and no disk; theirs is 1,000 clients and ours is 16. The concurrency mismatch alone probably dominates the ratio, and this document cannot say by how much |
| 3,477–3,516 ops/sec, 4 Raft groups, 16 in flight, no durability, 8 vCPU | Dragonboat v3: 9,000,000 writes/sec, 16 B values, 48 Raft shards on 3 servers of 22 cores each, fsync strictly honoured, client count not stated | **Three orders of magnitude**, across every axis at once: 66 cores against 8, 48 groups against 4, NVMe-backed fsync against memory, and a client concurrency the source does not state. Reported because Requirement 9.4 names Dragonboat as the closest peer *in kind*; it is not evidence about this implementation's efficiency and should not be read as such |
| 1,289 ops/sec single-group hot arm at 1 in flight (development machine, Tier B) | Dragonboat v3: 1,250,000/sec single group, 16 B, 3 cores at 2.8 GHz per server, fsync honoured | Same caveats. The one axis that *does* match is the group count |
| p99 write latency: **not measured** — `n/a` in most rows for want of samples | Dragonboat v3: "<5ms P99 write latency when handling 8 million writes per second at 16 bytes each" | No comparison is drawn. Ours is absent, not large; theirs is a bound, not a value |
| Linearizable point read 1,298.2 ops/sec, p50 5,939.8 us, Tier B, 8 in flight | etcd 3.2.0 linearizable: 1,353 QPS at 1 connection / 1 client, 0.7 ms | The *rates* are within a few percent and the latencies are not, which is a coincidence of concurrency rather than a result: theirs is one client, ours is eight. Read the p50 column and not the ops/sec column |
| Local stale read 870,369.6 ops/sec, p50 0.8 us, **NOT LINEARIZABLE**, and the row is UNSTABLE at 46.2% | etcd 3.2.0 serializable: 2,909 QPS at 1 connection / 1 client, 0.3 ms | Ours is an in-process call on the same machine as the client; etcd's crosses a network. This pair is here to show the shape of the comparison, not its size, and our row carries no quotable rate |
| Point-get read | TiKV 6.1: 212,000 OPS, workload C, 3 nodes, 40 vCPU each | Not compared. TiKV's number includes RocksDB, its payload size and client concurrency are both unstated, and ours is a proposal through the log. Three unstated axes is too many for a ratio to mean anything |
| Anything | braft | **No comparison is possible.** braft publishes its benchmark results only as PNG charts; Requirement 9.3 forbids using a number that cannot be sourced to a retrievable document, and a chart read by eye is not one. Its configuration is in the register; its numbers are not |

### What the largest gaps are attributable to

Requirement 16.3 asks for the cost attribution that explains the largest gaps
and the hypothesis each confirms or refutes.

1. **Concurrency and cluster size, not efficiency, dominate the etcd gap.** Our
   row is 16 operations in flight on one machine; etcd's is 1,000 clients over
   three. Nothing in the decomposition explains an eightfold rate difference —
   the decomposition says transport is 9% of an operation and routing is under
   13%, so there is no 8x of overhead to find. The gap is a configuration
   difference this document could close by measuring at higher concurrency and
   at Tier C, and did not.
2. **Write amplification is the one attributable structural cost.** 6.6–8.7
   entry-sends per commit against a floor of two means the replication path
   moves three to four times the bytes it must. That is H2's redundancy, and it
   is the largest single quantity in this document that is a property of the
   implementation rather than of the machine or the configuration.
3. **The 4 KiB collapse is not attributable to the implementation at all**, and
   two sessions of work concluded that it was. Eight cores remove it. This is
   the finding that most changed as a result of measuring on a second machine,
   and it is the reason Requirement 18.7 asks for that machine.

## What this document could not answer

Requirement 16.5, stated plainly rather than filled in with the nearest
available number.

- **Whether this implementation is competitive on a durable write.** The
  durability axis above has a real fsync in it, which is new, but it is Tier B —
  three hosts in one process, on a laptop SSD. Every external write number is a
  cluster of machines. This is still the single most important open question and
  it still needs Tier C underneath it.
- **How much of the 28% JSON-line cost is the format and how much is the
  `std::map` beside it.** H4 names both; only the first is priced.
- **What a real network does to the round.** The inter-round interval is
  0.75–0.91 ms on loopback and the response-driven-pacing hypothesis predicts it
  tracks the RPC round trip. On a real network that is 100x larger. Nothing here
  tested it; Tier E would.
- **Whether the write amplification is avoidable.** It is measured, repeatedly
  and on two machines. Nothing establishes that a different send path would
  remove it, and this suite deliberately has no byte counter on that path
  (Requirement 8.2).
- **p99 write latency, at all.** Most rows report `n/a`: a 400–600-operation
  window does not contain enough samples. The budget would have to grow.
- **Whether the response-driven pacing hypothesis is true.** It now fits two
  axes and two machines. It has never been measured directly, because that needs
  instrumentation on the leader's send path.
- **Anything about arm64, a second cloud provider, or Tier D/E.** Recorded as
  not delivered in the tier table above, with reasons.

## Follow-on work the measurements motivate

Requirement 16.4: ordered by measured cost, each a candidate spec, none of it
implemented here.

1. **A host binary in `cmd/`.** Unblocks Tiers C, D and E simultaneously, which
   is every tier that could carry a like-for-like claim. It is the single
   highest-value item in this list and nothing else in it matters as much.
2. **Run the durability axis on a cloud instance, against a provisioned
   volume.** The harness half is done — `durability_mode`, the fan-out
   controller, the barrier and entry counters, and the row above — so what
   remains is a volume whose class and IOPS the provenance records, which
   `kv_cluster_options::_data_dir` already accepts and nothing has yet used.
   Cheap, and it turns the barrier column from a laptop SSD's number into one.
3. **Decide whether `tick()` should batch without a controller.** Separate from
   the above and larger than it: today a multi-group host with a file-backed log
   and no controller fsyncs once per appended entry. Whether that is the
   intended contract or an omission is a design question this measurement work
   is not entitled to answer.
4. **Instrument the leader's send path.** Settles response-driven pacing and
   would say whether the 6.6x write amplification is a design property or an
   accident. Requirement 8.2 keeps a byte counter out of production code, so
   this needs a design decision before it needs a measurement.
5. **Sweep client concurrency past 64, at Tier C.** The etcd comparison is
   dominated by a concurrency mismatch that is cheap to remove.
6. **A larger measured window, for p99.** The cheapest item here and the one
   that removes the most `n/a` cells.

## Out of scope

- **Tuning.** Nothing in this document was tuned for. Every row uses the same
  cluster shape, the same budgets and the same warm-up rule, and that constancy
  is worth more than any single number in it.
- **Backend comparison.** The matrix builds and runs under folly, boost and
  stdexec, which is a portability claim and not a performance one. Three numbers
  from three backends on one row of forty operations is not a comparison and is
  not presented as one.
- **Locally re-running an external system.** Requirement 10 permits it; nothing
  here does it.

## The external comparison register

Requirement 11.4 asks for the register in full rather than a summary. It is
maintained as machine-readable data at
[`doc/data/multi_raft_external_comparison_register.json`](data/multi_raft_external_comparison_register.json)
and rendered below by `scripts/render-external-register.py`, so the document and
the data cannot drift apart.

Every record carries every field of Appendix A. A field the source does not
state is written **not stated** and is never inferred (9.2). A number that
cannot be sourced to a retrievable document is not entered at all (9.3) — the
braft record below exists to say exactly that.

<!-- BEGIN GENERATED REGISTER: scripts/render-external-register.py -->

*14 records. Generated from `doc/data/multi_raft_external_comparison_register.json` by `scripts/render-external-register.py`; edit the JSON, not this block.*

#### `etcd-3.2.0-write-1conn`

| Field | Value |
|---|---|
| Implementation, version | etcd 3.2.0 (published on the v3.5 documentation site; the benchmark itself was run against 3.2.0 with Go 1.8.3) |
| Kind | database |
| Source URL or document | https://etcd.io/docs/v3.5/op-guide/performance/ |
| Date retrieved | 2026-08-30 |
| Author of the number | the project itself |
| Hardware | Google Cloud Compute Engine. 3 etcd servers, each 8 vCPU, 16 GB memory, 50 GB SSD. 1 client machine, 16 vCPU, 30 GB memory, 50 GB SSD. Ubuntu 17.04. CPU model not stated; network performance not stated; SSD class and IOPS not stated. |
| Cluster size / replication factor | 3 members; replication factor 3 (every etcd member holds the full log) |
| Raft group count | 1 |
| Payload size | key 8 bytes, value 256 bytes |
| Read/write mix | 100% write |
| Client concurrency | 1 connection, 1 client |
| Durability | not stated on the page (etcd fsyncs its WAL by default, but the page does not say so and 9.2 forbids inferring it) |
| Batching configuration | not stated |
| Metric and unit as stated | 10,000 keys, target Leader: 583 QPS, average latency 1.6 ms, 48 MB memory |
| Comparison class | indicative — external system is a database with its own WAL and boltdb storage engine (9.5); our tiers below C cannot carry a like-for-like claim at all (3.3) |

#### `etcd-3.2.0-write-100conn-leader`

| Field | Value |
|---|---|
| Implementation, version | etcd 3.2.0 (published on the v3.5 documentation site) |
| Kind | database |
| Source URL or document | https://etcd.io/docs/v3.5/op-guide/performance/ |
| Date retrieved | 2026-08-30 |
| Author of the number | the project itself |
| Hardware | Google Cloud Compute Engine. 3 etcd servers, each 8 vCPU, 16 GB memory, 50 GB SSD. 1 client machine, 16 vCPU, 30 GB memory, 50 GB SSD. Ubuntu 17.04. CPU model not stated; network performance not stated; SSD class and IOPS not stated. |
| Cluster size / replication factor | 3 members; replication factor 3 |
| Raft group count | 1 |
| Payload size | key 8 bytes, value 256 bytes |
| Read/write mix | 100% write |
| Client concurrency | 100 connections, 1,000 clients |
| Durability | not stated |
| Batching configuration | not stated |
| Metric and unit as stated | 100,000 keys, target Leader: 44,341 QPS, average latency 22 ms, 124 MB memory |
| Comparison class | indicative — database with a storage engine (9.5); tier mismatch (3.3); client concurrency 1,000 against our 1–64 |

#### `etcd-3.2.0-write-100conn-all-members`

| Field | Value |
|---|---|
| Implementation, version | etcd 3.2.0 (published on the v3.5 documentation site) |
| Kind | database |
| Source URL or document | https://etcd.io/docs/v3.5/op-guide/performance/ |
| Date retrieved | 2026-08-30 |
| Author of the number | the project itself |
| Hardware | Google Cloud Compute Engine. 3 etcd servers, each 8 vCPU, 16 GB memory, 50 GB SSD. 1 client machine, 16 vCPU, 30 GB memory, 50 GB SSD. Ubuntu 17.04. CPU model not stated; network performance not stated; SSD class and IOPS not stated. |
| Cluster size / replication factor | 3 members; replication factor 3 |
| Raft group count | 1 |
| Payload size | key 8 bytes, value 256 bytes |
| Read/write mix | 100% write |
| Client concurrency | 100 connections, 1,000 clients |
| Durability | not stated |
| Batching configuration | not stated |
| Metric and unit as stated | 100,000 keys, target All members: 50,104 QPS, average latency 20 ms, 126 MB memory |
| Comparison class | indicative — database with a storage engine (9.5); tier mismatch (3.3) |

#### `etcd-3.2.0-read-linearizable-1conn`

| Field | Value |
|---|---|
| Implementation, version | etcd 3.2.0 (published on the v3.5 documentation site) |
| Kind | database |
| Source URL or document | https://etcd.io/docs/v3.5/op-guide/performance/ |
| Date retrieved | 2026-08-30 |
| Author of the number | the project itself |
| Hardware | Google Cloud Compute Engine. 3 etcd servers, each 8 vCPU, 16 GB memory, 50 GB SSD. 1 client machine, 16 vCPU, 30 GB memory, 50 GB SSD. Ubuntu 17.04. |
| Cluster size / replication factor | 3 members; replication factor 3 |
| Raft group count | 1 |
| Payload size | key 8 bytes, value 256 bytes |
| Read/write mix | 100% read, Linearizable consistency |
| Client concurrency | 1 connection, 1 client |
| Durability | not applicable to a read; not stated |
| Batching configuration | not stated |
| Metric and unit as stated | 10,000 requests, Linearizable: 1,353 QPS, average latency 0.7 ms |
| Comparison class | indicative — database (9.5); the nearest of our rows is the read taxonomy's GET-through-the-log, which is a different mechanism (a proposal) from etcd's ReadIndex |

#### `etcd-3.2.0-read-serializable-1conn`

| Field | Value |
|---|---|
| Implementation, version | etcd 3.2.0 (published on the v3.5 documentation site) |
| Kind | database |
| Source URL or document | https://etcd.io/docs/v3.5/op-guide/performance/ |
| Date retrieved | 2026-08-30 |
| Author of the number | the project itself |
| Hardware | Google Cloud Compute Engine. 3 etcd servers, each 8 vCPU, 16 GB memory, 50 GB SSD. 1 client machine, 16 vCPU, 30 GB memory, 50 GB SSD. Ubuntu 17.04. |
| Cluster size / replication factor | 3 members; replication factor 3 |
| Raft group count | 1 |
| Payload size | key 8 bytes, value 256 bytes |
| Read/write mix | 100% read, Serializable consistency |
| Client concurrency | 1 connection, 1 client |
| Durability | not applicable to a read; not stated |
| Batching configuration | not stated |
| Metric and unit as stated | 10,000 requests, Serializable: 2,909 QPS, average latency 0.3 ms |
| Comparison class | indicative — database (9.5); this is the nearest external analogue of our local stale read, which is likewise not linearizable |

#### `etcd-3.2.0-read-linearizable-100conn`

| Field | Value |
|---|---|
| Implementation, version | etcd 3.2.0 (published on the v3.5 documentation site) |
| Kind | database |
| Source URL or document | https://etcd.io/docs/v3.5/op-guide/performance/ |
| Date retrieved | 2026-08-30 |
| Author of the number | the project itself |
| Hardware | Google Cloud Compute Engine. 3 etcd servers, each 8 vCPU, 16 GB memory, 50 GB SSD. 1 client machine, 16 vCPU, 30 GB memory, 50 GB SSD. Ubuntu 17.04. |
| Cluster size / replication factor | 3 members; replication factor 3 |
| Raft group count | 1 |
| Payload size | key 8 bytes, value 256 bytes |
| Read/write mix | 100% read, Linearizable consistency |
| Client concurrency | 100 connections, 1,000 clients |
| Durability | not applicable to a read; not stated |
| Batching configuration | not stated |
| Metric and unit as stated | 100,000 requests, Linearizable: 141,578 QPS, average latency 5.5 ms |
| Comparison class | indicative — database (9.5); client concurrency 1,000 against our 8 |

#### `etcd-3.2.0-read-serializable-100conn`

| Field | Value |
|---|---|
| Implementation, version | etcd 3.2.0 (published on the v3.5 documentation site) |
| Kind | database |
| Source URL or document | https://etcd.io/docs/v3.5/op-guide/performance/ |
| Date retrieved | 2026-08-30 |
| Author of the number | the project itself |
| Hardware | Google Cloud Compute Engine. 3 etcd servers, each 8 vCPU, 16 GB memory, 50 GB SSD. 1 client machine, 16 vCPU, 30 GB memory, 50 GB SSD. Ubuntu 17.04. |
| Cluster size / replication factor | 3 members; replication factor 3 |
| Raft group count | 1 |
| Payload size | key 8 bytes, value 256 bytes |
| Read/write mix | 100% read, Serializable consistency |
| Client concurrency | 100 connections, 1,000 clients |
| Durability | not applicable to a read; not stated |
| Batching configuration | not stated |
| Metric and unit as stated | 100,000 requests, Serializable: 185,758 QPS, average latency 2.2 ms |
| Comparison class | indicative — database (9.5); client concurrency 1,000 against our 8 |

#### `dragonboat-v3-multigroup-write`

| Field | Value |
|---|---|
| Implementation, version | Dragonboat v3 (the README describes v3 as the latest release; the hardware document is docs/test.md on master) |
| Kind | library |
| Source URL or document | https://github.com/lni/dragonboat and https://github.com/lni/dragonboat/blob/master/docs/test.md |
| Date retrieved | 2026-08-30 |
| Author of the number | the project itself |
| Hardware | "Three servers each with a single 22-core Intel XEON E5-2696v4 processor, all cores can boost to 2.8Ghz", "40GE Mellanox NIC", "Intel 900P for storing the RocksDB's WAL and Intel P3700 1.6T for storing all other data", "Ubuntu 16.04 with Spectre and Meltdown patches, ext4 file-system". Memory not stated. |
| Cluster size / replication factor | 3 servers; replication factor not stated (three NodeHost instances across three servers implies 3, but the source does not say it) |
| Raft group count | "48 Raft shards on three NodeHost instances across three servers" |
| Payload size | 16 bytes value; key size not stated (16, 128 and 1024 byte payloads were all tested) |
| Read/write mix | 100% write for the 9 million figure; 9:1 read:write for the 11 million figure |
| Client concurrency | not stated as a count — "Each request is handled in its own goroutine" |
| Durability | "fsync is strictly honored" |
| Batching configuration | not stated |
| Metric and unit as stated | "9 million writes per second when the payload is 16bytes each or 11 million mixed I/O per second at 9:1 read:write ratio" |
| Comparison class | indicative — the closest peer in kind (a multi-group Raft library), but every axis except payload size is mismatched: 22 cores per node against our 8, NVMe-backed fsync against our memory persistence, 48 groups against our 4, and a client concurrency the source does not state |

#### `dragonboat-v3-singlegroup-write`

| Field | Value |
|---|---|
| Implementation, version | Dragonboat v3 |
| Kind | library |
| Source URL or document | https://github.com/lni/dragonboat and https://github.com/lni/dragonboat/blob/master/docs/test.md |
| Date retrieved | 2026-08-30 |
| Author of the number | the project itself |
| Hardware | As above. The README additionally states CPU usage of "3 cores (2.8GHz) on each server" for this figure. |
| Cluster size / replication factor | 3 servers; replication factor not stated |
| Raft group count | 1 |
| Payload size | 16 bytes value; key size not stated |
| Read/write mix | 100% write |
| Client concurrency | not stated |
| Durability | "fsync is strictly honored" |
| Batching configuration | not stated |
| Metric and unit as stated | "1.25 million per second when payload is 16 bytes each, average latency is 1.3ms and the P99 latency is 2.6ms" |
| Comparison class | indicative — single-group library figure, which is the closest in shape to our Zipfian hot-group arm, but fsync-durable against our memory persistence and on 22-core hardware |

#### `dragonboat-v3-p99-at-8m`

| Field | Value |
|---|---|
| Implementation, version | Dragonboat v3 |
| Kind | library |
| Source URL or document | https://github.com/lni/dragonboat |
| Date retrieved | 2026-08-30 |
| Author of the number | the project itself |
| Hardware | As above; the README's latency table does not restate it. |
| Cluster size / replication factor | 3 servers; replication factor not stated |
| Raft group count | not stated for this figure |
| Payload size | 16 bytes each |
| Read/write mix | 100% write |
| Client concurrency | not stated |
| Durability | "fsync is strictly honored" |
| Batching configuration | not stated |
| Metric and unit as stated | "<5ms P99 write latency when handling 8 million writes per second at 16 bytes each" |
| Comparison class | indicative — this is the only external p99 write figure in the register, and it is stated as a bound ("<5ms") rather than a value |

#### `dragonboat-v3-high-rtt`

| Field | Value |
|---|---|
| Implementation, version | Dragonboat v3 |
| Kind | library |
| Source URL or document | https://github.com/lni/dragonboat |
| Date retrieved | 2026-08-30 |
| Author of the number | the project itself |
| Hardware | As above, with "30ms" RTT between nodes; the README does not say how the RTT was produced (real distance or injected). |
| Cluster size / replication factor | 3 servers; replication factor not stated |
| Raft group count | not stated for this figure |
| Payload size | not stated for this figure |
| Read/write mix | "I/O" — mix not stated for this figure |
| Client concurrency | "a much larger number of clients" — count not stated |
| Durability | "fsync is strictly honored" |
| Batching configuration | not stated |
| Metric and unit as stated | "2 million I/O per second can still be achieved using a much larger number of clients" at 30 ms RTT |
| Comparison class | indicative, and thin — payload size, mix and client count are all unstated, so this record exists mainly to bound what a WAN figure looks like for a library of this kind |

#### `braft-master-no-usable-number`

| Field | Value |
|---|---|
| Implementation, version | braft, master branch (no version is stated on the benchmark document) |
| Kind | library |
| Source URL or document | https://github.com/baidu/braft/blob/master/docs/cn/benchmark.md |
| Date retrieved | 2026-08-30 |
| Author of the number | the project itself |
| Hardware | 12 cores, Intel Xeon E5-2620 v2 @ 2.10GHz; LENOVO SAS 300G (approximately 800 IOPS random write, approximately 200 MB/s sequential write); 10 Gigabit NIC with multi-queue not enabled. Memory not stated. |
| Cluster size / replication factor | 3 replicas |
| Raft group count | not stated |
| Payload size | 512 bytes among others; the full set of sizes is only legible from the charts |
| Read/write mix | 100% write (synchronous RPC writes committed through the Raft group) |
| Client concurrency | 100 threads |
| Durability | sync enabled (the document states sync is on by default for all of these tests) |
| Batching configuration | not stated |
| Metric and unit as stated | NO NUMBER IS ENTERED. braft publishes its QPS and latency results only as images (benchmark0.png, benchmark.png); the document's text carries the configuration but no figure. Requirement 9.3 forbids using a number that cannot be sourced to a retrievable document, and a chart read by eye is not one. |
| Comparison class | no comparison — the C++ multi-group peer Requirement 9.4 asks for is present in the register as a configuration record and as a stated absence, not as a number |

#### `tikv-6.1-rawkv-point-get`

| Field | Value |
|---|---|
| Implementation, version | TiKV 6.1 |
| Kind | database |
| Source URL or document | https://tikv.org/docs/6.1/deploy/performance/overview/ |
| Date retrieved | 2026-08-30 |
| Author of the number | the project itself |
| Hardware | Virtual machines. Intel(R) Xeon(R) CPU E5-2630 v4 @ 2.20GHz, 40 vCPU, 64 GiB memory, 500 GiB NVMe SSD. Network not stated. |
| Cluster size / replication factor | 3 TiKV nodes; replication factor not stated. PD node count and client node count not stated on this page. |
| Raft group count | not stated — TiKV shards into Regions and the page does not say how many the 10M-record dataset produced |
| Payload size | not stated (go-ycsb defaults are not restated on the page) |
| Read/write mix | workload C (100% read) for the point-get figure |
| Client concurrency | not stated in the text — the page varies it across figures without listing the values |
| Durability | not stated |
| Batching configuration | not stated |
| Metric and unit as stated | "212,000 OPS" point-get read, workload C, 10M records and 10M operations, measured with GO YCSB; "average latency less than 10 milliseconds" |
| Comparison class | indicative — TiKV is a database: the number includes RocksDB and, on the transactional path, a transaction layer (9.5). This is also the implementation this project's multi-raft design is explicitly drawn from, which makes it the most interesting record and not the most comparable one. |

#### `tikv-6.1-rawkv-update`

| Field | Value |
|---|---|
| Implementation, version | TiKV 6.1 |
| Kind | database |
| Source URL or document | https://tikv.org/docs/6.1/deploy/performance/overview/ |
| Date retrieved | 2026-08-30 |
| Author of the number | the project itself |
| Hardware | Virtual machines. Intel(R) Xeon(R) CPU E5-2630 v4 @ 2.20GHz, 40 vCPU, 64 GiB memory, 500 GiB NVMe SSD. Network not stated. |
| Cluster size / replication factor | 3 TiKV nodes; replication factor not stated |
| Raft group count | not stated |
| Payload size | not stated |
| Read/write mix | workload A (50% read, 50% update) for the update figure |
| Client concurrency | not stated |
| Durability | not stated |
| Batching configuration | not stated |
| Metric and unit as stated | "43,200 OPS" update, workload A, 10M records and 10M operations, measured with GO YCSB. The p99 latency is published as a figure only and is not entered. |
| Comparison class | indicative — database with a storage engine (9.5); payload size and client concurrency both unstated, which are the two axes a write comparison most needs |

<!-- END GENERATED REGISTER -->

## See Also

- `.kiro/specs/multi-raft-performance/` — requirements, design and the task log
  every number here came from
- [doc/multi_raft_performance_cloud_cost_estimate.md](multi_raft_performance_cloud_cost_estimate.md)
  — the pre-registered cost of every cloud shape
- [doc/future_backend_performance_comparison.md](future_backend_performance_comparison.md)
  — the report structure this document follows
- [doc/http_transport_performance_comparison.md](http_transport_performance_comparison.md)
  — the transport comparison, including the gRPC rows H3 draws on
- [doc/protobuf_serializer_performance_comparison.md](protobuf_serializer_performance_comparison.md)
  — serializer wire size and throughput
