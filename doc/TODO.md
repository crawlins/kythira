## TODO: Outstanding Tasks and Improvements

**Last Updated**: June 22, 2026

## Current Status

The project is **PRODUCTION READY** ✅ with 100% test pass rate.

- **All tests passing** (100%)
- **0 tests failing, 0 tests disabled, 0 flaky tests**
- All specifications complete across all 7 feature areas (node bootstrap now complete)
- Build clean with no errors or warnings

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

## Completed Specifications (All 6/6 Complete)

| Spec | Tasks | Status |
|------|-------|--------|
| Raft Consensus | 287/287 | ✅ Complete — includes Phase 5 multi-node testing (700–731) |
| HTTP Transport | 17/17 | ✅ Complete — A+ SSL/TLS, 931K+ ops/sec |
| CoAP Transport | 26/26 | ✅ Complete — DTLS, block transfer, 30K+ ops/sec |
| Folly Concept Wrappers | 55/55 | ✅ Complete — full wrapper ecosystem |
| Network Simulator | 26/26 | ✅ Complete — connection pooling, lifecycle management |
| Network Concept Template Fix | all | ✅ Complete — unified single-parameter concepts |

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

- [ ] **Membership change (add/remove server)** — joint consensus (Raft §6)
  implementation; `add_server()`/`remove_server()` API exists but log append,
  joint quorum, config-entry apply path, and node recovery are all missing;
  spec at `.kiro/specs/membership-change/`; 20 tasks across 7 phases
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

### Minor Enhancements

- [ ] **State machine examples** — counter, register, replicated log, and
  distributed lock examples for documentation/demonstration purposes
  (counter and register already exist as test targets)
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
