# Implementation Plan — clang-tidy Result Cache and Shard Balancing

## Status: 7/9 tasks complete

**Last Updated**: November 7, 2026 (cache live in CI; tasks 6 and 8
done; task 7's code merged, its live apply owed). Written November 6,
when Requirement 4 was amended so every same-repository branch writes,
not only `main`, and task 1 met its gate.

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

- [x] 1. Throwaway measurement and correctness check
  - Prototype driver (key per design, `local:` backend, `--verify-sample`)
    run from this thread's branch by throwaway push-triggered workflows
    rather than a dispatch workflow and draft PR: run 42160557316 replayed
    the 20 first-parent `main` commits from 431bd33 to 23d1209 in four
    chained legs on one `ubuntu-24.04` runner at a time, carrying the cache
    directory through the Actions cache. Workflows and prototype removed
    from the branch afterwards (copies in the project's shared files).
  - **Hit rate.** After the cold first commit, the next 14 commits (legs
    1-3) hit 744-749 of the 746-749 TUs that have a compile command; median
    99.9%. The one wide change in the window (85829cc) still hit 722 and
    missed 27.
  - **Speed.** The cold run took 12,762 s (3.5 h) on one 4-vCPU runner
    (analysis 50,652 CPU-s). Warm, the work per commit was key computation
    (307-442 CPU-s, 0.4-0.6 s per TU, almost all of it the `-M` scan) plus
    the misses: an estimated 186-373 s wall per commit at `-j4`, 864 s for
    85829cc. Median about 240 s, **1.9% of uncached**, far under the 30%
    gate. (Measured walls of 1,480-3,350 s include re-analysing the 10%
    verification sample and the 22 TUs below.)
  - **False cleans: zero.** Every replayed `main` commit passed clang-tidy,
    so re-checking hits there cannot catch a stale clean; the per-commit
    10% samples (72-79 hits each, 1,099 in all) found none, as expected.
    The real test was run 42221510397: PR head 69e7858 (based on replay
    commit 18), whose CI run 42132799851 failed on a
    `modernize-use-nodiscard` finding in
    `include/raft/azure_federated_credential.hpp`. From the leg-1 cache, it
    hit 660 TUs and missed 111; the TU that reports that header
    (`tests/azure_blob_client_unit_test.cpp`) missed and reported the
    finding, and **all 660 hits re-analysed fresh were clean**. This
    replaces Requirement 1.3's "every hit on every commit", which would
    have cost a full 3.5-hour analysis per commit.
  - **Found on the way: 22 TUs have no compile command** in the
    `ci_full_defconfig` database (the discovery nodes, DNS, GCP and Ion
    tests, the Ion example, `multi_raft_node_s3_unavailable_test`,
    `poco_peer_discovery_unit_test`, `shard_group_id_ion_wire_property_test`).
    `run-clang-tidy` skips them silently, so CI has never analysed them; the
    prototype fed them to clang-tidy, which failed them. The driver (task
    4) must skip and count them as `run-clang-tidy` does. Adding them to a
    CI config is its own piece of work, started separately on November 6.
  - **File costs** (task 2) from the cold run: 768 TUs, 50,652 s in all,
    copied to the project's shared files as `costs-431bd33.txt`. The spread
    is wide: `tests/object_store_persistence_unit_test.cpp` alone takes
    1,896 s (31.6 min), the next 546 s. A shard can never finish uncached in
    less time than its slowest TU, so that one file bounds the uncached
    critical path near 32 minutes until it is split.
  - Gate (Requirement 1.6) **met**: zero false cleans, warm median 1.9% of
    uncached. Tasks 2-9 proceed.
  - _Requirements: 1.1-1.6_

- [x] 2. Cost-weighted shard planner
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
  - **Done in #716 (f3d67db), November 7, 2026.** Two deviations. CI
    checks `build/tidy-shard-sources.txt` is non-empty after configure (the
    round-robin fallback removes it) in place of a check-build-config.py
    assertion. And `plan --jobs J` charges each shard's first, longest file
    J times: run-clang-tidy walks its files as a Python set, so with -j4
    the longest can start last, and plain LPT gave the shard holding
    `object_store_persistence_unit_test.cpp` (1,896 s) a 52-minute run.
  - **Criterion 8.3 is not met on GitHub's runners, and cannot be by
    balancing.** Measured on the #716 head 9050050 (static-analysis step, seconds, by runner CPU): EPYC
    7763 3,362 and 3,174; EPYC 9V74 2,396 and 2,352; EPYC 9V45 1,886;
    Xeon 8573C 1,689 (the light 4,758 CPU-s shard). Each shard's CPU-s
    were within 10% of the others but the runners differ by up to 1.8x in
    speed, so the slowest shard is whichever lands on an EPYC 7763. The
    cache (tasks 4-6) removes the dependence: a warm run is about 2% of
    the work. Each shard now logs its CPU model next to its file count.

- [x] 3. Cache key library and its unit tests
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
  - **Done.** `scripts/tidy_cache_key.py` (KeyContext, KeyUnavailable,
    scan_argv, parse_deps) and `scripts/tests/test_tidy_cache_key.py`, 21
    cases. Deviation: not a ctest. No test leg has both clang++-18 and
    clang-tidy-18, so it runs as a step in clang-tidy shard 0 with
    `TIDY_CACHE_KEY_TESTS_REQUIRED=1` (missing tools fail, not skip),
    like the other script tests. The workspace-move case found a bug the
    task 1 prototype shared: dependencies were sorted by absolute path
    before tokenising, so moving the workspace reordered workspace
    headers against /usr ones and changed every key. CI's workspace path
    never changes, so task 1's results stand; the library sorts by the
    tokenised path.

- [x] 4. Cache driver
  - `scripts/tidy-cache.py run` per design Component 2: thread pool,
    `HEAD`/`PUT` against the S3-compatible endpoint with standard-library
    SigV4, `local:` backend, clean-only stores, `== <tu>` output blocks,
    "cached: <tu>" lines, non-zero exit on any finding.
  - Skip TUs with no entry in `compile_commands.json`, as `run-clang-tidy`
    does, and count them in the summary (task 1 found 22).
  - Fail open: 10 s timeout, 2 retries, backend disabled after 20
    consecutive errors.
  - `build/tidy-cache-stats.json` and the `$GITHUB_STEP_SUMMARY` table;
    exit 2 on the silent no-op.
  - Tests against a local stub HTTP server: hit, miss, store only when clean,
    no store on finding/crash/timeout, read-only mode never PUTs, 403 on PUT
    is a warning, error budget, silent no-op exit.
  - _Requirements: 3.1-3.4, 5.1-5.4, 6.1-6.3, 10.1_
  - **Done.** `scripts/tidy-cache.py run` imports the task 3 key library.
    Modes off, read, readwrite, `local:<dir>`, and `env` (read or
    readwrite from `$OCI_BUILD_CACHE_MODE`). SigV4 is checked against
    botocore when that is importable (not in CI). A PUT refused with
    403/404/409/412 is confirmed with a HEAD, so the create-only PR key
    counts an existing entry as present. With `--costs`, TUs start
    longest first, which fixes the late-starting 1,896 s file task 2 hit
    under run-clang-tidy. `scripts/tests/test_tidy_cache.py`: 22 cases
    against a stub S3 server plus two end-to-end runs of the CLI with
    clang-tidy-18 (cold, warm, a real finding never stored, the silent
    no-op); runs beside the key tests in clang-tidy shard 0.

- [x] 5. `KYTHIRA_TIDY_CACHE` CMake knob
  - Cache variable with `off` (default), `read`, `readwrite`,
    `local:<dir>`; reject anything else with `FATAL_ERROR`.
  - `off` leaves the `static-analysis` command byte-identical to today's
    (check by diffing `build.ninja` before and after).
  - Other modes run the driver with the planner's file list, the clang-tidy
    and compiler paths found at configure, and `-j` as today.
  - `static-analysis-fix` stays uncached.
  - _Requirements: 7.1, 10.1_
  - **Done.** With `off` the generated `build.ninja` is byte-identical
    to before (diffed on a mini project holding the clang-tidy section,
    shard 2 of 6). Other modes keep today's command as
    `static-analysis-uncached` and make `static-analysis` run the driver
    with the planner's list, `--costs` and the same `-j`. Added mode
    `env`, which defers read or readwrite to `$OCI_BUILD_CACHE_MODE`, so
    task 6 can configure once without guessing what the build-cache
    action exported.

- [x] 6. CI wiring
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
  - **Done, with one deviation: no real fork pull request was run.** The
    repository has no fork to open one from. The `off` path is still
    checked on every run: the `static-analysis-variants` legs configure
    with the default `KYTHIRA_TIDY_CACHE=off`, which is the unchanged
    `run-clang-tidy` target, and they passed in run 42358433807. The mode
    step's `off` branch is two lines of shell, taken whenever the
    build-cache action reports no key, which is the case for a fork.
    - Cold: PR #720 head 6c5b91e, run 42351017144. Every shard printed
      `clang-tidy cache: env (build-cache key in readwrite mode)`; 760 TUs
      analysed and stored, 0 backend errors, 0 findings; static-analysis
      1456-3389 s per shard (about 3340 s on the four EPYC 7763 runners);
      keying about 0.7 s per TU; run wall 79 minutes.
    - Warm: the main push of 087a880, run 42356568825. 760/760 hits, 0
      misses, 0 stored, 0 backend errors; static-analysis 12-22 s per
      shard (22 s on EPYC 7763, 12-14 s on EPYC 9V45), each shard job
      about 90 s including setup. Run wall 21 minutes (was 62-71 before
      the cache, 79 cold); the slowest job is now Coverage (clang++-18)
      at 21 minutes, then the full-suite and x64 build-and-test legs at
      20. The runner lottery no longer matters:
      the slowest shard is no longer the run's critical path. That commit
      changed no C++, so every key matched; a typical PR misses only the
      TUs its change reaches.
    - The comment above the job and its timeout note carry these figures.
  - A "Choose the clang-tidy cache mode" step picks `env`
    (the driver then reads read or readwrite from `OCI_BUILD_CACHE_MODE`)
    when the build-cache action handed the run a key, and `off` when it
    did not or `vars.TIDY_CACHE_DISABLED` is `true`. Stats upload as
    `tidy-cache-stats-shard-<n>` whenever the mode is not `off`. The
    variant legs stay uncached.

- [ ] 7. Bucket lifecycle and audit prefix
  - _Code written (rule `expire-clang-tidy-results`, audit prefix, two
    new stub checks; the PR key's policy already covers the whole bucket).
    Live apply owed: `provision.sh --apply` after the maintainer's go._
  - Depends on the PR-only build-cache key from the thread "Let PR runs
    write build caches"; check that its policy covers `clang-tidy/`.
  - `scripts/oci-build-cache/provision.sh`: add the 30-day `clang-tidy/`
    lifecycle rule to `LIFECYCLE_JSON`.
  - `scripts/oci-build-cache/audit.sh` and `test-audit.sh`: report the new
    prefix.
  - **Applying the lifecycle rule to the live bucket is a live-cloud change
    and needs the maintainer's go in his own words.**
  - _Requirements: 4.2, 4.3, 9.1-9.3_

- [x] 8. Weekly uncached audit
  - `tidy-audit.yml`: weekly schedule plus `workflow_dispatch`, all six
    shards with the cache forced `off` on `main`'s head, stats uploaded,
    `tidy-shard.py --refresh` output published as an artifact.
  - On failure, look up the latest `ci.yml` run on the same SHA; if it
    passed, write "false clean" to the summary with the TU names, so the
    maintainer can set `TIDY_CACHE_DISABLED=true`.
  - Dispatch it once and record the run ID.
  - _Requirements: 7.4, 8.4_
  - **Done.** First dispatch: run 42359957647 on 4cc6559, November 7.
    All six shards were clean uncached (30-56 minutes each), the report
    job printed "no false clean", and it uploaded the refreshed
    `tidy-file-costs` artifact. Each shard runs the driver with
    `--mode off`, which is run-clang-tidy plus per-TU timings, and the
    driver's stats now list `finding_tus`. The report job fetches the
    shards' stats with `gh run download` (no new pinned action), uploads
    a refreshed `tidy-file-costs` artifact, and on failure checks the
    latest `ci.yml` run on the same SHA to name a false clean. It passes
    only the read-only build-cache key, so it never writes the bucket.

- [ ] 9. Docs and post-merge confirmation
  - `doc/ci_build_cache.md`: a "clang-tidy result cache" section (key,
    trust model, modes, audit, refreshing the costs file, how to disable).
  - After a week on `main`: median and p90 tidy-shard wall time and CI run
    wall time, hit rate per run, false cleans found by the audit (expected
    zero). Record them here.
  - `doc/TODO.md`: move the row out of "Not Started" with the results.
  - _Requirements: 10.2, 10.3_
  - **Docs written November 7; the week of `main` figures is owed (from
    November 14).** `doc/ci_build_cache.md` has the section.
