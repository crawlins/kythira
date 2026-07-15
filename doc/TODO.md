## TODO: Outstanding Tasks and Improvements

**Last Updated**: July 15, 2026

## Current Status

The project is **PRODUCTION READY** ✅ with 100% test pass rate.

- **All tests passing** (100%) — 385 tests registered in CTest
- **0 tests failing, 0 tests disabled**
- All specifications complete across all 8 feature areas (membership change now complete),
  plus peer-to-peer log replication/gossip catch-up, state machine examples, the
  stdexec future backend, the Folly-vs-stdexec performance benchmark suite, and
  RPC-internal mTLS for `ca_cluster_node`
- Build clean with no errors or warnings
- Coverage floor: 88.95% (non-decreasing ratchet, see `coverage_floor.txt`)

### What Changed (July 15, 2026)

- **`ca-cluster-node-ami` spec authored** (not yet implemented) — a
  Packer-based build pipeline for a golden, secret-free AMI with
  `ca_cluster_node` and its systemd unit pre-installed, resolving the
  "(baked into an AMI, e.g. via Packer)" placeholder already referenced by
  `docker/ca_cluster_node/README.md`'s Path 3,
  `docker/ca_cluster_node/ecs-task-definitions/README.md`'s automated
  alternative, and `tests/ca_cluster_node_real_ec2_test.cpp`'s
  `KYTHIRA_EC2_TEST_AMI` env var — all three assumed this AMI existed with
  no template or script actually producing it. The binary is extracted
  from `docker/ca_cluster_node/Dockerfile`'s existing `builder` stage
  (`docker create`/`cp`) rather than recompiled independently, so there
  remains exactly one place that knows how to build `ca_cluster_node` from
  source; the source AMI is Ubuntu 24.04 (matching the Dockerfile's
  runtime stage) rather than Amazon Linux 2023, to avoid a glibc ABI
  mismatch; no secrets are baked in (unseal passphrase, auth token, RPC
  bootstrap credentials stay injected per-instance at launch time,
  unchanged from today's manual systemd-install flow). CI wiring follows
  the existing `real-cloud-tests.yml` three-level toggle model and
  `scripts/ci-cloud-credentials/` bundle pattern, gated independently of
  the existing `ca-cluster-node` bundle since every AMI build leaves a
  billable AMI/snapshot behind. Full spec at
  `.kiro/specs/ca-cluster-node-ami/`; draft PR #44.

### What Changed (July 14, 2026)

- **ca-cluster-rpc-mtls complete — all 13 tasks**:
  `.kiro/specs/ca-cluster-rpc-mtls/` secures `ca_cluster_node`'s
  Raft-internal RPC channel (previously plain, unauthenticated TCP via
  `tcp_rpc_client`/`tcp_rpc_server`) with mutual TLS, via a two-phase
  bootstrap: peers first mutually authenticate using a small, static,
  operator-provisioned credential (distributed the same way as the
  existing unseal passphrase), then, once the CA root exists, each node
  self-service-acquires its own CA-issued peer certificate and cuts over
  automatically — no operator action beyond initial provisioning.
  `tcp_rpc.hpp` itself is untouched; the new transport
  (`include/raft/tls_tcp_rpc.hpp`, `tls_tcp_rpc_client`/
  `tls_tcp_rpc_server`) is a sibling satisfying the same
  `network_client`/`network_server` concepts, wired in via a second
  `ca_cluster_raft_types` alternative selected by a runtime check in
  `cmd/ca_cluster_node/main.cpp` (template-instantiated once per Types, no
  duplicated ~500-line node-construction body). Adds `ca_state_machine`'s
  `record_rpc_tls_ready` command/set, `--rpc-tls-cert`/`--rpc-tls-key`/
  `--rpc-timeout-ms` CLI flags, and updated `docker/ca_cluster_node/`
  deployment packaging (systemd unit, env example, ECS task definitions)
  for the bootstrap credential.
  - **Real bugs found and fixed during multi-process integration testing**
    (none of which were caught by unit/2-node-in-process testing alone —
    all three only manifested under a real 3-process cluster with actual
    TLS handshake latency):
    1. `tls_tcp_rpc_client` originally rebuilt its `SSL_CTX*` from scratch
       — including re-reading and re-parsing the identity cert/key files
       from disk — on every single RPC call. At this project's default
       50ms heartbeat cadence, that's disk I/O plus a full asymmetric-key
       setup on Raft's own liveness-timer critical path; under real host
       contention it reliably drove elections into the hundreds of terms.
       Fixed by caching one long-lived `SSL_CTX*` per client, mutated only
       by `reload_identity()`, mirroring the server side.
    2. The accepted server-side socket had no `SO_RCVTIMEO`/`SO_SNDTIMEO`
       at all (unlike the client's own `connect_to()`) — a client that
       gave up mid-handshake left the server's per-connection thread
       blocked forever, leaking one thread and one fd per stall and
       compounding under load.
    3. A node was switching *what it presented* (to its own newly-acquired
       CA-issued certificate) at the same moment it acquired that
       certificate — but a peer that hadn't independently reached the
       CA root yet was still evaluating incoming connections under
       `pinned_fingerprint` alone and would reject the now-unrecognized
       cert outright. Fixed by decoupling "widen what this node accepts"
       (triggered the moment the CA root is known to exist, via
       `maybe_widen_rpc_trust_policy()`) from "switch what this node
       presents" (only after acquiring its own certificate) — every peer
       observing the same replicated root widens before any single peer
       can finish acquiring and start presenting.
    4. `node<Types>::read_state()`/`submit_command()` have no built-in
       leader-forwarding — a follower's call fails immediately with "not
       leader" rather than reaching the actual leader. The original
       design (mirrored from this spec's own design.md sketch) called
       both unconditionally from every node, which works for the leader
       but never for followers. Fixed by adding a leader/follower split
       (`fetch_root_cert_pem()`, matching the CSR-signing path's existing
       split) that uses the client-facing HTTP API — a transport
       completely unaffected by RPC-TLS trust state — for followers, and
       piggybacking a follower's own `record_rpc_tls_ready` submission
       onto its CSR-signing request so the leader, which alone can
       actually call `submit_command()` successfully, submits it on the
       follower's behalf.
- **ca-cluster-rpc-mtls: CI-only deadlock found and fixed post-merge**:
  the PR above passed every local/sandbox test run, merged, and then
  failed `ca_cluster_node_rpc_tls_test` reliably (3/3 retries, both
  compilers) on GitHub Actions' shared runners specifically. Root cause:
  the leader's own `fetch_root_cert_pem()` is an in-process read, so it
  could widen-then-acquire-then-switch its presented RPC identity within
  the same maintenance tick the CA root committed on — before any
  follower's maintenance thread had a real chance to widen its own trust
  policy (which needs an HTTP round trip to the leader's `/v1/root-ca`,
  itself gated on a quorum-confirmed `read_state()`). Once the leader
  switched, every follower started rejecting its traffic, which broke the
  very read-index heartbeats `/v1/root-ca` needed to keep answering — a
  genuine circular deadlock, not a race that resolves given more time.
  Fixed with a 3-second grace period (`k_identity_acquire_grace`) between
  a node first observing the CA root and that node switching its
  presented identity, giving every already-alive peer's widen step a real
  window while RPC is still universally on the old, mutually-trusted
  bootstrap credential. Verified locally under `taskset`-constrained CPU
  plus background load in addition to the unconstrained runs, then
  confirmed clean on CI. Landed as a separate commit
  (`fix(ca-cluster-node): delay RPC-TLS identity switch until peers can
  widen`) on the same PR.
- **ca-cluster-rpc-mtls: coverage-ratchet CMake gating bug found and
  fixed**: `tls_tcp_rpc_unit_test`/`tls_tcp_rpc_integration_test` were
  accidentally registered inside `tests/CMakeLists.txt`'s
  `if(TARGET ca_service)` block, which — unlike their actual dependency
  (`certificate_authority` only) — is unavailable under coverage builds
  (`ENABLE_COVERAGE` disables `cmd/ca_service`/`cmd/ca_cluster_node`
  entirely), so neither test binary, and none of `tls_tcp_rpc.hpp`'s
  coverage, was ever measured. Moved both out to be gated only by the
  enclosing `if(TARGET certificate_authority)`. Once actually measured,
  `reload_identity()`/`reload_trust_policy()`/`is_running()` (needed for
  live cutover, but not exercised by the round-trip-shaped tests written
  first) and the new `record_rpc_tls_ready` state-machine command turned
  up as genuine, unrelated-to-this-fix coverage gaps — closed with a
  dedicated reload test, an append_entries/install_snapshot round trip
  (previously only request_vote was exercised), and a
  `record_rpc_tls_ready`/`rpc_tls_ready_node_ids()` unit test. Coverage
  floor raised 88.92% → 88.95%.
- **`ca-cluster-rpc-mtls-real-aws` spec authored** (not yet
  implemented) — see Certificate Management, below, for the summary;
  full spec at `.kiro/specs/ca-cluster-rpc-mtls-real-aws/`.

### What Changed (July 13, 2026, later)

- **future-backend-performance-benchmark complete — all 23 tasks**:
  `.kiro/specs/future-backend-performance-benchmark/` adds a benchmark
  suite comparing `kythira::Future<T>` (Folly) against
  `kythira::stdexec_backend::Future<T>` (stdexec) across a fixed catalog of
  9 scenarios (creation/resolution for 3 payload shapes, same-/cross-thread
  promise fulfillment, `thenValue` chains at 3 depths, `thenError`,
  `via(scheduler)`, `collectAll` at 3 widths, `collectAny`, `delay`/`within`
  overhead). Every scenario is one function template
  (`examples/future-backend-benchmark/benchmark_harness.hpp`) instantiated
  once per backend via a `folly_backend_traits`/`stdexec_backend_traits`
  pair, so there is exactly one implementation per scenario — the two
  backends' numbers can never silently drift apart by comparing two
  different operations. Adds `tests/future_backend_benchmark_test.cpp`
  (CTest-registered, `LABELS "performance;benchmark;future-backend"`,
  hardware-independent sanity floors only — no test compares one backend's
  result against the other's) and
  `examples/future-backend-benchmark/benchmark_report.cpp` (a standalone,
  developer-run comparison report writing timestamped CSV/JSON to
  `test_results/`). Builds and runs Folly-only when `stdexec_FOUND` is
  false, with the `stdexec` column and delta omitted from the report rather
  than printed as zeroed placeholders. Does not change
  `KYTHIRA_DEFAULT_FUTURE_BACKEND` or recommend a default; see
  `doc/future_backend_performance_comparison.md` for methodology, the full
  scenario catalog, known structural asymmetries (the cross-thread
  scenario's per-iteration `std::thread` spawn cost, the GCC
  `-fno-strict-aliasing` mitigation already in place for `stdexec` targets),
  and reference numbers from a real run.

### What Changed (July 13, 2026)

- **stdexec future backend complete — all 52/52 tasks**: `.kiro/specs/stdexec-future-backend/`
  Phases 3–6 (Tasks 14–35) implemented — the full continuation/
  transformation/scheduling surface on `stdexec_backend::Future<T>`
  (`thenValue`/`thenTry`/`thenError`/`ensure`/`via`/`delay`/`within`),
  `FutureFactory`/`FutureCollector` (`collectAll`/`collectAny`/
  `collectAnyWithoutException`/`collectN`), `scheduler_executor_shim`,
  backend selection (`KYTHIRA_DEFAULT_FUTURE_BACKEND` CMake option,
  `include/raft/future_default.hpp`), and a full test suite (19 targets,
  `ctest -L stdexec`) including cross-backend fidelity tests comparing
  Folly and stdexec behavior directly and compile-time backend
  non-interference checks. Phases 0–2 (the concept regenericization and
  core `Try`/`single_shot_channel`/`Promise`/`Future` primitives) had
  already landed on this branch. Also closed the 4 sub-tasks left
  unchecked from that earlier Phase 0–2 work: Property 5 (Optional
  Dependency Isolation, `scripts/verify-optional-dependency-isolation.sh`
  + the `verify-optional-dependency-isolation` CMake target, using
  `-DCMAKE_DISABLE_FIND_PACKAGE_stdexec=ON` rather than mutating
  `vcpkg_installed/`), Property 2 (Concept-Layer Folly Independence,
  `tests/concepts_future_folly_independence_test.cpp` — a raw
  `add_executable` that deliberately doesn't link `network_simulator`, so
  a regression pulling Folly into `concepts/future.hpp` would fail to
  compile rather than pass silently), Property 1 (Concept Regenericization
  Preserves Folly Compliance,
  `tests/folly_backend_concept_regenericization_property_test.cpp`), and
  Property 3 (Unit Type Equivalence,
  `tests/unit_type_equivalence_property_test.cpp`).
  - **`within()`'s implementation changed mid-spec**: the original design
    used `exec::when_any` to race the original sender against a timeout,
    but `when_any`'s cancel-the-loser behavior depends on a stop token
    reaching the losing branch through this file's `any_sender_t<T>` type
    erasure — and `any_receiver_t<T>` is declared with the default empty
    query-forwarding list, so that stop token never arrives, leaving
    `when_any` waiting forever for a `single_shot_channel`-backed loser
    that can never acknowledge a stop request it never received.
    Reimplemented using the same "race to fulfill a shared channel,
    loser's result silently discarded" pattern already used by
    `FutureCollector::collectAny`, launched via `exec::start_detached`
    rather than `exec::async_scope::spawn()` (the latter's destructor
    asserts every spawned operation has already completed, which a
    losing branch — e.g. a promise the caller never fulfills — can
    violate for an unbounded time).
  - **Real GCC 13 miscompilation found and fixed**: at `-O2`/`-O3`, GCC 13
    miscompiles `exec::any_sender`'s small-buffer-optimized move
    constructor — moving one `any_sender` holding a small payload (e.g.
    `stdexec::just(int)`, the common case) into another `any_sender` of
    the same type corrupts the heap. The corruption doesn't crash
    immediately; it crashes on a later, unrelated allocation, which made
    the first several repro attempts misleading (a property test calling
    `FutureFactory::makeFuture(v).get()` in a 200-iteration loop always
    crashed on exactly the *second* iteration). Fixed with
    `-fno-strict-aliasing` for GCC builds only (`CMakeLists.txt`);
    `clang++-18` is unaffected at every optimization level. Full
    diagnosis in `.kiro/specs/stdexec-future-backend/spike-notes.md`'s
    "Phase 3 findings" section.

### What Changed (July 11–12, 2026)

- **Peer-to-peer log replication and TCP gossip transport implemented**: lands
  both `.kiro/specs/peer2peer-log-replication/` (the abstract catch-up
  mechanism) and `.kiro/specs/peer2peer-gossip-transport/` (a real network
  transport for it), since the gossip spec couldn't compile or be exercised
  without its foundation and neither had been started. Previously, log
  replication in `node<Types>` was a strict star topology — only the leader
  could supply missing entries, so its own CPU/bandwidth capped how fast a
  cluster converged when many members fell behind at once (rolling restart,
  healed partition, bursty joins).
  - New opt-in `peer2peer_replicator` concept
    (`include/raft/peer2peer_replication.hpp`), mirroring `peer_discovery`'s
    shape: a `no_op_peer2peer_replicator` default guarantees zero behavioral
    change for any `Types` bundle that doesn't opt in, and
    `static_peer2peer_replicator` is an in-memory reference/test
    implementation.
  - New `fetch_log_entries_request`/`response` RPC pair (`types.hpp`) plus
    optional `network_client_with_log_fetch`/`network_server_with_log_fetch`
    extension concepts (`network.hpp`), wired into `json_serializer.hpp` and
    `simulator_network.hpp` exactly like the existing `ClusterJoin`/
    `ClusterLeave` optional extensions.
  - `raft.hpp`: extracted `append_entries_with_consistency_check()` from
    `handle_append_entries()`'s Rules 3–5 so the peer-to-peer fetch path
    reuses the exact same conflict/truncation guarantees as leader-driven
    replication — a bad or stale source peer can only cause wasted local
    work, never a committed entry being lost or altered.
  - A replicator's peer set now tracks `cluster_members()` automatically via
    `sync_peer2peer_membership()`, wired into every `_configuration`
    mutation site (including `set_cluster_configuration()`, which the
    peer2peer-log-replication spec's own design doc had missed because it
    mutates `_configuration`'s fields in place rather than via a single
    assignment) — never separately configured.
  - `maybe_gossip_progress()`/`maybe_catch_up_from_peer()` piggyback on
    `check_election_timeout()` (called unconditionally for every node state
    by every existing binary's external timer loop) since this codebase has
    no dedicated maintenance-thread tick.
  - **`tcp_gossip_peer2peer_replicator`** (`include/raft/tcp_gossip_transport.hpp`,
    583 lines): a real anti-entropy gossip implementation (randomized
    push-pull digest exchange, Cassandra/Dynamo-style — not SWIM, since
    Raft's own election timeouts already cover liveness detection),
    self-contained TCP listener plus background gossip thread, entirely
    independent of whatever `network_client_type`/`network_server_type` the
    owning node uses for Raft RPCs. Starts its background threads lazily via
    an explicit `start()`/`stop()` pair (detected structurally via
    `if constexpr (requires {...})`) rather than in the constructor, since
    `node_config<Types>` holds `peer2peer_replicator_type` by value and
    `node<Types>`'s constructor moves it once — moving an object after its
    background threads have captured `this` would dangle.
  - 6 new test files (concept/no-op/static unit tests, an end-to-end
    property suite proving a joining node converges via a peer while
    excluded from ever reaching the leader, a `remove_server()`-revokes-
    eligibility test, a no-op-vs-undeclared parity test, pure-logic
    merge/prune/wire unit tests for the gossip transport, a real-TCP
    single-process integration test, and a mixed-transport property suite —
    real gossip sockets, simulated Raft RPCs — covering catch-up
    convergence, freshness expiry, and membership-removal). Full existing
    regression suite verified green alongside them.
- **State machine examples completed**: `replicated_log_state_machine` and
  `distributed_lock_state_machine` brought up to parity with
  `counter`/`register` (test targets, `CMakeLists.txt` wiring). Found and
  fixed a determinism defect in `distributed_lock_state_machine::apply()`:
  it called `std::chrono::steady_clock::now()` to compute lock expiry, which
  is non-deterministic across Raft replicas (clock skew, GC pauses,
  different machines) and violates the `state_machine` concept's requirement
  that every replica reach identical state from the same command at the same
  log index. Replaced wall-clock expiry with log-index-based expiry
  (`expiry_index = acquire_index + timeout_entries`), using the `index`
  argument `apply()` already receives — the one value every replica is
  guaranteed to agree on. The `ACQUIRE` command's third argument is renamed
  `timeout_ms` → `timeout_entries` to match; `include/raft/examples/README.md`
  and this file both updated accordingly. New
  `tests/replicated_log_state_machine_test.cpp` and
  `tests/distributed_lock_state_machine_test.cpp` mirror the existing
  counter/register test structure, each with a `static_assert` against
  `kythira::state_machine` to catch future signature drift at compile time;
  distributed lock's suite includes a dedicated determinism test applying
  the same command sequence — including an expiry and re-acquisition — to
  two independent instances and asserting byte-identical `get_state()` after
  every command.
- **Coverage hook no longer hangs on debuginfod network stalls**: the
  pre-commit coverage-ratchet step intermittently took several minutes to
  what looked like an indefinite hang at "[coverage] Measuring ...". Root
  cause: `llvm-profdata-18`/`llvm-cov-18` on this system are built with
  debuginfod (libcurl) support, and `DEBUGINFOD_URLS` is set globally in the
  environment; left alone, both tools attempt a network round-trip per test
  binary to fetch debug info they already have embedded locally, and in this
  network-restricted environment those connections stall (silently dropped,
  not refused) instead of failing fast. Confirmed directly — clearing
  `DEBUGINFOD_URLS` took the coverage report over all 306 test binaries from
  66 minutes wall clock down to 1.9 seconds. Fix: explicitly clear
  `DEBUGINFOD_URLS` for both the `llvm-profdata` merge and `llvm-cov` report
  invocations. An earlier, incorrect diagnosis had attributed this to LLVM's
  internal thread pool and worked around it with `--num-threads=1`; that
  workaround is superseded by this fix.

### What Changed (July 9–10, 2026)

- **Membership change (joint consensus) spec verified complete**: all 20 tasks
  in `.kiro/specs/membership-change/` were found already substantially
  implemented in the codebase — this spec's tracking document had simply
  never been updated to reflect that. Verified every task against
  `requirements.md` by direct code reading; the one genuine gap
  (`tests/node_recovery_unit_test.cpp`, task 20) was added, covering
  no-persisted-state, term+voted_for-only, snapshot-only, and
  snapshot-plus-trailing-log-entries restart scenarios (including a
  configuration log entry correctly overriding the snapshot's own
  configuration per Requirement 8.3).
- **CI flakiness diagnosed and fixed**: pulled JUnit artifacts from 8 recent
  CI runs to find the actual failure signatures rather than guessing. Three
  independent causes accounted for nearly all flaky `build-and-test`/coverage
  failures:
  - `ca_cluster_node_test` (6/8 sampled failures) — a real 3-node Raft
    cluster brought up as subprocesses, flaking under CPU contention from
    `ctest -j$(nproc)` on 4-vCPU runners. Fixed with `--repeat until-pass:3`
    on the `build-and-test` job's ctest invocation (already present on the
    coverage job, but missing here) and `PROCESSORS 4` on the test itself so
    ctest's scheduler stops co-scheduling other tests alongside it.
  - Coverage floor comparison was byte-exact against a floor set on the
    authoring dev's machine; CI's own measurement of the same tree can land
    a few tenths of a point lower from run-to-run counter/scheduling noise
    (observed: 88.10% vs. an 88.16% floor). Added a 0.50pp tolerance band to
    CI's enforcement check only — the local ratchet, which is what actually
    raises the floor, stays exact.
  - Coverage job intermittently failed at link time with "No space left on
    device" — coap-transport-security's added test binaries ate back into
    the headroom reclaimed for certificate-authority. Widened the
    disk-reclaim step (JVM, Az CLI, PowerShell, GHC's second install path,
    the runner's swapfile, apt's package cache).

### What Changed (July 7–8, 2026)

- **Certificate Authority framework complete**: all 35 tasks of
  `.kiro/specs/certificate-authority/` implemented, built, and tested. In-process
  `certificate_authority` (root CA generation, leaf issuance, revocation/CRL,
  `from_existing()` round-trip), `temp_cert_files` RAII helper, and
  `ca_service` CLI (`cmd/ca_service/`) for both oneshot Docker/Podman-volume
  provisioning and a long-running `--serve` HTTP API mode
  (`local`/`aws-acm-pca` providers, bearer-token auth, `/v1/certificates`,
  `/v1/certificates/renew`, `/v1/certificates/revoke`, `/v1/crl`,
  `/v1/root-ca`).
- **`aws_acm_pca_provider`**: `certificate_provider` implementation backed by
  AWS Certificate Manager Private CA; unit/LocalStack/real-AWS test tiers
  following the project's existing always-compiled-but-runtime-skipped
  convention.
- **TLS hot-reload**: `reload_tls_material()`/`enable_auto_reload()` for both
  `cpp_httplib_server`/`cpp_httplib_client` and `coap_server`/`coap_client`,
  plus `ca_test_fixture::renew()` and `temp_cert_files::replace_atomically()`
  (atomic write-tmp-then-rename) so certificate rotation never serves a
  half-written file.
- **`ca_cluster_node`** (`cmd/ca_cluster_node/`): a Raft-replicated CA —
  `ca_state_machine` records bootstrap/issuance/revocation as a deterministic
  replicated ledger; the leader reconstructs a `certificate_authority` via
  `from_existing()` and replays the ledger on every election; a `noop`
  command is submitted immediately on election so previous-term entries
  commit retroactively (Raft §5.4.2/Figure 8). Multi-node test coverage
  drives real 3-process clusters over subprocesses, including leader
  failover and restarted-follower recovery. Packaged for 3-AZ AWS deployment
  (systemd unit, ECS task definitions, `docker/ca_cluster_node/`).
- **ACME support (RFC 8555)**: `acme_test_server` (self-contained mock CA,
  `tests/acme_test_server.hpp`) and `acme_certificate_provider`
  (`include/raft/acme_certificate_provider.hpp`) — full JWS-signed order
  lifecycle, http-01 and dns-01 (RFC 2136 UPDATE) challenges, RFC 8738
  `"ip"`-typed identifiers, per-identifier challenge-type dispatch
  (`acme_identifier::classify()`/`challenge_for()` — IP identifiers always
  use http-01 regardless of configured challenge type), and `.local` (mDNS)
  challenge validation via ordinary `getaddrinfo()` with a distinguishable
  `mdnsResolverUnavailable` error when the validating host has no mDNS
  resolver configured (nsswitch.conf-based capability probe with a
  test-only override).
- **Fingerprint-pinned bootstrap** (`include/raft/ca_bootstrap_client.hpp`):
  `fetch_trusted_root()` lets a fresh instance, given only an out-of-band
  SHA-256 root fingerprint and bearer token, establish first-contact trust
  in a `ca_service`/`ca_cluster_node` TLS listener without any prior
  certificate chain to verify against — `--print-root-fingerprint` prints
  the operator-distributable fingerprint.
- **Two pre-existing `raft.hpp` bugs found and fixed** while wiring
  `ca_cluster_node`'s multi-node tests (not part of the certificate-authority
  spec's own scope, but blocking correct multi-node behavior):
  - `read_state()`'s quorum check used
    `raft_future_collector<T>::collect_majority()`, which computed
    `(followers.size()/2)+1` — wrongly requiring acknowledgment from *every*
    follower in a 3-node cluster instead of a majority *including* the
    leader's implicit self-vote. This made linearizable reads unavailable
    with exactly one node down, in any cluster size. Fixed with
    `collect_all_with_timeout()` plus an explicit
    `required_follower_acks = (heartbeat_futures.size() + 1) / 2`.
  - After a restart/election, previously-persisted log entries from a prior
    term were never retroactively committed, stalling `read_state()`/state
    application indefinitely. Fixed via the standard Raft no-op-on-election
    technique: `ca_cluster_node` submits a `ca_command_type::noop` command
    immediately upon becoming leader.
  - Added `node<Types>::known_leader()` public accessor needed for
    `ca_cluster_node`'s redirect-to-leader HTTP routes.
- **httplib gotcha documented**: `SSLClient::enable_server_certificate_verification(false)`
  disables cpp-httplib's *entire* verification block, including any custom
  `server_certificate_verifier_` callback — not just the default chain check.
  `ca_bootstrap_client.hpp` deliberately never calls it, relying solely on
  the callback's explicit `CertificateAccepted`/`CertificateRejected` return.
- **`quorum_management_test`/`docker_quorum_manager_test` linker fix**:
  both were missing the `Boost::context`/`libboost_context.a` link already
  required by every other Folly-linking test target (undefined reference to
  `boost::context::detail::make_fcontext` when actually exercising Folly
  fibers) — pre-existing, unrelated to this spec, fixed opportunistically
  while verifying a full build.

### What Changed (June 19–22, 2026)

- **`rfc2136_dns_sd_discovery` implemented** (`include/raft/rfc2136_dns_sd_discovery.hpp`,
  500 lines): DNS-SD peer discovery over unicast DNS via RFC 2136 dynamic update. Registers
  PTR, SRV, and TXT records per node under a configured service domain. A background fresher
  thread renews the TXT record's `fresh_until=<epoch>` field every
  `freshness_interval / 2` so that peers from crashed nodes (whose destructor never
  runs) are automatically filtered out by `find_peers`. `register_node` returns a
  `folly::Future<void>` that resolves once the UPDATE is acknowledged. Destructor sends
  a DELETE UPDATE best-effort. Fault injection points: `raft/dns/rfc2136/dns_sd/update`
  (throws) and `.../update/noop` (silent pass-through).

- **Unit and chaos tests for `rfc2136_dns_sd_discovery`** added to
  `tests/dns_peer_discovery_unit_test.cpp` (now 21 total cases across 3 suites) and
  `tests/chaos/dns_peer_discovery_chaos_test.cpp` (now 17 total cases across 3 suites).
  The `rfc2136_dns_sd_suite` covers: register resolves future, find_peers returns peers,
  deregister on dtor, freshness filtering. The `rfc2136_dns_sd_chaos_suite` covers:
  register throws on fault, dtor silent when deregister faulted, find_peers returns
  empty on fault, noop fault lets register succeed.

- **BIND9 Docker image** (`docker/bind9/Dockerfile`): multi-stage Ubuntu 24.04 build
  of BIND9 with RFC 2136 Dynamic Update enabled on a private `example.local.` zone;
  includes `dig` for healthchecks.

- **DNS discovery Docker scenario test** (`tests/docker_chaos/dns_discovery_test.cpp`,
  3 cases): `all_nodes_healthy`, `all_nodes_discover_peers`,
  `stopped_node_absent_after_deregister`. Runs via `docker-dns-discovery-tests` CMake
  target using `docker/dns-discovery-compose.yml` (BIND9 + 3 `dns_discovery_node`
  containers).

- **DNS-SD discovery Docker scenario test** (`tests/docker_chaos/dns_sd_discovery_test.cpp`,
  3 cases): `all_nodes_healthy`, `all_nodes_discover_peers`,
  `dead_node_absent_after_freshness_expiry`. Runs via `docker-dns-sd-discovery-tests`
  CMake target using `docker/dns-sd-discovery-compose.yml` (BIND9 + 3
  `dns_sd_discovery_node` containers). The `dead_node_absent_after_freshness_expiry`
  case kills node1 with SIGKILL and waits 25 s for the 20 s freshness interval to
  expire, then verifies the dead node is no longer reported by the surviving nodes.

- **`poco_peer_discovery` Docker scenario test** (`tests/docker_chaos/poco_discovery_test.cpp`,
  3 cases) added via `docker-poco-discovery-tests` CMake target; runs against a
  `docker/poco-discovery-compose.yml` cluster.

- **Podman support in Docker test harness**: `tests/docker_chaos/os_faults.hpp` now
  provides `container_runtime()` (reads `$KYTHIRA_CONTAINER_RUNTIME`, default
  `"docker"`) and `compose_prefix()` (reads `$KYTHIRA_COMPOSE_COMMAND`, defaults to
  `[runtime, "compose"]`); all command-vector builders use these instead of the
  hardcoded `"docker"` string. `tests/docker_chaos/CMakeLists.txt` auto-detects
  `docker` then `podman` via `find_program`, exposes `CONTAINER_RUNTIME` and
  `COMPOSE_COMMAND` CMake cache variables, and forwards both as env vars into every
  scenario-test invocation.

- **Rootless Podman compatibility**: `docker/dns-discovery-compose.yml` and
  `docker/dns-sd-discovery-compose.yml` no longer assign static IPs to BIND9
  (`ipv4_address` was silently ignored by rootless Podman). `DNS_SERVER` is now the
  compose service name (`"dns-test-bind9"`, `"dns-sd-test-bind9"`). Both node
  binaries (`cmd/dns_discovery_node`, `cmd/dns_sd_discovery_node`) resolve the
  service name to an IP via `getaddrinfo(AF_INET)` before handing it to ldns, which
  only accepts IP literals.

- **Coverage build fixes**: `cmd/` subdirectories excluded from CMake build when
  `ENABLE_COVERAGE=ON` to prevent `GcovrMergeAssertionError` (header-only classes
  compiled in both test TUs with `FIU_ENABLE` and node binaries without it produced
  conflicting gcov line-number metadata). `tests/docker_chaos/` excluded from gcovr
  (binaries compiled but never run by ctest). Coverage floor raised: 79.9% → 80.3%.

- **CLAUDE.md** created at repo root with steering directives: Conventional Commits
  format required for all commit messages; commit bodies must be detailed summaries
  (motivation, trade-offs, root cause, sub-changes); all container-based tests must
  work with both Docker and rootless Podman (no static IPs, no hardcoded `"docker"`,
  no root-only networking).

### What Changed (June 18, 2026)

- **DNS peer discovery tests complete**: comprehensive unit tests (14 cases,
  `tests/dns_peer_discovery_unit_test.cpp`) and chaos tests (12 cases,
  `tests/chaos/dns_peer_discovery_chaos_test.cpp`) for `rfc1035_peer_discovery`
  and `rfc2136_ldns_discovery`; both test targets are guarded by `LIBLDNS_FOUND`
  and registered in CTest with appropriate labels (`unit;dns;peer_discovery`,
  `chaos;dns;peer_discovery`).
- **Fault injection points added** to `rfc1035_peer_discovery.hpp`
  (`"raft/dns/rfc1035/find_peers/fail"`, `.../inject_ipv4`, `.../inject_mixed`)
  and `rfc2136_ldns_discovery.hpp` (`"raft/dns/rfc2136/send_update"`,
  `.../noop`) — all compile to no-ops without `FIU_ENABLE`.
- **`register_node` bug fixed** in `rfc2136_ldns_discovery`: `_self_address` is
  now set *after* a successful `send_update()` call, not before — previously a
  failed registration left `_self_address` set, causing the destructor to attempt
  deregistration of an address that was never successfully registered.
- **Node bootstrap spec fully complete**: all 20 tasks done including CoAP
  multicast adaptor (`coap_multicast_peer_discovery`), RFC 1035 query class,
  RFC 2136 dynamic-DNS class, 6 property tests, and DNS unit/chaos tests.

### What Changed (June 12, 2026)

- **Docker chaos testing complete**: real multi-node cluster in Docker containers with
  OS-level fault injection; `chaos_node` binary (`cmd/chaos_node/`) with TCP RPC
  (`tcp_rpc.hpp`), file persistence (`file_persistence.hpp`), HTTP control plane, and
  libfiu TCP remote-control server (`fiu_remote.hpp`); multi-stage `Dockerfile` +
  `docker-compose.yml` (3 nodes, `NET_ADMIN` for iptables); Python harness
  (`tests/docker_chaos/`) with `ChaosCluster`, `ChaosNode`, network partition helpers,
  raw-socket `fault_control.py`, and 3 test files (AZ partition, persistence faults,
  combined safety assertions); `docker-chaos-image` and `docker-chaos-tests` CMake
  targets; spec at `.kiro/specs/docker-chaos/`.

- **libfiu integration complete**: fault injection chaos testing implemented across 5 phases
  (21 tasks); `include/raft/fault_injection.hpp` guard header; `fiu_do_on()` calls in
  `persistence.hpp`, `simulator_network.hpp`, `test_state_machine.hpp`; `debug_state()`
  accessor on `kythira::node`; RAII `fault_profiles.hpp`; `safety_assertions.hpp` helpers;
  `tests/chaos/` with smoke, profile-verification, and 8 safety/liveness property tests;
  `chaos-tests` CMake target; `README.md` "Chaos Testing" section; `DEPENDENCIES.md` updated.

### What Changed (June 11, 2026)

- **clang-tidy zero findings confirmed**: all 291 compilation units clean after
  fixing narrowing conversions, enum sizes, branch-clone, else-after-return,
  use-after-move suppressions, and compiler diagnostic errors in `future.hpp`
  and `coap_transport_impl.hpp`.
- **libfiu integration spec created**: fault injection chaos testing design at
  `.kiro/specs/libfiu-integration/`; macro approach (`fiu_do_on` in production
  sources, compiles to no-op without `FIU_ENABLE`); 21 tasks across 5 phases.
- **Membership change spec created**: joint consensus (Raft §6) implementation
  design at `.kiro/specs/membership-change/`; 20 tasks across 7 phases covering
  log entry type discriminant, leader log append, joint quorum, apply path,
  follower update, property tests, and node recovery on restart.
- **Node bootstrap spec created**: `peer_finder` concept + `ClusterJoin` RPC for
  fresh-node cluster join at `.kiro/specs/node-bootstrap/`; 15 tasks across 7
  phases; `no_op_peer_finder` default preserves all existing behaviour.

### What Changed (June 10, 2026)

- **clang-tidy integration**: `.clang-tidy` config with `WarningsAsErrors: "*"`;
  CMake `static-analysis` and `static-analysis-fix` targets (parallel via
  `run-clang-tidy`, sequential fallback); pre-commit hook step (opt-in with
  `TIDY_CHECK=1`, skip with `SKIP_TIDY_CHECK=1`); zero findings across all 291
  compilation units; spec at `.kiro/specs/clang-tidy/`.
- **clang-format integration**: `.clang-format` config (Google base, 4-space
  indent, 100-column limit); CMake `format` and `format-check` targets;
  pre-commit hook now checks staged files before the coverage ratchet;
  `SKIP_FORMAT_CHECK=1` escape hatch; 349 source files reformatted in a
  style-only commit; spec at `.kiro/specs/clang-format/`.

### What Changed (June 9, 2026)

- **Code coverage infrastructure**: `ENABLE_COVERAGE` CMake option + gcovr targets
  (`coverage`, `coverage-html`, `coverage-reset`); `coverage_floor.txt` baseline
  at 84.8%; pre-commit ratchet hook enforces non-decreasing coverage.
- **Membership API refactored**: `handle_node_removal(node_id)` replaced by
  `handle_cluster_membership_change(old_config, new_config)` — provides full
  context for both add and remove operations; notification fires after commit.
- **Command type encoding fixed**: `test_key_value_state_machine` enum aligned
  to `{get=0, put=1, del=2}` matching the command generator and inline test
  state machines. Fixes `state_machine_determinism_property_test`.
- **Trailing whitespace removed** from all 402 source files.

---

## Completed Specifications (All 8/8 Complete)

| Spec | Tasks | Status |
|------|-------|--------|
| Raft Consensus | 287/287 | ✅ Complete — includes Phase 5 multi-node testing (700–731) |
| HTTP Transport | 17/17 | ✅ Complete — A+ SSL/TLS, 931K+ ops/sec |
| CoAP Transport | 26/26 | ✅ Complete — DTLS, block transfer, 30K+ ops/sec |
| Folly Concept Wrappers | 55/55 | ✅ Complete — full wrapper ecosystem |
| Network Simulator | 26/26 | ✅ Complete — connection pooling, lifecycle management |
| Network Concept Template Fix | all | ✅ Complete — unified single-parameter concepts |
| Certificate Authority | 35/35 | ✅ Complete — local CA, `ca_service`/`ca_cluster_node`, ACME (RFC 8555/8738), fingerprint-pinned bootstrap; task 31's LocalStack/real-EC2 tests compile-verified only (no AWS access in this environment) |
| Membership Change | 20/20 | ✅ Complete — joint consensus (Raft §6) add/remove server, joint quorum, config-entry apply path, follower update, node recovery on restart; found already substantially implemented, `tests/node_recovery_unit_test.cpp` added to close the one real gap |

---

## Remaining Work (All Optional)

### Build Tooling

- [x] **clang-format integration** — `.clang-format` config (Google base, 4-space
  indent, 100-col); CMake `format`/`format-check` targets; pre-commit hook
  checks staged files first; `SKIP_FORMAT_CHECK=1` escape hatch
- [x] **clang-tidy integration** — `.clang-tidy` config with `WarningsAsErrors: "*"`;
  CMake `static-analysis`/`static-analysis-fix` targets; pre-commit opt-in hook
  step; zero findings across 291 source files
- [x] **Code coverage** — CMake `ENABLE_COVERAGE` option with gcovr targets,
  HTML reports, and pre-commit ratchet hook (`coverage_floor.txt` = 84.8%)
- [x] **CI reliability (flaky build-and-test/coverage jobs)** — `ca_cluster_node_test`
  (real multi-process Raft cluster, flaked under `ctest -j$(nproc)` CPU
  contention) now retries via `--repeat until-pass:3` and is isolated from
  co-scheduling via `PROCESSORS 4`; coverage floor check has a 0.50pp
  tolerance band for CI-vs-local measurement noise; coverage job's
  disk-reclaim step widened after intermittent "No space left on device"
  link failures
- [x] **stdexec future backend** — a second, `stdexec` (P2300 sender/receiver)
  backed `Future`/`Promise`/`Try`/`Executor` implementation alongside the
  default Folly one, for new code wanting direct access to `stdexec`
  schedulers/algorithms; `include/raft/future_stdexec.hpp`, backend
  selection via `KYTHIRA_DEFAULT_FUTURE_BACKEND` CMake option
  (`include/raft/future_default.hpp`); no existing production call site
  converted, Folly stays the default and required dependency regardless;
  spec at `.kiro/specs/stdexec-future-backend/`; 52/52 tasks complete;
  found and fixed a real GCC 13 `-O2`/`-O3` miscompilation of
  `exec::any_sender`'s small-buffer-optimized move constructor along the
  way (`-fno-strict-aliasing` for GCC builds, `clang++-18` unaffected)
- [ ] **Remove unused includes** — `#include <future>` in
  `http_transport_impl.hpp`, duplicate folly includes in `simulator_impl.hpp`
- [ ] **Folly CMake detection** — improve `find_package` logic so builds
  gracefully degrade when Folly is absent

### New Transport Implementations

- [ ] **Boost.Beast HTTP transport** — async I/O via Boost.Asio, HTTP/2,
  native keep-alive, better Folly EventBase integration
- [ ] **Proxygen HTTP transport** — Facebook's framework, HTTP/3/QUIC,
  connection multiplexing, native Folly integration

### Protocol Completeness

- [x] **Peer-to-peer log replication (gossip catch-up)** — opt-in
  `peer2peer_replicator_type` extension so a lagging member pulls missing log
  entries from another member that already has them instead of exclusively
  from the leader, addressing the single-leader `replicate_to_followers()`
  fan-out bottleneck for catch-up scenarios (rolling restarts, healed
  partitions, bursty joins); leader remains sole commit authority (no change
  to `_commit_index`/election safety), no-op default (`no_op_peer2peer_replicator`)
  preserves today's leader-only behavior exactly, activates only for catch-up
  (steady-state replication unchanged); the replicator's own peer set tracks
  `node<Types>::cluster_members()` — the replicated log's core cluster
  membership (`_configuration.nodes()`, unioned with `old_nodes()` during
  joint consensus, excluding learners) — pushed via `sync_peer2peer_membership()`
  at every `_configuration` mutation site, not separately maintained
  configuration; spec at `.kiro/specs/peer2peer-log-replication/`;
  21 tasks across 4 phases complete
- [x] **Peer-to-peer gossip transport** — `tcp_gossip_peer2peer_replicator`,
  a real anti-entropy gossip implementation (randomized push-pull digest
  exchange, Cassandra/Dynamo-style, not SWIM — Raft's own election timeouts
  already cover liveness detection) of the `peer2peer_replicator` concept
  above; self-contained TCP listener + background gossip thread,
  independent of the Raft RPC transport (`network_client_type`/
  `network_server_type` untouched); current membership comes exclusively
  from `sync_peer2peer_membership()` (driven by the log, per the spec above),
  never separately configured — only node-ID-to-address resolution
  (`address_book`) remains static, since addresses aren't log data; depends
  on `.kiro/specs/peer2peer-log-replication/`; test strategy deliberately
  avoids subprocess-spawning tests (single-process, real-TCP, multi-instance
  instead) after `ca_cluster_node_test` was diagnosed as this project's
  dominant CI-flake source; spec at `.kiro/specs/peer2peer-gossip-transport/`;
  14 tasks across 4 phases complete
- [x] **Membership change (add/remove server)** — joint consensus (Raft §6):
  log entry type discriminant, leader append of C_old+new, joint quorum
  (commit-index and election), `apply_committed_entries()` config-entry
  handling, C_new append after joint commit, leader step-down on
  self-removal, follower configuration update and truncation revert, node
  recovery on restart (log/snapshot/config reload); property tests for add
  server, remove server, and leader-crash-mid-change; spec at
  `.kiro/specs/membership-change/`; 20 tasks across 7 phases complete
- [x] **Node bootstrap** — `peer_discovery` concept + `ClusterJoin` RPC so a fresh
  node can locate an existing cluster and request membership without out-of-band
  `set_cluster_configuration()` calls; spec at `.kiro/specs/node-bootstrap/`;
  20 tasks across 7 phases complete; `no_op_peer_discovery` default preserves all
  existing behaviour; includes `rfc1035_peer_discovery`, `rfc2136_ldns_discovery`,
  `coap_multicast_peer_discovery` adaptors; 6 property tests + 14 unit tests +
  12 chaos tests; `register_node` ordering bug fixed (set `_self_address` after
  successful `send_update`)
- [x] **`poco_peer_discovery`** — registers and discovers nodes via the platform
  DNS-SD daemon (Avahi on Linux) using Poco DNSSD; TXT-record freshness with
  background renewal thread; Docker scenario test (`docker-poco-discovery-tests`);
  spec at `.kiro/specs/dns-peer-discovery/`
- [x] **`rfc2136_dns_sd_discovery`** — DNS-SD over unicast DNS via RFC 2136; publishes
  PTR + SRV + TXT records per node; background fresher thread renews `fresh_until`
  TXT field so stale entries from crashed nodes expire; 6 unit tests + 4 chaos tests;
  Docker scenario test (`docker-dns-sd-discovery-tests`) with BIND9; spec at
  `.kiro/specs/dns-peer-discovery/`
- [ ] **`rfc6763_peer_discovery`** (partial) — SRV-query-only peer discovery; building
  block for `rfc6763_ldns_peer_discovery`; spec at `.kiro/specs/dns-peer-discovery/`
- [ ] **`rfc6763_ldns_peer_discovery`** (full) — registers PTR + instance SRV +
  cluster-level SRV + domain-level SRV via RFC 2136; spec at
  `.kiro/specs/dns-peer-discovery/`

### Certificate Management

- [x] **Certificate authority framework** — in-process `certificate_authority`
  (root CA generation, leaf issuance, revocation/CRL, `from_existing()`),
  `temp_cert_files` RAII helper, `ca_service` CLI (oneshot Docker/Podman
  provisioning + `--serve` HTTP API mode with `local`/`aws-acm-pca`
  providers); spec at `.kiro/specs/certificate-authority/`; 35 tasks complete
- [x] **`aws_acm_pca_provider`** — `certificate_provider` backed by AWS
  Certificate Manager Private CA; unit/LocalStack/real-AWS test tiers
- [x] **TLS hot-reload** — `reload_tls_material()`/`enable_auto_reload()` for
  `cpp_httplib_server`/`client` and `coap_server`/`client`; atomic
  write-tmp-then-rename certificate rotation via
  `temp_cert_files::replace_atomically()`
- [x] **`ca_cluster_node`** — Raft-replicated CA (`ca_state_machine` ledger of
  bootstrap/issuance/revocation, leader reconstructs via `from_existing()`
  and replays the ledger on election); multi-node subprocess test coverage
  including leader failover; packaged for 3-AZ AWS deployment (systemd, ECS
  task definitions); LocalStack/real-EC2 tests compile-verified only (no AWS
  access in this environment)
- [x] **ACME support (RFC 8555/8738)** — `acme_test_server` mock CA,
  `acme_certificate_provider` (JWS order lifecycle, http-01/dns-01
  challenges, `"ip"`-typed identifiers, per-identifier challenge-type
  dispatch), `.local` (mDNS) challenge validation with a distinguishable
  `mdnsResolverUnavailable` error when unavailable
- [x] **Fingerprint-pinned bootstrap** — `ca_bootstrap_client::fetch_trusted_root()`
  establishes first-contact TLS trust from an out-of-band SHA-256 root
  fingerprint + bearer token, before any certificate chain exists to verify
  against
- [x] **`ca_cluster_node` RPC mTLS** — secures the Raft-internal RPC channel
  between `ca_cluster_node` peers (previously plain TCP via
  `tcp_rpc_client`/`tcp_rpc_server`, itself untouched) with mutual TLS via
  a new sibling transport (`include/raft/tls_tcp_rpc.hpp`,
  `tls_tcp_rpc_client`/`tls_tcp_rpc_server`); two-phase bootstrap — a
  static, operator-provisioned shared credential (mirroring the existing
  unseal-passphrase distribution) authenticates peers before the CA root
  exists, then each node self-service-acquires its own CA-issued
  certificate and cuts over automatically once a Raft-replicated
  `rpc_tls_ready` readiness set shows every configured peer has done the
  same; spec at `.kiro/specs/ca-cluster-rpc-mtls/`, 13/13 tasks complete;
  4 real concurrency bugs (persistent client `SSL_CTX*`, server-side
  socket timeouts, accept/present trust-widening ordering, follower
  RPC-forwarding gaps) plus a 5th, CI-only deadlock (leader switching
  identity before any follower had time to widen its own trust policy,
  which broke the read-index heartbeats the follower's own widen step
  needed — closed with a 3-second grace period) found and fixed via
  multi-process and real-CI testing, none of which loopback/2-node-
  in-process testing alone caught; real-AWS validation tracked separately
  — see below
- [x] **`ca_cluster_node` RPC mTLS — real-AWS validation** — extends
  `certificate-authority`'s existing real-EC2 harness
  (`tests/ca_cluster_node_real_ec2_test.cpp`, plain-TCP) to run RPC TLS
  across three real, separate EC2 instances via a new sibling fixture
  (`tests/ca_cluster_node_rpc_tls_real_ec2_test.cpp`): bootstrap-and-cutover
  with the bootstrap credential deleted afterward, staggered node join,
  restart without the bootstrap credential, and a network-isolation
  recovery scenario (subnet-level deny-all NACL swap, reusing the same
  proven technique `aws_quorum_manager_real_ec2_test.cpp` already
  implements, rather than the per-instance security-group reassignment
  originally speced but never actually used anywhere in this codebase) —
  the last of which no loopback test can exercise at all. Directly
  motivated by the CI-only deadlock above: loopback/CI-container testing
  already missed one real race once, so this adds a second,
  environment-specific line of defense on real infrastructure. Also
  generalizes `aws-quorum-manager`'s cost-tracking and signal-driven-cleanup
  apparatus (previously only in `tests/aws_quorum_manager_real_ec2_test.cpp`)
  into a shared header (`tests/aws_real_ec2_test_support.hpp`) so every
  real-EC2 test binary gets both — including
  `ca_cluster_node_real_ec2_test.cpp`, which had neither before and would
  have leaked a VPC and running EC2 instances if killed mid-run; fixed an
  unrelated pre-existing bug found along the way in that same file (its
  `--peers`/curl checks used `https://` against a server the test never
  actually configures with `--tls-cert`/`--tls-key`). Spec complete,
  9/9 tasks implemented (`.kiro/specs/ca-cluster-rpc-mtls-real-aws/`); full
  project builds cleanly and every fixture was confirmed to fail gracefully
  with a clear "skip" message when AWS credentials are absent — same
  compile-verified-only limitation `ca_cluster_node_real_ec2_test.cpp`
  already had (no AWS account available in this environment to actually run
  any of the three real-EC2 binaries).
- [ ] **`ca_cluster_node` custom AMI (Packer build pipeline)** — spec
  authored, not yet implemented; produces a golden, secret-free AMI with
  `ca_cluster_node` and its systemd unit pre-installed, giving
  `aws_ec2_quorum_manager_config.image_id` and
  `KYTHIRA_EC2_TEST_AMI` a real, scripted producer instead of a manually
  hand-built AMI; spec at `.kiro/specs/ca-cluster-node-ami/`

### Cloud Provider Support

- [x] **AWS** — `aws_ec2/asg_quorum_manager` (node ID = EC2 instance ID hex,
  `DescribeInstanceStatus` liveness, consistency poll) and
  `aws_acm_pca_provider` (`certificate_provider` backed by AWS Certificate
  Manager Private CA); `aws-sdk-cpp` features: `acm-pca`, `autoscaling`,
  `ec2`, `iam`, `s3`, `sts`
- [ ] **Microsoft Azure** — quorum manager backed by a Virtual Machine Scale
  Set (node ID = VM instance ID, instance-view power state for liveness) and
  a `certificate_provider` backed by Azure Key Vault Certificates
- [ ] **Google Cloud Platform (GCP)** — quorum manager backed by a Managed
  Instance Group (node ID = GCE instance ID, `instances.get` status for
  liveness) and a `certificate_provider` backed by Certificate Authority
  Service (CAS)
- [ ] **Oracle Cloud Infrastructure (OCI)** — quorum manager backed by an
  Instance Pool and a `certificate_provider` backed by OCI Certificates
  Service
- [ ] **Alibaba Cloud** — quorum manager backed by an Auto Scaling Group and
  a `certificate_provider` backed by Alibaba Cloud SSL Certificates Service

### Metrics Backends

All entries below are `kythira::metrics`-concept implementations
(`include/raft/metrics.hpp`) — today only satisfied by `noop_metrics`, a
zero-cost stub. `metrics_type` is a compile-time template parameter on
`raft_types`/`tcp_raft_types` (default `noop_metrics`), so adding a concrete
backend needs no change to that seam, only a new type satisfying the
concept (`set_metric_name`/`add_dimension`/`add_one`/`add_count`/
`add_duration`/`add_value`/`emit`, all non-blocking — I/O deferred to a
background emitter).

- [ ] **AWS CloudWatch** — `PutMetricData`-backed implementation forwarding
  to CloudWatch instead of a third-party agent; natural pairing with
  `aws_ec2_quorum_manager`/`aws_asg_quorum_manager`, already implemented
  (Cloud Provider Support, above)
- [ ] **Azure Monitor** — forwards to Azure Monitor's custom-metrics API;
  pairs with the Azure quorum manager/certificate provider entry above
- [ ] **GCP Cloud Monitoring** — forwards to Cloud Monitoring's
  `timeSeries.create` API; pairs with the GCP quorum manager/certificate
  provider entry above
- [ ] **OCI Monitoring** — forwards to OCI Monitoring's `PostMetricData`
  API; pairs with the OCI quorum manager/certificate provider entry above
- [ ] **Alibaba Cloud CloudMonitor** — forwards to CloudMonitor's custom-
  metrics API; pairs with the Alibaba Cloud quorum manager/certificate
  provider entry above
- [ ] **Prometheus** — exposes an HTTP `/metrics` scrape endpoint (text
  exposition format); the dominant pull-based backend for Kubernetes/
  container deployments
- [ ] **Telegraf** — emits to a local Telegraf agent (StatsD or InfluxDB
  line protocol over UDP/TCP), letting operators fan metrics out to
  whatever Telegraf output plugin they already run (InfluxDB, Graphite,
  Kafka, etc.) without Kythira needing to pick one
- [ ] **VictoriaMetrics** — Prometheus-remote-write-compatible, so likely
  shares most of the Prometheus implementation's wire format rather than
  needing a wholly separate one
- [ ] **NetData** — via NetData's StatsD-compatible collector or its own
  REST API, for operators already running NetData for host-level
  monitoring who want Kythira's Raft/RPC metrics in the same dashboard

### Minor Enhancements

- [x] **State machine examples** — counter, register, replicated log, and
  distributed lock examples for documentation/demonstration purposes
  (all four now have test targets)
- [x] **libfiu integration** — fault injection chaos testing; spec at
  `.kiro/specs/libfiu-integration/`; 21 tasks complete: CMake detection,
  `fiu_do_on` fault points in persistence/network/state machine, RAII fault
  profiles, safety assertion helpers, smoke + profile + 8 safety/liveness tests
- [x] **Docker chaos testing** — real multi-node `chaos_node` cluster; TCP RPC +
  file persistence + HTTP control plane + libfiu TCP remote; Docker packaging;
  C++ harness (`harness.hpp`, `os_faults.hpp`, `fault_control.hpp`) with
  injectable `CmdExecutor`/`HttpGet`/`HttpPost` stubs for unit testing;
  32 harness unit tests (`docker_chaos_harness_unit_tests`,
  `docker_chaos_fault_control_unit_tests`) registered in CTest; 7 chaos scenario
  tests (election recovery, crash recovery, network degradation, AZ partition,
  persistence faults, safety assertions, leadership stability) + 3 DNS discovery
  scenario tests + 3 DNS-SD discovery scenario tests + 3 poco_peer_discovery
  scenario tests; Podman runtime support and rootless Podman compatibility;
  25 original tasks + 5 expansion tasks complete;
  spec at `.kiro/specs/docker-chaos/`
- [ ] **Memory usage profiling** — optional optimization pass

---

## Historical Notes

Full task-by-task implementation history is preserved in the spec files under
`.kiro/specs/`. Per-component status details are in `doc/RAFT_IMPLEMENTATION_STATUS.md`,
`doc/RAFT_TESTS_FINAL_STATUS.md`, and `doc/PERFORMANCE_VALIDATION.md`.
