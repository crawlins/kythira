# Design Document

## Overview

Two independent changes, one per field:

- **Body limit, Proxygen only.** `rpc_request_handler` checks
  `Content-Length` in `onRequest` and a running total in `onBody`, and sends
  one 413 with `Connection: close` when either exceeds the limit.
- **Connection limit, all three servers.** Each server object owns one
  `connection_gate`, a small counter shared by all of its listeners. Every
  accepted socket asks the gate for a slot before any read or handshake. A
  socket that gets none is closed at once. The slot is released when the
  connection is destroyed.

The gate is the same type on all three transports, so "what the limit means"
is written once. Each transport differs only in where it calls `try_acquire`
and where the slot is released.

## Shared piece: `connection_gate`

New header `include/raft/http_connection_gate.hpp`, no third-party includes,
so its unit test builds on every leg regardless of which HTTP transports are
enabled.

```cpp
namespace kythira::http_detail {

class connection_gate : public std::enable_shared_from_this<connection_gate> {
public:
    explicit connection_gate(std::size_t limit);  // throws std::invalid_argument on 0

    /// RAII slot. Releasing is idempotent; moving transfers ownership.
    class slot {
    public:
        slot() = default;
        slot(slot&&) noexcept;
        auto operator=(slot&&) noexcept -> slot&;
        ~slot();
        explicit operator bool() const noexcept;
    private:
        friend class connection_gate;
        explicit slot(std::shared_ptr<connection_gate> gate);
        std::shared_ptr<connection_gate> _gate;
    };

    /// Empty slot when `limit` connections are already live.
    auto try_acquire() -> slot;
    auto live() const noexcept -> std::size_t;
    auto limit() const noexcept -> std::size_t;

private:
    std::size_t _limit;
    std::atomic<std::size_t> _live{0};
};

}  // namespace kythira::http_detail
```

`try_acquire` is a compare-exchange loop: read `_live`, return empty if it is
at `_limit`, else CAS it up by one. Lock-free, because cpp-httplib calls it
from its accept thread and Beast and Proxygen from event-loop threads.

### Key design decision: the slot holds a `shared_ptr` to the gate

A connection can outlive the server's `stop()`: Beast sessions are
`shared_ptr`s that finish their last write after the acceptor closes, and
Proxygen drops connections asynchronously. If the slot held a raw pointer to
a gate owned by the server, a late release would write to freed memory
(Requirement 2.6). Holding a `shared_ptr` keeps the gate alive until its last
slot goes, at the cost of one atomic refcount per connection. That is
negligible next to an `accept()`.

### Key design decision: refuse by closing, not by answering 503

A 503 needs the server to read a request first, and on TLS to finish a
handshake. Both cost the CPU and memory the limit exists to protect. Closing
the socket also matches what Beast Requirement 11.3 asked for ("rejecting
new connections past a live count") and what Proxygen's own wangle layer does
when `Acceptor::canAccept` returns false (set `SO_LINGER {1, 0}` and close,
so the kernel sends RST and frees the socket at once). All three transports
use that RST close.

The client sees a connection reset or EOF. Each client already treats those
as transient: they fail the RPC with the transport's existing network error,
and Raft retries on the next heartbeat or election timeout.

## cpp-httplib

`httplib::Server::new_task_queue` is a factory the server calls once per
`listen()`. In cpp-httplib 0.18.3 (the vcpkg floor), `listen_internal`
calls `task_queue->enqueue(...)` once per accepted socket, and when
`enqueue` returns `false` it shuts down and closes the socket
(`httplib.h:6814-6818`). That is the hook.

```cpp
class gated_task_queue final : public httplib::TaskQueue {
public:
    gated_task_queue(std::shared_ptr<connection_gate> gate, std::size_t threads);
    auto enqueue(std::function<void()> fn) -> bool override {
        auto s = _gate->try_acquire();
        if (!s) { note_refused(); return false; }
        return _pool.enqueue([s = std::move(s), fn = std::move(fn)]() mutable {
            fn();          // process_and_close_socket: runs the whole keep-alive session
            s = {};        // release after the socket is closed
        });
    }
    auto shutdown() -> void override { _pool.shutdown(); }
private:
    std::shared_ptr<connection_gate> _gate;
    httplib::ThreadPool _pool;
};
```

`make_listener()` sets `server->new_task_queue` on both the plain and the
SSL server (the same "site that is easy to miss" that `tcp_nodelay` already
calls out). All listeners of one `cpp_httplib_server` share one gate.

### Key design decision: size the pool to the limit

cpp-httplib serves a whole keep-alive connection on one pool thread. With
the default pool (`CPPHTTPLIB_THREAD_POOL_COUNT`, about the core count), a
connection admitted by the gate beyond that count waits in the pool's queue
with no reader, until some other connection closes. An admitted connection
must be served, so the inner `ThreadPool` gets `max_concurrent_connections`
threads. The cost is idle threads: up to 100 per listener by default, each
with a stack reservation but almost no resident memory. A pool shared by all
listeners would halve that on a `*` bind. The spike (task 1) checks whether
`new_task_queue` can return a non-owning wrapper around one shared pool
without `listen_internal`'s `shutdown()` call stopping it for the other
listener. If it cannot, each listener keeps its own pool.

The body limit already works through `set_payload_max_length`; only its
field comment changes.

## Beast

`do_accept` (`beast_http_transport_impl.hpp:1796`) gains one step before it
builds the stream:

```cpp
auto s = _gate->try_acquire();
if (!s) {
    refuse(socket);          // SO_LINGER {1,0}, close, note_refused()
} else if (_config.enable_ssl) {
    ... make_shared<server_session<...>>(std::move(stream), this, ..., std::move(s));
}
```

`server_session` stores the slot as a member, so it is released when the
last `shared_ptr` to the session goes, after `finish()`. Both the plain and
the TLS branch pass it. The gate is created in the constructor, so a zero
limit fails there (Requirement 2.5).

## Proxygen

### Body limit

`rpc_request_handler` gains a `_limit` (copied from config at construction
by `rpc_handler_factory`), a `_received` count and a `_rejected` flag.

- `onRequest`: if the headers carry a parseable `Content-Length` greater
  than `_limit`, call `reject_too_large()` and return.
- `onBody`: if `_rejected`, drop the chunk. Otherwise add
  `body->computeChainDataLength()` to `_received` *before* coalescing or
  copying. If it exceeds `_limit`, clear `_body` (and `shrink_to_fit`) and
  call `reject_too_large()`. Checking before the copy is what bounds memory:
  today's code coalesces and copies first.
- `onEOM`: if `_rejected`, return. Otherwise unchanged.
- `reject_too_large()`: set `_rejected`, log and count, then
  `ResponseBuilder(downstream_).status(413, "Payload Too Large")`
  with `Content-Type: text/plain`, `Connection: close`, body
  `"Request body exceeds maximum allowed size"` (Beast's text), and
  `sendWithEOM()`.

Responding before the request's EOM is legal in HTTP/1.1, and Proxygen's
`HTTPTransaction` allows the egress side to finish while ingress is still
open. `Connection: close` makes the session close after the transaction,
which discards whatever body the client is still sending. The spike confirms
that with the vendored Proxygen the transaction completes (so
`requestComplete()` runs and `request_finished()` is called exactly once)
and that a late `onBody` after the response is still delivered to the
handler rather than to a deleted object. If `requestComplete()` can fire
before ingress ends, the handler must not `delete this` until both sides are
done; the spike decides between relying on Proxygen's ordering and adding
`sendAbort()` after the 413 instead of `Connection: close`.

### Connection limit

`HTTPServerAcceptor` is `final`, so `canAccept()` cannot be overridden the
way wangle intends, and the wangle `Acceptor::canAccept` in current releases
always returns true (no built-in load-shed limit). Two candidates; the spike
picks one by reading the vendored headers and running the test from task 6:

**A. Session info callback (preferred).** `HTTPServer::setSessionInfoCallback`
registers an `HTTPSession::InfoCallback`. `onCreate(const HTTPSessionBase&)`
runs on the session's EventBase when a session is built, before any request
is parsed; `onDestroy` runs when it is torn down. The callback keeps a
`std::unordered_map<const HTTPSessionBase*, slot>` per EventBase (no lock
needed: each map is touched only from its own thread). `onCreate` calls
`try_acquire`; on success it stores the slot, on failure it schedules
`dropConnection()` on that session with `runInLoop` (the header forbids
starting asynchronous work inside `onCreate` itself) and counts a refusal.
`onDestroy` erases the entry, releasing the slot. The drawback: for TLS the
handshake has already happened by the time the session exists, so a flood of
TLS connections still costs handshakes. Plaintext refusal costs one
`accept()` and one session allocation.

**B. Own acceptor factory.** `HTTPServer::start()` takes a
`getAcceptorFactory`. A factory returning a subclass of
`HTTPSessionAcceptor` (not final) that overrides `canAccept()` refuses at
accept time, before the handshake, with wangle's own RST close. The cost is
reproducing what `HTTPServerAcceptor` sets up (handler chain, codec factory,
session callbacks) in project code, which then has to follow Proxygen
releases.

A is the default because it uses only public, documented hooks and is a few
dozen lines. B is the fallback if A cannot drop the session cleanly, or if
the spike measures a TLS flood as a real problem at the default limit.
Either way the header comment describes the mechanism actually chosen
(Requirement 4.2).

## Default sizing (Requirement 3.2)

Inbound connections at one node are at most `(peers) × (connections each
peer's client keeps to it)`:

- Proxygen client: up to `connection_pool_size` sessions per target
  (`sessions->capacity`, `proxygen_http_transport_impl.hpp:985`), default 10.
- Beast client: one pooled stream per in-flight request per target, capped
  by its pool (the spike records the exact bound from
  `beast_http_transport_impl.hpp`).
- cpp-httplib client: one `httplib::Client` per target with keep-alive, so
  one connection per concurrently-calling thread (`connection_pool_size` is
  unread, audit T15).

A five-node cluster on the Proxygen client therefore reaches at most
4 × 10 = 40, under the default 100. The spike counts the largest
configuration actually used in `tests/`, `examples/` and the multi-raft
benchmarks (several Raft groups sharing one transport multiply the count)
and records it in the tasks file. If anything exceeds 100, that test sets a
larger limit explicitly; the default does not change (Requirement 3.1).

## Logging and metrics

`connection_gate` has no logging of its own. Each transport's refusal path
logs through the transport's existing logger with the peer address, and the
connection-refusal log is rate-limited with a `std::atomic` timestamp on the
gate (`should_log_refusal()`, at most once per second). Beast and Proxygen
servers already copy `_metrics` to emit per-request errors; they add the two
counters from Requirement 5.2 the same way. cpp-httplib's server emits no
metrics today and gets the log line only.

## Error handling

| Situation | Behaviour |
|---|---|
| `max_concurrent_connections == 0` | `std::invalid_argument` from the server constructor |
| `max_request_body_size == 0` | non-empty bodies refused with 413 on every transport |
| `Content-Length` unparseable on Proxygen | not refused up front; the running total in `onBody` still applies |
| Refused connection | RST close; accept loop continues |
| Connection torn down after `stop()` | slot's `shared_ptr` keeps the gate alive; release is safe |

## Testing Strategy

1. **`http_connection_gate_unit_test`** (no transport dependency): acquire to
   the limit, refuse one, release, acquire again; slot move and double
   release; a gate destroyed before its slots (via the `shared_ptr`);
   concurrent acquire/release from several threads never exceeds the limit
   (checked with a high-water mark).
2. **Per-transport body-limit tests**: raw socket, `Content-Length` one over
   the limit, read the status line, expect 413 and `text/plain`. Proxygen
   adds a chunked body that crosses the limit mid-stream and a body exactly
   at the limit (expect 200 from an echo handler).
3. **Per-transport connection-limit tests**: limit 2; open two raw TCP
   connections and leave them idle; a third connection reads EOF or reset
   within a short bound without sending anything; close one of the first
   two; a Raft RPC through the normal client then succeeds. On cpp-httplib
   and Beast this needs the idle connections to stay open longer than the
   test, so the test sets `request_timeout` well above its own duration.
4. **Shared across listeners**: bind `*`, hold one connection on 127.0.0.1
   and one on ::1 with limit 2, expect the third (on either) refused. Skips
   when ::1 is unavailable, the way `localhost_and_star_binds_round_trip`
   handles it.
5. **Cross-transport parity**: extend `tests/http_implementation_interop_test.cpp`
   (or the Proxygen interop rig, whichever already starts all three servers)
   with one oversized request per server and identical assertions.
6. **Regression**: the existing HTTP transport, interop and negotiation
   suites run unchanged.

Ports: each new test takes the next free offset from its file's
`test_bind_port` base, as the existing tests do.
