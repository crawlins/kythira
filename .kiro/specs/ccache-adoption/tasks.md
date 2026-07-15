# Implementation Plan — ccache Adoption

## Status: Not Started

**Last Updated**: July 15, 2026

## Overview

Wire ccache into the CMake build (auto-detected, opt-out available) and into
every CI job that compiles this project's own code, persisting each job's
ccache directory across runs so the measured ~59% best-case speedup (PR #49,
closed without merging — a throwaway experiment, not part of this codebase)
actually materializes in practice rather than staying cold every run.

Reference material to read before starting:
- `.kiro/specs/ccache-adoption/design.md` — exact `CMakeLists.txt` snippet,
  exact workflow YAML for each of the three build configs, and the cache-key
  scheme.
- `CMakeLists.txt` lines 1-33 — where `ENABLE_COVERAGE`'s option block lives
  today; the ccache block goes immediately above it.
- `DEPENDENCIES.md` lines 50-103 — the existing "Optional Dependencies"
  section format to match exactly.
- `.github/workflows/ci.yml` — `build-and-test` (lines 17-134) and
  `coverage` (lines 138-378) jobs.
- `.github/workflows/real-cloud-tests.yml` — the `aws` job.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1],
      "description": "CMakeLists.txt auto-detection — nothing else can be meaningfully verified without this existing first"
    },
    {
      "wave": 2,
      "tasks": [2, 3],
      "description": "Optional-dependency isolation check and documentation — both only need task 1"
    },
    {
      "wave": 3,
      "tasks": [4, 5, 6],
      "description": "CI wiring for the three build configs — independent of each other, each depends only on task 1 existing on the branch"
    },
    {
      "wave": 4,
      "tasks": [7],
      "description": "Real-world re-measurement — depends on wave 3 being merged and two real pushes happening"
    }
  ]
}
```

## Tasks

- [ ] 1. `CMakeLists.txt` auto-detection
  - Add the `KYTHIRA_ENABLE_CCACHE` option block from design.md's Component 1,
    placed between the existing `CMAKE_EXPORT_COMPILE_COMMANDS` line and the
    `ENABLE_COVERAGE` block (i.e., before any `find_package` call).
  - Verify: `cmake -B build-ccache-test -G Ninja` on a machine with `ccache`
    installed prints `ccache: enabled (...)`; the same command with
    `-DKYTHIRA_ENABLE_CCACHE=OFF` prints the disabled message and configure
    still succeeds; on a machine without `ccache` on `PATH`, configure prints
    the not-found message and still succeeds. Remove the scratch build dir
    afterward.
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6_

- [ ] 2. Optional-dependency isolation check
  - Configure and build twice from clean build directories (`-DKYTHIRA_ENABLE_CCACHE=OFF`
    and `=ON`, `ccache` installed) per design.md's Testing Strategy item 1.
  - Diff `ninja -C build -t targets` (or equivalent) output between the two
    builds — confirm identical target lists.
  - Run `ctest` against both and confirm the same pass/fail set (a subset
    run is fine — this is checking build-graph/behavior equivalence, not
    re-running the full suite twice for its own sake).
  - This is a one-time manual verification, not a new permanent CTest
    target — record the result (pass/fail, and which two commands were
    compared) directly in this task's checkbox commit, not as a new test file.
  - _Requirements: 2.1, 2.2_

- [ ] 3. `DEPENDENCIES.md` documentation
  - Add the `ccache` entry from design.md's Component 2 to the "Optional
    Dependencies" section, after the existing `libssh2` entry (or wherever
    keeps the section's existing ordering sensible — this project's existing
    entries aren't alphabetized, so match by topical grouping, not strict
    order).
  - Verify: entry follows the Status/Purpose/Installation/Notes format of
    every other entry in that section; the Notes line states the measured
    figure with its "best case, nothing changed" caveat, not an unqualified
    speed claim.
  - _Requirements: 6.1, 6.2, 6.3_

- [ ] 4. CI wiring — `build-and-test` (both matrix legs)
  - Add `ccache` to "Install system dependencies"'s apt package list
    (`ci.yml` line ~32-40).
  - Add the `Restore ccache` / `ccache size limit` steps before "Configure
    (Release)", and the `Save ccache` step (`if: always()`) after "Build",
    per design.md's Component 3 — exact key scheme:
    `ccache-${{ runner.os }}-${{ matrix.compiler }}-${{ github.ref_name }}-${{ github.run_id }}`
    with the two-level `restore-keys` fallback shown in design.md.
  - Do NOT add any `-DCMAKE_..._COMPILER_LAUNCHER` flag to the `cmake -B`
    invocation — task 1's auto-detection already covers it.
  - Verify: `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml'))"`
    succeeds; a pushed commit's `build-and-test` job log shows a "Restore
    ccache"/"Save ccache" step pair for each matrix leg and `ccache -s`-style
    activity is implied by the Build step's `ninja` output changing shape
    (fewer/faster compile lines) on a second, unchanged-source push — full
    confirmation is task 7, this task's own verify step only needs to show
    the steps ran without error on the first (necessarily cold) push.
  - _Requirements: 3.1, 3.2, 4.1, 4.2, 4.3, 5.1_

- [ ] 5. CI wiring — `coverage`
  - Same shape as task 4, applied to `coverage`'s "Install system
    dependencies" (`ci.yml` line ~171) and around "Configure (Coverage)"/
    "Build (Coverage)" (lines ~204-213).
  - Key prefix `ccache-coverage-${{ runner.os }}-${{ github.ref_name }}-`,
    `--max-size=1G` (not `2G` — this job's known disk pressure).
  - Place the "Restore ccache" step after the existing "Free disk space"
    step (line ~160), not before — add a comment cross-referencing that
    step's "No space left on device" history comment.
  - Verify: same as task 4's verify step, applied to the `coverage` job; in
    addition, confirm the coverage percentage reported by "Measure coverage"
    is unchanged from the pre-ccache baseline on the same commit (Property 1
    — ccache must not perturb coverage instrumentation output).
  - _Requirements: 3.1, 3.2, 4.1, 4.2, 4.4, 5.1, 5.2, 5.3_

- [ ] 6. CI wiring — `real-cloud-tests.yml`'s `aws` job
  - Same shape as task 4, applied to the `aws` job's "Install system
    dependencies" step and around its "Configure (Release, real-cloud tests
    enabled)"/"Build" steps.
  - Key prefix `ccache-realcloud-${{ runner.os }}-${{ github.ref_name }}-`,
    `--max-size=2G`.
  - Lowest priority of the three CI tasks — this workflow only runs weekly/
    on-demand, so it's fine to land after tasks 4-5 if sequencing matters for
    review size; the mechanism is identical, no new design questions.
  - Verify: `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/real-cloud-tests.yml'))"`
    succeeds; steps present in the workflow file. Full runtime verification
    requires this workflow actually firing (weekly cron or manual
    `workflow_dispatch` with the `ami-build`/other bundles left off) — not
    blocking for merging this task, since the job's own existing gating
    (AWS credentials, toggles) is unrelated to and unaffected by this change.
  - _Requirements: 3.1, 3.2, 4.1, 4.2, 4.4, 5.1_

- [ ] 7. Real-world re-measurement
  - After tasks 1-6 are merged to `main`, on the first two consecutive
    pushes to any branch that don't touch `include/`, `src/`, `cmd/`,
    `tests/`, or `examples/` (e.g. another docs-only or CI-only PR, which
    this project produces often — see PR #44/#45/#48's own history),
    record `build-and-test`'s `clang++-18` leg's "Build" step duration on
    each of the two runs.
  - Add the result to `doc/TODO.md`'s "What Changed" log (or this file, if
    preferred) alongside the original PR #49 baseline (29m07s cold /
    11m59s warm) — either confirming a comparable warm-build duration or
    flagging a discrepancy (most likely cause: a `restore-keys` mismatch
    per design.md's Property 2) to investigate.
  - This task is observational, not a hard gate — per Requirement 7.3, do
    not add a CI assertion that fails the build based on timing.
  - _Requirements: 7.1, 7.2, 7.3_

## Notes

- This spec deliberately touches no C++ source — `CMakeLists.txt` and CI
  YAML only. There is no new library code, no new test binary, and no
  `vcpkg.json` change.
- The `packer-ca-cluster-node`/`ami-build`/`docs` jobs are explicitly
  untouched (Requirement 8.3) — none of them run `cmake --build` against
  this repository.
- Docker/Packer builder-stage caching (`docker/ca_cluster_node/Dockerfile`)
  and a remote/shared ccache backend are explicitly out of scope
  (Requirement 8.1-8.2) — candidate follow-up specs, not part of this one.
- If a future contributor is tempted to raise `--max-size` on the
  `coverage` job specifically, re-read that job's "Free disk space" step's
  comment first — the two are budgeting against the same constrained disk,
  and this project has hit "No space left on device" there twice already.
