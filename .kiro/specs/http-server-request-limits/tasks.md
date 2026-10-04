# Implementation Plan

## Implementation Status

Spec written 2026-10-02 against `main` at `9c738d2`. Tasks 1, 2, 4-8 done
2026-10-03; task 3 (cpp-httplib) done 2026-10-04 once PR #435 had replaced
that server's task queue with `growing_task_queue`, which the gate wraps.

Findings from the work, beyond what the design predicted:

- **Beast never sent its 413 for a declared `Content-Length`.** Beast raises
  `body_limit` from `finish_header()` before it marks the header done, and
  the 413 path required `is_header_done()`, so the commonest oversized
  request got a bare close. Only chunked bodies reached the 413. The
  existing test only checked that the RPC failed, so it passed either way.
- **The Beast 413 path then crashed on the Folly backend** ("pure virtual
  method called"): the session, and the strand executor it owns, died
  before the flattened error-chain future dispatched through that executor.
  `read_loop()` now holds the session until the chain's last callback.
- **cpp-httplib's 413 had no body or `Content-Type`.** An error handler now
  gives it Beast's and Proxygen's `text/plain` body (Requirement 6.5).
- **cpp-httplib needed no shared pool (1.1).** httplib 0.58 closes the
  socket when `enqueue()` returns false, before any read or TLS handshake.
  Each listener keeps its own `growing_task_queue` (sized to the limit, so
  `listen_internal`'s `shutdown()` stays per listener); only the gate is
  shared, which is enough to make a `*` bind draw on one limit.
- **Logging (Requirement 5.1) is metrics only.** None of the three HTTP
  servers has a logger to write through; the refusal counters are emitted
  under each transport's existing prefix
  (`beast_http.server.request_too_large`, `...connection_refused`, and the
  same under `proxygen_http.server.`) rather than a shared `http.server.`
  name with a `transport` dimension, matching every other server metric.

## Overview

Enforce `max_request_body_size` on the Proxygen server and
`max_concurrent_connections` on all three HTTP servers through one shared
`connection_gate`, then correct the config comments and add behavioural
tests. Tasks 2 to 5 depend on task 1's findings; tasks 3, 4 and 5 are
independent of each other once task 2 lands.

## Tasks

- [x] 1. Spike: confirm the hooks against the vendored libraries
  - [x] 1.1 cpp-httplib: confirm `enqueue() == false` closes the socket in
    the vcpkg-resolved version, and whether `new_task_queue` can hand each
    listener a non-owning wrapper over one shared `ThreadPool`
    (`listen_internal` calls `shutdown()` on its queue when it returns)
  - [x] 1.2 Proxygen body limit: confirm that a 413 sent from `onRequest` or
    mid-`onBody` with `Connection: close` completes the transaction, that
    `requestComplete()` or `onError()` runs exactly once afterwards, and
    that no `onBody` reaches a deleted handler; otherwise switch to
    `sendAbort()` after the response
  - [x] 1.3 Proxygen connection limit: choose candidate A (session info
    callback) or B (own acceptor factory) per the design; confirm
    `dropConnection()` from `runInLoop` after `onCreate` closes the socket
    and fires `onDestroy`
  - [x] 1.4 Count the worst-case inbound connections per node across
    `tests/`, `examples/` and the multi-raft benchmarks (Requirement 3.2);
    record the figure and any test that needs a raised limit here
  - _Requirements: 1.2, 1.4, 2.1, 3.2_
  - Done: 1.2 holds as designed: `Connection: close` completes the
    transaction, `requestComplete()` runs once, and a client still streaming
    after the 413 is cut off without reaching a deleted handler
    (`body_still_streaming_after_413_is_discarded`). 1.3 is candidate A;
    `dropConnection()` queued with `runInLoop` resets the socket and fires
    `onDestroy`. 1.4: every test, example and multi-raft harness that
    starts a Beast or Proxygen server uses the default
    `connection_pool_size` (10) and at most five nodes, so the worst case is
    4 x 10 = 40 inbound per server. Several Raft groups share one transport
    and so one pool. No test needed a raised limit.

- [x] 2. `connection_gate`
  - [x] 2.1 Add `include/raft/http_connection_gate.hpp` with `connection_gate`
    and its RAII `slot`, a lock-free `try_acquire`, `std::invalid_argument`
    on a zero limit, and the rate-limited `should_log_refusal()`
  - [x] 2.2 Add `tests/http_connection_gate_unit_test.cpp`: limit, release,
    move, double release, gate outliving its server via the slot, and a
    multi-threaded high-water-mark check; register it outside the HTTP
    transport Kconfig gates
  - _Requirements: 2.1, 2.2, 2.5, 2.6, 6.6_

- [x] 3. cpp-httplib connection limit
  - [x] 3.1 Add `gated_task_queue` and set `new_task_queue` in
    `make_listener()` and `configure_ssl_server()`, one gate per
    `cpp_httplib_server`, inner pool sized to the limit (shared across
    listeners if 1.1 allows)
  - [x] 3.2 No metric (the server emits none today) and no log line (it has
    no logger); refusals are counted on the gate
  - [x] 3.3 Tests: oversized body gets 413 on the wire; limit-2 hold, refuse,
    release, succeed; shared limit across a `*` bind
  - _Requirements: 2.1-2.7, 5.1, 6.1, 6.3, 6.4_

- [x] 4. Beast connection limit
  - [x] 4.1 Create the gate in the `boost_beast_server` constructor; in
    `do_accept`, acquire before building the stream and RST-close on
    refusal; move the slot into `server_session` on both the plain and TLS
    branches
  - [x] 4.2 Log and emit `http.server.connection_refused`; emit
    `http.server.request_too_large` from the existing 413 path
  - [x] 4.3 Tests: raw-socket 413 assertion (the existing
    `server_rejects_oversized_request_body` only checks that the RPC fails);
    limit-2 hold, refuse, release, succeed; shared limit across a `*` bind
  - _Requirements: 2.1-2.7, 5.1, 5.2, 6.1, 6.3, 6.4_

- [x] 5. Proxygen limits
  - [x] 5.1 Body limit in `rpc_request_handler`: `Content-Length` check in
    `onRequest`, running total before any copy in `onBody`, single 413 with
    `Connection: close`, `onEOM` skip after rejection
  - [x] 5.2 Connection limit via the mechanism chosen in 1.3, one gate per
    `proxygen_server`, validated in the constructor
  - [x] 5.3 Log and emit both counters
  - [x] 5.4 Tests: 413 by `Content-Length`; 413 for a chunked body crossing
    the limit; a body exactly at the limit is served; limit-2 hold, refuse,
    release, succeed; shared limit across a `*` bind
  - _Requirements: 1.1-1.7, 2.1-2.7, 5.1, 5.2, 6.1-6.4_

- [x] 6. Cross-transport parity test
  - [x] 6.1 Send one oversized request to each of the three servers and
    assert the same status and `Content-Type`
  - _Requirements: 6.5_

- [x] 7. Documentation
  - [x] 7.1 Rewrite the field comments on all three server config structs,
    replacing the false Proxygen "accept-time counter" paragraph
  - [x] 7.2 Update `doc/http_transport_troubleshooting.md` (and any other
    `doc/` page that describes HTTP server configuration)
  - [x] 7.3 Add a one-line note to the Implementation Status of
    `.kiro/specs/proxygen-http-transport/tasks.md` (task 4) and
    `.kiro/specs/boost-beast-http-transport/tasks.md` pointing to this spec
    for Requirement 11.3's enforcement
  - _Requirements: 4.1-4.3_

- [x] 8. Regression run
  - [x] 8.1 Run the cpp-httplib, Beast and Proxygen transport, interop and
    negotiation suites with enforcement on, and fix any test that relied on
    unlimited connections by setting its limit explicitly (from 1.4)
  - _Requirements: 3.3, 3.4, 2.7_
