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
  - **ASan named it on the second run**, exactly as
    `doc/`-recorded practice for this repo predicts (prefer ASan to gdb for a
    timing-dependent crash here). `build-asan` configured with
    `-DKYTHIRA_SANITIZER=address -DCMAKE_BUILD_TYPE=RelWithDebInfo`, run with
    `LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libasan.so.8.0.0
    ASAN_OPTIONS=detect_leaks=0`

    ```
    SEGV on unknown address 0x03e8003d5b31 — WRITE
      #1 folly::AtomicNotificationQueue<Function<void()>>::push
      #3 folly::EventBase::runInEventBaseThread          EventBase.cpp:926
      #4 proxygen_client<…>::send_rpc_folly_fast_path    proxygen_http_transport_impl.hpp:1127
      #7 proxygen_client<…>::send_rpc                    proxygen_http_transport_impl.hpp:1280
      #8 transport_client_handle<…>::send_append_entries multi_raft_transport_harness.hpp:123
     #10 node<…>::…                                      raft.hpp:5656
     #11 error_handler<…>::execute_with_retry_impl       error_handler.hpp:715
     #12   …the delayed-retry continuation                error_handler.hpp:704
    ```

  - **What sets it off** is `error_handler`'s delayed retry. It fires on a
    *process-wide* timer (folly's Timekeeper — nothing ties it to any owner's
    lifetime) after a 700–900 ms exponential backoff, and the storm of
    `proxygen: session unavailable for new transaction` immediately before the
    fault is the same teardown in progress: every failing send arms another
    retry. Frame #12 is *past* `error_handler.hpp:696`'s `scope->enter()` /
    `stop_flag` guard, so the owner had not been marked stopped yet
  - **The obvious reading of that stack is wrong, and the experiment that
    disproved it is worth not repeating.** "The retry outlives the
    `proxygen_client`" was the first hypothesis. Holding the client by
    `weak_ptr` in `transport_client_handle` and draining the retry storm before
    destroying clients was implemented and measured: the fault **survived
    unchanged**, and the new stack shows the `lock()` *succeeding* — frame #8
    moved into the guarded path. **The client is alive when the fault happens.**
    That change was reverted rather than kept: it costs a `weak_ptr::lock()` on
    every RPC in a suite whose whole purpose is measuring RPC throughput, and it
    fixes nothing
  - **So the dangling thing is inside `proxygen_client`, on a live client.**
    ASan reports "SEGV on unknown address", not heap-use-after-free — an
    *unmapped* pointer, not a freed-and-quarantined heap object. A destroyed
    `folly::EventBase` would have been reported as the latter, since ASan tracks
    it. An unmapped value is what you read out of memory that is no longer the
    object you think it is
  - **A second hypothesis was tested and is also wrong.** "`get_or_create_slot()`
    returns `pooled_connection&` and the caller reads `slot.event_base` after
    `_mutex` is released" is the shape `6741ba9` fixed in Beast, and it is
    genuinely there in `proxygen_http_transport_impl.hpp:1102-1111` (and 894-903
    on the generic path). It was fixed — the accessor changed to copy
    `event_base`, `session` and `connect` out by value under the lock — and
    measured: **6 faults in 12 ASan runs, an unchanged rate, with an identical
    stack**. Reverted, for the same reason the first attempt was: an unverified
    change to production code that fixes nothing is worse than the knowledge
    that it does not
  - **What the evidence actually points at.** The faulting address is
    *unmapped*, not freed-and-quarantined heap — which is what a **stack- or
    TLS-resident** object looks like after its thread is gone, and is why ASan
    cannot label it. folly's `IOThreadPoolExecutor::threadRun` takes its
    `EventBase` from the thread-local `EventBaseManager`
    (`IOThreadPoolExecutor.cpp:219`) and clears it at thread exit
    (`eventBaseManager_->clearEventBase()`), so the `EventBase` dies with its IO
    thread. `proxygen_client` caches that raw `EventBase*` in
    `pooled_connection` for the whole connection lifetime — deliberately, per
    Requirement 21.3, since an `HTTPUpstreamSession` is permanently pinned to
    the `EventBase` it was created on — and holds **no keep-alive on it**. Once
    `IOThreadPoolExecutor::join()` has run, that pointer addresses an unmapped
    thread-local. Both failed hypotheses are consistent with this: the client
    being alive does not help, and copying the pointer out under a lock copies a
    pointer that is already dead
  - **The candidate fix, and the reason it was not just applied.** Hold
    `folly::Executor::KeepAlive<folly::EventBase>` in `pooled_connection` and in
    whatever the RPC captures, instead of a bare `EventBase*` — folly's own
    answer to this, and the same lesson as the Beast fix's
    `keepAliveAcquire`/`keepAliveRelease` work. The risk that needs designing
    for first: a KeepAlive held by an in-flight retry **delays
    `IOThreadPoolExecutor::join()`**, and `error_handler` re-arms on failure
    with a growing backoff, so a naive version can turn a use-after-free into a
    teardown that blocks for seconds. Ordering already helps — every fixture
    clears its clients (releasing the pooled KeepAlives) before joining the pool
    — but this belongs in a designed transport CR, not a patch at the end of an
    investigation
  - **TSan reproduces it too, at 3 runs in 8** — and it takes a moment to see
    that, because TSan reports this one as
    `ThreadSanitizer:DEADLYSIGNAL` / `ERROR: ThreadSanitizer: SEGV on unknown
    address`, not as the `WARNING: ThreadSanitizer: data race` a grep for TSan
    findings would look for. The faulting address is from the same narrow family
    ASan reported (`0x03e8003d5b31`, `0x03e8003d6251`, `0x03e8003d6ab0`), so it
    is the same defect. Rates so far: **gdb 0/20, TSan 3/8, ASan 2/3, plain
    build 1/10**. Only the debugger hides it
  - Those TSan runs also named an unrelated **real** race in
    `console_logger::format_timestamp` — `std::localtime`'s shared `std::tm`,
    and glibc's `tzset_internal` freeing timezone state under it — which is
    fixed separately. With that fixed, TSan reports **no data race at all** on
    this suite, which is what makes the remaining SEGV easy to see
  - **The KeepAlive was built, in both scopings, and both regress. Do not
    build it a third time until the storm below is gone.** Holding
    `folly::Executor::KeepAlive<folly::EventBase>` in `pooled_connection`
    instead of the bare pointer compiles and is the right *shape*; it also
    turns the fault into a teardown that never finishes, which is the trap the
    previous handoff predicted and is now measured rather than feared:

    | Scoping | Result |
    |---|---|
    | KeepAlive `.copy()` captured into each RPC's closure and its terminal continuation | run killed at the 600s cap; the test body finished (`8/8 operations committed`) and the process then spun on CPU for ~9 minutes producing no output |
    | KeepAlive held only by the pooled slot, released when the client is destroyed, no per-RPC copy | **4 runs, 4 hangs**, each killed at 300s. The slowest of the twelve baseline runs was 158s |

    The second scoping was the attempt to dodge the trap: a token whose
    lifetime is the *client's* is released by `_clients.clear()` whether or not
    every chain it started ever settled, so it cannot be held hostage by an
    unsettled retry. It hangs anyway, and — the part that matters — it hangs
    **inside the test body**, before a single operation commits, not in
    teardown. Something about holding the loop open changes steady-state
    behaviour, not just shutdown. Both were reverted; the tree carries neither
  - **The baseline that "6/12" came from was mis-classified, and the corrected
    reading is worse.** Re-reading the same twelve ASan logs by outcome rather
    than by "did ASan print anything": **6 ASan faults, 3 ASan-clean test
    failures, 3 passes**. The six non-faulting runs were not six clean runs.
    `a_kv_cluster_commits_over_proxygen` passes **1 run in 4** under ASan
    before any change is made, which is the instrument every task-5 measurement
    has been read off
  - **The storm is not teardown noise — it is the defect, and it is present in
    a passing three-second run on an idle machine.**
    `proxygen_detail::connect_if_needed` reuses `*session_slot` when
    `existing->isReusable()`, and `send_on_session[_folly]` then calls
    `session->newTransaction()`, which returns `nullptr` and fails the RPC with
    `proxygen: session unavailable for new transaction`. Between those two
    steps sits a `.via(evb)` hop, and behind them sits one pooled
    `HTTPUpstreamSession` per target over **plaintext HTTP/1.1**
    (`loopback_url()` is `http://`), whose
    `getMaxConcurrentOutgoingStreams()` is 1. A multi-Raft leader replicates
    four groups to the same peer concurrently, so several RPCs pass the
    `isReusable()` check together and all but one are refused. `error_handler`
    then re-arms each refusal with exponential backoff — which is where the
    storm, and the retry that owns the faulting frame, both come from
  - **The control says it is Proxygen's, not the harness's.** Three
    repetitions of each smoke case, `build-default`, idle machine, counting
    `session unavailable` / `connection unavailable` in the log:

    | Transport | run 1 | run 2 | run 3 |
    |---|---:|---:|---:|
    | Beast | 0 | 0 | 0 |
    | cpp-httplib | 0 | 0 | 0 |
    | **Proxygen** | **176** | **97** | **27** |

    against roughly 900–1100 `Sending AppendEntries` per run. Retries recover
    most of them on a fast machine (~93% of sends are still received), which is
    why this has been invisible: it costs latency and log volume, not
    correctness. Under ASan the overlap widens until only **20–33%** of
    AppendEntries are ever received — 25452 sent / 5335 received in one
    *passing* baseline run — and that is the regime the teardown fault lives in
  - **This is the same class of defect `6741ba9` fixed in Beast** (`fix(beast):
    check connections out exclusively per RPC`) and it was never fixed in
    Proxygen. Beast's zero column above is that fix
  - **What the next CR should do**, and why it comes before any KeepAlive
    verdict: give `pooled_connection` a small pool of sessions per target with
    exclusive checkout, all pinned to the same `EventBase` so Requirement 21.3
    still holds, bounded by `connection_pool_size` the way Beast's is, with a
    waiter queue rather than an immediate failure when the pool is empty.
    Checkout and release happen on the pinned `EventBase`, so the pool needs no
    mutex; `session_liveness_tracker` has to remove a dying session from the
    pool rather than null a single slot. Until that lands, the
    KeepAlive cannot be evaluated: its measured failure mode is a teardown that
    outlives a retry storm, and the storm is the thing being removed
  - Verify with **ASan**, and against the corrected baseline (6 faults / 3 test
    failures / 3 passes in 12), never with gdb, which has never once reproduced
    it. Count hangs separately from faults: no baseline run exceeded 158s, so a
    run past 300s is a new failure mode and not a slow one
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
