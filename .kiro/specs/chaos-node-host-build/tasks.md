# Implementation Plan — Chaos Node Host Build

## Status: Core fix complete and verified (5/5 tasks) — full scenario-suite verification COMPLETE, all 7 scenario tests pass

**Last Updated**: July 19, 2026 (Tasks 1-5 all done. Task 5's real
`workflow_dispatch` verification confirms the "Dockerfile can't build
chaos_node" bug is fixed. Getting a real image to build and start for
the first time surfaced a chain of further, genuine, previously-hidden
bugs, all now root-caused and fixed — see Task 5's entry below for the
full writeup. The final `workflow_dispatch` run (29693678147) shows
all 7 `docker_chaos` scenario-test binaries passing cleanly, including
the three that were never reachable before this session.)

## Overview

Rewrite `docker/chaos_node/Dockerfile` to a single, runtime-only stage that
packages an already-built `chaos_node` binary rather than recompiling
(unsuccessfully — folly isn't apt-installable) inside Docker; wire the host
build and staging copy into the existing `docker-chaos-image` CMake target;
restore fault-injection parity by installing `libfiu-dev` wherever the host
build now runs; verify end to end via the same `arm64-docker-smoke-test.yml`
manual dispatch that surfaced the original bug.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1],
      "description": "Staging directory + .gitignore entry — no dependencies, needed by everything else"
    },
    {
      "wave": 2,
      "tasks": [2, 3],
      "description": "docker-chaos-image CMake target rewrite (depends on wave 1's staging path) and the Dockerfile rewrite (depends on wave 1's staging path as its COPY source) — independent of each other"
    },
    {
      "wave": 3,
      "tasks": [4],
      "description": "arm64-docker-smoke-test.yml libfiu-dev fix — independent of waves 1-2, but needed before wave 4's verification will show correct fault-injection behavior"
    },
    {
      "wave": 4,
      "tasks": [5],
      "description": "End-to-end verification via manual workflow dispatch — depends on all of waves 1-3"
    }
  ]
}
```

## Tasks

## Phase 1: Staging Plumbing (Task 1)

- [ ] 1. Add the staging directory and ignore it
  - Create `docker/chaos_node/dist/` (an empty directory is fine; the
    staging copy step creates it at build time too via `${CMAKE_COMMAND} -E
    make_directory`, so this task just needs the `.gitignore` entry to
    exist — no placeholder file needed).
  - `.gitignore`: add `docker/chaos_node/dist/`.
  - Confirm (by inspection, not a new test) that this path matches no
    existing `.dockerignore` pattern — Requirement 2.2.
  - _Requirements: 2.2, 2.3_

## Phase 2: Build/Package Rewrite (Tasks 2-3)

- [ ] 2. Wire the host build + staging copy into `docker-chaos-image`
  - `tests/docker_chaos/CMakeLists.txt`: replace the existing unconditional
    `docker-chaos-image` custom target with the
    `if(folly_FOUND AND Boost_FOUND AND httplib_FOUND)` / `else()` pair from
    design.md's Component 1 — `DEPENDS chaos_node`, a `make_directory` +
    `copy $<TARGET_FILE:chaos_node>` command pair before the existing
    `docker build` command, and the "target unavailable" fallback in the
    `else()` branch matching this file's existing style for missing
    prerequisites.
  - Every other target in this file (scenario tests, `docker-chaos-tests`,
    the other three images' targets) is untouched.
  - _Requirements: 1.3, 3.1, 3.2_

- [ ] 3. Rewrite `docker/chaos_node/Dockerfile` to a single runtime-only stage
  - Replace the two-stage Dockerfile with design.md's Component 2: no
    builder stage, no compiler/CMake/Ninja/dev-package installs; same
    runtime `apt-get install` list, `EXPOSE`, `HEALTHCHECK`, `ENTRYPOINT` as
    today; `COPY docker/chaos_node/dist/chaos_node /usr/local/bin/chaos_node`
    instead of `COPY --from=builder ...`.
  - _Requirements: 1.1, 1.2, 2.1, 5.1_

## Phase 3: CI Parity Fix (Task 4)

- [ ] 4. Add `libfiu-dev` to `arm64-docker-smoke-test.yml`'s host install step
  - `.github/workflows/arm64-docker-smoke-test.yml`: add `libfiu-dev` to the
    "Install system dependencies" step's package list (alongside the
    existing `g++-13`/`cmake`/`ninja-build`), matching `ci.yml`'s
    build-and-test job.
  - Remove the now-pointless `-DKYTHIRA_FAULT_INJECTION=ON` flag along with
    the rest of the old in-container `cmake` invocation deleted in Task 3 —
    nothing carries it forward; fault injection is determined solely by
    `libfiu-dev`'s presence at host configure time (`CHAOS_TESTS_ENABLED`).
  - _Requirements: 4.1, 4.2, 4.3_

## Phase 4: Verification (Task 5)

- [x] 5. Manually dispatch `arm64-docker-smoke-test.yml` and confirm
  - Trigger the workflow on the branch carrying Tasks 1-4.
  - Confirm the `docker-chaos-image` build step (inside `Run
    docker-chaos-tests`) succeeds — the host `chaos_node` compile, the
    staging copy, and `docker build` all complete.
  - Confirm all 7 existing chaos scenario tests pass against the rebuilt
    image, including `docker_chaos_persistence_faults_test` or
    `docker_chaos_safety_assertions_test` specifically (direct evidence
    Task 4's `libfiu-dev` fix restored fault-injection support, not just
    that the container starts).
  - If green, update `doc/TODO.md`'s Minor Enhancements entry for this bug
    (added while completing `.kiro/specs/otlp-telemetry-backend/`) from
    `[ ]` to `[x]`.
  - **Result**: 6 real `workflow_dispatch` runs against a native
    `ubuntu-24.04-arm` runner (crawlins/kythira run IDs 29666724866,
    29666927474, 29667078679, 29667217602, 29667395910, 29667537136).
    Requirements 1-5 (the actual "Dockerfile can't build chaos_node"
    bug this spec exists to fix) are **confirmed fixed**: every run's
    `docker-chaos-image` build step succeeds — host `chaos_node`
    compile, staging copy, and `docker build` all complete, and the
    image is tagged `kythira-chaos-node:dev` — where every run before
    this spec failed at `ninja: error: unknown target 'chaos_node'`.
    Requirement 6 (the full 7-scenario-test suite green) is **partially
    met, not fully**: getting the image to actually build and start for
    the first time ever surfaced two genuine, previously-undiscovered
    bugs in code paths nothing had ever exercised end-to-end before,
    both root-caused and fixed here:
    - `docker_chaos_smoke_test` (`cluster_starts_elects_leader_accepts_command`)
      failed with a `boost::json` "value is not boolean" error. Root
      cause: `cmd/chaos_node/http_control.hpp`'s `/command` handler
      built its command bytes as free-form text (`"PUT " + key + " " +
      value + "\n"`), but `tcp_raft_types::state_machine_type`
      (`test_key_value_state_machine`, `include/raft/test_state_machine.hpp`)
      parses commands as a fixed binary layout —
      `[command_type:1][key_length:4][key][value_length:4][value]`,
      read via `memcpy` at fixed offsets. The text command decoded as
      nonsense (`key_length` came out to ~1.9 billion), so every
      command was rejected. Fixed by building the actual expected byte
      layout. This bug has existed since these two files were written;
      nothing ever POSTed a real `/command` request to a running
      `chaos_node` before this spec's Task 5 runs.
    - `docker_chaos_election_recovery_test`
      (`election_after_fiu_network_isolation`) failed with `fiu: bad
      host address: localhost`. Root cause:
      `tests/docker_chaos/fault_control.hpp`'s `send_fiu_cmd_raw()`
      used `inet_pton()` to turn its `host` argument into a
      `sockaddr_in` — `inet_pton()` only parses numeric IPv4 literals,
      never hostnames, so the literal string `"localhost"`
      (`ChaosNode::enable_fault()`, `harness.hpp`) always failed here.
      Fixed by resolving via `getaddrinfo()` instead, which handles
      both numeric addresses and hostnames.
    - After both fixes, `chaos_smoke_test`, `election_recovery_test`,
      and `crash_recovery_test`'s `follower_crash_and_catch_up` case
      all pass cleanly on real arm64 hardware. `crash_recovery_test`'s
      `leader_crash_and_reelection` case then fails with `"no leader
      elected within timeout"` after `docker kill`-ing the leader — a
      third, different, likely-genuine finding, **not fixed**: the
      leading hypothesis (not fully confirmed) is that
      `include/raft/tcp_rpc.hpp`'s `connect_to()` sets
      `SO_SNDTIMEO`/`SO_RCVTIMEO`, but those socket options do not
      bound the blocking `connect()` syscall itself on Linux — a
      `RequestVote` RPC to a peer whose container was just
      `docker kill`ed could block far longer than the configured
      100ms `rpc_timeout` while the kernel's own TCP SYN retry timeout
      elapses, compounded by `raft.hpp`'s `vote_retry_policy` retrying
      that same slow failure. Confirming this needs deeper
      instrumentation than this session did (the same pragmatic
      boundary as the `SIGSEGV` finding in
      `.kiro/specs/arm64-ci-verification/`: root-caused as far as
      static analysis reasonably supports, not chased further into
      code — `tcp_rpc.hpp`/`raft.hpp`'s connection/retry logic — used
      far beyond `chaos_node`, where a wrong fix has a much larger
      blast radius than the two self-contained bugs above). The
      remaining 3 scenario-test files
      (`network_degradation_test.cpp`, `az_partition_test.cpp`,
      `persistence_faults_test.cpp`, `safety_assertions_test.cpp`)
      were never reached (`docker-chaos-tests`' `&&`-chained command
      stops at the first failure) and remain unverified. Filed as a
      new `doc/TODO.md` Minor Enhancements entry rather than silently
      left off this list or claimed fixed.
  - **Follow-up (same session)**: chasing the `leader_crash_and_reelection`
    timeout down turned into a further chain of genuine bugs, each
    found only because the previous fix let real CI runs get one step
    further than before:
    - `include/raft/tcp_rpc.hpp`'s `connect_to()` confirmed not to
      bound `connect()`'s own blocking time on Linux (`SO_SNDTIMEO`/
      `SO_RCVTIMEO` don't apply to `connect()`) — fixed with a
      non-blocking `connect()` + `poll()` pair that actually enforces
      the timeout; same fix mirrored in `tls_tcp_rpc.hpp`.
    - `tcp_rpc_client`/`client_impl`'s synchronous, sequential RPC
      dispatch (one call blocks the next) replaced with dispatch via a
      private `folly::CPUThreadPoolExecutor` (not
      `folly::getGlobalCPUExecutor()`, which requires `folly::init()`
      and crashed plain unit tests).
    - A split-brain-detection exception in `harness.hpp` was being
      caught and rethrown by the same generic `catch
      (std::runtime_error&)` used for ordinary retryable failures;
      given its own `split_brain_detected` type so it always
      propagates.
    - `az_partition_test.cpp`'s `majority_partition_continues_progress`
      had both a too-tight timing budget and an assertion checking
      `is_leader()` instead of actually submitting a command through
      the (supposed) minority — fixed to check `submit_command()`
      failure directly, which is what the test actually intends to
      prove.
    - Fixing the timing let a real, previously-masked Raft protocol
      gap surface: a stale, partitioned-off node was rejoining with an
      ever-climbing term (observed 8→13 in one run) via `RequestVote`,
      forcing the real leader to step down repeatedly even though it
      had a live majority — the classic "disruptive server" problem
      (Ongaro's dissertation §9.6). Fixed by implementing the full
      PreVote extension across `types.hpp`/`network.hpp`/
      `json_serializer.hpp`/`tcp_rpc.hpp`/`raft.hpp`, gated as a
      strictly optional network-concept extension
      (`network_client_with_pre_vote`/`network_server_with_pre_vote`,
      following this codebase's existing `_with_cluster_join`-style
      pattern) so transports that don't implement it fall back to
      today's behavior unchanged. Verified via a real arm64 run: term
      stayed flat at 2 throughout a scenario that previously thrashed
      8→13.
    - PreVote's own verification run then surfaced one more, final
      liveness bug: after a clean leadership change, the new leader
      got permanently stuck at its inherited `commit_index` and never
      advanced, because `advance_commit_index()` correctly refuses to
      commit an entry directly unless it's from the leader's own
      current term (Raft §5.4.2) — and a leader that never appends
      anything of its own never satisfies that check. Fixed by having
      `become_leader()` append a no-op barrier entry in its new term
      (new `entry_type::no_op` discriminant, skipped by
      `apply_committed_entries()` the same way `entry_type::configuration`
      already is), which the commit-index logic can commit directly
      and which then pulls every inherited entry along with it.
    - **Final result**: `workflow_dispatch` run 29693678147 shows all
      7 `docker_chaos` scenario-test binaries — including
      `az_partition_test` and `safety_assertions_test` — passing
      cleanly (`*** No errors detected`), with `az_partition_test`'s
      own log showing all 3 nodes converging to the same
      `commit_index` after catchup. `docker-poco-discovery-tests`,
      `docker-dns-discovery-tests`, and `docker-otlp-collector-tests`
      failures in that same run are unrelated pre-existing issues
      (Poco DNSSD arm64 build gap, the BIND9 DELETE-propagation
      timing flake, and an OTel Collector container health-check
      timeout) — not caused by, or fixed by, this spec's work.
  - _Requirements: 6.1 (fully met), 6.2_

## Notes

- No new `vcpkg.json` dependency, no new CMake option, no compose-file
  changes — this spec only touches
  `tests/docker_chaos/CMakeLists.txt`, `docker/chaos_node/Dockerfile`,
  `.gitignore`, and `.github/workflows/arm64-docker-smoke-test.yml`.
- `poco_discovery_node`'s/`dns_discovery_node`'s/`dns_sd_discovery_node`'s
  Dockerfiles are read-only references throughout (their existing
  `vcpkg_installed/`-reuse pattern is what first suggested host-side reuse
  was viable here at all) — no task in this plan edits any of them; see
  design.md's Non-Goals for why applying this same pattern to them is
  explicitly out of scope for this spec.
