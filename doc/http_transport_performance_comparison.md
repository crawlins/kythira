# HTTP Transport Performance Comparison

**Last Updated**: August 12, 2026 (gRPC added as a fourth transport row —
`.kiro/specs/grpc-transport/` Task 13.4's performance sanity pass; see
`## gRPC` below for the full re-measured run. `## Results` below is still
the original July 28, 2026 three-transport run; the `## Generic bridge vs.
fast path` section's numbers are from July 30, 2026, on a CI host)

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
  `## Generic bridge vs. fast path, and large-body scenarios` below for the
  *separate* generic-bridge-vs-fast-path-specific benchmark (within
  Proxygen only), added to the same program but not (yet) re-run.

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

## Generic bridge vs. fast path, and large-body scenarios

Requirement 17.1/17.3 (`.kiro/specs/proxygen-http-transport/requirements.md`)
ask for two more comparisons, both now implemented in
`examples/raft/http_transport_comparison_benchmark.cpp`'s `main()` as two
additional tables printed after the cross-transport one above:

1. **Generic bridge vs. Folly fast path** (`bench_proxygen_generic_bridge`/
   `bench_proxygen`), same small `RequestVote`-shaped workload as `## Method`
   above, both under this program's `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly`
   build. Reachable at all only because of Property 12's test-only escape
   hatch (`proxygen_client::send_rpc_via_generic_bridge_for_test`,
   `include/raft/proxygen_http_transport.hpp`) — without it there would be
   no way to force the generic bridge under a Folly-backend `Types` bundle,
   since `send_rpc`'s own `if constexpr` dispatch (Requirement 16.1) always
   selects the fast path for one.
2. **1 MiB `install_snapshot` body, generic bridge vs. fast path**
   (`bench_proxygen_large_snapshot_body`) — the Introduction's zero-copy
   `folly::IOBuf` claim, at a body size actually representative of what this
   project's Raft implementation treats as its one large-body RPC (20
   warmup + 200 measured iterations per path, rather than the small-body
   scenarios' 200 + 2000 — see the function's own comment for why).

**Measured** (July 30, 2026, PR #117): this repository's own development
environment could not obtain a working `vcpkg install` (a from-scratch
bootstrap failed downloading `proxygen`'s and its transitive dependencies'
upstream GitHub release archives — not a transient network error, but that
environment's GitHub access being scoped to this repository specifically),
so these numbers instead come from a real GitHub Actions CI runner
(4-core `ubuntu-24.04`, g++-13, `-O3` Release), captured via a temporary
CI step added specifically to run this normally-excluded (`performance`/
`slow`-labeled) benchmark once and print its output — see PR #117's commits
for that temporary step, since it was reverted immediately after this run
rather than left as a permanent CI job (this benchmark's whole point is to
be run and read by a person, not gated on).

| Transport | ops/sec | p50 (µs) | p95 (µs) | p99 (µs) |
|---|---:|---:|---:|---:|
| Proxygen (generic bridge) | 9,089 | 103.3 | 140.3 | 148.6 |
| Proxygen (Folly fast path) | 8,996 | 105.0 | 142.6 | 147.8 |

| Transport | ops/sec | p50 (µs) | p95 (µs) | p99 (µs) |
|---|---:|---:|---:|---:|
| Proxygen 1 MiB snapshot (generic bridge) | 52 | 19,315.4 | 19,597.5 | 19,679.0 |
| Proxygen 1 MiB snapshot (fast path) | 53 | 19,003.7 | 19,324.5 | 19,423.9 |

(For reference, the same run's cross-transport table, `## Results`'
own comparison re-measured on this CI host rather than the original
development host: cpp-httplib 12 ops/sec / 82,001.3µs p50; Boost.Beast
7,547 ops/sec / 131.5µs p50; Proxygen fast path 9,018 ops/sec / 105.2µs
p50 — same relative ordering as `## Results` above, different host so not
directly comparable number-for-number to that table.)

**Interpreting these numbers: no measurable fast-path advantage at either
body size, on this run.** This is a genuine, slightly surprising result
worth stating plainly rather than explaining away — the Introduction's
architectural case for Requirement 16 (the fast path skips one
`kythira::promise_default<T>` translation hop the generic bridge pays) is
still structurally true, but at neither the small `RequestVote` size nor
the 1 MiB `install_snapshot` size did that translate into a measurable
throughput or latency win here: the generic bridge is marginally *faster*
in both tables (9,089 vs. 8,996 ops/sec; 52 vs. 53 ops/sec — the second
pair is within one iteration's rounding of each other and not a real
difference either way). Plausible reasons this project's own architecture
suggests, none independently confirmed by a further benchmark: the skipped
translation hop is genuinely cheap relative to the dominant costs at both
sizes (TCP/TLS-free loopback I/O and JSON serialization for the small case;
the 1 MiB body's own copy/transfer cost for the large case, which
`proxygen_detail::http_response`'s accumulate-into-`std::string` posture
(`include/raft/proxygen_http_transport.hpp`'s own header comment on that
struct) pays on *both* paths identically, generic bridge and fast path
alike — meaning this scenario was never actually positioned to isolate an
`IOBuf`-specific win in the first place, only to *check for* one that a
future, more targeted change (skipping that accumulation step) would be
needed to actually surface. Matching this document's own
"comparison, not a declaration of a winner" stance (`## Overview` above):
this measures what Requirement 17.1/17.3 asked to be measured, and the
honest answer is "no difference here," not "the fast path doesn't work" —
Task 12's own test coverage (`generic_bridge_forced_matches_fast_path_result`,
`tests/proxygen_transport_test.cpp`) already confirms both paths are
correct; this section is about their relative cost, and on this run, cost
was a wash.

## gRPC (Task 13.4's transport-level sanity pass)

`.kiro/specs/grpc-transport/` Task 13.4 asks for throughput/latency
numbers for the gRPC transport itself —
`doc/protobuf_serializer_performance_comparison.md` measures the
*serializer* and explicitly does not satisfy it. The benchmark now carries
gRPC as a fourth row (`bench_grpc`), plus a gRPC row in the 1 MiB
`install_snapshot` table (`bench_grpc_large_snapshot_body`), guarded by
`KYTHIRA_BENCH_HAS_GRPC` so the three-transport comparison still builds
where gRPC is absent. gRPC is not an HTTP-transport sibling in the code —
it owns its framing and codegen — but it answers the same question this
program exists for, so it is a row here rather than a second harness.

**Measured** (August 12, 2026, development host: 4 logical cores, g++-13
`-O3` Release, loopback, `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly`). All rows
below are from this single run, so they are comparable to each other but
not number-for-number to the July tables above (different host/day):

| Transport | ops/sec | p50 (µs) | p95 (µs) | p99 (µs) |
|---|---:|---:|---:|---:|
| cpp-httplib | 12 | 82,971.1 | 84,088.0 | 85,076.0 |
| Boost.Beast | 6,302 | 131.9 | 238.8 | 715.1 |
| Proxygen (Folly fast path) | 5,872 | 147.8 | 231.2 | 821.1 |
| **gRPC** | **3,349** | **228.2** | **514.3** | **1,236.8** |

| Transport | ops/sec | p50 (µs) | p95 (µs) | p99 (µs) |
|---|---:|---:|---:|---:|
| Proxygen 1 MiB snapshot (generic bridge) | 27 | 34,750.4 | 56,004.6 | 70,996.7 |
| Proxygen 1 MiB snapshot (fast path) | 35 | 27,581.6 | 35,707.5 | 37,619.1 |
| **gRPC 1 MiB snapshot** | **657** | **1,379.3** | **2,619.2** | **3,222.0** |

(The same run's within-Proxygen small-RPC pair read generic bridge 6,734
vs. fast path 5,691 ops/sec — the *opposite* ordering of the July 30 CI
run's wash. Treated as run-to-run variance on a 4-core host, consistent
with that section's own conclusion that the two paths' cost difference is
below this benchmark's noise floor at this body size.)

**Interpreting the gRPC rows:**

- **Small RPCs: gRPC is ~half the throughput of Beast/Proxygen here**
  (3,349 vs. 5,872–6,302 ops/sec; p50 228µs vs. 132–148µs). A
  single-connection, strictly-serialized ping-pong is close to a
  worst case for gRPC's machinery — every call pays HTTP/2 framing,
  protobuf encode/decode, completion-queue dispatch, and the transport's
  own executor hop, with none of the multiplexing that machinery buys
  anything on. This is a floor-shape sanity number, not a verdict on
  loaded behavior — matching this document's comparison-not-winner stance.
- **Large bodies: gRPC is ~19–24x faster than either Proxygen path**
  (657 vs. 27–35 ops/sec; p50 1.4ms vs. 27.6–34.8ms), and this *is* a
  structural difference, not noise: the HTTP transports JSON-encode the
  snapshot's 1 MiB byte vector through the `serializer_type` seam (text
  inflation plus per-byte encoding on both ends), while gRPC carries it as
  a protobuf `bytes` field over its own binary framing. For
  `install_snapshot`-shaped traffic, the transport's body encoding
  dominates everything else this document measures.
- The gRPC scenarios bind port 0 and read `bound_port()` back rather than
  taking a fixed 2809x port like the HTTP scenarios — `grpc_server`
  supports it, and it sidesteps the port-collision class entirely.

## Non-goals

- This is not a decision to change any call site's default transport —
  `cmd/ca_service`/`cmd/ca_cluster_node`/every existing example keep using
  whichever transport they already use.
- This does not measure TLS handshake cost.
