# Requirements Document

## Introduction

This specification defines the requirements for a **second** HTTP transport
implementation, backed by [Boost.Beast](https://www.boost.org/doc/libs/release/libs/beast/),
alongside the existing cpp-httplib-backed implementation
(`.kiro/specs/http-transport/`, `include/raft/http_transport.hpp`,
`include/raft/http_transport_impl.hpp`). It satisfies the same
`kythira::network_client`/`kythira::network_server` concepts
(`include/raft/network.hpp`) and the same `kythira::transport_types` concept
(`include/raft/types.hpp`, shared with the CoAP transport) as the existing
implementation, so it is a drop-in alternative at the `Types` template
parameter — not a replacement, and not a change to which transport any
existing call site uses.

This mirrors the precedent already established for Future backends
(`.kiro/specs/stdexec-future-backend/`, `.kiro/specs/boost-future-backend/`):
add a second, independent implementation of an existing concept-defined
interface, without touching any call site that already works, gated behind
its own opt-in build configuration.

**Why a second HTTP transport at all.** cpp-httplib is a synchronous,
blocking-call library: `cpp_httplib_client` issues one blocking HTTP request
per RPC (parallelism comes from the caller submitting work onto
`executor_type` — see `include/raft/executor_default.hpp` — not from the
transport itself), and `cpp_httplib_server` runs its own internal
accept-and-dispatch loop on a dedicated thread
(`include/raft/http_transport.hpp`'s `cpp_httplib_server::_server_thread`).
This is simple and has worked well for this project's actual production RPC
path, which in practice runs over `include/raft/tcp_rpc.hpp`/
`tls_tcp_rpc.hpp` (raw TCP, not HTTP) rather than the HTTP transport — HTTP
transport today is used in the one example
(`examples/raft/http_transport_example.cpp`) and its own test suite, not by
`cmd/ca_service`/`cmd/ca_cluster_node`. Boost.Beast, built directly on
Boost.Asio (already a required vcpkg dependency of this project, per
`vcpkg.json`), offers genuine asynchronous I/O: many in-flight requests
sharing a small number of OS threads via an `io_context` event loop, instead
of one blocking call (or one server thread) per connection. That is the
actual reason to add it — not a stylistic preference for a different
Boost library.

This feature does not remove cpp-httplib, does not convert any existing
production call site, does not change which transport `cmd/ca_service`,
`cmd/ca_cluster_node`, or any existing example uses by default, and does not
modify the `network_client`/`network_server`/`transport_types` concepts
themselves.

## Glossary

- **Boost.Beast**: The `boost-beast` vcpkg package (not currently a
  dependency of this project — confirmed by inspection of `vcpkg.json` while
  writing this document; `boost-asio`, which Beast is built on, already is),
  providing HTTP/1.1 (and WebSocket, out of scope here — see Requirement 15)
  message parsing/serialization layered directly on `boost::asio` streams.
- **`net::io_context`**: Boost.Asio's event loop/executor (`boost::asio::io_context`,
  aliased as `net::io_context` in Beast's own examples and this document) —
  the thing whose `run()` must be called on one or more threads for any
  asynchronous Beast operation to actually make progress.
- **`beast::tcp_stream`**: Beast's wrapper around `net::ip::tcp::socket` that
  adds per-operation timeouts (`expires_after`) — Beast's replacement for
  cpp-httplib's `connection_timeout`/`request_timeout` config fields, but
  implemented as a stream-level primitive rather than a client-level config
  struct.
- **`beast::ssl_stream<beast::tcp_stream>`**: Beast's TLS-wrapped stream,
  layered on the same `boost::asio::ssl::stream` template this project
  already depends on transitively (`OpenSSL::SSL`/`OpenSSL::Crypto`, per
  root `CMakeLists.txt`) — not a new TLS library, a new way of driving the
  same OpenSSL contexts this project's `cpp_httplib_client`/
  `cpp_httplib_server`/`tls_tcp_rpc.hpp` already configure.
- **`beast::flat_buffer`**: Beast's growable byte buffer used to accumulate
  partial reads across `async_read`/`read` calls until a complete HTTP
  message is parsed.
- **`beast::http::request<Body>`/`beast::http::response<Body>`**: Beast's
  HTTP message types, templated on a `Body` type (this feature uses
  `beast::http::string_body`, matching this project's existing
  `std::vector<std::byte>`-oriented `rpc_serializer` concept, which already
  hands transports pre-serialized byte buffers rather than requiring
  transport-level knowledge of the wire format).
- **Synchronous (blocking) Beast API**: `beast::http::read`/`write`
  free functions operating directly on a stream, one call per message,
  blocking the calling thread until complete — Beast's alternative to its
  own `async_read`/`async_write`, offered on every stream type Beast
  provides.
- **Asynchronous Beast API**: `beast::http::async_read`/`async_write`,
  composed via completion handlers/coroutines, driven by an `io_context`
  event loop — the model this feature actually adopts (Requirement 14),
  distinct from every other transport already in this codebase (see that
  requirement's rationale).
- **`transport_types`**: The existing concept (`include/raft/types.hpp`)
  already shared between the HTTP and CoAP transports, requiring
  `serializer_type`, `metrics_type`, `executor_type`, and a
  `future_template<T>` member template whose instantiations satisfy
  `kythira::future`. This feature adds no new members to this concept and
  no new concrete `Types` bundle is strictly required — any existing
  `transport_types`-satisfying bundle (e.g. `kythira::http_transport_types`)
  works unchanged, since Beast's own dependency (an `io_context` reference)
  is a *construction-time* concern (Requirement 8), not something the
  `Types` bundle itself needs to carry.
- **RPC endpoint paths**: `/v1/raft/request_vote`, `/v1/raft/append_entries`,
  `/v1/raft/install_snapshot` — identical to the existing cpp-httplib
  transport (`.kiro/specs/http-transport/` Requirements 3-5), not
  reinvented.

## Requirements

### Requirement 1

**User Story:** As a distributed systems developer, I want a Beast-backed
`network_client` implementation, so that I can send Raft RPCs over HTTP
using genuinely asynchronous I/O instead of one blocking call per request.

#### Acceptance Criteria

1. WHEN `boost_beast_client<Types>` is instantiated with a `Types` that
   satisfies `kythira::transport_types` THEN the system SHALL provide
   `send_request_vote`, `send_append_entries`, and `send_install_snapshot`
   methods matching `kythira::network_client`'s exact signatures
   (`include/raft/network.hpp`)
2. WHEN `boost_beast_client<Types>` is evaluated against the
   `kythira::network_client` concept THEN it SHALL satisfy the concept
   without any modification to the concept itself
3. WHEN `boost_beast_client<Types>` is constructed THEN the system SHALL
   accept the same node-id-to-URL mapping shape
   (`std::unordered_map<std::uint64_t, std::string>`) as
   `cpp_httplib_client`, so existing cluster-configuration code that builds
   this map is transport-agnostic
4. WHEN a request is sent THEN the system SHALL serialize it using
   `Types::serializer_type` (the templated `rpc_serializer`), identical to
   the existing cpp-httplib transport's serialization step — this feature
   does not introduce a second serialization mechanism

### Requirement 2

**User Story:** As a distributed systems developer, I want a Beast-backed
`network_server` implementation, so that I can receive Raft RPCs over HTTP
using genuinely asynchronous I/O instead of a dedicated thread per accepted
connection.

#### Acceptance Criteria

1. WHEN `boost_beast_server<Types>` is instantiated with a `Types` that
   satisfies `kythira::transport_types` THEN the system SHALL provide
   `register_request_vote_handler`, `register_append_entries_handler`,
   `register_install_snapshot_handler`, `start`, `stop`, and `is_running`
   methods matching `kythira::network_server`'s exact signatures
2. WHEN `boost_beast_server<Types>` is evaluated against the
   `kythira::network_server` concept THEN it SHALL satisfy the concept
   without any modification to the concept itself
3. WHEN `boost_beast_server<Types>` is constructed THEN the system SHALL
   accept the same `(bind_address, bind_port)` shape as `cpp_httplib_server`
4. WHEN a request arrives at `/v1/raft/request_vote`,
   `/v1/raft/append_entries`, or `/v1/raft/install_snapshot` THEN the system
   SHALL route it to the correspondingly-registered handler, matching the
   existing cpp-httplib transport's endpoint routing
   (`.kiro/specs/http-transport/` Requirements 6-8) exactly

### Requirement 3

**User Story:** As a Raft node, I want RequestVote/AppendEntries/
InstallSnapshot RPCs sent over the Beast transport to behave identically, in
externally observable terms, to the same RPCs sent over the existing
cpp-httplib transport, so that the choice of transport is invisible to Raft
core logic.

#### Acceptance Criteria

1. WHEN `send_request_vote`/`send_append_entries`/`send_install_snapshot`
   is called on `boost_beast_client<Types>` THEN the system SHALL POST to
   the same endpoint path the cpp-httplib transport uses for that RPC
2. WHEN the HTTP request succeeds with status 200 THEN the system SHALL
   deserialize the response body using `Types::serializer_type` and
   complete the returned `Types::future_template<Response>` with the
   deserialized value
3. WHEN the HTTP request fails (connection refused, DNS failure, TLS
   handshake failure, or timeout — Requirement 10) THEN the system SHALL
   complete the returned future with an exception, matching the existing
   cpp-httplib transport's failure-to-exception mapping in kind (an
   exception the caller can catch), not necessarily the same concrete
   exception type
4. WHEN two Beast-backed RPCs to different target nodes are in flight
   concurrently on the same `boost_beast_client<Types>` instance THEN the
   system SHALL complete both correctly and independently — this is
   Requirement 14's actual payoff: unlike `cpp_httplib_client`, where
   concurrency comes entirely from the caller's own executor submitting
   multiple blocking calls in parallel, `boost_beast_client` achieves this
   through the `io_context` event loop itself

### Requirement 4

**User Story:** As a Raft server, I want the Beast transport's server side
to handle RequestVote/AppendEntries/InstallSnapshot RPCs with the same
request/response/error-status semantics as the existing cpp-httplib
transport, so that clients cannot distinguish which server-side transport
they are talking to.

#### Acceptance Criteria

1. WHEN a POST request arrives at a registered RPC endpoint THEN the system
   SHALL deserialize the request body using `Types::serializer_type`,
   invoke the registered handler, serialize its response, and reply with
   HTTP status 200 — identical externally observable behavior to
   `.kiro/specs/http-transport/` Requirements 6-8
2. WHEN the request body fails to deserialize THEN the system SHALL reply
   with HTTP status 400, matching `.kiro/specs/http-transport/` Requirement
   13.1-13.2
3. WHEN the registered handler throws THEN the system SHALL reply with HTTP
   status 500, matching `.kiro/specs/http-transport/` Requirement 13.3
4. WHEN a request arrives for an unregistered/unrecognized path THEN the
   system SHALL reply with an appropriate non-200 status (404), a case the
   existing cpp-httplib transport's own request routing already handles via
   `httplib::Server`'s default behavior and this feature must handle
   explicitly, since Beast provides no built-in path router

### Requirement 5

**User Story:** As a system operator, I want the Beast server's lifecycle
(`start`/`stop`/`is_running`) to behave like the existing cpp-httplib
server's, so that operational tooling built against one transport works
against the other without changes.

#### Acceptance Criteria

1. WHEN `start()` is called THEN the system SHALL bind the configured
   address/port, begin accepting connections, and return once the listener
   is actually accepting (not merely once a background thread has been
   spawned) — matching `.kiro/specs/http-transport/` Requirement 9.1-9.2's
   observable contract
2. WHEN `stop()` is called THEN the system SHALL stop accepting new
   connections and SHALL allow in-flight requests to complete before the
   call returns, matching Requirement 9.3-9.4 of the existing spec — for
   Beast specifically, this means closing the listening acceptor and
   waiting (e.g. via a condition variable, not by touching the
   caller-owned `io_context` itself — Requirement 8.3) for the last
   in-flight session's completion handler to run, not merely closing the
   acceptor and returning immediately while sessions are still in flight
3. WHEN `is_running()` is called THEN the system SHALL report whether the
   server is currently accepting connections
4. WHEN `start()` is called on an already-running server, or `stop()` on an
   already-stopped one THEN the system SHALL behave safely (no crash, no
   double-bind, no double-join) — matching the defensive behavior already
   expected of `cpp_httplib_server`

### Requirement 6

**User Story:** As a security-conscious operator, I want the Beast
transport to support HTTPS with the same certificate/cipher/TLS-version
configuration surface as the existing cpp-httplib transport, so that
choosing Beast does not mean giving up TLS features already relied on in
production.

#### Acceptance Criteria

1. WHEN the Beast client is configured with an `https://` URL THEN the
   system SHALL establish the connection over `beast::ssl_stream<beast::tcp_stream>`
   rather than a plain `beast::tcp_stream`
2. WHEN the Beast server is configured with `enable_ssl=true` THEN the
   system SHALL accept connections over `beast::ssl_stream<beast::tcp_stream>`
3. WHEN establishing a TLS connection THEN the system SHALL validate the
   peer certificate against the configured CA, matching
   `.kiro/specs/http-transport/` Requirement 10.3-10.4's observable
   behavior (rejected connection, future/accept completes with an error)
4. WHEN SSL certificate/key file paths are configured THEN the system SHALL
   load them into a `boost::asio::ssl::context`, reusing the identical
   config field names and semantics as `cpp_httplib_client_config`/
   `cpp_httplib_server_config` (`ca_cert_path`, `client_cert_path`/
   `client_key_path`, `ssl_cert_path`/`ssl_key_path`, `cipher_suites`,
   `min_tls_version`/`max_tls_version`, `require_client_cert`) — a Beast
   config struct with the same fields, not a redesigned configuration
   surface, so switching a call site between transports is a type change,
   not a config-rewrite
5. WHEN client certificate authentication is required (server-side
   `require_client_cert=true`) THEN the system SHALL configure the
   `boost::asio::ssl::context` to request and verify a client certificate,
   matching `.kiro/specs/http-transport/` Requirement 10.10-10.11
6. WHEN TLS material is invalid or missing at configuration time THEN the
   system SHALL report a clear, specific error (which file, what went
   wrong), matching Requirement 10.12/10.14 of the existing spec, not a
   generic OpenSSL error code surfaced unmodified

### Requirement 7

**User Story:** As a system operator managing certificate rotation, I want
the Beast transport to support reloading TLS material without dropping
established connections, matching the existing cpp-httplib transport's
`reload_tls_material()`/`enable_auto_reload()` behavior.

#### Acceptance Criteria

1. WHEN `reload_tls_material()` is called on `boost_beast_client<Types>`
   THEN the system SHALL validate the new certificate/key material first
   (all-or-nothing) and, on success, apply it to a fresh
   `boost::asio::ssl::context` used for subsequently-established
   connections, matching `cpp_httplib_client::reload_tls_material()`'s
   documented behavior — retired contexts/streams are kept alive rather
   than destroyed, since an in-flight RPC's `beast::ssl_stream` may still
   reference the old context
2. WHEN `reload_tls_material()` is called on `boost_beast_server<Types>`
   THEN the system SHALL apply the new material to the live SSL context
   without closing the listening acceptor or dropping established
   connections, matching `cpp_httplib_server::reload_tls_material()`'s
   documented behavior
3. WHEN `reload_tls_material()` fails validation THEN the system SHALL
   throw and leave the previous, still-valid material in effect, matching
   the existing transport's all-or-nothing contract
4. WHEN `enable_auto_reload(poll_interval)` is called THEN the system SHALL
   start a background polling loop that calls `reload_tls_material()`
   whenever the configured certificate file's mtime changes, matching the
   existing transport's polling-based (not filesystem-event-based)
   approach
5. WHEN `disable_auto_reload()` is called THEN the system SHALL stop the
   polling loop cleanly (joined, not detached), matching the existing
   transport's shutdown contract

### Requirement 8

**User Story:** As a developer integrating the Beast transport, I want
explicit control over the `io_context` and the threads running it, so that
I can share a single I/O thread pool across multiple Beast client/server
instances rather than paying a dedicated thread pool per instance.

#### Acceptance Criteria

1. WHEN `boost_beast_client<Types>`/`boost_beast_server<Types>` is
   constructed THEN the system SHALL accept a reference to a caller-owned
   `net::io_context` rather than constructing and owning one internally —
   the `io_context`'s lifetime and thread pool are the caller's
   responsibility, not the transport's, so one `io_context` (and its
   thread(s) calling `run()`) can be shared across a client, a server, and
   any other Beast-based component in the same process
2. WHEN no threads are calling `run()` on the provided `io_context` THEN
   the system SHALL make no progress on any in-flight operation (both
   Requirement 3's futures and Requirement 5's server lifecycle) — this is
   the expected, documented consequence of Requirement 8.1's design and
   SHALL be stated explicitly in this feature's own documentation
   (Requirement 18), not left to be discovered by a caller who forgot to
   run the `io_context`
3. WHEN `stop()` is called on `boost_beast_server<Types>` THEN the system
   SHALL NOT itself stop or destroy the caller-provided `io_context` — only
   its own listener/connections, since the same `io_context` may be serving
   other work the caller still needs running

### Requirement 9

**User Story:** As a developer, I want connection reuse for the Beast
client, so that repeated RPCs to the same target node do not pay a fresh
TCP/TLS handshake every time, matching the existing cpp-httplib transport's
connection pooling.

#### Acceptance Criteria

1. WHEN `send_request_vote`/`send_append_entries`/`send_install_snapshot`
   is called for a target node with no existing open connection THEN the
   system SHALL establish one and reuse it for subsequent requests to that
   same node
2. WHEN an established connection to a target node is idle beyond a
   configured timeout THEN the system SHALL close it, matching
   `.kiro/specs/http-transport/` Requirement 11.4's behavior
3. WHEN a pooled connection is found to be broken (peer closed, network
   error) on the next request THEN the system SHALL transparently
   establish a fresh connection rather than surfacing the staleness as a
   caller-visible error
4. WHEN connection pool size is configured (matching
   `cpp_httplib_client_config::connection_pool_size`) THEN the system SHALL
   respect it as an upper bound on simultaneously-open connections

### Requirement 10

**User Story:** As a distributed systems developer, I want configurable
timeouts for both connection establishment and full request completion, so
that a slow or unresponsive peer cannot block a Raft RPC indefinitely.

#### Acceptance Criteria

1. WHEN `send_request_vote`/`send_append_entries`/`send_install_snapshot`
   is called with a `timeout` parameter THEN the system SHALL enforce it as
   the deadline for the *entire* RPC (connect + write + read), using
   `beast::tcp_stream::expires_after`, not a per-`async_read`/
   `async_write`-call timeout that could be individually satisfied while
   the RPC as a whole overruns
2. WHEN the timeout elapses before the RPC completes THEN the system SHALL
   cancel the in-flight operation and complete the returned future with a
   timeout-specific exception, matching
   `.kiro/specs/http-transport/` Requirement 12.4-12.5's observable
   behavior
3. WHEN `connection_timeout` (matching the existing config field) is
   configured separately from the per-RPC `timeout` parameter THEN the
   system SHALL apply it specifically to the connect step, for cases where
   a caller wants a tighter bound on "can I even reach this node" than on
   "how long until I get a response"

### Requirement 11

**User Story:** As a developer, I want the same configuration surface
(client and server config structs) as the existing cpp-httplib transport,
so that switching a call site between transports requires changing a type,
not rewriting configuration.

#### Acceptance Criteria

1. WHEN a Beast client config struct is defined THEN the system SHALL
   include every field `cpp_httplib_client_config` has (Requirement 6.4,
   plus `connection_pool_size`, `connection_timeout`, `request_timeout`,
   `keep_alive_timeout`, `user_agent`), with identical names, types, and
   default values
2. WHEN a Beast server config struct is defined THEN the system SHALL
   include every field `cpp_httplib_server_config` has
   (`max_concurrent_connections`, `max_request_body_size`,
   `request_timeout`, plus the TLS fields from Requirement 6.4), with
   identical names, types, and default values
3. WHEN a config field has no Beast-specific meaning (e.g.
   `max_concurrent_connections`, which cpp-httplib enforces via its own
   internal thread pool sizing) THEN the system SHALL document, in that
   field's own comment, how Beast enforces the equivalent behavior instead
   (e.g. rejecting new connections past a live count, tracked explicitly,
   since an `io_context`-based acceptor has no built-in connection cap)

### Requirement 12

**User Story:** As a system operator, I want the Beast transport to emit
the same category of metrics as the existing cpp-httplib transport, so that
dashboards/alerts built against one transport's metrics work against the
other.

#### Acceptance Criteria

1. WHEN `boost_beast_client<Types>`/`boost_beast_server<Types>` is
   constructed THEN the system SHALL accept a `Types::metrics_type`
   instance, identical to the existing transport's constructor shape
2. WHEN a request is sent (client) or received (server) THEN the system
   SHALL emit metrics for request count, latency, and size, matching
   `.kiro/specs/http-transport/` Requirement 16.3-16.4
3. WHEN an error occurs THEN the system SHALL emit error metrics with a
   type distinguishing at least: connection failure, TLS failure, timeout,
   and deserialization failure — matching Requirement 16.5's intent, with
   categories Beast's own error surface (`boost::system::error_code`,
   `beast::error`) makes straightforward to distinguish
4. WHEN connection pool operations occur (Requirement 9) THEN the system
   SHALL emit connection lifecycle metrics, matching Requirement 16.6
5. WHEN server lifecycle events occur (Requirement 5) THEN the system
   SHALL emit start/stop metrics, matching Requirement 16.7

### Requirement 13

**User Story:** As a developer, I want the Beast transport to work with any
existing `transport_types`-satisfying `Types` bundle without requiring a
new one, so that adopting Beast is a template-argument change, not a
type-system migration.

#### Acceptance Criteria

1. WHEN `boost_beast_client<Types>`/`boost_beast_server<Types>` is defined
   THEN the system SHALL constrain `Types` with `requires kythira::transport_types<Types>`,
   identical to `cpp_httplib_client`/`cpp_httplib_server`'s existing
   constraint
2. WHEN an existing `Types` bundle such as `kythira::http_transport_types`
   (`include/raft/http_transport.hpp`) is used to instantiate
   `boost_beast_client`/`boost_beast_server` THEN the system SHALL compile
   and behave correctly without any change to that `Types` bundle
3. WHEN `kythira::transport_types` itself is examined THEN the system SHALL
   NOT modify it — this feature adds a new *consumer* of the concept, not a
   new *requirement* on it
4. WHEN generic code already written against `network_client`/
   `network_server` (e.g. anything templated the way
   `.kiro/specs/http-transport/` Requirement 1.3 describes) is instantiated
   with `boost_beast_client`/`boost_beast_server` in place of
   `cpp_httplib_client`/`cpp_httplib_server` THEN the system SHALL require
   no changes to that generic code

### Requirement 14

**User Story:** As a library maintainer, I want the choice between Beast's
synchronous and asynchronous APIs made deliberately and documented, not
assumed, since this project has no existing precedent for the
`io_context`-driven async model and every other transport in this codebase
(`cpp_httplib_client`/`server`, `include/raft/tcp_rpc.hpp`/`tls_tcp_rpc.hpp`)
uses blocking I/O dispatched onto `executor_type` instead.

#### Acceptance Criteria

1. WHEN this feature is implemented THEN the system SHALL use Beast's
   asynchronous API (`async_read`/`async_write`/`async_connect`/
   `async_accept`, composed via completion handlers or
   `boost::asio::awaitable` coroutines — the specific composition style is
   an implementation detail the design document, not this requirements
   document, SHALL settle) rather than Beast's synchronous API — using the
   synchronous API on an executor-submitted blocking call, mirroring
   `cpp_httplib_client`'s existing pattern, would be simpler to implement
   but would forfeit the actual reason (stated in this document's
   Introduction) to add Beast as a second transport at all
2. WHEN an asynchronous Beast operation (`async_connect`, `async_write`,
   `async_read`, individually) completes THEN the system SHALL bridge that
   single operation's completion into fulfilling a
   `kythira::promise_default<T>` (`setValue`/`setException` called from
   inside the completion handler, the same fulfillment pattern
   `include/raft/tcp_rpc.hpp` already uses for its own executor-submitted
   completions, adapted to fire from an `io_context` thread instead) —
   NOT the whole connect+write+read sequence bridged as one monolithic
   completion handler per RPC
3. WHEN the per-operation futures from Acceptance Criterion 2 need to be
   sequenced (connect, then write, then read, then deserialize) THEN the
   system SHALL compose them using `future_transformable`'s `thenValue`/
   `thenError` (`include/concepts/future.hpp`), already implemented by
   every backend, rather than hand-nesting completion handlers or
   callbacks — each connect/write/read primitive is a small,
   independently testable adaptor function (`async_connect_kf`,
   `async_write_kf`, `async_read_kf` — Requirement 19 governs the concrete
   type these adaptors return), and an RPC's full send path is the chain of
   `.thenValue(...)` calls over them, not a bespoke state machine per RPC
   method
4. WHEN multiple threads call `io_context::run()` concurrently
   (Requirement 8.1) THEN the system SHALL be safe under that concurrency —
   no data race on shared client/server state, no double-fulfillment of a
   promise from two threads racing on the same completion
5. WHEN this design decision is documented THEN the system SHALL record,
   in `design.md`, the concrete alternative considered (synchronous API on
   `executor_type`) and why it was rejected, following this project's
   existing convention of justifying non-obvious API-shape decisions
   in-document rather than only in a commit message (see
   `.kiro/specs/boost-future-backend/design.md`'s "Why a third namespace"
   section for the precedent this follows)

### Requirement 15

**User Story:** As a developer, I want the scope of this feature stated
explicitly, so that expectations about what "Boost.Beast HTTP transport"
does and does not include are accurate.

#### Acceptance Criteria

1. WHEN documentation for this feature is written THEN the system SHALL
   state explicitly that no existing production call site
   (`cmd/ca_service`, `cmd/ca_cluster_node`, any existing example) is
   converted from cpp-httplib to Beast by this feature
2. WHEN documentation for this feature is written THEN the system SHALL
   state explicitly that cpp-httplib is not removed, deprecated, or made
   optional-only as a result of this feature
3. WHEN documentation for this feature is written THEN the system SHALL
   state explicitly that Beast's WebSocket support
   (`boost::beast::websocket`) is out of scope — this feature only uses
   Beast's HTTP/1.1 message layer
4. WHEN documentation for this feature is written THEN the system SHALL
   state explicitly that HTTP/2 is out of scope (Beast itself does not
   implement HTTP/2; cpp-httplib does not either — parity, not a
   regression)
5. WHEN documentation for this feature is written THEN the system SHALL
   state explicitly that this feature does not touch
   `include/raft/tcp_rpc.hpp`/`tls_tcp_rpc.hpp` (the actual production RPC
   transport) or the CoAP transport, even though both share infrastructure
   (`transport_types`, `executor_default`) with this feature
6. WHEN documentation for this feature is written THEN the system SHALL
   provide a short example (modeled on
   `examples/raft/http_transport_example.cpp`) showing `boost_beast_client`/
   `boost_beast_server` instantiated side by side with the existing
   `cpp_httplib_client`/`cpp_httplib_server` for the same `Types` bundle

### Requirement 16

**User Story:** As a developer, I want the Beast transport validated with
the same rigor as the existing cpp-httplib transport and the existing
Future-backend precedents, so that I can trust its correctness and
concept compliance before it is used anywhere real.

#### Acceptance Criteria

1. WHEN `boost_beast_client<Types>`/`boost_beast_server<Types>` are
   compiled THEN the system SHALL `static_assert` their compliance with
   `kythira::network_client`/`kythira::network_server`, mirroring the
   assertion pattern already used elsewhere in this codebase for
   concept-constrained types
2. WHEN the existing `tests/http_client_*`/`tests/http_server_*`/
   `tests/http_integration_test.cpp`/`tests/http_ssl_*` test files are used
   as a template THEN the system SHALL add a parallel `tests/beast_*` suite
   covering the same behaviors for the Beast transport — request/response
   round-trips, TLS (mutual and server-only), timeout enforcement,
   connection pooling, malformed-request handling, and concurrent RPC
   correctness (Requirement 3.4)
3. WHEN property-based tests are written for this feature THEN the system
   SHALL follow this project's existing property-test tagging convention
   (`**Feature: boost-beast-http-transport, Property N: ...**`) and
   `BOOST_AUTO_TEST_CASE` timeout requirements
4. WHEN the cpp-httplib and Beast transports are both exercised against
   the same `Types` bundle and the same RPC sequence THEN the system SHALL
   produce equivalent externally-observable results (response content,
   error classification into the same status-code categories), even though
   internal I/O mechanics differ — mirroring
   `.kiro/specs/boost-future-backend/` Requirement 10.4's
   cross-implementation equivalence testing precedent
5. WHEN the Beast transport test suite is run THEN the system SHALL
   execute exclusively through CTest, per this project's test execution
   standards, labeled `beast-http` so `ctest -L beast-http` can run just
   this suite

### Requirement 17

**User Story:** As a developer, I want the `boost-beast` dependency added
using this project's existing optional-dependency machinery, so that
building without it degrades the same way every other optional dependency
already does.

#### Acceptance Criteria

1. WHEN `vcpkg.json` is updated THEN the system SHALL add `boost-beast` as
   a dependency (header-only; no new library artifact to link, only new
   headers and a `find_package`/target to depend on)
2. WHEN root `CMakeLists.txt` is updated THEN the system SHALL gate
   Beast-transport targets using `kythira_find_optional`/
   `kythira_kconfig_gate` (`cmake/Kconfig.cmake`), the same mechanism every
   other optional dependency in this project already uses, rather than a
   bespoke `if(...)` check
3. WHEN a Kconfig symbol for this feature is added THEN the system SHALL
   follow the existing "Futures" menu's precedent (`Kconfig`,
   `CONFIG_STDEXEC_BACKEND`/`CONFIG_BOOST_FUTURE_BACKEND`) for how an
   optional, non-default capability is exposed — a `CONFIG_BOOST_BEAST_TRANSPORT`
   boolean, default `n`, with help text stating the dependency and linking
   to this spec directory
4. WHEN `boost-beast` is unavailable (not installed, or
   `CONFIG_BOOST_BEAST_TRANSPORT=n`) THEN the system SHALL skip the
   Beast-transport targets/tests with a `message(STATUS ...)`, matching
   this project's established graceful-degradation convention (see
   `doc/TODO.md`'s Folly-decoupling entries for the current worked example
   of this exact pattern, including the pitfall of a subdirectory-level
   gate being more conservative than necessary — this feature's own gating
   SHALL be scoped per-target from the start, not added as a later
   follow-up)

### Requirement 18

**User Story:** As a developer, I want a pre-implementation spike that
confirms Beast's actual API shape against the specific version this project
will vendor, so that the design does not assume API stability or behavior
Beast itself does not guarantee or that differs from what its
documentation-only description suggests.

#### Acceptance Criteria

1. WHEN implementation begins THEN the system SHALL first record, in a
   `spike-notes.md` in this spec's directory (mirroring
   `.kiro/specs/stdexec-future-backend/spike-notes.md` and
   `.kiro/specs/boost-future-backend/spike-notes.md`'s precedent), the
   exact `boost-beast`/`boost-asio` version vcpkg resolves for this
   project's `builtin-baseline` (`vcpkg.json`)
2. WHEN the spike is performed THEN the system SHALL confirm empirically
   (a throwaway compile-and-run against a real socket, not documentation
   reading alone) that `beast::tcp_stream::expires_after` actually cancels
   an in-flight `async_read`/`async_write` the way Requirement 10.1-10.2
   assumes, since Beast's own documentation describes this as
   best-effort/OS-dependent for already-issued syscalls
3. WHEN the spike is performed THEN the system SHALL confirm whether this
   project's existing OpenSSL context-configuration code (certificate
   loading, cipher suite restriction, min/max TLS version — currently
   written against `httplib::SSLServer`'s and `tls_tcp_rpc.hpp`'s own
   direct `SSL_CTX`-level calls) can be reused as-is against
   `boost::asio::ssl::context`, or needs an adaptation layer, and record
   which
4. WHEN the spike is performed THEN the system SHALL confirm which
   coroutine/completion-handler composition style
   (`boost::asio::awaitable<T>` + `co_spawn`, vs. stackless
   `boost::asio::coroutine`, vs. plain nested completion-handler callbacks)
   compiles cleanly under this project's existing C++23 + Clang 16+/GCC 13+
   compiler matrix (root `CMakeLists.txt`), since `awaitable<T>` coroutine
   support quality has historically varied across compiler versions more
   than plain callback composition does
5. WHEN the spike concludes THEN the system SHALL record the specific
   Boost release and minimum compiler versions validated, matching
   `.kiro/specs/stdexec-future-backend/` Requirement 13.5 and
   `.kiro/specs/boost-future-backend/` Requirement 9.4's precedent

### Requirement 19

**User Story:** As a developer relying on `future_transformable`'s
`thenValue`/`thenError` chaining to compose the Beast async bridge
(Requirement 14.3), I want a concrete, well-defined constraint on which
`transport_types`-satisfying `Types` bundles this feature actually
supports, so that the internal `Promise`/`Future` pairing used by the
chain is guaranteed to match the public `Types::future_template<T>` return
type the caller sees.

#### Acceptance Criteria

1. WHEN `boost_beast_client<Types>`/`boost_beast_server<Types>` is
   instantiated THEN the system SHALL require
   `Types::template future_template<T>` to be `kythira::future_default<T>`
   for every `T` this feature uses it with (`kythira::request_vote_response<>`,
   `append_entries_response<>`, `install_snapshot_response<>`) — not merely
   "any type satisfying the `future` concept" — since `async_connect_kf`/
   `async_write_kf`/`async_read_kf` (Requirement 14.3) construct
   `kythira::promise_default<T>` internally, and that type's
   `getFuture()` returns `kythira::future_default<T>` specifically,
   regardless of which of the three backends (`folly`/`stdexec`/`boost`)
   `KYTHIRA_DEFAULT_FUTURE_BACKEND` currently resolves to
2. WHEN this constraint is violated (a `Types` bundle whose
   `future_template` is something else — `kythira::http_transport_types`'s
   own current default, `network_simulator::SimpleFuture`/`folly::Future`
   behind a dead `#ifdef FOLLY_AVAILABLE`, or `std_http_transport_types`'s
   `std::future`) THEN the system SHALL fail to compile with a clear
   constraint diagnostic (a `requires` clause, not a deep substitution
   failure inside `send_rpc`'s implementation), rather than silently
   constructing a `kythira::future_default<T>` where a different concrete
   type was expected
3. WHEN a canonical `Types` bundle for this feature is defined THEN the
   system SHALL provide one (e.g. `kythira::future_default_http_transport_types`)
   with `template<typename T> using future_template = kythira::future_default<T>;`,
   following the exact pattern already used ad hoc, per-file, in 13
   existing CoAP test files (`tests/coap_concurrent_processing_property_test.cpp`
   and others — confirmed by direct inspection while writing this
   requirement) — this feature formalizes an existing, precedented pattern
   into one reusable, named bundle rather than requiring every caller to
   redefine it locally
4. WHEN `kythira::http_transport_types` (the existing cpp-httplib
   transport's own default `Types` bundle) is examined THEN the system
   SHALL NOT modify it to satisfy this constraint — it keeps its own
   `future_template` unchanged, since altering it is out of scope
   (Requirement 15.1-15.2) and would affect the existing cpp-httplib
   transport's own callers; a caller wanting to use both transports
   against the same `Types` bundle SHALL use the new canonical bundle
   (Acceptance Criterion 3) for both, not mix `http_transport_types` with
   `boost_beast_client`/`server`
5. WHEN this constraint is documented THEN the system SHALL state
   explicitly, in both `design.md` and the new `Types` bundle's own header
   comment, that it is a deliberate narrowing relative to
   `transport_types`'s own, broader concept surface — a design choice
   trading `Types`-bundle flexibility for the ability to use
   `future_transformable` composition internally — not an oversight or a
   TODO to widen later without a specific reason to do so
