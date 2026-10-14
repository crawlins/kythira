# Implementation Plan — mold Linker Adoption

## Status: 0/7 tasks complete

**Last Updated**: October 9, 2026 (spec written; nothing implemented).

## Overview

Measure mold against ld.bfd on this repository's CI, and only if it clears
Requirement 1.4's bar, add a `KYTHIRA_LINKER` knob to the root
`CMakeLists.txt`, request mold by name in every linking CI job, and guard
against the silent no-op with a `.comment` check.

Reference material to read before starting:
- `.kiro/specs/mold-linker/design.md` — the CMake block, the job pool, the CI
  assertion step.
- `CMakeLists.txt`, the `KYTHIRA_COMPILER_LAUNCHER` block — the shape to
  follow and the reason explicit values are fatal when missing.
- `cmd/oci_heartbeat_writer/CMakeLists.txt` — the `--as-needed` /
  `libatomic.a` ordering that must survive (Requirement 4.4).
- `.kiro/specs/oci-build-cache/tasks.md` task 1 — how a throwaway CI
  measurement was run and recorded here before.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1],
      "description": "Measurement and equivalence on a throwaway branch; go/no-go gate for everything else"
    },
    {
      "wave": 2,
      "tasks": [2, 3],
      "description": "CMake knob and link pool, then docs; both land in one PR"
    },
    {
      "wave": 3,
      "tasks": [4, 5],
      "description": "CI wiring for ci.yml, then the other workflows"
    },
    {
      "wave": 4,
      "tasks": [6, 7],
      "description": "Post-merge confirmation and the two-week soak that unblocks the Docker follow-up"
    }
  ]
}
```

## Tasks

- [ ] 1. Throwaway measurement and equivalence check
  - Branch `measure/mold-linker`, draft PR, closed when done. Per design.md
    Component 0: on `Build & Test (clang++-18, x64)`,
    `Build & Test (g++-13, arm64)` and `Coverage (clang++-18)`, build the same
    commit with the default linker and with `-fuse-ld=mold`, sccache warm.
  - Record per leg: Build step wall clock, summed link-step time and count
    from `.ninja_log`, the five slowest links, peak RSS of the slowest link
    under each linker.
  - In the same runs, collect the equivalence evidence: `ninja -t targets all`
    diff; `readelf -d -l -n` on `multi_raft_node`, `ca_cluster_node`,
    `oci_heartbeat_writer_node` and the slowest test binary; `ctest` pass/fail
    set; coverage totals. Run the `alt-coap-backends` leg with mold once
    (Requirement 4.6) and the TSan job once (Requirement 4.7).
  - Confirm Ubuntu 24.04's `mold` package installs and works on both x64 and
    arm64 runners, and record its version.
  - If peak link memory under mold is more than 25% above ld.bfd on any leg,
    also run the stdexec `future-backend-compat` leg and measure with the
    link pool at 1 and 2 (Requirement 1.5).
  - Write the numbers here. If summed link time on the clang++-18 x64 leg
    drops by less than 30%, mark tasks 2-7 not applicable and close the spec.
  - _Requirements: 1.1-1.5, 4.1-4.7, 5.4_

- [ ] 2. `KYTHIRA_LINKER` and `KYTHIRA_LINK_JOBS` in `CMakeLists.txt`
  - Add design.md Component 1's block after the `KYTHIRA_COMPILER_LAUNCHER`
    block, and the `KYTHIRA_LINK_JOBS` pool beside it.
  - Verify with a scratch CMake project (no vcpkg needed): every
    `KYTHIRA_LINKER` value with mold on and off `PATH`; the status line;
    `build.ninja` has `-fuse-ld=mold` exactly when expected; the
    install-mold-then-reconfigure case switches to mold; an invalid value and
    a missing explicit linker are fatal; `KYTHIRA_LINK_JOBS=2` puts link edges
    in a `kythira_link` pool and `KYTHIRA_LINK_JOBS=abc` is fatal.
  - Verify Correctness Property 2 on the real tree: diff `CMakeCache.txt`
    between `-DKYTHIRA_LINKER=mold` and `=default` configures.
  - _Requirements: 2.1-2.7, 3.1, 3.4, 6.1, 6.3, 7.2_

- [ ] 3. `DEPENDENCIES.md` entry
  - Add the `mold` entry from design.md Component 2 after `sccache`, citing
    task 1's figure and conditions.
  - _Requirements: 7.1_

- [ ] 4. CI wiring in `ci.yml`
  - For `build-and-test`, `gcp-sdk-build`, `ion-serializer-build`,
    `config-variants`, `alt-coap-backends`, `dns-discovery-build`,
    `no-folly`, `coverage`, `tsan` and `future-backend-compat`: add `mold`
    to the apt list, `-DKYTHIRA_LINKER=mold` to every `cmake -B`, and the
    `.comment` assertion step after Build (design.md Component 3).
  - Set `KYTHIRA_LINK_JOBS` only on legs task 1 showed need it, with a comment
    citing the run (Requirement 6.2).
  - Confirm the vcpkg cache step reports a hit with no rebuild on every leg
    (Requirement 3.3), and that no step exports `LDFLAGS` (Requirement 3.2).
  - Do not touch `static-analysis`, `kconfig-check`, `packer-ca-cluster-node`,
    `workflow-input-limits`, `aws-ci-policies`, `docs` or `docs-deploy`.
  - _Requirements: 3.2, 3.3, 5.1-5.3, 6.2_

- [ ] 5. CI wiring in the other workflows
  - Same three edits in `container-tests.yml`, `arm64-docker-smoke-test.yml`,
    `perf-cloud.yml`, `coap-flake-measure.yml` and the linking jobs of
    `real-cloud-tests.yml` (not `ami-build`).
  - Verify `container-tests.yml` on its PR trigger, `arm64-docker-smoke-test`
    by dispatch, and one dispatched run of the cheapest AWS real-cloud leg;
    the remaining real-cloud and perf jobs pick it up on their next scheduled
    run, which task 6 checks.
  - _Requirements: 5.1-5.3_

- [ ] 6. Post-merge confirmation
  - On the first two `main` runs after tasks 4-5 merge, read the clang++-18
    x64 Build step duration and summed link time and record them next to task
    1's numbers, as confirmation or a discrepancy to chase.
  - Check the first scheduled run of each `real-cloud-tests.yml` and
    `perf-cloud.yml` job passed its `.comment` assertion.
  - Informational only; no timing assertion is added to CI.
  - _Requirements: 1.3, 5.3_

- [ ] 7. Soak and Docker follow-up decision
  - Two weeks after task 6, list any CI failure attributed to the linker. If
    there are none, open a follow-up spec or task for installing mold in the
    `docker/ca_cluster_node` and `docker/ca_service` builder stages
    (Requirement 8.1); if there are, record them here and leave the builders
    on ld.bfd.
  - _Requirements: 8.1_
