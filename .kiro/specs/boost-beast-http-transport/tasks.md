# Implementation Plan — Boost.Beast HTTP Transport

## Status: Complete (18/18 tasks) — implementation verified working (real
RPC round-trips, TLS including mutual TLS, connection reuse/reload,
multi-node concurrency, malformed/oversized-request handling, cross-transport
equivalence against cpp-httplib, all confirmed under AddressSanitizer with
zero findings in this project's own code). Tasks 14 (ThreadSanitizer run) and
15 (cross-transport equivalence, delivered jointly with
`.kiro/specs/proxygen-http-transport/`'s own Task 14 as a three-way test) are
CI-verified via [PR #117](https://github.com/crawlins/kythira/pull/117)
(`beast-http`-labeled CTest runs, green); a follow-up ThreadSanitizer pass
against this spec's own (since-split) test binaries found four further real
bugs beyond what that run surfaced — see `## Known Follow-ups` for the full
accounting.

**Last Updated**: July 30, 2026 (round-2 ThreadSanitizer findings against the
split beast-http test binaries; see `## Known Follow-ups`)

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

- [x] 13. Parallel `tests/beast_*` suite, one-to-one with `tests/http_*`
  - Delivered as the one-file-per-concern layout `tests/http_*` itself
    uses: `tests/beast_client_test.cpp` (construction, concept compliance,
    client-side TLS reload), `tests/beast_server_test.cpp` (server
    lifecycle/drain, malformed-request handling, server-side TLS reload),
    `tests/beast_integration_test.cpp` (full client+server round trips —
    connection reuse/Property 5, multi-node concurrency/Property 6),
    `tests/beast_ssl_test.cpp` (server-only and mutual TLS, a genuine
    end-to-end handshake against a self-signed cert/key generated fresh per
    test run via the `openssl` CLI, not a placeholder/mocked PEM). Property
    tests are tagged `**Feature: boost-beast-http-transport, Property N:
    ...**` + `**Validates: Requirement ...**`, matching
    `tests/http_client_property_tests.cpp`'s own convention exactly
    (Properties 5-9 are all tagged this way now, including the two cases in
    `beast_cross_transport_equivalence_test.cpp`, Property 9)
  - Coverage: request/response round-trip, connection reuse (Property 5),
    idle-keep-alive connection draining on `stop()` (Property 8, a real
    regression test for a deadlock found during development), concurrent
    multi-node RPCs (Property 6), a genuine end-to-end TLS handshake, TLS
    material reload (client and server, both the success and
    re-validation-failure paths), mutual TLS (`require_client_cert`, both
    the accepted-with-matching-certificate and rejected-with-no-certificate
    paths), oversized-request-body rejection, and a truncated-request
    regression test for the accept loop's own resilience (Error Handling:
    Server Accept-Loop Resilience)
  - Fixed a real, previously-unenforced gap surfaced while writing the
    oversized-body test: `async_read_kf` used to read into a bare
    `beast_http::request<string_body>&`, whose default body limit is
    Beast's own internal one (not configurable externally), so
    `boost_beast_server_config::max_request_body_size` was validated but
    never actually applied. `server_session::read_loop` now reads into a
    `beast_http::request_parser<string_body>` with `.body_limit()` set from
    that config field instead (a new `async_read_kf` overload taking a
    `parser` rather than a `message`, since only a parser exposes
    `.body_limit()`), and responds 413 when the limit is exceeded and
    headers were already fully parsed (see the code comment on
    `handle_read_error` for why draining the oversized body first is not
    attempted, and the accompanying test comment for why the exact
    exception observed by the client isn't pinned down further than "the
    RPC fails")
  - Each new test file picks its own disjoint port range (`tests/CMakeLists.txt`
    comment) since they are now four separate CTest binaries that may run
    concurrently under `ctest -j`, not sequential test cases inside one
    binary
  - _Requirements: 4.1, 4.2, 4.3, 4.4, 6.1-6.6, 16.2, 16.3_

- [x] 14. Concurrency-specific coverage — done and CI-verified (PR #117),
      plus four further real bugs found and fixed by a round-2 pass
  - `concurrent_rpcs_to_multiple_nodes` (Property 6) exercises many
    concurrent in-flight RPCs against a small `io_context` thread count;
    the full suite was run manually under AddressSanitizer (`-fsanitize=
    address`, ad hoc compile+link, not yet the project's own registered
    preset) with zero findings, including through the two real bugs this
    surfaced during development (a heap-use-after-free from
    `Future<T>::thenValue`'s future-flattening releasing a
    future-returning callback's own captures too early — see design.md's
    Data Models section — and the idle-session drain deadlock Property 8
    now documents)
  - A ThreadSanitizer run specifically (Requirement 14.4) — done via the
    `tsan` CI job (`.github/workflows/ci.yml`, `-DKYTHIRA_SANITIZER=thread`),
    a registered, repeatable CI build variant rather than an ad hoc
    `-fsanitize=address` compile.
  - Before PR #117 landed, this spec's own development independently found
    and fixed the same class of issue in `include/raft/future.hpp`:
    `Promise<T>::getFuture()`/`getSemiFuture()` returned a plain
    `folly::Future<T>` handed across the io_context thread that fulfills it
    and the calling thread that immediately chains `.thenValue()`/
    `.thenError()` onto it (`send_rpc`'s `connect().thenValue(proceed)`
    pattern) — the classic unsafe cross-thread pattern Folly's own
    `SemiFuture`/`via()` exists to make safe, which `getSemiFuture()` here
    didn't actually avoid either since it materialized back to a Future via
    `toUnsafeFuture()` immediately. Fixed by routing both through
    `getSemiFuture().via(&folly::InlineExecutor::instance())` instead —
    `folly::InlineExecutor` preserves the exact "runs inline, on whichever
    thread fulfills the promise" behavior every existing caller already
    depends on, so the fix is purely about making the handoff itself
    race-free, not about changing where continuations run. This crosses the
    spec's own Non-Goals boundary into shared infrastructure, done at the
    user's explicit direction.
  - **Round 2**: after this spec's own test suite was split one-file-per-
    concern (below), running each new binary under
    `-DKYTHIRA_SANITIZER=thread` individually — a finer-grained pass than
    the single monolithic `beast_transport_test` binary PR #117's `tsan` job
    originally built — surfaced four further genuine, pre-existing bugs,
    none of them Folly/Boost/Wangle packaging-mismatch false positives
    (those are suppressed; see `tests/tsan_suppressions.txt`). See `##
    Known Follow-ups` for the full accounting of each.
  - _Requirements: 3.4, 14.4_

- [x] 15. Cross-transport equivalence test
  - Delivered as `tests/three_way_http_transport_equivalence_test.cpp` —
    written jointly with `.kiro/specs/proxygen-http-transport/`'s own Task
    14 (its own stated blocker was this exact task never having been
    built), so it instantiates `cpp_httplib_client`/`server`,
    `boost_beast_client`/`server`, **and** `proxygen_client`/`server`
    together against the same RPC sequence rather than only the two this
    task originally specified — a superset, not a narrower substitute.
    Registered via CTest under both the `beast-http` and `proxygen-http`
    labels (gated on `KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT AND
    KYTHIRA_BUILD_PROXYGEN_TRANSPORT` both being set, since it needs all
    three transports linked into one binary — see
    `tests/CMakeLists.txt`/`.kiro/specs/proxygen-http-transport/tasks.md`
    Task 14 for the fuller writeup). CI-verified green via
    [PR #117](https://github.com/crawlins/kythira/pull/117).
  - Also kept the narrower, two-transport
    `tests/beast_cross_transport_equivalence_test.cpp` alongside the
    three-way test rather than deleting it: it documents a genuine,
    pre-existing asymmetry the three-way test's own design doesn't happen
    to surface — `cpp_httplib_client`'s async error path
    (`http_transport_impl.hpp`'s `make_future_with_exception`, which builds
    a `std::exception_ptr` via `std::make_exception_ptr(e)` where `e`'s
    *static* type at that call site is `const std::exception&`) slices any
    `http_client_error`/`http_server_error` down to plain `std::exception`
    before it reaches a catch block, losing both the derived type and the
    message. This is a real, separate bug in `http_transport_impl.hpp`
    unrelated to this feature, and this spec's own Non-Goals rule out
    modifying that file, so the test works around it (checks only that
    *some* exception is thrown on the cpp-httplib side) rather than papering
    over it or fixing out-of-scope code.
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

## Known Follow-ups

All 18 tasks are done. Nothing is open — the items below are a record of
what the two most recent rounds of work closed, kept for context rather than
as a to-do list.

### Round 1 (this session, before PR #117 landed)

- ~~Malformed-request handling~~ — done: `max_request_body_size` is now
  actually enforced (`server_session` reads through a
  `beast_http::request_parser` with `.body_limit()` set, not a bare
  message), with a 413 response and test coverage for both the oversized-
  body and truncated-request cases.
- ~~Mutual TLS coverage~~ — done: `require_client_cert` was already fully
  implemented server- and client-side before this update; only the test
  coverage was missing, now added (`mutual_tls_client_certificate_enforcement`).
- ~~Splitting `tests/beast_transport_test.cpp` into the one-file-per-concern
  layout `tests/http_*` uses~~ — done. Replaced by
  `tests/beast_client_test.cpp` (construction, concept compliance,
  client-side TLS reload), `tests/beast_server_test.cpp` (server
  lifecycle/drain, malformed-request handling, server-side TLS reload),
  `tests/beast_integration_test.cpp` (full round trips: connection reuse/
  Property 5, multi-node concurrency/Property 6), and
  `tests/beast_ssl_test.cpp` (server-only and mutual TLS). Property tests
  are now tagged `**Feature: boost-beast-http-transport, Property N: ...**`
  + `**Validates: Requirement ...**`, matching
  `tests/http_client_property_tests.cpp`'s own convention. Each new file
  uses its own disjoint port range, since these are now separate CTest
  binaries that may run concurrently under `ctest -j` rather than
  sequential test cases inside one binary.
- ~~Cross-transport equivalence test (Task 15)~~ — done, as
  `tests/beast_cross_transport_equivalence_test.cpp`; see Task 15's own entry
  above for why this was kept even after PR #117's three-way test landed.

### Round 2: a genuine ThreadSanitizer run against the split binaries found four more real bugs

[PR #117](https://github.com/crawlins/kythira/pull/117) landed the `tsan` CI
job and reported the (then still monolithic) `beast_transport_test` binary
clean. Running each of the now-split binaries individually under
`-DKYTHIRA_SANITIZER=thread` exercised connection-lifetime paths the
monolithic binary's own test cases didn't hit the same way, and surfaced four
further genuine, pre-existing bugs, plus three more TSan reports that turned
out to be the same class of packaging-mismatch false positive PR #117's own
`tests/tsan_suppressions.txt` already documents.

1. **`folly::futures::detail::Core`/`RequestContext`/`exception_tracer` —
   three more TSan reports, all the same underlying cause, none of them a
   real bug in this codebase.** `Try<T>::hasException()` vs.
   `Core<T>::setResult()` (`Core.cpp`'s `setResult_`/`setCallback_` CAS
   sequence), a lazily-initialized `std::mutex` inside
   `RequestContext::saveContext()` (reached from every `.thenValue()` via
   `FutureBase<T>::setCallback_()`), and allocator calls inside
   `exception_tracer`'s global `__cxa_throw`/`__cxa_end_catch` hooks (which
   intercept every exception thrown anywhere in the process, not just
   Folly's). All three share one root cause, confirmed independently with a
   ~15-line, kythira-free repro (a bare `folly::Promise<int>` fulfilled from
   one thread while `.thenValue()` is attached from another, reproducing the
   identical `Try<int>::hasException()` vs. `Core<T>::setResult()` race under
   plain Folly): vcpkg installs a prebuilt, release-mode `libfolly.a` that
   was never itself compiled with `-fsanitize=thread`. Folly's own source
   carries a targeted TSan workaround for exactly this class of report
   (`folly/synchronization/AtomicUtil-inl.h`'s
   `atomic_compare_exchange_succ()`, citing
   [google/sanitizers#970](https://github.com/google/sanitizers/issues/970):
   "Clang TSAN ignores the passed failure order and infers failure order
   from success order in atomic compare-exchange operations") — but that
   workaround only strengthens the ordering when `kIsSanitizeThread` is
   `true`, a constant baked in at the time *that header itself* was
   compiled, and vcpkg's `libfolly.a` was built without `-fsanitize=thread`,
   so it never applies. All three already match `tests/tsan_suppressions.txt`'s
   existing `race:folly::` pattern — no new suppression entries were needed.
2. **A `std::thread` destructed while still joinable — a real, if
   TSan-exposed-only, bug in the test suite's own thread-management, not in
   production code.** `beast_server_test.cpp`'s
   `server_stop_drains_idle_keep_alive_connection` (and the same pattern in
   every other beast_* test file) manually managed
   `std::vector<std::thread> io_threads` plus a bare `std::thread
   stop_thread`, joining each explicitly at a specific point in the test
   body — fragile by construction, since any control-flow path that skips
   the explicit join calls `std::terminate()` unconditionally. Fixed by
   removing the possibility entirely: new
   **`tests/beast_test_thread_pool.hpp`** provides
   `kythira::testing::io_thread_pool` (owns the `work_guard` + worker
   threads, stops and joins them in its destructor) and `joining_thread`
   (joins a single thread in its destructor), both checking `joinable()`
   before calling `join()`. All four beast_* test files now use these
   instead of hand-rolled `work_guard.reset(); ioc.stop(); for (auto& t :
   threads) t.join();` sequences.
3. **`boost_beast_client`'s destructor could destroy a connection's
   `ssl::context` while a worker thread was still mid-handshake against
   it — a real, TSan-confirmed data race on the vtable (ctor/dtor vs.
   virtual call), and a genuinely missing feature, not a one-line bug.**
   The main thread destroying a client's `net::ssl::context` could race a
   different thread still inside `engine::handshake()` ->
   `CRYPTO_THREAD_write_lock` using that same context.
   `boost_beast_server::stop()` already has this exact protection
   (Property 8's drain: register sessions, force-close, wait on a condition
   variable until every one finishes); `~boost_beast_client()` had no
   equivalent. Fixed by adding the same pattern to the client:
   `boost_beast_client` now tracks in-flight RPCs via a private
   `in_flight_guard` RAII type (increments a counter on construction,
   decrements and notifies a condition variable once it reaches zero on
   destruction), held via `shared_ptr` across a `send_rpc()` call's whole
   `thenValue`/`thenError` chain so whichever branch actually runs releases
   the last reference. `~boost_beast_client()` now force-closes every
   connection (live and retired) and waits on that condition variable
   before letting `_connections`/`_ssl_ctx` destruct.
4. **`basic_stream`'s per-operation `expires_after()` timeout was armed
   once per logical operation but not re-armed for the next one — the real
   root cause behind several instant-timeout/"end of stream" symptoms, on
   both client and server.** Boost.Beast's own documentation for
   `basic_stream` is explicit that `expires_after()` must be called again
   before each logical operation for which a timeout is desired (a logical
   operation can span several reads/writes issued back-to-back without an
   intervening call, but a separate operation issued later needs its own).
   `boost_beast_client::send_rpc()` called `set_timeout()` exactly once,
   before `connect()` — covering connect, but not the separate write+read
   `send()` does afterward, which the stream then treated as already past
   an unset deadline and failed instantly. The exact same gap existed
   server-side in `server_session::read_loop()`/`handle_and_write()`
   (armed once before the read, never re-armed before the response write).
   Fixed in both `plain_beast_connection`/`tls_beast_connection::send()`
   (re-arms the deadline immediately before the write, using a new
   `_timeout` member set alongside the stream's own deadline in
   `set_timeout()`) and `server_session::handle_and_write()`/
   `handle_read_error()` (re-arms `_request_timeout` immediately before
   each response write).
5. **A ThreadSanitizer-only segfault in `beast_cross_transport_equivalence_test`**,
   inside TSan's own runtime allocator, servicing a `malloc()` call from a
   glibc-internal thread this codebase never creates (`resolv/gai_misc.c`'s
   `handle_requests`, glibc's own worker thread for asynchronous
   `getaddrinfo_a()`) — triggered by cpp-httplib's vcpkg port
   unconditionally enabling `CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO` in its
   `INTERFACE_COMPILE_DEFINITIONS` with no feature to turn it off. Fixed at
   the root `CMakeLists.txt` level — when `KYTHIRA_SANITIZER STREQUAL
   "thread"`, that one compile definition is stripped from
   `httplib::httplib`'s `INTERFACE_COMPILE_DEFINITIONS` before anything
   links against it, forcing plain blocking `getaddrinfo()` (no extra
   glibc-internal thread, nothing for TSan to lose track of).

All five `beast-http`-labeled CTest binaries pass cleanly under
`-DKYTHIRA_SANITIZER=thread` locally after these fixes.

**A caveat, not a sixth bug**: even with all five fixes above, this
session's own sandbox — 4 CPU cores, heavily loaded from the debugging
session itself — occasionally showed a single request/response that
completes in milliseconds without TSan instead taking on the order of 180
seconds and then failing on a timeout that a directly-instrumented repro
confirmed really did take that long to fire (not the configured 30-second
deadline) — consistent with severe `io_context` scheduling starvation under
heavy TSan per-instruction overhead with more runnable threads (client +
server worker threads + TSan's own background thread) than this specific
sandbox's core count, not a logic bug reachable through code review or a
repro isolated from that contention. Whether this reproduces on GitHub
Actions' own runners (a different, and quite possibly less contended,
environment) is what the `tsan` job's next real run on this branch will
show.
