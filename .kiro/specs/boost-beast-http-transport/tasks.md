# Implementation Plan — Boost.Beast HTTP Transport

## Status: In Progress (15/18 tasks) — implementation complete and verified
working (real RPC round-trips, TLS, connection reuse/reload, multi-node
concurrency, all confirmed under AddressSanitizer with zero findings);
Tasks 13-15's remaining scope (fuller test-file parity, a formal `build-asan`/
ThreadSanitizer run, and the cross-transport equivalence test) is deferred,
see `## Notes`

**Last Updated**: July 26, 2026

## Overview

Adds a second `kythira::network_client`/`network_server` implementation,
backed by Boost.Beast's asynchronous HTTP API driven by a caller-owned
`net::io_context`, alongside the existing cpp-httplib-backed implementation
(`include/raft/http_transport.hpp`). Does not remove cpp-httplib, does not
convert any existing production call site, and does not modify
`network_client`/`network_server`/`transport_types`. `boost-beast` is a new
vcpkg dependency (header-only); `boost-asio`, which it is built on, is
already required. The async I/O bridge is composed using this project's
existing `kythira::promise_default`/`future_default` and
`future_transformable`'s `thenValue`/`thenError` chaining
(`include/concepts/future.hpp`) rather than hand-nested completion
handlers — the first use of that composition machinery for I/O in this
codebase, made possible by defining a canonical `Types` bundle whose
`future_template` is `kythira::future_default` (Requirement 19).

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [0],
      "description": "Spike: confirm Beast's actual API shape (expires_after cancellation, OpenSSL context reuse, coroutine/callback composition style) against the vendored version before committing to adaptor code — everything else depends on this"
    },
    {
      "wave": 2,
      "tasks": [1],
      "description": "vcpkg/CMake/Kconfig wiring for the new boost-beast dependency — needed before any Beast header can be included anywhere in the build"
    },
    {
      "wave": 3,
      "tasks": [2, 3],
      "description": "io_context ownership model and shared config structs (mirroring cpp_httplib_client_config/cpp_httplib_server_config) — both are pure groundwork, independent of each other"
    },
    {
      "wave": 4,
      "tasks": [4],
      "description": "async_*_kf primitive adaptors and the canonical future_default_http_transport_types bundle — depends on wave 3's io_context ownership model; everything client/server-side that does real I/O depends on this"
    },
    {
      "wave": 5,
      "tasks": [5, 6],
      "description": "TLS context setup (boost::asio::ssl::context, reusing or adapting existing certificate-loading code per the spike's findings) and connection-pool/strand design — both needed before client or server can do real I/O, independent of each other"
    },
    {
      "wave": 6,
      "tasks": [7, 8],
      "description": "boost_beast_client (send_request_vote/append_entries/install_snapshot as thenValue chains over the wave-4 adaptors, connection reuse, timeout enforcement) and boost_beast_server (accept loop, endpoint routing, handler dispatch) — depend on wave 5, independent of each other"
    },
    {
      "wave": 7,
      "tasks": [9, 10],
      "description": "TLS material reload (client and server) — depends on wave 6's client/server existing"
    },
    {
      "wave": 8,
      "tasks": [11],
      "description": "Metrics emission across client/server/connection-pool/lifecycle events — depends on wave 6"
    },
    {
      "wave": 9,
      "tasks": [12, 13, 14, 15],
      "description": "Concept-compliance static_asserts and the parallel tests/beast_* suite (one-to-one with tests/http_*), including the concurrency-specific and cross-transport-equivalence coverage — depends on all implementation waves; individual test files are independent of each other"
    },
    {
      "wave": 10,
      "tasks": [16, 17],
      "description": "Documentation, scope-boundary statement, and the side-by-side example — depends on the implementation being stable enough to demonstrate"
    }
  ]
}
```

## Tasks

## Phase 0: Spike (Task 0)

- [x] 0. Spike: confirm Boost.Beast's actual API shape before committing to design details
  - Confirm the exact `boost-beast`/`boost-asio` version vcpkg resolves for
    this project's `builtin-baseline`
  - Confirm `beast::tcp_stream::expires_after` actually cancels an
    in-flight `async_read`/`async_write`, via a throwaway compile-and-run
    against a real socket, not documentation alone
  - Confirm whether the existing OpenSSL context-configuration code
    (`http_transport_impl.hpp`'s certificate loading/validation,
    `tls_tcp_rpc.hpp`'s direct `SSL_CTX` calls) can be reused as-is against
    `boost::asio::ssl::context`, or needs an adaptation layer
  - Confirm which coroutine/completion-handler composition style
    (`boost::asio::awaitable<T>` + `co_spawn`, stackless
    `boost::asio::coroutine`, or plain callbacks) compiles cleanly under
    this project's C++23 + Clang 16+/GCC 13+ matrix
  - Record the exact Boost release and minimum compiler versions validated
  - Record findings in `spike-notes.md` in this spec directory
  - _Requirements: 18.1, 18.2, 18.3, 18.4, 18.5_

## Phase 1: Dependency Wiring (Task 1)

- [x] 1. Add `boost-beast` as an optional vcpkg dependency, gated through existing machinery
  - `vcpkg.json`: add `boost-beast` dependency entry
  - Root `CMakeLists.txt`: `beast` added to the single unified
    `find_package(Boost QUIET COMPONENTS ...)` call (the same way `json`
    already is — `boost-beast` isn't a separate CMake config package, it's
    a Boost component), then gated with `kythira_kconfig_gate`/
    `kythira_kconfig_require` (not a literal `kythira_find_optional` call,
    since Boost itself is looked up once, not per-component)
  - `Kconfig`: add `CONFIG_BOOST_BEAST_TRANSPORT` (default `n`), help text
    referencing this spec directory
  - Confirmed (built both ways, `cmake -S . -B ...` with and without
    `-DKCONFIG_BOOST_BEAST_TRANSPORT=OFF`) that with the config symbol off
    the rest of the build configures identically to before this task, and
    that with it left unset (no Kconfig `.config` supplied — CI's actual
    mode) `Boost::beast` is found and the feature auto-enables, matching
    every other optional dependency's degrade-gracefully convention
  - _Requirements: 17.1, 17.2, 17.3, 17.4_

## Phase 2: Groundwork (Tasks 2-3)

- [x] 2. Design and document the `net::io_context` ownership contract
  - Both `boost_beast_client`/`boost_beast_server` constructors take
    `net::io_context&`, never own one
  - Document explicitly (header comment + `requirements.md` cross-ref) that
    no progress happens without a caller-run `io_context::run()` thread,
    and that `stop()` never touches the caller's `io_context` itself
  - _Requirements: 8.1, 8.2, 8.3_

- [x] 3. Define `boost_beast_client_config`/`boost_beast_server_config`
  - Field-for-field match with `cpp_httplib_client_config`/
    `cpp_httplib_server_config` (names, types, defaults)
  - Document, per-field, how any cpp-httplib-specific field (e.g.
    `max_concurrent_connections`) maps to Beast's own enforcement mechanism
  - _Requirements: 11.1, 11.2, 11.3_

## Phase 3: Future-Concept-Based Async Composition (Task 4)

- [x] 4. Implement `async_connect_kf`/`async_write_kf`/`async_read_kf` and `future_default_http_transport_types`
  - Each adaptor: construct a `kythira::promise_default<kythira::unit>`,
    issue the corresponding Beast `async_*` call with a completion handler
    that `setValue`/`setException`s it, return `promise.getFuture()`
    immediately — design.md's Phase 2.5
  - New `future_default_http_transport_types<RPC_Serializer, Metrics, Executor>`
    bundle in `include/raft/beast_http_transport.hpp`, with
    `future_template = kythira::future_default<T>`, formalizing the
    pattern already used ad hoc in 13 existing CoAP test files
  - `requires` clause on `boost_beast_client`/`boost_beast_server`
    constraining `Types::future_template<T>` to `kythira::future_default<T>`
    — a compile-time error with a clear message on violation, not a deep
    substitution failure inside `send_rpc`; implemented as a named
    `future_default_transport_types` concept (subsumes `transport_types`),
    confirmed to both accept `future_default_http_transport_types` and
    reject `kythira::http_transport_types` via `static_assert` in
    `tests/beast_transport_test.cpp`
  - The adaptors are exercised through the full client/server integration
    tests (`tests/beast_transport_test.cpp`) rather than in isolation
    against a bare loopback endpoint as originally planned here — real
    development surfaced a genuine, non-obvious bug this way
    (`Future<T>::thenValue`'s future-flattening releasing a callback's own
    captures as soon as it *synchronously* returns, not once the future it
    returned actually completes — see design.md's Data Models section) that
    an isolated per-adaptor unit test would likely not have caught, since
    it only manifests once adaptors are chained the way `send()`/
    `handle_and_write()` actually chain them
  - _Requirements: 14.2, 14.3, 19.1, 19.2, 19.3, 19.4, 19.5_

## Phase 4: TLS and Connection Infrastructure (Tasks 5-6)

- [x] 5. Wire `boost::asio::ssl::context` construction from the existing certificate-loading code
  - Apply the Phase 0 spike's finding: reuse directly, or add the thin
    adaptation layer it identified
  - Cover client-side (`ca_cert_path`, optional client cert/key),
    server-side (`ssl_cert_path`/`ssl_key_path`, optional
    `require_client_cert`), cipher suite restriction, min/max TLS version
  - Specific, actionable error messages on invalid/missing material (not a
    raw OpenSSL error code)
  - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5, 6.6_

- [x] 6. Design and implement the per-target-node connection pool
  - One persistent `beast::tcp_stream`/`beast::ssl_stream<beast::tcp_stream>`
    per target node, strand-wrapped for safe concurrent access
    (`pooled_connection`, design.md's Data Models)
  - Idle-timeout eviction; transparent reconnect on a broken pooled
    connection; `connection_pool_size` enforced as an upper bound
  - _Requirements: 9.1, 9.2, 9.3, 9.4_

## Phase 5: Client and Server (Tasks 7-8)

- [x] 7. Implement `boost_beast_client<Types>`
  - `send_request_vote`/`send_append_entries`/`send_install_snapshot`,
    each: serialize via `Types::serializer_type`, then compose the wave-4
    adaptors as a `thenValue`/`thenError` chain (connect → write → read →
    deserialize, design.md's Phase 2.5) rather than a hand-written state
    machine
  - `beast::tcp_stream::expires_after(timeout)` set once, bounding the
    *entire* chain (connect + write + read), not each async step
    individually
  - `static_assert` against `network_client`
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 3.1, 3.2, 3.3, 3.4, 10.1, 10.2, 10.3_

- [x] 8. Implement `boost_beast_server<Types>`
  - `register_*_handler`, `start()`/`stop()`/`is_running()` matching
    `network_server`'s exact contract, including `start()` only returning
    once actually accepting and `stop()` draining in-flight requests before
    returning (without touching the caller-owned `io_context` — Requirement
    8.3)
  - Accept loop: `async_accept` → per-connection session object (Beast's
    standard `shared_from_this`-based session pattern) → immediate
    re-`async_accept`; no dedicated server thread the way cpp-httplib has
  - Explicit path routing against the three RPC endpoints; 404 for
    anything else (Beast provides no built-in router); 400 on
    deserialization failure; 500 on handler exception
  - `static_assert` against `network_server`
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 4.1, 4.2, 4.3, 4.4, 5.1, 5.2, 5.3, 5.4_

## Phase 6: TLS Material Reload (Tasks 9-10)

- [x] 9. Implement `boost_beast_client::reload_tls_material()`/`enable_auto_reload()`/`disable_auto_reload()`
  - All-or-nothing validation, then swap in a fresh
    `boost::asio::ssl::context` for subsequently-established connections
    only; retired contexts kept alive, not destroyed
  - Poll-based auto-reload (mtime-driven, matching the existing transport),
    cleanly joinable stop
  - _Requirements: 7.1, 7.3, 7.4, 7.5_

- [x] 10. Implement `boost_beast_server::reload_tls_material()`/auto-reload
  - Same all-or-nothing contract, applied without closing the listening
    acceptor or dropping established connections
  - _Requirements: 7.2, 7.3, 7.4, 7.5_

## Phase 7: Metrics (Task 11)

- [x] 11. Wire `Types::metrics_type` emission across client, server, connection pool, and lifecycle events
  - Request count/latency/size (client send, server receive)
  - Error metrics distinguishing connection/TLS/timeout/deserialization
    failure categories
  - Connection lifecycle and server start/stop metrics
  - _Requirements: 12.1, 12.2, 12.3, 12.4, 12.5_

## Phase 8: Testing (Tasks 12-15)

- [x] 12. Concept-compliance static_asserts
  - `boost_beast_client`/`boost_beast_server` against `network_client`/
    `network_server` at the bottom of `beast_http_transport.hpp`
  - Confirm both compile and behave correctly when instantiated with the
    new `future_default_http_transport_types` bundle, and confirm the
    `requires` clause (Task 4) correctly rejects `kythira::http_transport_types`
    (whose `future_template` is not `kythira::future_default`)
  - _Requirements: 13.1, 13.2, 13.3, 13.4, 16.1, 19.2_

- [ ] 13. Parallel `tests/beast_*` suite, one-to-one with `tests/http_*`
  - Delivered as one file, `tests/beast_transport_test.cpp`, not the
    one-file-per-concern layout `tests/http_*` uses — covers request/
    response round-trip, connection reuse (Property 5), idle-keep-alive
    connection draining on `stop()` (Property 8, a real regression test for
    a deadlock found during development), concurrent multi-node RPCs
    (Property 6), a genuine end-to-end TLS handshake (self-signed
    cert/key generated fresh per test run via the `openssl` CLI, not a
    placeholder/mocked PEM), and TLS material reload (client and server,
    both the success and re-validation-failure paths)
  - **Not done**: malformed-request handling (413/408-equivalent responses,
    partial/truncated request bodies), mutual TLS (`require_client_cert`),
    and the one-file-per-concern granularity `tests/http_*` has (separate
    `beast_client_test.cpp`/`beast_server_test.cpp`/`beast_ssl_*`/property
    tests tagged `**Feature: ... Property N: ...**`) — deferred, see
    `## Notes`
  - _Requirements: 16.2, 16.3_

- [ ] 14. Concurrency-specific coverage
  - `concurrent_rpcs_to_multiple_nodes` (Property 6) exercises many
    concurrent in-flight RPCs against a small `io_context` thread count;
    the full suite was run manually under AddressSanitizer (`-fsanitize=
    address`, ad hoc compile+link, not yet the project's own `build-asan`
    preset) with zero findings, including through the two real bugs this
    surfaced during development (a heap-use-after-free from
    `Future<T>::thenValue`'s future-flattening releasing a
    future-returning callback's own captures too early — see design.md's
    Data Models section — and the idle-session drain deadlock Property 8
    now documents)
  - **Not done**: a ThreadSanitizer run specifically (Requirement 14.4 asks
    for `build-asan`, which is this project's own registered TSan-capable
    build type/CMake preset, not an ad hoc `-fsanitize=address` compile) —
    deferred, see `## Notes`
  - _Requirements: 3.4, 14.4_

- [ ] 15. Cross-transport equivalence test
  - Instantiate both `cpp_httplib_client`/`server` and
    `boost_beast_client`/`server` against equivalent RPC sequences (note:
    against different `Types` bundles per Requirement 19.4 — `http_transport_types`
    for cpp-httplib, `future_default_http_transport_types` for Beast, both
    using the same `serializer_type`); assert equivalent
    externally-observable results
  - All Beast tests registered exclusively through CTest, labeled
    `beast-http`
  - **Not done at all** — deferred, see `## Notes`
  - _Requirements: 16.4, 16.5_

## Phase 9: Documentation (Tasks 16-17)

- [x] 16. Write the scope-boundary documentation
  - State explicitly: no production call site converted, cpp-httplib not
    removed/deprecated, WebSocket and HTTP/2 out of scope, `tcp_rpc.hpp`/
    `tls_tcp_rpc.hpp`/CoAP transport untouched
  - _Requirements: 15.1, 15.2, 15.3, 15.4, 15.5_

- [x] 17. Write the side-by-side usage example
  - Modeled on `examples/raft/http_transport_example.cpp`, showing
    `boost_beast_client`/`boost_beast_server` instantiated next to
    `cpp_httplib_client`/`cpp_httplib_server`, including the
    shared-`io_context`-plus-thread-pool pattern (`design.md`'s Phase 2
    example)
  - _Requirements: 15.6_

## Notes

- Depends on nothing landing first — `boost-asio` is already a required
  dependency (`vcpkg.json`), and `include/concepts/future.hpp`/
  `kythira::future_default`/`kythira::promise_default` are already
  backend-neutral, unchanged by this spec (Requirement 13.3, 19.4).
- One new vcpkg dependency: `boost-beast` itself (header-only). Everything
  else this spec touches — `boost-asio`, OpenSSL, the future/promise
  concept machinery — is already required for other reasons.
- `include/raft/http_transport.hpp`/`http_transport_impl.hpp` and
  `kythira::http_transport_types` are not modified by this spec — the new
  `future_default_http_transport_types` bundle (Task 4, design.md Phase
  2.5) is additive, in the new `beast_http_transport.hpp` header, not a
  change to the existing one.
- `include/raft/tcp_rpc.hpp`/`tls_tcp_rpc.hpp` (the actual production RPC
  transport) and the CoAP transport are unaffected — this spec's scope is
  strictly the new Beast-backed HTTP transport (Requirement 15.5).
- If the Phase 0 spike (Task 0) finds that `beast::tcp_stream::expires_after`
  does not reliably cancel an in-flight operation on the vendored Boost
  version, Requirement 10/Property 4's timeout-bounding design needs
  revisiting before Task 7 (client) proceeds — this is the one spike
  finding most likely to change downstream design, flagged here so it
  isn't missed if the spike surfaces it.

## Known Follow-ups (Tasks 13-15, deferred)

The implementation (Tasks 0-12, 16-17) is complete, and `tests/beast_transport_test.cpp`
gives real, verified coverage of every correctness property in design.md
except Property 9 (cross-transport equivalence, Task 15, never attempted).
What's left of Tasks 13-15, in priority order for whoever picks this up
next:

1. **Cross-transport equivalence test (Task 15)** — not started at all. The
   most valuable of the three remaining items: instantiate
   `cpp_httplib_client`/`server` and `boost_beast_client`/`server` against
   the same RPC sequence and assert equivalent externally-observable
   results, per design.md's Property 9.
2. **Malformed-request handling and mutual TLS coverage (Task 13)** —
   `tests/beast_transport_test.cpp` doesn't yet cover truncated/oversized
   request bodies (`max_request_body_size` is a config field but is not
   actually enforced by the current implementation — `async_read_kf` reads
   into a plain `beast_http::request<string_body>&`/`response<string_body>&`
   rather than a `beast_http::request_parser` with `.body_limit()` set, a
   real, separate gap from the test-coverage one), or `require_client_cert`
   (mutual TLS).
3. **Splitting `tests/beast_transport_test.cpp` into the one-file-per-concern
   layout `tests/http_*` uses** (`beast_client_test.cpp`/
   `beast_server_test.cpp`/`beast_ssl_*`) and tagging property tests
   `**Feature: boost-beast-http-transport, Property N: ...**` — lower
   priority than 1-2; the single file's coverage is real, just organized
   differently than the rest of this project's test suite.
4. **A ThreadSanitizer run under this project's own `build-asan` CMake
   preset** (Task 14) — the concurrency/lifetime bugs this feature's
   development actually found (documented in design.md's Data Models
   section and Property 8) were caught by manual, ad hoc
   `-fsanitize=address` runs against `tests/beast_transport_test.cpp`, not
   a registered, repeatable CI build variant. Worth doing before leaning on
   this transport for anything beyond development/testing use.
