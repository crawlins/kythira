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

- [x] 5. **Fix the teardown fault — measured, and it is Proxygen's, not Beast's**
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
  - **A session pool is written and measured, and it is not finished.**
    `pooled_connection` now holds a `session_pool` — sessions checked out
    exclusively for one transaction, `established` counting idle plus
    checked-out plus connecting against `connection_pool_size`, and a waiter
    queue where the old code returned an immediate failure. On
    `fix/proxygen-session-pool`, **not merged**. What it does on a Release
    build is exactly what the diagnosis predicted:

    | | run 1 | run 2 | run 3 |
    |---|---:|---:|---:|
    | `session unavailable`, before | 176 | 97 | 27 |
    | `session unavailable`, after | **0** | **0** | **0** |

    and the transport-axis row moved with it: Proxygen / JSON / 128 B / 16 in
    flight went from **355.1 ops/sec at ±23.8%** to **1455.7 ops/sec at
    ±16.0%**, from below Beast's row to slightly above it. Still UNSTABLE, so
    still not quotable — what changed is the size of the number, not its
    verdict. All six Proxygen-touching suites pass
  - **Everything below this line that was measured under AddressSanitizer was
    measured on a mis-linked binary, and every ASan conclusion in it is
    withdrawn.** `build-asan` is `CMAKE_BUILD_TYPE=RelWithDebInfo`, which has no
    imported configuration of its own, so CMake fell back to the first one
    vcpkg's imported targets list — `DEBUG` — and linked
    `vcpkg_installed/x64-linux/debug/lib/libproxygen.a` and `libfolly.a`, built
    without `NDEBUG`, into translation units compiled *with* `-DNDEBUG`. `NDEBUG`
    changes those libraries' class layouts: `sizeof(proxygen::HTTPSessionBase)`
    is 1624 with it and 1632 without, measured with this project's own include
    paths and defines. Every field of a session the library constructed was
    therefore read eight bytes off. Fixed by
    `CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO` in `CMakeLists.txt`
  - **The instrumentation the entry below asked for is what found it**, and it
    took the one rebuild it was budgeted. `newTransaction()` collapses every
    refusal to `nullptr`; proxygen's own `newTransactionWithError()` returns a
    reason string. Logged with the session's own counters at the moment
    `proxygen::HTTPConnector` handed the session over — before this pool had
    touched it — a freshly connected upstream session read:

    | build | `getNumOutgoingStreams()` | `getMaxConcurrentOutgoingStreams()` | `getNumStreams()` |
    |---|---:|---:|---:|
    | Release | 0 | 1 | 0 |
    | ASan (mis-linked) | 1 | 0 | 2839225156 |

    `supportsMoreTransactions()` is `out < max`, so it was false before the
    session had ever been used and *every* transaction was refused. Three
    diagnostics were decisive and cost nothing: the reason string was
    unanimously the stream limit and never `draining_`, all 7612 refusals across
    two runs named 7612 *distinct* sessions, and `getNumStreams()` returned a
    value that is fixed within a run and different between runs — deterministic
    garbage, not heap corruption
  - **The corrected numbers, twelve ASan runs each, and they settle both
    questions.** With the link fixed and nothing else changed:

    | | passes | sanitizer reports | `session unavailable` per run | wall clock |
    |---|---:|---:|---:|---:|
    | `main`, no pool | 12/12 | 0 | 38–379 | 2–25 s |
    | this branch, with the pool | **12/12** | **0** | **0** | **3–4 s** |

    against a documented pre-change baseline of 3 passes in 12 at 241 s. So the
    ASan memory faults were the build defect, and the refusal storm was not:
    it is real on a correctly linked sanitizer build too, at 38–379 per run, and
    the pool removes it there exactly as it does on Release. The 25-second tail
    on three of `main`'s twelve runs goes with it
  - **The two hypotheses ruled out below were both ruled out for the wrong
    reason, and stay closed anyway.** `k_election_budget` was genuinely
    unscalable — doctrine 54's defect, fixed on its own in #283 regardless of
    any of this. `capacity = 2` genuinely changed nothing. Neither experiment
    was wrong; both were run against a binary that could not have elected at
    any budget or any capacity
  - **It is not the budget, and that was worth ruling out.** The benchmark's
    `k_election_budget` was a hard-coded 30 s that no `KYTHIRA_TEST_TIMEOUT_SCALE`
    reached — doctrine 54's defect exactly, and fixed here regardless. With the
    ASan build reconfigured at scale 8 the case gets **241 seconds** and still
    elects nothing, accumulating 3823 refusals and **zero** connect errors
  - **Nor is it the connection count, and that experiment has been run.**
    Forced `capacity = 2`, rebuilt, three runs: **3754 / 3818 / 3807 refusals
    and no election**, against 3823 at capacity 10. Identical. Reverted — the
    count is not the mechanism, and the knob's default is not the fix
  - Both builds are `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly`, so the sanitizer and
    the Release build run the *same* (fast) path
  - `proxygen_server_config::max_concurrent_connections` was checked and
    exonerated: the field is declared and its doc comment describes an
    accept-time counter, but **nothing in the impl reads it**. Checked again
    since, and it is wider than that entry recorded: neither
    `beast_http_transport_impl.hpp` nor `http_transport_impl.hpp` reads its own
    copy either, so the field is inert in all three HTTP transports and the
    Proxygen doc comment's "the same precedent Beast used" describes enforcement
    that does not exist anywhere. Separate defect, not this task's
  - **Closed on a paired 60-run Release control, run back to back on one idle
    machine.** Everything above was argued from Release runs measured sessions
    apart, or from ASan runs on a binary that turned out to be mis-linked. This
    is the same case (`a_kv_cluster_commits_over_proxygen`), the same binary
    except for `include/raft/proxygen_http_*.hpp`, 60 runs each arm, with
    `main`'s headers rebuilt into `build-default` for the control:

    | arm | runs | `session unavailable` | runs with a storm | memory faults | mean | max |
    |---|---:|---:|---:|---:|---:|---:|
    | `main`, no pool | 60 | 10216 (170/run) | **60/60** | **0** | 7.0 s | 25.4 s |
    | this branch, pooled | 60 | **0** | **0/60** | **0** | 3.0 s | 3.7 s |

    Three things follow, and the third is the one that changes the task.
    **The storm is exactly as diagnosed** — every single control run has it, at
    the 170/run the earlier three-run sample predicted, and the pool removes all
    of it. **The 25-second tail is the storm's**, not a scheduling artefact: it
    is present in the control and absent from the pooled arm, whose *max* is
    nearer the control's *mean* than its max. And **the teardown fault this task
    is named for did not occur in either arm**
  - **So the fault is not "fixed" here; it is no longer reproducible.** The
    documented Release rate was 1 in 10. Zero in 60 puts the 95% upper bound at
    3/60 = 5% (rule of three), so a 10% rate is excluded — on `main`, with no
    pool, on the build type that was never mis-linked. Together with the
    withdrawal of every ASan figure above, **no correctly-built configuration
    currently reproduces the Proxygen teardown fault at all.** This box is
    ticked for the storm — measured, diagnosed, fixed, and controlled against —
    and explicitly not as a claim that a SEGV was chased down
  - **The leading hypothesis for where that 1-in-10 went is task 5a, and it is
    cheap to state.** The three smoke cases run in one process in declaration
    order: cpp-httplib, **Beast**, then Proxygen. Task 5a's fault is a Beast
    `asio_strand_executor` outliving its `io_context`, and it fires during a
    *later* fixture's startup, on a Folly thread, with Boost.Test attributing it
    to whichever case is current. That is precisely the shape task 5 was opened
    for — "after the Proxygen case's work was done, in its teardown" — and it
    would explain why three sessions spent inside `proxygen_client` found
    nothing: the object already destroyed was Beast's. **A hypothesis, not a
    finding**
  - **The hypothesis was tested and is neither confirmed nor refuted, because
    the control reproduces nothing either.** Thirty runs of all three smoke
    cases in one process — the exact shape the original "9 clean, 1 fault in
    10" was measured in — on a `build-default` carrying `main`'s Beast headers,
    and thirty more on the same build with 5a's fix in:

    | arm | runs | faults | smoke cases committed | mean |
    |---|---:|---:|---:|---:|
    | pre-fix Beast | 30 | **0** | 90/90 | 11.5 s |
    | post-fix Beast | 30 | **0** | 90/90 | 11.5 s |

    So the 1-in-10 is not currently reachable in this configuration at all, and
    an experiment whose control is silent decides nothing. Note what it does
    *not* say: the mechanism **is** available in the smoke suite — Beast's case
    tears its cluster down immediately before Proxygen's builds one, which is
    exactly 5a's shape — but only **once** per run against the value-size
    sweep's nineteen, so a per-teardown rate that saturates the sweep would
    still be sparse here. 0 in 30 puts the 95% upper bound near 9.5%, which is
    only just under the documented rate
  - **Leave the hypothesis open and stop spending runs on this shape.** Settling
    it needs a case that tears a Beast cluster down many times and *then* builds
    a Proxygen one — not more repetitions of a configuration in which neither
    arm faults
  - _Requirements: 15.5_

- [x] 5a. **A second teardown fault, on the Beast arm — not at 4096-byte values**
  - Found by the first full-sweep run on an idle machine.
    `write_throughput_by_value_size` — a **Beast-only** case — aborts with
    `memory access violation at address: 0x…: no mapping at fault address`,
    checkpointed at `multi_raft_http_benchmark_test.cpp:318`, which is the
    `kv_cluster` constructor of a *later* repetition. So it faults while
    standing the next cluster up, after the previous one has been torn down:
    the same shape, and the same "no mapping" signature, as the Proxygen fault
    task 5 was opened for
  - **It is not the session pool's, and the control says so rather than the
    argument.** Three runs on this branch: 3 faults. Three runs with
    `include/raft/proxygen_http_*.hpp` checked out from `main` and the case
    rebuilt: **2 faults in 3**. The case never instantiates `proxygen_client`
    in the first place; the control is what turns that from a claim into a
    measurement
  - Which also means the previous handoff's "the Beast `corrupted double-linked
    list` has not recurred since `6741ba9`" needs re-reading: it has not
    recurred *in the rows that were being run*. The 4096-byte row was never one
    of them, because no full sweep had completed
  - Not obviously the same fault `6741ba9` fixed — that one was a connection
    outliving the executor its callbacks were scheduled on, and this one faults
    building a fresh cluster. Establish the rate under ASan first, the way task
    5's was, before choosing between them
  - **Root cause found, and ASan names it exactly on the corrected build.**
    `build-asan` with `CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO` in place (its
    `link.txt` now names no `debug/lib/libproxygen.a`) reports, on **6 runs out
    of 6, mean 10 s to fault**, one signature every time:

    ```
    ERROR: AddressSanitizer: heap-use-after-free  READ of size 8  thread T18
      #0  asio::detail::strand_executor_service::strand_impl::~strand_impl()
                                        strand_executor_service.ipp:89
      #11 kythira::beast_detail::asio_strand_executor::~asio_strand_executor()
                                        beast_http_transport.hpp:430
      #21 kythira::beast_detail::asio_strand_executor::keepAliveRelease()
                                        beast_http_transport.hpp:474
      #22 folly::futures::detail::CoreBase::~CoreBase()          Core.cpp:437
      #47 error_handler<…>::execute_with_retry_impl  (node<…beast…>::send_append_entries_to)
      #59 folly::Future<folly::Unit>::delayed(…)               Future-inl.h:455

    freed by thread T0 here:
      #10 asio::detail::service_registry::destroy_services()
      #12 asio::execution_context::~execution_context()
      #13 asio::io_context::~io_context()
      #14 testing::beast_http_transport<…>::~beast_http_transport()
                                        multi_raft_transport_harness.hpp:391
      #15 testing::kv_cluster<…>::~kv_cluster()  multi_raft_transport_harness.hpp:612
      #16 one_measurement<…>            multi_raft_http_benchmark_test.cpp:369
    ```

    Read it as one sentence: **the main thread destroys the fixture's
    `io_context` at the end of a repetition, and a Folly future core on another
    thread then releases the last keep-alive on a Beast
    `asio_strand_executor`, whose `any_io_executor` member is the final owner of
    an Asio `strand_impl` — and `~strand_impl` dereferences
    `service_->mutex_`, which the `io_context` took with it.**
    `strand_executor_service.ipp:89` is that dereference, and it is the first
    line of the destructor, so there is no way to reach it safely
  - **The class's own doc comment states the precondition that is being
    violated.** `asio_strand_executor` (`beast_http_transport.hpp:411-427`)
    explains that it self-pins through Folly's `KeepAlive` protocol precisely so
    that "the object that owns it can be destroyed first", and justifies that
    with: "`_ex` is a copy of the stream's Asio executor and stays valid as long
    as the caller's `io_context` does, **which this transport already
    requires**." The two halves contradict each other. Opting into outliving
    your owner while requiring your owner's `io_context` to outlive *you* is
    safe only if something enforces the second, and nothing does — the harness
    fixture's `shutdown()` drains the Asio side correctly (`_clients.clear()`,
    `_work.reset()`, `_ioc.stop()`, join) and knows nothing about the Folly
    chains still holding keep-alives
  - **What holds the chain open is `error_handler`'s delayed retry** — frame #59
    is `folly::Future<Unit>::delayed`, on the process-wide Timekeeper, exactly
    the mechanism task 5's investigation named. It is the same *setter* as task
    5's stack and a completely different *victim*: there the argument was about
    `proxygen_client`'s pooled `EventBase*`, here it is Beast's strand. That is
    why "one fault mechanism on two fixtures is at least as likely as two",
    written when this task was opened, was the right instinct
  - **It is `main`'s, not this branch's, by construction and not by argument.**
    `git diff origin/main...HEAD` touches `proxygen_http_transport{,_impl}.hpp`,
    `CMakeLists.txt`, the spec and the benchmark's own `.cpp` — no Beast code at
    all — and `write_throughput_by_value_size` never instantiates a
    `proxygen_client`. The earlier "3 faults on this branch, 2 in 3 on main"
    control agreed; this supersedes it by making the control unnecessary
  - **It is not the 4096-byte row, and that matters for the title.** On Release
    the fault lands where it was first seen: the 4096 B row, entering repetition
    2, after 62 s. Under ASan it lands in the **16-byte** row, entering
    repetition 2, after 10 s. The value size is not a term in the mechanism — it
    only sets how much wall clock passes before a fixture is torn down with a
    retry still armed, and 4096 B is simply the slowest row (40.6 ops/sec
    against 1024 B's 847, a 20x cliff worth its own look). **Any row can fault;
    the sweep just reaches 4096 B last.** Retitle when this closes
  - **There is now a 10-second, 6-out-of-6 instrument**, which changes what a
    fix has to clear. Every earlier lifetime attempt in this spec was judged
    against a 1-in-10 or 1-in-4 rate needing a dozen runs per arm; this one can
    be falsified in a minute. Use it, and count hangs separately from faults as
    task 5 had to
  - **Two candidate fixes, and neither is applied, deliberately.** This spec has
    twice recorded that an unverified lifetime change that fixes nothing is
    worse than the knowledge that it does not, and both candidates are ownership
    changes rather than patches:
    1. **Make the `io_context` outlive its executors.** The honest reading of
       the doc comment: hand `asio_strand_executor` a `shared_ptr<void>` keeper
       owning the `io_context`, plumbed from whoever constructs it. **Sized, and
       it is smaller than it sounds — four additive defaulted parameters on the
       client path only.** `asio_strand_executor` is constructed in exactly two
       places, `beast_http_transport_impl.hpp:397` and `:460`, the plain and TLS
       connection constructors, each from its own `_stream.get_executor()`;
       those two connections are created in exactly one place,
       `boost_beast_client<Types>::make_connection` (`:722`, `:725`), from
       `net::make_strand(_ioc)`. So a `std::shared_ptr<void> context_keeper = {}`
       defaulted onto `asio_strand_executor`, both connection constructors and
       `boost_beast_client`'s constructor leaves all fourteen existing
       `boost_beast_client`/`boost_beast_server` construction sites compiling
       untouched, and only the harness fixture — which owns the `io_context` and
       is the thing destroying it too early — passes one.
       **`boost_beast_server` needs nothing at all**: `server_session` holds its
       executor *by value*, so `keepAliveAcquire()` returns `false` there, it
       never self-pins, and it cannot outlive anything. That asymmetry is
       already documented in `keepAliveAcquire()`'s own comment, and it is what
       makes the fix client-only
    2. **Detach the executor before the `io_context` dies** — clear `_ex` so a
       surviving executor holds no Asio state, and drop work posted afterwards.
       Narrower, and there is precedent in the same file (the non-Folly
       `asio_strand_executor` already has `close()`/`closed()`). The hazard is
       that a `beast_connection` is also destroyed on ordinary pool eviction,
       mid-run, where dropping a continuation would silently strand a live RPC —
       so a detach must be driven by "the `io_context` is going away", which is
       knowledge the connection does not have today
    Direction 1 is the one to design; direction 2 is what to reach for if 1's
    constructor change proves too wide for this spec
  - **Direction 1 is built and measured, and it clears the reproducer.** A
    `std::shared_ptr<void> context_keeper`, defaulted empty, now threads through
    `asio_strand_executor`, both `beast_connection` subclasses and
    `boost_beast_client`; the benchmark's Beast fixture holds its `io_context`
    by `shared_ptr` and passes it as that keeper. **Member declaration order is
    the load-bearing part of the change** — the keeper is declared *before*
    `_ex` in the executor and before `_stream` in each connection, because
    members are destroyed in reverse declaration order and the whole point is
    that the `io_context` is released *after* the Asio strand that needs its
    service registry. `shutdown()` no longer owns the `io_context`'s
    destruction: it stops it and joins every io thread, then drops the
    fixture's reference, and whichever reference goes last destroys it — safely,
    because nothing is inside `run()` by then, which is all `~io_context`
    requires.

    | | runs | time to fault | `heap-use-after-free` | ASan reports | repetitions completed |
    |---|---:|---|---:|---:|---:|
    | before | 6 | mean 10 s, max 14 s | **6/6** | 6 | 6 (one per run) |
    | after | 6 | none within a 180 s cap | **0** | **0** | **77** |

    The post-fix protocol is a hard 180-second cap rather than a run to
    completion: the fault used to arrive after *exactly one* repetition inside
    fourteen seconds, so twelve times its mean with zero reports is the
    falsifying test, and `reps` is recorded so that a clean run cannot be a run
    that did no work. Seventy-seven repetitions replaced six.
  - **On Release the sweep now finishes, which it had never done.** The case
    that opened this task ran end to end for the first time, all four value
    sizes, five repetitions each, exit 0 — and produced the payload axis's first
    real numbers: 16 B 1462.1 ops/sec (spread 7.4%), 128 B 1417.2 (3.8%),
    1024 B 809.5 (5.2%), **4096 B 56.3 (43.7%, UNSTABLE)**. The 1024→4096 cliff
    is a factor of fourteen and the only unstable row of the four; it wants its
    own look under task 8 or 11, and it is not a teardown question
  - **Regression surface checked, not assumed.** All six Beast suites
    (`beast_client_test`, `beast_server_test`, `beast_integration_test`,
    `beast_ssl_test`, `beast_negotiation_retry_test`,
    `beast_cross_transport_equivalence_test`) pass, as do all three benchmark
    smoke cases. Every existing `boost_beast_client`/`boost_beast_server`
    construction site compiles untouched, because the parameter is defaulted —
    and `boost_beast_server` was given nothing at all, since `server_session`
    holds its executor **by value** (`beast_http_transport_impl.hpp:1356`), so
    `keepAliveAcquire()` returns `false`, it never self-pins, and it cannot
    outlive anything. That asymmetry was verified in the tree, not inferred
    from the comment that describes it
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

- [x] 7. Complete the RPC-provider axis
  - CBOR and protobuf rows over Beast; Ion behind its own gate
  - Confirm each row's label comes from the serializer's own `media_type()`
  - Assert the node-internal serializer stayed at JSON across every row
  - **Three of the four are done; only Ion is outstanding.** The CBOR and
    protobuf rows are in `write_throughput_by_rpc_serializer`, the second behind
    `KYTHIRA_BENCH_HAS_PROTOBUF` with a `BOOST_TEST_MESSAGE` saying so when the
    gate is off rather than a silently missing row
  - **Row labels already come from the serializer, and now so does the
    assertion.** `run_put_workload` sets `_serializer` from
    `Transport::transport_bundle::serializer_type{}.media_type()`, so a row
    cannot disagree with the wire. The node-internal serializer was pinned only
    by the `kv_host_types::serializer_type` alias — a pin nothing read, and so a
    pin that could be moved silently. It is now carried on `benchmark_result` as
    `_node_serializer`, read off `kv_cluster<Transport>::types` (the bundle the
    cluster was actually built from, not a re-derivation of it), and checked
    against `application/json` in `one_measurement` — every repetition of every
    row in every sweep, not just the serializer axis
  - **The three existing rows came back stable, which is new for this suite.**
    Run with the assertion above in place, `build-default`, five repetitions
    each, 600 operations at 16 in flight over 128 B values:

    | wire serializer | headline | min | max | spread | verdict |
    |---|---:|---:|---:|---:|---|
    | `application/json` | 1310.0 ops/sec | 1201.4 | 1417.6 | 8.3% | stable |
    | `application/cbor` | 1512.5 ops/sec | 1404.3 | 1573.4 | 7.2% | stable |
    | `application/x-protobuf` | 1525.3 ops/sec | 1444.7 | 1590.8 | 5.3% | stable |

    All three clear the ±10% bar, so `repeated_result` calls them comparable and
    does not print the "MUST NOT ENTER A COMPARISON TABLE" banner — the first
    rows in this suite to manage it. **They still carry "machine was not quiet at
    start"**, which is the provenance flag, not the stability verdict, and this
    spec's standing rule is that a number is re-measured on a host whose
    one-minute load is genuinely below 0.5 before it is quoted anywhere. Read
    them as "the axis now produces stable rows", not as the answer: the ordering
    (CBOR and protobuf within 1% of each other and ~15% above JSON) is the shape
    to confirm, not to publish
  - **Ion's row is in, behind `KYTHIRA_BENCH_HAS_ION`, and it has been
    measured.** Its gate is `if(TARGET raft_ion_serializer)`, the same shape as
    protobuf's — but its *off* case is not the same situation and the code says
    so rather than printing the same message. ion-c is installed in this
    environment; what is unset is `CONFIG_ION_SERIALIZER`, which **every
    checked-in defconfig deliberately leaves out** (`configs/ci_full_defconfig`
    explains why: ion-c comes from the opt-in `ion` vcpkg feature). So this row
    compiles out on every CI leg and in `build-default`, and measuring it needs
    a build configured with that symbol selected — which is what was done here,
    rather than shipping a row that had never been compiled
  - **Ion is the slowest wire serializer on this axis, and the label proves the
    label mechanism.** One run, five repetitions each, 600 operations at 16 in
    flight over 128 B values, on an Ion-enabled Release build:

    | wire serializer | headline | spread | verdict |
    |---|---:|---:|---|
    | `application/json` | 1416.0 ops/sec | 20.2% | UNSTABLE |
    | `application/cbor` | 1533.1 ops/sec | 4.1% | stable |
    | `application/x-protobuf` | 1557.5 ops/sec | 3.2% | stable |
    | `application/x-amzn-ion` | 1089.1 ops/sec | 13.9% | UNSTABLE |

    **`application/x-amzn-ion` is the strongest evidence in this spec that row
    labels come from the serializer and not from a hand-written string.** Ion is
    the one serializer here whose `media_type()` depends on instance state —
    binary and text are different media types off the same class — and the row
    printed the binary one because that is what the default-constructed
    serializer on the wire actually was. Nothing in the benchmark names Ion's
    media type anywhere
  - Note the JSON row came back at 20.2% here against 8.3% on the earlier run
    an hour before, on the same machine and the same binary path. Nothing about
    JSON changed; the machine did. That is the spread doing its job, and it is
    why none of these numbers may be quoted until they are taken on a host whose
    one-minute load is genuinely below 0.5
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

- [x] 14. A new workflow file, not new inputs
  - `.github/workflows/perf-cloud.yml`, explicitly dispatched, no default
    schedule, layered opt-in
  - **Do not add a `workflow_dispatch` input to `real-cloud-tests.yml`** — it is
    at exactly 25, which is the cap, and exceeding it silently invalidates the
    whole file. Confirm `ci.yml`'s guard job still passes after the new file
    lands
  - Short-lived OIDC credentials only, reusing `scripts/ci-cloud-credentials/`
  - **Done, and `real-cloud-tests.yml` was not touched at all.** The new file
    carries 5 inputs against its own budget; `real-cloud-tests.yml` still reads
    exactly 25. `ci.yml`'s `workflow-input-limits` guard was run locally against
    the new tree and passes — `coap-flake-measure` 8, `perf-cloud` 5,
    `prune-actions-caches` 1, `real-cloud-tests` 25
  - **The opt-in is the same two-layer shape** `real-cloud-tests.yml` uses: a
    per-run `workflow_dispatch` input wins when given, otherwise the repository
    variable (`PERF_CLOUD_ENABLED` / `PERF_CLOUD_AWS_ENABLED`) decides, otherwise
    the job does not run. No schedule, so a run that costs money is always a
    decision somebody made
  - **Credentials are the existing OIDC role, and it already suffices.**
    `vars.AWS_CI_ROLE_ARN` (`kythira-ci-real-cloud-tests`) was inspected rather
    than assumed: its inline policy carries 123 `ec2:` actions including
    `RunInstances`, `TerminateInstances`, `DescribeInstances`,
    `DescribeInstanceTypes`, `DescribeImages` and `CreateTags`. **Task 16 needs
    no new IAM work**, and no long-lived key is introduced
  - _Requirements: 18.1, 18.2, 18.3_

- [x] 15. Machine provenance capture
  - A script the job runs on the instance that records provider, region, AZ,
    instance type, vCPU count and CPU model as the guest reports it, memory,
    stated network performance, storage class and IOPS, image, kernel and tenancy
    into the result JSON
  - Fail the run if the instance type is burstable
  - **`scripts/perf-cloud/capture-provenance.sh`, and the fields split into two
    kinds for a reason.** *Guest-observed* — vCPU, CPU model, memory, kernel,
    arch — come from `nproc`, `/proc/cpuinfo` and `uname` on the instance,
    because Requirement 18.4 asks for the CPU "as the guest reports it" and what
    the hypervisor advertises is a different claim from what the guest is
    scheduled on. *Provider-stated* — network performance, storage class, IOPS —
    are control-plane facts invisible from inside the guest, so the workflow
    reads them with `describe-instance-types` where the credentials already are
    and passes them in. **The instance itself needs no cloud permissions**,
    which is worth more than the tidiness: an instance with an instance profile
    is an instance that can be used to do something else
  - **Unavailable fields are emitted as `null`, never omitted and never
    guessed** — a row whose provenance silently lost a field is worse than one
    that says it does not know, because only the second is visible in the
    artifact. Tenancy in particular is *not* defaulted to `shared`: assuming the
    common case is exactly how a dedicated-tenancy run gets mislabelled
  - **The burstable refusal is tested in both directions**, which matters
    because it is a guard that should never fire in normal use and so would
    otherwise never be exercised: `t3.large`, `t4g.medium`, `Standard_B2s` and
    `e2-micro` all exit 1 with the reason; `c5.2xlarge`, `c7g.2xlarge` and
    `m5.2xlarge` all exit 0 and land the type in the JSON. It is a hard failure
    rather than a flag on the row, because a row that should never be published
    is cheaper to prevent than to explain
  - Run on the development machine it exists to escape, it reports
    `Intel(R) Core(TM) i5-6300U @ 2.40GHz`, 4 vCPU — which is Requirement 18's
    user story stated as data
  - _Requirements: 18.4, 18.5, 6.4_

- [ ] 16. Shape 1 — one AWS instance, Tier B
  - Provision one non-burstable instance, build or fetch the benchmark binary,
    run the same scenarios with the same configuration as the local rows, pull
    the artifacts back, tear down
  - Publish the local-vs-cloud delta as its own finding: that number *is* the
    hardware confound
  - _Requirements: 18.6, 18.7, 18.13_

- [x] 17. Cost and safety controls
  - Pre-registered cost estimate per run, in the spec's own doc
  - Hard wall-clock ceiling on the measured phase
  - Unconditional teardown that runs even when the measurement fails
  - Post-run leak audit that **fails the job** if anything provisioned still
    exists — instances, volumes, addresses, security groups, placement groups
  - **Done before task 16, deliberately.** The controls that bound a spend land
    before the thing that spends; the reverse order is how a first live run
    becomes the one that leaks
  - **`doc/multi_raft_performance_cloud_cost_estimate.md` carries real rates,
    not estimates of rates.** Six candidate non-burstable types read from the
    AWS Pricing API on August 29, 2026 — `c6g.2xlarge` $0.2720/hr through
    `m7i.2xlarge` $0.4032/hr. Shape 1 at the 54-minute ceiling is **$0.31** on
    `c5.2xlarge`; the *expected* run is ~20 minutes, so **$0.11–$0.13**. Shape 2
    (three instances) is $0.93. **8 vCPU is chosen to match the local machine's
    core count rather than to exceed it** — the point of 18.7 is to remove the
    hardware confound, and a 16-vCPU cloud row against a 4-core local one
    replaces it with a bigger one
  - **Three independent ceilings**, because the interesting failure is the one
    where the first two do not run: `timeout` around the benchmark, the job's
    own `timeout-minutes`, and the unconditional teardown plus audit. Only the
    third proves *billing* stopped rather than that *work* stopped
  - **The audit is `scripts/perf-cloud/audit-aws-leaks.sh` and it was tested
    against real AWS in both directions.** Clean tag: six resource classes
    queried, exit 0. Then a free security group was created carrying the run
    tag, and the audit found it, printed it, and **exited 1**; the group was
    deleted and the re-audit came back clean. A leak detector whose failure path
    has never fired is a leak detector nobody has tested
  - Everything keys off one run-scoped tag (`kythira-perf-run=perf-<run>-<attempt>`)
    rather than a list of resource names kept in sync with the provisioning
    code, and the workflow tags **at creation time** in
    `--tag-specifications` rather than in a later `CreateTags` that a crash
    could skip — an untagged resource is invisible to this audit
  - **One gap, stated rather than hidden**: if the GitHub runner dies between
    `RunInstances` and teardown, nothing on the AWS side stops the instance, and
    the leak is caught by the *next* run's audit. Closing it wants an
    instance-side scheduled `shutdown -h`; the exposure is bounded by the
    workflow being dispatch-only
  - **Testing the failure path found two defects that a working audit would
    have hidden, and both are the same shape: a leak auditor that cannot see
    reports "clean".**
    1. Every query was written `2>/dev/null`, so an `AccessDenied` produced
       empty output, which counted as zero survivors, which printed `clean` and
       exited 0. **The script would have reported success precisely when it had
       lost the ability to detect anything.** Failed queries are now counted
       separately and fail the job on their own, saying UNKNOWN rather than
       clean.
    2. The first fix did not work either. Under `set -e`, `out=$(cmd)` takes
       the command's exit status, so a failing describe aborted the script
       before any of the new handling ran — measured as exit **254** with no
       diagnosis. `out=$("$@" 2>&1) || rc=$?` makes it a tested command, which
       `set -e` leaves alone
  - **The CI role is missing four of the actions this needs, and that was found
    by reading the policy rather than by a failed run.**
    `kythira-ci-real-cloud-tests` has `ec2:DescribeVolumes` and
    `DescribeSecurityGroups` but **not** `DescribeAddresses`,
    `DescribePlacementGroups` or `DescribeKeyPairs`, and has singular
    `ssm:GetParameter` but not `GetParameters`. Three of the audit's six queries
    would have failed in CI — and before fix (1) above, failed *silently as
    clean*. `scripts/ci-cloud-credentials/aws/policies/perf-cloud.json` adds
    them as a new bundle; **the role must be re-provisioned with that bundle
    before task 16 runs**
  - All three audit paths are verified against real AWS: clean → exit 0; a
    tagged (free) security group → found, printed, exit 1, then deleted and
    re-audited clean; a credential that cannot query → 6 UNKNOWN reports and
    exit 1
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
