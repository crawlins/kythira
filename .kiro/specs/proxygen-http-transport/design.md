# Design Document

## Overview

This design adds a third implementation of `kythira::network_client`/
`kythira::network_server`, backed by Proxygen, alongside the existing
cpp-httplib-backed (`include/raft/http_transport.hpp`) and Boost.Beast-backed
(`include/raft/beast_http_transport.hpp`) implementations. It does not touch
any existing production call site, does not change which transport
`cmd/ca_service`/`cmd/ca_cluster_node`/any existing example uses, and does
not modify `network_client`, `network_server`, or `transport_types`.

Where the Beast spec's central design problem was introducing this project's
first `io_context`-driven asynchronous I/O model from scratch
(`.kiro/specs/boost-beast-http-transport/design.md`'s Overview), this
feature's central design problem is different in kind: **the asynchronous
model already exists in the library, but it is Folly's, not Boost.Asio's,
and it has a genuinely different threading shape** (single-threaded
`EventBase` instances pooled via `folly::IOThreadPoolExecutor`, rather than
one `io_context` shared by N threads). This design's job is to (a) bridge
that model into the same `network_client`/`network_server`/`transport_types`
concept surface Beast already proved out, generically, for any
`KYTHIRA_DEFAULT_FUTURE_BACKEND`, and (b) — the part with no precedent in
either existing transport — add an optional fast path that exploits the one
case where Proxygen's *internal* future type and this project's
*caller-visible* future type are the same underlying library, skipping a
translation hop neither cpp-httplib nor Beast has any equivalent of, because
neither is built on Folly internally.

## Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│  Concept Layer                                                          │
│  network_client / network_server (include/raft/network.hpp)            │
│  transport_types (include/raft/types.hpp)                              │
│  — unmodified by this feature                                          │
└──────┬─────────────────────┬──────────────────────────┬────────────────┘
       │ satisfies            │ satisfies                 │ satisfies
┌──────▼──────────┐  ┌────────▼──────────┐   ┌────────────▼───────────────┐
│ cpp-httplib      │  │ Boost.Beast        │   │ Proxygen (new)              │
│ (existing)       │  │ (existing)          │   │                              │
│ blocking calls;  │  │ async ops on        │   │ async ops on caller-shared   │
│ server owns its  │  │ caller-owned        │   │ folly::IOThreadPoolExecutor  │
│ own thread       │  │ net::io_context     │   │ (N independent EventBases,   │
│                   │  │ (N threads, 1       │   │ each single-threaded —       │
│                   │  │  io_context)        │   │  Requirement 8)              │
└──────┬────────────┘  └────────┬────────────┘   └───────────┬──────────────┘
       │                          │                              │
  httplib::Client/Server   beast::tcp_stream/            HTTPConnector /
  (OpenSSL via              ssl_stream<tcp_stream>        HTTPUpstreamSession /
   httplib::SSLServer)      (OpenSSL via                  HTTPServer
                             boost::asio::ssl::context)    (OpenSSL via
                                                            folly::SSLContext /
                                                            wangle::SSLContextConfig)
                                                                    │
                                                        ┌───────────┴────────────┐
                                                        │ Generic bridge          │
                                                        │ (any future backend,    │
                                                        │  Requirement 14) OR     │
                                                        │ Folly fast path         │
                                                        │ (Requirement 16,        │
                                                        │  folly backend only)    │
                                                        └─────────────────────────┘
```

All three transports are reached only through `Types` template arguments
already satisfying `transport_types` — nothing about which transport a piece
of generic Raft code uses changes based on this feature landing
(Requirement 13).

### Why Proxygen's own async model, and why it needs two different threading stories (client vs. server)

Considered and rejected: treat Proxygen the same way Beast treated
Boost.Asio — one caller-owned resource (there, `io_context&`; here, one
`folly::EventBase&`) handed identically to both the client and the server.
This does not work for Proxygen, for a reason discovered by direct
inspection of the vendored headers while writing this document, not by
assumption:

- **Client side** (`proxygen::HTTPConnector::connect`/`connectSSL`) takes an
  explicit `folly::EventBase*` per call and is genuinely non-blocking —
  this maps cleanly onto "caller shares one resource, transport issues
  non-owning calls against it," the same shape Beast's client already has.
- **Server side** (`proxygen::HTTPServer::start(onSuccess, onError,
  getAcceptorFactory, ioExecutor)`) **blocks the calling thread**, running
  the event loop itself until `stop()` is called from elsewhere — this is
  Proxygen's own documented behavior (`proxygen/httpserver/HTTPServer.h`),
  confirmed while writing this document, not merely inferred from
  Boost.Asio-shaped intuition. This is structurally closer to
  `cpp_httplib_server`'s `httplib::Server::listen()` (also a blocking call,
  which `cpp_httplib_server` already absorbs into its own dedicated
  `_server_thread`) than to Beast's `async_accept`-issued-from-a-caller-driven-loop
  model.

`proxygen_server<Types>` therefore owns and joins its own dedicated thread
to run `HTTPServer::start()` on — mirroring `cpp_httplib_server`'s existing
pattern, not Beast's — while `proxygen_client<Types>` takes a caller-shared
`folly::IOThreadPoolExecutorBase&`, mirroring Beast's client-side shape.
This asymmetry is a direct, unavoidable consequence of Proxygen's own API
shape (Requirement 5, Requirement 8), not a design preference — recorded
here explicitly, in the same spirit as
`.kiro/specs/boost-beast-http-transport/design.md`'s "Why the asynchronous
Beast API, not the synchronous one" section states its own rejected
alternative and why.

### Why `folly::IOThreadPoolExecutor`, not one shared `EventBase`, on the client side

A single `folly::EventBase` "can only drive an event loop for a single
thread" (`folly/io/async/EventBase.h`'s own header comment) — unlike
`boost::asio::io_context`, which explicitly supports multiple threads
calling `run()` concurrently on the same `io_context`. Sharing one
`EventBase` across a multi-core process would mean every Proxygen operation
in the process serializes onto one thread — a strictly worse concurrency
story than even `cpp_httplib_client`'s "N executor slots, each blocking
independently" model. `folly::IOThreadPoolExecutor` is Folly's own answer:
a pool of independent `EventBase`s, one per thread, with
`getEventBase()`/round-robin dispatch handing out one to each new unit of
work. This is also the type `proxygen::HTTPServer::start()`'s own
`ioExecutor` parameter accepts, so the same executor can, per Requirement 8,
be shared between a `proxygen_client` and a `proxygen_server` in the same
process — the direct analog of Beast's single shared `io_context`, just
built from a pool of single-threaded loops instead of one multi-threaded
one.

One structural consequence worth stating plainly, because it changes what
this feature has to build versus what Beast had to: **a connection, once
established on a given `EventBase`, stays on that `EventBase`'s thread for
its entire lifetime** — Proxygen never migrates an `HTTPUpstreamSession`
between threads. This gives same-connection operation ordering "for free"
(Requirement 3.5, Property 6 below) — Beast needed an explicit
`net::strand` per connection to get the equivalent guarantee
(`.kiro/specs/boost-beast-http-transport/design.md` Phase 3), because
`io_context`'s shared-thread-pool model does not provide it structurally.
This feature does not need a strand-equivalent primitive of its own
construction.

## Components and Interfaces

### Phase 0: Spike (Requirement 21)

Before committing to exact adaptor code, confirm against the actually
vendored `proxygen`/`folly`/`wangle`/`fizz` versions:
- `proxygen::HTTPServer::start()` genuinely blocks its calling thread as
  documented, and the precise timing contract of its `onSuccess` callback
  relative to that blocking behavior (a throwaway compile-and-run, not
  documentation reading alone — Requirement 21.2).
- Whether one `folly::IOThreadPoolExecutorBase` can genuinely be shared
  between a `proxygen_client` and a `proxygen_server` in the same process,
  and whether `HTTPConnector::connect`'s `EventBase*` argument is better
  satisfied by `executor->getEventBase()` per call or one `EventBase`
  pinned per target node for that node's whole connection lifetime
  (Requirement 21.3).
- Requirement 16's central technical claim — a `folly::Promise<T>`
  fulfilled inside an `HTTPTransaction::Handler` callback, wrapped into
  `kythira::Future<T>` via `explicit Future(folly_type ff)`, end to end,
  with the same scrutiny the Beast spec's own two real
  `AddressSanitizer`-caught heap-use-after-free bugs (its design.md's Data
  Models section) earned there. This spike's job is specifically to check
  whether an analogous lifetime hazard exists for Proxygen's own
  callback-into-Promise shape, not to assume the Beast fix's specific
  mechanism (recapturing state in a later `thenValue` continuation)
  transfers unchanged, since Proxygen's callback interface is a subclassed
  virtual-method handler, not a chain of lambdas — the hazard, if any,
  would have a different shape (Requirement 21.5).
- Whether this project's existing OpenSSL context-configuration knowledge
  transfers directly to `folly::SSLContext`/`wangle::SSLContextConfig`'s
  method names, or needs translation (Requirement 21.6).
- Record the exact Proxygen/Folly/wangle/fizz releases and minimum compiler
  versions validated, in `spike-notes.md`.

The rest of this section describes the design assuming the spike confirms
its working assumptions; any correction the spike surfaces is recorded in
`spike-notes.md` and reflected back into this document, following
`.kiro/specs/boost-beast-http-transport/`'s own precedent for a design
document that was updated post-implementation where reality diverged from
the original sketch (that spec's Data Models section, Connection Pool
Entry).

### Phase 1: Dependency and Build Wiring (Requirement 20)

```json
// vcpkg.json — new dependency
{
  "name": "proxygen",
  "version>=": "2026.02.23.00"
}
```

```cmake
# CMakeLists.txt, alongside the existing kythira_find_optional(FOLLY folly) /
# kythira_kconfig_gate(BOOST_BEAST_TRANSPORT) blocks — same optional-dependency
# machinery every other optional dependency in this project already uses.
kythira_find_optional(PROXYGEN_TRANSPORT proxygen CONFIG)

kythira_kconfig_gate(PROXYGEN_TRANSPORT)
set(KYTHIRA_BUILD_PROXYGEN_TRANSPORT FALSE)
if(_KYTHIRA_GATE_PROXYGEN_TRANSPORT AND TARGET proxygen::proxygen AND TARGET Folly::folly)
    set(KYTHIRA_BUILD_PROXYGEN_TRANSPORT TRUE)
    message(STATUS "Proxygen HTTP transport enabled (.kiro/specs/proxygen-http-transport/)")
else()
    message(STATUS "Proxygen HTTP transport disabled "
                    "(proxygen and/or Folly not found, or CONFIG_PROXYGEN_TRANSPORT=n)")
endif()
kythira_kconfig_require(PROXYGEN_TRANSPORT KYTHIRA_BUILD_PROXYGEN_TRANSPORT "proxygen::proxygen")
```

```kconfig
# Kconfig, "Transports" menu, immediately after BOOST_BEAST_TRANSPORT
config PROXYGEN_TRANSPORT
	bool "Proxygen HTTP transport (third implementation, alongside cpp-httplib and Beast)"
	default n
	depends on FOLLY
	help
	  Adds a third network_client/network_server implementation backed by
	  Meta's Proxygen library, satisfying the same concepts as the existing
	  cpp-httplib (include/raft/http_transport.hpp) and Boost.Beast
	  (include/raft/beast_http_transport.hpp) transports without replacing
	  either. Requires the proxygen vcpkg package, which itself
	  unconditionally depends on folly -- unlike most CONFIG_X=n
	  combinations in this project, CONFIG_PROXYGEN_TRANSPORT=y with
	  CONFIG_FOLLY=n is not a valid, degraded-but-buildable configuration.
	  See .kiro/specs/proxygen-http-transport/ for the full design,
	  including the optional Folly-native fast path this feature adds when
	  both this and CONFIG_FOLLY (as the selected default backend) are
	  active.
```

The `depends on FOLLY` line is the one genuinely new wrinkle relative to
Beast's own Phase 1 (`.kiro/specs/boost-beast-http-transport/design.md`
Phase 1) — Beast added no new transitive dependency beyond
`boost-asio`, already required; this feature's dependency (`proxygen`)
transitively but unconditionally requires `folly`, itself now optional in
this project (`doc/TODO.md`'s Folly-decoupling entries, `CONFIG_FOLLY`).
Gating on the conjunction of both targets being present (not either alone)
is Requirement 20.4's whole point.

### Phase 2: Client — `folly::IOThreadPoolExecutorBase` Ownership (Requirement 8)

```cpp
namespace kythira {

// proxygen_client takes a caller-shared executor, never owns one -- the
// direct analog of boost_beast_client's io_context&, adapted to Proxygen's
// own thread-pool-of-EventBases model (see design.md's "Why
// folly::IOThreadPoolExecutor" section for why this isn't one EventBase&).
template<typename Types>
requires kythira::transport_types<Types>
class proxygen_client {
public:
    proxygen_client(folly::IOThreadPoolExecutorBase& io_executor,
                     std::unordered_map<std::uint64_t, std::string> node_id_to_url_map,
                     proxygen_client_config config, typename Types::metrics_type metrics);
    // ...
private:
    folly::IOThreadPoolExecutorBase& _io_executor;  // non-owning
    // ...
};

}  // namespace kythira
```

A short usage example (Requirement 18.5, modeled on
`examples/raft/beast_transport_example.cpp`) makes the shared-executor
pattern concrete, and highlights the client/server asymmetry Phase 3 below
explains:

```cpp
auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(4);

kythira::proxygen_client<my_transport_types> client{*io_executor, node_map, {}, metrics};
kythira::proxygen_server<my_transport_types> server{"0.0.0.0", 8080, {}, metrics, io_executor};
server.start();   // owns + joins its own thread internally (Phase 3) --
                   // unlike boost_beast_server::start(), this does block
                   // internally until proxygen::HTTPServer signals
                   // onSuccess, then returns to the caller
// ... use client/server ...

server.stop();     // stops accepting, drains in-flight requests, joins its
                    // own thread -- does NOT stop io_executor
io_executor.reset();  // caller's own responsibility, only once nothing else
                       // needs it (mirrors Beast's ioc.stop() ownership split)
```

### Phase 3: Server — Blocking `start()` Reconciliation (Requirement 5)

```cpp
namespace kythira {

template<typename Types>
requires kythira::transport_types<Types>
class proxygen_server {
public:
    proxygen_server(std::string bind_address, std::uint16_t bind_port,
                     proxygen_server_config config, typename Types::metrics_type metrics,
                     std::shared_ptr<folly::IOThreadPoolExecutorBase> io_executor = nullptr);

    auto start() -> void;   // Requirement 5.1
    auto stop() -> void;    // Requirement 5.2
    auto is_running() const -> bool;

private:
    std::shared_ptr<folly::IOThreadPoolExecutorBase> _io_executor;  // may be
        // self-constructed (nullptr passed in) or caller-shared -- stop()
        // only tears down the self-constructed case (Requirement 8.4)
    std::unique_ptr<proxygen::HTTPServer> _http_server;
    std::thread _server_thread;   // owns HTTPServer::start()'s blocking call,
        // the same shape cpp_httplib_server's own _server_thread already
        // uses for httplib::Server::listen()'s equally blocking call
    std::mutex _start_mutex;
    std::condition_variable _start_cv;
    bool _accepting{false};

    // start(): constructs _http_server, spawns _server_thread running
    //   _http_server->start(onSuccess = [this]{ signal _accepting via
    //   _start_cv }, onError = [this](std::exception_ptr){ signal failure
    //   via the same cv }), then the calling thread blocks on _start_cv
    //   until onSuccess/onError fires -- giving start() the same
    //   "returns once actually accepting" contract cpp_httplib_server and
    //   boost_beast_server already have, despite HTTPServer::start() itself
    //   never returning until stop().
};

}  // namespace kythira
```

`stop()`: closes the acceptor first
(`proxygen::HTTPServer::stopListening()`), then drains in-flight
sessions — Proxygen's own `HTTPServer::stop()` documents that it "drop[s]
all connections immediately," the opposite of what Requirement 5.2 needs, so
this feature layers its own drain step in front of it, tracking live
sessions the same way `boost_beast_server::register_session`/
`session_finished` already do
(`.kiro/specs/boost-beast-http-transport/design.md` Phase 4), then calls
`proxygen::HTTPServer::stop()` only once the drain condition variable
reports zero live sessions, then joins `_server_thread`.

### Phase 4: Generic Async Composition (Requirements 14, 15)

Mirrors `.kiro/specs/boost-beast-http-transport/design.md` Phase 2.5's
shape exactly, adapted to Proxygen's callback interface instead of Beast's
`async_*` functions:

```cpp
namespace kythira::proxygen_detail {

// Bridges proxygen::HTTPConnector::Callback into a
// kythira::promise_default<HTTPUpstreamSession*> -- same shape as Beast's
// async_connect_kf, adapted to a subclassed-handler interface instead of a
// completion-handler lambda, since HTTPConnector::Callback is a pure
// virtual interface, not a callback parameter.
class connect_bridge : public proxygen::HTTPConnector::Callback {
public:
    explicit connect_bridge(kythira::promise_default<proxygen::HTTPUpstreamSession*> promise)
        : _promise(std::move(promise)) {}
    auto connectSuccess(proxygen::HTTPUpstreamSession* session) -> void override {
        _promise.setValue(session);
    }
    auto connectError(const folly::AsyncSocketException& ex) -> void override {
        _promise.setException(std::make_exception_ptr(ex));
    }
private:
    kythira::promise_default<proxygen::HTTPUpstreamSession*> _promise;
};

// Bridges proxygen::HTTPTransaction::Handler into a
// kythira::promise_default<std::string> (the accumulated response body) --
// same shape as Beast's async_read_kf, adapted to Proxygen's multi-callback
// (onHeadersComplete/onBody/onEOM/onError) interface rather than a single
// async_read completion.
class transaction_bridge : public proxygen::HTTPTransaction::Handler {
public:
    explicit transaction_bridge(kythira::promise_default<std::string> promise)
        : _promise(std::move(promise)) {}
    auto onHeadersComplete(std::unique_ptr<proxygen::HTTPMessage> msg) noexcept -> void override;
    auto onBody(std::unique_ptr<folly::IOBuf> chain) noexcept -> void override;
    auto onEOM() noexcept -> void override { _promise.setValue(std::move(_body)); }
    auto onError(const proxygen::HTTPException& error) noexcept -> void override {
        _promise.setException(std::make_exception_ptr(std::runtime_error(error.what())));
    }
    // onTrailers/onUpgrade/onEgressPaused/onEgressResumed: no-ops for this
    // feature's HTTP/1.1-request-response-only scope (Requirement 18.2)
private:
    kythira::promise_default<std::string> _promise;
    std::string _body;
};

}  // namespace kythira::proxygen_detail
```

**The RPC send path becomes a `thenValue` chain**, identical in spirit to
Beast's:

```cpp
template<typename Request, typename Response>
auto proxygen_client<Types>::send_rpc(std::uint64_t target, std::string_view endpoint,
                                       const Request& request, std::chrono::milliseconds timeout)
    -> typename Types::template future_template<Response> {
    if constexpr (std::same_as<typename Types::template future_template<Response>,
                                kythira::Future<Response>>) {
        return send_rpc_folly_fast_path<Request, Response>(target, endpoint, request, timeout);  // Requirement 16
    } else {
        auto& conn = get_or_create_connection(target);            // Requirement 9
        auto body = _serializer.serialize(request);

        return connect_if_needed(conn, timeout)                    // Requirement 10.3
            .thenValue([this, &conn, body = std::move(body), endpoint](proxygen::HTTPUpstreamSession*) {
                return proxygen_detail::send_on_session(conn, endpoint, body, timeout);  // Req 10.1
            })
            .thenValue([this](std::string response_body) {
                return _serializer.template deserialize<Response>(response_body);  // Req 3.2
            })
            .thenError([](std::exception_ptr e) -> Response {
                std::rethrow_exception(e);  // Req 3.3, 10.2
            });
    }
}
```

The `if constexpr` dispatch is the whole of Requirement 16's compile-time
condition — no macro, no `#ifdef`, decided purely from `Types`'s own member
type (Requirement 16.1, 16.5).

### Phase 5: Folly Fast Path (Requirement 16)

```cpp
template<typename Types>
template<typename Request, typename Response>
auto proxygen_client<Types>::send_rpc_folly_fast_path(std::uint64_t target, std::string_view endpoint,
                                                        const Request& request,
                                                        std::chrono::milliseconds timeout)
    -> kythira::Future<Response> {
    // Raw folly::Promise<T>, not kythira::promise_default<T> -- no generic
    // bridge type is constructed on this path at all (Requirement 16.2).
    auto promise = std::make_shared<folly::Promise<Response>>();
    auto folly_future = promise->getFuture();

    auto& conn = get_or_create_connection(target);
    auto* evb = conn.event_base;   // the EventBase this connection is
                                     // pinned to (design.md's Architecture
                                     // section) -- also where the
                                     // continuation below runs, avoiding an
                                     // extra cross-thread post the generic
                                     // bridge does not itself guarantee
                                     // avoiding (Requirement 16.3)
    auto body = _serializer.serialize(request);

    connect_if_needed(conn, timeout)
        .via(evb)
        .thenValue([this, &conn, body = std::move(body), endpoint, promise](proxygen::HTTPUpstreamSession*) {
            return proxygen_detail::send_on_session_folly(conn, endpoint, body, timeout);
        })
        .via(evb)
        .thenValue([this, promise](std::string response_body) {
            promise->setValue(_serializer.template deserialize<Response>(response_body));  // Req 3.2
        })
        .thenError([promise](folly::exception_wrapper ew) {
            promise->setException(std::move(ew));  // Req 3.3, 10.2
        });

    // The one translation this path cannot avoid: folly::Future<T> ->
    // kythira::Future<T>, via the already-existing constructor
    // (include/raft/future.hpp) -- not a new bridging mechanism
    // (Requirement 16.2).
    return kythira::Future<Response>(std::move(folly_future));
}
```

This function only exists in a translation unit where
`Types::template future_template<Response>` is `kythira::Future<Response>`,
so the `.via(evb)` and raw `folly::Promise<T>` usage never appears in a
build where the project's future backend is `stdexec`/`boost` — the
`if constexpr` branch in Phase 4 is provably unreachable there, not merely
untaken (Requirement 16.4).

### Phase 6: TLS (Requirements 6, 7)

`proxygen_client_config`/`proxygen_server_config` reuse the exact field set
from `cpp_httplib_client_config`/`cpp_httplib_server_config`/
`boost_beast_client_config`/`boost_beast_server_config` (Requirement 11).
Client-side, `ca_cert_path`/`client_cert_path`/`client_key_path`/
`cipher_suites`/`min_tls_version`/`max_tls_version` map onto
`folly::SSLContext::loadTrustedCertificates`/`loadCertificate`/
`loadPrivateKey`/`ciphers`/`setVerificationOption`, confirmed as real,
present method names on the vendored `folly::SSLContext`
(`folly/io/async/SSLContext.h`) while writing this document. Server-side,
the same fields populate a `wangle::SSLContextConfig` passed into
`proxygen::HTTPServerOptions`. Whether this project's existing
`cpp_httplib_*`/`boost_beast_*` certificate-loading *code* (not just field
names) is reusable as a shared helper or needs a third, independently
duplicated copy — matching Beast's own precedent of duplicating rather than
sharing TLS helper code across transport implementation files
(`.kiro/specs/boost-beast-http-transport/design.md` Phase 5) — is a Phase 0
spike question (Requirement 21.6).

`reload_tls_material()` follows the same all-or-nothing contract both
existing transports already use: validate everything first, construct a
*new* `folly::SSLContext`/`wangle::SSLContextConfig` from the validated
material, and swap it in for subsequently-established connections only —
retired contexts are kept alive, not destroyed, since an in-flight
`HTTPUpstreamSession` may still reference the old one.

## Data Models

### Exception Representation

Same normalization point as every backend and every existing transport
already uses: `std::exception_ptr` at the concept boundary. On the generic
bridge path, `folly::AsyncSocketException`/`proxygen::HTTPException` are
both wrapped via `std::make_exception_ptr`, matching the existing treatment
`boost-beast-http-transport/design.md`'s own Data Models section gives
`boost::system::system_error`. On the Folly fast path (Requirement 16),
`folly::exception_wrapper` is used directly where the generic path would
otherwise need `to_std_exception_ptr`/`to_folly_exception_wrapper`
(`include/raft/future.hpp`'s own `detail::` conversion helpers) — this is
itself part of the fast path's savings: no conversion at all, since
`folly::Promise<T>::setException` already accepts `folly::exception_wrapper`
natively, and `kythira::Future<T>`'s own construction from a raw
`folly::Future<T>` carries that representation through unchanged.

### Connection Pool Entry

```cpp
struct pooled_connection {
    proxygen::HTTPUpstreamSession* session{nullptr};  // non-owning; session
        // lifetime is managed by Proxygen's own reference counting
        // (HTTPSession::DestructorGuard idiom) once established
    folly::EventBase* event_base{nullptr};  // the EventBase this session is
        // pinned to for its lifetime (design.md's Architecture section) --
        // every operation on this connection, generic-bridge or fast-path,
        // is issued via this EventBase specifically, not "whichever
        // EventBase happens to be free," so Requirement 3.5's
        // no-interleaving guarantee holds regardless of which path is used
    std::chrono::steady_clock::time_point last_used;  // idle-timeout
        // eviction (Requirement 9.2) reads this, same shape as Beast's
        // own pooled_connection field of the same name and purpose
};
```

The Phase 0 spike (Requirement 21.5) may find that Proxygen's own session
lifetime management interacts with this pool's `last_used`-based eviction
differently than Beast's `unique_ptr<beast_connection>`-owned model did —
if so, this section is updated post-implementation, following
`.kiro/specs/boost-beast-http-transport/design.md`'s own precedent for
recording implementation-time refinements to an originally-sketched data
model rather than silently diverging from what was written here.

### Config Structs

`proxygen_client_config`/`proxygen_server_config`: field-for-field copies of
`cpp_httplib_client_config`/`cpp_httplib_server_config`/
`boost_beast_client_config`/`boost_beast_server_config` (Requirement 11) —
no new data model, so a call site's existing config literal is reusable
verbatim across a type-only transport swap to any of the three.

## Correctness Properties

*A property is a characteristic or behavior that should hold true across
all valid executions of a system.*

**Property 1: No Progress Without a Running Executor**
*For any* `proxygen_client` operation in flight, if no thread is currently
running the relevant `EventBase`'s loop, that operation should make no
further progress and should resume exactly where it left off once one
does; *for any* `proxygen_server`, no request is processed before `start()`
has signaled `onSuccess` and no request is processed after `stop()` begins
its drain
**Validates: Requirement 8.3**

**Property 2: Per-Step Promise Exactly-Once Fulfillment**
*For any* `connect_bridge`/`transaction_bridge` (generic path) or raw
`folly::Promise<T>` (fast path) instance, the returned future should
complete exactly once — with a value on success, an exception on failure —
regardless of which `EventBase` thread runs the fulfilling callback
**Validates: Requirements 14.1, 16.2**

**Property 3: Chain Short-Circuits on First Failure**
*For any* `send_rpc`/`send_rpc_folly_fast_path` call whose connect or send
step fails, the overall returned future should complete with that step's
exception, and no subsequent step in the chain should execute
**Validates: Requirements 3.3, 14.2**

**Property 4: Timeout Bounds the Whole RPC**
*For any* `send_rpc` call with a given `timeout`, the sum of time spent
across connect + send + receive should not exceed `timeout` even if no
individual step is itself slow
**Validates: Requirement 10.1**

**Property 5: Connection Reuse Correctness**
*For any* two RPCs to the same target node issued while a healthy
`HTTPUpstreamSession` to that node already exists, the second RPC should
reuse it rather than establishing a new one; *for any* RPC issued against a
pooled session that has since become unusable, the system should
transparently establish a fresh one rather than surfacing the staleness to
the caller
**Validates: Requirements 9.1, 9.3**

**Property 6: Structural Per-Connection Serialization**
*For any* two RPCs to the *same* target node issued concurrently, their
I/O on that node's shared `HTTPUpstreamSession` should never interleave —
*without* this feature constructing any explicit synchronization primitive
for that purpose, since both operations are only ever invoked from the
`EventBase` thread that session is pinned to, by Proxygen's own
construction; *for any* two RPCs to *different* target nodes issued
concurrently, both should be able to make progress independently,
potentially on different `EventBase` threads
**Validates: Requirement 3.4, 3.5** — the direct analog of Beast's own
Property 6, but with a materially different (structural vs.
explicitly-managed) mechanism, per design.md's "Why
`folly::IOThreadPoolExecutor`" section

**Property 7: TLS Material Reload Atomicity**
*For any* `reload_tls_material()` call, either every piece of new
certificate/key material is valid and all of it takes effect for
subsequently-established connections, or none of it does and the
previously-active material remains in effect
**Validates: Requirement 7.3**

**Property 8: Server Drain on Stop**
*For any* `stop()` call on `proxygen_server` while requests are in-flight,
every in-flight request should be allowed to complete before `stop()`
returns, and no new connection should be accepted after `stop()` is
called — even though `proxygen::HTTPServer::stop()` itself drops
connections immediately per its own documented contract, requiring this
feature's own drain step in front of it (design.md Phase 3), the same
shape `.kiro/specs/boost-beast-http-transport/design.md` Property 8's
addendum needed for the analogous gap Beast's own implementation found
**Validates: Requirement 5.2**

**Property 9: Cross-Transport Equivalence**
*For any* `Types` bundle and RPC sequence run through cpp-httplib, Beast,
and Proxygen transports, the externally-observable results (response
content, error status-code category) should be equivalent, even though
internal I/O mechanics differ across all three
**Validates: Requirement 19.5**

**Property 10: Concept Compliance**
*For any* concept `proxygen_client`/`proxygen_server` are intended to
satisfy (`network_client`, `network_server`), they should do so, verified
by `static_assert`
**Validates: Requirement 19.1**

**Property 11: Fast Path Reachability Is Exactly the Folly Backend**
*For any* `Types` bundle, the Folly fast path (Requirement 16) should be
taken if and only if `Types::template future_template<T>` is
`kythira::Future<T>` exactly — never for `stdexec`/`boost`-backed `Types`,
and never merely because `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly` was set at
CMake configure time without that also being reflected in `Types`'s own
member type
**Validates: Requirement 16.1, 16.4, 16.5**

**Property 12: Both Paths Produce Equivalent Results**
*For any* RPC sent under `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly`, forcing
the generic bridge path (a test-only escape hatch, since Property 11 would
otherwise always select the fast path in this configuration) and the fast
path should produce the same externally-observable result — this is what
makes Requirement 17's performance comparison a fair one (same behavior,
different cost), not merely two different code paths that happen to both
"work"
**Validates: Requirement 19.3**

## Error Handling

### Exception Safety Guarantees

Same three-tier structure (basic/strong/no-throw) this codebase's other
transport/future designs already use. As with Beast's own connection pool,
a failed `send_rpc`/`send_rpc_folly_fast_path` call never leaves its target
node's pooled connection in a half-written state a *subsequent* RPC could
observe — on any failure, the connection is either fully torn down or left
exactly as it was before the failed call started.

### Cancellation

`proxygen::HTTPTransaction` exposes its own timeout/cancellation surface
(the specific mechanism — `HHWheelTimer`-based idle timeout vs. an explicit
deadline check — is a Requirement 21 spike question, Requirement 10.1). A
timed-out RPC surfaces as an ordinary `std::exception_ptr`- or
`folly::exception_wrapper`-carried timeout error at the `send_rpc` boundary
(Property 4), consistent with how both existing transports and
`boost-future-backend`'s `within(timeout)` already present timeouts to
callers — no new exception category invented specifically for this
transport.

### Connection Pool Staleness

Covered by Property 5: detected lazily, on the next attempted use (no
background health-checking of connections not currently needed), matching
both existing transports' reactive-not-proactive approach.

### Server Accept-Loop Resilience

A single connection's I/O error must not stop the server from continuing to
accept and serve other connections. Since `proxygen::HTTPServer` owns its
own accept loop internally (unlike Beast's hand-written `do_accept()`), this
feature's own responsibility here is narrower than Beast's: primarily
ensuring the request-handler-level `RequestHandler`/`HTTPTransactionHandlerAdaptor`
this feature registers per RPC endpoint does not itself let one malformed
request or one handler exception propagate in a way that affects any other
in-flight session (Requirement 4.2-4.3) — the accept loop's own resilience
is Proxygen's responsibility, not something this feature re-implements.

## Testing Strategy

Mirrors `.kiro/specs/boost-beast-http-transport/` Requirement 16's/this
spec's own Requirement 19's approach, with two categories genuinely new to
this feature:

1. **Concept compliance**: `static_assert` `proxygen_client`/
   `proxygen_server` against `network_client`/`network_server`.
2. **Parallel test suite**: `tests/proxygen_client_test.cpp`,
   `tests/proxygen_server_test.cpp`, `tests/proxygen_integration_test.cpp`,
   `tests/proxygen_ssl_*` — one-to-one with the existing `tests/http_*`/
   `tests/beast_*` files, covering the same scenarios.
3. **Three-way cross-transport equivalence**: extending
   `.kiro/specs/boost-beast-http-transport/`'s own two-way property test
   (cpp-httplib vs. Beast) to include Proxygen, asserting equivalent
   externally-observable results across all three (Property 9).
4. **Fast-path-specific coverage (new)**: a test confirming the Folly fast
   path is actually taken under `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly`
   (Requirement 19.3, via the metrics distinction Requirement 12.6 adds), a
   test confirming the generic bridge is used instead under `stdexec`/
   `boost`, and a test forcing the generic bridge under the Folly backend
   specifically (Property 12's test-only escape hatch) to assert both paths
   produce equivalent results before Requirement 17's benchmark compares
   their cost.
5. **Concurrency-specific coverage**: many concurrent in-flight RPCs
   against a small `folly::IOThreadPoolExecutor` thread count
   (Requirement 3.4), most practically checked with ThreadSanitizer
   (`build-asan`), matching Beast's own precedent for this test category.
6. All tests run exclusively through CTest, labeled `proxygen-http`
   (Requirement 19.6).

## Non-Goals

- **Converting any existing production call site**, or removing/deprecating
  either existing HTTP transport.
- **HTTP/2**, despite Proxygen itself supporting it — this feature targets
  HTTP/1.1 parity with both existing transports specifically so
  cross-transport equivalence testing (Property 9) is meaningful; adding
  HTTP/2 multiplexing as a new capability neither existing transport has is
  a deliberately separate, unscoped idea.
- **WebSocket and QUIC/HTTP-3** (`mvfst`, a transitive dependency of the
  vendored `proxygen` port) — this feature only uses Proxygen's HTTP/1.1
  request/response layer.
- **Modifying `include/raft/tcp_rpc.hpp`/`tls_tcp_rpc.hpp` or the CoAP
  transport.**
- **Modifying `kythira::http_transport_types` or Beast's
  `future_default_http_transport_types`** — this feature's own canonical
  bundle (Requirement 15.3) is an independent duplicate, not a shared
  dependency on Beast's header, so `CONFIG_PROXYGEN_TRANSPORT` and
  `CONFIG_BOOST_BEAST_TRANSPORT` remain fully independent opt-ins.
- **Widening `transport_types` itself** to make the Folly fast path
  detectable through the concept surface rather than `Types`'s own
  concrete member type — a real alternative considered and rejected for
  the same reason Beast's own design.md rejected widening
  `transport_types` for its Requirement 19 constraint: a shared-concept
  change affecting the CoAP transport too, for a generalization neither
  feature currently needs.
- **A generic "any Folly-backed library's callback interface into
  `kythira::Future`" adaptor facility** usable beyond this transport's own
  connect/send bridging — Requirement 16 covers exactly what this feature
  needs; generalizing the pattern is a separate, unscoped idea.
- **Making the Folly fast path mandatory, or removing the generic bridge
  path** — the generic bridge remains this feature's only path under
  `stdexec`/`boost` backends, and stays available (Property 12's escape
  hatch) even under the Folly backend, for testing and for any future
  reason a caller might want to force it.

## Post-Spike Addendum

Following `.kiro/specs/boost-beast-http-transport/`'s own precedent (that
spec's Data Models section was updated post-implementation where reality
diverged from its original sketch), the Phase 0 spike (`spike-notes.md`)
surfaced two real corrections and one deliberate refinement to this
document's original sketch, applied directly in
`include/raft/proxygen_http_transport.hpp`/`_impl.hpp`:

1. **Version correction**: the Introduction/Requirement 20.1's
   `proxygen 2026.02.23.00` was a documentation error at authoring time —
   the actual version resolved at this project's pinned `builtin-baseline`
   is `2025.05.19.00` (`spike-notes.md` Finding, header).
2. **`HTTPTransaction::Handler` is a 10-method interface, not 4** — Phase
   4's code sample only showed `onHeadersComplete`/`onBody`/`onEOM`/
   `onError`; the real (aliased) `HTTPTransactionHandler` also requires
   `setTransaction`/`detachTransaction` (both pure virtual) plus four more
   with in-class no-op defaults (`onTrailers`/`onUpgrade`/
   `onEgressPaused`/`onEgressResumed`, still overridden explicitly for
   clarity). `spike-notes.md` Finding 2.
3. **Server-side design refinement**: rather than a raw
   `HTTPTransactionHandler` mirroring the client side, `proxygen_server`
   is built on Proxygen's own higher-level `RequestHandler`/
   `RequestHandlerFactory`/`ResponseBuilder` API — Proxygen's own
   documented extension point for exactly this shape of server.
   `spike-notes.md` Finding 6.
4. **EventBase assignment settled** (Requirement 21.3's open question):
   one `EventBase` pinned per target node, for that node's whole
   connection lifetime — not round-robin per call. Required for
   correctness (`HTTPUpstreamSession` is permanently pinned to its
   creating `EventBase`), not a style preference. `spike-notes.md`
   Finding 3.
5. **Session liveness tracking** (Requirement 9.3) needed an explicit
   `HTTPSessionBase::InfoCallback`-based mechanism
   (`session_liveness_tracker`), not just a lazy `isReusable()` check from
   an arbitrary thread — that check, and the reuse-or-reconnect decision
   built on it, must run on the connection's own pinned `EventBase`
   thread to avoid a real race with Proxygen's own session teardown.
   `spike-notes.md` Finding 4.
6. **Canonical `Types` bundle naming** (Requirement 15.3): the struct/
   concept are named `future_default_proxygen_transport_types`/
   `proxygen_future_default_transport_types` — distinct from Beast's
   `future_default_http_transport_types`/`future_default_transport_types`
   specifically so a translation unit including both transports' headers
   (the three-way cross-transport equivalence test) never hits a
   duplicate-concept-definition error.
