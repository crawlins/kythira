# Design Document

## Overview

This design adds a second implementation of `kythira::network_client`/
`kythira::network_server`, backed by Boost.Beast, alongside the existing
cpp-httplib-backed implementation (`include/raft/http_transport.hpp`,
`include/raft/http_transport_impl.hpp`). It does not touch any existing
production call site, does not change which transport `cmd/ca_service`/
`cmd/ca_cluster_node`/any existing example uses, and does not modify
`network_client`, `network_server`, or `transport_types`
(`include/raft/network.hpp`, `include/raft/types.hpp`).

Where `.kiro/specs/stdexec-future-backend/`'s central design problem was
bridging a pull-model future library onto Raft's push-model transport code,
and `.kiro/specs/boost-future-backend/`'s was a project-wide ABI macro, this
feature's central design problem is different in kind: **this project has no
existing precedent for an `io_context`-driven asynchronous I/O model.**
Every other transport already in this codebase —
`cpp_httplib_client`/`cpp_httplib_server` (blocking calls, one per RPC or
one accept loop per server thread) and `include/raft/tcp_rpc.hpp`/
`tls_tcp_rpc.hpp` (blocking socket I/O dispatched onto `executor_type` via
`executor_default::submit`) — uses blocking calls, not composed
asynchronous operations driven by an event loop. Boost.Beast supports both
styles (Glossary: Synchronous vs. Asynchronous Beast API), and using it
synchronously would be a strictly smaller, lower-risk design — but it would
also forfeit the actual reason to add Beast as a second transport at all
(Requirement 14, `requirements.md`'s Introduction): genuine concurrency from
a small, shared `io_context` thread pool instead of one blocking call (or
one thread) per in-flight connection.

## Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│  Concept Layer                                                          │
│  network_client / network_server (include/raft/network.hpp)            │
│  transport_types (include/raft/types.hpp, shared w/ CoAP transport)    │
│  — unmodified by this feature                                          │
└───────┬──────────────────────────────────────┬─────────────────────────┘
        │ satisfies                              │ satisfies
┌───────▼────────────────────┐        ┌──────────▼──────────────────────┐
│ cpp-httplib transport       │        │ Boost.Beast transport (new)      │
│ (existing, unchanged)       │        │                                   │
│ include/raft/               │        │ include/raft/                    │
│  http_transport.hpp          │        │  beast_http_transport.hpp        │
│  http_transport_impl.hpp     │        │  beast_http_transport_impl.hpp   │
│                               │        │                                   │
│ cpp_httplib_client/server    │        │ boost_beast_client/server        │
│ blocking calls; server owns  │        │ async ops on caller-owned         │
│ its own accept-loop thread   │        │ net::io_context (Requirement 8)   │
└───────┬──────────────────────┘        └──────────┬───────────────────────┘
        │                                             │
   httplib::Client/Server                    beast::tcp_stream /
   (OpenSSL via                              beast::ssl_stream<tcp_stream>
    httplib::SSLServer)                      (OpenSSL via
                                               boost::asio::ssl::context),
                                              beast::http::request/response,
                                              beast::flat_buffer
```

Both transports are reached only through `Types` template arguments already
satisfying `transport_types` — nothing about which transport a piece of
generic Raft code uses changes based on this feature landing (Requirement
13).

### Why the asynchronous Beast API, not the synchronous one on `executor_type`

Considered and rejected: mirror `cpp_httplib_client`/`tcp_rpc.hpp` exactly —
call `beast::http::read`/`write` (the blocking free functions) inside a
lambda submitted to `Types::executor_type` via `executor_default::submit`,
fulfilling a `Promise<T>` when the blocking call returns. This would work,
would be substantially simpler to implement and test (no `io_context`
lifetime/threading design at all — Requirement 8 would not exist), and
would still satisfy every acceptance criterion in `requirements.md` *except*
Requirement 3.4's actual payoff (concurrency from the event loop itself,
not from the caller submitting more parallel blocking calls) and Requirement
14 itself, which exists specifically to rule this simpler design out.

Rejected because: if the synchronous API is the shape adopted, this feature
has no design or operational advantage over the existing cpp-httplib
transport at all — same blocking-call-per-request model, same "N in-flight
requests need N threads (or N executor slots) doing nothing but waiting on
a socket" resource profile, just with Beast's headers instead of
cpp-httplib's. The entire justification for taking on a new dependency
(`requirements.md`'s Introduction) depends on actually using Beast's
asynchronous API. This mirrors `boost-future-backend/design.md`'s own
"Why a third namespace, not a third `#ifdef` branch" section: stating the
simpler alternative explicitly and why it was rejected, rather than only
implying it.

The cost of this decision is real and is not hidden: `io_context` ownership
and threading (Requirement 8) is a genuinely new concern for callers of
this transport, unlike every other transport in this codebase, and is
called out explicitly in this feature's own documentation (Requirement
15.6's example) rather than left implicit.

## Components and Interfaces

### Phase 0: Spike (Requirement 18)

Before committing to exact adaptor code, confirm against the actually
vendored `boost-beast`/`boost-asio` version (not just documentation read
while writing this document):
- `beast::tcp_stream::expires_after` actually cancels an in-flight
  `async_read`/`async_write` (a throwaway compile-and-run against a real
  socket, per Requirement 18.2 — Beast's own docs describe cancellation of
  already-issued syscalls as best-effort/OS-dependent).
- Whether this project's existing OpenSSL context-configuration code
  (`cpp_httplib_client::configure_ssl_client`/`load_client_certificates`,
  `cpp_httplib_server::configure_ssl_server`/`load_server_certificates` in
  `http_transport_impl.hpp`, and `tls_tcp_rpc.hpp`'s own direct `SSL_CTX`
  calls) is reusable as-is against `boost::asio::ssl::context`, or needs an
  adaptation layer.
- Which coroutine/completion-handler composition style
  (`boost::asio::awaitable<T>` + `co_spawn`, stackless
  `boost::asio::coroutine`, or plain nested callbacks) compiles cleanly
  under this project's C++23 + Clang 16+/GCC 13+ matrix.
- Record the exact Boost release and minimum compiler versions validated,
  in `spike-notes.md`.

The rest of this section assumes the completion-handler/coroutine style the
spike settles on; code samples below use plain completion-handler
composition as the illustrative default, since it has the widest compiler
support of the three options and the spike may simply confirm it is the
right (not merely the safe) choice.

### Phase 1: Dependency and Build Wiring (Requirement 17)

```json
// vcpkg.json — new dependency, header-only, alongside the existing
// boost-asio entry it's built on
{
  "name": "boost-beast",
  "version>=": "1.89.0"
}
```

```cmake
# CMakeLists.txt, alongside the existing kythira_find_optional(EDHOC ...) /
# find_package(stdexec ...) block — same optional-dependency machinery
# every other optional dependency in this project already uses
# (cmake/Kconfig.cmake), not a bespoke if(...) check.
kythira_find_optional(BOOST_BEAST_TRANSPORT boost-beast CONFIG)
```

```kconfig
# Kconfig, new leaf config alongside the existing "Futures" menu's own
# precedent for an optional, non-default capability
config BOOST_BEAST_TRANSPORT
	bool "Boost.Beast HTTP transport (second implementation, alongside cpp-httplib)"
	default n
	help
	  Adds a second network_client/network_server implementation backed by
	  Boost.Beast, satisfying the same concepts as the existing cpp-httplib
	  transport (include/raft/http_transport.hpp) without replacing it.
	  Requires the boost-beast vcpkg package. See
	  .kiro/specs/boost-beast-http-transport/ for the full design,
	  including why this uses Beast's asynchronous API (a genuinely new
	  io_context-driven I/O model for this codebase) rather than a
	  simpler synchronous-on-executor design.
```

Targets and tests gated on `TARGET boost-beast::boost-beast` (or the
project's usual `_FOUND` variable, whichever `kythira_find_optional`
produces) per-target from the start — Requirement 17.4 explicitly calls out
*not* repeating the subdirectory-level-gate mistake `doc/TODO.md`'s
Folly-decoupling entries document being found and fixed as a follow-up
there. This feature's own targets (`tests/beast_*`) live inside the
already-existing `tests/CMakeLists.txt`, individually gated, from their
first commit.

### Phase 2: `io_context` Ownership (Requirement 8)

```cpp
namespace kythira {

// Both boost_beast_client and boost_beast_server take a net::io_context&,
// never own one. Callers are expected to run one or more threads calling
// io_context::run() — this transport makes no progress otherwise, and that
// is a deliberate, documented consequence of sharing one io_context (and
// its thread pool) across multiple Beast-based components in the same
// process, not an oversight (Requirement 8.2).
template<typename Types>
requires kythira::transport_types<Types>
class boost_beast_client {
public:
    boost_beast_client(net::io_context& ioc,
                        std::unordered_map<std::uint64_t, std::string> node_id_to_url_map,
                        boost_beast_client_config config, typename Types::metrics_type metrics);
    // ...
private:
    net::io_context& _ioc;  // non-owning
    // ...
};

}  // namespace kythira
```

A short usage example (Requirement 15.6, modeled on
`examples/raft/http_transport_example.cpp`) makes the shared-`io_context`
pattern concrete:

```cpp
net::io_context ioc;
std::vector<std::thread> io_threads;
for (int i = 0; i < 4; ++i) {
    io_threads.emplace_back([&ioc] { ioc.run(); });
}

kythira::boost_beast_client<my_transport_types> client{ioc, node_map, {}, metrics};
kythira::boost_beast_server<my_transport_types> server{ioc, "0.0.0.0", 8080, {}, metrics};
// ... use client/server; both are driven by the same 4-thread pool ...

server.stop();      // stops accepting, drains in-flight requests -- does NOT stop ioc
ioc.stop();          // caller's own responsibility, only once nothing else needs it
for (auto& t : io_threads) t.join();
```

### Phase 2.5: Future-Concept-Based Async Composition (Requirements 14, 19)

The naive design — bridge the *whole* connect+write+read sequence for one
RPC into a single monolithic completion handler that fulfills one
`Promise<Response>` — works, but reimplements, per RPC method, exactly the
kind of step-sequencing `future_transformable`'s `thenValue`/`thenError`
(`include/concepts/future.hpp`) already exists to express declaratively,
and every one of the three backends already implements. Using it here is a
better fit than it might first appear: the *reason* this codebase's
concept-neutral futures haven't been used for I/O composition before now is
that no other transport in this codebase does asynchronous, event-loop-driven
I/O at all — `cpp_httplib_client`/`tcp_rpc.hpp` fulfill one `Promise` per
*whole blocking call*, so there was never a multi-step async sequence to
compose. Beast's connect→write→read sequence is the first one.

**Three small primitive adaptors**, each wrapping exactly one Beast async
operation:

```cpp
namespace kythira::beast_detail {

// Each adaptor: construct a kythira::promise_default<T>, issue the Beast
// async_* call with a completion handler that setValue()s/setException()s
// it, return promise.getFuture() immediately. The io_context thread that
// eventually runs the completion handler is what actually fulfills the
// promise -- these functions themselves never block.

inline auto async_connect_kf(beast::tcp_stream& stream, net::ip::tcp::endpoint ep)
    -> kythira::future_default<kythira::unit> {
    kythira::promise_default<kythira::unit> promise;
    auto future = promise.getFuture();
    stream.async_connect(ep, [promise = std::move(promise)](boost::system::error_code ec) mutable {
        if (ec) {
            promise.setException(std::make_exception_ptr(boost::system::system_error(ec)));
        } else {
            promise.setValue(kythira::unit{});
        }
    });
    return future;
}

// async_write_kf(stream, request) / async_read_kf(stream, buffer, response):
// identical shape -- construct promise, issue the Beast async_* call,
// setValue(unit{})/setException(...) from the completion handler.

}  // namespace kythira::beast_detail
```

**The RPC send path becomes a `thenValue` chain**, not a hand-written state
machine:

```cpp
template<typename Request, typename Response>
auto boost_beast_client<Types>::send_rpc(std::uint64_t target, std::string_view endpoint,
                                          const Request& request, std::chrono::milliseconds timeout)
    -> typename Types::template future_template<Response> {
    auto& conn = get_or_create_connection(target);          // Requirement 9
    conn.stream.expires_after(timeout);                      // Requirement 10.1 --
                                                               // bounds the whole chain below,
                                                               // not each step individually
    auto body = _serializer.serialize(request);

    return kythira::beast_detail::async_connect_kf(conn.stream, conn.endpoint)
        .thenValue([&conn, body = std::move(body)](kythira::unit) {
            return kythira::beast_detail::async_write_kf(conn.stream, body);
        })
        .thenValue([&conn](kythira::unit) {
            return kythira::beast_detail::async_read_kf(conn.stream, conn.buffer, conn.response);
        })
        .thenValue([this](kythira::unit) {
            return _serializer.template deserialize<Response>(conn.response.body());  // Req 3.2
        })
        .thenError([](std::exception_ptr e) -> Response {
            std::rethrow_exception(e);  // Req 3.3, 10.2 -- caller's future
                                          // completes with this exception
        });
}
```

Each step is independently unit-testable against a real (or loopback) TCP
endpoint without instantiating a full `boost_beast_client`, and adding a
fourth step (e.g. a retry-on-connect-failure policy, if a future requirement
needs one) is one more `.thenValue`/`.thenError`, not a rewrite of a bespoke
callback tree.

**Requirement 19's constraint follows directly from this design**: since
`async_connect_kf`/`async_write_kf`/`async_read_kf` return
`kythira::future_default<kythira::unit>` (via `kythira::promise_default`
internally), the chain's final type is only assignable to
`Types::template future_template<Response>` when that member type *is*
`kythira::future_default<Response>`. A canonical bundle makes this concrete
rather than leaving every caller to define it locally:

```cpp
// include/raft/beast_http_transport.hpp
template<typename RPC_Serializer, typename Metrics, typename Executor>
struct future_default_http_transport_types {
    template<typename T> using future_template = kythira::future_default<T>;
    using serializer_type = RPC_Serializer;
    using metrics_type = Metrics;
    using executor_type = Executor;
};
```

— the exact pattern already used ad hoc, per test file, in 13 existing CoAP
property tests (`tests/coap_concurrent_processing_property_test.cpp` and
others), just formalized into one reusable, named bundle. This is a
narrower `Types` constraint than `cpp_httplib_client`/`server` impose
(Requirement 19.5) — a deliberate trade of `Types`-bundle flexibility for
the ability to use `future_transformable` composition internally, not an
oversight.

### Phase 3: Client (Requirements 1, 3, 9, 10)

```cpp
template<typename Types>
requires kythira::transport_types<Types>
class boost_beast_client {
public:
    // ...as above...

    auto send_request_vote(std::uint64_t target, const kythira::request_vote_request<>& request,
                           std::chrono::milliseconds timeout) ->
        typename Types::template future_template<kythira::request_vote_response<>>;
    // send_append_entries / send_install_snapshot: identical shape

    auto reload_tls_material() -> void;              // Requirement 7.1
    auto enable_auto_reload(std::chrono::seconds poll_interval) -> void;  // Requirement 7.4
    auto disable_auto_reload() -> void;               // Requirement 7.5

private:
    net::io_context& _ioc;
    boost_beast_client_config _config;
    typename Types::metrics_type _metrics;
    kythira::json_rpc_serializer_or_whatever_Types_provides _serializer;  // Types::serializer_type

    // Connection pool (Requirement 9): one persistent beast::tcp_stream (or
    // beast::ssl_stream<beast::tcp_stream> when the target URL is https://)
    // per target node, reused across RPCs the way cpp_httplib_client's own
    // _http_clients map reuses httplib::Client instances. Guarded by the
    // same io_context's own single-threaded-per-stream-object rule: all
    // operations on one node's stream are issued from strand-wrapped
    // handlers (net::strand<net::io_context::executor_type>) so concurrent
    // RPCs to the *same* node serialize safely without an explicit mutex,
    // while RPCs to *different* nodes run genuinely concurrently.
    std::unordered_map<std::uint64_t, /* pooled connection state */> _connections;
    mutable std::mutex _connections_mutex;  // guards the map itself, not
        // each connection's I/O (that's the strand's job) -- narrow lock
        // scope, matching cpp_httplib_client's own _mutex usage pattern.

    // Per-RPC flow (send_request_vote and friends all funnel through this):
    //   1. Look up or create a strand-wrapped connection for `target`.
    //   2. Serialize `request` via Types::serializer_type.
    //   3. Construct a Promise<Response> (Types::future_template's backend).
    //   4. Post a lambda onto the connection's strand that:
    //      a. Sets stream.expires_after(timeout)                     (Req 10.1)
    //      b. async_connect (if not already connected) -> async_write
    //         -> async_read, each completion re-entering the same lambda
    //         (or a coroutine awaiting each step, per the Phase 0 spike's
    //         chosen composition style)
    //      c. On success: deserializes the response, calls
    //         promise.setValue(...)                                   (Req 3.2)
    //      d. On any error (connect/write/read/timeout): calls
    //         promise.setException(...)                               (Req 3.3, 10.2)
    //   5. Returns promise.getFuture() immediately -- the actual I/O
    //      happens later, on whichever thread is running io_context::run().
    template<typename Request, typename Response>
    auto send_rpc(std::uint64_t target, std::string_view endpoint, const Request& request,
                  std::chrono::milliseconds timeout) ->
        typename Types::template future_template<Response>;
};
```

The `Promise<T>`/`setValue`/`setException` fulfillment pattern in step 4c/4d
is the same one `tcp_rpc.hpp` already uses for its own executor-submitted
completions (Requirement 14.2) — the only difference is *what* calls
`setValue`/`setException`: an `executor_type`-submitted lambda there, an
`io_context`-driven completion handler here.

### Phase 4: Server (Requirements 2, 4, 5)

```cpp
template<typename Types>
requires kythira::transport_types<Types>
class boost_beast_server {
public:
    boost_beast_server(net::io_context& ioc, std::string bind_address, std::uint16_t bind_port,
                       boost_beast_server_config config, typename Types::metrics_type metrics);

    auto register_request_vote_handler(/* ... */) -> void;
    // register_append_entries_handler / register_install_snapshot_handler: identical shape

    auto start() -> void;   // Requirement 5.1
    auto stop() -> void;    // Requirement 5.2 -- posts a stop to the
                             // acceptor's strand, closes it, and waits
                             // (via a condition_variable, not by touching
                             // _ioc itself -- Requirement 8.3) for the
                             // in-flight-connection count to reach zero
    auto is_running() const -> bool;

    auto reload_tls_material() -> void;  // Requirement 7.2

private:
    net::io_context& _ioc;
    net::ip::tcp::acceptor _acceptor;   // constructed, not started, until start()
    std::optional<boost::asio::ssl::context> _ssl_ctx;  // present iff enable_ssl
    std::atomic<std::size_t> _live_connections{0};
    std::atomic<bool> _running{false};

    // Accept loop: async_accept -> spawn a per-connection session object
    // (owns its own beast::flat_buffer + stream, self-managing lifetime via
    // shared_from_this, the standard Beast server pattern) -> immediately
    // re-issue async_accept for the next connection. No dedicated
    // "server thread" the way cpp_httplib_server has one -- accepting and
    // serving both happen on whichever io_context threads are running.
    auto do_accept() -> void;

    // Per-session: async_read a beast::http::request<string_body> into a
    // flat_buffer -> match the parsed target path against the three
    // registered RPC endpoints (Requirement 4.4's explicit 404 for
    // anything else, since Beast has no built-in router) -> deserialize,
    // invoke handler, serialize response, async_write -> on completion,
    // if keep-alive, re-issue async_read on the same stream (Requirement
    // 9.1's server-side counterpart); otherwise close.
};
```

### Phase 5: TLS (Requirements 6, 7)

`boost_beast_client_config`/`boost_beast_server_config` reuse the exact
field set from `cpp_httplib_client_config`/`cpp_httplib_server_config`
(Requirement 11) — `ca_cert_path`, `client_cert_path`/`client_key_path` (or
`ssl_cert_path`/`ssl_key_path` server-side), `cipher_suites`,
`min_tls_version`/`max_tls_version`, `require_client_cert`. The
Phase-0-spike-dependent question is whether the existing certificate
loading/validation code (`load_client_certificates`/
`validate_certificate_files` in `http_transport_impl.hpp`) can be shared
as-is between the cpp-httplib and Beast transports (both ultimately
configure an OpenSSL `SSL_CTX`, just reached via different wrapper types —
`httplib::SSLServer`'s constructor vs. `boost::asio::ssl::context`) or needs
a thin adaptation layer; either way, no new certificate-loading logic is
invented, only re-pointed at a different context type.

`reload_tls_material()` (Requirement 7) follows the existing transport's
all-or-nothing contract: validate everything first, construct a *new*
`boost::asio::ssl::context` from the validated material, and swap it in for
subsequently-established connections only — exactly like
`cpp_httplib_client::reload_tls_material()`'s existing "retired clients
kept alive, not destroyed" comment already documents, since an in-flight
`beast::ssl_stream<beast::tcp_stream>` holds a reference into whichever
context was live when it was constructed.

## Data Models

### Exception Representation

Same normalization point as every backend already uses: `std::exception_ptr`
at the concept boundary. `boost::system::error_code` (the result of every
Beast/Asio async operation's first completion-handler parameter) and
`beast::error` (Beast-specific codes, e.g. `beast::http::error::end_of_stream`)
are both wrapped in `boost::system::system_error`, itself
`std::exception`-derived, so `std::make_exception_ptr` needs no
Beast-specific unwrapping step — the same treatment
`boost-future-backend/design.md`'s own Data Models section gives
`boost::promise_already_satisfied`/`boost::broken_promise`.

### Unit Representation

Reuses `kythira::unit` exactly as defined by the concept layer. Every
`async_connect_kf`/`async_write_kf` step (Phase 2.5) resolves to
`kythira::future_default<kythira::unit>` on success — there is no
meaningful "value" from connecting or writing, only "did it complete" —
matching how `FutureFactory::makeReadyFuture()` already uses `unit` for the
same reason elsewhere in this codebase.

### Connection Pool Entry

```cpp
struct pooled_connection {
    beast::tcp_stream stream;                              // or ssl_stream<tcp_stream>
    net::ip::tcp::endpoint endpoint;
    net::strand<net::io_context::executor_type> strand;     // serializes all
        // operations on this one connection; RPCs to *other* nodes use a
        // different connection's strand and run genuinely concurrently
    beast::flat_buffer buffer;                               // reused across
        // requests on this connection -- Beast grows it as needed, this
        // design does not reset its capacity between requests, only its
        // consumed/committed state
    std::chrono::steady_clock::time_point last_used;          // idle-timeout
        // eviction (Requirement 9.2) reads this, does not need its own timer
};
```

### Config Structs

`boost_beast_client_config`/`boost_beast_server_config`: field-for-field
copies of `cpp_httplib_client_config`/`cpp_httplib_server_config`
(Requirement 11) — no new data model, deliberately, so a call site's
existing config literal is reusable verbatim across a type-only transport
swap.

## Correctness Properties

*A property is a characteristic or behavior that should hold true across
all valid executions of a system.*

**Property 1: No Progress Without a Running `io_context`**
*For any* `boost_beast_client`/`boost_beast_server` operation in flight,
if no thread is currently executing `io_context::run()` on the associated
`io_context`, that operation should make no further progress, and should
resume exactly where it left off once a thread resumes running it
**Validates: Requirement 8.2**

**Property 2: Per-Step Promise Exactly-Once Fulfillment**
*For any* `async_connect_kf`/`async_write_kf`/`async_read_kf` call, the
returned future should complete exactly once — with a value on success, an
exception on failure — regardless of which thread's `io_context::run()`
call happens to execute the completion handler
**Validates: Requirements 14.2, 14.4**

**Property 3: Chain Short-Circuits on First Failure**
*For any* `send_rpc` call whose connect, write, or read step fails, the
overall returned future should complete with that step's exception, and no
subsequent step in the `thenValue` chain (Phase 2.5) should execute
**Validates: Requirements 3.3, 14.3**

**Property 4: Timeout Bounds the Whole RPC, Not Each Step**
*For any* `send_rpc` call with a given `timeout`, the sum of time spent
across connect+write+read should not exceed `timeout` even if no
individual step is itself slow — `expires_after` is set once per RPC on
the connection's stream, not reset before each step
**Validates: Requirement 10.1**

**Property 5: Connection Reuse Correctness**
*For any* two RPCs to the same target node issued while a healthy
connection to that node already exists, the second RPC should reuse the
existing connection rather than establishing a new one; *for any* RPC
issued against a pooled connection that has since become unusable (peer
closed, network error), the system should transparently establish a fresh
one rather than surfacing the staleness to the caller
**Validates: Requirements 9.1, 9.3**

**Property 6: Per-Node Serialization, Cross-Node Concurrency**
*For any* two RPCs to the *same* target node issued concurrently, their
I/O on that node's shared connection should never interleave (strand
serialization); *for any* two RPCs to *different* target nodes issued
concurrently, both should be able to make progress without waiting on each
other
**Validates: Requirement 3.4**

**Property 7: TLS Material Reload Atomicity**
*For any* `reload_tls_material()` call, either every piece of new
certificate/key material is valid and all of it takes effect for
subsequently-established connections, or none of it does and the
previously-active material remains in effect — never a partially-applied
state
**Validates: Requirement 7.3**

**Property 8: Server Drain on Stop**
*For any* `stop()` call on `boost_beast_server` while requests are
in-flight, every in-flight request should be allowed to complete before
`stop()` returns, and no new connection should be accepted after `stop()`
is called
**Validates: Requirement 5.2**

**Property 9: Cross-Transport Equivalence**
*For any* `Types` bundle and RPC sequence run through both
`cpp_httplib_client`/`server` and `boost_beast_client`/`server`, the
externally-observable results (response content, error status-code
category) should be equivalent, even though internal I/O mechanics differ
**Validates: Requirement 16.4**

**Property 10: Concept Compliance**
*For any* concept `boost_beast_client`/`boost_beast_server` are intended to
satisfy (`network_client`, `network_server`), they should do so, verified
by `static_assert`
**Validates: Requirement 16.1**

## Error Handling

### Exception Safety Guarantees

Same three-tier structure (basic/strong/no-throw) this codebase's other
transport/future designs already use. The connection pool (Data Models,
above) adds one specific guarantee: a failed `send_rpc` call never leaves
its target node's pooled connection in a half-written state that a
*subsequent* RPC could observe — on any failure, the connection is either
fully torn down (removed from `_connections`, a fresh one built next time)
or left exactly as it was before the failed call started, never partially
mutated.

### Cancellation

Beast's cancellation channel is `beast::tcp_stream::expires_after` plus
`boost::asio::cancellation_signal` (available via a completion token
argument, if the Phase 0 spike's chosen composition style supports it
directly) — distinct from `stdexec`'s stop-token-based cancellation and
Folly's lack of one. A timed-out RPC surfaces as an ordinary
`std::exception_ptr`-carried timeout error at the `send_rpc` boundary
(Property 4, Requirement 10.2), consistent with how `boost-future-backend`'s
`within(timeout)` and the existing cpp-httplib transport's own timeout
handling both already present timeouts to callers — no new exception
category invented specifically for this transport.

### Connection Pool Staleness

Covered by Property 5 above: detected lazily, on the next attempted use
(no background health-checking connections that aren't currently needed),
matching `cpp_httplib_client`'s own reactive-not-proactive approach to
broken pooled connections.

### Server Accept-Loop Resilience

A single connection's I/O error (mid-request disconnect, malformed
request, TLS handshake failure) must not stop the server's accept loop
from continuing to serve other connections — each per-connection session
object's error handling is scoped to that session only, and `do_accept()`
re-issues itself unconditionally after each accept completion (success or
failure) rather than only after a successful one.

## Testing Strategy

Mirrors `.kiro/specs/boost-future-backend/` Requirement 10's approach:

1. **Concept compliance**: `static_assert` `boost_beast_client`/
   `boost_beast_server` against `network_client`/`network_server` at the
   bottom of `beast_http_transport.hpp`, matching this codebase's existing
   assertion convention for concept-constrained types.
2. **Parallel test suite**: `tests/beast_client_test.cpp`,
   `tests/beast_server_test.cpp`, `tests/beast_integration_test.cpp`,
   `tests/beast_ssl_*` — one-to-one with the existing `tests/http_client_*`/
   `tests/http_server_*`/`tests/http_integration_test.cpp`/`tests/http_ssl_*`
   files, covering the same scenarios (Requirement 16.2).
3. **Cross-transport equivalence**: a property test instantiating both
   `cpp_httplib_client`/`server` and `boost_beast_client`/`server` against
   the same `Types` bundle and the same sequence of RPCs, asserting
   equivalent externally-observable results (Requirement 16.4) — modeled on
   `.kiro/specs/boost-future-backend/` Requirement 10.4's cross-backend
   equivalence testing for futures, applied here to transports instead.
4. **Concurrency-specific coverage**: the one genuinely new test category
   this feature needs that the cpp-httplib transport's own suite has no
   equivalent for — many concurrent in-flight RPCs against a small
   `io_context` thread count (Requirement 3.4), and `io_context::run()`
   called from multiple threads simultaneously with no data race on shared
   client/server state (Requirement 14.4), most practically checked with
   ThreadSanitizer (this project's existing `build-asan`/clang-tidy
   tooling) rather than relying on property-test flakiness alone to surface
   a race.
5. All tests run exclusively through CTest, labeled `beast-http`
   (Requirement 16.5).

## Non-Goals

- **Converting any existing production call site** (`cmd/ca_service`,
  `cmd/ca_cluster_node`, any existing example) from cpp-httplib to Beast.
- **Removing, deprecating, or making optional-only the cpp-httplib
  transport.**
- **Beast's WebSocket support** (`boost::beast::websocket`) — this feature
  only uses Beast's HTTP/1.1 message layer.
- **HTTP/2** — neither Beast nor the existing cpp-httplib transport
  implements it; parity, not a regression.
- **Modifying `include/raft/tcp_rpc.hpp`/`tls_tcp_rpc.hpp`** (the actual
  production RPC transport) **or the CoAP transport**, even though both
  share infrastructure (`transport_types`, `executor_default`) with this
  feature.
- **Modifying `kythira::http_transport_types`** or any other existing
  `Types` bundle to satisfy Requirement 19's `future_default` constraint —
  the new `future_default_http_transport_types` bundle is additive, not a
  change to what already exists.
- **Widening `transport_types` itself** (e.g. adding a `promise_template`
  member) to make this feature's `Types` constraint less narrow — a real
  alternative considered (design.md's Phase 2.5) and deliberately not
  taken, since it would be a shared-concept change affecting the CoAP
  transport too, for a generalization this feature does not currently need.
- **A generic, reusable "Asio operation → kythira future" adaptor library**
  usable beyond this transport's own three primitive operations
  (`async_connect_kf`/`async_write_kf`/`async_read_kf`) — those three cover
  exactly what this feature needs; generalizing the pattern into a
  standalone facility is a separate, unscoped idea this design does not
  pursue.
