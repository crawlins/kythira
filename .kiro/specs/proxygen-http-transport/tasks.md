# Implementation Plan — Proxygen HTTP Transport

## Status: Not Started (0/17 tasks)

**Last Updated**: July 28, 2026 (spec authored, no implementation yet)

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

- [ ] 0. Spike: confirm Proxygen's actual API shape before committing to design details
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

- [ ] 1. Add `proxygen` as an optional vcpkg dependency, gated through existing machinery, requiring Folly
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

- [ ] 2. Config structs and the canonical Types bundle
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

- [ ] 3. Generic (any-future-backend) async bridge
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

- [ ] 4. TLS context setup, client and server
  - Client: `folly::SSLContext` built from `proxygen_client_config`'s TLS
    fields (`loadTrustedCertificates`/`loadCertificate`/`loadPrivateKey`/
    `ciphers`/`setVerificationOption`), used via
    `HTTPConnector::connectSSL`
  - Server: `wangle::SSLContextConfig` built from `proxygen_server_config`'s
    TLS fields, passed into `proxygen::HTTPServerOptions`
  - Certificate/key/CA validation error messages match the specificity
    (which file, what went wrong) both existing transports already provide
  - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5, 6.6_

- [ ] 5. Connection pool design
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

- [ ] 6. `proxygen_client<Types>`
  - Constructor taking `folly::IOThreadPoolExecutorBase&` (non-owning)
  - `send_request_vote`/`send_append_entries`/`send_install_snapshot`,
    all funneling through a shared `send_rpc` (generic-path body only in
    this task; the `if constexpr` fast-path branch is Task 8)
  - Timeout enforcement bounding the whole connect+send+receive sequence
    (Requirement 10.1), `connection_timeout` applied specifically to
    `HTTPConnector::connect`'s own `timeoutMs` (Requirement 10.3)
  - `static_assert` against `kythira::network_client`
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 3.1, 3.2, 3.3, 3.4, 8.1, 8.3, 10.1, 10.2, 10.3_

- [ ] 7. `proxygen_server<Types>`
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

- [ ] 8. Optional Folly-native fast path
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

- [ ] 9. `reload_tls_material()`/`enable_auto_reload()`/`disable_auto_reload()`, client and server
  - All-or-nothing validation, fresh `folly::SSLContext`/
    `wangle::SSLContextConfig` construction and swap-in, retired contexts
    kept alive for in-flight sessions
  - Polling-based auto-reload matching both existing transports' mtime-poll
    approach, cleanly joined on `disable_auto_reload()`
  - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5_

## Phase 8: Metrics (Task 10)

- [ ] 10. Metrics emission
  - Request count/latency/size (client and server), error-type-distinguished
    failure metrics, connection lifecycle metrics, server start/stop metrics
  - A label/tag distinguishing fast-path-taken vs. generic-bridge-taken on
    the existing request-count metric (Requirement 12.6) — this is the
    signal Task 12's fast-path test coverage and Task 15's benchmark both
    depend on being observable without separate instrumentation
  - _Requirements: 12.1, 12.2, 12.3, 12.4, 12.5, 12.6_

## Phase 9: Testing (Tasks 11-14)

- [ ] 11. Concept compliance and core parallel test suite
  - `static_assert`s (already added incrementally in Tasks 6-7, verified
    here as a complete pass)
  - `tests/proxygen_client_test.cpp`, `tests/proxygen_server_test.cpp`,
    `tests/proxygen_integration_test.cpp`, `tests/proxygen_ssl_*.cpp` —
    one-to-one with `tests/http_*`/`tests/beast_*`, covering
    request/response round-trips, TLS (mutual and server-only), timeout
    enforcement, connection pooling, malformed-request handling
  - All registered via CTest, labeled `proxygen-http`
  - _Requirements: 19.1, 19.2, 19.4, 19.6_

- [ ] 12. Fast-path-specific test coverage
  - A test confirming the Folly fast path is actually taken under
    `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly` (via Task 10's metrics
    distinction, not merely "the RPC succeeded")
  - A test confirming the generic bridge is used instead under `stdexec`/
    `boost`
  - A test forcing the generic bridge under the Folly backend specifically
    (Property 12's escape hatch) and asserting equivalent results to the
    fast path for the same RPC
  - _Requirements: 19.3_

- [ ] 13. Concurrency-specific coverage
  - Many concurrent in-flight RPCs against a small
    `folly::IOThreadPoolExecutor` thread count (Requirement 3.4)
  - A test specifically exercising Requirement 3.5/Property 6's structural
    no-interleaving claim (concurrent RPCs to the *same* target node,
    asserting response correctness under interleavable timing, not just
    "it didn't crash")
  - Run under `build-asan`/ThreadSanitizer, matching Beast's own precedent
    for this test category
  - _Requirements: 3.4, 3.5_

- [ ] 14. Three-way cross-transport equivalence
  - Extends `.kiro/specs/boost-beast-http-transport/`'s own two-way
    (cpp-httplib vs. Beast) property test to include Proxygen, asserting
    equivalent externally-observable results across all three for the same
    `Types`-bundle shape and RPC sequence
  - _Requirements: 19.5_

## Phase 10: Performance Benchmark (Task 15)

- [ ] 15. Extend the future-backend performance benchmark harness
  - New scenarios in `examples/performance_benchmark_report.cpp`/
    `tests/performance_benchmark_test.cpp` comparing the generic bridge
    path and the Folly fast path for the same RPC-shaped operation, under
    `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly`
  - Reported as a comparison report (not a sanity floor gating CI),
    matching `.kiro/specs/future-backend-performance-benchmark/`'s own
    distinction and rationale
  - At least one scenario specifically targeting the `folly::IOBuf`
    zero-copy claim (a large, `install_snapshot`-sized body) rather than
    only small-message latency, so the Introduction's architectural claims
    are either measured or explicitly labeled unmeasured in `design.md`
  - Results recorded in this spec's own documentation
  - _Requirements: 17.1, 17.2, 17.3, 17.4_

## Phase 11: Documentation (Task 16)

- [ ] 16. Documentation and examples
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

## Notes

This spec was authored without any implementation started — `spike-notes.md`
does not yet exist in this directory and should be created as Task 0's
output, following `.kiro/specs/boost-beast-http-transport/spike-notes.md`'s
and `.kiro/specs/stdexec-future-backend/spike-notes.md`'s precedent for
format. Several requirements in `requirements.md` explicitly defer a naming
or mechanism decision to the spike (e.g. Requirement 8.1's
`EventBase*`-vs-executor question, Requirement 10.1's timeout mechanism,
Requirement 15.3's exact bundle name, Requirement 16.3's continuation
scheduling mechanism) — `design.md` should be updated to reflect the spike's
actual findings before Task 1 begins, the same way Beast's own `design.md`
was updated post-implementation where reality diverged from its original
sketch (that spec's Data Models section is the worked example).

The single highest-risk unknown in this spec is Requirement 21.4/21.5's
question: does wrapping a `folly::Promise<T>` fulfilled inside a
`proxygen::HTTPTransaction::Handler` callback into `kythira::Future<T>`
actually behave correctly end to end, with no lifetime hazard analogous to
the two real, `AddressSanitizer`-confirmed heap-use-after-free bugs the
Beast spec's own `thenValue`-flattening composition surfaced during its
implementation. Requirement 16's entire fast path — the specific thing this
spec exists to add beyond what a straightforward "port Beast's approach to
Proxygen" spec would already cover — depends on this holding. The Phase 0
spike (Task 0) should treat this as its most important open question, not a
formality to confirm in passing.
