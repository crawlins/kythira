# Implementation Plan

## Implementation Status

**Complete (6/6), October 16, 2026.** Tasks 1 to 5, 6.2 and 6.3 are on
`main` (PR #423; re-checked against the tree October 15, 2026:
`MSG_NOSIGNAL` writes and 1 MiB frame reads in `tcp_rpc.hpp`,
`tcp_connection_tracker.hpp` and its unit test, the gossip
`listener_limits`, the four suites in the `tsan` job, and the CHANGELOG
entry). Task 6.1 closed October 16, 2026:

- ThreadSanitizer: the `tsan` job runs `tcp_connection_tracker_unit_test`,
  `tcp_rpc_unit_test`, `tls_tcp_rpc_integration_test` and
  `tcp_gossip_transport_integration_test` on every push.
- AddressSanitizer: the same four, built locally with
  `-fsanitize=address,undefined` (g++-13, boost backend). The tracker, TLS
  and gossip suites were clean. `tcp_rpc_unit_test` hit one UBSan finding,
  fixed in the same change: `resolve_using()` subtracted the
  `time_point::min()` that `learn()` stores for a never-resolved name, which
  overflowed and wrapped negative, so a peer learned from an incoming RPC
  before its name resolved was never looked up in DNS at all. All four are
  clean after the fix, and `bounded_resolver_test` has a regression case.
- docker_chaos: Container Scenario Tests run
  [38937810252](https://github.com/crawlins/kythira/actions/runs/38937810252)
  (PR #565's head, October 16) passed under both Docker and rootless
  Podman, including `docker-chaos-tests`, `quorum-healing-tests` and
  `docker-mtls-chaos-tests`, whose `chaos_node` clusters talk over the
  hardened `tcp_rpc` and `tls_tcp_rpc` servers. The weekly schedule keeps
  running them.

Where #423 differs from the plan below:

- The TLS server cases live in `tests/tls_tcp_rpc_integration_test.cpp`,
  not `tests/tls_tcp_rpc_unit_test.cpp`, because the certificate fixtures
  are there.
- The shared cases are free functions in
  `tests/tcp_server_hardening_cases.hpp` rather than an adapter template.
- `admit()` sets the request deadline at accept time, not when the
  connection thread starts.
- Each `start()` creates a fresh tracker, so stats count from the latest
  start.

## Overview

Add a shared connection tracker with phase deadlines, connection caps and a
draining `stop()`; make the accept loops survive transient errors; stop the
plain TCP writes from raising SIGPIPE; read frames incrementally; then adopt
all of it in `tcp_rpc_server`, `tls_tcp_rpc_server` and the gossip listener.

## Tasks

- [x] 1. Framing helpers
  - [x] 1.1 Switch `tcp_detail::write_all()` to `send()` with `MSG_NOSIGNAL`,
    retrying on `EINTR`; on platforms without the flag, set `SO_NOSIGPIPE`
    in `connect_to()` and on accepted sockets
  - [x] 1.2 Make `tcp_detail::frame_recv()` and `tls_detail::frame_recv()`
    read the body in chunks of at most 1 MiB, keeping the 64 MiB cap
  - [x] 1.3 Add the SIGPIPE cases (client and server side, SIGPIPE at
    `SIG_DFL`) and the chunked-read cases to `tests/tcp_rpc_unit_test.cpp`
  - _Requirements: 1.1, 1.2, 1.3, 1.5, 7.1, 8.1_

- [x] 2. Limits and tracker
  - [x] 2.1 Create `include/raft/tcp_connection_tracker.hpp` with
    `tcp_server_limits`, `tcp_server_connection_stats` and `validate()`
  - [x] 2.2 Implement `tcp_detail::connection_tracker`: `admit()` with the
    global and per-source caps, `ticket` with `enter(phase)` and an RAII
    release that closes the fd under the tracker mutex, the reaper thread,
    `shutdown_and_drain()`, and `stats()`
  - [x] 2.3 Implement `tcp_detail::run_accept_loop()` with the transient,
    back-off and fatal error classes and the injectable spawn function
  - [x] 2.4 Unit-test the tracker and loop directly over `socketpair()` and
    loopback sockets: caps, IPv4-mapped source folding, reaper timing,
    drain, and a spawn function that throws (new
    `tests/tcp_connection_tracker_unit_test.cpp`, registered in
    `tests/CMakeLists.txt`)
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 3.1-3.5, 4.1-4.5, 5.1, 5.2, 5.3,
    5.6, 6.1, 6.5, 7.2_

- [x] 3. `tcp_rpc_server`
  - [x] 3.1 Add the `tcp_server_limits` constructors; hold the tracker by
    `shared_ptr`; move it in the move constructor
  - [x] 3.2 Replace `accept_loop()` with `run_accept_loop()`; mark the
    handler and reply phases in `handle()`; drop the lambda's own
    `close(client)`
  - [x] 3.3 Order `stop()` as in the design: running flag, back-off notify,
    listeners, accept-thread join, `shutdown_and_drain()`
  - [x] 3.4 Add `connection_stats()`
  - [x] 3.5 Add the deadline, limit, `EMFILE` and `stop()` cases to
    `tests/tcp_rpc_unit_test.cpp` through the shared cases in
    `tests/tcp_server_hardening_cases.hpp`
  - _Requirements: 2.1-2.4, 3, 4, 5.1-5.5, 6.2, 7.2, 8.1, 8.2_

- [x] 4. `tls_tcp_rpc_server`
  - [x] 4.1 Add `server_limits` to `tls_tcp_rpc_config`
  - [x] 4.2 Adopt the tracker and accept loop in `tls_detail::server_impl`;
    the request phase spans `SSL_accept()` through `frame_recv()`
  - [x] 4.3 Remove the fixed `SO_RCVTIMEO`/`SO_SNDTIMEO` and replace the
    "has gotten away with the same omission" comment with a pointer to this
    spec
  - [x] 4.4 Instantiate the shared deadline, limit and `stop()` cases in
    `tests/tls_tcp_rpc_integration_test.cpp`, including a client that stalls
    mid-handshake
  - _Requirements: 2.5, 2.6, 3, 4, 5, 6.3, 8.2, 8.3_

- [x] 5. Gossip listener
  - [x] 5.1 Add `listener_limits` to `tcp_gossip_config`
  - [x] 5.2 Adopt the tracker and accept loop in
    `tcp_gossip_peer2peer_replicator`; add `shutdown_and_drain()` to
    `stop_listener()` after the join
  - [x] 5.3 Instantiate the shared cases and the server-side SIGPIPE case in
    `tests/tcp_gossip_transport_integration_test.cpp`
  - _Requirements: 1.3, 2.5, 3, 4, 5, 6.4, 8.2_

- [x] 6. Verification and documentation
  - [x] 6.1 Run the three suites and the new tracker test built with
    `KYTHIRA_SANITIZER=address` and `=thread`, and the docker_chaos quorum
    scenarios that use `chaos_node` over `tcp_rpc`
  - [x] 6.2 Add the four suites to the `tsan` job in
    `.github/workflows/ci.yml`
  - [x] 6.3 Record the change and the defaults in `doc/CHANGELOG.md`
  - _Requirements: 8.1, 8.4_

## Notes

- `tls_tcp_rpc` keeps `ignore_sigpipe_once()`; OpenSSL's socket BIO writes
  outside `tcp_detail::write_all()`.
- The client-side whole-RPC deadline, metrics on the TCP transports, and
  gossip digest authentication are left to their own specs.
