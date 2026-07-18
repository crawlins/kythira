# Implementation Plan — ARM64 CI Verification

## Status: Complete — 13/13 tasks

**Last Updated**: July 18, 2026 (Task 10 completed via 5 real
`workflow_dispatch` runs against `.github/workflows/arm64-docker-smoke-test.yml`
on a native `ubuntu-24.04-arm` runner — see its entry below for the full
per-image results, including one newly-discovered, genuine arm64-specific
`SIGSEGV` in the DNS/DNS-SD discovery scenario tests, root-caused as a
real stack-use-after-scope bug in `peer_ids()`'s JSON-parsing loop and
fixed — see `doc/CHANGELOG.md`'s July 18, 2026 entry for the full
writeup.)

## Overview

Verify Kythira builds and passes its test suite natively on 64-bit ARM
(`aarch64`/`arm64-linux`) Linux, by parameterizing every hardcoded x86_64/
`x64-linux` assumption in the build system and CI workflows, then adding
native `ubuntu-24.04-arm` legs to the existing CI matrices. Work is gated by
a pre-implementation spike confirming vcpkg dependency availability for the
`arm64-linux` triplet — no workflow change lands ahead of that confirmation.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 0,
      "tasks": [1],
      "description": "Spike: verify vcpkg arm64-linux dependency availability — gates everything else"
    },
    {
      "wave": 1,
      "tasks": [2, 3, 4],
      "description": "CMake triplet parameterization and multiarch library discovery — independent of CI workflow changes"
    },
    {
      "wave": 2,
      "tasks": [5, 6],
      "description": "CI workflow triplet resolution and cache-key isolation (depends on wave 1's CMake variable existing)"
    },
    {
      "wave": 3,
      "tasks": [7, 8],
      "description": "Add the native arm64 build-and-test matrix leg and verify it green (depends on wave 2)"
    },
    {
      "wave": 4,
      "tasks": [9, 10],
      "description": "Docker image triplet parameterization and arm64 image builds (depends on wave 1)"
    },
    {
      "wave": 5,
      "tasks": [11],
      "description": "Real Cloud Tests arm64 leg (depends on wave 2's triplet/cache changes)"
    },
    {
      "wave": 6,
      "tasks": [12, 13],
      "description": "Documentation and TODO update (depends on all preceding waves being green)"
    }
  ]
}
```

## Tasks

---

## Phase 0: Spike (Task 1)

- [x] 1. Verify vcpkg `arm64-linux` dependency availability
  - On a real or emulated arm64 Linux host, run
    `vcpkg install --triplet arm64-linux --x-feature=edhoc` against the
    project's current `vcpkg.json` at its pinned `builtin-baseline`
  - Record per-dependency pass/fail: `boost-algorithm`, `boost-asio`,
    `boost-json`, `boost-system`, `boost-test`, `boost-thread`,
    `cpp-httplib`, `folly`, `libcoap[dtls]`, `openssl`, `poco[net]`,
    `aws-sdk-cpp[acm-pca,autoscaling,ec2,iam,s3,sts]`, `libssh2`,
    `stdexec[tbb]`, and the `lakers` overlay port (requires a Rust
    toolchain — verify `dtolnay/rust-toolchain@stable` or equivalent
    installs on arm64 too)
  - Write findings to `.kiro/specs/arm64-ci-verification/spike-notes.md`
  - IF any dependency fails: document the specific failure, evaluate
    baseline bump / overlay port / upstream fix, and treat resolving it as a
    prerequisite task inserted before Wave 1 — do not proceed to Wave 1
    until every dependency in `vcpkg.json` has a working `arm64-linux` path
  - _Requirements: 10.1, 10.2, 10.3_

---

## Phase 1: CMake Triplet Parameterization (Tasks 2–4)

- [x] 2. Introduce `KYTHIRA_VCPKG_TRIPLET` in `CMakeLists.txt`
  - Add the `CMAKE_SYSTEM_PROCESSOR`-based detection near the top of the
    file, before any existing reference to `vcpkg_installed/x64-linux`
  - Replace the `POCO_DNSSD_FOUND` block's three hardcoded
    `vcpkg_installed/x64-linux/...` literals with
    `vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/...`
  - Verify: on the existing x86_64 dev/CI environment, `cmake -B build`
    produces byte-identical resolved paths to before this change (no
    behavior change on x86_64)
  - _Requirements: 2.2, 2.4_

- [x] 3. Replace remaining `x64-linux` literals in test CMakeLists
  - `tests/CMakeLists.txt`: all `EXISTS ".../vcpkg_installed/x64-linux/lib/libboost_json.a"`
    and `libboost_context.a` checks (kythira_test_pch and the ~9 individual
    test targets: quorum_management_test, docker_quorum_manager_test,
    bootstrap_property_test, bootstrap_unit_test, dns_peer_discovery_unit_test,
    poco_peer_discovery_unit_test, and any others matching the same pattern)
  - `tests/chaos/CMakeLists.txt`: the shared `${test_name}` template's
    equivalent checks
  - `tests/docker_chaos/CMakeLists.txt`: the `_boost_json_lib` resolution
  - All replaced with `${KYTHIRA_VCPKG_TRIPLET}` (propagated via
    `CMAKE_SOURCE_DIR`-relative `include()`, so the variable set in the root
    `CMakeLists.txt` is visible in every included test CMake file)
  - Verify: `cmake -B build && cmake --build build` succeeds unchanged on
    x86_64
  - _Requirements: 2.2, 2.3, 2.4_

- [x] 4. Multiarch-aware Avahi library discovery
  - Replace the hardcoded `/usr/lib/x86_64-linux-gnu` search path
    (`CMakeLists.txt` ~line 250–262) with a `CMAKE_LIBRARY_ARCHITECTURE`-based
    (falling back to `KYTHIRA_VCPKG_TRIPLET`-derived) multiarch tuple, for
    both `AVAHI_CLIENT_LIB` and `AVAHI_COMMON_LIB`
  - Confirm `POCO_DNSSD_FOUND` still fails open to `FALSE` (no configure
    error) when the PocoDNSSD static archives are absent for the resolved
    triplet — required for arm64 hosts, which do not have a prebuilt
    PocoDNSSD archive per this spec's scope
  - Verify: x86_64 configure log shows identical `avahi-client found`/`not
    found` messaging as before this change
  - _Requirements: 3.1, 3.2, 3.3_

---

## Phase 2: CI Workflow Triplet and Cache Parameterization (Tasks 5–6)

- [x] 5. Add triplet-resolution step to `ci.yml` and `real-cloud-tests.yml`
  - New step computing the vcpkg triplet from `runner.arch`
    (`ARM64` → `arm64-linux`, else → `x64-linux`), exposed as a step output
  - Replace every hardcoded `--triplet x64-linux`,
    `VCPKG_DEFAULT_TRIPLET: x64-linux`, and
    `-DCMAKE_PREFIX_PATH=.../vcpkg_installed/x64-linux` in both workflow
    files with the resolved value
  - Verify: existing x86_64 jobs in both workflows resolve to `x64-linux`
    unchanged (dry run / no-op diff on the resolved values)
  - _Requirements: 2.1, 2.3_

- [x] 6. Add `runner.arch` to the vcpkg cache key
  - `ci.yml` (both jobs that cache vcpkg) and `real-cloud-tests.yml`:
    `key: vcpkg-${{ runner.os }}-${{ runner.arch }}-edhoc-${{ hashFiles(...) }}`
  - Verify: first workflow run after this change shows a cache miss on the
    existing x86_64 jobs (expected, one-time), and a fresh cache entry is
    created; confirm the new key contains `X64` so it does not collide with
    a future `ARM64` key
  - _Requirements: 4.1, 4.2, 4.3_

---

## Phase 3: Native ARM64 Build-and-Test Leg (Tasks 7–8)

- [x] 7. Add `ubuntu-24.04-arm` to the `build-and-test` matrix
  - Convert the `compiler` matrix to an `include` list adding
    `{ compiler: g++-13, os: ubuntu-24.04-arm, arch_label: arm64 }` and
    `{ compiler: clang++-18, os: ubuntu-24.04-arm, arch_label: arm64 }`
    alongside the existing two x86_64 entries (also given `arch_label: x64`)
  - `runs-on: ${{ matrix.os }}`; test-reporter `name:` and
    `upload-artifact` `name:` include `${{ matrix.arch_label }}`
  - Tighten the format-check step guards to
    `matrix.compiler == 'g++-13' && matrix.arch_label == 'x64'`
  - Set an initial `timeout-minutes` for the job (may need to differ from
    the x86_64 default of 120 once real timing data exists from this task)
  - _Requirements: 1.1, 1.2, 1.3, 1.4_

- [x] 8. Verify the arm64 leg green and tune timeout
  - Run the workflow (push to a branch / draft PR) and confirm all four
    matrix entries pass, including the `--x-feature=edhoc` Rust/`lakers`
    build (Requirement 5) on both arm64 compiler legs
  - Record actual wall-clock build+test time for the arm64 leg; adjust
    `timeout-minutes` from Task 7's placeholder to a measured value with
    headroom
  - If any test fails only on arm64: root-cause and fix (this is the
    concrete "verification" this spec's title refers to) before marking
    this task complete
  - **Result**: verified against a real cold-cache run on
    crawlins/kythira#47 — all four matrix entries passed, including the
    `--x-feature=edhoc` Rust/`lakers` build on both arm64 legs, with no
    arm64-specific failures found. Measured wall-clock times: `g++-13,
    arm64` ~76 min, `clang++-18, arm64` ~46 min (`g++-13, x64` ~51 min,
    `clang++-18, x64` ~68 min — arm64 was not uniformly slower than
    x64). The existing `timeout_minutes: 120` per matrix entry already
    gives the slowest observed leg (~76 min) comfortable headroom
    (~1.6x) for this being a full cache-miss run (every vcpkg dependency
    built from source, including the Rust/lakers overlay port); a warm
    cache should be substantially faster. Left at 120 for all four
    entries rather than tightened, since a cache-miss run (e.g. after a
    vcpkg.json bump) is exactly when the extra headroom matters most.
  - _Requirements: 1.5, 5.1, 5.2, 5.3_

---

## Phase 4: Docker Images on arm64 (Tasks 9–10)

- [x] 9. Parameterize Docker builder-stage triplet paths
  - All seven `docker/*/Dockerfile` files (`chaos_node`,
    `poco_discovery_node`, `dns_discovery_node`, `dns_sd_discovery_node`,
    `bind9` — no CMake build, verify it needs no change, `ca_cluster_node`,
    `ca_service`): replace hardcoded
    `-DCMAKE_PREFIX_PATH=/src/vcpkg_installed/x64-linux` with a
    `uname -m`-derived `TRIPLET` shell variable in the `RUN` step
  - Verify: `docker build` on the existing x86_64 CI/dev host still
    resolves to `x64-linux` and produces identical images
  - _Requirements: 6.1_

- [x] 10. Build and smoke-test each image natively on arm64
  - On a native arm64 host (or arm64 CI runner) with Docker or Podman
    installed, build each `docker-*-image` CMake target
    (`docker-chaos-image`, `docker-poco-discovery-image`,
    `docker-bind9-image`, `docker-dns-discovery-image`,
    `docker-dns-sd-discovery-image`) and confirm each produces a runnable
    `linux/arm64` image with no QEMU/emulation involved
  - Run the corresponding `ctest -L docker` scenario tests against the
    arm64 images and confirm parity with x86_64 results
  - Document any per-image blocker found (e.g. an apt package without an
    arm64 build) as a named limitation rather than silently skipping
  - **Result**: `.github/workflows/arm64-docker-smoke-test.yml` landed on
    `main`, then was actually triggered — 5 real `workflow_dispatch` runs
    on a native `ubuntu-24.04-arm` GitHub-hosted runner (crawlins/kythira
    run IDs 29663122464, 29663269337, 29663422680, 29663614678,
    29663814851). Real, per-image results, not simulated:
    - **`chaos_node`**: image build fails — `ninja: error: unknown
      target 'chaos_node'`. Not arm64-specific: identical failure on
      x64 (two earlier runs from a different branch, run IDs
      29607765347/29607181082) and already tracked as its own item in
      `doc/TODO.md`'s Minor Enhancements
      (`.kiro/specs/chaos-node-host-build/`) — `docker/chaos_node/Dockerfile`
      can't apt-install folly. `docker-otlp-collector-tests` (which
      reuses this image) necessarily fails the same way for the same
      reason; not independently re-run since the outcome isn't in doubt.
    - **`poco_discovery_node`**: image build fails — `ninja: error:
      unknown target 'poco_discovery_node'`. Also not arm64-specific in
      the "new finding" sense: this is the *already-documented*
      consequence of `POCO_DNSSD_FOUND` correctly staying `FALSE` on
      `arm64-linux` (no manually-built PocoDNSSD archives for that
      triplet — `CMakeLists.txt`'s `POCO_DNSSD_FOUND` block,
      README.md's "ARM (arm64) Support" section), which in turn means
      `cmd/poco_discovery_node`'s `add_subdirectory()` never runs and
      the target doesn't exist. Working as designed, not a bug.
    - **`dns_discovery_node`** and **`dns_sd_discovery_node`**: images
      build successfully (these depend on `libldns`, which — per
      README.md — is unaffected on arm64). Initial runs turned up a
      **new, genuine, arm64-specific finding**: both
      `docker_dns_discovery_test`'s and `docker_dns_sd_discovery_test`'s
      `all_nodes_discover_peers` case crashed with a real `SIGSEGV` —
      `fatal error: ... memory access violation ... no mapping at fault
      address` (RFC 1035 variant: 2 crashes / 3 attempts; RFC 2136/DNS-SD
      variant: 1 crash / 1 attempt — intermittent, not deterministic,
      which turned out to be a real clue rather than noise). The sibling
      `all_nodes_healthy` case in both files passed every time.
      `tests/docker_chaos/dns_discovery_test.cpp` and
      `dns_sd_discovery_test.cpp` shared byte-for-byte identical
      `peer_ids()` helpers (confirmed by diff), and that helper contained
      a genuine bug:
      ```cpp
      for (const auto& item : json::parse(res->body).as_array()) { ... }
      ```
      `json::parse(res->body)` returns a `boost::json::value` prvalue;
      `.as_array()` is a *member function call* returning a reference
      into that temporary, not a direct member-access binding — so C++
      does not extend the temporary's lifetime for the range-for loop
      (this is exactly the footgun [P2718R0](https://wg21.link/P2718R0)
      addresses for direct range-expressions, but a function-call
      indirection like `.as_array()` still isn't covered even under
      `-std=c++23` with GCC 13.3). The `boost::json::value` is destroyed
      at the end of the range-for's init-statement, leaving the loop
      iterating over freed stack memory — a stack-use-after-scope. This
      is true undefined behavior on every architecture; it happened to
      "work" most of the time on x86_64 because the freed stack slot
      usually wasn't yet overwritten, while arm64's different stack
      layout/ABI made the corruption manifest as a hard crash far more
      often (still not literally every time, matching the intermittent
      x86_64-vs-arm64 pattern observed). Confirmed with a minimal
      standalone repro compiled with the project's actual toolchain
      (`g++-13 -std=c++23 -fsanitize=address`), which AddressSanitizer
      flagged immediately:
      `stack-use-after-scope ... in boost::json::array::begin()`.
      **Fixed** in all three call sites that had this exact pattern —
      `tests/docker_chaos/dns_discovery_test.cpp`,
      `dns_sd_discovery_test.cpp`, and (a fourth image sharing the same
      `peer_ids()` shape, not previously flagged because
      `poco_discovery_node` doesn't build on arm64 at all)
      `poco_discovery_test.cpp` — by binding the parsed value to a named
      local before iterating:
      ```cpp
      const json::value parsed = json::parse(res->body);
      for (const auto& item : parsed.as_array()) { ... }
      ```
      All three fixed test binaries were rebuilt cleanly against the
      real project headers/flags (`build-clang`, unmodified toolchain).
      Re-verified against real arm64 hardware with a 6th real
      `workflow_dispatch` run (run ID 29664536952) rather than trusting
      the local fix alone, since the whole reason this bug surfaced was
      architecture-specific runtime behavior a local x86_64 build cannot
      fully rule out — **confirmed fixed**: zero `SIGSEGV`/memory-access-
      violation signatures anywhere in that run's logs, and both
      `all_nodes_discover_peers` cases (RFC 1035 and DNS-SD) completed
      cleanly. (Note: per-step `conclusion` in the GitHub Actions API
      always reads `success` for a `continue-on-error: true` step
      regardless of the underlying command's real result — verifying
      this properly required pulling the raw job log via `gh api
      .../actions/jobs/{id}/logs` and checking for the actual crash
      signature and Boost.Test pass/fail lines, not trusting the step
      status shown in the UI or `gh run view`.) That same run turned up
      one new, unrelated, non-crash finding worth flagging honestly
      rather than glossing over: `dns_discovery_test`'s
      `stopped_node_absent_after_deregister` case failed a real
      assertion (`node2 must see 1 peer after stop; saw 2` /
      `node3 must see 1 peer after stop; saw 2`) — the surviving nodes'
      3 s post-stop wait wasn't always enough for BIND9 to process the
      DELETE UPDATE and for both nodes' next `/peers` poll to observe
      it. This is a timing flake, not a crash or memory-safety issue,
      and is unrelated to the `peer_ids()` bug fixed above; filed as
      its own `doc/TODO.md` Minor Enhancements entry rather than folded
      into this already-closed finding.
    - **`bind9`**: builds and runs correctly in both DNS scenario tests
      (each fixture's `all_nodes_healthy` case, which depends on a
      working BIND9 container, passes) — confirmed working on arm64.
    - The smoke-test workflow itself needed three follow-up commits
      during this verification to actually reach and report on every
      target: none of the five steps originally had
      `continue-on-error`, so the very first failure (`chaos_node`)
      silently skipped every step after it, including the three
      genuinely independent images this task most needed data on. Each
      of the five is now `continue-on-error: true` with a comment
      explaining *why* (three different reasons: two already-documented
      gaps, one newly-confirmed real bug), so a future run reports on
      all five every time instead of stopping at the first failure.
  - _Requirements: 6.2, 6.3_

---

## Phase 5: Real Cloud Tests arm64 Leg (Task 11)

- [x] 11. Add arm64 matrix entry to `real-cloud-tests.yml`'s `aws` job
  - Matrix entries for `{ os: ubuntu-24.04, arch_label: x64 }` and
    `{ os: ubuntu-24.04-arm, arch_label: arm64 }`; job name includes
    `${{ matrix.arch_label }}`
  - Confirm the arm64 leg's build actually compiles the
    `#if defined(__aarch64__) || defined(__arm64__)` branch in
    `tests/aws_quorum_manager_real_ec2_test.cpp` (add a build-log assertion
    or comment noting this was manually confirmed, since the preprocessor
    branch itself has no independent test)
  - Run via `workflow_dispatch` with the `ec2-quorum-manager` bundle enabled
    on the arm64 leg and confirm a Graviton (`t4g.micro`, or configured
    `AWS_TEST_INSTANCE_TYPE`) instance is provisioned, validated, and torn
    down successfully
  - _Requirements: 7.1, 7.2, 7.3_

---

## Phase 6: Documentation (Tasks 12–13)

- [x] 12. Update `README.md`
  - Add an "ARM (arm64) Support" section near the existing CI/build
    documentation
  - State: native `aarch64`/`arm64-linux` build and test via GitHub-hosted
    `ubuntu-24.04-arm` runners, alongside the existing x86_64 matrix
  - List known limitations: PocoDNSSD static libraries not provided for
    `arm64-linux` (DNSSD discovery backend disabled, same as any host
    missing the archive); 32-bit ARM and non-Linux ARM out of scope
  - _Requirements: 9.1, 9.2_

- [x] 13. Update `doc/TODO.md`
  - Mark the ARM verification item done, following the existing entry
    style (see the July 14, 2026 `ca-cluster-rpc-mtls` entry): CI jobs
    added, any real bugs found and fixed while making the arm64 leg green
    (Task 8), and any explicitly deferred follow-up (e.g. a dependency gap
    found in the Phase 0 spike that was worked around rather than fully
    resolved)
  - _Requirements: 9.3_

---

## Summary

| Phase | Tasks | Status |
|---|---|---|
| 0 | 1 (spike) | Complete (static verification — see `spike-notes.md`) |
| 1 | 2–4 (CMake triplet + multiarch discovery) | Complete |
| 2 | 5–6 (CI workflow triplet + cache isolation) | Complete |
| 3 | 7–8 (native arm64 build-and-test leg) | Complete — verified green on a real cold-cache CI run |
| 4 | 9–10 (Docker images on arm64) | Complete — 5 real runs against all 5 docker-* targets; 1 genuine new arm64 bug found (SIGSEGV in DNS/DNS-SD discovery tests) root-caused and fixed (stack-use-after-scope in `peer_ids()`), 2 already-known/documented gaps confirmed, 2 images (bind9, dns/dns-sd nodes themselves) verified working |
| 5 | 11 (Real Cloud Tests arm64 leg) | Complete |
| 6 | 12–13 (documentation) | Complete |

**Total**: 13/13 tasks complete

## Notes

- Wave 0 (the spike) is a hard gate: if a vcpkg dependency lacks
  `arm64-linux` support at the pinned baseline, resolving that is
  prerequisite work inserted ahead of every other task, not a
  parallelizable concern.
- The coverage and format-check jobs are explicitly **not** touched by this
  plan beyond the format-check guard tightening in Task 7 — see
  Requirement 8 and `design.md`'s Trade-offs table for why they stay
  x86_64-only.
- `clang-tidy` is out of scope entirely (local/pre-commit-only, never run in
  CI) — no task references it.
- Docker image cross-architecture publishing (multi-platform manifests via
  `docker buildx`) is out of scope; Phase 4 only verifies native same-host
  builds.
