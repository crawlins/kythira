# Implementation Plan — Event-Driven I/O for the libcoap Client

## Status: In progress — tasks 1–6 done; 7 and 8 need CI and the benchmark machine

**Last Updated**: October 2, 2026

This plan implements `.kiro/specs/coap-client-event-driven-io/design.md`. Four
phases, 8 tasks. Phase 1 is a gate: the design rests on six claims about
libcoap (design §1) that come from reading its source, and the task 14 record
exists because readings of this transport have been wrong before. Nothing in
phases 2–4 starts until task 1 has confirmed or corrected them.

## Overview

The libcoap client's I/O thread sleeps 5 ms between non-blocking libcoap
steps, which caps replies at about one per socket per 5 ms. The task 14 matrix
hit that ceiling at every group count on a 2 ms tick. This plan makes the
thread wait on socket readiness instead of sleeping and, on each wake, process
every reply that is ready, without ever holding `_mutex` across a blocking
wait (PR #227).

**Depends on PR #385** (`coap-transport-multi-raft`), which adds the CoAP row
of the multi-Raft benchmark, the per-group send probe and
`scripts/coap-send-probe-summary.py`. Tasks 4, 7 and 8 use all three; task 1
does not, and can start before #385 lands.

## Task Dependency Graph

```json
{
  "waves": [
    { "wave": 1, "tasks": [1], "description": "GATE: confirm libcoap's readiness fd, per-call datagram limit and deadline API on both builds" },
    { "wave": 2, "tasks": [2, 3], "description": "Configuration and wake signalling" },
    { "wave": 3, "tasks": [4, 5], "description": "Readiness loop and paced fallback" },
    { "wave": 4, "tasks": [6], "description": "Tests in both modes" },
    { "wave": 5, "tasks": [7, 8], "description": "Flake measurement and the task 14 re-run" }
  ]
}
```

---

## Tasks

---

## Phase 1: Gate (Task 1)

- [x] 1. Probe libcoap and record the answers in design §1
  - Write a standalone probe (a test binary under `tests/`, not shipped) that,
    against a real libcoap context, checks L1–L6 of design §1: the value of
    `coap_context_get_coap_fd()`; whether a zero-timeout `poll()` on it tracks
    pending input; how many of N queued replies one
    `coap_io_process(ctx, COAP_IO_NO_WAIT)` call dispatches; what
    `coap_io_prepare_epoll()` returns with and without a pending CON; and
    whether a session created after the first `poll()` wakes it.
  - Run it against the vcpkg libcoap port and against the system libcoap 4.3.4
    the task 14 machine used. Record version, `COAP_EPOLL_SUPPORT`, and each
    answer in a table appended to design §1.
  - **Stop condition.** If L3 is false — one call already drains every ready
    datagram — the bottleneck task 14 found is not where this design puts it.
    Record that, mark the spec blocked, and report back rather than building
    phases 2–4.
  - If L2 or L4 is false, adopt the fallback design §1 names for it and update
    the design before phase 2.
  - Verify: the table exists with a measured answer per claim per build.
  - _Requirements: 1.1, 1.3, 2.1, 2.2, 6.1_
  - **Result:** L1–L6 hold on both builds; the gate passes. Two additions,
    recorded in design §1: `coap_io_prepare_epoll()` returns 0 for "no timer
    scheduled" (L4), and with PR #385's per-peer session and raised NSTART a
    peer's replies queue together on one socket, so L3 is what binds (L7).

---

## Phase 2: Configuration and Wake (Tasks 2–3)

- [x] 2. Add the wait mode, budget and intervals to `coap_client_config`
  - `coap_io_wait_mode { automatic, readiness, paced }` and the three fields
    of design §5, with the defaults in design §2's table.
  - Validation in `coap_config_validation.hpp`: zero budget, non-positive
    maximum wait, and negative paced interval rejected with the existing
    error style.
  - Mode selection in the constructor after the context exists; one log line
    naming the mode and the reason. `readiness` with no epoll fd throws.
  - Verify: validation unit tests; constructing with each mode on this build
    logs the expected reason.
  - _Requirements: 6.1, 6.2, 6.3, 6.4_
  - **Done.** `validate_io_wait_config()` is split out of
    `validate_client_config()` and also run by the constructor, since a zero
    budget or a non-positive wait would make the loop misbehave rather than
    fail. Mode selection also honours `KYTHIRA_COAP_IO_WAIT_MODE` when the
    config says `automatic` (see task 6).

- [x] 3. Wake fd and `signal_io_wake()`
  - Create the `eventfd` in readiness mode only; close it after `_io_thread`
    is joined in `~coap_client()`.
  - Coalesce with an atomic pending flag as design §3 shows; the I/O thread
    clears it and drains the eventfd before computing its next timeout.
  - Call it after `coap_send()` in `send_rpc()` (single-PDU and block-wise),
    after a multicast collector is registered, and after DTLS session
    creation; register a `std::stop_callback` on `_io_thread`'s token that
    calls it.
  - Verify: a unit test signals from several threads while the I/O thread is
    and is not waiting, and every signal is followed by a pass.
  - _Requirements: 4.1, 4.2, 4.3, 4.4, 5.1, 5.3_
  - **Done.** The eventfd is owned by a small RAII member declared before
    `_io_thread`, so it is closed after the thread is joined even when the
    constructor throws after starting it. The DTLS wake is in
    `initiate_dtls_handshake()` and `establish_dtls_connection()`, after the
    session exists. The multi-thread signal check is
    `sends_wake_a_waiting_thread` in `tests/coap_io_wait_mode_test.cpp`,
    through `send_rpc()` rather than the private `signal_io_wake()`.

---

## Phase 3: The Loop (Tasks 4–5)

- [x] 4. Replace the fixed sleep with the readiness loop
  - Implement design §2 in the `coap_client` constructor: budget-bounded
    drain with one `_mutex` acquisition per step and a zero-timeout readiness
    `poll()` between steps; multicast cleanup; timeout from
    `coap_io_prepare_epoll()`, the earliest open multicast window and
    `io_max_wait`, computed under `_mutex`; `poll()` on the epoll fd and wake
    fd outside it.
  - Rewrite the loop's doc comment. Keep the PR #227 history in it (why the
    lock is never held across a wait) and replace the "5 ms of bounded
    response latency" trade-off with the new one.
  - Emit the per-pass step count through the `KYTHIRA_COAP_SEND_PROBE` log
    stream and extend `scripts/coap-send-probe-summary.py` to summarise it.
  - Verify: design §4's table holds by inspection, and a debug assertion (or
    a test-only hook) confirms `_mutex` is not owned by the I/O thread when
    it enters `poll()` or `sleep_for`.
  - _Requirements: 1.1–1.5, 2.1–2.4, 2.6, 3.1, 3.2, 3.3_
  - **Done**, in `coap_client::run_io_loop()`. `coap_io_prepare_epoll()`
    returning 0 means "no timer", so it maps to `io_max_wait` (design §1 L4).
    The probe line is one aggregate per second per client
    (`[stall-probe] io_passes ... steps_hist=1:a,2:b,3-4:c,...`), not one per
    pass, and `scripts/coap-send-probe-summary.py` sums it per cell. The lock
    check is behavioural rather than an assertion: `std::recursive_mutex`
    cannot report its owner, so `the_lock_is_free_while_the_thread_waits`
    times a public call that takes `_mutex` while the thread sits in a 10 s
    wait.

- [x] 5. Paced fallback with the same drain
  - Implement design §6: `_io_dispatch_count` incremented in the response and
    NACK handlers; drain until a step dispatches nothing or the budget is
    reached; then sleep `io_paced_interval` outside the lock.
  - If task 1 found L2 false, the readiness loop uses this same dispatch-count
    test in place of the readiness `poll()` between steps.
  - Verify: forcing `paced` on this build runs the whole CoAP suite.
  - _Requirements: 2.5, 6.1, 6.5_
  - **Done.** `_io_dispatches` also feeds `io_loop_stats()`. L2 held, so the
    readiness loop keeps its `poll()` test.

---

## Phase 4: Tests and Measurement (Tasks 6–8)

- [x] 6. Tests in both modes
  - The tests of design §8's table, each new file with the copyright header.
  - Run the existing CoAP suite in both modes via a fixture parameter, not a
    second build. No existing test may be weakened, skipped or re-timed.
  - Timing checks are budgets with slack, never latency targets.
  - Verify: `ctest -R coap` passes locally in both modes; CI runs both.
  - _Requirements: 1.5, 2.3, 4.1, 5.4, 6.5, 7.1_
  - **Done.** `tests/coap_io_wait_mode_test.cpp` covers design §8's table.
    Two deviations: the burst check allows up to N/4 passes for N = 32
    rather than ⌈N / budget⌉ + 1, because on loopback the client can start
    draining before the last reply lands (measured: 1 pass in both modes);
    and the retransmission check uses a 1 s `ack_timeout`, libcoap's floor
    (`coap_session_set_ack_timeout()` ignores values under a second).
  - The "fixture parameter" is an environment variable:
    `tests/CMakeLists.txt` registers a `<name>_paced` twin of every libcoap
    CoAP test with `KYTHIRA_COAP_IO_WAIT_MODE=paced`, labelled `paced_io`
    instead of `coap` so `-L coap` (the flake workflow's selection) is
    unchanged. One build, both modes, no test edited.
  - Run in the cloud sandbox against libcoap 4.3.5 with the Boost future
    backend (no Folly there): every CoAP test that builds without Folly, in
    both modes. The Folly-only tests run on CI.

- [ ] 7. Flake and lock-wait regression check
  - Run the coap-flake-measure workflow before (on `main`) and after, same
    selection, same runner class, 20 iterations each as PR #227 did.
  - Collect the send probe's `lock_wait_us` for the task 14 cells and compare
    with the recorded figures. If p95 regresses, apply design §4's conditional
    yield, re-measure, and record which remedy was taken.
  - Revisit the defaults in design §2 against what the probe shows (steps per
    pass, how often the budget is reached); change them only with the data.
  - Verify: no increase in failures; lock wait median 0 µs and p95 no worse
    per cell.
  - _Requirements: 3.4, 7.2, 7.4_

- [ ] 8. Re-run the CoAP cells of the multi-Raft matrix and record them
  - The task 14 CoAP cells at 1, 8 and 64 groups on the standard 2 ms tick,
    same machine description, 1920 operations at 16 in flight, five
    repetitions; also the 10 ms-per-group pass for comparison.
  - Record in `doc/multi_raft_performance_comparison.md` beside the existing
    CoAP table: ops/sec, p50, per-group p50 range, outcome, sends against
    server receipts per second, and the steps-per-pass distribution.
  - State hypotheses E1–E5 of design §8 before the run; give each a verdict
    after it in the document's hypothesis-table form, refutations included.
  - For any cell that still fails, say what binds it now. Do not credit or
    blame this change for C4's block-wise collapse or `multi_raft`'s per-tick
    re-sends.
  - Add a `doc/CHANGELOG.md` entry linking PR #227 and the new numbers.
  - Verify: the section exists with measured numbers and verdicts.
  - _Requirements: 8.1, 8.2, 8.3, 8.4_

---

## Notes

**Why the gate.** Every attempt to diagnose this transport from analysis alone
has produced a plausible story the data later contradicted; task 14's own
record refuted the lock hypothesis its specification was written around.
Design §1's L3 is the claim this whole plan rests on, and it costs one probe
to check.

**The one thing not to do.** The shortcut that will suggest itself is going
back to a blocking `coap_io_process(ctx, timeout)` and holding `_mutex` around
it, since libcoap "already waits on the socket". That is exactly the loop
PR #227 removed, with a median `send_rpc()` lock wait of 19.9 s. Waiting must
happen in `poll()`, outside the lock, outside libcoap.

**Where the risk concentrates.**
1. Task 4's lock churn. Releasing and reacquiring `_mutex` per drain step is
   new; task 7's lock-wait comparison is what keeps it honest.
2. Timers that used to be serviced by the 5 ms cadence by accident — multicast
   windows, retransmissions — now need an explicit deadline or wake. Task 6's
   tests set `io_max_wait` far longer than any real deadline so that a missing
   one fails instead of hiding behind the cap.
3. Paced mode on builds without epoll gets less coverage in practice than the
   readiness path. Running the whole suite in both modes (task 6) is the
   remedy.
