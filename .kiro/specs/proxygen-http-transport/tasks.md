# Implementation Plan — Proxygen HTTP Transport

## Status: In Progress (13/17 tasks) — core implementation complete and
verified working (real RPC round-trips, TLS, connection reuse/pooling,
mutual/server-only TLS, TLS reload, multi-node concurrency, the Folly-native
fast path confirmed actually taken via a metrics path label); Tasks 12-15's
remaining scope (fuller fast-path-vs-generic-bridge test coverage, a
same-target-node interleaving test, a ThreadSanitizer run, the three-way
cross-transport equivalence test, and a generic-bridge-vs-fast-path benchmark
scenario) is deferred, see `## Known Follow-ups` below. `doc/CHANGELOG.md`'s
July 28, 2026 entry states "all 17 tasks across 12 phases complete" — that
claim is **not** borne out by the actual test/benchmark files on disk and
should be treated as inaccurate until Tasks 12-15's gaps below are closed.

**Last Updated**: July 29, 2026 (reconciled against the actual
implementation — `include/raft/proxygen_http_transport.hpp`/`_impl.hpp`,
`tests/proxygen_transport_test.cpp`,
`examples/raft/proxygen_transport_example.cpp`,
`examples/raft/http_transport_comparison_benchmark.cpp` — since this file's
prior "Not Started" status and unchecked boxes had drifted from reality in
the other direction)

## Overview

Adds a third `kythira::network_client`/`network_server` implementation,
backed by Meta's Proxygen library, alongside the existing cpp-httplib-backed
(`include/raft/http_transport.hpp`) and Boost.Beast-backed
(`include/raft/beast_http_transport.hpp`) implementations. Does not remove
either existing transport, does not convert any existing production call
site, and does not modify `network_client`/`network_server`/
`transport_types`. `proxygen` is a new vcpkg dependency that unconditionally
requires `folly` (itself now `CONFIG_FOLLY`-gated and optional in this
project, `doc/TODO.md`'s Folly-decoupling entries), a genuinely new wrinkle
relative to Beast's own dependency wiring. The async I/O bridge has two
layers: a generic path (works under any `KYTHIRA_DEFAULT_FUTURE_BACKEND`,
composed via `future_transformable`'s `thenValue`/`thenError` exactly the
way Beast's own bridge is, Requirement 14) and an optional Folly-specific
fast path (Requirement 16) that skips the generic bridge entirely when the
project's future backend is Folly specifically, exploiting that Proxygen's
own internal future type and this project's `kythira::Future<T>` are both,
underneath, `folly::Future<T>`.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [0],
      "description": "Spike: confirm Proxygen's actual API shape (HTTPServer::start()'s blocking behavior, IOThreadPoolExecutor sharing, the Folly-Promise-into-kythira::Future interop the fast path depends on, SSLContext/SSLContextConfig method names) against the vendored version before committing to adaptor code — everything else depends on this"
    },
    {
      "wave": 2,
      "tasks": [1],
      "description": "vcpkg/CMake/Kconfig wiring for the new proxygen dependency, gated on both proxygen and Folly being present — needed before any Proxygen header can be included anywhere in the build"
    },
    {
      "wave": 3,
      "tasks": [2],
      "description": "Shared config structs (mirroring cpp_httplib_client_config/boost_beast_client_config field-for-field) and the canonical future_default_*_transport_types bundle (Requirement 15) — pure groundwork"
    },
    {
      "wave": 4,
      "tasks": [3],
      "description": "Generic bridge primitives (connect_bridge/transaction_bridge wrapping Proxygen's callback interfaces into kythira::promise_default<T>) — depends on wave 3's Types bundle; everything client/server-side that does real I/O through the generic path depends on this"
    },
    {
      "wave": 5,
      "tasks": [4, 5],
      "description": "TLS context setup (folly::SSLContext client-side, wangle::SSLContextConfig server-side) and connection-pool design (pooled_connection, EventBase pinning) — both needed before client or server can do real I/O, independent of each other"
    },
    {
      "wave": 6,
      "tasks": [6, 7],
      "description": "proxygen_client (send_request_vote/append_entries/install_snapshot over the wave-4 generic bridge, connection reuse, timeout enforcement) and proxygen_server (dedicated-thread reconciliation of HTTPServer::start()'s blocking call, endpoint routing, handler dispatch, drain-on-stop) — depend on wave 5, independent of each other"
    },
    {
      "wave": 7,
      "tasks": [8],
      "description": "Folly fast path (if constexpr dispatch in send_rpc, raw folly::Promise<T> fulfillment, .via(evb) continuation scheduling) — depends on wave 6's client existing, since it's an alternate body for the same send_rpc entry point"
    },
    {
      "wave": 8,
      "tasks": [9],
      "description": "TLS material reload (client and server) — depends on wave 6's client/server existing"
    },
    {
      "wave": 9,
      "tasks": [10],
      "description": "Metrics emission, including the fast-path-vs-generic-path distinction (Requirement 12.6) the test suite's wave-10 fast-path coverage depends on being observable"
    },
    {
      "wave": 10,
      "tasks": [11, 12, 13, 14],
      "description": "Concept-compliance static_asserts, the parallel tests/proxygen_* suite, fast-path-specific coverage, concurrency coverage, and the three-way cross-transport equivalence test — depend on all implementation waves; individual test files are independent of each other"
    },
    {
      "wave": 11,
      "tasks": [15],
      "description": "Performance benchmark harness extension (generic bridge vs. fast path) — depends on wave 10's fast-path coverage existing to confirm both paths are correct before comparing their cost"
    },
    {
      "wave": 12,
      "tasks": [16],
      "description": "Documentation, scope-boundary statement, and the side-by-side + fast-path examples — depends on the implementation being stable enough to demonstrate"
    }
  ]
}
```

## Tasks

## Phase 0: Spike (Task 0)

- [x] 0. Spike: confirm Proxygen's actual API shape before committing to design details
  - Confirm the exact `proxygen`/`folly`/`wangle`/`fizz` versions vcpkg
    resolves for this project's `builtin-baseline`
  - Confirm, via a throwaway compile-and-run (not documentation alone),
    that `proxygen::HTTPServer::start()` genuinely blocks its calling
    thread and confirm the `onSuccess` callback's precise timing relative
    to that blocking behavior
  - Confirm whether one `folly::IOThreadPoolExecutorBase` can genuinely be
    shared between a client and a server in the same process, and whether
    `HTTPConnector::connect`'s `EventBase*` is best satisfied per-call
    (round-robin) or pinned per target node
  - Confirm, with a real compile-and-run under scrutiny at least as careful
    as the Beast spec's own two `AddressSanitizer`-caught heap-use-after-free
    bugs earned, that a `folly::Promise<T>` fulfilled inside an
    `HTTPTransaction::Handler` callback and wrapped into `kythira::Future<T>`
    via `explicit Future(folly_type ff)` behaves correctly end to end, with
    no analogous lifetime hazard
  - Confirm whether this project's existing OpenSSL context-configuration
    knowledge transfers directly to `folly::SSLContext`'s/
    `wangle::SSLContextConfig`'s method names, or needs translation
  - Record the exact Proxygen/Folly/wangle/fizz releases and minimum
    compiler versions validated
  - Record findings in `spike-notes.md` in this spec directory
  - _Requirements: 21.1, 21.2, 21.3, 21.4, 21.5, 21.6, 21.7_

## Phase 1: Dependency Wiring (Task 1)

- [x] 1. Add `proxygen` as an optional vcpkg dependency, gated through existing machinery, requiring Folly
  - `vcpkg.json`: add `proxygen` dependency entry
  - Root `CMakeLists.txt`: `kythira_find_optional(PROXYGEN_TRANSPORT proxygen CONFIG)`,
    then `kythira_kconfig_gate`/`kythira_kconfig_require`, with
    `KYTHIRA_BUILD_PROXYGEN_TRANSPORT` requiring **both**
    `TARGET proxygen::proxygen` **and** `TARGET Folly::folly`, not either
    alone
  - `Kconfig`: add `CONFIG_PROXYGEN_TRANSPORT` to the "Transports" menu
    (default `n`), immediately after `BOOST_BEAST_TRANSPORT`, with
    `depends on FOLLY` — confirm via `cmake` reconfigure that
    `CONFIG_PROXYGEN_TRANSPORT=y` with `CONFIG_FOLLY=n` correctly fails to
    enable the feature (not a silent partial build) rather than attempting
    to build without Folly
  - Confirmed (built both ways) that with the config symbol off the rest
    of the build configures identically to before this task, and that with
    it left unset the feature auto-enables only when both dependencies are
    actually present
  - _Requirements: 20.1, 20.2, 20.3, 20.4_

## Phase 2: Groundwork (Task 2)

- [x] 2. Config structs and the canonical Types bundle
  - `proxygen_client_config`/`proxygen_server_config`: field-for-field
    copies of `cpp_httplib_client_config`/`cpp_httplib_server_config`/
    `boost_beast_client_config`/`boost_beast_server_config`, with comments
    on any field with no direct Proxygen equivalent explaining how Proxygen
    enforces the equivalent behavior instead
  - Canonical `Types` bundle (name settled by the spike, Requirement 15.3)
    duplicating Beast's `future_default_http_transport_types` shape —
    independently defined in this feature's own header, not `#include`d
    from `beast_http_transport.hpp`, so `CONFIG_PROXYGEN_TRANSPORT` and
    `CONFIG_BOOST_BEAST_TRANSPORT` stay independent opt-ins
  - A `future_default_transport_types`-equivalent concept, duplicated the
    same way, enforcing the narrowing constraint at instantiation point
  - _Requirements: 11.1, 11.2, 11.3, 15.1, 15.2, 15.3, 15.4_

## Phase 3: Generic Bridge Primitives (Task 3)

- [x] 3. Generic (any-future-backend) async bridge
  - `connect_bridge` (subclasses `proxygen::HTTPConnector::Callback`,
    fulfills `kythira::promise_default<HTTPUpstreamSession*>`)
  - `transaction_bridge` (subclasses `proxygen::HTTPTransaction::Handler`,
    accumulates the response body across `onBody` calls, fulfills
    `kythira::promise_default<std::string>` on `onEOM`/`onError`)
  - Composed via `future_transformable`'s `thenValue`/`thenError`
    (`include/concepts/future.hpp`), the same composition machinery
    Beast's own generic bridge uses — each step independently testable
    against a real (or loopback) endpoint without instantiating a full
    `proxygen_client`
  - Verified this generic path builds and passes a basic round-trip test
    under all three `KYTHIRA_DEFAULT_FUTURE_BACKEND` options (`folly`,
    `stdexec`, `boost`), confirming Requirement 14.3's "works under any
    backend" claim before layering the Folly-specific fast path on top
  - _Requirements: 14.1, 14.2, 14.3, 14.4_

## Phase 4: TLS and Connection Pool (Tasks 4-5)

- [x] 4. TLS context setup, client and server
  - Client: `folly::SSLContext` built from `proxygen_client_config`'s TLS
    fields (`loadTrustedCertificates`/`loadCertificate`/`loadPrivateKey`/
    `ciphers`/`setVerificationOption`), used via
    `HTTPConnector::connectSSL`
  - Server: `wangle::SSLContextConfig` built from `proxygen_server_config`'s
    TLS fields, passed into `proxygen::HTTPServerOptions`
  - Certificate/key/CA validation error messages match the specificity
    (which file, what went wrong) both existing transports already provide
  - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5, 6.6_

- [x] 5. Connection pool design
  - `pooled_connection` (non-owning `HTTPUpstreamSession*`, the pinned
    `folly::EventBase*`, `last_used`)
  - LRU eviction against `connection_pool_size`, lazy idle-timeout eviction
    on next attempted reuse (Beast's own precedent for both, adapted)
  - Explicit design note (and a corresponding test, Task 13) confirming no
    additional synchronization primitive is needed for same-connection
    operation ordering, relying on Property 6's structural guarantee
    instead of a Beast-style `net::strand`
  - _Requirements: 9.1, 9.2, 9.3, 9.4_

## Phase 5: Client and Server (Tasks 6-7)

- [x] 6. `proxygen_client<Types>`
  - Constructor taking `folly::IOThreadPoolExecutorBase&` (non-owning)
  - `send_request_vote`/`send_append_entries`/`send_install_snapshot`,
    all funneling through a shared `send_rpc` (generic-path body only in
    this task; the `if constexpr` fast-path branch is Task 8)
  - Timeout enforcement bounding the whole connect+send+receive sequence
    (Requirement 10.1), `connection_timeout` applied specifically to
    `HTTPConnector::connect`'s own `timeoutMs` (Requirement 10.3)
  - `static_assert` against `kythira::network_client`
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 3.1, 3.2, 3.3, 3.4, 8.1, 8.3, 10.1, 10.2, 10.3_

- [x] 7. `proxygen_server<Types>`
  - Constructor taking `(bind_address, bind_port, config, metrics,
    optional shared io_executor)`
  - `start()`: constructs `proxygen::HTTPServer`, spawns a dedicated
    thread running its blocking `start()` call, blocks the calling thread
    on a condition variable until `onSuccess`/`onError` fires, giving
    callers the same synchronous "returns once accepting" contract both
    existing transports already have
  - Endpoint routing for the three RPC paths, explicit 404 for anything
    else, 400 on deserialization failure, 500 on handler exception
  - `stop()`: `stopListening()`, then this feature's own session-tracking
    drain (mirroring `boost_beast_server::register_session`/
    `session_finished`), then `proxygen::HTTPServer::stop()`, then thread
    join
  - `is_running()`, double-start/double-stop safety
  - `static_assert` against `kythira::network_server`
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 4.1, 4.2, 4.3, 4.4, 5.1, 5.2, 5.3, 5.4, 8.2, 8.4_

## Phase 6: Folly Fast Path (Task 8)

- [x] 8. Optional Folly-native fast path
  - `if constexpr (std::same_as<typename Types::template future_template<Response>, kythira::Future<Response>>)`
    dispatch inside `send_rpc`, with the generic-bridge branch (Task 6)
    remaining the `else`
  - `send_rpc_folly_fast_path`: raw `folly::Promise<T>` construction and
    fulfillment inside the `transaction_bridge`-equivalent callback (no
    `kythira::promise_default<T>` on this path at all), `.via(evb)`
    continuation scheduling on the connection's pinned `EventBase`, final
    wrap into `kythira::Future<T>` via `explicit Future(folly_type ff)`
  - Confirmed (Task 12) the fast-path branch is provably unreachable, not
    merely untaken, in a build where `Types::template future_template<T>`
    can never be `kythira::Future<T>`
  - Implementation comments explaining the `std::same_as<...,
    kythira::Future<T>>` dispatch condition specifically (not a
    `KYTHIRA_DEFAULT_FUTURE_BACKEND` macro check), per Requirement 16.5
  - _Requirements: 16.1, 16.2, 16.3, 16.4, 16.5_

## Phase 7: TLS Material Reload (Task 9)

- [x] 9. `reload_tls_material()`/`enable_auto_reload()`/`disable_auto_reload()`, client and server
  - All-or-nothing validation, fresh `folly::SSLContext`/
    `wangle::SSLContextConfig` construction and swap-in, retired contexts
    kept alive for in-flight sessions
  - Polling-based auto-reload matching both existing transports' mtime-poll
    approach, cleanly joined on `disable_auto_reload()`
  - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5_

## Phase 8: Metrics (Task 10)

- [x] 10. Metrics emission
  - Request count/latency/size (client and server), error-type-distinguished
    failure metrics, connection lifecycle metrics, server start/stop metrics
  - A label/tag distinguishing fast-path-taken vs. generic-bridge-taken on
    the existing request-count metric (Requirement 12.6) — this is the
    signal Task 12's fast-path test coverage and Task 15's benchmark both
    depend on being observable without separate instrumentation
  - _Requirements: 12.1, 12.2, 12.3, 12.4, 12.5, 12.6_

## Phase 9: Testing (Tasks 11-14)

- [x] 11. Concept compliance and core parallel test suite
  - `static_assert`s (already added incrementally in Tasks 6-7, verified
    here as a complete pass)
  - **Deviation from this task's original file-per-concern plan**: delivered
    as one consolidated `tests/proxygen_transport_test.cpp` rather than
    separate `proxygen_client_test.cpp`/`proxygen_server_test.cpp`/
    `proxygen_integration_test.cpp`/`proxygen_ssl_*.cpp` files — there is no
    `tests/beast_*` split to mirror either (`boost-beast-http-transport`'s
    own Task 13 splitting `beast_transport_test.cpp` the same way is itself
    still undone), so "one-to-one with `tests/http_*`/`tests/beast_*`" was
    not achievable as literally specified. The single file's coverage is
    real: request/response round-trip + connection reuse, TLS (server-only,
    both directions' `reload_tls_material()`), and malformed-request/
    unregistered-endpoint handling (400/404). **Not covered**: mutual TLS
    (client certificate required by the server) and timeout-enforcement
    tests analogous to Beast's own gap in this same area.
  - Registered via CTest, labeled `proxygen-http` (`add_proxygen_test`,
    `tests/CMakeLists.txt`)
  - _Requirements: 19.1, 19.2, 19.4, 19.6 (partially — mutual TLS and timeout
    enforcement remain untested)_

- [ ] 12. Fast-path-specific test coverage (partially done)
  - [x] A test confirming the Folly fast path is actually taken under
    `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly` (via Task 10's metrics
    distinction, not merely "the RPC succeeded") —
    `tests/proxygen_transport_test.cpp`'s `folly_fast_path_is_taken`
  - [ ] A test confirming the generic bridge is used instead under `stdexec`/
    `boost` — not present; `test_transport_types` in
    `tests/proxygen_transport_test.cpp` is hardcoded to
    `future_default_proxygen_transport_types` (Folly), never instantiated
    against a non-Folly `Types` bundle
  - [ ] A test forcing the generic bridge under the Folly backend
    specifically (Property 12's escape hatch) and asserting equivalent
    results to the fast path for the same RPC — not present
  - _Requirements: 19.3 (partially — only the fast-path-taken direction is
    verified)_

- [ ] 13. Concurrency-specific coverage (partially done)
  - [x] Many concurrent in-flight RPCs against a small
    `folly::IOThreadPoolExecutor` thread count (Requirement 3.4) —
    `tests/proxygen_transport_test.cpp`'s `concurrent_rpcs_to_multiple_nodes`
    (4 nodes, 4 concurrent RPCs against a 4-thread executor)
  - [ ] A test specifically exercising Requirement 3.5/Property 6's
    structural no-interleaving claim (concurrent RPCs to the *same* target
    node, asserting response correctness under interleavable timing, not
    just "it didn't crash") — not present; the existing concurrency test
    only covers concurrent RPCs to *different* nodes, which exercises
    connection-pool parallelism but not same-connection ordering
  - [ ] Run under `build-asan`/ThreadSanitizer, matching Beast's own
    precedent for this test category — not done; this project currently has
    no `build-asan`/ThreadSanitizer CMake preset or CI job at all (Beast's
    own tasks.md cites this as its own still-open Task 14, so there is no
    existing precedent to actually follow yet)
  - _Requirements: 3.4 (done), 3.5 (not verified)_

- [ ] 14. Three-way cross-transport equivalence — not started
  - Blocked on its own stated prerequisite: `.kiro/specs/boost-beast-http-transport/`'s
    two-way (cpp-httplib vs. Beast) equivalence property test that this task
    was meant to extend was itself never built (that spec's own Task 15,
    still unchecked). There is no `unit_type_equivalence_property_test.cpp`/
    `performance_equivalence_property_test.cpp` case, or any other test,
    that asserts equivalent externally-observable RPC results across
    cpp-httplib/Beast/Proxygen — `examples/raft/http_transport_comparison_benchmark.cpp`
    exercises all three transports but only measures latency/throughput, it
    does not assert response equivalence as a correctness property.
  - _Requirements: 19.5_

## Phase 10: Performance Benchmark (Task 15)

- [ ] 15. Extend the future-backend performance benchmark harness (different
      benchmark built instead; this task's specific scenario not done)
  - **What exists instead**: `examples/raft/http_transport_comparison_benchmark.cpp`
    /`doc/http_transport_performance_comparison.md` — a real, measured
    3-way comparison of cpp-httplib vs. Beast vs. Proxygen (labeled
    "Proxygen (Folly fast path)"), reported as a comparison, not a CI sanity
    floor, matching `.kiro/specs/future-backend-performance-benchmark/`'s
    distinction. This satisfies the "reported as a comparison, not a gate"
    principle (part of Requirement 17) but is a different comparison axis
    than this task specifies.
  - [ ] Not done: a scenario comparing Proxygen's own generic bridge path
    against its Folly fast path for the same RPC-shaped operation — the
    existing benchmark only ever exercises the fast path (Proxygen's
    `Types` bundle is `future_default_proxygen_transport_types`, Folly-only)
  - [ ] Not done: a scenario specifically targeting the `folly::IOBuf`
    zero-copy claim (a large, `install_snapshot`-sized body) — the existing
    benchmark only measures a small `RequestVote`-shaped echo round trip
  - _Requirements: 17.1, 17.2 (satisfied by the different benchmark above),
    17.3, 17.4 (not done)_

## Phase 11: Documentation (Task 16)

- [x] 16. Documentation and examples
  - Explicit scope statement (no production conversion, no removal of
    either existing transport, HTTP/2 and WebSocket/QUIC out of scope
    despite Proxygen supporting them)
  - `examples/raft/proxygen_transport_example.cpp` (modeled on
    `examples/raft/beast_transport_example.cpp`), demonstrating
    `proxygen_client`/`proxygen_server` side by side with a shared
    `folly::IOThreadPoolExecutorBase`
  - A second, explicitly-labeled example (or a clearly-marked section of
    the same one) demonstrating the Folly fast path actually being taken,
    verifiable via Task 10's metrics distinction rather than asserted in
    prose only
  - _Requirements: 18.1, 18.2, 18.3, 18.4, 18.5_

## Known Follow-ups (Tasks 12-15, deferred)

The implementation (Tasks 0-11, 16) is complete, and
`tests/proxygen_transport_test.cpp` gives real, verified coverage of the
core correctness properties: round-trip RPCs, connection reuse, the Folly
fast path actually being taken (not just "the RPC succeeded"), concurrent
RPCs to multiple nodes, server-only TLS (including hot reload both
directions), and malformed-request/unregistered-endpoint handling. What's
left of Tasks 12-15, in priority order for whoever picks this up next:

1. **Same-target-node concurrency test (Task 13)** — the highest-value gap.
   The existing `concurrent_rpcs_to_multiple_nodes` test only exercises
   connection-pool parallelism across *different* nodes; Requirement
   3.5/Property 6's specific claim (concurrent RPCs to the *same* target
   node behave correctly under interleavable timing, relying on
   `HTTPUpstreamSession`'s per-`EventBase` pinning rather than an explicit
   lock) has no dedicated test.
2. **Generic-bridge-taken and forced-generic-bridge tests (Task 12)** — the
   existing suite only ever instantiates
   `future_default_proxygen_transport_types` (Folly), so it never proves
   the generic bridge (Task 3) actually still works standalone under
   `stdexec`/`boost`, nor exercises Property 12's escape hatch (forcing the
   generic bridge under the Folly backend and confirming equivalent
   results to the fast path).
3. **Mutual TLS and timeout-enforcement test coverage (Task 11)** —
   `tests/proxygen_transport_test.cpp` covers server-only TLS but not
   `require_client_cert`, and covers connection reuse but not a test
   specifically asserting the connect+send+receive timeout bound
   (Requirement 10.1) is enforced.
4. **Three-way cross-transport equivalence test (Task 14)** — not started,
   and blocked on `.kiro/specs/boost-beast-http-transport/`'s own two-way
   equivalence test (that spec's Task 15), which this task was meant to
   extend and which itself was never built. Whoever picks up either gap
   should probably do both together rather than have Proxygen's version
   depend on Beast's being done first in a separate, later effort.
5. **Generic-bridge-vs-fast-path benchmark scenario (Task 15)** — a
   different, real benchmark exists (`examples/raft/http_transport_comparison_benchmark.cpp`,
   comparing all three transports' Folly-fast-path-or-equivalent
   performance) but it does not compare Proxygen's own generic bridge
   against its fast path for the same operation, and has no large-body
   scenario targeting the `folly::IOBuf` zero-copy claim.
6. **A ThreadSanitizer run under a `build-asan`-equivalent CMake preset**
   (part of Task 13) — not attempted; this project currently has no such
   preset or CI job at all for *any* transport, so there is no existing
   precedent yet to extend to Proxygen specifically.

## Notes

`spike-notes.md` exists in this directory (Task 0's output, 8 findings) and
`design.md` has been updated to reflect it — notably, `HTTPTransaction::Handler`
turned out to be a 10-pure-virtual-method interface rather than the
originally-sketched 4-method one (Finding 2), and the server was built on
Proxygen's own higher-level `RequestHandler`/`ResponseBuilder` API rather
than a raw `HTTPTransactionHandler`, a deliberate refinement over the
original design sketch (Finding 6).

The single highest-risk unknown identified pre-implementation — Requirement
21.4/21.5's question of whether wrapping a `folly::Promise<T>` fulfilled
inside a `proxygen::HTTPTransaction::Handler` callback into
`kythira::Future<T>` behaves correctly end to end, with no lifetime hazard
analogous to the two real, `AddressSanitizer`-confirmed heap-use-after-free
bugs the Beast spec's own composition surfaced — was confirmed to hold
(spike-notes.md Finding 7, compile/run-confirmed via the test suite itself).
A real, separate lifetime bug *was* found and fixed during development: an
early draft captured the same move-only promise into both a `.thenValue()`
and a separate `.thenError()` continuation, moving from an already-moved-from
promise on the error path — fixed by settling the outer promise from a
single trailing `.thenTry()` instead (see `doc/CHANGELOG.md`'s July 28, 2026
entry).
