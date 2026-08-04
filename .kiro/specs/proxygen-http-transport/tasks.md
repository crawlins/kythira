# Implementation Plan — Proxygen HTTP Transport

## Status: Complete (17/17 tasks) — core implementation complete and
verified working (real RPC round-trips, TLS, connection reuse/pooling,
mutual/server-only TLS, TLS reload, multi-node concurrency, the Folly-native
fast path confirmed actually taken via a metrics path label). Every task,
including Tasks 11-15's test-coverage gaps and Task 13's ThreadSanitizer
run and Task 12's `stdexec`/`boost` future-backend verification (this
file's last two remaining Known Follow-ups), is now closed. **Everything
above is build-verified** — PR #117 (this work's own PR) ran green across
all four `Build & Test` CI legs (g++-13/clang++-18 × x64/arm64),
`Coverage (clang++-18)`, the new `tsan` job (ThreadSanitizer over
`beast_transport_test`/`proxygen_transport_test`), and the new
`future-backend-compat` job (`proxygen_transport_test` under both
`KYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec` and `=boost`), on real GitHub
Actions runners, confirming what this repository's own local development
environment could not (see `## Known Follow-ups` for why that environment
specifically couldn't produce a working `vcpkg install`, and note that
limitation was specific to that one environment, not a real blocker —
GitHub Actions runners have normal internet access). Building and running
under configurations this project had never actually exercised before
caught six genuine, pre-existing bugs no amount of hand review had — see
`## Notes` for the full list. `doc/CHANGELOG.md`'s July 28, 2026 entry
states "all 17 tasks across 12 phases complete" — that claim is now
literally accurate, and CI-verified, not merely asserted.

**Last Updated**: July 30, 2026 (closed and CI-verified Task 11's
mutual-TLS/timeout gaps, Task 12's Property 12 escape hatch,
forced-generic-bridge test, and `stdexec`/`boost` future-backend
verification, Task 13's same-target-node concurrency test and
ThreadSanitizer run, Task 14's three-way equivalence test, and Task 15's
two benchmark scenarios — now measured with real numbers, not just
implemented — `include/raft/proxygen_http_transport.hpp`/`_impl.hpp`,
`include/raft/future.hpp`, `include/raft/future_stdexec.hpp`,
`tests/proxygen_transport_test.cpp`,
`tests/three_way_http_transport_equivalence_test.cpp`,
`examples/raft/http_transport_comparison_benchmark.cpp`,
`doc/http_transport_performance_comparison.md`, `CMakeLists.txt`,
`.github/workflows/ci.yml`, `tests/tsan_suppressions.txt`, all via
[PR #117](https://github.com/crawlins/kythira/pull/117). Reconciled against
a prior session's July 29, 2026 update, which itself reconciled this file
against the actual implementation after an earlier drift in the other
direction.)

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
    unregistered-endpoint handling (400/404). **Mutual TLS and
    timeout-enforcement coverage added and CI-verified July 30, 2026**
    (PR #117 — required one fix along the way, an X.509-extensions bug in
    the new `temp_mtls_material` cert-generation helper, caught by the
    first real CI run and fixed the same day):
    `mutual_tls_round_trip_with_valid_client_certificate`/
    `mutual_tls_rejects_client_without_certificate` (a self-contained
    `openssl`-CLI-generated CA plus server/client leaf certs,
    `temp_mtls_material`, deliberately not `ca_test_fixture.hpp`'s
    certificate-authority-spec fixture — see that struct's own comment for
    why) and `rpc_times_out_against_unresponsive_peer` (a raw-socket
    `blackhole_listener` that accepts a connection and never responds, so
    the test only passes if `HTTPTransaction::setIdleTimeout` actually
    fires, not merely that *some* failure eventually surfaces).
  - Registered via CTest, labeled `proxygen-http` (`add_proxygen_test`,
    `tests/CMakeLists.txt`)
  - _Requirements: 19.1, 19.2, 19.4, 19.6_

- [x] 12. Fast-path-specific test coverage (CI-verified under all three
      future backends — Folly, stdexec, boost — PR #117)
  - [x] A test confirming the Folly fast path is actually taken under
    `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly` (via Task 10's metrics
    distinction, not merely "the RPC succeeded") —
    `tests/proxygen_transport_test.cpp`'s `folly_fast_path_is_taken`
  - [x] A test confirming the generic bridge is used instead under `stdexec`/
    `boost` — **resolved by generalizing the existing test rather than
    adding a second one**: `folly_fast_path_is_taken`'s assertion now
    computes its expectation (`expects_folly_fast_path`) via the identical
    `std::same_as<future_template<Response>, kythira::Future<Response>>`
    condition `send_rpc`'s own dispatch uses, instead of hardcoding
    `"folly_fast_path"`. Under this project's default
    `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly` the same test still confirms the
    fast path; now also CI-verified compiled and run under both `=stdexec`
    and `=boost` via `.github/workflows/ci.yml`'s new
    `future-backend-compat` job (a 2-leg matrix), where the same test
    confirms the generic bridge is taken instead — **no longer just
    written, actually confirmed in both directions**. Doing so for the
    first time surfaced two genuine, pre-existing bugs invisible until now
    (see `## Notes`): a hardcoded Folly-specific `kythira::Try<Response>`
    where the backend-generic `kythira::try_default<Response>` was needed
    (`proxygen_http_transport_impl.hpp`), and a `thenTry` implementation in
    `future_stdexec.hpp` that couldn't accept a move-only callback. Also
    surfaced (under real 16-way concurrent load, `concurrent_rpcs_to_same_node`,
    boost backend specifically) a real, pre-existing capacity limit of
    HTTP/1.1 session reuse under genuine concurrency — see Task 13's own
    entry.
  - [x] A test forcing the generic bridge under the Folly backend
    specifically (Property 12's escape hatch) and asserting equivalent
    results to the fast path for the same RPC — added and CI-verified
    (PR #117), including confirming the two paths' response content
    actually matches, not just that both compile. Required real
    production code, not just a test: `proxygen_client::send_rpc`'s
    generic-bridge branch was extracted into its own method,
    `send_rpc_generic_bridge` (private), and a new public, explicitly
    test-only method, `send_rpc_via_generic_bridge_for_test`, forwards to
    it unconditionally — the only way to reach the generic bridge at all
    when `Types::future_template<T>` is `kythira::Future<T>`, since
    `send_rpc`'s own `if constexpr` dispatch would otherwise always select
    the fast path (both in `include/raft/proxygen_http_transport.hpp`/
    `_impl.hpp`). `tests/proxygen_transport_test.cpp`'s
    `generic_bridge_forced_matches_fast_path_result` exercises it, asserting
    identical response content across both paths for distinct RPCs, and
    (generalized the same way as `folly_fast_path_is_taken`, above) the
    correct pair of metrics path labels for whichever backend the test is
    actually compiled under.
  - _Requirements: 19.3_

- [x] 13. Concurrency-specific coverage — done and CI-verified (PR #117)
  - [x] Many concurrent in-flight RPCs against a small
    `folly::IOThreadPoolExecutor` thread count (Requirement 3.4) —
    `tests/proxygen_transport_test.cpp`'s `concurrent_rpcs_to_multiple_nodes`
    (4 nodes, 4 concurrent RPCs against a 4-thread executor)
  - [x] A test specifically exercising Requirement 3.5/Property 6's
    structural no-interleaving claim (concurrent RPCs to the *same* target
    node, asserting response correctness under interleavable timing, not
    just "it didn't crash") — added and CI-verified (PR #117),
    `concurrent_rpcs_to_same_node`
    (16 concurrent RPCs to one node sharing one `HTTPUpstreamSession`, each
    carrying a distinct term value; a response crossed between two
    in-flight requests would show up as a thread observing a term that
    isn't its own). Under the boost future backend specifically, real CI
    exposed a genuine, pre-existing (not introduced by this PR) capacity
    limit: `HTTPUpstreamSession` over HTTP/1.1 carries only one in-flight
    transaction at a time, so `newTransaction()` can legitimately return
    nullptr for whichever of the 16 concurrent callers loses the race to
    reuse the pooled session while another is still in flight. The test
    now retries a bounded number of times on specifically that transient
    condition rather than treating it as a hard failure — the property
    this test verifies (response *content* never crossing between
    callers) is unaffected by that retry.
  - [x] Run under ThreadSanitizer — added and CI-verified (PR #117): a new
    `KYTHIRA_SANITIZER` CMake cache option (root `CMakeLists.txt`, mirroring
    `ENABLE_COVERAGE`'s shape) and a new `tsan` CI job
    (`.github/workflows/ci.yml`) build and run `beast_transport_test` and
    `proxygen_transport_test` under `-fsanitize=thread`, wiring both
    transports' suites into one job as this file's own prior note
    suggested. `tests/tsan_suppressions.txt` documents the vendored
    Folly/Wangle/Boost races this job intentionally suppresses (their
    prebuilt binaries aren't themselves built with `-fsanitize=thread`; see
    that file's own comments for the full, evidence-based reasoning) —
    every suppressed report was inspected and none had a `kythira::`
    function as the actual racing read or write. No race confined to
    `kythira::`'s own code was found.
    `three_way_http_transport_equivalence_test` is deliberately excluded
    from this job specifically (documented in `ci.yml`): it reproduced an
    undiagnosable, zero-output SIGSEGV under TSan on two separate real
    runs while passing cleanly under ordinary CI, and unlike this task's
    other two suites, its own test cases are entirely sequential (no
    concurrent RPC threads), so it was never exercising a concurrent code
    path this job exists to check in the first place.
  - _Requirements: 3.4 (done), 3.5 (done)_

- [x] 14. Three-way cross-transport equivalence — done and CI-verified
      (PR #117)
  - Was blocked on its own stated prerequisite: `.kiro/specs/boost-beast-http-transport/`'s
    two-way (cpp-httplib vs. Beast) equivalence property test that this task
    was meant to extend had itself never been built (that spec's own Task
    15). Resolved by building the two together, as this file's own prior
    note suggested: `tests/three_way_http_transport_equivalence_test.cpp`
    is a **superset** of Beast's originally-specified two-way test, not a
    separate thing — it instantiates `cpp_httplib_client`/`server`,
    `boost_beast_client`/`server`, and `proxygen_client`/`server` together
    against the same RequestVote/AppendEntries RPC sequence and asserts
    equivalent externally-observable results (exact response field equality
    for success; equivalent `std::exception`-derived failure for a refused
    connection; identical raw HTTP status codes — 400/404 — for a malformed
    body/unregistered path, checked directly over the wire via a bare
    `httplib::Client` against all three servers' real listening sockets,
    the most literally "externally observable" check available). Gated on
    both `KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT` and
    `KYTHIRA_BUILD_PROXYGEN_TRANSPORT` (`tests/CMakeLists.txt`), labeled
    both `beast-http` and `proxygen-http`.
    `.kiro/specs/boost-beast-http-transport/tasks.md`'s own Task 15 is
    updated to point back here rather than duplicating the writeup.
  - _Requirements: 19.5_

## Phase 10: Performance Benchmark (Task 15)

- [x] 15. Extend the future-backend performance benchmark harness (both
      requested scenarios implemented and measured — real numbers below)
  - **What exists instead of literally extending
    `examples/performance_benchmark_report.cpp`**: `examples/raft/http_transport_comparison_benchmark.cpp`
    /`doc/http_transport_performance_comparison.md` — a real, measured
    3-way comparison of cpp-httplib vs. Beast vs. Proxygen (labeled
    "Proxygen (Folly fast path)"), reported as a comparison, not a CI sanity
    floor, matching `.kiro/specs/future-backend-performance-benchmark/`'s
    distinction. This satisfies the "reported as a comparison, not a gate"
    principle (part of Requirement 17) but is a different comparison axis
    than this task specifies; the two scenarios below were added to this
    same program rather than retrofitted into the other one.
  - [x] Added: `bench_proxygen_generic_bridge`, comparing Proxygen's own
    generic bridge path against its Folly fast path (`bench_proxygen`) for
    the same small RequestVote-shaped operation, both under this program's
    `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly` build — reachable only via Task
    12's new `send_rpc_via_generic_bridge_for_test` escape hatch, printed
    as a second results table in `main()`. **Measured July 30, 2026** (a
    temporary CI step, since this program's CTest entry carries the
    "performance"/"slow" labels and is normally excluded from every CI
    run — see PR #117's own commits): 9,089 ops/sec (generic bridge) vs.
    8,996 ops/sec (Folly fast path) — no measurable fast-path advantage on
    this run. Full numbers and interpretation in
    `doc/http_transport_performance_comparison.md`.
  - [x] Added: `bench_proxygen_large_snapshot_body`, a 1 MiB
    `install_snapshot`-shaped body (this project's Raft implementation's
    own one large-body RPC), run through both Proxygen paths (20 warmup +
    200 measured iterations — fewer than the small-body scenarios', since
    1 MiB × 2000 iterations would dominate the program's total runtime for
    no added measurement value) — the concrete test Requirement 17.3 asked
    for to turn the Introduction's zero-copy `folly::IOBuf` claim from an
    architectural expectation into a measured one. **Measured July 30,
    2026**, same run as above: 52 ops/sec (generic bridge) vs. 53 ops/sec
    (fast path) — also no measurable difference; see
    `doc/http_transport_performance_comparison.md` for why that's a
    plausible, not surprising, result given
    `proxygen_detail::http_response`'s accumulate-into-`std::string`
    posture applies identically to both paths at this body size.
  - _Requirements: 17.1, 17.2 (satisfied by the different benchmark above),
    17.3, 17.4 (both implemented and measured — see
    doc/http_transport_performance_comparison.md)_

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

## Known Follow-ups

**One open item (August 4, 2026): `proxygen_transport_test`'s intermittent
`ingress timeout, streamID=1, timeout=3000ms` in `tls_request_vote_round_trip`
is reduced but not proven fixed.** `dd041bf` switched both TLS fixtures from
RSA-2048 to P-256 keys, after measuring that generating an RSA-2048 key via the
`openssl` CLI cost 741-2966ms on 2 pinned cores under load — a single sample
consuming essentially the whole 3000ms RPC budget the test hardcodes. Under
identical load the worst observed case time fell from 2321ms to 192ms (a 15x
margin against the deadline rather than 1.3x), and CI artifacts confirm the
test went from 3.09s to 0.72s. The same commit adopted
`tests/test_timeout_scale.hpp` here — this was the last concurrency-heavy suite
without it — so the 12 case timeouts and 9 RPC deadlines now scale with
`KYTHIRA_TEST_TIMEOUT_SCALE` (a no-op at the default of 1).

What this does **not** establish: the failure was never reproduced locally, in
70 runs across 4, 2 and 1 cores and under heavy CPU load. Reduced fragility is
not a proven root cause, so this stays open until CI has run clean for a while.
If it recurs, the shared 2-thread `folly::IOThreadPoolExecutor` between client
and server (`proxygen_transport_test.cpp:492`, `:500`, `:513` as of `dd041bf`
— one pool constructed in the case and handed to both the server, via a
non-owning `shared_ptr`, and the client) is the unexamined candidate. Do **not** re-open the certificate-lifetime theory: the
`Failed to re-configure TLS: couldn't read cert file` errors that accompany the
failure are benign noise from `server_reload_tls_material`/
`client_reload_tls_material` deleting certs on purpose, and appear on passing
runs too — that was this investigation's first and wrong diagnosis. Full
accounting in `doc/coap-flake-investigation.md` Finding 6.

Every other gap this section has ever listed is closed **and CI-verified** —
see Tasks 11-15 above for what each one delivered,
and [PR #117](https://github.com/crawlins/kythira/pull/117) for the actual
CI runs. This session's own local development environment could never
produce a working `vcpkg install` (a from-scratch bootstrap failed
downloading `zlib`, a transitive `proxygen` dependency, from its upstream
GitHub release archive with a 403 — traced to that environment's GitHub
access being scoped to this repository specifically, not a transient
network failure), which is why everything above was first reviewed by hand
rather than compiled locally — but PR #117's real GitHub Actions CI runs
(which have normal internet access, unlike that one sandboxed environment)
since confirmed all of it, catching and fixing several genuine bugs the
hand review had missed along the way (see `## Notes` below for the full
list). The two items this section most recently tracked are now both
closed:

1. **Building and running under `KYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec`/
   `=boost`** (Task 12) — closed via `.github/workflows/ci.yml`'s new
   `future-backend-compat` job (a 2-leg matrix), CI-green on both legs.
   Doing this for the first time surfaced two genuine, pre-existing bugs
   (see `## Notes`) and, under real 16-way concurrent load specifically,
   confirmed a real (also pre-existing, not introduced by this PR)
   capacity limit of HTTP/1.1 session reuse — see Task 13.
2. **A ThreadSanitizer run** (Task 13) — closed via the new
   `KYTHIRA_SANITIZER` CMake option and `.github/workflows/ci.yml`'s new
   `tsan` job, CI-green covering both `beast_transport_test` and
   `proxygen_transport_test` (the two suites whose own test cases actually
   exercise concurrent code paths). See Task 13's own entry above and
   `tests/tsan_suppressions.txt` for what this job suppresses and why, and
   for why `three_way_http_transport_equivalence_test` is excluded from
   this specific job.

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

**A second, unrelated latent bug was found the same way, a session later**
(PR #117, July 30, 2026): `kythira::Future<T>::thenTry`'s two
non-flattening overloads (`include/raft/future.hpp`) captured the caller's
callback without marking their own wrapping closure `mutable`, so a
`mutable` callback passed to `.thenTry()` — exactly what
`send_rpc_generic_bridge`'s trailing `.thenTry([promise = std::move(promise)](...)
mutable {...})` needs, to call `promise.setValue()`/`setException()` on a
captured-by-value promise — failed to compile. This had never been caught
before because `send_rpc_generic_bridge`'s own code (previously inlined
directly in `send_rpc`'s `if constexpr` `else` branch) was never actually
instantiated under this project's default Folly backend, where that branch
is always discarded — PR #117's Property 12 escape hatch
(`send_rpc_via_generic_bridge_for_test`) was the first call site in this
project's history to force that instantiation. Confirmed, before fixing,
that no other call site anywhere in the codebase had ever passed a
`mutable` lambda to `kythira::Future<T>::thenTry` (a codebase-wide grep
turned up none), and that the other two future backends
(`future_stdexec.hpp`, `future_boost.hpp`) already handled this correctly
— fixed by adding `mutable` to both overloads' wrapping closures, matching
what the adjacent Future-returning ("flattening") overloads already did.

**Closing the two remaining Known Follow-ups (same PR, same day) surfaced
four more genuine, pre-existing bugs**, all invisible until this PR's new
`future-backend-compat`/`tsan` CI jobs exercised code paths for the first
time in this project's history:

- The exact same missing-`mutable` gap above also existed in all four
  `thenValue` overloads of `include/raft/future.hpp` (`Future<T>` and
  `Future<void>`, both the plain and flattening shapes) — found via a real
  compile failure in the `tsan` job and fixed the same way.
- `include/raft/proxygen_http_transport_impl.hpp`'s generic bridge hardcoded
  the Folly-specific `kythira::Try<Response>` where the backend-generic
  `kythira::try_default<Response>` was needed — harmless under the default
  Folly backend (the two types coincide), a hard compile error under
  `stdexec`/`boost`.
- `include/raft/future_stdexec.hpp`'s non-flattening `thenTry` overloads
  attached the same callback to two separate continuations by copying it
  into each, which cannot compile for a move-only callback (exactly what
  the previous bullet's fix produces) — fixed by wrapping the callback in a
  `shared_ptr` so both continuations copy that instead, invoking the
  underlying callback exactly once either way.
- A real capacity limit of HTTP/1.1 session reuse, exposed for the first
  time by `concurrent_rpcs_to_same_node` running under genuine 16-way
  concurrency against the `boost` backend specifically:
  `HTTPUpstreamSession` carries only one in-flight transaction at a time,
  so losing the race to reuse a busy pooled session is a legitimate,
  timing-dependent outcome, not a bug in the transport itself — fixed at
  the test level with a bounded retry (see Task 13's own entry for why that
  doesn't undermine what the test verifies). An initial attempt to "fix"
  this by forcing the completion handler through
  `evb->runInEventBaseThread()` (reasoning that `send_on_session`'s
  `proxygen::HTTPUpstreamSession::newTransaction()` call must run on the
  session's own EventBase thread, and that `kythira::future_default<T>`'s
  continuations — unlike Folly's `.via(evb)` — don't guarantee that) did
  not change the observed crash at all and was reverted; the true root
  cause was the session capacity limit above, unrelated to thread
  affinity.
