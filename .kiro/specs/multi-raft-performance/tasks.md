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
    own look under task 8 or 11, and it is not a teardown question.
    **The cliff is explained now — see the value-size cross-row read below.**
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

- [x] 8. The concurrency and distribution axes
  - `write_throughput_by_concurrency`: in-flight 1, 8, 64 — the sweep that
    answers whether single-group throughput stops rising (H7)
  - Uniform vs. Zipfian at fixed concurrency, which is the only configuration in
    the matrix that concentrates load on one group
  - Report **entries per AppendEntries** and **RPCs per committed entry** as a
    function of concurrency (H1, H2)
  - **The concurrency sweep runs in both distributions, and that is what makes
    it answer 8.7 rather than something adjacent.** Requirement 8.7 asks for
    *single-group* throughput against concurrency. The uniform arm cannot supply
    it: uniform keys over a four-shard tiling spread the load across four
    `node<Types>` instances, so its curve is the cluster's, while H7 is a claim
    about one group's mutex. The Zipfian arm at theta 0.99 puts nearly every key
    in the lowest shard and is as close to a single-group curve as this fixture
    reaches without changing the cluster shape every other row shares.
    `write_throughput_by_key_distribution` then repeats the pair at 16 in
    flight — the configuration the transport, serializer and value-size rows all
    use — so the Zipfian number can be quoted against those tables and not only
    against itself
  - **The instrumentation is in the harness, not in production code.**
    `rpc_counters` (five relaxed atomics) hangs off `kv_cluster` and every
    `transport_client_handle` the cluster hands its hosts points at it, so the
    three mandatory RPCs are counted on the way out. That is the one point on
    the send path all three HTTP transports share, which makes the figures
    comparable across the transport axis by construction. `run_put_workload`
    takes an `rpc_snapshot` before the clock starts and one after the workers
    join and carries the difference on the row, which is what confines a ratio
    to the measured window — the warm-up, the election and the teardown all move
    the atomics and none of them belong in "RPCs per committed entry". Empty
    AppendEntries are counted apart from entry-bearing ones, because heartbeats
    are a function of the heartbeat interval rather than of offered load
  - **Two runs, eight rows each, 40 repetitions per run, `build-default`.** Both
    runs: **zero elections in any measured window** (40/40 `term sum … (steady)`)
    and **zero failed operations of any kind** — no `not_leader`, no `timeout`,
    no `other`. That is the cleanest data this suite has produced, and it means
    every number below describes steady-state replication rather than recovery.
    Run 2 was taken at a one-minute load of 4.06 against run 1's 2.04, which is
    why its throughput column is uniformly lower and why having both matters:

    | arm | in flight | run 1 ops/sec | run 2 ops/sec | H1 entries/AE run 1 | run 2 | H2 RPCs/commit run 1 | run 2 |
    |---|---:|---:|---:|---:|---:|---:|---:|
    | uniform | 1 | 1162.1 | 930.0 | 1.03 | 1.04 | 4.08 | 4.16 |
    | uniform | 8 | 1229.4 | 1184.3 | 2.57 | 2.61 | 3.79 | 3.89 |
    | uniform | 16 | 1379.2 | 1136.5 | 4.77 | 4.71 | 3.74 | 3.95 |
    | uniform | 64 | 727.4 | 596.6 | 29.51 | 25.85 | 4.29 | 4.66 |
    | zipfian | 1 | 1460.5 | 1216.2 | 1.08 | 1.10 | 3.77 | 4.04 |
    | zipfian | 8 | 1403.9 | 1145.0 | 6.60 | 6.56 | 3.83 | 3.87 |
    | zipfian | 16 | 1221.1 | 804.3 | 11.48 | 11.43 | 3.93 | 4.41 |
    | zipfian | 64 | 513.1 | 373.7 | 48.73 | 46.95 | 5.28 | 6.64 |

  - **H2 is REFUTED, and the number that refutes it is the flat column.** H2
    predicted that N concurrent submissions to one group issue N
    `replicate_to_followers()` calls, so RPCs per committed entry should rise
    with N. Across a 64-fold range of concurrency, in both distributions and in
    both runs, it sits between 3.74 and 6.64 — it does not scale with N at all.
    The residual rise at 64 in flight (4.29→4.66 uniform, 5.28→6.64 Zipfian) is
    real but is a few tens of percent against a 64-fold change in the
    independent variable
  - **H1 is REFUTED as stated, and what replaces it is more specific than a
    tick.** At one operation in flight H1 is exactly right — 1.03 entries per
    AppendEntries, one client call, one log entry, one replication round. But
    the factor rises to 25.9–29.5 (uniform) and 47.0–48.7 (Zipfian) at 64 in
    flight. Batching exists; nothing in the tree configures it. **The
    AppendEntries *rate* is what identifies the mechanism:**

    | arm | in flight | window | AppendEntries | AppendEntries/sec |
    |---|---:|---:|---:|---:|
    | uniform | 1 | 0.344 s | 1632 | 4741 |
    | uniform | 8 | 0.325 s | 1516 | 4659 |
    | uniform | 16 | 0.429 s | 2216 | 5163 |
    | uniform | 64 | 2.200 s | 6869 | 3123 |
    | zipfian | 1 | 0.274 s | 1506 | 5499 |
    | zipfian | 8 | 0.285 s | 1534 | 5384 |
    | zipfian | 16 | 0.485 s | 2324 | 4794 |
    | zipfian | 64 | 3.118 s | 8446 | 2709 |

    From 1 to 16 in flight the offered load rises sixteen-fold and the
    AppendEntries rate does not move (4659–5499/sec); at 64 it *falls*. A
    replication round driven by proposals would track the proposals. One driven
    by the tick would not — and 4 groups x 2 followers at the 2 ms tick is
    4000 rounds/sec, which brackets the measured rate from below. So entries
    accumulate between tick-driven rounds and the batching factor is arrival
    rate times inter-round interval. That is **incidental coalescing**, which is
    precisely the distinction Requirement 8.5 exists to draw, and it is why the
    thirty-fold rise in batching buys no throughput.
    **CORRECTED by task 11: the round is not tick-driven.** The bracket above is
    a coincidence of one cadence. Sweeping the tick 1 → 20 ms leaves the
    per-stream inter-round interval at 1.89–2.26 ms, so at a 20 ms tick a stream
    issues about nine rounds per tick. The refutation of H1 stands and so does
    "incidental coalescing"; what does not is the mechanism named for it. What
    the batch size actually tracks is proposals outstanding **per group** — 1.03
    / 2.57 / 4.77 / 29.5 at 1 / 8 / 16 / 64 in flight over four shards, and 6.60
    / 11.48 / 48.73 in the single-group arm. See task 11 and H6
  - **H7 is CONFIRMED, and the answer to "the concurrency at which it stops
    rising" is "it never rises".** In the hot-group arm throughput falls
    monotonically from a single in-flight operation: 1460.5 → 1403.9 → 1221.1 →
    513.1 in run 1, and 1216.2 → 1145.0 → 804.3 → 373.7 in run 2. The uniform
    arm, which has four groups to spread across, gains 19% between 1 and 16 in
    flight and then collapses to roughly half at 64. Sixteen-fold concurrency
    buys under 20%
  - **An unasked-for finding: at one in flight the hot group BEATS the spread
    cluster.** 1460.5 against 1162.1 in run 1, 1216.2 against 930.0 in run 2 —
    both directions confirmed on rows that are `stable` in at least one run. With
    no concurrency to exploit, concentrating on one group wins; the ordering
    inverts by 64 in flight (513.1 against 727.4), and that inversion is the
    per-group lock becoming visible. The gap between the two arms at a given
    concurrency is what per-group serialization costs, measured rather than
    asserted
  - **The ratios are an order of magnitude more stable than the rate, and that
    is why these verdicts stand on a machine where no throughput row may be
    quoted.** Run 2's per-repetition spreads, five repetitions per row:

    | row | throughput spread | entries/AE spread | RPCs/commit spread |
    |---|---:|---:|---:|
    | uniform, 1 | 10.1% | **1.0%** | 2.1% |
    | uniform, 8 | 4.9% | **2.9%** | 3.7% |
    | uniform, 16 | 3.1% | **1.0%** | 2.3% |
    | uniform, 64 | 9.5% | **2.9%** | 8.4% |
    | zipfian, 1 | 3.9% | **0.9%** | 2.7% |
    | zipfian, 8 | 4.0% | **0.5%** | 3.2% |
    | zipfian, 16 | 20.0% | **1.0%** | 4.2% |
    | zipfian, 64 | 5.6% | **4.8%** | 9.6% |

    The Zipfian 16 row is the case that makes the point: its throughput spread is
    20.0% and `repeated_result` correctly refuses to let it into a comparison
    table, while its batching factor spans 11.33 to 11.57. A ratio counts events;
    a rate divides by wall-clock time, and wall-clock time is what a loaded
    machine perturbs. The per-repetition line prints both ratios for exactly this
    reason — a reader who only saw the median run could not tell a stable ratio
    from a lucky one
  - **Every throughput number above still carries "machine was not quiet at
    start"** (load 2.04 and 4.06), so the standing rule holds: no headline here
    is quotable until it is re-taken on a genuinely quiet host. The hypothesis
    verdicts are a different kind of claim — they rest on a ratio that replicated
    to within 1% across two runs whose throughput differed by up to 34%, and on
    monotonicity, neither of which a load average reverses
  - _Requirements: 5.1, 7.6, 8.5, 8.7_

- [x] 9. The read taxonomy
  - Three separately-reported read kinds: `read_state` (quorum-confirmed
    whole-store), `GET` through the log (linearizable point read), local stale
    read
  - `read_state` reported in ops/sec **and** bytes/sec, and as a curve against
    shard size, which confirms or refutes H5
  - **The kinds cannot be aggregated, structurally.** `read_kind` is an enum
    with no value meaning "a read", `benchmark_result::_read_kind` is an
    `optional<read_kind>` that a read row always carries, and every row prints
    its kind *and* the consistency it actually provides, in the words a
    comparison has to match on (Requirement 2.2). The local row's line reads
    `NOT LINEARIZABLE (local replica, may be arbitrarily stale)`
  - **The local read prefers a replica that is not the leader.** A stale read
    served by the leader is stale only in theory. `follower_for_key` returns a
    non-leader replica when one exists and falls back to the leader when none
    does — a fallback rather than a skip, because a row that silently measured
    nothing would be worse than one that measured a leader-local read and can be
    seen in the tally to have done so
  - **Reads run against a preloaded store, and the preload is asserted.**
    `preload_keys` writes `kv_key(i * stride)` and `workload_options::_key_stride`
    makes the sampler draw the same indices, so every read hits. The repetition
    `BOOST_REQUIRE`s that every key committed: a read row over a
    nine-tenths-loaded store measures a miss path while claiming not to, and
    counting is the only way to know. The preload sits inside the repetition
    rather than being shared across the five, because a repetition is a whole
    measurement (doctrine 43)
  - **The three kinds, 1000 preloaded 128 B keys at stride 100 so all four
    shards hold 250 each, 8 in flight, five repetitions:**

    | kind | consistency | ops/sec | spread | p50 | bytes/op | MiB/sec | RPCs/read |
    |---|---|---:|---:|---:|---:|---:|---:|
    | `read_state` | linearizable (heartbeat quorum) | 2260.2 | 52.1% UNSTABLE | 3268.5 us | 36508 | 78.7 | 3.03 |
    | `GET` through the log | linearizable (ordered through the log) | 1298.2 | 4.9% stable | 5939.8 us | 128 | 0.2 | 3.69 |
    | local stale | **NOT LINEARIZABLE** | 870369.6 | 46.2% UNSTABLE | **0.8 us** | 128 | 106.2 | **0.00** |

  - **The measured price of linearizability is a factor of 7400 in latency.**
    The local read's p50 is 0.8 us against the linearizable point read's
    5939.8 us. In throughput it is 670-fold. Neither number is a surprise in
    direction and both are worth having in the register, because "reads are
    cheap if you will accept staleness" is the kind of claim that otherwise gets
    made without a number
  - **The local read's RPC count is the structural proof of its own label.**
    332 RPCs across 200,000 reads — 0.0017 per read, and those are the
    background heartbeats, not the reads. "No consensus" is not merely asserted
    in a doc comment; the counter from task 8 shows it
  - **The point read is the expensive one, and that was not expected.**
    `read_state` returns the entire 36 KB store and is **1.7x faster in ops/sec
    and lower in p50** than a `GET` that returns one 128-byte value (2260.2
    against 1298.2; 3268.5 us against 5939.8 us). The reason is in the RPC
    column: `GET` is submitted as a proposal, so it costs a log entry and a
    replication round, while `read_state` needs only a heartbeat quorum to
    confirm leadership. At a 250-key shard, reading everything is cheaper than
    reading one thing
  - **H5 CONFIRMED, and the curve is dead linear.** Stride 1, so the preloaded
    keys are contiguous and land in one shard — the configuration H5 is about.
    All three rows `stable`:

    | keys in shard | ops/sec | spread | bytes/op | **bytes/key** | MiB/sec | p50 |
    |---:|---:|---:|---:|---:|---:|---:|
    | 100 | 2448.8 | 6.7% | 14608 | **146.08** | 34.1 | 3010.6 us |
    | 1000 | 1670.8 | 2.5% | 146008 | **146.01** | 232.7 | 4557.5 us |
    | 5000 | 597.2 | 5.8% | 730008 | **146.00** | 415.7 | 12590.2 us |

    Bytes returned per stored key is 146.08, 146.01, 146.00 across a fifty-fold
    range of shard size. `read_state` transfers the whole store and its cost
    tracks the store exactly, which is what H5 claims
  - **An independent cross-check falls out of the two cases being configured
    differently.** The taxonomy row used stride 100 and 1000 keys spread over
    four shards, so each shard held 250; it returned 36508 bytes per operation,
    or **146.03 bytes/key**. The shard-size curve used stride 1 and one shard.
    Two cases with different key layouts, different shard counts and different
    budgets agree on bytes-per-key to within 0.05%. That is not a result about
    Raft; it is evidence that the bytes accounting measures what it says
  - **Requirement 2.4's "both ops/sec and bytes/sec" is not bookkeeping, and
    this curve is why.** Read in ops/sec the sweep is 2448.8 → 597.2, a
    four-fold *fall* that looks like a regression. Read in bytes/sec it is
    34.1 → 415.7 MiB/sec, a twelve-fold *rise*. Both are true: the machine does
    steadily more total work per second as the shard grows, and steadily less of
    it per operation. A row reported in one unit only would mislead in whichever
    direction that unit ran
  - **The local read's budget is per-kind, and the first attempt got it wrong.**
    At 400 operations the local row completed in about a millisecond, so
    starting and joining eight threads was most of what was timed; it came back
    at 44.4% spread, which said nothing about the system. `read_budget` now
    gives it 200,000 — about a quarter-second of work — and that also makes it
    **the first row in this suite with enough samples for a p99 at all**
    (Requirement 5.3's 1,000-sample threshold): p50 0.8 us, p95 4.3 us,
    p99 9.7 us. Its *rate* is still UNSTABLE at 46.2% and must not be quoted;
    its latency distribution is the best-supported one here
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

- [x] 11. Cost attribution
  - **Tier A exists now.** `fabric_transport` is a fixture with the same shape
    as the three HTTP ones — `transport_bundle`, `client_type` / `server_type`,
    `name()`, `tier()`, `capabilities()`, `client(id)` / `server(id)`,
    `drain()` / `shutdown()` — over `tests/multi_raft_test_fabric.hpp`, so
    `kv_cluster` instantiates on it with no special case and any row in the
    suite can be taken at either tier. Its `transport_bundle` reports
    `none (no wire)` as its serializer rather than leaving the column blank,
    which would read as "not recorded"
  - **Every row now carries its tier** (`deployment_tier` on `benchmark_result`,
    read off the fixture) and prints Requirement 3.3's warning at the point of
    the number rather than once in a preamble a quoted row leaves behind (3.1,
    3.3). `drain()` is new API on every fixture and is load-bearing: `stop()`
    makes a transport refuse *new* requests and does not wait for handlers
    already running, and a fabric worker holds a handler that captured a host
    `kv_cluster::shutdown()` is about to free
  - **Routing** is `routing_mode` — `by_key` (the production path every other
    row uses), `attributed_key` and `attributed_group`. Three values rather than
    two because the two attributed arms must differ *only* in which
    `submit_command` overload runs: `by_key`'s leader discovery resolves the key
    on every host, which costs more shard-map lookups than the one inside
    `submit_command`, and differencing against it would have reported routing
    several times too large
  - **Tick-cadence sweep** at 1 / 2 / 5 / 20 ms, reported as every percentile
    against the cadence plus RPCs per commit and the per-stream inter-round
    interval (8.6)
  - **Decomposition published with a stated residual**, built entirely from tier
    and addressing deltas with no counter added to any production path (8.2).
    The residual is `(routing at Tier A) − (routing at Tier B)`, and it is named
    as that rather than as slack
  - **What it found.** Three runs, five repetitions per row.
    - **H6 refuted, in the opposite direction**: every percentile *falls* as the
      clock slows (p50 22.1 → 9.8 ms over 1 → 20 ms) and throughput rises
      2.3–3.1x. No floor appears at any cadence
    - **H2 confirmed on the tick axis** after task 8 refuted it on the
      concurrency axis: RPCs per committed entry 5.41 → 2.23 and 6.05 → 2.24,
      monotone, agreeing to 2.6%, toward a floor of ~2
    - **Task 8's "the round is tick-driven" is wrong.** The per-stream
      inter-round interval is 2.17 / 2.02 / 2.15 / 2.26 ms at tick periods of
      1 / 2 / 5 / 20 ms — flat to 12% across a twentyfold change
    - **Routing is not where the time goes.** At one operation in flight the two
      overloads differ by 72.9 and 37.9 µs against bands of 262.5 and 299.4 µs
      (21.9 ± 265.5 and 39.3 ± 212.8 in two earlier runs). Not resolved in any
      run, and the bound is the result: under ~300 µs, at most 39% of one
      committed operation. Reported as a bound rather than as the point
      estimate, which the next run contradicts
    - **Transport and wire serialization is 57–58% of one operation** at 16 in
      flight — 9395 µs of 16376 and 9786 of 16764 — and the consensus core is
      41–42%. Those two are resolved against their inputs' spread; routing and
      the residual are not
  - Requirement 8.8's citations are printed with the decomposition, each with
    the way its conditions differ from this suite's
  - _Requirements: 3.1, 3.3, 8.1–8.3, 8.6, 8.8_

- [x] 12. Report artifacts
  - **`tests/multi_raft_benchmark_rows.hpp` came first, and is the point.**
    Requirement 12 wants a CI-registered regression subset and Requirement 11.6
    wants a report generator that runs a subset of the matrix. Those are two
    programs with different budgets and different failure behaviour; they must
    not be two *measurements*. The cluster shape, the budgets, the warm-up rule,
    the structural checks and the repetition loop now live in one header, and
    what differs between the consumers is a `row_observer` — three callbacks
    (`_message`, `_require`, `_check`) that the suite backs with Boost.Test and
    the report binary with `std::cout` and a thrown exception. Without it the
    artifact would describe rows CI never ran
  - `write_row_spec` / `read_row_spec` replace a seven-parameter list of which
    two were `std::size_t`. A designated initialiser cannot be transposed, and
    adding an axis no longer touches every call site
  - **`tests/multi_raft_performance_report.cpp`** — its own `main`, a catalog of
    35 rows as data, `--list`, and `--axis` / `--scenario` / `--tier`
    case-insensitive substring selectors (11.6). A row whose precondition fails
    is **abandoned and recorded as not measured**, and the matrix continues; a
    run in which every row was abandoned exits non-zero. `--budget-scale` and
    `--out-dir` exist so the CTest entry is the same program on the same
    catalog, not a second one
  - The catalog carries the two rows a CI-registered test cannot: Requirement
    4.3's **512 in flight**, and the open-loop arm, whose rate has to be chosen
    for the machine
  - **`tests/multi_raft_report_artifacts.hpp`** writes the timestamped CSV and
    JSON pair to `test_results/` (11.1), self-describing per 11.5: tier and what
    that tier forbids, both serializers, cluster shape, load mode **and its
    controlling parameter**, routing, tick cadence, spread, verdict, per-cause
    tally, replication counters, the machine, and **every repetition**, not only
    the median. Two rules the writers hold: an absent value is `null` in JSON
    and an **empty cell** in CSV, never `0` — a p99 the row lacked samples for is
    not zero, and `headline_ops_per_second` must stay absent below
    `k_required_repetitions` on disk as it is in memory; and the verdict travels
    with the number, because a CSV read by a script that never saw the printed
    output is exactly where Requirement 6.3's gate gets lost
  - **Open loop (4.1, 4.2)** offers a fixed rate on a schedule computed before
    the window and measures each operation **from its intended start time**.
    `_mean_schedule_lag` is on every row: a bounded worker pool cannot serve a
    true open loop, and when it falls behind the offered rate silently becomes
    the closed-loop rate — the lag is what makes that visible instead
  - **The first run of it found a defect in the statistics.** In open loop the
    rate is an *input*, so the throughput spread is ~0.0% whatever the system
    did, and `verdict()` would have stamped `stable` on every open-loop row ever
    taken — including one whose repetitions ranged from a 682 us p50 to an
    84,704 us p50. `governing_spread()` now switches on the load mode, the
    verdict is computed from it, and both spreads are printed and written with
    the governing one named
  - **An unasked-for corroboration of task 11.** At 300 ops/sec offered, entries
    per AppendEntries is **1.00** and RPCs per committed entry is **10.1–11.0**,
    against 4.7 and ~4.0 at sixteen in flight closed-loop. Both follow from
    task 11's mechanism: the batch tracks proposals outstanding per group (at
    300/sec with a 0.7 ms latency there is a fifth of one), and the rounds per
    commit rise because the tick keeps firing regardless
  - `tests/CMakeLists.txt`'s five conditional wiring blocks are now
    `kythira_wire_multi_raft_bench(<target>)`, called for both binaries. A
    target built with a different set of `KYTHIRA_BENCH_HAS_*` defines than the
    other would silently measure a smaller matrix
  - The CTest entry is `--axis smoke`, one Tier A row of 40 operations, 15
    seconds. It checks *this program* — argument parsing, catalog, observer,
    artifact writers — and is not a measurement
  - **Not in this task**: `doc/multi_raft_performance_comparison.md` (11.2–11.4)
    needs Requirement 9's comparison register in full, which is the external
    comparison work
  - _Requirements: 4.1–4.3, 11.1, 11.5, 11.6_

- [x] 13. Portability
  - Build and run under folly, boost and stdexec; `std::move(f).get()` everywhere;
    `.detach()` on any fire-and-forget continuation
  - **The run half is done now (13.1).** `multi_raft_performance_report` was
    built and **run** under all three backends on one machine back to back, and
    each run reports its own backend in the machine block (13.5): folly 2168.2,
    boost 1568.2 and stdexec 2034.1 ops/sec on the Tier A smoke row. Those three
    numbers are **not** a backend comparison — one row of forty operations at
    12–20% spread on a machine that was not quiet — and are recorded only as
    evidence that the matrix runs, which is what 13.1 asks for
  - **The graceful shrink is checked and it needed a fix (13.3, 13.4).** The
    report binary's catalog omitted an unavailable row *silently*, which is
    exactly the failure 13.4 names: a shorter, entirely plausible table. Every
    `#else` in `build_catalog` now records the row and the reason, and the list
    is printed on every invocation — including when it is empty, saying so,
    because "nothing was dropped" and "this program does not track drops" look
    identical otherwise. It is in the JSON artifact too, as `dropped_rows`, since
    a consumer reading the file has no other way to tell a build that measured
    everything from one that measured what it could. Verified on the checked-in
    default configuration, which has no ion-c: the Ion row is named with
    `KYTHIRA_BENCH_HAS_ION undefined (requires CONFIG_ION_SERIALIZER and the
    ion-c vcpkg feature)`
  - The absent-Beast case drops nine whole axes rather than one row — Beast is
    the transport every non-transport axis holds fixed — and is named per axis,
    because a run that printed one Tier A row and nothing else would read as a
    complete matrix
  - **The build half was done earlier, and it took a production fix.**
    `include/raft/http_transport_impl.hpp`'s `make_future_with_exception` /
    `make_future_with_value` constructed the future directly from an
    `exception_ptr` or a value, which only folly's `Future` and the simulator's
    accept — `boost_backend::Future` is built from a `boost::future` and
    `stdexec_backend::Future` from a sender, so neither compiled. They now go
    through `future_factory_default::makeExceptionalFuture` /
    `makeFuture`, which all three backends expose. Nothing had instantiated
    `cpp_httplib_client` under a non-folly backend before this suite
  - _Requirements: 13.1–13.5, 17.2_

- [x] 11a. The 1024→4096 B cliff, explained
  - Left open by task 6 and carried in every handoff since. Closed with the
    instrument task 11 built: `report_value_size_sweep` reads the axis across
    rows in round interval, entry-bearing rounds per commit, batch size and
    **write amplification** — entry-sends per commit against the
    once-per-follower floor a commit cannot avoid
  - Two runs of the five-point sweep (2048 B added to bracket the step), five
    repetitions per row. `ent/AE` and `round interval` replicate to 2%; the
    4096 B row is the known-unstable one and its amplification varies 2x
    run-to-run, so the *direction* is what is claimed and not the magnitude:

    | value | ops/sec | AE/commit | round interval | ent/AE | amplification |
    |---:|---:|---:|---:|---:|---:|
    | 16 B | 1289 / 1324 | 3.75 / 3.62 | 1.64 / 1.66 ms | 4.89 / 4.84 | **9.2x / 8.8x** |
    | 128 B | 1203 / 1204 | 3.89 / 3.79 | 1.69 / 1.74 ms | 4.73 / 4.68 | **9.2x / 8.9x** |
    | 1024 B | 727 / 760 | 4.87 / 4.58 | 2.22 / 2.26 ms | 4.41 / 4.47 | **10.7x / 10.2x** |
    | 2048 B | 392 / 414 | 6.97 / 6.38 | 2.84 / 2.93 ms | 4.30 / 4.40 | **15.0x / 14.0x** |
    | 4096 B | 61 / 52 | 30.96 / 35.69 | 4.03 / 4.09 ms | 4.03 / 4.28 | **62x / 76x** |

  - **Task 11's hypothesis is REFUTED.** It predicted the cliff would appear as
    a longer round *trip* at a flat round *count* — response-driven pacing
    inflating the interval and nothing else. The interval does rise, but only
    2.5x over a 256-fold value range and smoothly; entry-bearing rounds per
    commit rise **8.3x and 9.9x**. The round count is the larger factor, not the
    smaller one
  - **The batch size is invariant to value size**: 4.03–4.89 entries per
    AppendEntries across a 256-fold range and four runs. Read with task 11 (flat
    across a twentyfold tick change) and task 8 (tracks in-flight per group),
    the batch is set by proposals outstanding per group and by **nothing else**
  - **Unasked-for, and the more useful half: write amplification is ~9x per
    follower even at 16 B.** Every committed entry crosses the wire nine times
    to each follower before it commits, against a floor of once. That is H2's
    redundancy claim — refuted by task 8 on RPC *counts* — surfacing in entry
    *sends*, where the same window is retransmitted round after round
  - The amplification is flat to 1 KiB (8.8–10.7), rises to 14–15 at 2 KiB, and
    explodes to 62–76 at 4 KiB. One doubling of value size costs a 4.4–5.4x rise
    in retransmission, which no other doubling on the axis does
  - **Not measured here, and answered by task 11b below**: why that doubling. The
    obvious instrument is a byte counter on the send path, and Requirement 8.2
    keeps one out of production code — so 11b asks the question by changing the
    *encoded* size while holding the value size fixed instead
  - _Requirements: 1.4, 8.5_

- [x] 11b. The knee is a function of the ENCODED size
  - The question 11a left open, asked without the counter 8.2 forbids: run the
    same value sizes under JSON and under CBOR. `json_rpc_serializer`
    base64-expands a byte array by 4/3 and quotes it; `cbor_serializer` writes
    byte strings natively, so a 4 KiB value is ~5.5 KiB on the JSON wire and
    ~4 KiB on the CBOR wire before framing
  - **The prediction was stated in the case's own doc comment before the run**:
    if the knee is a threshold in encoded bytes, CBOR should push it out; if it
    is in the value size — a per-entry cost, a copy, an allocator — both arms
    should knee in the same place
  - Two runs of five repetitions, 1 KiB / 2 KiB / 4 KiB:

    | value | JSON ampl | CBOR ampl | CBOR/JSON | JSON ops/sec | CBOR ops/sec | CBOR/JSON |
    |---:|---:|---:|---:|---:|---:|---:|
    | 1 KiB | 11.07 / 11.07 | 9.63 / 9.39 | **0.87 / 0.85** | 655.9 / 631.9 | 906.1 / 938.4 | 1.38 / 1.49 |
    | 2 KiB | 16.20 / 15.92 | 11.27 / 11.32 | **0.70 / 0.71** | 345.7 / 355.8 | 628.4 / 636.6 | 1.82 / 1.79 |
    | 4 KiB | 60.73 / 35.87 | 18.18 / 20.63 | **0.30 / 0.58** | 57.6 / 101.1 | 304.9 / 256.2 | 5.29 / 2.53 |

  - **CBOR's amplification curve has no knee**: 9.4–9.6 → 11.3 → 18.2–20.6, a
    smooth 2.0–2.2x over a fourfold value range, where JSON's rises 3.2–5.5x with
    the last doubling alone accounting for 2.3–3.8x. The CBOR/JSON ratio falls
    monotonically in both runs and replicates to **1.5%** at 1 KiB and 2 KiB
  - The 4 KiB row is unstable in both arms — JSON's amplification varies 1.7x
    between runs — so its **direction is claimed and its magnitude is not**
  - **H3 is confirmed with a curve, and at the top of Requirement 1.4's own
    range the effect is not small.** At 128 B the serializer axis found the
    encodings close, which is what H3 predicted; at 4 KiB CBOR is **2.5x to 5.3x
    faster**, and the reason is not encode/decode CPU — it is that it retransmits
    less
  - **The mechanism looks like a feedback, not a threshold.** CBOR's round
    interval is only 1.3–1.4x shorter at 4 KiB while its amplification is
    1.7–3.3x lower: a small change in the driving term producing a large change
    in the accumulated one. **The loop is not isolated** — this comparison cannot
    separate "encoded size" from anything else that differs between the two
    serializers, and the case says so in as many words
  - _Requirements: 1.4, 8.4, 8.5, 17.2_

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

- [x] 16. Shape 1 — one AWS instance, Tier B
  - Provision one non-burstable instance, build or fetch the benchmark binary,
    run the same scenarios with the same configuration as the local rows, pull
    the artifacts back, tear down
  - Publish the local-vs-cloud delta as its own finding: that number *is* the
    hardware confound
  - **Run, on real hardware, twice, and torn down clean both times.** One
    `c5.2xlarge` in `us-east-1a` — Xeon Platinum 8124M at 3.00 GHz, 8 vCPU,
    15,502 MiB, kernel 7.0.0-1011-aws, gp3 root at 3000 IOPS, shared tenancy.
    Two repetitions of a five-case sweep on the one instance, 6.3 minutes each.
    The post-run audit came back clean on all six resource classes both times,
    and the instance's total billed life was about sixteen minutes — **$0.09**
    against the pre-registered $0.11–$0.13 expectation in
    `doc/multi_raft_performance_cloud_cost_estimate.md`
  - **The binary is SHIPPED, not rebuilt, and that is the experiment rather
    than a shortcut.** Requirement 18.7 wants the local-vs-cloud delta to *be*
    the hardware confound. Rebuilding on the instance would fold a different
    compiler, a different vcpkg resolution and a different feature set into that
    delta with no way afterwards to say which of them moved the number.
    `multi_raft_http_benchmark_test` links only libstdc++, libm, libgcc_s and
    libc, so the same Release ELF that produced the local table runs unmodified
    on an Ubuntu 24.04 AMI and the delta is hardware and nothing else. It is
    also the only version that fits Requirement 18.11's ceiling: a from-source
    build of this dependency set is hours
  - **The single largest result is that the 4 KiB cliff is a property of the
    development machine and not of this implementation.** Two sessions of work
    (tasks 11a and 11b) characterised a collapse to a twentieth of the 16 B
    throughput at 4 KiB, with entry-bearing rounds per commit rising 8.3–9.9x.
    On eight modern cores the same sweep, same binary, same configuration:

    | value | local ops/sec | cloud r1 | r2 | local ratio | cloud ratio | local AE/commit | cloud AE/commit |
    |---:|---:|---:|---:|---:|---:|---:|---:|
    | 16 B | 1289 / 1324 | 3636.3 | 3643.0 | 1.00x | 1.00x | 3.75 / 3.62 | 2.77 / 2.78 |
    | 128 B | 1203 / 1204 | 3477.4 | 3515.8 | 0.93x | 0.96x | 3.89 / 3.79 | 2.82 / 2.81 |
    | 1024 B | 727 / 760 | 2801.8 | 2782.8 | 0.56x | 0.77x | 4.87 / 4.58 | 2.99 / 2.96 |
    | 2048 B | 392 / 414 | 2124.7 | 2110.1 | 0.30x | 0.58x | 6.97 / 6.38 | 3.26 / 3.28 |
    | 4096 B | 61 / 52 | 1280.5 | 1218.5 | **0.047x** | **0.35x** | 30.96 / 35.69 | 3.73 / 3.93 |

    A twentyfold collapse becomes a smooth threefold decline. Both measurements
    are correct; only one of them is about the code. **This is exactly the
    confound Requirement 18.7 exists to remove, and it removed a conclusion**
  - **What survives the machine change, and therefore is structural.** Entries
    per AppendEntries is 4.44–4.83 across the whole 256-fold value range on the
    cloud instance against 4.03–4.89 locally — the batch is invariant to payload
    on both machines at essentially the same value. And write amplification does
    not vanish: every committed entry still crosses the wire **6.6 to 8.7 times
    per commit** on the fast machine against a floor of two. The blow-up to
    62–76x is local; the baseline redundancy of ~6.6x is not
  - **Every row came back `stable`, which has never happened before in this
    suite.** Spreads of 0.8–3.9% across five repetitions, on both runs, on every
    row of five cases. Every throughput row ever taken on the development
    machine carries `UNSTABLE` or `machine was not quiet at start`. Doctrine 90
    said a ratio is measurable where a rate is not; this says where a rate
    becomes measurable
  - **H6 replicates in direction and not in magnitude**, which is the kind of
    result only a second machine can produce. A slower tick still makes
    everything faster and no percentile shows a floor at any cadence — the p50
    at a 20 ms tick is 3.75 ms, under a fifth of the tick period — but the
    effect is 1.40x on eight cores against 2.3x on four. RPCs per committed
    entry converges to 2.10 (cloud) and 2.23 (local), both approaching the floor
    of two: **the asymptote is structural and the approach to it is hardware**
  - **The strongest new evidence for response-driven pacing.** The per-stream
    inter-round interval is 0.75–0.91 ms on the cloud instance against
    1.89–2.26 ms locally, in each case flat to about 20% across a twentyfold
    tick sweep. It moved when the machine's RPC round trip moved and did not
    move when the clock moved. Still a hypothesis — nothing has instrumented the
    leader's send path — but with two points of support instead of one
  - **Task 11b's conclusion holds on the machine it was taken on and does not
    generalise, and this task says so rather than leaving it.** The CBOR/JSON
    amplification ratio is 0.30–0.87 locally and **0.92–0.99** on the cloud
    instance. The case prints its own decision rule beside the table — *at or
    near 1.00x says the knee is NOT about encoded size* — and on eight cores
    the rule says "not encoded size". Both readings are right: there is no knee
    on the cloud instance for the encoding to remove. Encoded size governs the
    *severity* of a knee that is itself a CPU-starvation effect
  - **The routing bound tightened sevenfold and is still a bound.** Under
    38.2–39.2 µs at one operation in flight, at most 11.9–13.3% of one committed
    operation, against 212.8–299.4 µs locally. Six runs across two machines now
    put the two `submit_command` overloads within a few microseconds of each
    other against bands an order of magnitude wider. Doctrine 107 stands
  - **Do NOT read the decomposition's share shift as "transport got cheap".**
    Transport plus wire serialization falls from 57–58% of an operation locally
    to 8.9–9.9% on the cloud instance, but at 16 in flight a p50 is dominated by
    queueing behind fifteen other operations, so the "consensus core" residual
    is largely `concurrency ÷ throughput`. What is real is that transport fell
    21x between the machines while the total fell 3.6x
  - **`scripts/perf-cloud/run-aws-shape-1.sh` is the deliverable, and it is a
    script rather than workflow steps on purpose.** A provisioning sequence that
    only ever runs inside a workflow is one whose failure modes are only ever
    discovered inside a workflow, which is a bad property for the step that
    spends money. `perf-cloud.yml` now builds the binary on a GitHub runner and
    calls the same script an operator calls by hand
  - **Four safety nets, not three, and the fourth closes the gap task 17 stated
    as open.** Task 17's three ceilings all live on the *controlling* machine,
    so none of them fires if that machine dies. The instance now boots with a
    cloud-init `shutdown -h +N` and
    `instance-initiated-shutdown-behavior=terminate` — terminate rather than
    stop, because a stopped instance still bills its volume. Sizing it was a
    live hazard caught before it cost anything: `ceiling + 25` covers one
    repetition, not every repetition and certainly not an on-instance build, and
    a dead-man switch that fires mid-build destroys the work and looks like a
    network failure
  - **The perf-cloud IAM bundle was insufficient and the gap was found by trying
    to use it, not by reading it.** `policies/perf-cloud.json` had
    `RunInstances` and the describes but no `CreateKeyPair`, `CreateSecurityGroup`
    or `AuthorizeSecurityGroupIngress` — that is, no way for the controlling
    machine to reach the instance it just launched at all, and no way to get an
    artifact back. Added as `PerfCloudEphemeralAccessPath`, plus
    `GetConsoleOutput` so a boot that never answers can be diagnosed rather than
    guessed at. Both the CI OIDC role and the developer identity were
    re-provisioned from the same bundle file
  - **The provenance script's variable names are a contract, and the first live
    run got three of them wrong.** `capture-provenance.sh` reads
    `KYTHIRA_PERF_STATED_TENANCY`, `_STORAGE_CLASS` and `_STORAGE_IOPS`; the
    caller passed `KYTHIRA_PERF_TENANCY`, `_STATED_STORAGE` and `_STATED_IOPS`.
    Three fields came back `null`. **That is the design working** — Requirement
    18.4's "null, never omitted and never guessed" is what made the mismatch
    visible in the artifact instead of invisible in a plausible default — and it
    is why the call site now lists them against the script's own variable list
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

- [x] 18. Shape 1 on Graviton
  - The arm64 leg already exists in CI; a modern arm64 instance is its own point
    in the design space, not a portability check
  - **Run on a `c7g.2xlarge` in `us-east-1d`** — Neoverse-V1, 8 vCPU, 15,665
    MiB, kernel 7.0.0-1011-aws, aarch64, gp3 root at 3000 IOPS, "Up to 15
    Gigabit", shared tenancy. Two repetitions of the same five-case filter the
    x64 row used, both exit 0, torn down with a clean six-class audit. About
    seventy minutes of instance life at $0.29/hr — **$0.34** — of which
    fifty-five minutes was the build
  - **The binary was built ON the instance, and that is a deliberate departure
    from task 16's rule.** Shipping a prebuilt binary is preferred precisely so
    the local-vs-cloud delta is hardware alone; there is no arm64 build host on
    the development side of this project, so the Graviton row is either built
    there or not measured. `run.json` records `binary_origin` so a reader can
    tell the two modes apart, and the recipe is `ci.yml`'s step for step and
    pin for pin — g++-13, Release, `configs/ci_full_defconfig`, the same pinned
    vcpkg commit
  - **Graviton is faster, and the margin GROWS with payload size** — which a
    clock-speed difference would not do:

    | value | c5.2xlarge r1 / r2 | c7g.2xlarge r1 / r2 | c7g/c5 |
    |---:|---:|---:|---:|
    | 16 B | 3636.3 / 3643.0 | 3770.0 / 3837.8 | 1.04 / 1.05x |
    | 128 B | 3477.4 / 3515.8 | 3720.4 / 3770.2 | 1.07 / 1.07x |
    | 1024 B | 2801.8 / 2782.8 | 3119.9 / 3120.1 | 1.11 / 1.12x |
    | 2048 B | 2124.7 / 2110.1 | 2560.3 / 2511.7 | 1.21 / 1.19x |
    | 4096 B | 1280.5 / 1218.5 | 1631.0 / 1577.3 | 1.27 / 1.29x |

    A flat 1.05x would be a faster core; 1.04x rising monotonically to 1.29x is
    a *per-byte* advantage, which points at the encode-and-copy path rather than
    at instruction throughput. Not isolated here, and stated as a direction
  - **The 4 KiB decline is shallower again**: 0.43 / 0.41x of the 16 B rate
    against 0.35 / 0.33x on x64 and **0.047 / 0.039x** on the development
    machine. Three machines now, and the cliff tasks 11a and 11b characterised
    is visible on exactly one of them
  - **The structural ratios are architecture-independent, which is the finding
    that matters most.** Entries per AppendEntries is 4.45–4.86 on Graviton
    against 4.44–4.83 on x64 and 4.03–4.89 locally — three machines, two
    instruction sets, one value. RPCs per committed entry converges to
    **2.09–2.10** on both cloud machines at a 20 ms tick (2.23 locally), against
    a floor of two. Amplification at 16 B is 6.74–6.81 against 6.68–6.72
  - **H6 replicates a third time**, at 1.34x against x64's 1.40x and the
    development machine's 2.3x, with every percentile falling as the clock slows
    and no floor at any cadence. The per-stream round interval is 0.70–0.88 ms
    against 0.75–0.91 ms on x64 and 1.89–2.26 ms locally — it tracks the
    machine, never the clock, on every machine tried
  - **The routing bound is now the tightest it has ever been: ≤22.1 µs, at most
    7.8% of one committed operation** (≤36.1 µs on the first run). Across three
    machines and eight runs the two `submit_command` overloads have never
    separated. Doctrine 107 holds and the bound keeps improving
  - **CBOR/JSON amplification is 0.87–0.99**, and the case's own decision rule
    reads "not encoded size" here as it does on x64. Task 11b's conclusion is
    now known to be specific to the development machine on two independent cloud
    machines rather than one
  - **The serializer axis is within 3.3% at 128 B** — JSON 3683.0, CBOR 3803.0,
    protobuf 3752.4, every row `stable` at 0.7–1.5% spread
  - **Getting here found a bug that made this task impossible.**
    `capture-provenance.sh` could never run on aarch64: `grep '^model name'
    /proc/cpuinfo` matches nothing there, and under `set -o pipefail` that grep's
    exit 1 killed the script even though the `sed` at the end of the pipe had
    succeeded. The lscpu fallback on the next line was unreachable for the same
    reason. It cost a fifty-five-minute build to find, because provenance ran
    after the build; it now runs **before** it, so a failure of this kind costs
    seconds
  - _Requirements: 18.6, 18.12_

- [ ] 19. Tier D on a cloud instance
  - `file_persistence` plus `tick_batch_controller` against a real volume;
    report fsyncs/sec and entries per fsync, and the volume class and IOPS
  - This is the first configuration in the whole spec that could carry a
    like-for-like comparison against a published *durable* number
  - **NOT DELIVERED, and the reason is recorded here rather than left for the
    reader to infer from an absent row (Requirement 3.7).** Two things are
    missing and only one of them was known. The known one: Tier D is defined by
    Requirement 3.1 as *Tier C plus* file persistence, and Tier C needs a host
    process per node — the `cmd/` binary that is Appendix B's third open
    question and task 20's first line. The harness's
    `persistence_engine_type` is also a fixed
    `memory_persistence_engine` in `tests/multi_raft_transport_harness.hpp`,
    which is a smaller change but a real one
  - **The unknown one, found by reading `multi_raft.hpp` for this task: the
    per-group batching fallback that `tick_batch_controller`'s doc comment
    described DOES NOT EXIST.** That comment said `tick()` "falls back to
    per-group batching through `batched_persistence_engine`", collapsing each
    group's appends into one barrier per ready group. Nothing in `include/` or
    `src/` calls `begin_batch()` at all except `group_scoped_persistence`'s
    conditional forwarder, and nothing calls *that* — the only driver is the
    caller-supplied `tick_batch_controller`. **Without a controller there is no
    batching whatsoever** — and for `file_persistence_engine` that is not
    merely slower, it is **not durable**. `append_log_entry` outside a batch
    calls `append_to_log_file`, which flushes the `ofstream` and stops;
    `sync_log_and_directory()`, the only fsync on the log path, is reached from
    `commit_batch()` and from nowhere else. A multi-group host with a
    file-backed log and no controller therefore writes to the page cache and
    issues no barrier at all — which is exactly the `buffered` mode Requirement
    3.5 insists be labelled "not durable" wherever it appears. The comment is
    corrected in this commit to say what the code does; building the fallback
    is a production behaviour change and is deliberately not made from a
    performance spec
  - **The harness half is built and measured, and what it measured is that
    `tick_batch_controller` CANNOT make a multi-group host durable.**
    `durability_mode` (three values), `benchmark_persistence_engine` (a handle
    over either engine, with the counters outside it), a per-host controller
    fanning out across that host's stores, and
    `write_throughput_by_durability`. Three runs of five repetitions each on
    this machine, plus an independent run through the report generator:

    | mode | ops/sec | p50 | fsync/sec/host | entries/fsync | **barriered** | barriers | empty batches | entries |
    |---|---:|---:|---:|---:|---:|---:|---:|---:|
    | memory | 1137–1185 | 13.0–13.6 ms | 0.00 | 0.00 | 0.0% | 0 | 0 | 1200 |
    | file/buffered (NOT DURABLE) | 794–839 | 18.4–19.1 ms | 0.00 | 0.00 | 0.0% | 0 | 0 | 1200 |
    | file/barrier | 688–714 | 21.5–22.4 ms | 76.2 / 95.1 | 1.72 / 1.77 | **19.9% / 24.5%** | 139 / 166 | 802 / 1078 | 1200 |

  - **`barriered` is the column that decides whether a row is durable at all,
    and it is 20–25%.** Three quarters of the appended log reached the page
    cache and no barrier ever reached it. The reason is structural rather than
    a tuning problem: `multi_raft`'s tick opens the batch, runs the persist
    phase and commits it inside one `tick()` call, but **a proposal appends on
    the caller's thread and a follower appends on its RPC handler's thread**,
    and neither is inside that window. The hook is in the wrong place for the
    appends it is meant to cover
  - **So Tier D is not reachable by supplying a controller**, which is what
    this task's own plan assumed. Reaching it needs the barrier moved to where
    appends happen — which is a production design decision about `node` and
    `multi_raft`, not a benchmark change, and this spec is not entitled to make
    it. That is the single most useful thing this task produced and it is a
    negative result
  - **It was nearly missed, and the near-miss is the methodological point.**
    The first version of this row reported `entries/fsync` as
    `total entries ÷ barriers` — 8.45 — which quietly averaged over entries no
    fsync had touched. Adding the covered-entry counter dropped it to 1.72–1.77
    and put a 20–25% beside it. **Print the denominator's own denominator**: a
    ratio whose numerator is "all entries" and whose divisor is "barriers" is
    only meaningful if every entry was barriered, and nothing had checked that
  - **And the counter was itself dropped once, silently, by an aggregate.**
    `operator-(durability_snapshot, durability_snapshot)` is a designated
    initialiser; omitting the new member value-initialised it, so the field
    arrived at the report as a plausible zero. The row then said a barrier had
    covered **0%** of entries while also reporting 122 barriers — two numbers
    that cannot both be true, which is the only reason it was caught. There is
    a `static_assert` on the struct's size beside that function now
  - **`file/buffered` issues ZERO barriers against 1200 appended entries**, on
    every run, and the case asserts it — so a future change giving `tick()` a
    batching fallback fails here rather than silently relabelling an arm
  - **The cost split still holds and is the other useful number.** The
    JSON-line append alone — buffered against memory, no fsync in either —
    costs **~33% of throughput**; the partial barrier on top costs a further
    ~13%. H4 predicted the encode cost would be measurable and it now is; the
    memory growth it also names still is not
  - **The follow-on now has a spec: `.kiro/specs/durable-append-barrier/`.**
    It moves the barrier to the advertise boundary — before a leader counts an
    entry toward `match_index`, before a follower returns success — and makes
    that affordable with group commit. Ten tasks, the first of which must
    produce a *failing* coverage test. **Tier D needs that spec and this one**
  - **What is still missing: the TIER, and now also a barrier that covers
    anything.** Every row above is Tier B with three hosts in one process, and
    the cloud half — `kv_cluster_options::_data_dir` and the
    `KYTHIRA_BENCH_DATA_DIR` override, both wired and never exercised — has not
    run, so the volume class and IOPS Requirement 3.4 asks for are unreported
  - **HANDOVER, August 31 2026 — the barrier half is delivered and this task's
    blocker is now one item rather than two.**
    `.kiro/specs/durable-append-barrier/` is complete: the barrier is taken at
    the boundary where `node` advertises an append, group commit coalesces
    concurrent appends behind one `fsync`, and `tick_batch_controller` is
    removed. Re-running this axis unchanged on the development machine:

    | mode | ops/sec | p50 | fsync/sec/host | entries/fsync | **barriered** | barriers | empty | entries |
    |---|---:|---:|---:|---:|---:|---:|---:|---:|
    | memory | 826.2 | 18.6 ms | 0.00 | 0.00 | 0.0% | 0 | 0 | 1200 |
    | file/buffered (NOT DURABLE) | 575.8 | 26.9 ms | 0.00 | 0.00 | 0.0% | 0 | 1044 | 1200 |
    | file/barrier | 164.1 | 95.6 ms | 128.99 | 1.27 | **100.0%** | 943 | 3 | 1202 |

    **100% barriered**, against 19.9–24.5%. The case now *asserts* it rather
    than printing it (Requirement 2.2 of that spec), and the cost is what the
    design predicted in advance: a correct implementation is slower than the
    one that was not doing the work. Read the barriered row against `buffered`
    rather than against `memory` — that difference, 576 → 164 on this machine,
    is the fsync and nothing else
  - **This is a handover note, not a measurement.** The row above is Tier B on
    a laptop SSD; it is here to record that the blocker moved, not to be
    quoted. What this task still needs is unchanged and is now a single item:
    **Tier C, i.e. the host binary**, which is
    `.kiro/specs/multi-raft-host-binary/`. The volume class and IOPS
    Requirement 3.4 asks for still require a cloud row against a provisioned
    volume, and `entries/fsync` of 1.27 on four cores says nothing about what
    group commit yields on a machine that can actually run appends
    concurrently
  - _Requirements: 3.4, 3.5, 18.7_

- [x] 20. Shape 2 — Tier E, one host per instance
  - A host binary in `cmd/`, N instances, service discovery between them
  - Measure and report inter-node RTT and bandwidth **before** the measured
    window, plus the placement
  - **NOT DELIVERED (Requirement 3.7).** Its first line is the blocker for
    tasks 19 and 20 both, and for Tier C as well: there is no process in this
    tree that hosts `multi_raft` and accepts client traffic. `cmd/chaos_node`
    is the nearest precedent and is not a substitute — it hosts a single-group
    node for fault-injection scenarios, not a sharded multi-Raft host with a
    client-facing entry point. Appendix B's second and third open questions
    (where the load driver runs, and whether the host belongs in `cmd/`) are
    both still open and both have to be answered before this task is even
    specifiable
  - **It now has a spec: `.kiro/specs/multi-raft-host-binary/`.** Ten tasks,
    and it settles Appendix B's questions 2 and 3 — the host belongs in `cmd/`,
    and the load driver gets its own process. The larger half of that work is
    the client-facing data path rather than the host. Its task 1 extracts the
    workload seam so a Tier C row and a Tier B row differ in tier and nothing
    else, and its task 5 proves that by running one row both ways at Tier B
    before Tier C is claimed
  - **`scripts/perf-cloud/run-aws-shape-1.sh` was written with this in mind and
    does not reach it.** It provisions one instance; Shape 2 needs N, a
    placement group, service discovery between them, and an RTT/bandwidth
    measurement *before* the measured window. The run-scoped tag, the
    unconditional teardown and the leak audit all generalise; nothing else does
  - **DELIVERED September 1, 2026** by `.kiro/specs/multi-machine-placement/`,
    which is now 10/10. `scripts/perf-cloud/run-aws-shape-2.sh` provisions N+1
    instances, opens the Raft port between them, probes every ordered pair
    before the window and tears down from an EXIT trap. Discovery is
    `aws_ec2_peer_discovery` behind the existing `peer_discovery` concept, with
    `--discovery ec2-tag` on the host; the static list stays the default for a
    measured row, because a control-plane call inside a window measures EC2.
  - **The rows: 1037.9 ops/sec one AZ and 775.0 across three, both stable over
    11 windows with zero elections**, against 1175.4 for the same binaries and
    workload co-located on one instance. Inter-node RTT 171 µs and 469 µs
    against a 51–69 µs loopback baseline.
  - **This is the row task 11 was waiting for.** Its response-driven-pacing
    prediction — that the round interval tracks the RPC round trip — was
    untestable on loopback because the round trip barely varied. Read against
    measured RTT it gives **4.39 round trips per operation at one AZ and 4.18
    at three**, two independent placements agreeing across a 3.5x difference in
    round trip. Predicted first, then measured, at two widely separated points.
  - Tier E's tier table can stop saying "containers only". **Tier D is still
    unrun** and is a volume and a run away, not a spec.
  - _Requirements: 3.1 (Tier E), 18.8_

- [x] 21. A second provider
  - GCP, following the Workload Identity Federation path already live and the
    audit pattern its own live run established
  - **Run on a `n2-standard-8` in `us-central1-a`** — Intel Xeon @ 2.80GHz, 8
    vCPU, 32,090 MiB, kernel 6.17.0-1022-gcp, pd-balanced boot disk, shared
    tenancy. Two repetitions of the same five-case filter the two AWS rows
    used, both exit 0, deleted with a clean five-class audit. About fifteen
    minutes at $0.3886/hr — **$0.10**
  - **The blocker recorded as "expired gcloud login" was wrong, and finding
    that out was most of the work.** The gcloud *user* credential is expired,
    but the application-default credential still refreshes and carries
    `cloud-platform` scope. What actually blocked it was that
    `cloudresourcemanager.googleapis.com` was not enabled, so the CI service
    account's role bindings could not even be **read**
  - **And once read, the IAM delta was ZERO.**
    `kythira-ci-real-cloud-tests@…` already held
    `roles/compute.instanceAdmin.v1`, `roles/compute.networkAdmin` and
    `roles/iam.serviceAccountUser` — everything Shape 1 needs. **No role was
    granted.** The only change to the project was enabling one read-only API,
    which is a much smaller footprint than this task's plan assumed
  - **A third machine, a second provider, a second cloud vendor's silicon —
    and the structural ratios do not move:**

    | quantity | local (4 core) | c5.2xlarge | c7g.2xlarge | n2-standard-8 |
    |---|---:|---:|---:|---:|
    | entries per AppendEntries | 4.03–4.89 | 4.44–4.83 | 4.45–4.86 | 4.46–4.84 |
    | amplification at 16 B | ~9x | 6.68–6.72 | 6.74–6.81 | 6.84–6.95 |
    | RPC/commit at a 20 ms tick | 2.23 | 2.10 | 2.09–2.10 | 2.10 |
    | CBOR/JSON amplification | 0.30–0.87 | 0.92–0.99 | 0.87–0.99 | 0.96–1.01 |
    | 4096 B ÷ 16 B throughput | **0.039–0.047** | 0.33–0.35 | 0.41–0.43 | 0.40–0.41 |

    **RPCs per committed entry converges to 2.10 on all three cloud machines**,
    against a floor of two — one AppendEntries per follower. Two instruction
    sets and two cloud vendors produce the same number to three figures. That
    is as strong a statement as this suite can make that the quantity is a
    property of the algorithm rather than of anything underneath it
  - **H6 is refuted a fourth time**, at 1.44–1.49x here against 1.34x on
    Graviton, 1.40x on AWS x64 and 2.3x locally; every percentile falls as the
    clock slows and no floor appears at any cadence
  - **The knee verdict is unanimous across the cloud.** CBOR/JSON amplification
    is 0.96–1.01 here, and the case's own printed decision rule — *at or near
    1.00x says the knee is NOT about encoded size* — now reads the same way on
    three machines. Task 11b's finding is confirmed as specific to the
    development machine rather than to the implementation
  - **`scripts/perf-cloud/audit-gcp-leaks.sh` is a re-implementation, not a
    shared abstraction, and that was the right call**: GCE has no key pairs
    (SSH keys are metadata), its boot disk is a zonal child of the instance,
    and its firewall rules cannot carry labels. What IS carried over verbatim
    are the two properties `audit-aws-leaks.sh` learned the hard way — a failed
    query reports UNKNOWN rather than clean, and `out=$(...) || rc=$?` keeps
    `set -e` from aborting before the handling runs
  - **GCP needed a third property AWS did not, and it is the nastier one.**
    `gcloud compute ... list` **exits 0 when authentication fails** — it prints
    "WARNING: Some requests did not succeed" to stderr, emits nothing on
    stdout, and returns success. Measured directly with a deliberately invalid
    token: four of the five queries "succeeded" with empty output. That is the
    same "an auditor that cannot see reports clean" defect task 17 fixed on
    AWS, arriving through a different door — there a swallowed stderr, here a
    command that reports success having looked at nothing. The audit now
    inspects stderr for the partial-failure and authentication markers, and
    **captures stdout and stderr separately**, because merging them would let a
    benign "filter keys were not present" warning be counted as a surviving
    resource. Verified in both directions: bad credential → exit 1 with all
    five queries UNKNOWN and zero reported clean; good credential → exit 0 with
    all five clean
  - **No firewall rule is created, deliberately.** The default network already
    carries `default-allow-ssh` permitting tcp:22 from 0.0.0.0/0, so a narrower
    run-scoped rule cannot subtract from it and would only add a resource
    capable of leaking — and GCE firewall rules cannot carry labels, so it
    would have to be audited by name prefix, a weaker contract than every other
    resource here has. Tightening it would mean editing a project-wide rule,
    which is out of scope for a measurement
  - **GCE's dead-man switch is better than the one AWS forced.**
    `--max-run-duration` with `--instance-termination-action=DELETE` is
    enforced by the control plane, so it fires even if the guest never boots,
    and it **deletes** rather than stopping — a stopped GCE instance still
    bills its boot disk. It needs no credentials on the instance, so
    Requirement 18.4's "the instance needs no cloud permissions" survives
  - **SSH keys go in INSTANCE metadata, never project metadata.**
    `gcloud compute ssh` would push a key into the project's metadata, where it
    outlives the run and grants access to every instance in the project that
    accepts project keys. An instance-scoped key dies with the instance and
    cannot leak
  - **`capture-provenance.sh` grew a GCE branch** — a different server, header
    and path shape, so a second probe rather than a parameterisation of the
    first. GCE returns fully-qualified URLs for zone and machine type, and does
    not serve the region at all (it is the zone minus its trailing letter)
  - **One defect this run found in its own first attempt**: the boot disk class
    was read from `instances describe disks[0].type`, which reports the
    *attachment kind* — `PERSISTENT` — not the class. Requirement 3.4 asks for
    the volume class, and `PERSISTENT` would satisfy nobody comparing a
    pd-balanced row against a pd-ssd one. `disks describe type` is the right
    query and returns `pd-balanced`; the artifact from this run carries the
    wrong value and is superseded
  - _Requirements: 18.12_

## The comparison itself

- [x] 22. Build the external comparison register
  - One record per external number with every field of Appendix A; anything the
    source does not state recorded as **not stated**, never inferred
  - TiKV, Dragonboat, braft, etcd at minimum; database-level systems classified
    as such
  - **`doc/data/multi_raft_external_comparison_register.json`, 14 records over
    the four required implementations**, every one carrying all fifteen Appendix
    A fields. Machine-readable rather than prose because Requirement 11.4 wants
    the register reproduced *in full* inside the comparison document, which
    means the same content exists twice and can drift
  - **`scripts/render-external-register.py` owns the rendered block**, between
    two markers in `doc/multi_raft_performance_comparison.md`, and `--check`
    fails when it is stale. Wired into `ci.yml`'s `docs` job — about a second,
    against a class of defect (a register edited without the document being
    regenerated) that no reviewer would catch
  - **The validation is the reason it is a program and not a table.** It refuses
    a record with a missing Appendix A field, a record with an *empty* field —
    blank and "not stated" render identically and mean opposite things, which is
    exactly the inference Requirement 9.2 forbids — a `kind` outside
    library/database, a duplicate id, and any register that has lost one of
    Requirement 9.4's four named systems. Tested in both directions
  - **braft is in the register with no number, and that record is the point.**
    Its benchmark document states hardware (12 cores, Xeon E5-2620 v2 at 2.10
    GHz, LENOVO SAS 300G at ~800 IOPS random write, 10 GbE without multiqueue),
    3 replicas, 100 client threads and sync enabled — and publishes its QPS and
    latency **only as PNG charts**. Requirement 9.3 forbids a number that cannot
    be sourced to a retrievable document, and a chart read by eye is not one. So
    the C++ multi-group peer 9.4 asks for is present as a configuration record
    and a stated absence
  - **etcd is the richest source and TiKV the thinnest.** etcd's page states
    hardware, cluster size, key and value size, connection and client counts,
    and both consistency levels, so its seven records carry "not stated" only
    for durability and batching. TiKV's states 40 vCPU and 3 nodes and then
    leaves payload size, client concurrency, replication factor, group count,
    durability and batching all unstated — six of fifteen fields — which is
    itself the finding about how comparable that number is
  - **Dragonboat's headline needs two documents to be a record at all.** The
    README carries the numbers; `docs/test.md` carries the hardware, the 48-shard
    topology and "fsync is strictly honored". Neither alone satisfies 9.1, and
    the register cites both on every Dragonboat record
  - _Requirements: 9.1–9.5_

- [x] 23. `doc/multi_raft_performance_comparison.md`
  - Like-for-like and indicative tables kept separate; no bare multiplier
    anywhere; H1–H7 each with a verdict and the number behind it; every metric
    with no possible like-for-like comparison saying so explicitly
  - Absolute `https://github.com/crawlins/kythira/blob/main/doc/…` link from
    `README.md`, since a relative one fails the `docs` CI job
  - **The like-for-like table is EMPTY, and that is the document's headline.**
    Requirement 3.3 forbids a like-for-like claim from any tier below C; every
    row this spec has ever produced is Tier A or Tier B; every external number
    in the register was taken on a multi-machine cluster with a real disk under
    it. There is no pair of numbers a like-for-like table could truthfully hold.
    Requirement 9.8 is then discharged **metric by metric** — seven metrics,
    each with why no like-for-like comparison is possible and what it would
    take — rather than once in a preamble
  - **Requirement 3.7 is discharged in a tier table, not in a footnote.** Tiers
    C, D and E are all recorded as not delivered, all for the same reason: they
    need a host process in `cmd/`, which is Appendix B's third open question and
    was not built. Tier D additionally needs `file_persistence` plus
    `tick_batch_controller` wired into a harness whose `persistence_engine_type`
    is currently a fixed `memory_persistence_engine`
  - **No bare multiplier appears anywhere, including in the summary** (9.6).
    Every gap in the indicative table is stated with its tier, its comparison
    class and its mismatched axes, and two rows state that no comparison is
    drawn at all rather than drawing a weak one — braft (no retrievable number)
    and p99 write latency (ours is *absent*, not large, because a 400–600
    operation window has too few samples)
  - **The etcd write comparison is the one worth reading and the reason is a
    negative.** About an eighth of etcd's rate at roughly a sixtieth of the
    client concurrency, and the decomposition says there is no eightfold of
    overhead to find — transport is 9% of an operation and routing is under 13%.
    So the gap is attributed to configuration rather than to efficiency, and
    the document says which measurement would settle it
  - **The Dragonboat comparison is three orders of magnitude and is reported
    anyway, with a sentence saying it is not evidence.** 66 cores against 8, 48
    groups against 4, NVMe-backed fsync against memory persistence, and a client
    concurrency the source does not state. Requirement 9.4 names Dragonboat as
    the closest peer *in kind*; leaving it out would be the more misleading
    choice, and leaving it in without that sentence would be worse
  - **H4 is recorded as UNTESTED with its reason**, which is the only honest
    verdict: `file_persistence` is not wired into this harness, that is Tier D,
    and nothing measured here should be read as evidence about it
  - **Requirement 16.5's "what this could not answer" is six items long**, and
    16.4's follow-on list is ordered by measured cost with the `cmd/` host
    binary first, because it unblocks every tier that could ever carry a
    like-for-like claim
  - _Requirements: 9.6–9.8, 11.2–11.4, 16.1–16.5_

- [x] 24. CI regression tier
  - **The gap this closes is that nothing in this suite ran in CI at all.**
    `multi_raft_http_benchmark_test` carries `performance;benchmark`, `ci.yml`
    filters `^(slow|performance|verbose|benchmark|docker)$` on every ctest
    invocation, and the three-hour matrix is correctly excluded by that. Correct,
    and one configuration change away from "never checked"
  - `multi_raft_regression_tier` is a **second CTest entry over the same
    binary** with a `--run_test` filter, under labels CI does not exclude
    (`multi-raft;regression`). No extra compilation, and verified by
    `ctest -N -LE '^(slow|performance|verbose|benchmark|docker)$'` selecting it
    while the full matrix and the report smoke stay excluded (12.3)
  - **Tier A, 15.4 seconds** measured in Release on four cores, dominated by five
    elections rather than by the workload — the tier and the budget Requirement
    12.4 asks the design to state, and the design now states them. Tier A
    resolves what had been an open question there: a shared runner's socket
    behaviour varies by more than any regression these bounds could detect
  - **Ratios first (12.1)**, checked on *every* repetition rather than on the
    median, because a structural regression that appears once in five is still
    one: completion rate (1.0 — Tier A has no socket to lose and no disk to
    block on, so a failed operation is structural), entries per AppendEntries
    (≥ 1.0, which is an exactness check on the instrument rather than a
    performance bound, since the denominator counts only calls that carried
    something), and RPCs per committed entry in `[1.5, 60.0]` — bracketing every
    value tasks 8, 11 and 12 ever measured (2.2 to 11.0) with a floor below one
    AppendEntries per follower and a ceiling five times the highest seen
  - One wall-clock bound, on the median run, at **20 ops/sec** — two orders of
    magnitude below the 1568–2237 this row measured across three future
    backends. It catches a cluster that elected and then committed almost
    nothing, and catches nothing else
  - Every constant carries its reasoning beside it in a `regression_bounds`
    namespace (12.5), and `check_at_least` / `check_within` exist so that naming
    the metric, the bound, the measured value and the tier is a property of
    every message rather than of whoever wrote it (12.6)
  - **No external comparison can appear (12.2)**: there is no external number in
    that translation unit, and Tier A is the tier Requirement 3.1 labels never
    comparable to one
  - An election inside a measured window is **reported and not asserted**. On a
    loaded runner an election is a fact about the runner, and failing on it would
    make this the flaky test the whole design is written to avoid
  - `ci.yml` needs no change: `scripts/check-test-run.sh` derives its expected
    count from `ctest -N` under the same filter, so one more test raises both
    sides together, and `--floor 400` is a lower bound
  - _Requirements: 12.1–12.6_
