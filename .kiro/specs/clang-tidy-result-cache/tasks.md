# Implementation Plan — clang-tidy Result Cache and Shard Balancing

## Status: 0/9 tasks complete

**Last Updated**: November 6, 2026 (spec written; Requirement 4 amended the
same day so every same-repository branch writes, not only `main`).

## Overview

Measure a clean-result cache for clang-tidy on this repository first, then
balance the shards by measured cost, build the cache driver and its key,
wire both into `ci.yml`, and add a weekly uncached audit that proves the
cache never hides a finding.

Reference material to read before starting:
- `.kiro/specs/clang-tidy-result-cache/design.md` — the key, the driver, the
  planner, the CI wiring.
- `CMakeLists.txt`, the "clang-tidy targets" block — today's round-robin
  sharding and the `static-analysis` command line that `off` must keep.
- `.github/workflows/ci.yml`, the `static-analysis` job, and
  `.github/actions/oci-build-cache/action.yml` — where the credentials and
  the read-only/read-write split come from.
- `scripts/oci-build-cache/provision.sh` and `audit.sh` — the bucket's
  lifecycle rules and the audit's prefix list.
- `.kiro/specs/mold-linker/tasks.md` task 1 — how a throwaway CI measurement
  was run and recorded here before.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1],
      "description": "Throwaway measurement: hit rate, key overhead, zero false cleans, per-file costs. Gate for tasks 3-9"
    },
    {
      "wave": 2,
      "tasks": [2, 3],
      "description": "Shard planner with task 1's costs (useful on its own), and the key library with its unit tests"
    },
    {
      "wave": 3,
      "tasks": [4, 5],
      "description": "Cache driver and the CMake knob"
    },
    {
      "wave": 4,
      "tasks": [7, 6, 8],
      "description": "Lifecycle rule (needs the maintainer's go) and CI wiring, which needs the shared PR-only key to exist, and the weekly uncached audit"
    },
    {
      "wave": 5,
      "tasks": [9],
      "description": "Docs and post-merge confirmation on main"
    }
  ]
}
```

## Tasks

- [ ] 1. Throwaway measurement and correctness check
  - Prototype the key (design "Key computation") and a `local:` backend in a
    throwaway script; no CMake or `ci.yml` changes.
  - Add a throwaway dispatch-only `tidy-cache-measure.yml` on a scratch
    branch with a draft PR, split into 4 jobs of 5 commits each over the last
    20 commits on `main`, cache directory carried between jobs with
    `actions/cache`.
  - Per commit: TUs, hits, misses, key seconds, analysis seconds; for every
    hit, a fresh clang-tidy run compared with the cached clean.
  - Uncached full-tree runs on the first and last commit, recording per-TU
    file cost and the dependency-scan cost per TU.
  - Record results and run IDs here. Gate (Requirement 1.6): zero false
    cleans and warm median at most 30% of uncached. If missed, stop and ask
    the maintainer: fix the key, adopt anyway, or close.
  - Close the draft PR and delete the scratch branch.
  - _Requirements: 1.1-1.6_

- [ ] 2. Cost-weighted shard planner
  - `scripts/tidy-shard.py` (plan and `--refresh` modes) per design
    Component 1, with its own unit test (LPT order, ties, median for unknown
    files, determinism, `--count 1` keeps every file).
  - `configs/tidy-file-costs.txt` from task 1's uncached costs, with the
    copyright header and the source run ID in its comment header.
  - `CMakeLists.txt`: call the planner at configure, read the list back,
    keep the index range check, add the costs file to
    `CMAKE_CONFIGURE_DEPENDS`, fall back to round robin with a warning when
    Python is absent.
  - `scripts/check-build-config.py`: assert the planner's log line in CI.
  - Verify on a CI run: the six shards' wall times, slowest at most 1.15x the
    fastest's prediction (Requirement 8.3); record the run ID.
  - Can merge before task 1's gate is decided if task 1 produced the costs.
  - _Requirements: 8.1-8.3, 8.5_

- [ ] 3. Cache key library and its unit tests
  - The key computation as a module inside `scripts/tidy-cache.py` (or a
    sibling `scripts/tidy_cache_key.py`): version string, tool version and
    binary hash, per-directory `--dump-config`, normalised command, `-M`
    dependency scan with the PCH rewrite, raw-byte hashes memoised by
    `(path, size, mtime_ns)`.
  - Unit tests with a tiny fixture tree and compile database, one case per
    input in Requirement 2.1 changing only that input: a `NOLINT` removal,
    a header reached only through the PCH, a nested `.clang-tidy`, a `-D`
    change, a system-header content change, a workspace path change (key
    must NOT change), and a failed scan (no key, treated as miss).
  - Register as a ctest (label `unit`, needs clang-18) and add to
    `configs/no-folly-test-allowlist.txt` if it only registers in the folly
    block.
  - _Requirements: 2.1-2.7_

- [ ] 4. Cache driver
  - `scripts/tidy-cache.py run` per design Component 2: thread pool,
    `HEAD`/`PUT` against the S3-compatible endpoint with standard-library
    SigV4, `local:` backend, clean-only stores, `== <tu>` output blocks,
    "cached: <tu>" lines, non-zero exit on any finding.
  - Fail open: 10 s timeout, 2 retries, backend disabled after 20
    consecutive errors.
  - `build/tidy-cache-stats.json` and the `$GITHUB_STEP_SUMMARY` table;
    exit 2 on the silent no-op.
  - Tests against a local stub HTTP server: hit, miss, store only when clean,
    no store on finding/crash/timeout, read-only mode never PUTs, 403 on PUT
    is a warning, error budget, silent no-op exit.
  - _Requirements: 3.1-3.4, 5.1-5.4, 6.1-6.3, 10.1_

- [ ] 5. `KYTHIRA_TIDY_CACHE` CMake knob
  - Cache variable with `off` (default), `read`, `readwrite`,
    `local:<dir>`; reject anything else with `FATAL_ERROR`.
  - `off` leaves the `static-analysis` command byte-identical to today's
    (check by diffing `build.ninja` before and after).
  - Other modes run the driver with the planner's file list, the clang-tidy
    and compiler paths found at configure, and `-j` as today.
  - `static-analysis-fix` stays uncached.
  - _Requirements: 7.1, 10.1_

- [ ] 6. CI wiring
  - `ci.yml` `static-analysis` job: pick the mode (design "CI wiring"),
    `-DKYTHIRA_TIDY_CACHE=<mode>`, `--expect KYTHIRA_TIDY_CACHE=<mode>` in
    `check-build-config.py`, upload `tidy-cache-stats.json` with
    `if: always()`.
  - `vars.TIDY_CACHE_DISABLED == 'true'` forces `off`.
  - Mode `readwrite` whenever the build-cache action exported a writing key
    (main's read-write key or the shared PR-only key), `off` otherwise.
  - Verify three runs and record their IDs: a pull request on a cold prefix
    (all misses, stores), the `main` push after it merges (near-all hits),
    and a fork-style run with the secrets absent (`off`, today's output).
  - Update the comment above the job with the measured warm and cold
    figures.
  - _Requirements: 4.1, 4.3, 4.4, 6.2, 7.2, 7.3_

- [ ] 7. Bucket lifecycle and audit prefix
  - Depends on the PR-only build-cache key from the thread "Let PR runs
    write build caches"; check that its policy covers `clang-tidy/`.
  - `scripts/oci-build-cache/provision.sh`: add the 30-day `clang-tidy/`
    lifecycle rule to `LIFECYCLE_JSON`.
  - `scripts/oci-build-cache/audit.sh` and `test-audit.sh`: report the new
    prefix.
  - **Applying the lifecycle rule to the live bucket is a live-cloud change
    and needs the maintainer's go in his own words.**
  - _Requirements: 4.2, 4.3, 9.1-9.3_

- [ ] 8. Weekly uncached audit
  - `tidy-audit.yml`: weekly schedule plus `workflow_dispatch`, all six
    shards with the cache forced `off` on `main`'s head, stats uploaded,
    `tidy-shard.py --refresh` output published as an artifact.
  - On failure, look up the latest `ci.yml` run on the same SHA; if it
    passed, write "false clean" to the summary with the TU names, so the
    maintainer can set `TIDY_CACHE_DISABLED=true`.
  - Dispatch it once and record the run ID.
  - _Requirements: 7.4, 8.4_

- [ ] 9. Docs and post-merge confirmation
  - `doc/ci_build_cache.md`: a "clang-tidy result cache" section (key,
    trust model, modes, audit, refreshing the costs file, how to disable).
  - After a week on `main`: median and p90 tidy-shard wall time and CI run
    wall time, hit rate per run, false cleans found by the audit (expected
    zero). Record them here.
  - `doc/TODO.md`: move the row out of "Not Started" with the results.
  - _Requirements: 10.2, 10.3_
