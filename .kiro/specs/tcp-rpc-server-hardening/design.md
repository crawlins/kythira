# Design Document

## Overview

The three connection servers (`tcp_rpc_server`, `tls_tcp_rpc_server`, the
gossip listener) get one shared piece of machinery,
`tcp_detail::connection_tracker`, in a new header
`include/raft/tcp_connection_tracker.hpp`. It owns everything the servers
currently get wrong in the same way: how many connections are live, from
where, until when each may block, and when the last one has finished. Each
server's accept loop and per-connection thread become thin callers of it.

Separately, `tcp_detail::write_all()` sends with `MSG_NOSIGNAL`, and both
frame readers grow their buffers as bytes arrive.

```
accept thread (one per listener)          reaper thread (one per server)
──────────────────────────────            ──────────────────────────────
accept() ──error──> transient? back off   sleep until earliest deadline
   │                 else exit loop        or until woken
   ▼                                       shutdown(fd, SHUT_RDWR) on every
tracker.admit(fd, source)                  connection past its deadline
   │ refused ──> close(fd), count
   ▼ ticket (request phase, deadline = accept time + request_timeout)
std::thread(serve, ticket)  ──throws──> ticket releases, close(fd)
   │
   ▼  connection thread
read request (TLS handshake + trust check for TLS)
ticket.phase(handler, no deadline)
call handler
ticket.phase(reply, now + reply_timeout)
write reply
close(fd); ticket destroyed ──> tracker count--, notify
```

### Key design decision: enforce deadlines by shutting the socket down

The obvious alternative is to make every blocking call deadline-aware:
`poll()` before each `read()`, `SO_RCVTIMEO` reset to the remaining time
before each `SSL_read()`, and a non-blocking `SSL_accept()` loop for the
handshake. That touches three read paths, two of them through OpenSSL, and
each new blocking call added later would have to remember to do the same.

Instead a single reaper thread per server calls `shutdown(fd, SHUT_RDWR)` on
any connection whose current phase has outlived its deadline. On Linux this
wakes a thread blocked in `read()`, `write()`, `send()` or `recv()` on that
socket, and through them `SSL_accept()`, `SSL_read()` and `SSL_write()`;
each returns an error, the connection thread unwinds through its normal
failure path, and closes the socket. The read paths do not change at all,
and the TLS handshake is bounded for free. The same `shutdown()` is how
`stop()` unblocks connections (Requirement 5.2).

Because the reaper bounds every blocking call in the request and reply
phases, `tls_tcp_rpc_server` drops its fixed 30 s `SO_RCVTIMEO` and
`SO_SNDTIMEO` (Requirement 2.6). Its default `request_timeout` is the same
30 s, so the effective limit for a well-behaved slow handshake is unchanged.

`shutdown()` never closes the descriptor. The connection thread closes its
own socket, as the last step of releasing its ticket, so the reaper and
`stop()` cannot act on a descriptor number that has been reused
(Requirement 5.6); rule 2 below gives the ordering.

### Key design decision: cap connections instead of a worker pool

A fixed worker pool would bound threads, but a pool of N workers blocked on
N silent peers is the same outage with a smaller number. What actually
bounds the damage is the deadline (a silent peer holds a slot for at most
`request_timeout`) plus a cap on how many slots one source can hold. With
both in place, a thread per admitted connection is bounded by
`max_connections` and costs nothing extra, so this design keeps the
existing thread-per-connection model and adds the caps.

Over the cap, the server accepts and immediately closes rather than leaving
the connection in the kernel backlog. The client then fails fast with a
reset, which Raft already treats as an unreachable peer, instead of waiting
out a connect timeout. The per-source default of 32 is well above what one
Raft peer needs: PR #389 caps `tcp_rpc_client` at 2 in-flight RPCs per
endpoint, and a gossip peer makes one exchange per round.

### Key design decision: `MSG_NOSIGNAL`, not `SIG_IGN`

A library header that changes the process-wide SIGPIPE disposition affects
every other library and pipe in the embedding process. `MSG_NOSIGNAL` fixes
exactly the writes this code makes. `tls_tcp_rpc` keeps
`ignore_sigpipe_once()` because OpenSSL's socket BIO calls `write()` itself;
replacing that BIO is out of scope (Requirement 1.4).

## Components and Interfaces

### `tcp_server_limits` and stats (`include/raft/tcp_connection_tracker.hpp`)

```cpp
namespace kythira {

/// Connection limits shared by tcp_rpc_server, tls_tcp_rpc_server and the
/// tcp_gossip listener. See .kiro/specs/tcp-rpc-server-hardening/.
struct tcp_server_limits {
    /// accept() until the request frame is fully read; for TLS this
    /// includes the handshake and the trust-policy check.
    std::chrono::milliseconds request_timeout{std::chrono::seconds{30}};
    /// Handler return until the reply frame is fully written.
    std::chrono::milliseconds reply_timeout{std::chrono::seconds{30}};
    std::size_t max_connections{256};
    std::size_t max_connections_per_source{32};
};

struct tcp_server_connection_stats {
    std::size_t active_connections{0};
    std::uint64_t refused_global_limit{0};
    std::uint64_t refused_per_source_limit{0};
    std::uint64_t timed_out{0};
    std::uint64_t accept_errors{0};
};

}  // namespace kythira
```

`validate(const tcp_server_limits&, const char* who)` throws
`std::invalid_argument` naming the field for a zero value (Requirement 6.5).

### `tcp_detail::connection_tracker`

```cpp
class connection_tracker : public std::enable_shared_from_this<connection_tracker> {
public:
    explicit connection_tracker(tcp_server_limits limits);

    enum class phase { request, handler, reply };

    class ticket {               // move-only; RAII
    public:
        void enter(phase p);     // sets the deadline for p from limits, or none
        [[nodiscard]] auto fd() const -> int;
        ~ticket();               // erase entry, close(fd), notify drain
    };

    /// Admits `fd` from `source`, or returns nullopt (caller closes fd).
    auto admit(int fd, const sockaddr_storage& source) -> std::optional<ticket>;

    void start_reaper();
    /// Shuts down every tracked socket, stops the reaper, and waits until
    /// every ticket has been destroyed.
    void shutdown_and_drain();

    void note_accept_error();
    [[nodiscard]] auto stats() const -> tcp_server_connection_stats;
};
```

State, all under one mutex:

- `std::unordered_map<std::uint64_t, entry> _conns`, where `entry` holds the
  fd, the source key, the current phase and its deadline (`std::optional<
  steady_clock::time_point>`, empty in the handler phase).
- `std::unordered_map<std::string, std::size_t> _per_source`, keyed by the
  numeric source address with IPv4-mapped IPv6 folded to IPv4.
- `bool _draining`; counters; one condition variable for the reaper and one
  for the drain.

Rules that keep it correct:

1. **Servers hold the tracker by `shared_ptr`, and each connection thread
   captures its own `shared_ptr` through its ticket.** The ticket's
   destructor is the last thing the connection thread does, and it touches
   only the tracker, never the server. So when `shutdown_and_drain()` sees the
   count reach zero, no connection thread can touch the server again, even
   though the thread itself may still be unwinding (Requirement 5.1).
2. **The ticket closes the fd while holding the tracker mutex, after erasing
   the entry.** The reaper and `stop()` call `shutdown()` only on fds they
   find in `_conns` under the same mutex, so they can never reach a closed
   or reused descriptor.
3. **`admit()` refuses once `_draining` is set**, so a connection accepted
   in the window between `stop()` starting and the accept thread exiting is
   closed rather than started.
4. **The reaper** waits on its condition variable until the earliest
   deadline or a change in deadlines, shuts down expired entries, counts
   them in `timed_out`, and marks them expired so they are counted once. It
   is a `std::thread` owned by the tracker and joined by
   `shutdown_and_drain()`.
5. **Handler-phase connections are never shut down by the reaper**
   (Requirement 2.4). `shutdown_and_drain()` does shut them down, which is
   harmless: the handler is not reading the socket, and the reply write that
   follows fails cleanly (Requirement 5.3).

### Accept loop (shared shape)

A free function `tcp_detail::run_accept_loop(listen_fd, running, tracker,
backoff_cv, spawn)` replaces the three hand-written loops:

```cpp
auto delay = 10ms;
while (running) {
    sockaddr_storage src{}; socklen_t len = sizeof(src);
    int fd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&src), &len, SOCK_CLOEXEC);
    if (fd < 0) {
        if (!running || is_fatal_accept_error(errno)) break;   // EBADF, EINVAL, ENOTSOCK
        tracker.note_accept_error();
        if (needs_backoff(errno)) {                            // EMFILE, ENFILE, ENOBUFS, ENOMEM
            wait_for(backoff_cv, delay, [&]{ return !running; });
            delay = std::min(delay * 2, 1000ms);
        }
        continue;
    }
    delay = 10ms;
    auto t = tracker.admit(fd, src);
    if (!t) { ::close(fd); continue; }
    try {
        std::thread(spawn, std::move(*t)).detach();
    } catch (const std::system_error&) {
        // `t` was moved into the thread's argument tuple only on success;
        // on failure its destructor releases the slot and closes fd.
    }
}
```

`stop()` notifies `backoff_cv` after clearing `running`, so a back-off wait
ends at once (Requirement 3.3). `accept4(..., SOCK_CLOEXEC)` keeps accepted
sockets out of any child process; the existing loops leak them across
`exec`.

The `std::thread` constructor takes its callable and arguments by value
into internal storage before starting the thread; if starting fails it
throws and destroys that storage, which destroys the ticket. The design
relies on that, and the implementation must check it with a test seam
(see Testing Strategy) rather than assume it.

### Per-server changes

**`tcp_rpc_server`** gains `std::shared_ptr<tcp_detail::connection_tracker>
_conns`. The limits are validated in the constructor; `start()` creates a
fresh tracker from them and starts its reaper, so a server can be stopped
and started again, and `connection_stats()` counts from the latest
`start()`. `admit()` opens each ticket in the request phase with its
deadline taken at accept time, so time a connection spends waiting for its
thread to start counts against `request_timeout`, as Requirement 2.1
words it. `stop()`, in order: clear `_running`, notify the back-off cv,
shut down and close the listening sockets as today, join the accept
threads, then `_conns->shutdown_and_drain()`. `handle()` takes a ticket and
calls `enter(phase::handler)` before dispatch and `enter(phase::reply)`
before `frame_send()`. The move constructor moves `_conns` (it is legal only
before `start()`).

**`tls_tcp_rpc_server`** (in `tls_detail::server_impl`): the same, with the
limits from `tls_tcp_rpc_config::server_limits`. The two `setsockopt` calls
in `handle()` and their comment are removed; the request phase spans
`SSL_accept()` through `frame_recv()`.

**Gossip listener**: the same, with the limits from
`tcp_gossip_config::listener_limits`. `stop_listener()` already shuts the
listening sockets down before joining; `shutdown_and_drain()` is added
after the join.

`connection_stats()` on each returns `_conns->stats()`.

### Framing helpers (`tcp_rpc.hpp`, `tls_tcp_rpc.hpp`)

```cpp
inline auto write_all(int fd, const void* buf, std::size_t n) -> bool {
    const auto* p = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t w = ::send(fd, p, n, k_send_flags);   // MSG_NOSIGNAL where defined
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w; n -= static_cast<std::size_t>(w);
    }
    return true;
}
```

On platforms without `MSG_NOSIGNAL` (`__APPLE__`), `k_send_flags` is 0 and
`connect_to()` and the accept path set `SO_NOSIGPIPE` on the socket
instead. Every fd passed to `write_all()` is a socket, so `send()` is a
drop-in replacement for `write()`.

`frame_recv()` (both copies) reads the body in chunks: it keeps the
existing length check, then loops reading `min(remaining, 1 MiB)` bytes,
appending to a `std::string` it `reserve()`s only up to what has arrived
plus the next chunk. A peer that announces 64 MiB and stops costs at most
1 MiB until it sends more (Requirement 7.1).

## Error Handling

| Situation | Result |
|---|---|
| Peer closed before reply | `send()` returns `EPIPE`; `frame_send()` false; connection closes; no signal |
| Request phase deadline | reaper shuts the socket down; read fails; no handler call; `timed_out` +1 |
| Reply phase deadline | reaper shuts the socket down; write fails; `timed_out` +1 |
| Over either limit | accepted fd closed at once; matching `refused_*` +1 |
| Transient accept error | `accept_errors` +1; back off if resource-related; keep accepting |
| Listener fd invalid | accept loop exits (only reachable after `stop()` or a bug) |
| Thread start throws | ticket released, fd closed, keep accepting |
| `stop()` during handler | waits for the handler; its reply write fails cleanly |
| Zero limit or timeout | `std::invalid_argument` from the constructor |

There is no logger injected into these classes today, and this spec does not
add one; the stats accessor is the observable surface.

## Testing Strategy

All cases use `tcp_server_limits` with small values (for example 300 ms
timeouts, `max_connections = 4`, `max_connections_per_source = 2`) so each
test finishes in a few seconds. Cases 2 to 6 are written once, as free
functions in `tests/tcp_server_hardening_cases.hpp` that take the server's
port and stats, and are called for `tcp_rpc_server` (in
`tests/tcp_rpc_unit_test.cpp`), `tls_tcp_rpc_server` (in
`tests/tls_tcp_rpc_integration_test.cpp`, where the certificate fixtures
live) and the gossip listener (in
`tests/tcp_gossip_transport_integration_test.cpp`).

1. **SIGPIPE** (`tcp_rpc_server` and the gossip listener). Set SIGPIPE to
   `SIG_DFL` at the start of the test. A raw client sends a valid
   `request_vote_request`, sets `SO_LINGER {1, 0}` and closes, which sends a
   reset; the handler sleeps 100 ms so the reply write happens after the
   reset arrives. The test process must survive, and a following normal RPC
   must succeed. A second case does the same from the client side against a
   raw server that resets after `accept()`.
2. **Silent peer.** Connect and send nothing. The server closes the socket
   (the client's `read()` returns 0 or `ECONNRESET`) within
   `request_timeout` plus 200 ms; `timed_out == 1`; `active_connections`
   returns to 0.
3. **Trickling peer.** Send the 4-byte header, then one byte every 50 ms.
   Closed within `request_timeout` plus 200 ms despite never pausing longer
   than 50 ms.
4. **Limits.** Open `max_connections_per_source` idle sockets from
   127.0.0.1; the next one is closed at once and `refused_per_source_limit`
   is 1. Then bind further clients to 127.0.0.2 and 127.0.0.3 (Linux routes
   all of 127.0.0.0/8 to `lo`) to fill up to `max_connections`; the next is
   refused with `refused_global_limit == 1`.
   After the idle sockets time out, a normal RPC succeeds.
5. **`EMFILE` recovery.** Lower `RLIMIT_NOFILE`'s soft limit to the current
   descriptor count plus a few, open sockets until a connection attempt
   fails with `EMFILE` on the server side (`accept_errors > 0`), close them,
   restore the limit, and check a normal RPC succeeds. The test restores the
   limit in a scope guard so a failure cannot affect later cases.
6. **`stop()` drains.** (a) The handler blocks on a latch; a client sends a
   request; another thread calls `stop()`; the test checks `stop()` has not
   returned after 200 ms, releases the latch, and checks it returns and the
   handler-completed flag was set before it did. (b) A client connects and
   sends nothing; `stop()` returns in well under `request_timeout`. The
   use-after-free this replaces is only reliably caught by a sanitizer, so
   these suites are built with `KYTHIRA_SANITIZER=address` and
   `KYTHIRA_SANITIZER=thread` during implementation, and are added to CI's
   `tsan` job, which today runs only the Beast and Proxygen suites.
7. **Thread start failure.** `run_accept_loop()` takes its spawn function
   as a parameter; a unit test passes one that throws `std::system_error`
   and checks the fd is closed, `active_connections` is 0, and the loop
   keeps accepting.
8. **Chunked frame read.** Over a `socketpair()`, announce 8 MiB and send
   it in uneven pieces; `frame_recv()` returns the exact payload. Announce
   64 MiB, send 10 bytes and close; `frame_recv()` returns `nullopt`.
9. **Limits validation.** Each zero field throws `std::invalid_argument`.

The existing round-trip, bind and start/stop tests in all three files must
pass unchanged.

## Correctness Properties

### Property 1: A closed peer never kills the process

For any sequence of peer closes and resets, no write performed by
`tcp_rpc_client`, `tcp_rpc_server` or the gossip transport raises SIGPIPE.

**Validates:** Requirement 1

### Property 2: Every non-handler phase is bounded

For any connection, the time spent in its request phase is at most
`request_timeout` plus the reaper's wake-up latency, and likewise for the
reply phase, regardless of what the peer sends.

**Validates:** Requirement 2

### Property 3: Admitted connections are bounded

At every instant, `active_connections <= max_connections`, and for every
source address the number of its admitted connections is at most
`max_connections_per_source`.

**Validates:** Requirement 4

### Property 4: The listener outlives transient errors

While the server is running, its accept loop exits only on `EBADF`,
`EINVAL` or `ENOTSOCK`.

**Validates:** Requirement 3

### Property 5: Nothing runs on a stopped server

After `stop()` returns, no thread reads a member of the server or calls one
of its handlers.

**Validates:** Requirement 5
