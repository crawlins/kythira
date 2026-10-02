# Requirements Document

## Introduction

Kythira has three servers built on the same raw-socket pattern: the plain
TCP Raft transport (`tcp_rpc_server`, `include/raft/tcp_rpc.hpp`, used by
`chaos_node`), the mTLS Raft transport (`tls_tcp_rpc_server`,
`include/raft/tls_tcp_rpc.hpp`, used by `ca_cluster_node`), and the gossip
listener of `tcp_gossip_peer2peer_replicator`
(`include/raft/tcp_gossip_transport.hpp`). Each runs one accept thread per
listening socket and starts one detached `std::thread` per accepted
connection. The parallel-implementation parity audit (2026-10-02, items T3
and T4) and the vulnerability audit of the same day (item H4) found these
problems, all re-verified on `main` at `9c738d2`:

1. **SIGPIPE kills the process.** `tcp_detail::write_all()` calls `::write()`
   on a socket. If the peer has already closed it (a client whose RPC timed
   out, or one that reset the connection), the kernel raises SIGPIPE, whose
   default action terminates the process. `tcp_rpc_server` hits this on its
   reply write, `tcp_rpc_client` on its request write, and the gossip
   transport on both sides. `tls_tcp_rpc` avoids it only because its client
   and server constructors call `ignore_sigpipe_once()`, which sets SIGPIPE to
   `SIG_IGN` for the whole process.
2. **No deadline on server connections.** `tcp_rpc_server::handle()` and the
   gossip listener's `handle_incoming_exchange()` block in `read()` with no
   timeout. A peer that connects and sends nothing holds a thread and a file
   descriptor forever. `tls_tcp_rpc_server` sets 30 s `SO_RCVTIMEO` and
   `SO_SNDTIMEO`, but those bound each system call, not the connection, so a
   client that sends one byte every 29 s still holds the connection
   indefinitely, TLS handshake included.
3. **One `accept()` error stops the listener for good.** All three accept
   loops `break` on any negative return from `accept()`. `EMFILE` (out of
   file descriptors) or `ECONNABORTED` ends that listener while the process
   keeps running, so the node silently stops receiving Raft RPCs.
4. **Unbounded threads per connection.** There is no limit on concurrent
   connections. About a thousand idle connections exhaust the default
   descriptor limit and trigger item 3. With a higher limit, `std::thread`
   construction eventually throws `std::system_error` inside the accept
   loop, which escapes the thread function and calls `std::terminate`.
5. **Handler threads outlive the server.** The detached threads capture
   `this`. `stop()` joins only the accept threads, so a handler still
   running when the server is stopped or destroyed reads freed members
   (`_rv`, `_ser`, ...) and calls into a `node<Types>` that may already be
   gone.
6. **The frame reader trusts the length prefix.** `frame_recv()` allocates
   the full announced length, up to 64 MiB, zero-filled, as soon as the
   4-byte header arrives. Holding a few dozen connections with such a
   header costs a sender nothing and the server gigabytes.

This spec fixes all six in the two plain-TCP servers and applies the shared
fixes for items 2 to 6 to `tls_tcp_rpc_server` as well, so that the three
servers keep one connection-handling model.

### Relationship to other work

- **PR #389** (open, ClusterJoin/ClusterLeave on `tcp_rpc`) rewrites
  `tcp_rpc_client`'s dispatch into `call_endpoint()`, adds a per-endpoint
  in-flight limit to the client, and adds two dispatch branches and two
  move-constructor members to `tcp_rpc_server`. It does not touch the
  framing helpers, the accept loop, or connection lifetime. This spec's
  implementation (PR #423) was written against `main` without #389; the
  two overlap only in `handle()` and the move constructor, and only
  textually, so whichever merges second rebases over the other.
- The TLS transport's own mTLS, trust-policy and reload behaviour
  (`.kiro/specs/ca-cluster-rpc-mtls/`) is unchanged.

### Non-goals

- Connection reuse, a different serializer, or a different frame-size cap
  for the TCP transports (parity audit T13).
- Metrics on the TCP transports (parity audit T12). This spec exposes
  counters through accessors so tests and a later metrics spec can read
  them, and emits no metrics itself.
- Bounding the client side's whole RPC by its timeout. The client already
  sets `SO_SNDTIMEO`/`SO_RCVTIMEO` from the RPC timeout in `connect_to()`;
  a slow-drip server can stretch one RPC past it, but only a peer the
  client chose to call can do that. The helpers this spec adds make that a
  small follow-up.
- Authenticating gossip digests, or limiting how many node IDs a gossip
  exchange may add (vulnerability audit, low-severity list).
- The HTTP, Beast, Proxygen, gRPC and CoAP servers. They use their own
  libraries' connection handling.

## Glossary

- **Connection server**: any of `tcp_rpc_server`, `tls_tcp_rpc_server`, or
  the listener inside `tcp_gossip_peer2peer_replicator`.
- **Connection**: one accepted socket and the thread that serves it.
- **Request phase**: from `accept()` returning until the full request frame
  has been read. For `tls_tcp_rpc_server` this includes the TLS handshake
  and the trust-policy check.
- **Handler phase**: while the registered Raft handler (or the gossip merge)
  runs on the request.
- **Reply phase**: while the response frame is being written.
- **Source address**: the peer IP address `accept()` reports, without the
  port. An IPv4-mapped IPv6 address counts as its IPv4 address.
- **Transient accept error**: an `accept()` failure with `errno` in
  `EINTR`, `ECONNABORTED`, `EPROTO`, `EPERM`, `EMFILE`, `ENFILE`,
  `ENOBUFS`, `ENOMEM`, or one of the network errors Linux `accept(2)`
  says to treat like `EAGAIN` (`ENETDOWN`, `ENOPROTOOPT`, `EHOSTDOWN`,
  `ENONET`, `EHOSTUNREACH`, `EOPNOTSUPP`, `ENETUNREACH`).
- **`tcp_server_limits`**: the configuration struct this spec adds; see
  Requirement 6.

## Requirements

### Requirement 1: No SIGPIPE from the plain TCP transports

**User Story:** As an operator running `chaos_node`, I want a peer that
closes its end of a connection to cost at most that RPC, so that a client
timing out can never kill the server process.

#### Acceptance Criteria

1. WHEN `tcp_detail::write_all()` writes to a socket whose peer has closed
   or reset it THEN it SHALL return `false` and SHALL NOT raise SIGPIPE.
2. THE fix SHALL be local to the write call (`send()` with `MSG_NOSIGNAL`,
   or `SO_NOSIGPIPE` on platforms without that flag). It SHALL NOT change
   the process's SIGPIPE disposition.
3. THE fix SHALL cover every caller of `tcp_detail::write_all()` and
   `tcp_detail::frame_send()`: `tcp_rpc_client`, `tcp_rpc_server`, and both
   sides of the gossip transport.
4. `tls_tcp_rpc`'s existing `ignore_sigpipe_once()` SHALL be kept. OpenSSL
   writes through its own socket BIO, which this spec does not replace.
5. WHEN `write_all()` is interrupted by a signal before writing anything
   (`EINTR`) THEN it SHALL retry instead of failing.

### Requirement 2: Deadlines on server connections

**User Story:** As an operator, I want every connection a server accepts to
finish or be closed within a bounded time, so that silent or slow peers
cannot pin threads and descriptors.

#### Acceptance Criteria

1. WHEN a connection's request phase has not completed within
   `request_timeout` of `accept()` THEN the server SHALL close it without
   invoking any handler.
2. WHEN a connection's reply phase has not completed within `reply_timeout`
   of the handler returning THEN the server SHALL close it.
3. THE deadline SHALL bound the whole phase, not each system call. A peer
   that trickles bytes SHALL be closed when the phase deadline passes, however
   often it sends.
4. THE handler phase SHALL NOT be subject to a deadline. A slow
   `InstallSnapshot` handler is legitimate and the server cannot interrupt
   user code safely.
5. THE deadline SHALL apply to `tcp_rpc_server`, the gossip listener, and
   `tls_tcp_rpc_server`. For TLS, the handshake, the trust-policy check and
   the request read SHALL all count against `request_timeout`.
6. `tls_tcp_rpc_server`'s fixed 30 s per-call `SO_RCVTIMEO`/`SO_SNDTIMEO`
   SHALL be replaced by the phase deadline, so one setting governs all
   three servers.

### Requirement 3: Accept loops survive errors

**User Story:** As an operator, I want a listener to keep accepting after
a temporary failure, so that a descriptor spike does not leave a node
running but deaf.

#### Acceptance Criteria

1. WHEN `accept()` fails with a transient accept error AND the server is
   still running THEN the accept loop SHALL continue.
2. WHEN the error is `EMFILE`, `ENFILE`, `ENOBUFS` or `ENOMEM` THEN the loop
   SHALL wait before the next `accept()`, starting at 10 ms and doubling up
   to 1 s, and SHALL reset the wait after a successful `accept()`. It SHALL
   NOT spin.
3. THE back-off wait SHALL end early when the server is stopped, so
   `stop()` does not wait out a back-off.
4. THE loop SHALL exit only when the server has been stopped, or on an
   error that means the listening socket itself is unusable (`EBADF`,
   `EINVAL`, `ENOTSOCK`).
5. WHEN starting the thread for an accepted connection fails THEN the
   server SHALL close that connection and keep accepting. No exception
   SHALL escape an accept thread.

### Requirement 4: Bounded connections

**User Story:** As an operator of an mTLS CA cluster, I want an
unauthenticated client to be unable to exhaust a node's threads or
descriptors, so that a flood of idle connections cannot cost the cluster
its quorum.

#### Acceptance Criteria

1. THE server SHALL serve at most `max_connections` connections at once.
2. THE server SHALL serve at most `max_connections_per_source` connections
   at once from any one source address.
3. WHEN a newly accepted connection would exceed either limit THEN the
   server SHALL close it at once, without reading from it and without
   starting a thread for it.
4. A connection SHALL stop counting against both limits when its thread is
   about to exit, whether it finished, failed or timed out.
5. THE server SHALL count connections refused by each limit, readable
   through an accessor (Requirement 7.2).

### Requirement 5: No handler outlives `stop()`

**User Story:** As a developer embedding a server, I want `stop()` and the
destructor to guarantee that no connection thread still uses the server or
its handlers, so that shutting a node down cannot crash it.

#### Acceptance Criteria

1. WHEN `stop()` returns THEN no connection thread SHALL be running code
   that reads the server's members or calls a registered handler.
2. `stop()` SHALL unblock every connection in its request or reply phase
   (a blocking read, write or TLS handshake) promptly, without waiting for
   its deadline.
3. `stop()` SHALL wait for connections in the handler phase to return from
   the handler. It SHALL NOT interrupt a handler.
4. THE destructor SHALL give the same guarantee, since it calls `stop()`.
5. `stop()` SHALL remain idempotent and safe to call from any thread other
   than a connection thread of the same server.
6. A connection's thread SHALL close its own socket. `stop()` SHALL shut
   sockets down but SHALL NOT close them, so a descriptor number is never
   reused while a connection thread still holds it.

### Requirement 6: Configuration

**User Story:** As a developer, I want the limits in one struct with safe
defaults, so that existing callers get the protection without code changes
and tests can shrink the numbers.

#### Acceptance Criteria

1. THE system SHALL add `struct tcp_server_limits` with
   `request_timeout` (default 30 s), `reply_timeout` (default 30 s),
   `max_connections` (default 256) and `max_connections_per_source`
   (default 32).
2. `tcp_rpc_server` SHALL gain constructors taking a `tcp_server_limits`
   after the existing arguments. The existing constructors SHALL use the
   defaults.
3. `tls_tcp_rpc_config` SHALL gain a `tcp_server_limits server_limits{}`
   field. `tls_tcp_rpc_client` SHALL ignore it.
4. `tcp_gossip_config` SHALL gain a `tcp_server_limits listener_limits{}`
   field.
5. A zero timeout or a zero limit SHALL be rejected with
   `std::invalid_argument` at construction.

### Requirement 7: Bounded memory per connection and observability

**User Story:** As an operator, I want memory use to follow the bytes a
peer has actually sent, and I want to see how the limits are working.

#### Acceptance Criteria

1. THE frame readers (`tcp_detail::frame_recv()` and
   `tls_detail::frame_recv()`) SHALL grow their buffer as data arrives, in
   steps of at most 1 MiB, instead of allocating the announced length up
   front. The 64 MiB frame cap SHALL be unchanged.
2. EACH connection server SHALL expose a snapshot of
   `active_connections`, `refused_global_limit`, `refused_per_source_limit`,
   `timed_out` and `accept_errors`, through
   `auto connection_stats() const -> tcp_server_connection_stats`.

### Requirement 8: Tests and documentation

**User Story:** As a maintainer, I want each failure mode pinned by a test,
so that the fix cannot regress unnoticed.

#### Acceptance Criteria

1. Tests SHALL cover: a reply written to a reset connection with SIGPIPE at
   its default disposition; a silent connection and a trickling connection
   closed at `request_timeout`; both connection limits; the listener still
   serving after `EMFILE`; and `stop()` waiting for an in-flight handler and
   returning promptly while a connection sits in its request phase.
2. THE tests SHALL run for `tcp_rpc_server`, and the deadline, limit and
   `stop()` cases SHALL also run for `tls_tcp_rpc_server` and the gossip
   listener.
3. THE comment in `tls_tcp_rpc.hpp` saying plain TCP "has gotten away with
   the same omission" SHALL be replaced with a pointer to this spec.
4. `doc/CHANGELOG.md` SHALL record the new limits and their defaults. No
   transport README exists for the TCP or TLS-TCP transports today; the
   doc comment on `tcp_server_limits` is the reference.
