## TODO: Outstanding Tasks and Improvements

**Last Updated**: June 10, 2026

## Current Status

The project is **PRODUCTION READY** ✅ with 100% test pass rate.

- **279/279 tests passing** (100%)
- **0 tests failing, 0 tests disabled, 0 flaky tests**
- All specifications complete across all 6 feature areas
- Build clean with no errors or warnings

### What Changed (June 10, 2026)

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
- [ ] **clang-tidy integration** — add `.clang-tidy` config and CMake
  `static-analysis` target; configure checks and suppressions
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

### Minor Enhancements

- [ ] **State machine examples** — counter, register, replicated log, and
  distributed lock examples for documentation/demonstration purposes
  (counter and register already exist as test targets)
- [ ] **Stress testing** — high-load scenarios and chaos testing framework
- [ ] **Memory usage profiling** — optional optimization pass

---

## Historical Notes

Full task-by-task implementation history is preserved in the spec files under
`.kiro/specs/`. Per-component status details are in `doc/RAFT_IMPLEMENTATION_STATUS.md`,
`doc/RAFT_TESTS_FINAL_STATUS.md`, and `doc/PERFORMANCE_VALIDATION.md`.
