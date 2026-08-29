# Requirements Document

## Introduction

This specification defines a performance benchmark suite for the multi-Raft
host (`include/raft/multi_raft.hpp`, `multi_raft_impl.hpp`, merged to `main`
on August 27, 2026) driven by a **key/value store payload**, and a
**documented comparison** of the numbers it produces against the published
numbers of the other multi-Raft implementations this design was drawn from.

The question the suite exists to answer is narrow and was asked directly:
*how close are we to the other multi-Raft implementations?* Answering it
honestly is harder than measuring, because almost every published multi-Raft
number differs from ours on at least one axis that moves throughput by an
order of magnitude — durability mode, payload size, batching, client
concurrency, replication factor, and whether a "read" is a point lookup or a
whole-state transfer. A benchmark that produces a single ops/sec figure and
sets it beside somebody's README number is not a comparison; it is a
coincidence with a table around it. So this spec is as much about **what must
be recorded and disclosed** as about what must be measured.

Two axes are swept rather than chosen. This project ships three HTTP transports
and four RPC serializers, and picking one of each would make the headline number
a property of that pick — so the suite measures the **matrix** (Requirement 17),
labels every row with the transport and the serializer's own media type, and is
built so that a transport added later joins by supplying one fixture. CoAP is the
named future row: it has no multi-Raft binding today, and its documented lack of
TimeoutNow is the same conditional-capability case the HTTP transports already
exercise.

The suite is **measurement-only**. It does not change the consensus core, the
host, the serializers, or any default. Where a measurement exposes a
structural gap — and §"Structural hypotheses" below names seven the code
already suggests — the gap is *recorded with its cost*, and closing it is a
separate spec with its own requirements. This separation is deliberate: a
benchmark whose author is simultaneously optimizing the subject stops being an
instrument.

### Why the key/value payload

`kythira::test_key_value_state_machine` (`include/raft/test_state_machine.hpp:24`)
is the right subject for three reasons:

1. It is the payload every other multi-Raft implementation publishes against.
   TiKV, CockroachDB, Dragonboat, braft and etcd all report KV numbers, so a
   KV workload is the only one where a comparison is even in principle
   possible.
2. It is already the state machine the multi-Raft suites use, including
   `multi_raft_scale_test` — so the benchmark measures the same code path the
   correctness tests cover, not a benchmark-only fixture.
3. It is `splittable_state_machine`-complete (`split_state`, `absorb`,
   `approximate_size_bytes`, `suggest_split_keys` at
   `test_state_machine.hpp:285,304,349,383`), which means the split/merge
   machinery — the part of this design that is *distinctively* multi-Raft — can
   be exercised under load rather than only in functional tests.

### What is already known about the subject

These are facts read out of the tree, not assumptions, and the design phase
must not re-derive them:

- **The write path is `submit_command(key, command, timeout)`**
  (`include/raft/multi_raft_impl.hpp:2816`), which routes through
  `route_and_run` — shard-map lookup, epoch validation, cross-shard
  partitioner check, merge-freeze check, leadership check, load sampling —
  and then calls `node<Types>::submit_command`
  (`include/raft/raft.hpp:1573`).
- **A submission replicates inline on the caller's thread.**
  `node::submit_command` appends one entry, registers with the commit waiter,
  releases the lock and calls `replicate_to_followers()` directly
  (`include/raft/raft.hpp:1765`). There is no proposal queue and no
  accumulation window: **one client call produces one log entry and one
  replication round.**
- **Coalescing is incidental, not designed.** `send_append_entries_to` ships
  everything from `_next_index` up to `_max_entries_per_append` (default
  **100**, `include/raft/types.hpp:774`), so concurrent submissions to one
  group *do* end up in one AppendEntries — but each submission still issues
  its own replication round, so the RPCs-per-committed-entry ratio is an
  open, measurable quantity rather than a designed one.
- **The tick is the clock.** `multi_raft::tick()`
  (`multi_raft_impl.hpp:566`) runs three ordered phases — persist, send,
  apply — and reports each one's duration in `tick_report`. Nothing inside
  the library drives it; the caller owns the loop, as `multi_raft_scale_test`
  does. Tick cadence is therefore a *benchmark parameter* and a latency floor
  for anything that waits on a heartbeat.
- **Durability is a mode, not a constant.** `file_persistence::append_log_entry`
  outside a batch appends a JSON line and **does not fsync**
  (`include/raft/file_persistence.hpp:103`); only `commit_batch` issues the
  barrier (`file_persistence.hpp:156`, `sync_log_and_directory` at `:348`).
  `tick_batch_controller` (`multi_raft.hpp:138`, wired at `:428`) turns one
  tick over N groups into one barrier. `memory_persistence_engine` has no
  durability at all.
- **`read_state` is not a point read.** `node<Types>::read_state`
  (`raft.hpp:2396`) confirms leadership with a heartbeat quorum and then
  returns `_state_machine.get_state()` (`raft.hpp:2455,2655`) — the **entire
  serialized store**. Its cost scales with shard size, which is exactly why
  `_p99_read_latency` was excluded as a split trigger. A KV point read at
  comparable semantics must go through the log as a `GET` command
  (`test_state_machine.hpp:241`), or be served as an explicitly stale local
  read (`get_value`, `:203`).
- **No multi-Raft test has ever run over a real transport.** Every multi-Raft
  suite uses the in-process `message_fabric`
  (`tests/multi_raft_test_fabric.hpp`). The TCP/TLS/gRPC transports carry
  `group_id` and `TimeoutNow`, but nothing has driven `multi_raft` through a
  socket. Any number compared to a published number must come from a
  configuration that has one.
- **The three HTTP transports implement only the three mandatory RPCs.**
  `cpp_httplib_client`, `boost_beast_client` and `proxygen_client` expose
  `send_request_vote`, `send_append_entries` and `send_install_snapshot` and
  nothing else; their servers register the matching three handlers. None
  satisfies `network_client_with_pre_vote`, `..._with_log_fetch` or
  `..._with_timeout_now`. `multi_group_network_server::start()` installs the
  optional handlers behind `if constexpr` on exactly those concepts
  (`group_transport.hpp:257,272,313`), so an HTTP row simply has no leadership
  transfer — **the same class of limitation as CoAP's documented lack of
  TimeoutNow**, reached by the same mechanism.
- **None of the real transports is movable.** `boost_beast_client` and
  `proxygen_client` delete their move constructors outright; `cpp_httplib_client`
  holds a `std::mutex` and a `std::jthread`. `multi_raft_config` holds
  `network_client` / `network_server` **by value** and `multi_raft` moves them
  into place, so a real transport reaches the host only through a movable
  handle. That handle is harness code, not a production change.

### Non-goals

- **Declaring a winner, or a target.** The deliverable is a gap table with
  provenance, not a claim of parity and not a performance goal.
- **Optimizing anything.** No change to `raft.hpp`, `multi_raft*.hpp`, the
  serializers, or the transports is in scope, including changes that a
  measurement obviously motivates.
- **Reproducing anyone else's benchmark harness.** Running a competitor
  locally is optional (Requirement 10) and, if done, is a separate class of
  result from a published one.
- **Micro-benchmarks of components already covered elsewhere.**
  `future_backend_benchmark_test`, `protobuf_json_benchmark_test` and
  `raft_comprehensive_performance_benchmark` exist; this suite cites them for
  attribution rather than re-measuring them.
- **A YCSB or sysbench implementation.** The workload is defined here, in
  terms this system can actually serve; borrowing a workload name we do not
  faithfully implement would be a provenance claim we cannot support.

## Glossary

- **Host**: one `multi_raft` instance — one process's worth of groups, one
  shared transport, one striped executor.
- **Group / shard**: one Raft group and the key range it owns. Used
  interchangeably, matching the design's own usage.
- **Tier**: a deployment configuration of the benchmark (A–E, Requirement 3)
  that fixes transport, process boundary and durability. A number is
  meaningless without its tier.
- **Durability mode**: one of `none` (memory persistence), `buffered`
  (`file_persistence`, no fsync per append), or `barrier` (`file_persistence`
  with `tick_batch_controller`, one fsync per tick per host).
- **Offered load**: the rate at which the driver *attempts* operations,
  independent of completions. The open-loop control variable.
- **In-flight cap**: the maximum number of un-acknowledged client operations
  the driver permits. The closed-loop control variable.
- **Coordinated omission**: the measurement error in which a stalled system
  stops receiving requests, so the latencies of the requests it *would* have
  received are never sampled and the reported tail is fiction. Avoided by
  scheduling each operation against a fixed intended start time and measuring
  from that time, not from dispatch.
- **Pre-registration**: writing down the metric, its unit, its expected
  magnitude and the acceptance rule *before* the run that produces it.
  Doctrine, from `.kiro`'s own history: a number invented after seeing the
  data is not a measurement.
- **Like-for-like comparison**: a comparison in which payload size, cluster
  size, replication factor, durability mode, read/write semantics and client
  concurrency all match the external source, and both sides are libraries (or
  both are databases). Anything else is **indicative**.
- **Indicative comparison**: a comparison with at least one named, material
  axis mismatch. Permitted, but must carry the mismatch list wherever the
  number appears.
- **Gap ratio**: external number ÷ our number, for the same metric, always
  stated with the tier and comparison class.
- **Cost attribution**: a decomposition of one operation's cost into named
  components summing (within a stated residual) to the measured total.
- **Comparison target**: an external implementation whose published numbers
  are entered in the comparison register (Requirement 9).

## Structural hypotheses

Seven properties of the current implementation are visible in the code and
are expected to dominate any gap. They are listed here as **hypotheses to be
confirmed or refuted by measurement**, so that the design phase builds a
harness capable of testing them, and so that nobody later mistakes a guess
for a finding:

- **H1 — No proposal batching.** One client call, one log entry, one
  replication round (`raft.hpp:1573,1765`). Implementations that accumulate
  proposals over a window amortize the persist barrier and the RPC across
  hundreds of entries.
  **REFUTED as stated (task 8).** True at one operation in flight — 1.03 entries
  per AppendEntries — and false above it: 25.9–29.5 at 64 in flight with uniform
  keys, 47.0–48.7 with Zipfian. Batching happens; nothing configures it. The
  AppendEntries *rate* names the mechanism — flat at 4659–5499/sec while offered
  load rises sixteen-fold, then falling at 64 — so the replication round is
  tick-driven rather than proposal-driven and entries accumulate between rounds.
  That is incidental coalescing, and it buys no throughput: batching rises
  thirty-fold over a range in which throughput falls.
- **H2 — Replication rounds are not deduplicated.** N concurrent submissions
  to one group issue N `replicate_to_followers()` calls whose AppendEntries
  payloads overlap. Measured as **RPCs per committed entry**.
  **REFUTED (task 8).** RPCs per committed entry sits between 3.74 and 6.64
  across a 64-fold range of concurrency, in both key distributions and in two
  independent runs. If N submissions produced N rounds it would track N; it does
  not move with N at all. The same tick-driven round that refutes H1 is why —
  concurrent submissions land in one round rather than one each.
- **H3 — The wire encoding is JSON by default.** `json_rpc_serializer` is the
  serializer every multi-Raft test uses; the gRPC-vs-HTTP measurement already
  in `doc/http_transport_performance_comparison.md` found **19–24×** on a
  1 MiB payload for exactly this reason (byte arrays JSON-encoded). Small KV
  values should show a much smaller but non-zero effect.
- **H4 — The log is JSON lines and lives entirely in memory.**
  `file_persistence` keeps `_log` as a `std::map` *and* appends a JSON line per
  entry (`file_persistence.hpp:103`). Both the encode cost and the memory
  growth are measurable.
- **H5 — Reads transfer whole state.** `read_state` returns the serialized
  store (`raft.hpp:2455`), so read cost scales with shard size while a
  competitor's point read does not.
  **CONFIRMED (task 9), and the curve is linear to three figures.** Bytes
  returned per stored key is 146.08 / 146.01 / 146.00 at shard sizes of 100 /
  1000 / 5000 keys — a fifty-fold range. Throughput falls 2448.8 → 597.2 ops/sec
  over the same range while bytes/sec *rises* 34.1 → 415.7 MiB/sec, which is
  Requirement 2.4's reason for demanding both units. A separately-configured
  taxonomy row (four shards, stride 100) independently returned 146.03
  bytes/key.
  What did *not* follow from H5 is the comparison it invites: at a 250-key shard
  `read_state` is **1.7x faster in ops/sec and lower in p50** than this system's
  own linearizable point read, because `GET` is submitted as a proposal and pays
  a log entry and a replication round while `read_state` pays only a heartbeat
  quorum. The whole-store read is expensive against a *competitor's* point read,
  not against ours.
- **H6 — The tick sets a floor.** Anything that waits for a heartbeat waits
  for the caller's next `tick()`. At a 10 ms cadence that is a 10 ms floor on
  those paths regardless of how fast consensus is.
- **H7 — Per-group locking is coarse.** `node<Types>` serializes its own
  operations on one `std::mutex`. Across groups this is fine — it is why
  multi-Raft scales at all — but within a hot group it caps single-shard
  throughput.
  **CONFIRMED (task 8), and the cap is lower than the wording suggests.**
  Requirement 8.7 asks for the concurrency at which single-group throughput
  stops rising; in the hot-group (Zipfian) arm it never rises, falling
  monotonically from one operation in flight — 1460.5 → 1403.9 → 1221.1 → 513.1
  ops/sec. The four-group uniform arm gains 19% between 1 and 16 in flight and
  then halves at 64. At one in flight the hot group is *faster* than the spread
  cluster (1460.5 against 1162.1) and the ordering inverts by 64 (513.1 against
  727.4); that inversion is the lock becoming visible.

A hypothesis that measurement **refutes** must be recorded as refuted, with
the number that refuted it. That record is worth as much as the confirmations.

## Requirements

### Requirement 1 — The key/value workload

**User Story:** As a maintainer comparing this implementation to others, I
want the workload defined exactly and in this document, so that a number
produced a year from now describes the same work as today's, and so a reader
can judge whether it resembles the workload behind an external number.

#### Acceptance Criteria

1. WHEN the workload is defined THEN the system SHALL use
   `kythira::test_key_value_state_machine` as the state machine, unmodified,
   with commands built by its own `make_put_command`, `make_get_command` and
   `make_del_command` factories (`include/raft/test_state_machine.hpp:214,241,259`)
2. WHEN keys are generated THEN the system SHALL use fixed-width,
   lexicographically-ordered keys, so that key order is numeric order and every
   shard range is a genuine contiguous slice of the key space — matching the
   convention `multi_raft_scale_test` already uses
3. WHEN the key-space size is chosen THEN the system SHALL make it an explicit,
   reported parameter, and SHALL report the resulting per-shard key count and
   `approximate_size_bytes()` alongside every result, since read cost (H5) and
   split behaviour both depend on it
4. WHEN value sizes are chosen THEN the system SHALL measure at minimum
   **16 B, 128 B, 1 KiB and 4 KiB**, because published external numbers cluster
   at the small end (16–256 B) and the large end is where H3's encoding cost
   should appear
5. WHEN a key-access distribution is chosen THEN the system SHALL support at
   minimum **uniform** and **Zipfian** (skewed), SHALL report which was used,
   and SHALL report the skew parameter when Zipfian
6. WHEN a read/write mix is chosen THEN the system SHALL support at minimum
   **100% write**, **95% read / 5% write**, and **50/50**, and SHALL report the
   mix with every result
7. WHEN a command is submitted THEN the system SHALL route it by key through
   `multi_raft::submit_command(key, command, timeout)` — the production routing
   path including shard-map lookup and epoch validation — and SHALL NOT bypass
   routing by addressing a group directly, except in the dedicated routing-cost
   attribution scenario of Requirement 8
8. IF the state machine's stored size is required to stay stable during a
   measured window THEN the system SHALL pre-populate the key space before the
   window and use a write mix of PUT-over-existing-keys, so that shard size —
   and therefore H5's read cost — does not drift mid-measurement
9. WHEN the workload is documented THEN the system SHALL state, in the report,
   exactly which parts of it resemble and which parts differ from the workloads
   behind each external number it is set beside

### Requirement 2 — Read taxonomy

**User Story:** As a reader of the comparison, I want to know which of three
structurally different "reads" a read number describes, so that I am not shown
a whole-state transfer next to somebody's point lookup.

#### Acceptance Criteria

1. WHEN read performance is measured THEN the system SHALL measure and report
   three distinct read kinds separately, never aggregated:
   a. **`read_state` (quorum-confirmed whole-state)** — `multi_raft::read_state`,
      which confirms leadership by heartbeat quorum and returns the entire
      serialized store (`raft.hpp:2396,2455`)
   b. **`GET` through the log (linearizable point read)** — a `GET` command
      submitted as a proposal, which costs a log entry and returns one value
   c. **local stale read** — `test_key_value_state_machine::get_value` against
      a replica, no consensus, reported explicitly as **not linearizable**
2. WHEN a read result is reported THEN the system SHALL state its kind, its
   consistency level, and the bytes returned per operation
3. WHEN a read kind is compared to an external read number THEN the system
   SHALL match on consistency level and on bytes returned, and SHALL classify
   the comparison as *indicative* with the mismatch named whenever it cannot
4. WHEN `read_state` is measured THEN the system SHALL report throughput as
   **both** operations/sec and bytes/sec, because at a large shard the second
   is the number that describes the machine's actual work
5. WHEN `read_state` is measured across shard sizes THEN the system SHALL show
   its cost as a function of shard size, confirming or refuting H5 with a curve
   rather than a single point

### Requirement 3 — Deployment tiers

**User Story:** As a maintainer, I want each measurement labelled with the
configuration that produced it, so that the in-process number that is useful
for regression is never mistaken for the number that can be compared to
somebody's cluster.

#### Acceptance Criteria

1. WHEN the benchmark is run THEN the system SHALL support these tiers, and
   SHALL label every result with exactly one of them:
   - **Tier A — in-process fabric, `memory_persistence_engine`.** All hosts in
     one process over `tests/multi_raft_test_fabric.hpp`. Isolates host and
     consensus cost with no wire and no disk. **Never comparable to an external
     number**; used for regression and cost attribution.
   - **Tier B — in-process, real transport over loopback, memory persistence.**
     Adds serialization and socket cost.
   - **Tier C — one host process per node on one machine, real transport,
     memory persistence.** The standard shape of a published "no-fsync" number.
   - **Tier D — Tier C plus `file_persistence` with `tick_batch_controller`
     (durability mode `barrier`).** The only tier comparable to a published
     durable number.
   - **Tier E — multiple machines or containers** via the existing
     `tests/docker_chaos` harness, or one host process per cloud instance
     (Requirement 18.8). Optional for the first delivery.
2. WHEN Tier B or later is run THEN the system SHALL use a real transport and
   SHALL report which, with its serializer — drawn from the matrix Requirement
   17 defines, not from a single arbitrary choice
3. WHEN a tier below C is used THEN the system SHALL NOT publish a
   like-for-like comparison from it under any circumstances, and the report
   SHALL say so at the point of the number
4. WHEN Tier D is run THEN the system SHALL report the durability mode as
   `barrier`, the tick cadence, and the resulting **fsyncs per second per
   host** and **entries per fsync**, since those two numbers are what make a
   durable comparison meaningful
5. WHEN durability mode `buffered` is used THEN the system SHALL label the
   result **not durable** wherever it appears, because
   `file_persistence::append_log_entry` outside a batch does not fsync
   (`file_persistence.hpp:103`) and a reader will otherwise assume a file-backed
   log is a durable one
6. WHEN Tier E is designed THEN the system SHALL ensure the harness requires no
   restructuring to reach it, and SHALL honour the container-runtime rules in
   `CLAUDE.md` (no static IPs, no hardcoded `docker`, no privileged networking)
7. IF a tier cannot be delivered in the first pass THEN the system SHALL record
   it as not delivered with the reason, and SHALL NOT silently narrow the
   comparison claim to the tiers that were run

### Requirement 4 — The client driver

**User Story:** As a maintainer, I want the load generator to be correct in the
ways load generators are usually wrong, so that the tail latencies in the
comparison are real.

#### Acceptance Criteria

1. WHEN load is offered THEN the system SHALL support both a **closed-loop**
   mode (fixed in-flight cap) and an **open-loop** mode (fixed offered rate),
   and SHALL report which was used with the controlling parameter
2. WHEN latency is measured in open-loop mode THEN the system SHALL measure
   each operation from its **intended start time**, not from dispatch,
   correcting for coordinated omission, and SHALL state in the report that it
   does so
3. WHEN the in-flight cap is chosen THEN the system SHALL sweep it across at
   minimum 1, 8, 64 and 512 in-flight operations, since single-op latency and
   saturated throughput are different questions and one number cannot answer
   both
4. WHEN an operation fails THEN the system SHALL count it by cause —
   `not_leader`, epoch mismatch, merge freeze, commit timeout, transport error —
   and SHALL report the counts, because a throughput number computed over a run
   with a 5% rejection rate is measuring a different system than one at 0%
5. WHEN failures are counted THEN the system SHALL include failed operations in
   the *offered* count and exclude them from the *completed* count, and SHALL
   report both, so success rate is visible rather than folded into throughput
6. WHEN retries occur THEN the system SHALL report retries per completed
   operation, so that routing's own retry loop (`max_route_retries`, default 5,
   with `route_retry_backoff`) is visible rather than hidden inside latency
7. WHEN a measured window runs THEN the system SHALL discard any window during
   which a leadership change occurred in a group under load, and SHALL report
   how many windows were discarded — a window spanning an election measures
   election recovery, not steady state
8. WHEN the driver runs THEN the system SHALL account for its own CPU
   consumption separately from the host's, since in Tiers A and B they share a
   machine and a driver that saturates the box produces a number about the
   driver

### Requirement 5 — Pre-registered metrics

**User Story:** As a maintainer, I want every metric defined with its unit
before any run, so that a number cannot be reinterpreted after the fact to be
more flattering than it is.

#### Acceptance Criteria

1. WHEN the metric catalog is defined THEN the system SHALL define, with units,
   at minimum: **committed writes/sec per host**, **committed writes/sec per
   cluster**, **reads/sec by read kind**, **write latency p50/p99/p99.9**,
   **read latency p50/p99/p99.9 by read kind**, **bytes on the wire per
   committed entry**, **AppendEntries RPCs per committed entry**, **entries per
   AppendEntries**, **fsyncs/sec and entries per fsync** (durability
   `barrier`), **CPU seconds per committed operation**, **RSS per group**, and
   **tick phase durations** from `tick_report` (`multi_raft.hpp:93`)
2. WHEN a transport has a shared serialization point THEN the catalog SHALL
   additionally define **time waited on that point per operation** and a
   **per-group latency distribution**, per Requirement 17a
3. WHEN a latency percentile is reported THEN the system SHALL state the sample
   count it was computed from, and SHALL NOT label a percentile computed from
   fewer than 1,000 samples as p99 or from fewer than 10,000 as p99.9 — a
   standing correction of the pattern where "p99" was the slowest of eight
   samples
4. WHEN throughput is reported THEN the system SHALL report it as **completed,
   committed and applied** operations per second — not submissions, not
   appends — measured at the client
5. WHEN a per-core number is derived THEN the system SHALL report it as a
   clearly-labelled derived figure with the core count and the measured CPU
   utilization, and SHALL NOT use it to silently normalize away a hardware
   difference in a comparison
6. WHEN a metric is added after the first run THEN the system SHALL pre-register
   it before the run that publishes it, exactly as for the original catalog
7. WHEN the host's own instrumentation is used THEN the system SHALL prefer
   `tick_report` and `shard_report`'s existing counters
   (`_read_qps`, `_write_qps`, `_read_bytes_per_sec`, `_write_bytes_per_sec`,
   `shard_placement_driver.hpp:97`) over parallel bookkeeping, and SHALL
   cross-check them once against client-side counts — a divergence between what
   the host reports and what the client observed is itself a finding

### Requirement 6 — Statistical method and machine stability

**User Story:** As a maintainer, I want to know when the machine, rather than
the code, produced a number.

#### Acceptance Criteria

1. WHEN a scenario is measured THEN the system SHALL run warm-up iterations
   that are discarded, and SHALL report both the warm-up and measured counts
2. WHEN a scenario is measured THEN the system SHALL repeat the whole
   measurement at minimum **5 times**, report the **median** as the headline and
   the **min/max spread** beside it, and never report a single run as a result
3. WHEN the run-to-run spread of the headline metric exceeds **±10%** THEN the
   system SHALL mark the result **unstable** and SHALL NOT use it in a
   comparison table until the instability is explained or the run repeated on a
   quiet machine
4. WHEN a run begins THEN the system SHALL record a machine description:
   CPU model and core count, memory, kernel, filesystem and device type for the
   log directory, compiler and version, build type and flags, future backend,
   and whether other load was present
5. WHEN a run is performed for publication THEN the system SHALL run it on an
   otherwise-idle machine, and SHALL state in the report whether that condition
   held — `raft_comprehensive_performance_benchmark` was observed failing under
   `-j4` load and passing in isolation during the multi-Raft merge, which is
   precisely this effect
6. WHEN two configurations are compared to each other (ours vs. ours) THEN the
   system SHALL compare them **on the same machine in the same session**, in the
   spirit of the scale test's ratio-not-threshold rule
7. IF a measurement is inconclusive THEN the system SHALL report it as
   inconclusive permanently, and SHALL NOT allow a later retelling to promote it
   to a result

### Requirement 7 — Scaling axes

**User Story:** As a maintainer, I want the numbers to cover the axes on which
a multi-Raft implementation is actually judged, so that the comparison is not
a single point.

#### Acceptance Criteria

1. WHEN the matrix is defined THEN the system SHALL sweep **group count** at
   minimum 1, 8, 64, 256 and 1000 groups per host — 1000 being the number the
   design's scale claims are stated against and the number
   `multi_raft_scale_test` already uses. Its first three values are also what
   `.kiro/specs/coap-transport-multi-raft/` Requirement 7.1 asks for, so one
   sweep serves both specifications (Requirement 17a.1)
2. WHEN group count is swept THEN the system SHALL report throughput per host
   as a function of it, distinguishing **groups that exist** from **groups under
   load**, since the entire host design rests on cost tracking the latter
3. WHEN hibernation is enabled THEN the system SHALL report the hibernating
   fraction with every result, because a number taken over a mostly-hibernating
   population is a number about hibernation
4. WHEN cluster size is swept THEN the system SHALL measure at minimum 3 and
   5 voters, since replication factor is a first-order axis in every external
   number
5. WHEN the executor stripe count is swept THEN the system SHALL measure at
   minimum 1, 4 and (cores − 2) stripes, testing the design's own claim that
   **thread count is a property of the machine, never of the shard count**
6. WHEN load is concentrated THEN the system SHALL measure both **uniform load
   across groups** and **one hot group**, since H7's per-group lock caps the
   second and nothing else in the matrix would expose it
7. WHEN automatic split/merge is enabled THEN the system SHALL measure
   throughput and latency **through a split** — including the p99 impact on the
   splitting shard's own traffic — since the ability to split under load is the
   feature that distinguishes this from N independent Raft groups
8. WHEN split/merge is not the subject THEN the system SHALL disable automatic
   split/merge (`set_automatic_split_merge_enabled(false)`) and say so, so that
   a policy pass does not land inside a timed window

### Requirement 8 — Cost attribution

**User Story:** As a maintainer, I want a gap to have an address, so that the
comparison produces work items rather than a verdict.

#### Acceptance Criteria

1. WHEN a headline write number is produced THEN the system SHALL decompose one
   committed operation's cost into named components — routing (shard-map lookup,
   epoch validation, partitioner, load sampling), log append and encode,
   durability barrier, serialization, transport round trip, follower apply,
   commit-waiter fulfilment and future settlement — reported with a stated
   residual
2. WHEN the decomposition is produced THEN the system SHALL derive the transport
   and serializer components from the tier deltas (A→B→C→D) rather than from
   instrumentation inserted into production code paths
3. WHEN the routing component is isolated THEN the system SHALL compare
   `submit_command(key, …)` against `submit_command(group, epoch, …)`
   (`multi_raft_impl.hpp:2816,2824`), which is the one place Requirement 1.7's
   routing rule is deliberately relaxed
4. WHEN the serializer component is isolated THEN the system SHALL measure
   every RPC provider Requirement 17 lists, on one transport and one tier, so
   that H3's cost is attributed to the encoding rather than to the wire
5. WHEN H1 is tested THEN the system SHALL report **entries per AppendEntries**
   and **RPCs per committed entry** as a function of in-flight concurrency,
   which is the measurement that distinguishes designed batching from
   incidental coalescing
6. WHEN H6 is tested THEN the system SHALL sweep tick cadence and show which
   latency percentiles move with it and which do not
7. WHEN H7 is tested THEN the system SHALL show single-group throughput as a
   function of in-flight concurrency, and identify the concurrency at which it
   stops rising
8. WHEN a component's cost is reported THEN the system SHALL cite the existing
   measurement where one already exists — `doc/http_transport_performance_comparison.md`
   for transport, `doc/protobuf_serializer_performance_comparison.md` for
   serializers, `doc/future_backend_performance_comparison.md` for future
   overhead — rather than re-deriving it, and SHALL note when the cited
   measurement's conditions differ from this suite's

### Requirement 9 — The external comparison register

**User Story:** As a reader, I want to know exactly where every external number
came from and what it was measured under, so that I can check it and so that
nothing in the table is a remembered figure.

#### Acceptance Criteria

1. WHEN an external number is entered into the comparison THEN the system SHALL
   record, in a register in the report: implementation name and version, the
   **URL or document** the number came from, the **date retrieved**, whether it
   is the project's own claim or a third party's, the hardware, the cluster and
   group counts, replication factor, payload size, read/write mix, client
   concurrency, durability/fsync setting, batching configuration, and whether
   the measured thing is a **library** or a **database**
2. IF any field in 9.1 is not stated by the source THEN the system SHALL record
   it as **not stated** and SHALL NOT infer it, and the comparison SHALL be
   classified *indicative* on that axis
3. WHEN a number cannot be sourced to a retrievable document THEN the system
   SHALL NOT use it — remembered figures, including plausible ones, are
   forbidden, and a source that is thin must be **named as thin** rather than
   presented as if it carried detail it does not
4. WHEN the comparison set is chosen THEN the system SHALL include at minimum:
   **TiKV** (the implementation this design is explicitly drawn from — 27 mentions
   in `.kiro/specs/multi-raft/design.md`, most of them load-bearing), **Dragonboat** (a
   multi-group Raft *library*, the closest peer in kind), **braft** (the C++
   multi-group peer), and **etcd** (the single-group baseline every reader
   knows). CockroachDB and other database-level systems MAY be included, always
   classified as databases
5. WHEN a comparison is drawn against a database rather than a library THEN the
   system SHALL state that the external number includes a storage engine and,
   where applicable, a transaction layer, and SHALL classify the comparison
   *indicative* on that basis alone
6. WHEN a gap ratio is stated THEN the system SHALL state it with the tier, the
   comparison class, and the mismatched axes, and SHALL NOT state a bare
   multiplier anywhere in the document, including in a summary
7. WHEN the report presents the comparison THEN the system SHALL present a
   like-for-like table and an indicative table **separately**, never interleaved
8. WHEN no like-for-like comparison is possible for a metric THEN the system
   SHALL say so explicitly for that metric rather than substituting the closest
   indicative one

### Requirement 10 — Optional locally-run external measurement

**User Story:** As a maintainer, I want the option of running a competitor
myself on my own machine, because that is the only way to remove the hardware
axis — but I do not want a half-configured competitor's number presented as
theirs.

#### Acceptance Criteria

1. WHEN an external implementation is run locally THEN the system SHALL record
   the result in a **separate class** — "measured by us" — never merged with
   published numbers
2. WHEN an external implementation is run locally THEN the system SHALL record
   its version, build configuration, every non-default setting, and the exact
   command line, and SHALL state that it was run without that project's
   tuning guidance unless such guidance was followed and cited
3. WHEN a locally-run external number is below that project's own published
   number THEN the system SHALL report the discrepancy and SHALL treat our
   configuration as the suspect, not their implementation
4. WHEN a locally-run comparison is made THEN the system SHALL match durability
   mode, replication factor, payload and client concurrency, and SHALL name any
   axis it could not match
5. IF no external implementation is run locally THEN the system SHALL say so,
   and the comparison SHALL rest entirely on the register of Requirement 9

### Requirement 11 — Report artifacts

**User Story:** As a maintainer, I want the output in the shapes this project
already uses, so it is findable and diffable.

#### Acceptance Criteria

1. WHEN the report generator runs THEN the system SHALL write a timestamped
   CSV **and** JSON artifact to `test_results/`, following the existing
   `test_results/*_<timestamp>.*` convention used by
   `future_backend_benchmark_report`
2. WHEN results are published THEN the system SHALL write a human-readable
   `doc/multi_raft_performance_comparison.md` following the structure of
   `doc/future_backend_performance_comparison.md`: overview, quick start,
   scenario catalog, results, known structural asymmetries, reference machine,
   out of scope, see also
3. WHEN the document is written THEN the system SHALL include the structural
   hypotheses H1–H7 with each one's verdict (confirmed / refuted / untested) and
   the number behind it
4. WHEN the document is written THEN the system SHALL include the comparison
   register of Requirement 9 in full, not a summary of it
5. WHEN the JSON artifact is written THEN the system SHALL include every field
   required by Requirements 3, 5 and 6.4, so that a result is self-describing
   without its surrounding prose
6. WHEN the report generator is invoked THEN the system SHALL support running a
   subset (by tier, scenario or axis), since the full matrix will not fit in one
   sitting
7. WHEN a `README.md` link to the new document is added THEN the system SHALL
   use the absolute `https://github.com/crawlins/kythira/blob/main/doc/…` form,
   since a relative link from `README.md` to `doc/` fails the `docs` CI job

### Requirement 12 — CI regression tier

**User Story:** As a maintainer, I want CI to catch a performance regression
without CI ever deciding a comparison.

#### Acceptance Criteria

1. WHEN a CTest-registered variant exists THEN the system SHALL assert only
   **hardware-independent sanity floors** and **within-run ratios**, following
   the scale test's rule that a wall-clock threshold is a statement about the
   machine and a ratio is a statement about the implementation
2. WHEN a CTest-registered variant exists THEN the system SHALL NOT assert any
   relationship to an external implementation's number
3. WHEN the full matrix is registered THEN the system SHALL label it so it is
   excluded from the default run, following `multi_raft_scale_test`'s `scale`
   label and the existing `performance;benchmark` label convention, and SHALL
   set a timeout appropriate to its runtime
4. WHEN the CI variant runs THEN the system SHALL complete within a budget
   stated in the design, and the design SHALL state which tier it uses (Tier A
   is expected, since a CI runner's socket and disk behaviour is not a stable
   measurement substrate)
5. WHEN a floor is chosen THEN the system SHALL choose it low enough that a
   loaded runner does not fail it and high enough that a structural regression
   does, and SHALL record the reasoning next to the constant
6. WHEN a regression fires THEN the system SHALL make the failure message name
   the metric, the floor, the measured value and the tier

### Requirement 13 — Portability and optional dependencies

**User Story:** As a maintainer, I want the benchmark to build everywhere the
project builds, because a benchmark that only compiles under one backend
certifies one backend.

#### Acceptance Criteria

1. WHEN the benchmark is built THEN the system SHALL compile and run under all
   three future backends — folly, boost and stdexec — and the design SHALL note
   that `Future::get()` is rvalue-qualified under boost, so `std::move(f).get()`
   is the only portable form
2. WHEN a fire-and-forget continuation is written THEN the system SHALL
   `.detach()` it, since a discarded `.thenTry` never runs under stdexec
3. WHEN an optional dependency is absent (gRPC, protobuf, CBOR, Ion, libfiu)
   THEN the system SHALL still build and run, omitting the affected rows
   explicitly rather than emitting zeros or empty placeholders
4. WHEN a tier or scenario is unavailable in the current build THEN the system
   SHALL say which and why in the report output, since a silently smaller matrix
   reads as a completed one
5. WHEN the benchmark is run under a non-default backend THEN the system SHALL
   report the backend with the result, since backend choice is itself a cost
   component this project has already measured

### Requirement 14 — Measurement-only discipline

**User Story:** As a maintainer, I want the subject of measurement to be the
code that shipped, not a version tuned for the benchmark.

#### Acceptance Criteria

1. WHEN the benchmark is implemented THEN the system SHALL NOT modify
   `include/raft/raft.hpp`, `multi_raft.hpp`, `multi_raft_impl.hpp`, the
   serializers or the transports
2. IF an observability hook is genuinely required THEN the system SHALL prefer
   the existing counters and reports, and any new hook SHALL be additive,
   default-off, and SHALL NOT appear inside a lock or a hot path
3. WHEN a configuration is chosen for a measured run THEN the system SHALL
   report every non-default `multi_raft_config` and `raft_configuration` value,
   since a benchmark run with `_heartbeat_interval` at 10 ms and elections at
   40–80 ms is not measuring the shipped defaults
4. WHEN a measurement identifies an optimization THEN the system SHALL record it
   as a finding with its measured cost and SHALL NOT implement it in this spec
5. WHEN this spec completes THEN the system SHALL leave every default —
   including `KYTHIRA_DEFAULT_FUTURE_BACKEND`, the serializer choice and the
   transport choice — unchanged

### Requirement 15 — Steady state and hygiene

**User Story:** As a maintainer, I want a measured window to contain only the
thing being measured.

#### Acceptance Criteria

1. WHEN a measured window begins THEN the system SHALL first confirm that every
   group under load has a stable leader, and SHALL sample leadership again at
   the end — noting that `leader_of()` is a *sample*, so a window is validated by
   two observations plus a no-election-occurred check, never by one
2. WHEN a measured window runs THEN the system SHALL confirm no election, no
   split, no merge and no snapshot install occurred in a group under load,
   unless that event is the subject, and SHALL discard and re-run the window
   otherwise
3. WHEN the tick loop is driven THEN the system SHALL drive it from a dedicated
   thread per host at a stated cadence, and SHALL report tick cadence, achieved
   tick rate and the phase-duration breakdown, since a driver that cannot keep
   its cadence is the measurement's bottleneck
4. WHEN a future is awaited THEN the system SHALL NOT block on a future whose
   settlement depends on a tick the blocking thread is responsible for driving —
   the tick is this library's only clock, and a blocking wait with nobody
   ticking waits for a deadline that can never fire
5. WHEN a run ends THEN the system SHALL stop every host cleanly and SHALL
   confirm no group was left running, since an unstopped node destroyed by
   `~group_state` terminates the process
6. WHEN snapshotting could occur during a measured window THEN the system SHALL
   either disable it or report it, since an InstallSnapshot mid-window moves a
   whole state machine across the wire

### Requirement 16 — What the deliverable must answer

**User Story:** As the person who asked how close we are, I want the report to
answer that in a way I can act on.

#### Acceptance Criteria

1. WHEN the report is complete THEN the system SHALL state, for **committed
   write throughput at a stated payload and durability mode**, our number, the
   nearest defensible external number, the comparison class, and the gap ratio
   with its mismatched axes
2. WHEN the report is complete THEN the system SHALL state the same for **write
   latency p99** and for **each read kind**
3. WHEN the report is complete THEN the system SHALL state, for the largest
   gaps, the cost attribution that explains them and the hypothesis each
   confirms or refutes
4. WHEN the report is complete THEN the system SHALL list the follow-on work the
   measurements motivate, ordered by measured cost, each as a candidate spec —
   without implementing any of it
5. WHEN the report is complete THEN the system SHALL state plainly which
   questions it could **not** answer and what would be needed to answer them,
   rather than filling the gap with the closest available number

### Requirement 17 — The transport and RPC-provider matrix

**User Story:** As a maintainer, I want the numbers taken across every HTTP
transport and every RPC provider this project ships, so that the comparison
describes what this system does rather than what one arbitrary configuration
does — and so that a transport added later joins the matrix instead of forcing
the harness to be rewritten.

#### Acceptance Criteria

1. WHEN the transport axis is swept THEN the system SHALL measure over
   **cpp-httplib** (`include/raft/http_transport.hpp`), **Boost.Beast**
   (`beast_http_transport.hpp`) and **Proxygen** (`proxygen_http_transport.hpp`),
   each gated on its own configure-time flag
   (`KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT`, `KYTHIRA_BUILD_PROXYGEN_TRANSPORT`)
   rather than on all of them together, so a tree missing one still runs the
   rest of the matrix
2. WHEN a transport is unavailable in the current build THEN the system SHALL
   name the rows it did not run, in its own output, rather than printing a
   shorter table that reads as a complete one
3. WHEN the RPC-provider axis is swept THEN the system SHALL measure
   **`json_rpc_serializer`** and **`cbor_rpc_serializer`** in every
   configuration, **`protobuf_rpc_serializer`** when `PROTOBUF_SERIALIZER_FOUND`,
   and **`ion_rpc_serializer`** when the opt-in `ion` vcpkg feature is present
4. WHEN a row is reported THEN the system SHALL label it with the transport, the
   serializer's own `media_type()` — taken from the serializer rather than from
   a hand-written string, so a row cannot disagree with what went on the wire —
   and the tier
5. WHEN the RPC-provider axis is swept THEN the system SHALL hold the
   node-internal `Types::serializer_type` constant across every row, so the only
   thing moving is the wire encoding
6. WHEN a scenario is implemented THEN the system SHALL express it once, generic
   in the transport and in the serializer, and instantiate it per matrix cell —
   never as per-transport copies, which drift until the rows stop describing the
   same work (the rule `.kiro/specs/future-backend-performance-benchmark/`
   already sets for its own two-backend comparison)
7. WHEN a transport lacks an optional RPC THEN the system SHALL report every
   scenario needing it as **not run on that row**, never silently skipped: none
   of the three HTTP transports satisfies `network_client_with_timeout_now`, so
   leadership transfer and `scatter` are unavailable across the whole HTTP half
   of the matrix
8. WHEN a transport handle is written THEN the system SHALL forward each optional
   RPC behind a `requires` clause on the underlying transport, so that the handle
   satisfies exactly the extension concepts its transport does — a transport that
   grows `timeout_now` lights it up with no edit, and one that never has it fails
   the concept rather than failing to compile
9. WHEN cpp-httplib's row is reported THEN the system SHALL carry its known
   cause with it: the vendored `httplib.h` defaults `CPPHTTPLIB_TCP_NODELAY` to
   `false`, so every small RPC body pays the classic Nagle/delayed-ACK round
   trip, measured at **12 ops/sec against Beast's 3,527** on a bare RPC
   ping-pong (`doc/http_transport_performance_comparison.md`, run July 28, 2026).
   The row SHALL NOT be presented as evidence about cpp-httplib in general, and
   SHALL NOT be quietly dropped either — a transport this project ships is part
   of the answer to "how fast is it"
10. WHEN operation budgets differ between rows — as they must, since a row at
    ~83 ms per round trip cannot carry the same budget as one at ~250 µs — THEN
    the system SHALL keep throughput comparable (it is a rate) and SHALL report
    the tail as **unavailable** rather than computing a percentile from too few
    samples, per Requirement 5.3
11. WHEN rows are compared to each other THEN the system SHALL hold every other
    configuration value identical — cluster shape, group count, election and
    heartbeat timings, tick cadence, stripe count, value size, client
    concurrency — so the swept axis is the only thing that moved
12. WHEN the matrix is designed THEN the system SHALL make adding a transport a
    matter of adding one fixture: a bundle of `client_type` / `server_type`, a
    `name()`, a capability descriptor and accessors. **CoAP is the named future
    row**: `multi_raft` has no CoAP binding today, and when it gets one the
    conditional forwarding of 17.8 means its documented lack of TimeoutNow needs
    no special case anywhere in the harness
13. WHEN gRPC gains a multi-Raft binding THEN the system SHALL admit it as a row
    on the same terms, since its measured behaviour on large payloads (**657
    ops/sec against the HTTP transports' 27–35 on a 1 MiB `install_snapshot`**,
    same document) makes it the most likely transport to change the answer at
    the large end of the value-size sweep
14. WHEN the matrix runs THEN the system SHALL first prove, per transport, that a
    KV cluster over that transport **elects, commits and reads back** — a
    benchmark over a path nothing has ever exercised measures nothing
    trustworthy, and before this spec no test in the tree had driven
    `multi_raft` through a socket at all

### Requirement 17a — The shared-transport serialization point

**User Story:** As a maintainer, I want the matrix to measure what each shared
transport serializes, because every transport this project ships has a point at
which many groups become one queue, and that point — not the encoding, not the
network — is usually what a per-group throughput number is actually reporting.

This requirement reconciles two specifications that had each specified a
benchmark. `.kiro/specs/coap-transport-multi-raft/` Requirement 7 asks for N
groups over one shared CoAP client at N ∈ {1, 8, 64}, reporting per-group
send-path latency and time waiting on that client's `_mutex`. That is not a CoAP
question wearing a general coat; it is the general question, first asked where it
bites hardest. **One matrix answers both**, and CoAP becomes a row in it rather
than a second harness.

#### Acceptance Criteria

1. WHEN the group-count axis is swept THEN the system SHALL include **N ∈ {1, 8,
   64}** on *every* transport row — the values
   `.kiro/specs/coap-transport-multi-raft/` Requirement 7.1 states, and already a
   subset of Requirement 7.1 here. A transport that cannot reach the higher
   values SHALL report where it stopped and why, never silently run a shorter
   sweep
2. WHEN a row is reported THEN the system SHALL report the latency distribution
   **per group**, not only aggregated across groups. An aggregate hides the case
   this axis exists to find: one group starving while the mean looks healthy
3. WHEN a transport serializes work the host issues concurrently THEN the system
   SHALL name that point in the row and report **time spent waiting on it** as a
   first-class metric. The points known today: CoAP's `std::recursive_mutex`
   held across every libcoap call (`coap_transport.hpp`, required because
   libcoap's C API is not safe to call concurrently on one context);
   cpp-httplib's single `httplib::Client` per target; and — until it was fixed —
   Beast's single pooled connection per target
4. WHEN a transport already carries its own send-path instrumentation THEN the
   system SHALL consume it rather than adding a second scheme. CoAP's
   `KYTHIRA_COAP_SEND_PROBE` already separates lock acquisition, address
   resolution, session acquisition, PDU construction and `coap_send`, and
   Requirement 7.2 of that spec requires its reuse
5. WHEN no such instrumentation exists for a transport THEN the system SHALL
   derive the contention figure from the tier deltas and the concurrency sweep
   rather than instrumenting the transport, which Requirement 14.1 forbids
6. WHEN the contention measurement is reported THEN the system SHALL record the
   hypotheses it **refutes** alongside those it confirms, in the manner of
   `doc/coap-flake-investigation.md` — that document exists because this
   investigation had been attempted repeatedly from analysis alone, each attempt
   producing a plausible diagnosis the data later contradicted
7. WHEN the measurement shows a transport's shared serialization point is the
   binding constraint THEN the system SHALL record it as a finding with its cost
   and SHALL NOT change that transport's locking, I/O thread or context
   structure — identical in force to Requirement 14.4 here and to
   `.kiro/specs/coap-transport-multi-raft/` Requirement 7.4, for the same
   reason: such a change alters single-group behaviour and earns its own review
8. WHEN the CoAP row becomes runnable THEN it SHALL satisfy
   `.kiro/specs/coap-transport-multi-raft/` Requirement 7.1–7.3 by being a row
   in this matrix, and that spec's benchmark task SHALL be discharged here
   rather than by a second harness

### Requirement 18 — Measurement on real cloud hardware

**User Story:** As a maintainer whose development machine is old, I want the same
benchmark run on current cloud hardware, so that a gap against a published number
measured on a modern machine is a gap in the implementation rather than a gap in
my CPU.

#### Acceptance Criteria

1. WHEN cloud performance runs are added to CI THEN the system SHALL put them in
   a **new workflow file**, and SHALL NOT add any `workflow_dispatch` input to
   `.github/workflows/real-cloud-tests.yml` — that file sits at **exactly 25
   inputs**, which is GitHub's hard cap, and exceeding it invalidates the entire
   file silently (it happened once already, at PR #257, and `ci.yml` now carries
   a guard job asserting the limit)
2. WHEN the workflow is defined THEN the system SHALL make it explicitly
   dispatched, SHALL NOT put it on a default schedule, and SHALL gate it behind
   the same layered opt-in the existing real-cloud workflow uses, so that a
   performance run is always a decision someone made
3. WHEN credentials are needed THEN the system SHALL reuse the existing
   short-lived OIDC federation (AWS role assumption, GCP Workload Identity
   Federation, and the equivalents already provisioned under
   `scripts/ci-cloud-credentials/`), and SHALL NOT introduce a long-lived key
4. WHEN a cloud result is recorded THEN the system SHALL record the full machine
   provenance Requirement 6.4 asks for, extended with what only a cloud has:
   provider, region, availability zone, instance type, vCPU count and underlying
   CPU model, memory, stated network performance, storage class and IOPS, the
   machine image, the kernel, and the tenancy
5. WHEN an instance type is chosen for a published measurement THEN the system
   SHALL NOT use a **burstable** type (AWS `t*`, GCP `e2-micro`/`f1`, Azure `B`
   series, and their equivalents), because CPU credit exhaustion mid-run produces
   a number that describes a credit balance; the chosen type SHALL be recorded
   and the exclusion SHALL be stated
6. WHEN a cloud run and a local run are compared THEN the system SHALL have run
   the **same harness binary, the same scenarios and the same configuration**,
   with only the machine differing, and SHALL present the cloud numbers as their
   own rows carrying their own machine record — never merged with local rows and
   never averaged with them
7. WHEN a single-instance run is performed (Tier A, B or D on one VM) THEN the
   system SHALL treat it as the primary deliverable of this requirement: it
   removes the hardware confound, which is the stated reason for running in the
   cloud at all, and it costs one instance for minutes
8. WHEN a multi-instance run is performed (Tier E, one host process per VM) THEN
   the system SHALL additionally measure and report the **inter-node round-trip
   latency and available bandwidth** before the measured window, because a
   cluster number without its network is not comparable to a published cluster
   number, and SHALL record the placement (same zone, spread, or placement group)
9. WHEN the stability gate of Requirement 6.3 is applied THEN the system SHALL
   apply it unchanged to cloud runs, and the design SHALL expect to fail it more
   often: a shared-tenancy cloud instance has neighbours, and a run that would be
   marked unstable locally is not made trustworthy by having cost money
10. WHEN a run completes THEN the system SHALL tear down **every** resource it
    created, and SHALL run a post-run audit that fails the job if anything it
    provisioned still exists — instances, disks, addresses, security groups,
    placement groups — following the pattern the live GCP run established, where
    the audit is what proved nothing leaked
11. WHEN a run is defined THEN the system SHALL carry a **pre-registered cost
    estimate** (instance-hours × published rate, stated per run) in the same
    spirit as `doc/aws_acm_pca_test_cost_estimate.md`, and SHALL enforce a
    wall-clock ceiling on the measured phase so a hung benchmark cannot bill
    indefinitely
12. WHEN a provider is chosen THEN the system SHALL start with one, deliver it
    end to end, and only then add others — five half-wired providers produce no
    numbers. The design SHALL name the first and justify it
13. WHEN results leave the job THEN the system SHALL upload the CSV/JSON
    artifacts of Requirement 11.1, so the number exists somewhere other than a
    log that expires
14. WHEN a GitHub-hosted runner is used as the machine THEN the system SHALL
    label the result as such, SHALL record the runner class, and SHALL treat it
    as an **indicative** measurement only: hosted runners are shared, 2–4 vCPU,
    and their variance is not something the stability gate can be tuned around
15. WHEN cloud test selection is wired THEN the system SHALL preserve the
    standing exclusion already on record — Alibaba's hosted CA tests are not to
    be run — even though nothing in this requirement would otherwise reach them

## Appendix A — Comparison register template

Every external number carries this record. A blank field is written
**not stated**, never inferred.

| Field | Notes |
|---|---|
| Implementation, version | e.g. library vs. database is decided here |
| Kind | library / database |
| Source URL or document | must be retrievable |
| Date retrieved | |
| Author of the number | the project itself / third party |
| Hardware | CPU, cores, memory, storage device, network |
| Cluster size / replication factor | |
| Raft group count | 1 for a single-group baseline |
| Payload size | key size and value size separately |
| Read/write mix | and read consistency level |
| Client concurrency | in-flight or thread count |
| Durability | fsync per entry / per batch / none |
| Batching configuration | proposal batching, pipelining, max entries per RPC |
| Metric and unit as stated | verbatim from the source |
| Comparison class | like-for-like / indicative, with mismatched axes |

## Appendix B — Open questions for the design phase

1. ~~Which transport for Tiers B–E?~~ **Answered, and it became Requirement
   17: all three HTTP transports, swept, plus every RPC provider.** Picking one
   would have made the number a property of that choice. What remains open is
   narrower — whether the value-size sweep should extend past 4 KiB to reach the
   regime where gRPC's measured 19–24× advantage over the HTTP transports on a
   1 MiB payload starts to matter.
2. **How is the load driver deployed in Tiers C–E?** In-process with a host, or
   its own process? The second is more honest about CPU accounting and requires
   a client-facing entry point that does not exist yet.
3. **Does a benchmark host binary belong in `cmd/`?** Tier C and beyond need a
   process that hosts `multi_raft`; `cmd/chaos_node` is the nearest precedent.
4. **How is the key space partitioned across groups initially?** Pre-split into
   N ranges (as `multi_raft_scale_test` does) or grown by automatic split? The
   first is reproducible; the second is what a real deployment does.
5. **Should `GET`-through-the-log be added to the workload's command mix, or
   measured only in the read-taxonomy scenario?** It costs a log entry, so
   including it in a "read" mix changes what the write number means.
6. **What is the CI budget?** The full matrix is hours. The design must state
   the CI subset and its runtime, and whether a nightly or scheduled job is
   warranted rather than a per-PR one.
7. **Is Tier E in the first delivery?** It is the only tier with a real network
   between nodes, and therefore the only one whose latency resembles a
   published cluster number — but it is also the most expensive to stand up.

## Appendix C — Related work already in the tree

Cited rather than re-measured (Requirement 8.8):

- `tests/multi_raft_scale_test.cpp` — 1000 groups, ratio-based assertions,
  hibernation. The precedent for method as well as subject.
- `tests/raft_comprehensive_performance_benchmark.cpp` — throughput, latency
  percentiles, scalability and resource profiling, but against state machines
  directly rather than through replication.
- `tests/future_backend_benchmark_test.cpp`,
  `doc/future_backend_performance_comparison.md` — future overhead, and the
  model for this spec's report structure.
- `doc/http_transport_performance_comparison.md` — transport comparison
  including the gRPC rows.
- `doc/protobuf_serializer_performance_comparison.md` — serializer wire size
  and throughput.
- `tests/docker_chaos/` — the multi-container harness Tier E would use.
