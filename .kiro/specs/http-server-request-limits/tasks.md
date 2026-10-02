# Implementation Plan

## Implementation Status

Not started. Spec written 2026-10-02 against `main` at `9c738d2`.

## Overview

Enforce `max_request_body_size` on the Proxygen server and
`max_concurrent_connections` on all three HTTP servers through one shared
`connection_gate`, then correct the config comments and add behavioural
tests. Tasks 2 to 5 depend on task 1's findings; tasks 3, 4 and 5 are
independent of each other once task 2 lands.

## Tasks

- [ ] 1. Spike: confirm the hooks against the vendored libraries
  - [ ] 1.1 cpp-httplib: confirm `enqueue() == false` closes the socket in
    the vcpkg-resolved version, and whether `new_task_queue` can hand each
    listener a non-owning wrapper over one shared `ThreadPool`
    (`listen_internal` calls `shutdown()` on its queue when it returns)
  - [ ] 1.2 Proxygen body limit: confirm that a 413 sent from `onRequest` or
    mid-`onBody` with `Connection: close` completes the transaction, that
    `requestComplete()` or `onError()` runs exactly once afterwards, and
    that no `onBody` reaches a deleted handler; otherwise switch to
    `sendAbort()` after the response
  - [ ] 1.3 Proxygen connection limit: choose candidate A (session info
    callback) or B (own acceptor factory) per the design; confirm
    `dropConnection()` from `runInLoop` after `onCreate` closes the socket
    and fires `onDestroy`
  - [ ] 1.4 Count the worst-case inbound connections per node across
    `tests/`, `examples/` and the multi-raft benchmarks (Requirement 3.2);
    record the figure and any test that needs a raised limit here
  - _Requirements: 1.2, 1.4, 2.1, 3.2_

- [ ] 2. `connection_gate`
  - [ ] 2.1 Add `include/raft/http_connection_gate.hpp` with `connection_gate`
    and its RAII `slot`, a lock-free `try_acquire`, `std::invalid_argument`
    on a zero limit, and the rate-limited `should_log_refusal()`
  - [ ] 2.2 Add `tests/http_connection_gate_unit_test.cpp`: limit, release,
    move, double release, gate outliving its server via the slot, and a
    multi-threaded high-water-mark check; register it outside the HTTP
    transport Kconfig gates
  - _Requirements: 2.1, 2.2, 2.5, 2.6, 6.6_

- [ ] 3. cpp-httplib connection limit
  - [ ] 3.1 Add `gated_task_queue` and set `new_task_queue` in
    `make_listener()` and `configure_ssl_server()`, one gate per
    `cpp_httplib_server`, inner pool sized to the limit (shared across
    listeners if 1.1 allows)
  - [ ] 3.2 Log refusals; no metric (the server emits none today)
  - [ ] 3.3 Tests: oversized body gets 413 on the wire; limit-2 hold, refuse,
    release, succeed; shared limit across a `*` bind
  - _Requirements: 2.1-2.7, 5.1, 6.1, 6.3, 6.4_

- [ ] 4. Beast connection limit
  - [ ] 4.1 Create the gate in the `boost_beast_server` constructor; in
    `do_accept`, acquire before building the stream and RST-close on
    refusal; move the slot into `server_session` on both the plain and TLS
    branches
  - [ ] 4.2 Log and emit `http.server.connection_refused`; emit
    `http.server.request_too_large` from the existing 413 path
  - [ ] 4.3 Tests: raw-socket 413 assertion (the existing
    `server_rejects_oversized_request_body` only checks that the RPC fails);
    limit-2 hold, refuse, release, succeed; shared limit across a `*` bind
  - _Requirements: 2.1-2.7, 5.1, 5.2, 6.1, 6.3, 6.4_

- [ ] 5. Proxygen limits
  - [ ] 5.1 Body limit in `rpc_request_handler`: `Content-Length` check in
    `onRequest`, running total before any copy in `onBody`, single 413 with
    `Connection: close`, `onEOM` skip after rejection
  - [ ] 5.2 Connection limit via the mechanism chosen in 1.3, one gate per
    `proxygen_server`, validated in the constructor
  - [ ] 5.3 Log and emit both counters
  - [ ] 5.4 Tests: 413 by `Content-Length`; 413 for a chunked body crossing
    the limit; a body exactly at the limit is served; limit-2 hold, refuse,
    release, succeed; shared limit across a `*` bind
  - _Requirements: 1.1-1.7, 2.1-2.7, 5.1, 5.2, 6.1-6.4_

- [ ] 6. Cross-transport parity test
  - [ ] 6.1 Send one oversized request to each of the three servers and
    assert the same status and `Content-Type`
  - _Requirements: 6.5_

- [ ] 7. Documentation
  - [ ] 7.1 Rewrite the field comments on all three server config structs,
    replacing the false Proxygen "accept-time counter" paragraph
  - [ ] 7.2 Update `doc/http_transport_troubleshooting.md` (and any other
    `doc/` page that describes HTTP server configuration)
  - [ ] 7.3 Add a one-line note to the Implementation Status of
    `.kiro/specs/proxygen-http-transport/tasks.md` (task 4) and
    `.kiro/specs/boost-beast-http-transport/tasks.md` pointing to this spec
    for Requirement 11.3's enforcement
  - _Requirements: 4.1-4.3_

- [ ] 8. Regression run
  - [ ] 8.1 Run the cpp-httplib, Beast and Proxygen transport, interop and
    negotiation suites with enforcement on, and fix any test that relied on
    unlimited connections by setting its limit explicitly (from 1.4)
  - _Requirements: 3.3, 3.4, 2.7_
