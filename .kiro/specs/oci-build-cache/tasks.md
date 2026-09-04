# Implementation Plan — OCI-Hosted Build Cache

## Status: 0/12 tasks complete

**Last Updated**: September 4, 2026. Requirements, design and tasks written;
no implementation commits. Added to `doc/TODO.md`'s "Not Started" table the
same day (Requirement 8.4).

## Overview

Move the vcpkg binary cache and the compiler cache out of the 10 GB GitHub
Actions cache, which `scripts/prune-actions-caches.sh` measured at 10.37 GB
and continuously evicting, into one OCI Object Storage bucket reached through
the S3 Compatibility API — vcpkg by `x-aws`, this project's own compiles by
sccache's `s3` backend — so that warm builds happen on every run rather than
on the runs the Actions cache happened to keep. Pre-registered cost: about
$0.50 per month (`doc/sccache_dogfood_cost_estimate.md`).

Reference material to read before starting:
- `.kiro/specs/oci-build-cache/design.md` — the composite action's exact
  logic, the CMake launcher block, the per-leg step sequence, the IAM
  policies and the bucket layout.
- `.kiro/specs/ccache-adoption/` — the spec this supersedes in CI; its
  Task 7 is the reason Requirement 7 exists.
- `scripts/prune-actions-caches.sh` header — the measurement that motivates
  this.
- `.github/workflows/ci.yml` — every `VCPKG_BINARY_SOURCES` line and every
  "Restore ccache" / "Save ccache" pair (six jobs), plus
  `arm64-docker-smoke-test.yml` and `real-cloud-tests.yml`'s `aws` and `oci`
  jobs.
- `scripts/perf-cloud/audit-aws-leaks.sh` and
  `scripts/perf-cloud/test-audit-aws-leaks.sh` — the audit pattern, and the
  test-in-the-failing-direction pattern.
- `docker/sccache_runner/run.sh` on `feat/redis-compatible-kv` — the guarded
  `sccache --start-server` pattern.

## Task Dependency Graph

```json
{
  "waves": [
    { "tasks": [1, 2] },
    { "tasks": [3, 4, 5] },
    { "tasks": [6, 7] },
    { "tasks": [8, 9, 10] },
    { "tasks": [11, 12] }
  ],
  "edges": [
    [1, 5], [1, 7], [2, 3], [2, 6], [2, 7],
    [3, 6], [3, 7], [4, 7], [5, 7],
    [6, 8], [7, 8], [7, 9], [8, 10], [9, 10], [10, 11], [10, 12]
  ]
}
```

Tasks 1 and 2 are independent and both gate the wiring: nothing permanent
is written to a workflow until the throwaway measurement exists and the
bucket does.

## Tasks

- [ ] 1. Throwaway measurement
  - On a scratch branch, install a pinned sccache on every leg in
    Requirement 5.1's table, point it at a scratch bucket (or the real one
    once Task 2 exists), replace the ccache launcher for that run only, and
    run the full matrix twice: cold, then warm.
  - Record per leg from `sccache --show-stats`: compile requests, cacheable,
    non-cacheable with sccache's reason string, hits, misses, bytes read and
    written; and the Build step's wall clock both times. Call out the
    `kythira_test_pch` users and the coverage leg's
    `-fprofile-instr-generate` objects specifically (Requirement 1.3).
  - Run one job with `VCPKG_BINARY_SOURCES` on `x-aws` against the bucket,
    empty then populated: ports uploaded, bytes, and the install step's wall
    clock each time.
  - Write the table here, with run ids. If any leg is under 50% cacheable,
    note it as the leg Requirement 5.1 keeps on ccache. If warm wall clocks
    disagree with Requirement 6's thresholds, re-derive them here and in
    `requirements.md`.
  - Close the PR without merging.
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5_

- [ ] 2. Provision the bucket, users, policies and keys
  - Write `scripts/oci-build-cache/provision.sh` per design.md Component 1:
    dry-run by default, `--apply`, `--rotate`, idempotent, never writes a key
    to disk. Copyright header after the shebang.
  - Write `scripts/oci-build-cache/audit.sh` per Component 1, and
    `scripts/oci-build-cache/test-audit.sh` that tags a scratch bucket and
    asserts the audit fails, then removes it and asserts the audit passes.
  - Run the audit in the failing direction and paste both outputs here.
  - Apply, set the three repository variables and four secrets, and record
    the bucket name, namespace and the key creation dates here (never the
    keys).
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6_

- [ ] 3. Composite action `.github/actions/oci-build-cache/`
  - `action.yml` with the inputs in Requirement 4.5 and the logic in design.md
    Component 2: event-to-key selection, variable checks with `::error::`,
    the no-credentials `enabled=false` path, the environment export, and the
    pinned sccache download with SHA-256 verification for `X64` and `ARM64`.
  - Assert the AWS CLI is present and print its version.
  - Verify on a scratch workflow that a `push` to a non-main branch selects
    the RO key (the `push:main` case must be exact), and that unsetting a
    variable fails the job at the action.
  - _Requirements: 3.2, 4.1, 4.2, 4.3, 4.5, 5.3_

- [ ] 4. CMake `KYTHIRA_COMPILER_LAUNCHER`
  - Replace the `KYTHIRA_ENABLE_CCACHE` block in the root `CMakeLists.txt`
    with design.md Component 3, including the deprecation alias.
  - Verify all five states: `auto` with ccache present, `auto` with only
    sccache present, `auto` with neither, `sccache` explicit (present and
    absent, the latter a `FATAL_ERROR`), `none`, and `KYTHIRA_ENABLE_CCACHE=OFF`.
    Record the `CMakeCache.txt` launcher entry for each.
  - Diff `ninja -t targets` between launcher on and off (Requirement 9.1) and
    record that it is empty.
  - _Requirements: 5.2, 9.1_

- [ ] 5. `DEPENDENCIES.md` and `doc/ci_build_cache.md`
  - sccache entry beside the ccache one: install, the local read-only recipe
    (`ro` key, `AWS_ENDPOINT_URL`, `-DKYTHIRA_COMPILER_LAUNCHER=sccache`),
    and that no local build needs it.
  - `doc/ci_build_cache.md` per Requirement 8.2, written from the design and
    updated by Tasks 10 and 12 with measured figures.
  - _Requirements: 8.1, 8.2_

- [ ] 6. vcpkg binary cache to the bucket, every workflow
  - In `ci.yml` (six jobs), `arm64-docker-smoke-test.yml`, and
    `real-cloud-tests.yml` (`aws`, `oci`): add the composite action after
    "Resolve vcpkg triplet", delete the `export VCPKG_BINARY_SOURCES=...x-gha`
    line (the env now carries it), delete the "Export Actions cache
    credentials for vcpkg" step. Keep the `vcpkg_installed/` tree caches.
  - Verify with a `push` to `main` whose `vcpkg.json` hash is new (or by
    deleting the tree cache once): the install step uploads, the audit shows
    `vcpkg/x64-linux/` and `vcpkg/arm64-linux/` populated, and the next run
    with the tree cache deleted again downloads instead of building.
    Record run ids and port counts.
  - _Requirements: 3.1, 3.2, 3.3, 3.6, 4.1_

- [ ] 7. sccache replaces ccache on every moved leg
  - Per design.md Component 4, on each leg Task 1 cleared: add "Start
    sccache" (guarded), pass `-DKYTHIRA_COMPILER_LAUNCHER=` from its output,
    add "sccache statistics" with `if: always()` writing to the job summary,
    delete "Restore ccache" / "ccache size limit" / "Save ccache" and
    `CCACHE_DIR`. Any leg Task 1 kept on ccache gets a comment beside its
    Configure step saying why, with the measured cacheable fraction.
  - `RUSTC_WRAPPER` and `VCPKG_KEEP_ENV_VARS` come from the action; verify
    once, on a tree-cache miss, that the `lakers` port's cargo build reports
    sccache requests in the statistics.
  - Verify on a `push` to `main`: statistics in every summary, bucket
    `sccache/` object count rises. Record run id and per-leg counts.
  - _Requirements: 5.1, 5.4, 5.5, 5.6, 5.7_

- [ ] 8. Actions-cache accounting, re-measured
  - After Tasks 6 and 7 have run on `main` for two days, re-run the
    measurement in `scripts/prune-actions-caches.sh`'s header (total bytes,
    entry count, bytes per family) and rewrite that header's accounting
    paragraph with the new figures. The rules do not change.
  - Record before/after here.
  - _Requirements: 3.4_

- [ ] 9. Three-state credential verification and bad-endpoint run
  - Real runs: a `push` to `main` writes (object count rises); a
    `pull_request` from a branch of this repository reads with hits and
    reports sccache write errors, object count unchanged; a run with the
    four secrets temporarily renamed builds green with `enabled=false`.
  - One run with `OCI_BUILD_CACHE_NAMESPACE` pointed at a non-existent
    namespace: green, ports built from source, sccache start warning.
  - Record all four run ids here.
  - _Requirements: 3.5, 4.3, 7.2, 7.3, 9.4_

- [ ] 10. Second-run thresholds
  - On the second consecutive `push` run to `main` after Task 7, read from
    the job summaries: each `build-and-test` Build step ≤ 15 min; stdexec
    leg Build ≤ 45 min with swap under 4 GiB in "Report headroom after
    build"; every leg ≥ 90% hits over cacheable; zero ports built from
    source. A following `pull_request` run shows the same hit rates.
  - Record the table here, against the pre-registered figures. A miss on
    any threshold is a finding to investigate in this task, not a threshold
    to move.
  - _Requirements: 6.1, 6.2, 6.3, 6.4_

- [ ] 11. Month-one audit and the cost cross-reference
  - Run `scripts/oci-build-cache/audit.sh` one month after Task 7: bytes per
    prefix, request count and egress from the usage API, and the bill line.
    Paste it here beside the $0.50 and 1 to 5 TB pre-registered.
  - Add the one-paragraph cross-reference to
    `doc/sccache_dogfood_cost_estimate.md` with the measured figure.
  - _Requirements: 6.5, 7.4, 8.3_

- [ ] 12. TODO row and close-out
  - Update `doc/TODO.md`'s row for this spec from 0/12 with what each task
    found, in the manner of the table's other closed rows, and move it off
    "Not Started".
  - Confirm every task above cites a run id (Requirement 7.1).
  - _Requirements: 7.1, 8.4_

## Deferred

- sccache inside vcpkg port builds via a custom triplet (design.md,
  "Deferred").
- Pointing sccache at the Redis gateway; that is
  `doc/sccache_dogfood_cost_estimate.md`'s deployment, and this spec is its
  baseline.
