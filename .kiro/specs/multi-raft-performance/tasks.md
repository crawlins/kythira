# Implementation Plan

## Status: the Tier B substrate has landed; no number is quotable yet

Tasks 1–4, 6 and 10 are checked, and they are checked against the tree — the
three files are in `tests/` and the suite runs. Everything else is open. A
checked box in a spec whose code the reader cannot find is the exact drift
`doc/TODO.md`'s own "how this table stays honest" note was added to stop, so a
box gets ticked only after the code is on `main` and has been re-verified there,
never from this prose.

### What has landed

`tests/multi_raft_kv_workload.hpp`, `tests/multi_raft_transport_harness.hpp`,
`tests/multi_raft_http_benchmark_test.cpp` and their `tests/CMakeLists.txt`
registration:

- Builds with all three HTTP transports, the protobuf serializer, and under all
  three future backends.
- **Every smoke case passes** — three hosts, four shards, elections, committed
  PUTs, read-backs, all over real loopback sockets, on cpp-httplib, Beast and
  Proxygen. That is the first time anything in this tree has driven `multi_raft`
  through a socket, and it is what makes Tier B a substrate rather than a plan.
- **Every throughput row is repeated five times** and reported as a median with
  its spread and a verdict (task 10). Nothing this suite prints today is
  `stable`: the three transport rows came out at ±37.3%, ±10.9% and ±23.8% of
  their medians, so all three read `UNSTABLE — MUST NOT ENTER A COMPARISON
  TABLE`. That is the machinery working, not failing.
- It found two transport defects. The Beast concurrent-RPC crash is fixed on
  `main`; the teardown fault of task 5 is still open and is now pinned to
  Proxygen at a measured rate.

### What the numbers are, and are not

**No row here may be quoted.** Requirement 6.3 marks a row unstable above ±10%
spread, and no row measured so far is below it — on a machine that was also not
quiet, which the result states for itself (Requirement 6.5). The honest summary
of this spec's progress is that it can now *tell* whether a number is quotable,
and the answer is currently no for every one of them.

---

- [x] 1. The KV workload, independent of any transport
  - `tests/multi_raft_kv_workload.hpp`: `kv_put`/`kv_get`/`kv_del` to
    `test_key_value_state_machine`'s own encoding, `kv_partitioner`, fixed-width
    `kv_key`, deterministic non-constant `kv_value`, `kv_shard_ranges`
  - `key_sampler` over uniform and Zipfian
  - `latency_sample_set` whose `p99()`/`p999()` return `std::optional`, empty
    below 1,000 / 10,000 samples; `operation_tally` counting failures by cause
    and keeping `_offered` distinct from `_completed`; `benchmark_result`
  - _Requirements: 1.1–1.6, 4.4, 4.5, 5.1, 5.3_

- [x] 2. Movable handles over the non-movable transports
  - `transport_client_handle` / `transport_server_handle` forwarding the three
    mandatory RPCs unconditionally and every optional one behind a `requires`
    clause on the underlying transport
  - _Requirements: 14.1, 17.7, 17.8_

- [x] 3. A fixture per HTTP transport
  - `cpp_httplib_transport`, `beast_http_transport`, `proxygen_http_transport`,
    each owning its own runtime; `reserve_port()`; `harness_transport_types`
    pinned to `kythira::future_default`
  - _Requirements: 3.1 (Tier B), 3.2, 17.1, 17.12_

- [x] 4. `kv_cluster` and the closed-loop workload driver
  - One tick driver thread per host; constant configuration across rows;
    `run_command` measuring from before the routing lookup and classifying every
    failure; `run_read_state`; `term_sum()`; `run_put_workload`
  - _Requirements: 4.1, 4.3, 7.8, 15.1, 15.3, 15.4, 17.11_

- [ ] 5. **Fix the teardown fault — measured, and it is Proxygen's, not Beast's**
  - **The rate is measured, not assumed.** Ten consecutive runs of all three
    smoke cases outside a debugger: **9 clean, 1 fault**. The failing run
    printed `proxygen: 8/8 operations committed over a real socket` and then
    `fatal error: in
    "multi_raft_http_benchmark/a_kv_cluster_commits_over_proxygen": memory
    access violation at address: …: invalid permissions` — i.e. after the
    Proxygen case's work was done, in its teardown. cpp-httplib and Beast were
    clean in all ten
  - **The Beast `corrupted double-linked list` this task was opened for has not
    recurred**, in those ten runs or in any run since `fix(beast): check
    connections out exclusively per RPC` landed. That fix was for a connection
    outliving the executor its callbacks were scheduled on, which is the same
    shape as a teardown fault, so the Beast arm plausibly went with it — but one
    clean stretch is not a closure, and the box stays open until the Proxygen
    arm is understood, because a single fault mechanism showing up on two
    fixtures is at least as likely as two
  - A benchmark whose teardown corrupts memory cannot be trusted to have
    measured a clean steady state either, which is why this blocks quoting
    Proxygen's row specifically
  - **It hides from the debugger, and that is now measured too.** Twenty runs
    of the Proxygen smoke case alone under `gdb -batch -ex run`: **0 faults**.
    The spike's Beast arm behaved the same way. So a clean debugger run is
    evidence about the debugger, not about the bug — establish a rate outside
    one, before and after any fix. `ptrace_scope=1` here means running the
    target *under* gdb, never attaching
  - Because gdb suppresses it, the next step is a **sanitizer**, not another
    backtrace attempt: ASan first (an invalid-permissions access is what it is
    built to name), then TSan for the lifetime ordering
  - Reproduce under ASan and under TSan before declaring it fixed
  - _Requirements: 15.5_

- [x] 6. Prove the other two transports end to end
  - Both smoke cases run and pass: cpp-httplib and Proxygen each elect on four
    shards, commit a PUT per shard and read every value back through the log
    over real loopback sockets
  - What they surfaced is task 5's teardown fault, which is Proxygen's and is
    left open there rather than folded into this box
  - **cpp-httplib's cost tracks the documented Nagle round trip, and is not a
    new effect.** Measured at 13.1 ops/sec median (min 8.2, max 16.4) at four
    in flight, p50 211 ms per operation — i.e. ~305 ms of occupancy per worker
    per committed PUT, which is two to three ~83 ms round trips, exactly what a
    commit costs when each replication hop pays Nagle/delayed-ACK once. It sits
    beside the 12 ops/sec `doc/http_transport_performance_comparison.md` already
    records for a bare ping-pong, unchanged under a real workload
  - _Requirements: 17.14, 17.9_

- [ ] 7. Complete the RPC-provider axis
  - CBOR and protobuf rows over Beast; Ion behind its own gate
  - Confirm each row's label comes from the serializer's own `media_type()`
  - Assert the node-internal serializer stayed at JSON across every row
  - _Requirements: 8.4, 17.3, 17.4, 17.5_

- [ ] 8. The concurrency and distribution axes
  - `write_throughput_by_concurrency`: in-flight 1, 8, 64 — the sweep that
    answers whether single-group throughput stops rising (H7)
  - Uniform vs. Zipfian at fixed concurrency, which is the only configuration in
    the matrix that concentrates load on one group
  - Report **entries per AppendEntries** and **RPCs per committed entry** as a
    function of concurrency (H1, H2)
  - _Requirements: 5.1, 7.6, 8.5, 8.7_

- [ ] 9. The read taxonomy
  - Three separately-reported read kinds: `read_state` (quorum-confirmed
    whole-store), `GET` through the log (linearizable point read), local stale
    read
  - `read_state` reported in ops/sec **and** bytes/sec, and as a curve against
    shard size, which confirms or refutes H5
  - _Requirements: 2.1–2.5_

- [x] 10. Statistical method
  - `repeated_result` runs every throughput row **five** times and reports the
    median run as the headline with min/max beside it.
    `headline_ops_per_second()` returns `std::optional` and is empty below
    `k_required_repetitions`, so "never report a single run as a result" (6.2) is
    a property of the type rather than of whoever is reading
  - `spread()` is the ± half-width around the median, and `verdict()` /
    `comparable()` gate a row out of a comparison table above ±10% (6.3). The
    printed row says `MUST NOT ENTER A COMPARISON TABLE` in words
  - The headline names a real run, not an average, so the p50/p95/p99 beside it
    came from the window that produced it
  - Warm-up and measured counts are reported per row (6.1); a repetition is a
    whole measurement, cluster construction and election included, because that
    is where the variance being guarded against lives
  - `machine_description` / `describe_machine()` capture CPU, memory, kernel,
    the filesystem *and device* behind a named directory, compiler, build type
    and flags, sanitizer, future backend, and the load average with a
    `_quiet_at_start` flag (6.4, 6.5). Build type and flags come from CMake, not
    from `NDEBUG`. Captured by a global fixture, so `--run_test` cannot deselect
    a row's provenance; one description per process is also what makes 6.6's
    "same machine, same session" structural
  - **Ours-vs-ours comparisons are same-process by construction** (6.6), and an
    inconclusive row stays inconclusive: `result_verdict` is computed from the
    runs, so a retelling cannot promote one (6.7)
  - _Requirements: 6.1–6.7_

- [ ] 11. Cost attribution
  - Tier A rows (the existing fabric) beside Tier B rows, so the transport and
    serializer components come from the tier delta rather than from
    instrumentation inserted into production paths
  - Routing cost from `submit_command(key,…)` against `submit_command(group,
    epoch,…)`
  - Tick-cadence sweep for H6
  - Decomposition published with a stated residual
  - _Requirements: 8.1–8.3, 8.6, 8.8_

- [ ] 12. Report artifacts
  - Timestamped CSV and JSON into `test_results/`, self-describing per
    Requirement 11.5
  - A report binary supporting subset selection by tier, scenario or axis, and
    open-loop load with coordinated-omission correction
  - _Requirements: 4.2, 11.1, 11.5, 11.6_

- [ ] 13. Portability
  - Build and run under folly, boost and stdexec; `std::move(f).get()` everywhere;
    `.detach()` on any fire-and-forget continuation
  - **The build half is done, and it took a production fix.**
    `include/raft/http_transport_impl.hpp`'s `make_future_with_exception` /
    `make_future_with_value` constructed the future directly from an
    `exception_ptr` or a value, which only folly's `Future` and the simulator's
    accept — `boost_backend::Future` is built from a `boost::future` and
    `stdexec_backend::Future` from a sender, so neither compiled. They now go
    through `future_factory_default::makeExceptionalFuture` /
    `makeFuture`, which all three backends expose. Nothing had instantiated
    `cpp_httplib_client` under a non-folly backend before this suite
  - Still open: actually *running* the matrix under boost and stdexec, and the
    graceful-shrink check below
  - Confirm the matrix shrinks gracefully with each optional dependency absent,
    and that every dropped row is named in the output
  - _Requirements: 13.1–13.5, 17.2_

## Real-cloud measurement

- [ ] 14. A new workflow file, not new inputs
  - `.github/workflows/perf-cloud.yml`, explicitly dispatched, no default
    schedule, layered opt-in
  - **Do not add a `workflow_dispatch` input to `real-cloud-tests.yml`** — it is
    at exactly 25, which is the cap, and exceeding it silently invalidates the
    whole file. Confirm `ci.yml`'s guard job still passes after the new file
    lands
  - Short-lived OIDC credentials only, reusing `scripts/ci-cloud-credentials/`
  - _Requirements: 18.1, 18.2, 18.3_

- [ ] 15. Machine provenance capture
  - A script the job runs on the instance that records provider, region, AZ,
    instance type, vCPU count and CPU model as the guest reports it, memory,
    stated network performance, storage class and IOPS, image, kernel and tenancy
    into the result JSON
  - Fail the run if the instance type is burstable
  - _Requirements: 18.4, 18.5, 6.4_

- [ ] 16. Shape 1 — one AWS instance, Tier B
  - Provision one non-burstable instance, build or fetch the benchmark binary,
    run the same scenarios with the same configuration as the local rows, pull
    the artifacts back, tear down
  - Publish the local-vs-cloud delta as its own finding: that number *is* the
    hardware confound
  - _Requirements: 18.6, 18.7, 18.13_

- [ ] 17. Cost and safety controls
  - Pre-registered cost estimate per run, in the spec's own doc
  - Hard wall-clock ceiling on the measured phase
  - Unconditional teardown that runs even when the measurement fails
  - Post-run leak audit that **fails the job** if anything provisioned still
    exists — instances, volumes, addresses, security groups, placement groups
  - _Requirements: 18.10, 18.11_

- [ ] 18. Shape 1 on Graviton
  - The arm64 leg already exists in CI; a modern arm64 instance is its own point
    in the design space, not a portability check
  - _Requirements: 18.6, 18.12_

- [ ] 19. Tier D on a cloud instance
  - `file_persistence` plus `tick_batch_controller` against a real volume;
    report fsyncs/sec and entries per fsync, and the volume class and IOPS
  - This is the first configuration in the whole spec that could carry a
    like-for-like comparison against a published *durable* number
  - _Requirements: 3.4, 3.5, 18.7_

- [ ] 20. Shape 2 — Tier E, one host per instance
  - A host binary in `cmd/`, N instances, service discovery between them
  - Measure and report inter-node RTT and bandwidth **before** the measured
    window, plus the placement
  - _Requirements: 3.1 (Tier E), 18.8_

- [ ] 21. A second provider
  - GCP, following the Workload Identity Federation path already live and the
    audit pattern its own live run established
  - _Requirements: 18.12_

## The comparison itself

- [ ] 22. Build the external comparison register
  - One record per external number with every field of Appendix A; anything the
    source does not state recorded as **not stated**, never inferred
  - TiKV, Dragonboat, braft, etcd at minimum; database-level systems classified
    as such
  - _Requirements: 9.1–9.5_

- [ ] 23. `doc/multi_raft_performance_comparison.md`
  - Like-for-like and indicative tables kept separate; no bare multiplier
    anywhere; H1–H7 each with a verdict and the number behind it; every metric
    with no possible like-for-like comparison saying so explicitly
  - Absolute `https://github.com/crawlins/kythira/blob/main/doc/…` link from
    `README.md`, since a relative one fails the `docs` CI job
  - _Requirements: 9.6–9.8, 11.2–11.4, 16.1–16.5_

- [ ] 24. CI regression tier
  - Sanity floors and within-run ratios only, never a comparison assertion
  - State the tier the CI subset runs at and its runtime budget
  - Failure messages naming metric, floor, measured value and tier
  - _Requirements: 12.1–12.6_
