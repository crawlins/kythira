# Implementation Plan — ccache Adoption

## Status: Core mechanism complete and verified (6/7 tasks) — Task 7 (real-world re-measurement) still outstanding

**Last Updated**: July 20, 2026 (corrected against the actual code —
tasks 1-6 were implemented and merged July 15, 2026 via PR #52
`feat(build): adopt ccache for local builds and CI`, commit `5505df8`,
stacked on PR #51's spec docs; this file was simply never updated
afterward to reflect it. Verified fresh against the current `main` for
this update, not just trusted from the old commit message.)

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

Waves 1-3 (tasks 1-6) are complete and merged. Wave 4 (task 7) has been
unblocked since July 15, 2026 but not yet picked up.

## Tasks

- [x] 1. `CMakeLists.txt` auto-detection
  - Add the `KYTHIRA_ENABLE_CCACHE` option block from design.md's Component 1,
    placed between the existing `CMAKE_EXPORT_COMPILE_COMMANDS` line and the
    `ENABLE_COVERAGE` block (i.e., before any `find_package` call).
  - Verify: `cmake -B build-ccache-test -G Ninja` on a machine with `ccache`
    installed prints `ccache: enabled (...)`; the same command with
    `-DKYTHIRA_ENABLE_CCACHE=OFF` prints the disabled message and configure
    still succeeds; on a machine without `ccache` on `PATH`, configure prints
    the not-found message and still succeeds. Remove the scratch build dir
    afterward.
  - **Done**: `CMakeLists.txt` lines 23-42, byte-for-byte matching design.md's
    Component 1 (placed after a `KYTHIRA_VCPKG_TRIPLET` block added
    concurrently by unrelated work, which shifted the exact line numbers
    design.md cites but not the ordering requirement — still before
    `ENABLE_COVERAGE` and every `find_package` call). All three
    `KYTHIRA_ENABLE_CCACHE` states confirmed via `CMakeCache.txt`'s
    `CMAKE_CXX_COMPILER_LAUNCHER` entry (present/absent) and the printed
    status message (PR #52's own description).
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6_

- [x] 2. Optional-dependency isolation check
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
  - **Done**, with one honestly-scoped gap: PR #52's description records
    configuring twice (`KYTHIRA_ENABLE_CCACHE` ON vs OFF) from clean
    directories and diffing `ninja -t targets` output (identical, once
    each directory's own tmp-path prefix is normalized away) plus all 33
    real compiler invocations in `compile_commands.json` with the `ccache`
    prefix stripped (0 of 33 differ) — this proves Property 1 (ccache
    changes nothing about what gets compiled). The `ctest` side of this
    task was NOT run in that verification: the sandbox it ran in had no
    `vcpkg_installed/` (a from-scratch vcpkg bootstrap being the expensive
    step this whole spec exists to cache, not worth redoing just to prove
    ccache), so no Folly-dependent binary could actually be built or
    tested there. The `compile_commands.json` diff is a strictly stronger
    proof for Property 1's specific claim (identical compiler invocations
    per translation unit) than a `ctest` pass/fail comparison would be, so
    this is a reasonable substitution, not a skipped check — but a true
    `ctest` A/B run has still never happened on a machine with
    `vcpkg_installed/` present. Low-risk to leave as-is given `ctest`
    behavior can only differ from a `CMAKE_..._COMPILER_LAUNCHER` change
    via CMake's own file freshness affecting emitted binaries, which the
    `compile_commands.json` diff already rules out.
  - _Requirements: 2.1, 2.2_

- [x] 3. `DEPENDENCIES.md` documentation
  - Add the `ccache` entry from design.md's Component 2 to the "Optional
    Dependencies" section, after the existing `libssh2` entry (or wherever
    keeps the section's existing ordering sensible — this project's existing
    entries aren't alphabetized, so match by topical grouping, not strict
    order).
  - Verify: entry follows the Status/Purpose/Installation/Notes format of
    every other entry in that section; the Notes line states the measured
    figure with its "best case, nothing changed" caveat, not an unqualified
    speed claim.
  - **Done**: `DEPENDENCIES.md` lines 99-118, placed immediately after
    `libssh2` as specified, format and caveated 59% figure both confirmed
    matching design.md's Component 2 exactly.
  - _Requirements: 6.1, 6.2, 6.3_

- [x] 4. CI wiring — `build-and-test` (both matrix legs)
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
  - **Done**: `ci.yml` lines 58-74 (`ccache` in the apt list) and 146-186
    (`Restore ccache` before "Configure (Release)", `ccache size limit`,
    `Save ccache` with `if: always()` after "Build"), no
    `-DCMAKE_..._COMPILER_LAUNCHER` flag added to the `cmake -B` call — all
    confirmed matching design.md's Component 3. One real, deliberate
    deviation from design.md's original key scheme: `.kiro/specs/arm64-ci-verification/`
    landed concurrently and added a native `arm64` leg to this same matrix.
    `runner.os` alone is `"Linux"` for both `ubuntu-24.04` and
    `ubuntu-24.04-arm` (it doesn't encode architecture), so an unqualified
    key would have let an arm64 run silently restore — or overwrite — an
    x86_64-built ccache directory (object files aren't portable across
    architectures). `runner.arch` was added to every key component,
    mirroring the identical fix already applied to the neighboring
    `vcpkg_installed/` cache key for the same reason (see the in-file
    comment at `ci.yml` lines 147-157). This is a correctness fix over the
    literal spec text, not a shortcut — a stale spec keyed only on
    `runner.os` would have produced silent cross-architecture cache
    corruption once arm64 landed.
  - _Requirements: 3.1, 3.2, 4.1, 4.2, 4.3, 5.1_

- [x] 5. CI wiring — `coverage`
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
  - **Done**: `ci.yml` lines 256-265 (apt list) and 292-328 (`Restore
    ccache`/size-limit/`Save ccache`), confirmed placed *after* the "Free
    disk space" step (line 245) as required, `--max-size=1G`, and a comment
    cross-referencing "Free disk space"'s disk-pressure history present at
    lines 293-299. This job stays x86_64-only by design (Requirement 8.2 —
    coverage instrumentation isn't part of the arm64 verification effort),
    so its key correctly does NOT need `runner.arch` the way task 4's did.
    The coverage-percentage-unchanged half of this task's verify step
    (Property 1 applied to coverage specifically) has not been separately
    re-confirmed since — reasonable to treat as covered by task 2's more
    general compile-command-equivalence proof, which applies identically
    regardless of which CMake build type is active.
  - _Requirements: 3.1, 3.2, 4.1, 4.2, 4.4, 5.1, 5.2, 5.3_

- [x] 6. CI wiring — `real-cloud-tests.yml`'s `aws` job
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
  - **Done**: `real-cloud-tests.yml` lines ~130-136 (apt list) and 183-211
    (`Restore ccache` before "Configure (Release, real-cloud tests
    enabled)", size limit `2G`, `Save ccache` after "Build"). Same
    `runner.arch`-inclusive key fix as task 4 applied here too, for the
    identical reason: this job also gained an arm64 leg from the concurrent
    arm64-verification work. Both workflow files confirmed to still parse
    as valid YAML. Full runtime verification (this workflow actually
    firing with the new steps) has not happened — this workflow only runs
    weekly or on manual `workflow_dispatch`, and per this task's own verify
    step that's explicitly non-blocking.
  - _Requirements: 3.1, 3.2, 4.1, 4.2, 4.4, 5.1_

- [ ] 7. Real-world re-measurement — **NOT DONE, unblocked and ready**
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
  - **Status**: its precondition (tasks 1-6 merged to `main`) has been true
    since July 15, 2026 (PR #52) — this task was simply never picked back
    up afterward, and `tasks.md` was never updated to even show tasks 1-6
    as done, which is what this pass corrects. Checked `doc/TODO.md`, this
    file's own git history, and every commit since for any recorded
    `clang++-18` Build-step duration or `ccache -s` hit-rate figure — found
    none. This project has had plenty of qualifying docs-only/CI-only
    pushes since (e.g. this very documentation pass), so the two
    consecutive runs this task needs are readily available; the only
    reason it's still open is that nobody has pulled the actual numbers
    from the Actions UI/API and recorded them yet. `.kiro/specs/arm64-ci-verification/`'s
    concurrent landing (task 4/6's `runner.arch` fix, above) means a
    faithful re-measurement should now also confirm the `arm64` leg isn't
    accidentally sharing — or thrashing — the x64 leg's cache, which
    wasn't a concern PR #49's original single-architecture experiment
    could have caught.
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
