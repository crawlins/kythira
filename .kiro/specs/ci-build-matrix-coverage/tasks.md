# Implementation Plan — CI Build-Matrix Coverage

## Status: In progress — 11/12 tasks

**Last Updated**: October 8, 2026 (Tasks 4, 5 and 12 done: the
`alt-coap-backends` job runs all 72 `coap_*` tests with both alternate
backends compiled in. Task 11 waits only on that job's first `main` run).

## Overview

Add five CI jobs (`kconfig-check`, `alt-coap-backends`, `no-folly`,
`config-variants`, `static-analysis`) that build the configurations this
repository claims to support but never builds, and fix whatever they find.
Each job lands green or not at all (Requirement 8). Phase 0 measures before
anything is added, because two of the jobs' shapes (the clang-tidy scope and
the Folly-free target set) depend on numbers nobody has yet.

## Task Dependency Graph

```json
{
  "1": [],
  "2": [],
  "3": ["1"],
  "4": ["1"],
  "5": ["4"],
  "6": ["1"],
  "7": ["6"],
  "8": ["1"],
  "9": ["1"],
  "10": ["9"],
  "11": ["3", "5", "7", "8", "10"],
  "12": ["11"]
}
```

## Tasks

## Phase 0: Measure (Task 1)

- [x] 1. Measure each configuration locally before touching CI
  - On a machine with a full vcpkg tree (Clark's, or a sandbox where
    `vcpkg install` works), record for each of the following: does it
    configure, does it build, which tests fail, and how long a cold and a
    sccache-warm build take.
    - `ci_full_defconfig` plus both alternate CoAP symbols, with
      `coap-cantcoap` and `coap-libnyoci` installed.
    - `CONFIG_FOLLY=n`, `CONFIG_PROXYGEN_TRANSPORT=n`, Boost default backend,
      against a hand-made Folly-free prefix.
    - `minimal_defconfig` (strict) and `no_cloud_defconfig` (non-strict).
    - `scripts/verify-optional-dependency-isolation.sh`.
    - `cmake --build build --target static-analysis` on a `ci_full_defconfig`
      clang++-18 tree, with `cmd/` added to `TIDY_SOURCES`: wall time and
      finding count.
  - List the code-generation targets a clang-tidy run needs built first.
  - Record results in this file under Notes; they size every later task.
  - Superseded, not done as written: no sandbox here has the vcpkg tree,
    so each configuration was measured as its job landed instead. The
    Folly-free build by a local configure plus `clang -M -MG` (Tasks 6-7),
    the clang-tidy scope and codegen prerequisites by a temporary CI
    workflow (Tasks 9-10), and `minimal_defconfig`, `no_cloud_defconfig`
    and the isolation script by the `config-variants` job's own PR runs
    (Task 8). The alternate CoAP configuration is measured by PR #463's
    runs (Tasks 4-5). See Notes and Task 11 for the figures.
  - _Requirements: 3.5, 7.3, 8.1_

## Phase 1: Cheap gates (Tasks 2–3)

- [x] 2. Add the `kconfig-check` job
  - Add `scripts/kconfig/check_defconfig_usage.py` (copyright header,
    `#!` first) and an empty `configs/defconfig-usage-allowlist.txt`.
  - Add the job per design §1; no vcpkg, `timeout-minutes: 10`.
  - Negative check (recorded in the PR, not committed): a renamed Kconfig
    symbol and an orphan defconfig each fail the job.
  - Until Tasks 4, 6 and 8 land, `minimal_defconfig` and
    `no_cloud_defconfig` are in the allowlist with "applied by
    config-variants once Task 8 lands"; Task 8 removes them.
  - Done. `check_defconfigs.py` also needed a fix the design had not
    expected: Kconfiglib leaves `warn_assign_undef` off by default, so a
    defconfig assigning a removed symbol loaded with no warning and the
    check passed. It now turns that on, plus `warn_assign_override` and
    `warn_assign_redun`; every checked-in defconfig is clean under all three.
  - _Requirements: 4.1, 4.2, 4.3_

- [x] 3. Add `cmd/` to `TIDY_SOURCES` and `FORMAT_SOURCES`
  - Edit both globs in the root `CMakeLists.txt`.
  - Run `format` with the pinned clang-format 22.1.5 and commit the result
    in the same change.
  - Done before tasks 9-10 picked it up: both globs on `main` already
    include `cmd/`.
  - _Requirements: 3.3_

## Phase 2: Alternate CoAP backends (Tasks 4–5)

- [x] 4. Add `configs/ci_alt_coap_defconfig`
  - `ci_full_defconfig` plus the two alternate-backend symbols, with a
    header stating the keep-in-sync rule.
  - Update `ci_full_defconfig`'s note on the two symbols to point at the new
    job.
  - _Requirements: 1.1, 1.2_

- [x] 5. Add the `alt-coap-backends` job
  - Setup block copied from `ion-serializer-build`, vcpkg features
    `edhoc coap-cantcoap coap-libnyoci`, vcpkg cache key includes the
    feature set.
  - Assert both symbols in `autoconf.hpp`; build the `coap_*` test targets;
    run `ctest -R '^coap_'`; `check-test-run.sh --floor`; the stub-case
    `--list_content` check per design §2.
  - Fix or exclude (with a `doc/TODO.md` entry) each failing test per
    Requirement 8.1.
  - Negative check: dropping `coap-libnyoci` from the install fails the
    stub-case check.
  - Done. Neither feature could ever have been installed from the manifest:
    `vcpkg-overlays/cantcoap` and `vcpkg-overlays/libnyoci` were missing
    from `vcpkg-configuration.json`'s `overlay-ports`, so `vcpkg install
    --x-feature=coap-cantcoap` failed with "the baseline does not contain
    an entry". Once installed, libnyoci's relocatable `.pc` paths (under
    `vcpkg_installed/`, inside the source tree) on `network_simulator`'s
    INTERFACE properties failed the `install(EXPORT)` at generate time;
    the root `CMakeLists.txt` now links `PkgConfig::LIBNYOCI` instead.
  - The `autoconf.hpp` assertion became `scripts/check-coap-alt-backends.sh
    configured`, which reads `generated/autoconf.cmake` and the compile
    commands: `autoconf.hpp` carries no macro for either backend, and
    `*_AVAILABLE` comes from CMake, not Kconfig.
  - The build list maps each `coap_*` test to its ninja target: the
    `coap_*_example` tests are named `<target>_test`, and
    `ctest --show-only=json-v1` omits the command of an unbuilt test.
  - First green run: 72 `coap_*` tests, no failures and no first-attempt
    failures, so nothing was excluded under Requirement 8.1. The floor is
    72. The stub-case negative check was run against a synthetic build
    tree; on the real job, dropping a feature fails the strict configure
    before the stub check is reached.
  - _Requirements: 1.1, 1.3, 1.4, 1.5, 1.6, 7.1–7.5, 8.1, 8.2_

## Phase 3: Folly-free build (Tasks 6–7)

- [x] 6. Add `scripts/make-folly-free-prefix.sh` and
  `configs/ci_no_folly_defconfig`
  - Script per design §3: `depend-info`-derived port set, `cp -al`, delete
    from `info/*.list`, assert no Folly file survives.
  - Defconfig per design §3.
  - Run it against Task 1's tree and confirm a strict configure succeeds.
  - _Requirements: 2.1, 2.2_

- [x] 7. Add the `no-folly` job
  - Build everything the configuration enables; full ctest with the
    standard label exclusion; `check-test-run.sh --floor`.
  - `folly_FOUND` and `build.ninja` assertions.
  - Disabled-test diff against a configure-only `ci_full_defconfig` tree;
    add `configs/no-folly-test-allowlist.txt` with a reason per entry.
  - Fix each Folly leak in backend-independent code the build exposes, or
    exclude it with a `doc/TODO.md` entry per Requirement 8.1.
  - Correct the root `CMakeLists.txt` comment above
    `kythira_find_optional(FOLLY folly)` and any other claim the job
    disproves.
  - Negative check: a stray `#include <folly/Unit.h>` in a
    backend-independent header fails the job.
  - _Requirements: 2.3, 2.4, 2.5, 7.1–7.5, 8.1–8.3_

## Phase 4: Remaining defconfigs and isolation (Task 8)

- [x] 8. Add the `config-variants` job
  - Three steps per design §4, separate build directories.
  - Add the optional `KYTHIRA_PREFIX_PATH` override to
    `verify-optional-dependency-isolation.sh`.
  - Remove `minimal_defconfig` and `no_cloud_defconfig` from the usage
    allowlist.
  - Done. What the job found, fixed under Requirement 8.1:
    `minimal_defconfig` left GCP and Azure on (strict configure failed on
    `CONFIG_GCP_SDK`); `CONFIG_AZURE_SDK=n` did not stop
    `azure-identity-cpp` pulling the SDK back in; and
    `CONFIG_HTTP_TRANSPORT_TLS=n` broke `ca_cluster_node`, ten TLS tests
    and the ACME client, which now skip or compile without TLS
    (`KYTHIRA_HTTP_TLS` in the root `CMakeLists.txt`).
  - _Requirements: 5.1, 5.2, 5.3, 6.1, 6.2, 7.4, 7.5_

## Phase 5: clang-tidy gate (Tasks 9–10)

- [x] 9. Bring the tree to zero findings
  - Using Task 1's run (with `cmd/` included), fix each finding or add a
    `NOLINT(<check>)` with a reason. Never disable a check globally to get
    to zero without recording why in `.clang-tidy`'s rationale block.
  - Done. Measured in CI rather than via Task 1 (see Notes): 873 findings.
    About 620 fixed by clang-tidy's fix-its, the rest by hand or a
    justified `NOLINT`; four checks disabled with reasons in `.clang-tidy`.
  - _Requirements: 3.4, 8.1_

- [x] 10. Add the `static-analysis` job
  - Install `clang-tidy-18`; configure `ci_full_defconfig` with clang++-18;
    build the codegen targets from Task 1 (or the whole tree if that proves
    brittle); run `static-analysis`.
  - If Task 1's full-tree time exceeds 60 minutes, add the diff-scoped
    `static-analysis-files` path for pull requests per design §5, and keep
    the full run on pushes to `main`.
  - Negative check: an introduced `bugprone-*` finding fails the job.
  - Done, sharded six ways instead of diff-scoped (see Notes). Codegen
    targets: `scripts/build-tidy-prerequisites.sh` builds every
    CUSTOM_COMMAND output (protoc) and every `.pch`; nothing else is needed.
    Negative check passed: an `if`/`else` with identical branches appended to
    a shard's first file made `clang-tidy-18` exit non-zero with
    `bugprone-branch-clone`.
  - _Requirements: 3.1, 3.2, 3.4, 3.5, 7.1–7.3_

## Phase 6: Close-out (Tasks 11–12)

- [ ] 11. Verify every new job green on the PR and on `main`
  - One green run of each job on the PR head and the first `main` push
    after merge. Record warm and cold durations in each job's
    `timeout-minutes` comment.
  - Done for four of the five jobs. `alt-coap-backends` is green on its
    PR (#463: 13.4 min with a warm vcpkg cache and a cold sccache, run
    [37396880148](https://github.com/crawlins/kythira/actions/runs/37396880148));
    its first `main` run and duration are still to record. Each landed through a PR whose head was green (#458
    `kconfig-check`, #479 `no-folly`, #478 `config-variants`, #491
    `static-analysis`), and all four were green on `main` in runs
    [37567880231](https://github.com/crawlins/kythira/actions/runs/37567880231)
    (`b7e7e01`) and
    [37584489048](https://github.com/crawlins/kythira/actions/runs/37584489048)
    (`2faddb0`). Durations, now in each `timeout-minutes` comment:

    | Job | Warm sccache | After a wide header change | Timeout |
    |---|---|---|---|
    | `kconfig-check` | 0.1 min | 0.1 min | 10 |
    | `no-folly` | 12.8 min | 63.8 min | 180 |
    | `config-variants` | 9.6 min | 39.4 min | 240 |
    | `static-analysis` (6 shards) | 28.9-45.2 min | 21.4-40.5 min | 90 |

    `2faddb0` changed headers included by most translation units, so its
    run recompiled most of the tree; that is the slowest case `main` has
    shown. No fully cold run (empty sccache) of these jobs has been seen
    on `main`, so `config-variants` keeps its cold-build sizing.
    `static-analysis` does not compile, so it has no warm/cold split.
  - _Requirements: 7.3, 8.2_

- [x] 12. Update stale specs and docs
  - kconfig-integration tasks/status: Req 5.4 now enforced.
  - clang-tidy tasks: task 4's "zero findings" now gated in CI.
  - coap-transport-cantcoap task 8 and coap-transport-libnyoci task 6:
    note the suites now run in CI.
  - `doc/TODO.md`: close the items this spec resolves and add any
    exclusions made under Requirement 8.1.
  - Done. The coap-transport-cantcoap and coap-transport-libnyoci notes
    landed with the `alt-coap-backends` job (PR #463). kconfig-integration
    and clang-tidy carry their notes; `doc/TODO.md`'s row and
    bookkeeping list are current; Requirement 8.1's exclusions are
    the no-folly allowlist's over-wide gates, which `doc/TODO.md`
    already tracks; Task 8 fixed what it found rather than excluding it. The root
    `CMakeLists.txt` comment above `kythira_find_optional(FOLLY folly)`
    and the `CONFIG_FOLLY` help text already describe the no-folly job.
  - _Requirements: 8.3_

## Notes

- **Tasks 9-10 (clang-tidy gate).** Measured in CI on 2026-10-06 with a
  temporary workflow, since no sandbox here has the vcpkg tree. One
  4-core runner took 9111 s (about 2.5 hours) for the full `ci_full_defconfig`
  tree; the generated-file prerequisites took seconds. Six round-robin
  shards (`KYTHIRA_TIDY_SHARD_COUNT`/`_INDEX`) took 1625-2395 s each, so
  every event analyses the full tree and Requirement 3.5's diff-scoped path
  was not built: a change to a widely included header reaches most
  translation units anyway. The target had never been runnable: its
  `cmake/check_compdb.cmake` helper was matched by `.gitignore`'s `*.cmake`
  and never committed. Findings: 873 unique, 340 braces, 146 implicit bool
  conversions, 66 `[[nodiscard]]`, then a long tail.

- **Tasks 6-7 (PR #479).** Measured locally rather than via Task 1: a
  Folly-free configure plus `clang -M -MG` over every translation unit
  found 17 registered tests including Folly headers unguarded (3 wrapper
  property tests, 14 CoAP tests naming `folly::Executor`); all fixed. The
  Folly-free tree then built and its suite ran with no Folly-related
  failure. First green CI run (2026-10-05): the pruned prefix and strict
  configure worked first time, CI named 29 more dropped tests than the
  local configure (stdexec, gRPC, proxygen), and the suite ran 388/388
  tests in about 8 minutes; the floor is 380. See design section 3, "As built", for where the
  implementation departs from the design.

- **Overlap with open PRs.** #389 adds a step to
  `arm64-docker-smoke-test.yml` and #392 changes CoAP client code that the
  `alt-coap-backends` job will start compiling. Neither touches the jobs
  this spec adds, but whichever lands second rebases.
- **Out of scope** (Requirement 9): libldns/DNS discovery in CI, per-PR
  Docker scenarios and a Podman job, compiler caches for the manual
  workflows, `future_backend_benchmark_test`, and header-level decoupling
  of the HTTP/CoAP transports from Folly.
