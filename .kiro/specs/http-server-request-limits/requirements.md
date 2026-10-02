# Requirements Document

## Introduction

The three HTTP Raft transports (cpp-httplib, Boost.Beast and Proxygen) share
one server config shape. Each has `max_request_body_size` (default 10 MiB)
and `max_concurrent_connections` (default 100). Only some of those fields do
anything:

| Field | cpp-httplib | Beast | Proxygen |
|---|---|---|---|
| `max_request_body_size` | enforced (`set_payload_max_length`, `http_transport_impl.hpp:1586,1616`) | enforced (`parser.body_limit`, `beast_http_transport_impl.hpp:1218`, 413 at `:1244`) | **never read** |
| `max_concurrent_connections` | **never read** | **never read** | **never read** |

Re-verified on `main` at `9c738d2` (2026-10-02). The parity audit
(`parallel-implementation-parity-audit.md`, findings T1 and T2) found the
same.

Two consequences follow:

1. **Proxygen has no body limit.** `rpc_request_handler::onBody`
   (`proxygen_http_transport_impl.hpp:1583-1588`) appends every chunk to a
   `std::vector<std::byte>` with no bound. Any peer that can reach the port
   can make the node allocate as much memory as it cares to send, before
   `dispatch()` gets a chance to reject the bytes. The same request against
   cpp-httplib or Beast is answered 413.
2. **No HTTP server limits connections.** The field exists on all three
   configs, `tests/http_config_test.cpp:56-103` checks only that the struct
   stores the value, and nothing reads it. The Proxygen header goes further
   and documents an enforcement that does not exist
   (`proxygen_http_transport.hpp:140-149`: "approximated by `proxygen_server`
   via its own accept-time counter that closes new connections past the
   limit"). Beast's `do_accept` (`beast_http_transport_impl.hpp:1796-1823`)
   starts a session for every accepted socket. cpp-httplib serves every
   connection from its default thread pool, whose size comes from
   `CPPHTTPLIB_THREAD_POOL_COUNT`, not from the config.

This spec makes both fields mean the same thing on all three transports, and
removes the false comment.

### Relationship to earlier specs

- `.kiro/specs/http-transport/` defined both fields for cpp-httplib.
- `.kiro/specs/boost-beast-http-transport/` Requirement 11.3 said a field
  with no native knob must document how the transport enforces it instead,
  "e.g. rejecting new connections past a live count, tracked explicitly".
  Beast documented nothing and tracks nothing.
- `.kiro/specs/proxygen-http-transport/` Requirement 11.3 repeated that
  rule. Its task 4 is checked off, and the comment it produced describes a
  counter that was never written.

This spec owns the enforcement. It does not reopen those specs' tasks; it
adds a note to each pointing here (task 7).

### Non-goals

- Client-side limits. `connection_pool_size` and `keep_alive_timeout` being
  unread on some clients (audit T15) is a separate gap.
- Per-peer or per-address limits, rate limiting, and slow-loris defence
  beyond the existing `request_timeout`.
- Response-size limits on the client.
- New metrics families. The audit's metrics gap (T12) has its own scope.
  Rejections are logged, and a counter is added only where the transport
  already emits server metrics through the same `metrics_type` (Requirement
  5).
- gRPC (`max_concurrent_rpcs` already works), TCP RPC and CoAP.

## Glossary

- **Body limit**: `max_request_body_size`, the largest request body in bytes
  a server reads before refusing the request.
- **Connection limit**: `max_concurrent_connections`, the largest number of
  accepted connections a server holds open at once, across every listener
  that one server object owns.
- **Live connection**: a TCP connection the server has accepted and not yet
  closed. A TLS connection is live from accept, before its handshake ends.
- **Listener**: one bound socket. A server bound to `*` has two (0.0.0.0 and
  ::), per `net_bind::resolve_bind_addresses()`.
- **Refused connection**: an accepted socket the server closes at once
  without reading from it, because the connection limit is reached.

## Requirements

### Requirement 1: Proxygen enforces the body limit

**User Story:** As an operator, I want a Proxygen node to refuse an oversized
request body the way the other two HTTP transports do, so that a peer cannot
exhaust the node's memory with one request.

#### Acceptance Criteria

1. WHEN a request declares a `Content-Length` greater than
   `max_request_body_size` THEN `proxygen_server` SHALL respond
   `413 Payload Too Large` from `onRequest`, before any body byte is
   buffered.
2. WHEN a request without a usable `Content-Length` (chunked, or an
   understated length) delivers body bytes whose running total exceeds
   `max_request_body_size` THEN `proxygen_server` SHALL stop buffering, free
   what it buffered, and respond 413 once.
3. WHEN a request is refused with 413 THEN `proxygen_server` SHALL NOT call
   `dispatch()` or any registered handler for it.
4. WHEN a 413 is sent THEN the response SHALL carry `Content-Type:
   text/plain` and `Connection: close`, and the server SHALL close the
   connection after the response, because unread body bytes on the wire
   would otherwise be parsed as the next request. This matches Beast
   (`beast_http_transport_impl.hpp:1238-1250`).
5. WHEN a request body is exactly `max_request_body_size` bytes THEN it
   SHALL be accepted. The limit is inclusive on all three transports.
6. WHEN body bytes arrive after the 413 was sent THEN `proxygen_server`
   SHALL discard them, and `onEOM` SHALL NOT send a second response.
7. WHEN `max_request_body_size` is 0 THEN every non-empty body SHALL be
   refused, the same as cpp-httplib and Beast. No value means "unlimited".

### Requirement 2: Every HTTP server enforces the connection limit

**User Story:** As an operator, I want `max_concurrent_connections` to cap
the open connections on whichever HTTP transport I pick, so that a burst of
connections cannot exhaust the node's threads, file descriptors or memory.

#### Acceptance Criteria

1. WHEN a server holds `max_concurrent_connections` live connections and
   accepts another THEN it SHALL close the new socket without reading from
   it or starting a TLS handshake, on cpp-httplib, Beast and Proxygen.
2. WHEN a live connection closes, for any reason (peer close, keep-alive
   expiry, `request_timeout`, error, 413) THEN the server SHALL release its
   slot, so a later connection is admitted.
3. WHEN one server object owns several listeners THEN the limit SHALL apply
   to their total, not to each listener.
4. WHEN a connection is refused THEN the server SHALL keep accepting: the
   accept loop SHALL NOT stop, block, or spin.
5. WHEN `max_concurrent_connections` is 0 THEN the server SHALL refuse to
   start with `std::invalid_argument`, since such a server could never serve
   a request. This is checked when the server is constructed, alongside the
   existing TLS validation.
6. WHEN `stop()` returns THEN the counter SHALL no longer be touched by any
   connection still being torn down. No slot release may run against a
   destroyed server.
7. WHEN the limit is not reached THEN request handling, keep-alive and TLS
   behaviour SHALL be unchanged from today.

### Requirement 3: Defaults keep a normal cluster working

**User Story:** As a developer, I want the default limit to admit every
connection a healthy cluster opens, so that turning enforcement on does not
cause failed RPCs or elections.

#### Acceptance Criteria

1. The defaults SHALL stay `max_request_body_size = 10 MiB` and
   `max_concurrent_connections = 100`. Changing them is a separate decision.
2. The design SHALL state how many inbound connections one node receives
   from each client transport (peers times pooled connections per peer) and
   show the default covers the largest configuration the repository's tests,
   examples and benchmarks use.
3. WHEN a client's connection is refused THEN the client SHALL see an
   ordinary connect or read failure that its existing retry handling treats
   as transient. No client change is required.
4. Every existing HTTP transport test, interop test and example SHALL pass
   unchanged with enforcement on.

### Requirement 4: Accurate documentation

**User Story:** As a developer reading a config struct, I want each field's
comment to say what the field does on that transport, so that I can rely on
it.

#### Acceptance Criteria

1. The `max_concurrent_connections` and `max_request_body_size` fields on
   `cpp_httplib_server_config`, `boost_beast_server_config` and
   `proxygen_server_config` SHALL each carry a comment saying how that
   transport enforces the field and what a client sees when it trips.
2. The false "accept-time counter" paragraph in
   `proxygen_http_transport.hpp:140-149` SHALL be replaced with a description
   of the mechanism this spec adds.
3. `doc/` pages that describe HTTP server configuration SHALL describe both
   limits the same way for all three transports.

### Requirement 5: Observability

**User Story:** As an operator, I want to know when a node refuses requests
or connections, so that I can tell an attack or a mis-sized limit from a
network fault.

#### Acceptance Criteria

1. WHEN a request is refused with 413 or a connection is refused at the
   limit THEN the server SHALL log once at warning level with the peer
   address and the limit. Connection refusals SHALL be rate-limited to one
   log line per second per server, so a flood cannot flood the log.
2. WHERE a transport already emits server-side metrics through
   `metrics_type` THEN it SHALL also emit `http.server.request_too_large` and
   `http.server.connection_refused` counters, with a `transport` dimension.
   A transport with no server metrics today gets the log line only.

### Requirement 6: Tests

**User Story:** As a maintainer, I want behavioural tests for both limits on
all three transports, so that the next refactor cannot quietly drop one.

#### Acceptance Criteria

1. Each transport SHALL have a test that sends a body one byte over the
   limit with a `Content-Length` and asserts a 413 on the wire, read through
   a raw socket rather than the Raft client, so the status code is checked
   and not just "the RPC failed".
2. Proxygen SHALL also have a test for the chunked case (Requirement 1.2) and
   for a body exactly at the limit (Requirement 1.5).
3. Each transport SHALL have a test that, with `max_concurrent_connections =
   2`, holds two idle connections open, sees a third closed by the server
   without a response, closes one of the first two, and then completes an
   RPC on a new connection.
4. Each transport SHALL have a test that the limit is shared across the two
   listeners of a `*` bind (Requirement 2.3), where the test environment has
   IPv6 loopback; otherwise the test skips with a message.
5. A cross-transport test SHALL send the same oversized request to all three
   servers and assert the same status and `Content-Type`.
6. The existing struct-only checks in `tests/http_config_test.cpp` SHALL
   stay, and the new behavioural tests SHALL be registered in CMake under
   the same Kconfig gates as each transport's existing tests.
