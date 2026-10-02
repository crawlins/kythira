# Design — Event-Driven I/O for the libcoap Client

## Overview

The client's I/O thread today does one non-blocking libcoap step, then sleeps
5 ms. The sleep is outside `_mutex`, which is what fixed PR #227's lock
starvation, but it also sets a ceiling: one datagram per socket per 5 ms. The
task 14 matrix ran straight into that ceiling at every group count.

The new loop keeps everything PR #227 got right and changes two things:

1. **Wait on readiness.** libcoap built with epoll exposes one descriptor,
   `coap_context_get_coap_fd()`, that becomes readable when any socket it owns
   has work. The I/O thread `poll()`s that descriptor and a client-owned
   `eventfd` together, outside the lock, with a timeout taken from libcoap's
   own next timer deadline. A datagram wakes it; nothing else needs to.
2. **Drain to empty.** On each wake it repeats the non-blocking libcoap step,
   one lock acquisition per step, until the readiness descriptor reports no
   more work or a per-pass budget is spent.

When libcoap has no epoll support the descriptor is `-1`, and the loop falls
back to paced mode: drain as in (2), then sleep as today.

| § | Topic | Touches |
|---|---|---|
| 1 | What libcoap gives us, and what task 1 must confirm | nothing — probe |
| 2 | The loop | `coap_client` constructor |
| 3 | Waking the thread | send paths, `_io_thread` stop |
| 4 | Lock discipline | the loop's structure |
| 5 | Configuration | `coap_client_config`, validation |
| 6 | Fallback | the loop's paced branch |
| 7 | What this does not change | — |
| 8 | Testing and measurement | tests, benchmark record |

---

## 1. libcoap facts the design rests on

Each is a reading of libcoap 4.3.x source and documentation. None is trusted
until task 1 has checked it against both libcoap builds the project actually
uses: the vcpkg port (`vcpkg.json` requires `>= 4.3.5`) and the system 4.3.4
that the task 14 benchmark machine linked.

| # | Claim | Why it matters | If false |
|---|---|---|---|
| L1 | `coap_context_get_coap_fd(ctx)` returns an epoll fd on Linux builds with `COAP_EPOLL_SUPPORT`, and `-1` otherwise | Selects readiness vs paced mode | Paced mode everywhere; §6 becomes the main path |
| L2 | That fd is pollable and is readable while any registered socket has pending input (level-triggered `EPOLLIN`), and while libcoap has pending output it registered `EPOLLOUT` for | A zero-timeout `poll()` on it is a correct "more work?" test | Drain test must use a different signal (see L2 fallback below) |
| L3 | `coap_io_process(ctx, COAP_IO_NO_WAIT)` services at most one datagram per ready socket per call, and also runs due timers (retransmission, block-wise, keep-alive) | Explains the ceiling; justifies looping | If it already drains, the ceiling is elsewhere and task 1 stops the spec |
| L4 | `coap_io_prepare_epoll(ctx, coap_ticks_now)` runs due timers and returns the milliseconds until the next one, with a documented value for "none scheduled" | Gives the wait its timeout | Use the maximum wait as the timeout; correctness holds, idle wake rate rises |
| L5 | Sessions created later (`coap_new_client_session*`) register their sockets in the same epoll set without any caller action | A new peer's replies wake the existing wait | Signal the wake fd after session creation as well (§3 already does after every send) |
| L6 | `coap_io_process()` handlers (response, NACK) run on the calling thread, inside the call | Handler code keeps running under `_mutex`, unchanged | — |

L2 fallback: if the epoll fd's own readability is not a reliable "more work"
signal, the drain loop instead repeats steps until one step's response and
NACK handlers dispatched nothing, counted by a per-step counter the handlers
already pass through. That is strictly weaker (one empty step per pass) but
needs no libcoap assumption.

L3 is the load-bearing claim. Task 1 tests it directly: queue N replies on one
socket, make one `NO_WAIT` call, count dispatches. If one call dispatches all
N, the bottleneck measured in task 14 is not where this design says it is, and
the right next step is to re-measure, not to build §2.

---

## 2. The loop

```cpp
_io_thread = std::jthread([this](std::stop_token st) {
    std::stop_callback wake_on_stop(st, [this] { signal_io_wake(); });
    const int coap_fd = _io_coap_fd;            // fixed at construction
    while (!st.stop_requested()) {
        // Drain: one lock acquisition per step, budget-bounded.
        std::size_t steps = 0;
        do {
            {
                std::lock_guard lock(_mutex);
                coap_io_process(_coap_context, COAP_IO_NO_WAIT);
            }
            ++steps;
        } while (steps < _config.io_drain_budget && fd_ready_now(coap_fd));
        record_io_pass(steps);

        cleanup_expired_multicast_requests();   // takes _mutex itself

        // Timeout: libcoap's next deadline, multicast windows, the cap.
        std::chrono::milliseconds timeout = _config.io_max_wait;
        {
            std::lock_guard lock(_mutex);
            timeout = std::min(timeout, libcoap_next_deadline());
            timeout = std::min(timeout, next_multicast_deadline_locked());
        }

        // Wait: outside the lock, on readiness or wake.
        wait_for_io(coap_fd, _io_wake_fd, timeout);   // poll(), drains wake fd
    }
});
```

Points that are deliberate:

- **The drain runs before the first wait**, so a pass that was woken by a
  timeout or a wake still gives libcoap one step to run timers (Requirement
  1.4), and a budget-exhausted pass is followed by a `poll()` that returns at
  once because the fd is still readable (Requirement 2.3).
- **`fd_ready_now()` is `poll(fd, POLLIN, 0)`.** It is a system call outside
  the lock; on a 4-CPU box it costs on the order of a microsecond, which is
  noise beside a datagram's dispatch.
- **`libcoap_next_deadline()`** wraps `coap_io_prepare_epoll()` (L4). It runs
  due timers as a side effect, which is harmless: the next drain step would
  have run them anyway.
- **The multicast deadline** is the earliest `start_time + timeout` among open
  `multicast_response_collector`s.
  Today the 5 ms cadence closes windows implicitly; without it, the timeout
  must include them or a window would close up to `io_max_wait` late.

### Defaults, and why

| Setting | Default | Reason |
|---|---|---|
| `io_drain_budget` | 64 steps | At the task 14 rates (~3,000 sends/s across peers) a 5 ms backlog is ~15 datagrams; 64 is 4× headroom while bounding one pass to well under a millisecond of handler work |
| `io_max_wait` | 100 ms | Safety net only — every real deadline is in the timeout already. 10 idle wakes/s against 200 today |
| `io_paced_interval` | 5 ms | Paced mode is today's loop plus the drain; it must not be slower than today |

The defaults are revisited in task 7 against the measurement, not tuned in
advance.

---

## 3. Waking the thread

The wake fd is an `eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)` created in the
constructor only in readiness mode. Paced mode needs none: it never waits
longer than the paced interval.

`signal_io_wake()`:

```cpp
void signal_io_wake() noexcept {
    if (_io_wake_fd < 0) return;
    if (_io_wake_pending.exchange(true, std::memory_order_acq_rel)) return;
    std::uint64_t one = 1;
    (void)::write(_io_wake_fd, &one, sizeof one);
}
```

`wait_for_io()` reads the eventfd to zero and clears `_io_wake_pending`
*before* it computes anything from the state the wake announced. Because the
eventfd counter persists, a signal that lands between the drain and the
`poll()` makes the `poll()` return at once; none is lost (Requirement 4.2).

Call sites, all after the libcoap call that created the new deadline:

- `send_rpc()`, after `coap_send()` returns, for both single-PDU and
  block-wise sends.
- The multicast send path, after the collector is registered.
- The stop callback (Requirement 5.1).

`coap_send()` on a UDP session writes the first transmission immediately, so
the wake is not needed for the datagram to leave; it is needed so that the
waiting thread learns of the retransmission timer, which may be earlier than
the deadline it is currently sleeping toward. With the default 100 ms cap and
`ack_timeout` ≥ the raft-rate profile's value this would rarely matter, but
correctness should not depend on that ordering.

The DTLS handshake path (`coap_client::establish_dtls_connection()`, with its
own `coap_io_process(_, 100)` loop) creates its session
before the I/O thread could know of it; L5 covers the socket, and a wake after
session creation is cheap insurance if L5 proves partial.

---

## 4. Lock discipline

PR #227's rule, restated as invariants the loop above satisfies and the tests
in §8 check:

| Operation | Holds `_mutex`? | Blocks? |
|---|---|---|
| `coap_io_process(ctx, NO_WAIT)` | yes | no |
| `coap_io_prepare_epoll()` | yes | no |
| `poll(coap_fd, 0)` readiness test | no | no |
| `poll({coap_fd, wake_fd}, timeout)` | no | **yes** |
| paced-mode `sleep_for` | no | **yes** |
| `signal_io_wake()` | either | no |

The lock is released between drain steps, not held across the whole drain.
Under a flood that means `send_rpc()` contends with the I/O thread once per
step instead of once per 5 ms. `std::recursive_mutex` is not fair, so this is
the one place the new loop could reintroduce a starvation tail: the I/O
thread now reacquires the lock immediately after releasing it. Two mitigations,
both cheap:

1. The readiness `poll()` between steps is a system call, which gives a
   waiting `send_rpc()` thread a real window (unlike the bare unlock/relock
   that PR #227 removed).
2. The drain budget bounds the run of back-to-back acquisitions.

Requirement 3.4 makes this a measured property: the probe's `lock_wait_us`
in the task 14 cells must not regress. If it does, the remedy is a
`std::this_thread::yield()` between steps *only when the probe shows a waiter*,
not lowering the budget blindly — and the change records which it was.

---

## 5. Configuration

```cpp
enum class coap_io_wait_mode { automatic, readiness, paced };

struct coap_client_config {
    // ... existing fields ...
    coap_io_wait_mode io_wait_mode{coap_io_wait_mode::automatic};
    std::size_t io_drain_budget{64};
    std::chrono::milliseconds io_max_wait{100};
    std::chrono::milliseconds io_paced_interval{5};
};
```

Validation joins the existing client checks in `coap_config_validation.hpp`
(Requirement 6.3). Mode selection happens once, in the constructor, after the
context exists, and is logged with the reason (`"readiness: epoll fd 7"`,
`"paced: libcoap built without epoll"`, `"paced: forced by config"`).

These fields are not added to Kconfig. They are tuning knobs with safe
defaults, and the existing CoAP Kconfig symbols select backends and features,
not loop constants.

---

## 6. Fallback: paced mode

```cpp
while (!st.stop_requested()) {
    std::size_t steps = 0;
    bool more = true;
    while (more && steps < _config.io_drain_budget) {
        std::size_t before = _io_dispatch_count.load(std::memory_order_relaxed);
        { std::lock_guard lock(_mutex); coap_io_process(_coap_context, COAP_IO_NO_WAIT); }
        ++steps;
        more = _io_dispatch_count.load(std::memory_order_relaxed) != before;
    }
    record_io_pass(steps);
    cleanup_expired_multicast_requests();
    std::this_thread::sleep_for(_config.io_paced_interval);
}
```

With no readiness fd the only "more work" signal is whether a step dispatched
anything. `_io_dispatch_count` is incremented in the response and NACK
handlers (which already run on this thread, inside `coap_io_process()`).
Paced mode therefore costs one empty step per pass over today, and gains the
whole backlog per pass instead of one datagram per socket.

This is also the L2 fallback of §1.

---

## 7. What this does not change

- **The server's pump loop.** `coap_server::start()` runs a lock-free blocking
  `coap_io_process(ctx, 20)` in a tight loop with no sleep; it already wakes on
  readiness, and no other thread touches its context. If task 1 confirms L3,
  it too services one datagram per socket per call — but it calls again
  immediately, so it has no pacing ceiling. Left alone.
- **The DTLS handshake waits** (`coap_io_process(_, 100)` and `(_, 50)` in the
  session-establishment path). They predate this spec and PR #227 explicitly
  excluded them. They are a remaining place where libcoap blocks on a thread
  other than the I/O thread, and are recorded here so a later reader does not
  mistake them for an oversight.
- **libnyoci and cantcoap.** Different loops, not measured by task 14.
- **The one-group block-wise collapse (C4)** and **`multi_raft` re-sending
  unacknowledged AppendEntries every tick.** Both are named in
  `doc/multi_raft_performance_comparison.md` as separate causes. This change
  may move the numbers they produce; it does not fix them, and §8's record
  must not claim it does.
- **Anything on the wire.**

---

## 8. Testing and measurement

New test files carry the project's copyright header (CLAUDE.md).

| Test | Asserts | Mode(s) |
|---|---|---|
| Burst drain | A server sends N ≥ 32 replies back to back; the client's pass counter shows them dispatched in ≤ ⌈N / budget⌉ + 1 passes | both |
| Wake on readiness | With `io_max_wait` = 10 s, a single request's reply is dispatched — the test only checks it completes inside a generous budget, never a tight latency figure | readiness |
| Retransmission still fires | A CON request to a silent port is NACKed with "too many retries" on the configured schedule, with `io_max_wait` far longer than `ack_timeout` | both |
| Multicast window closes on time | A collector with a short window resolves within window + slack, with `io_max_wait` far longer | both |
| Idle does not spin | Over a 1 s idle interval the pass counter is ≤ ⌈1 s / io_max_wait⌉ + 2 | readiness |
| Prompt shutdown | Destroying an idle client with `io_max_wait` = 10 s returns within the existing teardown bound | readiness |
| Forced readiness without epoll | `readiness` on a build where L1 gives `-1` throws at construction | readiness (skipped where epoll exists) |
| Validation | Zero budget, zero wait, negative interval each rejected | — |

Timing assertions follow the CoAP suite's own rule — budgets, not targets —
because this suite has blocked unrelated merges at a 20% CI pass rate before.

The whole existing CoAP suite runs twice in CI, once per mode, by a test
fixture parameter (not a second build).

Measurement (Requirement 8): the task 14 matrix cells for CoAP at 1, 8 and 64
groups, 2 ms tick, same machine description, five repetitions, plus the
send/receipt-per-second figures that exposed the problem and the new
drain-steps-per-pass distribution. Hypotheses to be stated before the run and
given verdicts after:

| | Hypothesis |
|---|---|
| E1 | Sends and server receipts per second match at 8 and 64 groups on a 2 ms tick |
| E2 | A full set of leaders forms at 8 and 64 groups on a 2 ms tick |
| E3 | Lock wait does not regress (Requirement 3.4) |
| E4 | The one-group cell still fails, for the block-wise reason C4 names |
| E5 | Most passes are one or two steps; the budget is rarely reached |
