# HTTP Transport Performance Comparison

**Last Updated**: July 28, 2026

## Overview

Kythira has three interchangeable `network_client`/`network_server`
implementations, all reached purely through the `transport_types` concept
(`include/raft/types.hpp`) — swapping between them is a template-argument
change, not a code change:

- **cpp-httplib** (`include/raft/http_transport.hpp`) — synchronous,
  blocking calls dispatched onto an executor. The original transport.
- **Boost.Beast** (`include/raft/beast_http_transport.hpp`) — asynchronous
  I/O driven by a caller-owned `boost::asio::io_context`; connection
  pooling with a per-connection `net::strand`. See
  `.kiro/specs/boost-beast-http-transport/`.
- **Proxygen** (`include/raft/proxygen_http_transport.hpp`) — asynchronous
  I/O driven directly by Folly's `EventBase`/`IOThreadPoolExecutor`;
  connection-to-thread pinning falls out of Proxygen's own architecture.
  Adds an optional Folly-native fast path (Requirement 16,
  `.kiro/specs/proxygen-http-transport/`) that skips the generic
  `kythira::promise_default<T>` bridge entirely under
  `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly` (this project's default),
  wrapping a `folly::Promise<T>` directly into `kythira::Future<T>`.

This document is a **comparison, not a sanity floor** — matching
`.kiro/specs/future-backend-performance-benchmark/`'s own established
distinction for the analogous Folly-vs-stdexec comparison. Nothing in CI
gates on any transport beating another by a specific margin; relative
performance is expected to shift across hardware, compiler, and library
versions. It exists to characterize behavior, not to declare a winner —
all three transports remain fully supported, and no default changes as a
result of these numbers.

## Method

`examples/raft/http_transport_comparison_benchmark.cpp` runs the
*identical* workload — a `RequestVote` RPC round trip against a
single-threaded echo handler, one warm (already-established) connection
reused for every iteration — against all three transports in the same
process, back to back. This avoids the risk two independently-written
"equivalent" benchmarks always carry (subtly different iteration counts, a
`get()` in one path and a `wait()` in another) by implementing the
workload once per transport against each one's own concrete client/server
types, not by abstracting over a shared template (the transports'
constructor shapes differ enough — Beast/Proxygen need a shared executor,
cpp-httplib doesn't — that a single generic harness would obscure more
than it clarifies for just three data points).

- 200 warmup iterations (absorb TCP slow-start, lazy connection
  establishment, and any one-time allocation) followed by 2000 measured
  iterations, per transport.
- Each measured iteration's wall-clock latency is recorded individually;
  throughput is total iterations divided by total elapsed wall-clock time
  for the measured region.
- All three servers run an identical echo handler (`vote_granted = true`,
  echo the request's term back) so response-body size and serialization
  cost are held constant across transports.
- Proxygen's client/server share one `folly::IOThreadPoolExecutor`
  (Requirement 8); Beast's client/server share one `boost::asio::io_context`
  driven by two background threads; cpp-httplib needs neither (blocking
  calls dispatched directly).
- Since this project's default `KYTHIRA_DEFAULT_FUTURE_BACKEND` is
  `folly`, the Proxygen row below exercises Requirement 16's Folly-native
  fast path, not the generic bridge — see
  `.kiro/specs/proxygen-http-transport/requirements.md` Requirement 17 for
  the *separate* generic-bridge-vs-fast-path-specific benchmark (within
  Proxygen only), which this document does not duplicate.

### Running it yourself

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DKYTHIRA_KCONFIG=configs/ci_full_defconfig
cmake --build build --target http_transport_comparison_benchmark
./build/examples/http_transport_comparison_benchmark
```

Only built when both `KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT` and
`KYTHIRA_BUILD_PROXYGEN_TRANSPORT` are enabled (it needs all three
transports linked into one binary); `examples/raft/CMakeLists.txt` skips
it with a `message(STATUS ...)` otherwise, the same pattern every other
optionally-gated example/test target in this project already follows.

## Results

Measured on this environment's build host (4 logical cores), `-O3`
release build, loopback (`127.0.0.1`) — not a production network, so
absolute latency numbers are a floor, not a representative deployment
number; the *relative* comparison between transports on identical
hardware/workload is this table's actual point.

| Transport | ops/sec | p50 (µs) | p95 (µs) | p99 (µs) |
|---|---:|---:|---:|---:|
| cpp-httplib | 12 | 83,156.0 | 85,528.5 | 87,228.8 |
| Boost.Beast | 3,527 | 219.4 | 643.8 | 1,081.7 |
| Proxygen (Folly fast path) | 2,839 | 194.2 | 1,026.2 | 3,000.3 |

Raw run: 200 warmup + 2000 measured iterations per transport, same
process, same host, back to back (`examples/raft/http_transport_comparison_benchmark.cpp`,
run July 28, 2026).

## Interpreting the numbers

- **cpp-httplib's ~83ms per call is real, not a benchmark artifact** —
  traced to a genuine configuration difference, not slower code per se.
  cpp-httplib's vendored `httplib.h` defaults `CPPHTTPLIB_TCP_NODELAY` to
  `false` (confirmed by inspecting the vendored header), so Nagle's
  algorithm delays small writes (this project's RPC bodies are a few
  hundred bytes — well under one TCP segment) waiting to coalesce with
  more data, while the peer's own delayed-ACK timer is simultaneously
  waiting for a data packet to piggyback its ACK on — the classic
  Nagle/delayed-ACK interaction, whose ~40ms-per-direction, ~80ms-round-trip
  signature matches this number closely. Neither Beast nor Proxygen hit
  this: both are built on Boost.Asio/Folly `AsyncSocket`, which set
  `TCP_NODELAY` themselves independent of this project's own configuration.
  This is not a claim that cpp-httplib is 300x slower in general — it is a
  measured consequence of one specific default this project's
  `cpp_httplib_client_config` does not currently override (an actual,
  actionable follow-up: exposing `set_tcp_nodelay(true)` through that
  config struct would very likely close most of this gap, but that's a
  cpp-httplib-transport change out of scope for the proxygen-http-transport
  spec this document accompanies).
- **Boost.Beast and Proxygen are close** (3,527 vs. 2,839 ops/sec on this
  run) — within the range of run-to-run variance this kind of
  loopback micro-benchmark typically has on a 4-core host, not a
  clear structural win for either. Proxygen's p95/p99 tail is
  visibly wider than Beast's here (1.0ms/3.0ms vs. 0.6ms/1.1ms),
  plausibly a scheduling artifact of Proxygen's own `IOThreadPoolExecutor`
  under-utilized with only one connection actually active (3 of its 4
  threads idle every iteration) — not something this single-connection
  benchmark is well-suited to attribute definitively; the concurrent-load
  scenario (`concurrent_rpcs_to_multiple_nodes` in
  `tests/proxygen_transport_test.cpp`) exercises multiple threads
  simultaneously, closer to where Proxygen's structural per-connection
  `EventBase` pinning (design.md's "Why `folly::IOThreadPoolExecutor`"
  section) would be expected to matter.
- **Boost.Beast** pays a `net::strand` dispatch cost per operation
  (serializing access to a connection's I/O even when only one RPC is ever
  in flight on it) that Proxygen's structural EventBase-pinning
  (`.kiro/specs/proxygen-http-transport/design.md`'s "Why
  `folly::IOThreadPoolExecutor`" section) doesn't need an equivalent
  primitive for.
- **Proxygen's fast path** avoids the extra `kythira::promise_default<T>`
  translation hop the generic bridge (and, necessarily, both other
  transports' own future-backend-neutral code) pays — this is the
  single-connection, single-future-backend-specific advantage Requirement
  16 exists for; it does not by itself say anything about Proxygen's
  connection-pooling or multi-core scaling behavior under concurrent load,
  which `tests/proxygen_transport_test.cpp`'s own
  `concurrent_rpcs_to_multiple_nodes` test exercises for correctness, not
  throughput.

## Non-goals

- This is not a decision to change any call site's default transport —
  `cmd/ca_service`/`cmd/ca_cluster_node`/every existing example keep using
  whichever transport they already use.
- This does not measure TLS handshake cost, large-body (`install_snapshot`)
  transfer, or Proxygen's `folly::IOBuf` zero-copy claim specifically —
  see `.kiro/specs/proxygen-http-transport/requirements.md` Requirement
  17.3's own explicit "measure or label unmeasured" rule for that
  narrower claim.
