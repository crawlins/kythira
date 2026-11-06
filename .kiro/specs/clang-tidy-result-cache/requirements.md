# Requirements Document — clang-tidy Result Cache and Shard Balancing

## Introduction

The `clang-tidy (shard N of 6)` jobs are the critical path of every CI run.
In the four most recent green runs on `main` (November 5, 2026: runs
42113857996, 42066543559, 42036833537 and 41972492849) the run took 62-71
minutes, the slowest tidy shard finished last every time, and every other
job had finished by minute 29-40. The six shards together spent 250-275 job
minutes per run, about 60% of all x64 runner time CI uses.

Two things make that slow, and this spec addresses both:

1. **Nothing is reused between runs.** Every event analyses all 772
   translation units from scratch, although a typical commit changes a
   handful of them. clang-tidy only parses, so sccache does not help
   (`ci.yml` says so above the job). A cache of clean results, keyed on
   everything that can change a result, lets an unchanged translation unit
   skip analysis entirely.
2. **The shards are unbalanced.** `CMakeLists.txt` deals the sorted file list
   round robin. In run 42113857996 one shard took 27 minutes and another 66.
   The per-file cost varies by more than an order of magnitude (a test file
   that includes Folly, Proxygen and the AWS SDK is far heavier than a
   serializer unit test), and round robin ignores it.

A cache that ever reports "clean" for a file that has a finding would quietly
disable the clang-tidy gate (`.kiro/specs/ci-build-matrix-coverage/`
Requirement 3). Correctness of the key therefore outranks hit rate
everywhere below, and Requirement 1 measures both before anything merges, the
way the ccache, OCI build-cache and mold specs did.

### Current state (verified November 6, 2026, on `main` at 6f97f8b)

- `CMakeLists.txt` globs `src/ tests/ examples/ cmd/` for `*.cpp` (772 files),
  sorts them, and with `KYTHIRA_TIDY_SHARD_COUNT`/`KYTHIRA_TIDY_SHARD_INDEX`
  keeps every N-th one. It writes the result to `build/tidy-sources.txt`.
- `static-analysis` runs `run-clang-tidy -p build -j <logical cores>` over
  that list. clang-tidy 18.1.3 (`clang-tidy-18` from Ubuntu 24.04), as
  pinned at the top of `.clang-tidy`. There is one `.clang-tidy`, at the
  repository root, with `WarningsAsErrors: "*"` and a `HeaderFilterRegex`
  that reports findings in project headers under `include/raft`,
  `include/network_simulator`, `include/concepts`, `tests` and `src`.
- `scripts/build-tidy-prerequisites.sh` builds the protoc output and the test
  precompiled header(s) first. Compile commands for test files reference the
  PCH (`-Xclang -include-pch`), so the PCH's source header is an input of
  those translation units that does not appear as a plain `#include`.
- The job already has OCI build-cache credentials through
  `.github/actions/oci-build-cache`: read-write on `main` (the
  `build-cache-write` environment), read-only on pull requests, none on fork
  pull requests. Requirement 4 uses the shared PR-only key for non-fork
  pull requests. The bucket `kythira-build-cache` has lifecycle rules only
  for the `sccache/` and `vcpkg/` prefixes (`scripts/oci-build-cache/provision.sh`).
- `curl` 8.5 on the runners supports `--aws-sigv4`, so the job can talk to
  the OCI S3-compatible endpoint without new tools.

## Glossary

- **TU**: a translation unit, i.e. one `*.cpp` file in `tidy-sources.txt`
  with its compile command.
- **clean result**: clang-tidy exited 0 and printed no diagnostic for the TU,
  including diagnostics in headers the `HeaderFilterRegex` admits.
- **cache key**: a digest of every input that can change a TU's clang-tidy
  result (Requirement 2).
- **false clean**: the cache serves "clean" for a TU that a fresh clang-tidy
  run would report a finding for. The one failure this spec exists to
  prevent.
- **file cost**: the measured wall-clock seconds clang-tidy spends on one TU
  on a standard 4-vCPU runner.
- **silent no-op**: CI asks for the cache, the cache does nothing (no
  lookups, or every lookup errors), and the job stays green while running at
  today's speed. Named after the same failure mode in the ccache and mold
  specs.

## Requirements

### Requirement 1: Measure before adopting

**User Story:** As the maintainer, I want the cache's hit rate, overhead and
correctness measured on this repository before it is wired into CI, so the
decision rests on numbers from these builds.

#### Acceptance Criteria

1. A throwaway branch and draft PR SHALL run the cache wrapper (Requirement
   2's key, a local-directory backend) over the last 20 commits on `main`, in
   order, starting from an empty cache, on a standard `ubuntu-24.04` runner.
2. For each commit it SHALL record: TUs analysed, cache hits, cache misses,
   key-computation time summed over all TUs, and analysis wall time with the
   cache against the uncached baseline for the same commit.
3. For every hit in that replay, it SHALL also run clang-tidy fresh and
   compare. Any hit whose fresh result is not clean is a false clean, and
   one false clean SHALL stop adoption until the key is fixed and the replay
   re-run.
4. It SHALL record each TU's file cost from the uncached runs, for
   Requirement 8.
5. The results SHALL be written to `tasks.md` task 1 with the run IDs.
6. Adoption SHALL proceed only if the replay shows zero false cleans AND the
   median per-commit analysis time with a warm cache is at most 30% of the
   uncached time. Otherwise the maintainer decides between fixing the key,
   adopting anyway, or closing the spec.

### Requirement 2: A cache key that covers every input

**User Story:** As the maintainer, I want a TU's cache key to change whenever
anything that could change its clang-tidy result changes, so the cache can
never hide a finding.

#### Acceptance Criteria

1. The key SHALL be a SHA-256 digest over, in a fixed and versioned order:
   1. a key-format version string, bumped whenever the key computation
      changes;
   2. the clang-tidy binary's `--version` output and the SHA-256 of the
      binary itself;
   3. the TU's effective configuration as printed by
      `clang-tidy --dump-config <file>` (which folds in every `.clang-tidy`
      on the path and `--checks`/`--config` overrides), plus any extra
      arguments the wrapper passes to clang-tidy;
   4. the TU's compile command from `compile_commands.json`, normalised as
      in criterion 3;
   5. the repository-relative path and the SHA-256 of the raw bytes of every
      file the TU reads, as listed by running the same compiler in
      dependency-only mode (`-M`) on the same command.
2. Raw file bytes, not preprocessed output, SHALL be hashed, so that a change
   to a comment, including adding or removing a `NOLINT`, `NOLINTNEXTLINE`
   or `NOLINTBEGIN/END`, changes the key. A preprocessed digest drops
   comments and would serve a stale clean after a `NOLINT` is removed.
3. Normalisation SHALL replace the absolute workspace path and the absolute
   build-directory path with fixed tokens, and SHALL drop only options that
   cannot affect parsing (`-o <file>`, `-MD`/`-MF <file>`/`-MT <target>`,
   the launcher prefix `sccache`). Every other option stays in the key.
4. A TU compiled with a precompiled header SHALL have the PCH's source header
   and that header's own dependency list in its key, computed by running the
   dependency scan with the PCH replaced by `-include <its source header>`.
   The `.pch` file's own bytes SHALL NOT be used, because they are not
   reproducible between runners.
5. Files outside the workspace (system and `vcpkg_installed/` headers) SHALL
   be keyed by their content hash too. Hashing a few hundred headers per TU
   is acceptable; the wrapper SHALL memoise hashes per file within one run.
6. If the dependency scan fails for a TU, the wrapper SHALL treat it as a
   miss, analyse it, and SHALL NOT store a result for it.
7. A unit test suite SHALL show, for each input in criterion 1, that changing
   only that input changes the key, including a `NOLINT` removal, a change in
   a header reached only through the PCH, a nested `.clang-tidy`, and a
   changed `-D` define.

### Requirement 3: Only clean results are cached

**User Story:** As a developer, I want every finding reported from a fresh
clang-tidy run, so cached output never stands in for a real diagnostic.

#### Acceptance Criteria

1. The wrapper SHALL store an entry only when clang-tidy exits 0 and prints
   no diagnostic. The entry SHALL hold the key inputs' summary (format
   version, tool version, TU path, commit SHA that produced it) and nothing
   that is replayed as output.
2. A TU whose analysis fails, crashes, times out or prints any diagnostic
   SHALL NOT be stored. On the next run it SHALL be analysed again.
3. A hit SHALL print one line naming the TU as cached, so the job log still
   accounts for every file.
4. `static-analysis` SHALL still fail if any TU reports a finding, exactly as
   it does today.

### Requirement 4: Who writes

**User Story:** As the maintainer, I want every CI run that can hold a
credential, on `main` and on every pull-request branch, to write its clean
results, so a merged PR's files are already cached when `main` re-checks
them. (Amended November 6, 2026 at the maintainer's request; the first
draft let only `main` write.)

#### Acceptance Criteria

1. `static-analysis` runs on `main` pushes and on pull requests from
   branches in this repository, whoever the author, SHALL read and write
   the cache.
2. `main` SHALL write with the existing read-write build-cache key. Pull
   requests SHALL write with the PR-only build-cache key that the thread
   "Let PR runs write build caches" provisions for `sccache/`, `vcpkg/` and
   `clang-tidy/` together (the maintainer chose one PR-only key for all
   three caches on November 6). This spec SHALL NOT create a key of its
   own.
3. The PR-only key is a repository secret, not an environment secret, so
   pull-request jobs from branches in this repository receive it.
4. Fork pull requests SHALL run with the cache `off` and behave exactly as
   today. GitHub gives `pull_request` runs from forks no secrets, and this
   spec SHALL NOT work around that with `pull_request_target`, which would
   run a fork's code next to the repository's secrets.
5. The accepted risk SHALL be written down in the docs (Requirement 10.2):
   anyone who can push a branch to this repository can store a "clean"
   entry for any key, including one for files they later merge with a
   finding in them. The weekly uncached audit (Requirement 7.4) is what
   catches that.

### Requirement 5: Fail open, never fail closed on the cache

**User Story:** As a developer, I want a cache outage to cost time, not a
red build or a missed finding.

#### Acceptance Criteria

1. Any backend error (DNS, TLS, timeout, 5xx, 403) on a lookup SHALL be
   treated as a miss and logged as a warning; the TU SHALL be analysed.
2. Any backend error on a store SHALL be logged as a warning and SHALL NOT
   fail the job.
3. Each backend request SHALL have a timeout of at most 10 seconds and at
   most 2 retries.
4. After 20 consecutive backend errors in one shard, the wrapper SHALL stop
   contacting the backend for the rest of that shard and analyse every
   remaining TU.

### Requirement 6: Observability and the silent no-op guard

**User Story:** As the maintainer, I want each shard to say how much the
cache did, and CI to fail if a requested cache did nothing.

#### Acceptance Criteria

1. Each shard SHALL write to `$GITHUB_STEP_SUMMARY`: TUs, hits, misses,
   stores, backend errors, key-computation seconds, analysis seconds, and the
   estimated seconds saved (sum of hit TUs' file costs).
2. When the job requested the cache (`KYTHIRA_TIDY_CACHE` is `read` or
   `readwrite`), a shard that made zero successful lookups SHALL fail with a
   message naming the silent no-op.
3. The wrapper SHALL write the same counters as JSON to
   `build/tidy-cache-stats.json`, uploaded as a job artifact.

### Requirement 7: Modes, escape hatch and audit

**User Story:** As the maintainer, I want to turn the cache off in one
place, and a regular uncached run that proves it has not been hiding
findings.

#### Acceptance Criteria

1. A CMake cache variable `KYTHIRA_TIDY_CACHE` SHALL take `off` (default),
   `read`, `readwrite` or `local:<directory>`. `off` SHALL leave the
   `static-analysis` command line byte-identical to today's.
2. `ci.yml` SHALL set `readwrite` whenever a writing build-cache key is
   present (Requirement 4) and `off` when none is.
3. A repository variable `TIDY_CACHE_DISABLED=true` SHALL force `off` in
   every CI job without a code change.
4. A scheduled workflow run, at least weekly, SHALL run all shards with the
   cache `off` on `main`'s head and fail if any TU reports a finding. If that
   run fails while the most recent cached `main` run passed on the same
   commit, it is a false clean: the run SHALL say so in its summary, and the
   cache SHALL be disabled (criterion 3) until the key is fixed.

### Requirement 8: Shards balanced by measured cost

**User Story:** As the maintainer, I want the shards to finish at about the
same time, so the slowest one is not 2.4 times the fastest.

#### Acceptance Criteria

1. A checked-in file `configs/tidy-file-costs.txt` SHALL list a file cost in
   seconds per TU path, one `<seconds> <path>` line each, sorted by path.
2. Shard assignment SHALL use longest-processing-time-first: sort TUs by
   cost descending (ties by path), and give each to the shard with the
   smallest running total (ties to the lowest index). TUs missing from the
   costs file SHALL get the median cost. The assignment SHALL be a pure
   function of the file list, the costs file and the shard count.
3. With the cache `off`, on Requirement 1's measured costs, the slowest
   shard's predicted time SHALL be at most 1.15 times the fastest's.
4. A script SHALL regenerate the costs file from the shards' timing output
   (Requirement 6.3), and the weekly uncached run (Requirement 7.4) SHALL
   publish a refreshed file as an artifact. Updating the checked-in file is a
   normal reviewed commit.
5. `KYTHIRA_TIDY_SHARD_COUNT=1` SHALL still analyse every TU, and the
   written `tidy-sources.txt` SHALL keep its current format.

### Requirement 9: Storage and lifecycle

**User Story:** As the maintainer, I want cache entries to expire on their
own and the audit to see them, consistent with the rest of the bucket.

#### Acceptance Criteria

1. `scripts/oci-build-cache/provision.sh` SHALL add a lifecycle rule
   deleting `clang-tidy/` objects 30 days after creation. Applying it to the
   live bucket is a real-cloud change and needs the maintainer's go.
2. `scripts/oci-build-cache/audit.sh` SHALL report the `clang-tidy/` prefix
   alongside `sccache/` and `vcpkg/`.
3. Entries SHALL be small (under 1 KiB each). The design SHALL estimate the
   steady-state object count and size.

### Requirement 10: Local use and documentation

**User Story:** As a developer, I want the same cache on my machine and docs
that say how it works.

#### Acceptance Criteria

1. `-DKYTHIRA_TIDY_CACHE=local:<dir>` SHALL cache to a local directory with
   the same key and the same clean-only rule, needing no credentials.
2. `doc/ci_build_cache.md` SHALL gain a section covering the key, the trust
   model, the modes, the weekly audit, and how to refresh the costs file.
3. `ci.yml`'s comment above the `static-analysis` job SHALL be updated with
   the measured numbers, and `doc/TODO.md` SHALL track the spec.
