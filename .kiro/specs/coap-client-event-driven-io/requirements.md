# Requirements — Event-Driven I/O for the libcoap Client

## Introduction

The libcoap client's I/O thread (`coap_client` constructor,
`include/raft/coap_transport_impl.hpp`) runs this loop for the lifetime of the
client:

```cpp
while (!stop_token.stop_requested()) {
    { std::lock_guard lock(_mutex); coap_io_process(_coap_context, COAP_IO_NO_WAIT); }
    cleanup_expired_multicast_requests();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}
```

That shape is the fix for a real defect (`0040eea`, PR #227, recorded in
`doc/CHANGELOG.md`): the loop before it held `_mutex` across a 20 ms
*blocking* `coap_io_process()`, so `send_rpc()` waited on the lock for a median
of 19.9 s on an idle CI runner. Draining without blocking under the lock and
sleeping outside it removed that starvation. Its stated trade-off was that an
incoming PDU may wait up to 5 ms for dispatch.

The CoAP row of the multi-Raft performance matrix
(`.kiro/specs/coap-transport-multi-raft/` task 14, recorded in
`doc/multi_raft_performance_comparison.md`) measured the cost of that
trade-off under load, and it is not 5 ms of latency. It is a throughput
ceiling:

- At a 2 ms tick with 8 and 64 groups the servers received 692–882
  AppendEntries per second against 2,488–2,975 sent. The rest queued inside
  libcoap; 52,307 and 74,452 requests were dropped as undeliverable at
  teardown, RequestVote queued behind them, and no full set of leaders formed.
- One `coap_io_process(ctx, COAP_IO_NO_WAIT)` call reads, by our reading of
  libcoap 4.3.x, at most one datagram per ready socket, so a fixed sleep
  between calls caps the reply rate at about one reply per socket per 5 ms
  regardless of how many are waiting. Task 1 confirms this on the libcoap
  builds the project uses before any code depends on it.
- Cutting the sleep to 1 ms on the four-group smoke check made sends and
  receipts match at about 1,500 per second and the check passed. That change
  was reverted, because it only moves the ceiling and multiplies the idle
  wake-up rate by five.

This specification replaces the fixed sleep with two things together: the
thread **waits on socket readiness** (and on its own timers) instead of
sleeping, and on each wake it **processes every reply that is ready** before
it waits again. It keeps the rule PR #227 established: `_mutex` is never held
across a blocking wait.

## Glossary

- **I/O thread** — `coap_client::_io_thread`, the only thread that calls
  `coap_io_process()` on the client context in steady state.
- **Pass** — one iteration of the I/O thread's outer loop: drain, housekeeping,
  wait.
- **Drain step** — one `coap_io_process(ctx, COAP_IO_NO_WAIT)` call made with
  `_mutex` held.
- **Readiness fd** — the descriptor libcoap exposes through
  `coap_context_get_coap_fd()`: an epoll descriptor that becomes readable
  when any socket libcoap owns has work. It is `-1` when libcoap was built
  without epoll support.
- **Wake fd** — an `eventfd` the client owns, written to wake the I/O thread
  out of its wait.
- **Paced mode** — the fallback used when no readiness fd exists: drain, then
  sleep, as today, but with the drain of Requirement 2.

---

## Requirements

---

### Requirement 1: Wait on Readiness, Not on a Clock

**User Story:** As an operator running Raft over CoAP, I want a reply to be
dispatched when it arrives, so that the transport's reply rate is set by the
network and the peer rather than by a sleep constant.

#### Acceptance Criteria

1. WHERE the readiness fd is available, the I/O thread SHALL block in a wait
   on the readiness fd and the wake fd together, and SHALL NOT call
   `std::this_thread::sleep_for` in steady state.
2. The wait SHALL return as soon as the readiness fd becomes readable, so
   that a datagram arriving while the thread is waiting starts a drain without
   any fixed delay.
3. The wait's timeout SHALL be the earliest of: the next deadline libcoap
   reports for its own timers (retransmission, block-wise, session
   keep-alive and expiry), the next open multicast collection window's close,
   and a configured maximum wait (Requirement 6).
4. A timeout or a wake SHALL be followed by a drain step even when the
   readiness fd is not readable, so that libcoap's timer work runs.
5. The I/O thread SHALL NOT busy-wait: with no traffic and no timers due, it
   SHALL wake no more often than once per configured maximum wait.

---

### Requirement 2: Process Every Ready Reply on Each Pass

**User Story:** As a maintainer, I want one pass to empty the backlog that is
ready, so that a burst of replies is dispatched in one go rather than one per
socket per iteration.

#### Acceptance Criteria

1. On each pass the I/O thread SHALL repeat drain steps while the readiness fd
   still reports work, rather than making a single `coap_io_process()` call.
2. Readiness SHALL be re-checked between drain steps with a zero-timeout
   poll of the readiness fd, outside `_mutex`.
3. A pass SHALL stop draining after a configured drain budget of steps
   (Requirement 6) even if work remains, so that multicast housekeeping and
   stop requests are never delayed by a sustained flood. Work left over SHALL
   make the following wait return immediately, so the budget bounds a pass
   and does not leave datagrams unread.
4. `_mutex` SHALL be acquired and released around each drain step
   individually, never across the whole drain, so that `send_rpc()` competes
   for the lock between steps exactly as it does today.
5. In paced mode the I/O thread SHALL still drain until a drain step finds no
   work or the budget is reached, using whatever readiness signal is
   available, before it sleeps.
6. The number of drain steps per pass SHALL be observable through the
   existing `KYTHIRA_COAP_SEND_PROBE` mechanism or a sibling counter in the
   same log stream, not a second instrumentation scheme.

---

### Requirement 3: The Lock Discipline of PR #227 Is Preserved

**User Story:** As a maintainer, I want the starvation PR #227 fixed to stay
fixed, so that the CoAP suite's dominant flake does not come back in a new
shape.

#### Acceptance Criteria

1. `_mutex` SHALL NOT be held across any blocking wait performed by the I/O
   thread: not the readiness wait, not the paced-mode sleep, not the
   inter-step readiness poll.
2. Every libcoap call the I/O thread makes, including the one that reports the
   next timer deadline, SHALL be made with `_mutex` held, because libcoap's C
   API is not safe to call concurrently on one context.
3. Waiting on the readiness fd SHALL be done with the operating system's
   `poll`/`epoll_wait`, never with a libcoap call, so that no libcoap function
   blocks while another thread may need the context.
4. The `send_rpc()` probe's `lock_wait_us` distribution in the task 14 cells
   SHALL NOT regress: median 0 µs and p95 no worse than the recorded figures
   for the same cell.

---

### Requirement 4: New Work Wakes the Thread

**User Story:** As a maintainer, I want a send made while the I/O thread is
waiting to be serviced on time, so that a long wait never delays a
retransmission or a newly opened multicast window.

#### Acceptance Criteria

1. After `send_rpc()` (and every other client path that calls `coap_send()`
   or opens a multicast collection) has handed a PDU to libcoap, the client
   SHALL signal the wake fd, so that the I/O thread recomputes its timeout
   from the deadlines that now exist.
2. Signalling SHALL be safe from any thread and SHALL NOT require `_mutex`.
   Signals made while the thread is not waiting SHALL NOT be lost: the next
   wait SHALL return immediately.
3. Signalling MAY be coalesced (for example an atomic "wake pending" flag
   tested before the write) so that a burst of sends costs one write, provided
   criterion 2 still holds.
4. The wake fd SHALL be drained by the I/O thread on each wake, so a burst of
   signals produces one extra pass, not one per signal.

---

### Requirement 5: Prompt and Safe Shutdown

**User Story:** As a test author, I want destroying a client to take no longer
than it does today, regardless of how long the I/O thread had decided to wait.

#### Acceptance Criteria

1. A stop request on `_io_thread` SHALL signal the wake fd (for example from a
   `std::stop_callback`), so that the thread leaves its wait immediately.
2. `~coap_client()` SHALL still join `_io_thread` before freeing the context,
   and SHALL still not hold `_mutex` while joining.
3. The wake fd SHALL be closed only after `_io_thread` has been joined.
4. Destroying an idle client SHALL complete within the same bound as today
   (dominated by the existing teardown, not by the I/O wait).

---

### Requirement 6: Configuration and the Fallback Path

**User Story:** As an operator, I want the new loop to work on every libcoap
build the project supports, and as a test author I want to exercise the
fallback without a second libcoap build.

#### Acceptance Criteria

1. `coap_client_config` SHALL gain an I/O wait mode with values `automatic`
   (default), `readiness` and `paced`. `automatic` SHALL use readiness when
   `coap_context_get_coap_fd()` returns a valid descriptor and paced
   otherwise; `readiness` SHALL fail client construction with a descriptive
   error when no readiness fd exists; `paced` SHALL force the fallback.
2. `coap_client_config` SHALL gain a drain budget (steps per pass), a maximum
   wait, and a paced-mode interval, each with a default chosen and justified
   in the design. The paced interval default SHALL stay 5 ms so that paced
   mode is no slower than today.
3. `coap_config_validation.hpp` SHALL reject a zero drain budget, a zero or
   negative maximum wait, and a negative paced interval.
4. Client construction SHALL log, once, which mode was selected and why.
5. Both modes SHALL be exercised by the test suite on CI, the paced mode by
   forcing it through configuration.

---

### Requirement 7: Single-Group Behaviour Is Unchanged or Better

**User Story:** As a user of single-group CoAP clusters, I want this change to
be invisible to me except that replies arrive sooner.

#### Acceptance Criteria

1. The existing CoAP test suite SHALL pass in both wait modes with no test
   weakened, skipped or re-timed to accommodate the change.
2. The coap-flake-measure workflow SHALL be run before and after on the same
   selection and runner class, with the same number of iterations PR #227
   used, and SHALL show no increase in failures.
3. No change SHALL be made to retransmission parameters, message types,
   block-wise settings, DTLS/OSCORE behaviour, or anything on the wire.
4. The client's CPU use when idle SHALL NOT increase; it is expected to fall
   (one wake per maximum wait instead of one per 5 ms).

---

### Requirement 8: The Result Is Measured and Recorded

**User Story:** As a maintainer, I want to know what this change bought, in
the same matrix that found the problem, including whatever it did not fix.

#### Acceptance Criteria

1. The CoAP cells of the multi-Raft matrix (task 14 of
   `coap-transport-multi-raft`) SHALL be re-run at 1, 8 and 64 groups at the
   standard 2 ms tick, on a machine of the same description, with the same
   operation count, concurrency and repetitions.
2. The results SHALL be recorded in `doc/multi_raft_performance_comparison.md`
   beside the original CoAP table, with sends against server receipts per
   second for each cell and the drain-steps-per-pass distribution.
3. The record SHALL state each hypothesis this change rests on and its
   verdict, including refutations, in the document's existing
   hypothesis-table form.
4. WHERE a cell still fails, the record SHALL say what binds it now. In
   particular the one-group block-wise collapse (finding C4) and `multi_raft`
   re-sending unacknowledged AppendEntries on every tick are separate causes
   this specification does not address, and the record SHALL NOT attribute
   their effect to the I/O loop.

---

### Requirement 9: Scope

**User Story:** As a reviewer, I want the change confined to the loop that was
measured, so that it can be reviewed and reverted on its own.

#### Acceptance Criteria

1. This specification SHALL change only the libcoap client's I/O thread, the
   wake signalling it requires on the client's send paths, and the
   configuration and validation of Requirement 6.
2. The libcoap server's pump loop (`coap_server::start()`) SHALL NOT be
   changed. It holds no lock, no other thread touches its context, and its
   blocking `coap_io_process(ctx, 20)` already wakes on readiness.
3. The DTLS handshake path's own blocking `coap_io_process(_, 100)` and
   `coap_io_process(_, 50)` calls SHALL NOT be changed; they are recorded in
   the design as a known remaining wait.
4. The libnyoci and cantcoap backends SHALL NOT be changed.
5. No change SHALL be made to `multi_raft`'s replication or retry behaviour.
