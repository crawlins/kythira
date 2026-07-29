# Requirements Document

## Introduction

This specification defines the requirements for a **third** HTTP transport
implementation, backed by [Proxygen](https://github.com/facebook/proxygen)
(Meta's C++ HTTP library), alongside the existing cpp-httplib-backed
implementation (`.kiro/specs/http-transport/`,
`include/raft/http_transport.hpp`) and the Boost.Beast-backed implementation
(`.kiro/specs/boost-beast-http-transport/`, `include/raft/beast_http_transport.hpp`).
It satisfies the same `kythira::network_client`/`kythira::network_server`
concepts (`include/raft/network.hpp`) and the same `kythira::transport_types`
concept (`include/raft/types.hpp`) as both existing implementations, so it is
a third drop-in alternative at the `Types` template parameter — not a
replacement for, and not a change to which transport, either existing
implementation's call sites use.

This mirrors the precedent both `.kiro/specs/boost-beast-http-transport/`
(a second `network_client`/`network_server`) and the Future-backend specs
(`.kiro/specs/stdexec-future-backend/`, `.kiro/specs/boost-future-backend/`)
already established: add a second (now third) independent implementation of
an existing concept-defined interface, gated behind its own opt-in build
configuration, without touching any call site that already works.

**Why a third HTTP transport at all.** cpp-httplib is synchronous and
blocking. Boost.Beast (this project's first asynchronous transport,
`.kiro/specs/boost-beast-http-transport/`) fixed that by driving I/O through
a caller-owned `boost::asio::io_context`, but still had to hand-build
several things from primitives: a connection pool with manual
least-recently-used eviction, a per-connection `net::strand` to prevent two
concurrent RPCs to the same node from interleaving their I/O on a shared
thread pool, and — confirmed the hard way, by an `AddressSanitizer`-caught
heap-use-after-free during that spec's implementation — careful manual
lifetime management of buffers/response objects across a `thenValue` chain.
Proxygen is a mature, production-hardened HTTP client/server library (it
serves a meaningful share of Meta's edge traffic) built directly on Folly's
`EventBase`/`AsyncSocket`, and gets several of these properties structurally
rather than by hand:

- **Connection-to-thread pinning is structural, not managed.** A
  `folly::EventBase` "can only drive an event loop for a single thread"
  (`folly/io/async/EventBase.h`'s own header comment, confirmed by direct
  inspection of the vendored header while writing this document) — unlike
  `boost::asio::io_context`, which explicitly supports multiple threads
  calling `run()` concurrently on the *same* `io_context` (Beast's own
  design, `.kiro/specs/boost-beast-http-transport/design.md`'s Phase 3,
  relies on this and compensates with an explicit `net::strand` per
  connection). Proxygen achieves multi-core concurrency via a
  `folly::IOThreadPoolExecutor` — a pool of *independent* `EventBase`
  instances, one per thread — and once a connection (an
  `HTTPUpstreamSession`/`HTTPTransaction`) is created on one of those
  `EventBase`s, every callback for that connection's lifetime fires on that
  same thread. This is the same guarantee Beast's per-connection
  `net::strand` exists to manually provide (Requirement 8, below), but here
  it falls out of the library's own architecture rather than requiring this
  feature to build and maintain it.
- **Zero-copy buffer chaining via `folly::IOBuf`**, rather than Beast's
  `beast::flat_buffer` (a single growable byte buffer whose reuse-across-calls
  turned out to be a real lifetime hazard in the Beast spec — see that
  spec's design.md Data Models section) or cpp-httplib's own internal
  buffering. This is a real, but as-yet-unquantified, potential win for
  larger request/response bodies (e.g. `install_snapshot` RPCs, which this
  project's Raft implementation already treats as the one RPC whose body
  size can be large — see `include/raft/raft.hpp`'s snapshot chunking) —
  Requirement 17 below requires this to be measured, not assumed.
- **A production HTTP/1.1 (and HTTP/2, though Requirement 18 puts HTTP/2 out
  of this feature's scope) codec and connection-pooling stack** that has
  already absorbed correctness fixes this project would otherwise have to
  rediscover independently, the way the Beast spec rediscovered two
  Folly-`thenValue`-flattening lifetime bugs during its own implementation.

**Why this also motivates an optional Folly-specific fast path, not offered
by either existing transport.** Proxygen's vcpkg port (`vcpkg-overlays` is
not needed; `proxygen` is a standard vcpkg port, confirmed present in this
project's vcpkg registry at version `2026.02.23.00` while writing this
document) unconditionally depends on `folly` — every `proxygen::` type this
feature touches (`HTTPConnector`, `HTTPTransaction::Handler`, `HTTPServer`)
is expressed in terms of `folly::EventBase`, `folly::AsyncSocket`, and
(deepest in the stack) `folly::Future`/`folly::Promise` — regardless of
which `KYTHIRA_DEFAULT_FUTURE_BACKEND` the rest of this project has
selected. This project's own Folly-backed future implementation,
`kythira::Future<T>` (`include/raft/future.hpp`, aliased as
`kythira::future_default<T>` specifically when
`KYTHIRA_DEFAULT_FUTURE_BACKEND=folly`, still this project's default per
root `CMakeLists.txt`), is not a fresh reimplementation — it is a thin
wrapper directly around `folly::Future<T>` (a private `_folly_future`
member) with an existing, already-implemented escape hatch,
`get_folly_future() && -> folly_type`, plus a constructor accepting a raw
`folly::Future<T>` directly (`explicit Future(folly_type ff)`). When the
project's selected future backend happens to be Folly, that escape hatch
means a `folly::Promise<T>` fulfilled directly by a
`proxygen::HTTPTransaction::Handler` callback can be wrapped into a
`kythira::Future<T>` with no intermediate translation step — no
`kythira::promise_default<T>` construction, no second
`setValue`/`setException` hop through this feature's own generic bridging
adaptor (Requirement 14). This is a genuinely narrower opportunity than
"Proxygen is fast" in general: it exists specifically at the intersection of
"the future backend is Folly" and "the transport is Proxygen," and this
feature's job is to detect that intersection at compile time and take the
shortcut *only* then, while remaining fully correct (via the ordinary
generic bridge) for every other combination.

This feature does not remove cpp-httplib or Boost.Beast, does not convert
any existing production call site, does not change which transport
`cmd/ca_service`, `cmd/ca_cluster_node`, or any existing example uses by
default, and does not modify the `network_client`/`network_server`/
`transport_types` concepts themselves.

## Glossary

- **Proxygen**: The `proxygen` vcpkg package — Meta's C++ HTTP library,
  providing HTTP/1.1 and HTTP/2 client (`proxygen::HTTPConnector`) and
  server (`proxygen::HTTPServer`) functionality on top of Folly's
  asynchronous I/O primitives. Not currently a dependency of this project —
  confirmed by inspection of `vcpkg.json` while writing this document.
- **`folly::EventBase`**: Folly's per-thread event loop — the analog of
  Boost.Asio's `io_context`, but explicitly single-threaded per instance
  (Introduction, above); concurrency comes from running several `EventBase`
  instances across a thread pool, not from multiple threads driving one.
- **`folly::IOThreadPoolExecutor`** (specifically
  `folly::IOThreadPoolExecutorBase`, the type `proxygen::HTTPServer::start()`
  actually accepts): a pool of `EventBase`-backed worker threads, each
  running its own `EventBase`; work (e.g. an accepted connection) is
  assigned to one of them and, once assigned, stays pinned to that thread
  for its lifetime.
- **`proxygen::HTTPConnector`**: The client-side connection-establishment
  type (`proxygen/lib/http/HTTPConnector.h`). `connect()`/`connectSSL()`
  take an explicit `folly::EventBase*` and a `Callback*`
  (`connectSuccess(HTTPUpstreamSession*)`/`connectError(...)`) — connecting
  is asynchronous and non-blocking, the same shape as Beast's
  `async_connect`.
- **`proxygen::HTTPUpstreamSession`**: The client-side established
  connection, obtained from a successful `HTTPConnector::Callback::connectSuccess`.
  `newTransaction(HTTPTransaction::Handler*)` opens a new request/response
  exchange on it (HTTP/1.1: one at a time per connection, matching this
  feature's own connection-reuse model, Requirement 9; HTTP/2, out of scope
  per Requirement 18, would allow several concurrently).
- **`proxygen::HTTPTransaction::Handler`**: The per-request/response
  callback interface (`proxygen/lib/http/session/HTTPTransaction.h`) —
  `onHeadersComplete(std::unique_ptr<HTTPMessage>)`,
  `onBody(std::unique_ptr<folly::IOBuf>)`, `onEOM()`, `onError(const
  HTTPException&)`, among others. This feature's generic bridge
  (Requirement 14) implements this interface once and fulfills a
  `kythira::promise_default<T>` from it, the same shape as Beast's
  `async_read_kf`/`async_write_kf` completion handlers wrap a Beast
  `async_*` call.
- **`proxygen::HTTPServer`**: The server-side listener/dispatcher
  (`proxygen/httpserver/HTTPServer.h`). `start(onSuccess, onError,
  getAcceptorFactory, ioExecutor)` **blocks the calling thread**, running
  the event loop itself, until `stop()` is called from another thread — a
  materially different calling convention from both `cpp_httplib_server`
  (whose own internal `_server_thread` already absorbs an equivalent
  blocking `httplib::Server::listen()` call) and `boost_beast_server`
  (which issues `async_accept` from within a caller-driven `io_context`
  that was never itself owned by the transport). Requirement 5 states how
  `proxygen_server` reconciles this with `network_server`'s
  synchronous-feeling `start()`/`stop()` contract.
- **`folly::SSLContext`**: Folly's OpenSSL `SSL_CTX*` wrapper
  (`folly/io/async/SSLContext.h`) — `loadCertificate`/`loadPrivateKey`/
  `loadTrustedCertificates`/`ciphers`/`setVerificationOption`/
  `authenticate`, all ultimately configuring the same kind of `SSL_CTX` this
  project's other two transports already configure via
  `boost::asio::ssl::context` (Beast) or `httplib::SSLServer`/direct
  `SSL_CTX*` calls (cpp-httplib, `tls_tcp_rpc.hpp`) — not a new TLS
  library, a third way of driving the same OpenSSL primitives.
- **`wangle::SSLContextConfig`**: The server-side TLS configuration struct
  `proxygen::HTTPServerOptions` accepts (`wangle/ssl/SSLContextConfig.h`) —
  certificate/key/CA paths, cipher list — Proxygen's server-side analog of
  `folly::SSLContext`'s client-side, imperative configuration calls.
- **`kythira::Future<T>`**: This project's Folly-backed future wrapper
  (`include/raft/future.hpp`) — a thin wrapper holding a private
  `folly::Future<T> _folly_future`, with `get_folly_future() &&` and a
  `explicit Future(folly_type ff)` constructor already exposing that
  interop point. Aliased as `kythira::future_default<T>` specifically when
  `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly` (`include/raft/future_default.hpp`).
  Always includable regardless of the project's selected backend — `#include
  <raft/future.hpp>` carries no `#ifdef` of its own — which is what lets
  this feature's Folly-specific fast path (Requirement 16) reference
  `kythira::Future<T>` by name unconditionally in its own implementation,
  even in a build where `KYTHIRA_DEFAULT_FUTURE_BACKEND` is `stdexec` or
  `boost` for the rest of the project.
- **`transport_types`**: The existing concept (`include/raft/types.hpp`)
  already shared between the HTTP, Beast, and CoAP transports, requiring
  `serializer_type`, `metrics_type`, `executor_type`, and a
  `future_template<T>` member template whose instantiations satisfy
  `kythira::future`. This feature adds no new members to this concept.
- **`future_default_http_transport_types`**: The canonical `Types` bundle
  the Beast spec introduced (`include/raft/beast_http_transport.hpp`,
  `.kiro/specs/boost-beast-http-transport/` Requirement 19.3) pinning
  `future_template<T>` to `kythira::future_default<T>`. This feature
  duplicates (does not `#include`) an identical bundle in its own header
  (Requirement 15.3) rather than depending on the Beast transport's own
  header, so that enabling the Proxygen transport never requires also
  enabling `KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT` — the two are independent,
  separately opt-in features that happen to need the identically-shaped
  bundle for the identical structural reason (Requirement 15).
- **RPC endpoint paths**: `/v1/raft/request_vote`, `/v1/raft/append_entries`,
  `/v1/raft/install_snapshot` — identical to both existing HTTP transports.

## Requirements

### Requirement 1: Proxygen-Backed Network Client

**User Story:** As a distributed systems developer, I want a Proxygen-backed
`network_client` implementation, so that I can send Raft RPCs over HTTP
through a production-hardened asynchronous HTTP client instead of
hand-composing one from lower-level primitives.

#### Acceptance Criteria

1. WHEN `proxygen_client<Types>` is instantiated with a `Types` that
   satisfies `kythira::transport_types` THEN the system SHALL provide
   `send_request_vote`, `send_append_entries`, and `send_install_snapshot`
   methods matching `kythira::network_client`'s exact signatures
2. WHEN `proxygen_client<Types>` is evaluated against the
   `kythira::network_client` concept THEN it SHALL satisfy the concept
   without any modification to the concept itself
3. WHEN `proxygen_client<Types>` is constructed THEN the system SHALL accept
   the same node-id-to-URL mapping shape
   (`std::unordered_map<std::uint64_t, std::string>`) as `cpp_httplib_client`
   and `boost_beast_client`, so existing cluster-configuration code is
   transport-agnostic across all three
4. WHEN a request is sent THEN the system SHALL serialize it using
   `Types::serializer_type`, identical to both existing transports — this
   feature does not introduce a third serialization mechanism

### Requirement 2: Proxygen-Backed Network Server

**User Story:** As a distributed systems developer, I want a Proxygen-backed
`network_server` implementation, so that I can receive Raft RPCs over HTTP
through the same production-hardened stack on the server side.

#### Acceptance Criteria

1. WHEN `proxygen_server<Types>` is instantiated with a `Types` that
   satisfies `kythira::transport_types` THEN the system SHALL provide
   `register_request_vote_handler`, `register_append_entries_handler`,
   `register_install_snapshot_handler`, `start`, `stop`, and `is_running`
   methods matching `kythira::network_server`'s exact signatures
2. WHEN `proxygen_server<Types>` is evaluated against the
   `kythira::network_server` concept THEN it SHALL satisfy the concept
   without any modification to the concept itself
3. WHEN `proxygen_server<Types>` is constructed THEN the system SHALL accept
   the same `(bind_address, bind_port)` shape as `cpp_httplib_server`/
   `boost_beast_server`
4. WHEN a request arrives at `/v1/raft/request_vote`,
   `/v1/raft/append_entries`, or `/v1/raft/install_snapshot` THEN the system
   SHALL route it to the correspondingly-registered handler, matching both
   existing transports' endpoint routing exactly

### Requirement 3: Client-Side RPC Behavioral Parity

**User Story:** As a Raft node, I want RequestVote/AppendEntries/
InstallSnapshot RPCs sent over the Proxygen transport to behave identically,
in externally observable terms, to the same RPCs sent over either existing
transport, so that the choice of transport is invisible to Raft core logic.

#### Acceptance Criteria

1. WHEN `send_request_vote`/`send_append_entries`/`send_install_snapshot` is
   called on `proxygen_client<Types>` THEN the system SHALL POST to the same
   endpoint path both existing transports use for that RPC
2. WHEN the HTTP request succeeds with status 200 THEN the system SHALL
   deserialize the response body using `Types::serializer_type` and complete
   the returned `Types::future_template<Response>` with the deserialized
   value
3. WHEN the HTTP request fails (connection refused, DNS failure, TLS
   handshake failure, or timeout — Requirement 10) THEN the system SHALL
   complete the returned future with an exception, matching both existing
   transports' failure-to-exception mapping in kind, not necessarily the
   same concrete exception type
4. WHEN two Proxygen-backed RPCs to different target nodes are in flight
   concurrently on the same `proxygen_client<Types>` instance THEN the
   system SHALL complete both correctly and independently, potentially on
   different `folly::EventBase` threads of the shared
   `folly::IOThreadPoolExecutor` (Requirement 8)
5. WHEN two Proxygen-backed RPCs to the *same* target node are issued in
   quick succession THEN the system SHALL NOT interleave their I/O on the
   shared connection — unlike Requirement 3.4's cross-node case, this
   requires no explicit synchronization primitive of this feature's own
   construction (contrast Beast's `net::strand`, `.kiro/specs/boost-beast-http-transport/design.md`
   Phase 3): once an `HTTPUpstreamSession` is created on a given
   `EventBase`, every subsequent operation on it is only ever invoked from
   that `EventBase`'s own thread, by Proxygen's own construction

### Requirement 4: Server-Side RPC Behavioral Parity

**User Story:** As a Raft server, I want the Proxygen transport's server
side to handle RequestVote/AppendEntries/InstallSnapshot RPCs with the same
request/response/error-status semantics as both existing transports, so that
clients cannot distinguish which server-side transport they are talking to.

#### Acceptance Criteria

1. WHEN a POST request arrives at a registered RPC endpoint THEN the system
   SHALL deserialize the request body using `Types::serializer_type`,
   invoke the registered handler, serialize its response, and reply with
   HTTP status 200 — identical externally observable behavior to both
   existing transports
2. WHEN the request body fails to deserialize THEN the system SHALL reply
   with HTTP status 400, matching `.kiro/specs/http-transport/` Requirement
   13.1-13.2 and `.kiro/specs/boost-beast-http-transport/` Requirement 4.2
3. WHEN the registered handler throws THEN the system SHALL reply with HTTP
   status 500, matching both existing transports' Requirement 13.3/4.3
4. WHEN a request arrives for an unregistered/unrecognized path THEN the
   system SHALL reply with an appropriate non-200 status (404) — Proxygen's
   `proxygen::RequestHandlerFactory` provides routing dispatch, but this
   feature is still responsible for the specific "no handler registered for
   this path" 404 behavior, not left to a library default that might differ
   from the other two transports'

### Requirement 5: Server Lifecycle (start/stop/is_running)

**User Story:** As a system operator, I want the Proxygen server's lifecycle
(`start`/`stop`/`is_running`) to present the same synchronous,
call-and-it's-done contract as both existing transports' servers, despite
`proxygen::HTTPServer::start()` itself having a materially different
(blocking-the-calling-thread) calling convention.

#### Acceptance Criteria

1. WHEN `start()` is called on `proxygen_server<Types>` THEN the system
   SHALL bind the configured address/port, begin accepting connections, and
   return once the listener is actually accepting — matching both existing
   transports' Requirement 9.1-9.2/5.1 observable contract — even though
   the underlying `proxygen::HTTPServer::start()` call itself blocks the
   thread that issues it until the server stops; `proxygen_server` SHALL
   own and join a dedicated thread to run that blocking call on (the same
   shape `cpp_httplib_server`'s own `_server_thread` already uses for
   `httplib::Server::listen()`'s equally blocking call), signaling
   readiness back to the calling thread via `HTTPServer::start()`'s own
   `onSuccess` callback rather than a fixed sleep or a race
2. WHEN `stop()` is called THEN the system SHALL stop accepting new
   connections and SHALL allow in-flight requests to complete before the
   call returns, matching Requirement 9.3-9.4/5.2 of the existing specs —
   for Proxygen specifically, this means calling `proxygen::HTTPServer::stop()`
   (which drops connections immediately per its own documented contract,
   confirmed by direct inspection of `proxygen/httpserver/HTTPServer.h`
   while writing this document) only after a drain step of this feature's
   own construction, mirroring the active-close drain
   `boost_beast_server::stop()` needed (`.kiro/specs/boost-beast-http-transport/design.md`
   Property 8's addendum) rather than relying on Proxygen's own `stop()` to
   wait for in-flight work, since it explicitly does not
3. WHEN `is_running()` is called THEN the system SHALL report whether the
   server is currently accepting connections
4. WHEN `start()` is called on an already-running server, or `stop()` on an
   already-stopped one THEN the system SHALL behave safely (no crash, no
   double-bind, no double-join), matching both existing transports'
   defensive behavior

### Requirement 6: TLS Support Parity

**User Story:** As a security-conscious operator, I want the Proxygen
transport to support HTTPS with the same certificate/cipher/TLS-version
configuration surface as both existing transports, so that choosing Proxygen
does not mean giving up TLS features already relied on in production.

#### Acceptance Criteria

1. WHEN the Proxygen client is configured with an `https://` URL THEN the
   system SHALL establish the connection via `HTTPConnector::connectSSL`
   with a `folly::SSLContext` built from the configured certificate
   material, rather than `HTTPConnector::connect`
2. WHEN the Proxygen server is configured with `enable_ssl=true` THEN the
   system SHALL configure `proxygen::HTTPServerOptions` with a
   `wangle::SSLContextConfig` built from the configured certificate
   material
3. WHEN establishing a TLS connection THEN the system SHALL validate the
   peer certificate against the configured CA, matching
   `.kiro/specs/http-transport/` Requirement 10.3-10.4's and
   `.kiro/specs/boost-beast-http-transport/` Requirement 6.3's observable
   behavior (rejected connection, future/accept completes with an error)
4. WHEN SSL certificate/key file paths are configured THEN the system SHALL
   accept the identical config field names and semantics as
   `cpp_httplib_client_config`/`cpp_httplib_server_config`/
   `boost_beast_client_config`/`boost_beast_server_config` (`ca_cert_path`,
   `client_cert_path`/`client_key_path`, `ssl_cert_path`/`ssl_key_path`,
   `cipher_suites`, `min_tls_version`/`max_tls_version`,
   `require_client_cert`) — a Proxygen config struct with the same fields,
   translated internally to `folly::SSLContext`/`wangle::SSLContextConfig`
   calls, not a redesigned configuration surface
5. WHEN client certificate authentication is required (server-side
   `require_client_cert=true`) THEN the system SHALL configure
   `wangle::SSLContextConfig`/the server's `folly::SSLContext` to request
   and verify a client certificate, matching both existing transports'
   Requirement 10.10-10.11/6.5
6. WHEN TLS material is invalid or missing at configuration time THEN the
   system SHALL report a clear, specific error (which file, what went
   wrong), matching both existing transports' Requirement 10.12/10.14/6.6,
   not a generic OpenSSL or Folly error surfaced unmodified

### Requirement 7: TLS Material Reload

**User Story:** As a system operator managing certificate rotation, I want
the Proxygen transport to support reloading TLS material without dropping
established connections, matching both existing transports'
`reload_tls_material()`/`enable_auto_reload()` behavior.

#### Acceptance Criteria

1. WHEN `reload_tls_material()` is called on `proxygen_client<Types>` THEN
   the system SHALL validate the new certificate/key material first
   (all-or-nothing) and, on success, apply it to a fresh `folly::SSLContext`
   used for subsequently-established connections, matching
   `cpp_httplib_client::reload_tls_material()`'s and
   `boost_beast_client::reload_tls_material()`'s documented behavior —
   retired contexts are kept alive rather than destroyed, since an
   in-flight `HTTPUpstreamSession` may still reference the old context
2. WHEN `reload_tls_material()` is called on `proxygen_server<Types>` THEN
   the system SHALL apply the new material to a fresh
   `wangle::SSLContextConfig` without closing the listener or dropping
   established connections, matching both existing transports' documented
   behavior
3. WHEN `reload_tls_material()` fails validation THEN the system SHALL
   throw and leave the previous, still-valid material in effect
4. WHEN `enable_auto_reload(poll_interval)` is called THEN the system SHALL
   start a background polling loop that calls `reload_tls_material()`
   whenever the configured certificate file's mtime changes, matching both
   existing transports' polling-based approach
5. WHEN `disable_auto_reload()` is called THEN the system SHALL stop the
   polling loop cleanly (joined, not detached)

### Requirement 8: EventBase/Thread-Pool Ownership

**User Story:** As a developer integrating the Proxygen transport, I want
explicit, documented control over the thread pool driving it, so that I can
share one `folly::IOThreadPoolExecutor` across multiple Proxygen-based
components in the same process, the way Requirement 8 of
`.kiro/specs/boost-beast-http-transport/` lets callers share one
`io_context` across Beast-based ones — while the underlying threading model
is genuinely different (Introduction, Glossary), not the same primitive
under a different name.

#### Acceptance Criteria

1. WHEN `proxygen_client<Types>` is constructed THEN the system SHALL accept
   a reference to a caller-owned `folly::IOThreadPoolExecutorBase`
   (or, if the Phase 0 spike (Requirement 21) finds a single shared
   `folly::EventBase*` a better fit for the client side specifically, since
   `HTTPConnector::connect` takes an `EventBase*` directly rather than an
   executor, that finding SHALL be recorded in `spike-notes.md` and
   reflected here) rather than constructing and owning its own thread pool
   internally
2. WHEN `proxygen_server<Types>` is constructed THEN the system SHALL accept
   an optional caller-owned `std::shared_ptr<folly::IOThreadPoolExecutorBase>`,
   forwarded to `proxygen::HTTPServer::start()`'s own `ioExecutor` parameter
   — when absent, the system SHALL let `HTTPServer::start()` construct its
   own (its own documented default behavior, confirmed by direct inspection
   of `proxygen/httpserver/HTTPServer.h` while writing this document),
   matching `cpp_httplib_server`'s existing self-contained-by-default
   posture rather than forcing every caller to plumb an executor through
3. WHEN no thread is executing the relevant `EventBase`'s loop (client side)
   or the server's own dedicated thread has not yet called
   `proxygen::HTTPServer::start()` (server side) THEN the system SHALL make
   no progress on any in-flight operation — the client-side half of this is
   the direct analog of Beast's Requirement 8.2 and SHALL be stated
   explicitly in this feature's own documentation (Requirement 19), not
   left to be discovered
4. WHEN `stop()` is called on `proxygen_server<Types>` THEN the system SHALL
   NOT stop or destroy a caller-provided `folly::IOThreadPoolExecutorBase`
   — only its own listener/connections and (if the server constructed its
   own executor because none was provided) that self-owned one, mirroring
   Beast's Requirement 8.3 distinction between "my own resources" and
   "resources the caller might still need"

### Requirement 9: Connection Reuse

**User Story:** As a developer, I want connection reuse for the Proxygen
client, so that repeated RPCs to the same target node do not pay a fresh
TCP/TLS handshake every time, matching both existing transports' connection
pooling.

#### Acceptance Criteria

1. WHEN `send_request_vote`/`send_append_entries`/`send_install_snapshot` is
   called for a target node with no existing open `HTTPUpstreamSession` THEN
   the system SHALL establish one (via `HTTPConnector::connect`/
   `connectSSL`) and reuse it for subsequent requests to that same node
2. WHEN an established session to a target node is idle beyond a configured
   timeout THEN the system SHALL close it, matching
   `.kiro/specs/http-transport/` Requirement 11.4's and
   `.kiro/specs/boost-beast-http-transport/` Requirement 9.2's behavior
3. WHEN a pooled session is found to be broken (peer closed, network error —
   surfaced via `HTTPTransaction::Handler::onError`/session-level callbacks)
   on the next request THEN the system SHALL transparently establish a
   fresh session rather than surfacing the staleness as a caller-visible
   error
4. WHEN connection pool size is configured (matching
   `cpp_httplib_client_config::connection_pool_size`/
   `boost_beast_client_config::connection_pool_size`) THEN the system SHALL
   respect it as an upper bound on simultaneously-open sessions

### Requirement 10: Configurable Timeouts

**User Story:** As a distributed systems developer, I want configurable
timeouts for both connection establishment and full request completion, so
that a slow or unresponsive peer cannot block a Raft RPC indefinitely.

#### Acceptance Criteria

1. WHEN `send_request_vote`/`send_append_entries`/`send_install_snapshot` is
   called with a `timeout` parameter THEN the system SHALL enforce it as the
   deadline for the *entire* RPC (connect + send + receive), using
   `HTTPTransaction::setIdleTimeout`/an explicit `folly::HHWheelTimer`
   deadline (the specific mechanism is a Requirement 21 spike question),
   not a per-callback timeout that could be individually satisfied while
   the RPC as a whole overruns
2. WHEN the timeout elapses before the RPC completes THEN the system SHALL
   cancel the in-flight operation and complete the returned future with a
   timeout-specific exception, matching both existing transports'
   Requirement 12.4-12.5/10.2 observable behavior
3. WHEN `connection_timeout` (matching the existing config field) is
   configured separately from the per-RPC `timeout` parameter THEN the
   system SHALL apply it specifically to `HTTPConnector::connect`'s own
   `timeoutMs` argument, for cases where a caller wants a tighter bound on
   "can I even reach this node" than on "how long until I get a response"

### Requirement 11: Configuration Struct Parity

**User Story:** As a developer, I want the same configuration surface
(client and server config structs) as both existing transports, so that
switching a call site between any two of the three transports requires
changing a type, not rewriting configuration.

#### Acceptance Criteria

1. WHEN a Proxygen client config struct is defined THEN the system SHALL
   include every field `cpp_httplib_client_config`/`boost_beast_client_config`
   has, with identical names, types, and default values
2. WHEN a Proxygen server config struct is defined THEN the system SHALL
   include every field `cpp_httplib_server_config`/`boost_beast_server_config`
   has, with identical names, types, and default values
3. WHEN a config field has no direct Proxygen-specific equivalent (e.g.
   `max_concurrent_connections`) THEN the system SHALL document, in that
   field's own comment, how Proxygen enforces the equivalent behavior
   instead, following `.kiro/specs/boost-beast-http-transport/` Requirement
   11.3's precedent for this exact situation

### Requirement 12: Metrics Parity

**User Story:** As a system operator, I want the Proxygen transport to emit
the same category of metrics as both existing transports, so that
dashboards/alerts built against one transport's metrics work against all
three.

#### Acceptance Criteria

1. WHEN `proxygen_client<Types>`/`proxygen_server<Types>` is constructed
   THEN the system SHALL accept a `Types::metrics_type` instance, identical
   to both existing transports' constructor shape
2. WHEN a request is sent (client) or received (server) THEN the system
   SHALL emit metrics for request count, latency, and size, matching
   `.kiro/specs/http-transport/` Requirement 16.3-16.4 and
   `.kiro/specs/boost-beast-http-transport/` Requirement 12.2
3. WHEN an error occurs THEN the system SHALL emit error metrics with a type
   distinguishing at least: connection failure, TLS failure, timeout, and
   deserialization failure, matching Requirement 12.3's precedent
4. WHEN connection pool operations occur (Requirement 9) THEN the system
   SHALL emit connection lifecycle metrics
5. WHEN server lifecycle events occur (Requirement 5) THEN the system SHALL
   emit start/stop metrics
6. WHEN the Folly-specific fast path (Requirement 16) is taken for a given
   RPC THEN the system SHALL be able to distinguish this in emitted metrics
   from the generic bridge path having been taken (e.g. a label/tag on the
   existing request-count metric, not a wholly new metric family) — this is
   what makes Requirement 17's benchmark-vs-production-traffic comparison
   possible without a separate ad hoc instrumentation pass later

### Requirement 13: Genericity Over transport_types

**User Story:** As a developer, I want the Proxygen transport to work with
any existing `transport_types`-satisfying `Types` bundle without requiring a
new one for basic use, so that adopting it is a template-argument change,
not a type-system migration — the same posture Requirement 13 of
`.kiro/specs/boost-beast-http-transport/` already established, carried
forward here (Requirement 15 narrows this specifically for the
`future_template` member, for the same reason Beast's own Requirement 19
did).

#### Acceptance Criteria

1. WHEN `proxygen_client<Types>`/`proxygen_server<Types>` is defined THEN
   the system SHALL constrain `Types` with `requires
   kythira::transport_types<Types>`, identical to all three transports'
   existing base constraint
2. WHEN `kythira::transport_types` itself is examined THEN the system SHALL
   NOT modify it — this feature adds a new *consumer* of the concept, not a
   new *requirement* on it
3. WHEN generic code already written against `network_client`/
   `network_server` is instantiated with `proxygen_client`/`proxygen_server`
   in place of either existing transport's types THEN the system SHALL
   require no changes to that generic code

### Requirement 14: Generic Async Composition via future_transformable

**User Story:** As a library maintainer, I want the generic (any-future-backend)
send/receive path built the same way Beast's was — composed via
`future_transformable`'s `thenValue`/`thenError` over small, independently
testable adaptors around Proxygen's own callback interface — so that this
feature works correctly under `KYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec` and
`=boost` as well as `=folly`, with the Folly-specific fast path
(Requirement 16) as a genuine optimization layered on top, not a
prerequisite for correctness.

#### Acceptance Criteria

1. WHEN this feature's generic path is implemented THEN the system SHALL
   bridge `proxygen::HTTPConnector::Callback` (connect) and
   `proxygen::HTTPTransaction::Handler` (send/receive) into fulfilling a
   `kythira::promise_default<T>` — `setValue`/`setException` called from
   inside the relevant callback method — the same fulfillment pattern
   `.kiro/specs/boost-beast-http-transport/` Requirement 14.2 already
   established for Beast's `async_connect_kf`/`async_write_kf`/
   `async_read_kf`, adapted to Proxygen's own callback shape
2. WHEN the per-step futures from Acceptance Criterion 1 need to be
   sequenced (connect, then send the request, then receive headers, then
   receive body, then deserialize) THEN the system SHALL compose them using
   `future_transformable`'s `thenValue`/`thenError`
   (`include/concepts/future.hpp`), matching Requirement 14.3 of the Beast
   spec, rather than hand-nesting Proxygen's own callbacks into a bespoke
   state machine per RPC method
3. WHEN this generic path is used (i.e. the Folly fast path's condition,
   Requirement 16, does not hold) THEN the system SHALL behave correctly
   regardless of which of the three `KYTHIRA_DEFAULT_FUTURE_BACKEND`
   options is selected, since `kythira::promise_default<T>`/
   `kythira::future_default<T>` (Requirement 15) resolve to whichever
   backend's concrete types are active
4. WHEN multiple `EventBase` threads (Requirement 8) are each driving
   different in-flight RPCs concurrently THEN the system SHALL be safe
   under that concurrency — no data race on shared client/server state, no
   double-fulfillment of a promise, relying on Requirement 3.5's structural
   per-connection thread-pinning guarantee for same-connection safety and
   ordinary synchronization (a mutex guarding the connection-pool map
   itself, matching Beast's own `_connections_mutex`) for cross-connection
   state

### Requirement 15: future_default Narrowing Constraint

**User Story:** As a developer relying on `future_transformable` composition
(Requirement 14) to build the generic bridge, I want the same concrete
`Types` constraint `.kiro/specs/boost-beast-http-transport/` Requirement 19
already established, applied here, so the internal `Promise`/`Future`
pairing used by the chain is guaranteed to match the public
`Types::future_template<T>` return type the caller sees.

#### Acceptance Criteria

1. WHEN `proxygen_client<Types>`/`proxygen_server<Types>` is instantiated
   THEN the system SHALL require `Types::template future_template<T>` to be
   `kythira::future_default<T>` for every `T` this feature uses it with —
   identical constraint, identical rationale, to
   `.kiro/specs/boost-beast-http-transport/` Requirement 19.1
2. WHEN this constraint is violated THEN the system SHALL fail to compile
   with a clear constraint diagnostic, reusing the existing
   `kythira::future_default_transport_types<Types>` concept's *shape*
   (`include/raft/beast_http_transport.hpp`) — duplicated into this
   feature's own header (Acceptance Criterion 3), not `#include`d from it,
   per the Glossary's `future_default_http_transport_types` entry
3. WHEN a canonical `Types` bundle for this feature is defined THEN the
   system SHALL provide one
   (`kythira::future_default_proxygen_transport_types` or, if the Phase 0
   spike finds no Proxygen-specific reason to diverge, a name-compatible
   sibling of Beast's own `future_default_http_transport_types` — the
   spike SHALL settle the exact name, recorded in `spike-notes.md`) with
   the identical member shape as Beast's bundle, and SHALL document, in
   both this bundle's header comment and this feature's `design.md`, that
   it is a deliberate, independent duplication (not a shared header
   dependency on the Beast transport) so that `CONFIG_PROXYGEN_TRANSPORT`
   and `CONFIG_BOOST_BEAST_TRANSPORT` remain fully independent opt-ins
4. WHEN neither `kythira::http_transport_types` nor Beast's
   `future_default_http_transport_types` is examined THEN the system SHALL
   NOT modify either — both are out of scope (Requirement 18), and a
   caller wanting to use this feature alongside either existing transport
   against the same conceptual `Types` bundle SHALL use this feature's own
   canonical bundle for the Proxygen half, matching Beast's own
   Requirement 19.4 precedent for the analogous situation

### Requirement 16: Optional Folly-Native Fast Path

**User Story:** As a developer who has already selected
`KYTHIRA_DEFAULT_FUTURE_BACKEND=folly` — this project's own default — and
who is choosing the Proxygen transport specifically because it is built on
Folly natively, I want an optional fast path that skips the generic bridge
(Requirement 14) entirely when both conditions hold, so that the "third
translation hop" the generic path otherwise pays (Proxygen's own internal
Folly promise → this feature's `kythira::promise_default<T>` →
`kythira::future_default<T>`) collapses to a single, direct wrap.

#### Acceptance Criteria

1. WHEN `Types::template future_template<T>` is `kythira::Future<T>`
   specifically (the Folly-backed class from `include/raft/future.hpp`,
   *not* merely "whatever `future_default<T>` currently aliases to" —
   Acceptance Criterion 5 explains why this distinction matters) THEN the
   system SHALL detect this at compile time via `if constexpr
   (std::same_as<typename Types::template future_template<T>,
   kythira::Future<T>>)`, requiring no CMake macro, no `#ifdef`, and no
   knowledge of which `KYTHIRA_DEFAULT_FUTURE_BACKEND` string CMake
   resolved — the condition is fully determined by the `Types` bundle's own
   member type, matching this project's general preference for
   concept/trait-based dispatch over preprocessor conditionals wherever the
   information needed is available at the type level
2. WHEN the Acceptance Criterion 1 condition holds THEN the system SHALL
   construct a raw `folly::Promise<T>` directly (not
   `kythira::promise_default<T>`) inside the relevant
   `HTTPTransaction::Handler` callback, fulfill it from that same callback,
   and wrap the resulting `folly::Future<T>`/`folly::SemiFuture<T>` into a
   `kythira::Future<T>` via the existing `explicit Future(folly_type ff)`
   constructor (`include/raft/future.hpp`) — no `kythira::promise_default<T>`
   is constructed on this path at all, and no `setValue`/`setException`
   translation step happens between Proxygen's own completion and the
   caller-visible future's completion
3. WHEN chaining the fast path's steps (connect → send → receive → deserialize)
   THEN the system SHALL prefer scheduling continuations on the same
   `folly::EventBase` the `HTTPTransaction` is already bound to (e.g. via
   `folly::Future<T>::via(evb)`, or by structuring the chain so
   `thenValue` callbacks already execute inline on the fulfilling thread —
   the specific mechanism is a Requirement 21 spike question) SHALL avoid
   an additional cross-thread post/wake that the generic bridge's own
   `kythira::promise_default<T>` fulfillment does not itself guarantee
   avoiding
4. WHEN `Types::template future_template<T>` is *not* `kythira::Future<T>`
   (the `stdexec`/`boost` backends, or a Folly-backed `Types` bundle that
   for some reason does not use `kythira::future_default<T>` as its
   `future_template`) THEN the system SHALL use the generic bridge
   (Requirement 14) unconditionally — this feature SHALL NOT require the
   Folly fast path's own preconditions to hold for basic correctness, and
   the fast path's own code SHALL be provably unreachable (not merely
   untaken) in a build where `Types::template future_template<T>` can never
   satisfy Acceptance Criterion 1's condition
5. WHEN this feature is documented THEN the system SHALL state explicitly,
   in both `design.md` and the fast path's own implementation comments, why
   the dispatch condition is `std::same_as<..., kythira::Future<T>>` and
   not merely "`KYTHIRA_DEFAULT_FUTURE_BACKEND == folly`": a `Types` bundle
   could in principle set `future_template<T>` to something else entirely
   even while the project-wide backend happens to be Folly (an unusual but
   not concept-forbidden configuration), and the fast path's correctness
   depends on the *actual* return type matching `kythira::Future<T>`
   exactly, not on which CMake option happened to be set

### Requirement 17: Performance Validation Methodology

**User Story:** As a developer evaluating whether Requirement 16's fast path
is worth its own added code path and maintenance cost, I want its
performance benefit measured with the same rigor
`.kiro/specs/future-backend-performance-benchmark/` already established for
comparing future backends against each other, not asserted from
architecture alone.

#### Acceptance Criteria

1. WHEN this feature's performance characteristics are evaluated THEN the
   system SHALL extend the existing benchmark harness
   (`.kiro/specs/future-backend-performance-benchmark/`,
   `examples/performance_benchmark_report.cpp`,
   `tests/performance_benchmark_test.cpp`) with at least two new scenarios:
   the generic bridge path and the Folly fast path, both exercising the
   same operation (a round-trip RPC, or the smallest realistic subset of
   one the harness's existing scenario shape supports) against each other
   under `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly`
2. WHEN the benchmark is run THEN the system SHALL report the comparison as
   a **comparison report**, not a **sanity floor** (matching
   `.kiro/specs/future-backend-performance-benchmark/requirements.md`'s
   Glossary distinction) — this feature does not gate CI on the fast path
   beating the generic path by any specific margin, since relative
   performance is expected to shift across hardware/compiler/library
   versions, matching that spec's own stated reason for the same design
   choice
3. WHEN this feature's Introduction claims a performance advantage from
   structural connection-to-thread pinning (Requirement 3.5) or zero-copy
   `IOBuf` buffer handling THEN the system SHALL either measure that
   specific claim (e.g. a benchmark scenario using a large
   `install_snapshot`-sized body, to test the `IOBuf` claim specifically)
   or explicitly label it, in `design.md`, as an architectural expectation
   not yet independently measured by this feature's own benchmark suite —
   this feature SHALL NOT present an unmeasured architectural claim as if
   it were a measured result
4. WHEN the benchmark results are recorded THEN the system SHALL record them
   in this spec's own documentation (`design.md` or a dedicated results
   section), following `.kiro/specs/future-backend-performance-benchmark/`'s
   own precedent of documenting real numbers rather than only a methodology

### Requirement 18: Scope Statement

**User Story:** As a developer, I want the scope of this feature stated
explicitly, so that expectations about what "Proxygen HTTP transport" does
and does not include are accurate, especially given that Proxygen itself is
*capable* of more than this feature uses.

#### Acceptance Criteria

1. WHEN documentation for this feature is written THEN the system SHALL
   state explicitly that no existing production call site is converted to
   Proxygen by this feature, and neither cpp-httplib nor Boost.Beast is
   removed, deprecated, or made optional-only as a result
2. WHEN documentation for this feature is written THEN the system SHALL
   state explicitly that HTTP/2 is out of scope, even though Proxygen
   itself supports it — this feature targets HTTP/1.1 request/response
   semantics matching both existing transports' wire behavior, for
   behavioral-parity and cross-transport-equivalence testing purposes
   (Requirement 19.4); adding HTTP/2 multiplexing as a genuinely new
   capability neither existing transport has is a deliberately separate,
   unscoped idea this design does not pursue
3. WHEN documentation for this feature is written THEN the system SHALL
   state explicitly that Proxygen's WebSocket support and its QUIC/HTTP-3
   support (via the `mvfst` dependency this project's vcpkg port already
   pulls in transitively — confirmed while writing this document) are both
   out of scope
4. WHEN documentation for this feature is written THEN the system SHALL
   state explicitly that this feature does not touch
   `include/raft/tcp_rpc.hpp`/`tls_tcp_rpc.hpp` (the actual production RPC
   transport) or the CoAP transport
5. WHEN documentation for this feature is written THEN the system SHALL
   provide a short example (modeled on
   `examples/raft/beast_transport_example.cpp`) showing `proxygen_client`/
   `proxygen_server` instantiated for the same `Types` bundle shape, and a
   second, explicitly-labeled example demonstrating the Folly fast path
   (Requirement 16) actually being taken (verifiable via Requirement 12.6's
   metrics distinction), not merely asserted in prose

### Requirement 19: Testing Rigor

**User Story:** As a developer, I want the Proxygen transport validated with
the same rigor as both existing transports, so that I can trust its
correctness and concept compliance — and, specifically for Requirement 16's
fast path, that it is actually exercised by the test suite and not merely
present but untested — before it is used anywhere real.

#### Acceptance Criteria

1. WHEN `proxygen_client<Types>`/`proxygen_server<Types>` are compiled THEN
   the system SHALL `static_assert` their compliance with
   `kythira::network_client`/`kythira::network_server`, mirroring both
   existing transports' assertion pattern
2. WHEN a test suite is written for this feature THEN the system SHALL add
   a parallel `tests/proxygen_*` suite (modeled on
   `tests/beast_*`/`tests/http_*`) covering request/response round-trips,
   TLS (mutual and server-only), timeout enforcement, connection pooling,
   malformed-request handling, and concurrent RPC correctness
   (Requirement 3.4-3.5)
3. WHEN the Folly fast path (Requirement 16) is tested THEN the system SHALL
   include at least one test that positively confirms it was taken (not
   merely that the RPC succeeded — success alone does not distinguish which
   path ran), using Requirement 12.6's metrics distinction or an
   equivalent compile-time-visible signal, run under
   `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly` specifically, plus at least one
   test confirming the generic bridge path is used instead under
   `KYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec`/`=boost`
4. WHEN property-based tests are written for this feature THEN the system
   SHALL follow this project's existing property-test tagging convention
   (`**Feature: proxygen-http-transport, Property N: ...**`) and
   `BOOST_AUTO_TEST_CASE` timeout requirements
5. WHEN the cpp-httplib, Beast, and Proxygen transports are all exercised
   against equivalent `Types` bundles and the same RPC sequence THEN the
   system SHALL produce equivalent externally-observable results (response
   content, error classification into the same status-code categories),
   extending `.kiro/specs/boost-beast-http-transport/` Requirement 16.4's
   two-way cross-transport equivalence testing to three transports
6. WHEN the Proxygen transport test suite is run THEN the system SHALL
   execute exclusively through CTest, labeled `proxygen-http`, matching
   both existing transports' Requirement 16.5/9.5 CTest-label convention

### Requirement 20: Dependency and Build Wiring

**User Story:** As a developer, I want the `proxygen` dependency added using
this project's existing optional-dependency machinery, so that building
without it degrades the same way every other optional dependency already
does — with the added wrinkle that Proxygen unconditionally requires Folly,
which is itself now an optional dependency in this project
(`doc/TODO.md`'s Folly-decoupling entries, `CONFIG_FOLLY`).

#### Acceptance Criteria

1. WHEN `vcpkg.json` is updated THEN the system SHALL add `proxygen` as a
   dependency, pinned to the version confirmed present in this project's
   vcpkg registry while writing this document (`2026.02.23.00` or newer)
2. WHEN root `CMakeLists.txt` is updated THEN the system SHALL gate
   Proxygen-transport targets using `kythira_kconfig_gate`/
   `kythira_kconfig_require` (`cmake/Kconfig.cmake`), the same mechanism
   `.kiro/specs/boost-beast-http-transport/` Requirement 17.2 already used,
   scoped per-target from the start (Requirement 17.4's precedent for *not*
   repeating the subdirectory-level-gate mistake `doc/TODO.md`'s
   Folly-decoupling entries document)
3. WHEN a Kconfig symbol for this feature is added THEN the system SHALL
   add `config PROXYGEN_TRANSPORT` to the existing "Transports" menu
   (`Kconfig`), immediately following `BOOST_BEAST_TRANSPORT`'s precedent —
   `bool`, `default n`, help text stating the dependency and linking to
   this spec directory — **and** SHALL declare `depends on FOLLY`
   (`Kconfig`'s existing `depends on` syntax, e.g.
   `HTTP_TRANSPORT_TLS`'s `depends on HTTP_TRANSPORT && OPENSSL`), since
   enabling this feature with `CONFIG_FOLLY=n` is not a degraded-but-buildable
   configuration the way most other `CONFIG_X=n` combinations are — it is
   simply not buildable, because `proxygen`'s own vcpkg port unconditionally
   depends on `folly`
4. WHEN `proxygen` and/or `folly` are unavailable (not installed,
   `CONFIG_PROXYGEN_TRANSPORT=n`, or `CONFIG_FOLLY=n`) THEN the system
   SHALL skip the Proxygen-transport targets/tests with a
   `message(STATUS ...)`, gated on the conjunction of `TARGET
   proxygen::proxygen` (or whatever target `kythira_find_optional`
   produces) **and** `TARGET Folly::folly` both being present, not either
   alone

### Requirement 21: Pre-Implementation Spike

**User Story:** As a developer, I want a pre-implementation spike that
confirms Proxygen's actual API shape against the specific version this
project will vendor, so that the design does not assume behavior Proxygen's
documentation-only description suggests but its real, vendored headers do
not actually provide — following the same precedent
`.kiro/specs/boost-beast-http-transport/` Requirement 18 and
`.kiro/specs/stdexec-future-backend/`/`.kiro/specs/boost-future-backend/`'s
own spikes already established, and given this document's own several
`design.md`-deferred "the spike SHALL settle this" markers (Requirements 8.1,
10.1, 15.3, 16.3).

#### Acceptance Criteria

1. WHEN implementation begins THEN the system SHALL first record, in a
   `spike-notes.md` in this spec's directory, the exact `proxygen`/`folly`/
   `wangle`/`fizz` versions vcpkg resolves for this project's
   `builtin-baseline`
2. WHEN the spike is performed THEN the system SHALL confirm empirically
   (a throwaway compile-and-run against a real socket) that
   `proxygen::HTTPServer::start()` genuinely blocks its calling thread as
   its header documentation states (Requirement 5.1's design depends on
   this), and confirm the specific mechanism (`onSuccess` callback timing
   relative to `start()`'s own blocking behavior) by which a caller can
   reliably learn "now accepting" from another thread
3. WHEN the spike is performed THEN the system SHALL confirm whether a
   single `folly::IOThreadPoolExecutorBase` can genuinely be shared between
   a `proxygen_client` and a `proxygen_server` in the same process
   (Requirement 8), and whether `HTTPConnector::connect`'s own
   `folly::EventBase*` parameter is best satisfied by
   `executor->getEventBase()` (round-robin per call) or one
   `EventBase` pinned for a given target node's whole connection lifetime
   — record which, and why
4. WHEN the spike is performed THEN the system SHALL confirm, with a real
   compile-and-run (not documentation reading alone), that Requirement 16's
   central technical claim holds: a `folly::Promise<T>` fulfilled from
   inside a `proxygen::HTTPTransaction::Handler` callback, wrapped into a
   `kythira::Future<T>` via `explicit Future(folly_type ff)`, behaves
   correctly end to end (no lifetime hazard analogous to the Beast spec's
   own `thenValue`-flattening-capture bug — Requirement 21.5 below) and
   that scheduling a continuation on the fulfilling `EventBase` avoids an
   observable extra thread hop relative to the generic bridge path
5. WHEN the spike is performed THEN the system SHALL specifically stress-test
   the fast path's promise/future lifetime across an asynchronous boundary,
   given `.kiro/specs/boost-beast-http-transport/`'s own experience finding
   two independent heap-use-after-free bugs in superficially similar
   Folly-future composition code — this spike's job is to determine whether
   an analogous hazard exists here, not to assume it does not merely
   because the Beast spec's specific fix (`thenValue` capture rules) does
   not directly apply to a differently-shaped completion callback
6. WHEN the spike is performed THEN the system SHALL confirm whether this
   project's existing OpenSSL context-configuration knowledge (certificate
   loading, cipher suite restriction, min/max TLS version) transfers
   directly to `folly::SSLContext`'s/`wangle::SSLContextConfig`'s own
   method names (Glossary), or needs translation, and record which
7. WHEN the spike concludes THEN the system SHALL record the specific
   Proxygen/Folly/wangle/fizz releases and minimum compiler versions
   validated, matching `.kiro/specs/boost-beast-http-transport/` Requirement
   18.5's precedent
