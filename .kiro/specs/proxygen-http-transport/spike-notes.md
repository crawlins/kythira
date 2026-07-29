# Spike Notes — Proxygen HTTP Transport

**Date**: 2026-07-28
**Proxygen/Folly/Wangle/Fizz version**: `2025.05.19.00` (vcpkg
`version-string`, confirmed via `/home/clark/vcpkg/ports/{proxygen,folly,wangle,fizz}/vcpkg.json`
at this project's exact `builtin-baseline` commit
`9a7f7340a6c5f11f24c3d59f85e07143feb84e06` — the local vcpkg checkout's
`HEAD` matches this commit exactly). **Correction to `requirements.md`**:
that document's Introduction and Requirement 20.1 state
`proxygen`/`2026.02.23.00` "confirmed present... while writing this
document" — the actual resolvable version at this project's pinned
baseline is `2025.05.19.00`. `vcpkg.json` and this feature's Kconfig help
text have been written against the real, confirmed version.
**Compiler**: matching this project's existing toolchain (`g++`/`clang++`,
`-std=c++23`).

## Method

Direct inspection of the vendored `proxygen`/`folly`/`wangle` headers
(`vcpkg buildtrees` — the actual source `vcpkg` fetched and is building
from, not upstream documentation or an assumed API shape), cross-checked
against a real `vcpkg install` of the full dependency chain
(`fizz`/`wangle`/`mvfst`/`proxygen`, ~226 packages total from this
project's manifest), followed by compiling this feature's own
implementation and test suite against the result. Findings below are
split into **header-inspection-confirmed** (verified by reading the real,
vendored header) and **compile/run-confirmed** (verified by actually
building and executing code) — the latter is authoritative where the two
could in principle diverge.

## Findings

### Finding 1: `proxygen::HTTPServer::start()` genuinely blocks, `onSuccess`/`onError` fire from the event loop — confirmed by header inspection

`proxygen/httpserver/HTTPServer.h`'s own doc comment: "Note this is a
blocking call and the current thread will be used to listen for incoming
connections... `onSuccess` callback will be invoked from the event loop
which shows that all the setup was successfully done." `stop()`'s own
comment confirms the opposite of a graceful drain: "Server will stop
listening for new connections and drop all connections immediately."
**Resolution**: `design.md`'s Phase 3 (dedicated `_server_thread`,
condition-variable-signaled readiness, an explicit drain step in front of
`HTTPServer::stop()`) proceeds exactly as designed —
`proxygen_server<Types>::start()`/`stop()` implement this in
`include/raft/proxygen_http_transport_impl.hpp`.

### Finding 2: `proxygen::HTTPTransaction::Handler` is an alias for `HTTPTransactionHandler`, a **10**-pure-virtual-method interface, not 4 — correction to `design.md`'s Phase 4 code sample

`design.md`'s Phase 4 sketch implements only
`onHeadersComplete`/`onBody`/`onEOM`/`onError` on `transaction_bridge`.
The real interface (`proxygen/lib/http/session/HTTPTransaction.h`) is a
top-level class `HTTPTransactionHandler` (not a nested `HTTPTransaction::Handler`
class — `HTTPTransaction::Handler` is a `using` alias for it, so
`design.md`'s naming still compiles, just isn't the class's real name) with
**ten** pure virtual methods: `setTransaction`, `detachTransaction`,
`onHeadersComplete`, `onBody`, `onTrailers`, `onEOM`, `onUpgrade`,
`onError`, `onEgressPaused`, `onEgressResumed`. `setTransaction`/
`detachTransaction` in particular are easy to miss from documentation
alone (design.md's sketch did) but are not optional. **Resolution**:
`transaction_bridge`/`folly_transaction_bridge`
(`include/raft/proxygen_http_transport.hpp`) implement all ten;
`onTrailers`/`onUpgrade`/`onEgressPaused`/`onEgressResumed` are no-ops
(HTTP/1.1 request/response scope, Requirement 18.2), `detachTransaction`
self-deletes and — Finding 2's own defensive addition — fulfills the
promise with an exception if neither `onEOM` nor `onError` already did, a
safety net `design.md`'s original sketch had no need to consider since it
didn't know `detachTransaction` was pure virtual and always invoked.

### Finding 3: `HTTPUpstreamSession` is genuinely, permanently pinned to the `folly::EventBase` it was created on — confirmed by header inspection; determines the EventBase-assignment policy

No API on `HTTPUpstreamSession` migrates it to a different `EventBase`;
`getEventBase()` returns the one fixed at construction (via the
`folly::AsyncTransport` it wraps). **Resolution to Requirement 21.3's open
question**: `folly::IOThreadPoolExecutorBase::getEventBase()` is called
**once per target node**, the first time `proxygen_client` sees it, and
pinned for that node's entire pooled-connection lifetime
(`proxygen_detail::pooled_connection::event_base`) — not round-robin per
call. This is not a style preference: round-robining `getEventBase()`
per-call would mean a pooled session created on thread A could later be
handed a `newTransaction()` call issued (incorrectly) via thread B's
`EventBase`, which Proxygen's own threading contract simply does not
support. Beast's analogous connection-pool design didn't face this
because `boost::asio::io_context` has no equivalent per-connection thread
affinity.

### Finding 4: session liveness (Requirement 9.3) needs its own tracking — `isReusable()`/`isClosing()` exist, but reading them off the connection's own `EventBase` thread is what actually matters

`HTTPUpstreamSession::isReusable()`/`isClosing()` exist and do what their
names suggest, but per Finding 3's own threading contract, they (like
every other operation on the session) may only meaningfully be consulted
from the session's own pinned `EventBase` thread — a caller reading a raw
`HTTPUpstreamSession*` from an arbitrary calling thread to decide
"reuse or reconnect" would race the session's own internal teardown.
**Resolution**: the entire reuse-or-reconnect decision
(`proxygen_detail::connect_if_needed`) runs *inside* the
`evb->runInEventBaseThread(...)` closure that also issues the actual
`newTransaction()` call — never on the RPC's original calling thread — and
a `session_liveness_tracker` (`HTTPSessionBase::InfoCallback::onDestroy`,
itself only ever invoked on that same `EventBase` thread) resets the
pool's `std::atomic<HTTPUpstreamSession*>` slot back to `nullptr` the
moment Proxygen itself tears a session down, so the next reuse attempt
transparently falls through to reconnect (Requirement 9.3) rather than
touching a dangling pointer.

### Finding 5: `folly::SSLContext`'s client-side method names transfer directly; `wangle::SSLContextConfig`'s server-side surface is narrower than `folly::SSLContext`'s

Client-side, `folly::SSLContext::loadCertificate`/`loadPrivateKey`/
`loadTrustedCertificates`/`ciphers`/`setVerificationOption`/`getSSLCtx()`
(the direct analog of `boost::asio::ssl::context::native_handle()`) all
exist with exactly the names `design.md`/`requirements.md` assumed —
confirmed by reading the vendored `folly/io/async/SSLContext.h` (already
installed in this project's `vcpkg_installed/`, since `folly` predates
this feature as a dependency). Server-side, `wangle::SSLContextConfig`
(`wangle/ssl/SSLContextConfig.h`) has `setCertificate(cert, key, password)`,
`sslCiphers`/`sslCiphersuites`, `clientCAFile`, `clientVerification` — but
only a single `sslVersion` **floor** (`folly::SSLContext::SSLVersion`), no
independent max-version field the way `folly::SSLContext`'s own
`SSL_CTX_set_min_proto_version`/`SSL_CTX_set_max_proto_version` pair (used
directly, client-side) provides. **Resolution (Requirement 11.3
precedent)**: `proxygen_server_config::max_tls_version` is still validated
(range-checked against `min_tls_version` at configure time, same as both
existing transports) but has no further server-side enforcement point on
this config surface — documented in `build_ssl_context_config()`'s own
comment (`proxygen_http_transport_impl.hpp`) rather than silently ignored.
TLS 1.3 is offered automatically by fizz/wangle whenever the peer supports
it regardless of this floor.

### Finding 6: server-side, Proxygen's own higher-level `RequestHandler`/`ResponseBuilder` API is a better fit than a raw `HTTPTransactionHandler` — a deliberate design refinement over `design.md`'s original sketch

`design.md`'s Phase 4 only describes the client-side generic bridge in
detail and doesn't fully specify the server-side handler shape. Direct
inspection of `proxygen/httpserver/{RequestHandler,RequestHandlerFactory,ResponseBuilder}.h`
shows this is Proxygen's own documented, intended extension point for
exactly this feature's use case (register a handler chain via
`HTTPServerOptions::handlerFactories`, respond via
`ResponseBuilder(downstream_).status(...).body(...).sendWithEOM()`) — both
simpler and more idiomatic than reimplementing routing/response-writing
against the lower-level `HTTPTransactionHandler` interface the client side
necessarily uses (no equivalent high-level API exists client-side in this
Proxygen version). **Resolution**: `proxygen_server<Types>` is implemented
against `proxygen::RequestHandler`/`RequestHandlerFactory`/`ResponseBuilder`
(`proxygen_detail::rpc_request_handler`/`rpc_handler_factory`), not a
server-side `HTTPTransactionHandler`.

### Finding 7: the Folly-Promise-into-`kythira::Future` fast path (Requirement 16's central claim) — compile/run-confirmed via the test suite itself, not a separate throwaway program

Given this project's existing precedent (every prior spike, including
Beast's own two `AddressSanitizer`-caught heap-use-after-free bugs) treats
a real compile-and-run as authoritative over documentation reading, and
given the scope of this feature, the fast path's correctness was
validated directly through `tests/proxygen_transport_test.cpp`'s
`folly_fast_path_is_taken` test (confirms, via the
`proxygen_http.client.request.sent`/`path=folly_fast_path` metrics
dimension — Requirement 12.6 — that the dispatch in `send_rpc` actually
selects `send_rpc_folly_fast_path`, not merely that the RPC succeeded) and
the full round-trip/concurrency/TLS test suite exercising that path under
real `ThreadSanitizer`/`AddressSanitizer` builds
(`build-asan`/`build-clang`, this project's existing sanitizer build
variants). No lifetime hazard analogous to the Beast spec's two
`thenValue`-flattening bugs was found: the fast path's `folly::Promise<T>`
is constructed on the calling thread, moved (not copied) into the
`runInEventBaseThread` closure, and settled exactly once via a single
`.thenTry()` continuation (not split across separate `.thenValue()`/
`.thenError()` captures of the same moved-from promise — an actual bug
this implementation initially had and fixed during development, see
`proxygen_http_transport_impl.hpp`'s own comment at that call site).

### Finding 8: dependency versions and minimum toolchain

`proxygen`/`folly`/`wangle`/`fizz`/`mvfst`: `2025.05.19.00` (all from the
same Meta OSS release train, resolved via this project's `builtin-baseline`).
Compiler/standard: this project's existing `g++`/`clang++` at `-std=c++23`
(no new floor introduced — Proxygen's own CMake requires C++17 minimum,
well below this project's C++23 baseline).

## Conclusions

- **Two real corrections to `requirements.md`/`design.md`**: the Proxygen
  version number (Finding, header above) and the
  `HTTPTransactionHandler` interface's true method count (Finding 2) —
  both incorporated into the implementation directly (this document is the
  durable record of the discrepancy, per this project's own established
  spike-notes precedent).
- **One deliberate design refinement over the original sketch**: the
  server side uses Proxygen's own higher-level `RequestHandler` API
  (Finding 6) rather than a raw `HTTPTransactionHandler`, since Proxygen
  itself provides and documents it as the intended extension point.
- **Requirement 16's central risk (a lifetime hazard analogous to Beast's
  two real bugs) was not found** — but a *different*, real bug (accidental
  double-move of a captured `promise` across split `thenValue`/`thenError`
  continuations) was found and fixed during this implementation's own
  development, underscoring why this spike step insists on a real
  compile-and-run rather than a design read-through (Finding 7).
- **EventBase assignment policy settled**: one `EventBase` pinned per
  target node for that node's whole connection lifetime, not round-robin
  per call (Finding 3) — required for correctness, not merely a style
  choice.
- **Canonical Types bundle name**: `kythira::future_default_proxygen_transport_types`
  (struct) / `kythira::proxygen_future_default_transport_types` (concept) —
  named distinctly from Beast's `future_default_http_transport_types`/
  `future_default_transport_types` specifically to avoid a
  duplicate-concept-definition error in a translation unit that includes
  both transports' headers (the three-way cross-transport equivalence
  test), settling Requirement 15.3's naming question.
