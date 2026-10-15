# Implementation Plan — mold Linker Adoption

## Status: 5/8 tasks complete

**Last Updated**: October 15, 2026 (task 1 measured; gate waived by the
maintainer; tasks 2-4 and 8 done; task 5 is wired and waits on one
dispatched real-cloud run; tasks 6-7 follow the merge).

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
      "tasks": [4, 5, 8],
      "description": "CI wiring for ci.yml, then the other workflows, with clang-built vcpkg trees for the clang jobs"
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

- [x] 1. Throwaway measurement and equivalence check
  - Ran on this PR's own branch with a throwaway `mold-measure.yml`
    workflow rather than a separate `measure/mold-linker` branch; the
    workflow and its scripts were removed before merge. Each variant was a
    fresh build of the same commit at the same path with sccache warm, then
    every link output deleted and relinked (the "link-only" pass), then the
    slowest link relinked alone under `/usr/bin/time -v` (`--no-fork` for
    mold, which otherwise forks and hides its RSS).
  - **mold could not link the clang++-18 legs as specified.** Against the
    GCC-built vcpkg tree every clang Release link failed: `VTT for
    google::LogMessage::LogStream refers to a discarded COMDAT section
    probably due to an ODR violation`, from `libfolly.a`'s
    `.debug_loclists`. Reproduced locally with glog 0.7.1 and a two-file
    program; identical on mold 2.30.0 (Ubuntu), 2.42.1 and 3.0.0.
    `--ignore-odr-violations` does not exist; `--gdb-index` and
    `-gsplit-dwarf` do not help; `--strip-debug` and `--noinhibit-exec` do,
    but drop debug info or downgrade every error. Building the ports with
    clang removes the mismatch, which became Requirement 9 and task 8.
    The g++-13 arm64 leg and the clang Debug coverage leg linked as is.
  - Round 1, GCC-built vcpkg (run 38628296820, Release legs run the full
    `ctest`, 639/639 pass under every linker):

    | Leg | Variant | Full build wall | Summed link | Link-only pass | Slowest link RSS |
    |---|---|---|---|---|---|
    | clang++-18 x64 | ld.bfd | 228 s | 349.7 s | 91 s | 587 MiB |
    | | ld.bfd + `--strip-debug` | 197 s | 225.9 s | 58 s | 504 MiB |
    | | mold + `--strip-debug` | 185 s | 179.3 s (−20.6% vs strip) | 45 s | 64 MiB |
    | | lld | 184 s | 176.7 s (−49.5%) | 53 s | 671 MiB |
    | g++-13 arm64 | ld.bfd | 194 s | 310.8 s | 100 s | 253 MiB |
    | | mold | 197 s | 306.8 s (−1.3%) | 76 s (−24%) | 6 MiB |
    | | lld | 175 s | 237.3 s (−23.7%) | 68 s (−32%) | 308 MiB |
    | coverage clang++-18 | ld.bfd | 2241 s | 1294.0 s | 327 s | 5235 MiB |
    | | mold | 2195 s | 599.5 s (−53.7%) | 224 s | 65 MiB |

  - Round 2, clang-built vcpkg on the clang legs (run 38648655946, no
    `--strip-debug`; the x64 full builds are cold because every kythira
    compile missed sccache once the vcpkg headers' hashes changed):

    | Leg | Variant | Full build wall | Summed link | Link-only pass | Slowest link RSS |
    |---|---|---|---|---|---|
    | clang++-18 x64 | ld.bfd | 5181 s | 306.2 s | 73 s / 288.7 s summed | 553 MiB |
    | | mold | 5274 s | 223.2 s (−27.1%) | 68 s / 269.1 s summed (−6.8%) | 65 MiB |
    | coverage clang++-18 | ld.bfd | 2245 s | 992.4 s | 229 s | 5072 MiB |
    | | mold | 2161 s | 663.9 s (−33.1%) | 170 s | 66 MiB |

  - Equivalence: `ninja -t targets all` identical in every pair; `ctest`
    pass sets identical on both Release legs; `DT_NEEDED`, PIE, RELRO,
    BIND_NOW, `GNU_STACK` and build-id match on `multi_raft_node`,
    `ca_cluster_node`, `oci_heartbeat_writer_node` and the slowest test, and
    `oci_heartbeat_writer_node` still has no `libatomic.so.1`. The only ELF
    difference under mold is `DF_STATIC_TLS` added to `DT_FLAGS`, which
    records that a static-TLS model is in use and is harmless for
    executables. (lld, not adopted, also dropped the x64
    `NT_GNU_PROPERTY_TYPE_0` note.) `CMakeCache.txt`
    differs only in the four entries Correctness Property 2 names.
    `alt-coap-backends` (Requirement 4.6), TSan (4.7) and coverage totals
    (4.5) are checked by this change's own CI run, where those jobs link
    with mold. That run found one real clash in `alt-coap-backends`:
    `libnyoci.a` and `libcoap-3.a` both define `coap_insert_option`, and
    the libnyoci tests had linked only because ld.bfd scans `libcoap-3.a`
    before anything needs it. mold reported the duplicate; libnyoci is now
    linked into its own tests ahead of `network_simulator` instead of
    through it, which every linker resolves to libnyoci's copy.
  - mold's peak link memory was far below ld.bfd's on every leg, so
    Requirement 1.5 did not trigger and no leg sets `KYTHIRA_LINK_JOBS`.
    Ubuntu 24.04's `mold` 2.30.0 installs and works on x64 and arm64.
  - **Gate: not met** (−27.1% < 30%). Clark waived it on October 15, 2026,
    choosing mold with clang-built vcpkg over lld and over closing the
    spec (Requirement 1.4 revision).
  - _Requirements: 1.1-1.5, 4.1-4.7, 5.4_

- [x] 2. `KYTHIRA_LINKER` and `KYTHIRA_LINK_JOBS` in `CMakeLists.txt`
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

- [x] 3. `DEPENDENCIES.md` entry
  - Add the `mold` entry from design.md Component 2 after `sccache`, citing
    task 1's figure and conditions.
  - _Requirements: 7.1_

- [x] 4. CI wiring in `ci.yml`
  - For `build-and-test`, `gcp-sdk-build`, `ion-serializer-build`,
    `config-variants`, `alt-coap-backends`, `dns-discovery-build`,
    `no-folly`, `coverage`, `tsan` and `future-backend-compat`: add `mold`
    to the apt list, `-DKYTHIRA_LINKER=mold` to every `cmake -B`, and the
    `.comment` assertion step after Build (design.md Component 3).
  - Set `KYTHIRA_LINK_JOBS` only on legs task 1 showed need it, with a comment
    citing the run (Requirement 6.2).
  - Confirm the vcpkg cache step reports a hit with no rebuild on every
    GCC leg (Requirement 3.3), and that the only `LDFLAGS` exported is task
    8's `-latomic` in a clang job's vcpkg step (Requirement 3.2).
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
  - Status: the edits are in. `container-tests.yml` is verified by this
    change's own PR run. The `arm64-docker-smoke-test` dispatch and the AWS
    real-cloud dispatch cost runner time and cloud money respectively, and
    wait for Clark's go-ahead.
  - _Requirements: 5.1-5.3_

- [x] 8. Clang-built vcpkg trees for the clang jobs
  - Per design.md Component 4: `build-and-test`'s clang++-18 legs set
    `vcpkg_cc`/`vcpkg_cxx`; `gcp-sdk-build`, `config-variants`, `no-folly`,
    `coverage`, the clang jobs of `real-cloud-tests.yml` and a clang
    dispatch of `coap-flake-measure.yml` set the same inline. Each adds
    `-clang++-18` to its cache key and exports the `-latomic` `LDFLAGS` in
    its vcpkg step only. Timeouts of the `ci.yml` jobs rise to 300 minutes
    for the first cold build.
  - Verify on this change's CI: every clang job's vcpkg step either
    restores the clang tree or builds it and links proxygen's samples, and
    every clang job's `.comment` check names mold.
  - _Requirements: 9.1-9.5, 3.2_

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
