# Implementation Plan

## Status: Not started in this repository — a spike exists, unlanded

**Every box below is unchecked, deliberately.** Tasks 1–4 have been *written and
run* as a spike, but that code is not on `main` and is not part of this spec's
own change. A checked box in a spec whose code the reader cannot find is the
exact drift `doc/TODO.md`'s own "how this table stays honest" note was added to
stop, and it has cost this project sessions before. They get ticked when the
implementation CR merges, re-verified against the tree rather than taken from
this prose.

### What the spike established

Written as `tests/multi_raft_kv_workload.hpp`,
`tests/multi_raft_transport_harness.hpp`,
`tests/multi_raft_http_benchmark_test.cpp` plus a `tests/CMakeLists.txt`
registration, landing in its own follow-up CR:

- It builds with all three HTTP transports and the protobuf serializer.
- **The Beast smoke case passes** — three hosts, four shards, elections,
  committed PUTs, read-backs, all over real loopback sockets. That is the first
  time anything in this tree has driven `multi_raft` through a socket, and it is
  what makes Tier B a substrate rather than a plan.
- It found the defect in task 5, which must be fixed before any number from this
  suite is quoted.
- cpp-httplib and Proxygen have **not been run at all** (task 6). Only Beast has.

The spike is why tasks 1–4 are written as narrowly as they are: they describe
code that exists, so they are a landing checklist rather than a design sketch.

---

- [ ] 1. The KV workload, independent of any transport
  - `tests/multi_raft_kv_workload.hpp`: `kv_put`/`kv_get`/`kv_del` to
    `test_key_value_state_machine`'s own encoding, `kv_partitioner`, fixed-width
    `kv_key`, deterministic non-constant `kv_value`, `kv_shard_ranges`
  - `key_sampler` over uniform and Zipfian
  - `latency_sample_set` whose `p99()`/`p999()` return `std::optional`, empty
    below 1,000 / 10,000 samples; `operation_tally` counting failures by cause
    and keeping `_offered` distinct from `_completed`; `benchmark_result`
  - _Requirements: 1.1–1.6, 4.4, 4.5, 5.1, 5.2_

- [ ] 2. Movable handles over the non-movable transports
  - `transport_client_handle` / `transport_server_handle` forwarding the three
    mandatory RPCs unconditionally and every optional one behind a `requires`
    clause on the underlying transport
  - _Requirements: 14.1, 17.7, 17.8_

- [ ] 3. A fixture per HTTP transport
  - `cpp_httplib_transport`, `beast_http_transport`, `proxygen_http_transport`,
    each owning its own runtime; `reserve_port()`; `harness_transport_types`
    pinned to `kythira::future_default`
  - _Requirements: 3.1 (Tier B), 3.2, 17.1, 17.12_

- [ ] 4. `kv_cluster` and the closed-loop workload driver
  - One tick driver thread per host; constant configuration across rows;
    `run_command` measuring from before the routing lookup and classifying every
    failure; `run_read_state`; `term_sum()`; `run_put_workload`
  - _Requirements: 4.1, 4.3, 7.8, 15.1, 15.3, 15.4, 17.11_

- [ ] 5. **Fix the teardown abort the spike found**
  - The Beast smoke case reports "No errors detected" and then the process
    aborts with `corrupted double-linked list` *after* the test module finishes.
    A benchmark whose teardown corrupts the heap cannot be trusted to have
    measured a clean steady state either
  - **It did not reproduce under `gdb -batch -ex run`** — that run exited
    normally. So it is intermittent, or sensitive to allocator layout. Do not
    read the clean gdb run as evidence the bug is elsewhere; run it repeatedly
    outside a debugger to establish a rate before and after any fix
  - Suspect ordering inside `beast_http_transport::shutdown()`: it currently
    clears `_clients`/`_servers`, then resets the work guard, then calls
    `_ioc.stop()`, then joins. `_ioc.stop()` kills pending handlers rather than
    letting them drain, and the Beast client's own destructor takes a drain
    mutex. Try resetting the work guard and letting `run()` exit naturally,
    joining, and only then destroying clients and servers — and confirm against
    the actual backtrace rather than the hypothesis
  - Reproduce under ASan and under TSan before declaring it fixed; `ptrace_scope=1`
    here means running the target *under* `gdb -batch -ex run`, not attaching
  - Re-check the same teardown path for cpp-httplib and Proxygen; the spike has
    only ever run Beast's
  - _Requirements: 15.5_

- [ ] 6. Prove the other two transports end to end
  - Run the cpp-httplib and Proxygen smoke cases; fix what they surface
  - Record cpp-httplib's actual per-operation cost, and confirm (or refute) that
    it tracks the documented ~83 ms Nagle/delayed-ACK round trip rather than
    something new
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

- [ ] 10. Statistical method
  - 5 repetitions, median headline with min/max spread, unstable flag above ±10%
    spread, warm-up counts reported
  - Machine description captured into the result
  - _Requirements: 6.1–6.6_

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
