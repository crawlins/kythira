## TODO: Outstanding Tasks and Improvements

**Last Updated**: June 18, 2026

## Current Status

The project is **PRODUCTION READY** ✅ with 100% test pass rate.

- **All tests passing** (100%)
- **0 tests failing, 0 tests disabled, 0 flaky tests**
- All specifications complete across all 7 feature areas (node bootstrap now complete)
- Build clean with no errors or warnings

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
  `docker_chaos_fault_control_unit_tests`) registered in CTest; 7 scenario
  test files (election recovery, crash recovery, network degradation, AZ
  partition, persistence faults, safety assertions, leadership stability);
  25 tasks complete; spec at `.kiro/specs/docker-chaos/`
- [ ] **Memory usage profiling** — optional optimization pass

---

## Historical Notes

Full task-by-task implementation history is preserved in the spec files under
`.kiro/specs/`. Per-component status details are in `doc/RAFT_IMPLEMENTATION_STATUS.md`,
`doc/RAFT_TESTS_FINAL_STATUS.md`, and `doc/PERFORMANCE_VALIDATION.md`.
