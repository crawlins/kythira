# Implementation Plan

## Implementation Status

Implemented in PR #423 (draft), which covers tasks 1 to 5, 6.2 and 6.3
and ticks them when it merges. Task 6.1's docker_chaos quorum run is still
open. Where #423 differs from the plan below:

- The TLS server cases live in `tests/tls_tcp_rpc_integration_test.cpp`,
  not `tests/tls_tcp_rpc_unit_test.cpp`, because the certificate fixtures
  are there.
- The shared cases are free functions in
  `tests/tcp_server_hardening_cases.hpp` rather than an adapter template.
- `admit()` sets the request deadline at accept time, not when the
  connection thread starts.
- Each `start()` creates a fresh tracker, so stats count from the latest
  start.

#423 was written against `main` without PR #389 (ClusterJoin/ClusterLeave
on `tcp_rpc`); whichever of the two merges second rebases over the other.

## Overview

Add a shared connection tracker with phase deadlines, connection caps and a
draining `stop()`; make the accept loops survive transient errors; stop the
plain TCP writes from raising SIGPIPE; read frames incrementally; then adopt
all of it in `tcp_rpc_server`, `tls_tcp_rpc_server` and the gossip listener.

## Tasks

- [ ] 1. Framing helpers
  - [ ] 1.1 Switch `tcp_detail::write_all()` to `send()` with `MSG_NOSIGNAL`,
    retrying on `EINTR`; on platforms without the flag, set `SO_NOSIGPIPE`
    in `connect_to()` and on accepted sockets
  - [ ] 1.2 Make `tcp_detail::frame_recv()` and `tls_detail::frame_recv()`
    read the body in chunks of at most 1 MiB, keeping the 64 MiB cap
  - [ ] 1.3 Add the SIGPIPE cases (client and server side, SIGPIPE at
    `SIG_DFL`) and the chunked-read cases to `tests/tcp_rpc_unit_test.cpp`
  - _Requirements: 1.1, 1.2, 1.3, 1.5, 7.1, 8.1_

- [ ] 2. Limits and tracker
  - [ ] 2.1 Create `include/raft/tcp_connection_tracker.hpp` with
    `tcp_server_limits`, `tcp_server_connection_stats` and `validate()`
  - [ ] 2.2 Implement `tcp_detail::connection_tracker`: `admit()` with the
    global and per-source caps, `ticket` with `enter(phase)` and an RAII
    release that closes the fd under the tracker mutex, the reaper thread,
    `shutdown_and_drain()`, and `stats()`
  - [ ] 2.3 Implement `tcp_detail::run_accept_loop()` with the transient,
    back-off and fatal error classes and the injectable spawn function
  - [ ] 2.4 Unit-test the tracker and loop directly over `socketpair()` and
    loopback sockets: caps, IPv4-mapped source folding, reaper timing,
    drain, and a spawn function that throws (new
    `tests/tcp_connection_tracker_unit_test.cpp`, registered in
    `tests/CMakeLists.txt`)
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 3.1-3.5, 4.1-4.5, 5.1, 5.2, 5.3,
    5.6, 6.1, 6.5, 7.2_

- [ ] 3. `tcp_rpc_server`
  - [ ] 3.1 Add the `tcp_server_limits` constructors; hold the tracker by
    `shared_ptr`; move it in the move constructor
  - [ ] 3.2 Replace `accept_loop()` with `run_accept_loop()`; mark the
    handler and reply phases in `handle()`; drop the lambda's own
    `close(client)`
  - [ ] 3.3 Order `stop()` as in the design: running flag, back-off notify,
    listeners, accept-thread join, `shutdown_and_drain()`
  - [ ] 3.4 Add `connection_stats()`
  - [ ] 3.5 Add the deadline, limit, `EMFILE` and `stop()` cases to
    `tests/tcp_rpc_unit_test.cpp` through the shared cases in
    `tests/tcp_server_hardening_cases.hpp`
  - _Requirements: 2.1-2.4, 3, 4, 5.1-5.5, 6.2, 7.2, 8.1, 8.2_

- [ ] 4. `tls_tcp_rpc_server`
  - [ ] 4.1 Add `server_limits` to `tls_tcp_rpc_config`
  - [ ] 4.2 Adopt the tracker and accept loop in `tls_detail::server_impl`;
    the request phase spans `SSL_accept()` through `frame_recv()`
  - [ ] 4.3 Remove the fixed `SO_RCVTIMEO`/`SO_SNDTIMEO` and replace the
    "has gotten away with the same omission" comment with a pointer to this
    spec
  - [ ] 4.4 Instantiate the shared deadline, limit and `stop()` cases in
    `tests/tls_tcp_rpc_integration_test.cpp`, including a client that stalls
    mid-handshake
  - _Requirements: 2.5, 2.6, 3, 4, 5, 6.3, 8.2, 8.3_

- [ ] 5. Gossip listener
  - [ ] 5.1 Add `listener_limits` to `tcp_gossip_config`
  - [ ] 5.2 Adopt the tracker and accept loop in
    `tcp_gossip_peer2peer_replicator`; add `shutdown_and_drain()` to
    `stop_listener()` after the join
  - [ ] 5.3 Instantiate the shared cases and the server-side SIGPIPE case in
    `tests/tcp_gossip_transport_integration_test.cpp`
  - _Requirements: 1.3, 2.5, 3, 4, 5, 6.4, 8.2_

- [ ] 6. Verification and documentation
  - [ ] 6.1 Run the three suites and the new tracker test built with
    `KYTHIRA_SANITIZER=address` and `=thread`, and the docker_chaos quorum
    scenarios that use `chaos_node` over `tcp_rpc`
  - [ ] 6.2 Add the four suites to the `tsan` job in
    `.github/workflows/ci.yml`
  - [ ] 6.3 Record the change and the defaults in `doc/CHANGELOG.md`
  - _Requirements: 8.1, 8.4_

## Notes

- `tls_tcp_rpc` keeps `ignore_sigpipe_once()`; OpenSSL's socket BIO writes
  outside `tcp_detail::write_all()`.
- The client-side whole-RPC deadline, metrics on the TCP transports, and
  gossip digest authentication are left to their own specs.
