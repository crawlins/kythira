# Requirements Document

## Introduction

This document specifies the requirements for adopting [ccache](https://ccache.dev/)
— a compiler cache that skips recompiling a translation unit whose preprocessed
content and compiler flags exactly match a prior compile — for this project's
CMake-driven C++ builds, both on a developer's own machine and in every CI job
that runs `cmake --build`.

This is not a speculative optimization. A throwaway experiment run directly
against this repository's own CI (draft PR #49, closed without merging once
the numbers were collected) measured the actual effect on a real, from-scratch
`clang++-18` Release build of this codebase:

| | Cold build | Warm build (nothing changed) |
|---|---|---|
| Wall-clock | 29m 07s | **11m 59s** |
| Cacheable compiler calls | 187 / 399 (46.9%) | 187 / 399 (46.9%) |
| Cache hits | 0 / 187 (0%) | **187 / 187 (100%)**, all direct-mode |
| Uncacheable calls (link steps) | 212 / 399 (53.1%) | 212 / 399 (53.1%) — unaffected either way |

A rebuild where no source file actually changed took **59% less wall-clock
time** (29m07s → 11m59s). The remaining time is structural, not a cache miss:
53% of every compiler-launcher invocation is a link step (CMake drives linking
through the compiler frontend), and ccache — correctly — does not, and cannot,
cache linking. This spec's acceptance criteria are calibrated against this
measured ~59% ceiling, not an aspirational number.

This project currently has **zero** build caching for its own source (`grep
-r ccache` across the repository returns nothing). The only existing cache is
`vcpkg_installed/` (Folly, Boost, AWS SDK, etc.), cached in CI via
`actions/cache` keyed on `vcpkg.json`'s hash — that cache is unaffected by
this spec and out of scope; ccache only ever sees this project's own ~408
translation units (368 in `tests/`, 33 in `examples/`, 6 in `cmd/`, 1 in
`src/`), never vcpkg's already-prebuilt dependency libraries.

## Glossary

- **ccache**: A C/C++ compiler cache. Wrapped in front of the real compiler
  via `CMAKE_C_COMPILER_LAUNCHER`/`CMAKE_CXX_COMPILER_LAUNCHER`, it hashes each
  translation unit's preprocessed content plus the exact compiler invocation
  and either returns a previously-cached object file or falls through to the
  real compiler and caches the result.
- **direct mode**: ccache's fast hit path — hashes the *un-preprocessed*
  source plus its `#include` graph (via a manifest) rather than running the
  full preprocessor. All 187/187 hits in the measured experiment were direct
  mode.
- **build config**: One combination of compiler + build type + CMake flags
  that this project's CI exercises independently. Four exist today:
  `g++-13` Release, `clang++-18` Release (`build-and-test`), `clang++-18`
  Debug+coverage-instrumented (`coverage`), and `clang++-18` Release with
  `-DKYTHIRA_AWS_REAL_EC2_TESTS=ON` (`real-cloud-tests.yml`'s `aws` job).
  ccache's own flag-hashing means these four never share cache entries with
  each other regardless of how their storage is arranged.
- **cache persistence** (CI-only concern): a local developer's ccache
  directory (`~/.ccache` by default) simply lives on disk between builds. A
  GitHub-hosted CI runner is destroyed after every job, so ccache only helps
  CI if its cache directory is saved and restored across runs via
  `actions/cache`.
- **restore-broad, save-unique**: the cache key pattern this spec uses for
  ccache's CI persistence — restore the most recent matching cache via a
  prefix `restore-keys`, but always save under a key unique to this run
  (`github.run_id`), so every run's newly-cached objects are captured for the
  *next* run rather than being silently dropped by `actions/cache`'s
  save-only-on-primary-miss behavior. This deliberately differs from the
  existing `vcpkg_installed/` cache, which is correctly keyed on
  `vcpkg.json`'s hash — that cache should only change when the hash changes,
  vcpkg is not what we are trying to keep "fresh" every run.

## Requirements

### Requirement 1: CMake auto-detection (the shared mechanism for both "local" and "CI")

**User Story:** As a developer or CI job building this project, I want ccache
used automatically when it's installed and to have zero effect when it isn't,
so that adopting it requires no per-invocation flags and never becomes a hard
build dependency.

#### Acceptance Criteria

1. The root `CMakeLists.txt` SHALL define `option(KYTHIRA_ENABLE_CCACHE
   "Use ccache to speed up rebuilds when available" ON)`, placed near the top
   of the file (after the `project()`/C++ standard block, before any
   `find_package` call), matching this file's existing placement convention
   for `ENABLE_COVERAGE`.
2. WHEN `KYTHIRA_ENABLE_CCACHE` is `ON` (the default) AND `find_program(CCACHE_PROGRAM
   ccache)` locates a `ccache` binary THEN the configure step SHALL set
   `CMAKE_C_COMPILER_LAUNCHER` and `CMAKE_CXX_COMPILER_LAUNCHER` to that
   binary's path and print a `message(STATUS "ccache: enabled (<path>)")` line.
3. WHEN `KYTHIRA_ENABLE_CCACHE` is `ON` AND `ccache` is NOT found THEN
   configure SHALL succeed exactly as it does today (no launcher set) and
   print `message(STATUS "ccache: not found, building without it")` — this
   SHALL NOT be a fatal error and SHALL NOT require any additional flag to
   suppress.
4. WHEN `KYTHIRA_ENABLE_CCACHE` is explicitly set to `OFF` THEN no launcher
   SHALL be set regardless of whether `ccache` is installed, and configure
   SHALL print `message(STATUS "ccache: disabled (KYTHIRA_ENABLE_CCACHE=OFF)")`.
5. No target definition, `find_package` call, or test SHALL reference
   `CCACHE_PROGRAM`/`KYTHIRA_ENABLE_CCACHE` directly — this mirrors every
   existing optional-dependency in this project (`KYTHIRA_HAS_OPENSSL`,
   `KYTHIRA_HAS_AWS_SDK`, etc.): detection lives in exactly one place, and
   its absence changes nothing else about the build graph.
6. This single mechanism SHALL be the entire "local" half of this spec —
   Requirement 6 covers only documenting it, not any additional local-only
   code path.

---

### Requirement 2: Optional-dependency isolation

**User Story:** As a maintainer, I need proof that building without ccache
installed produces the identical build graph as building with it, so that
this feature can never become a silent hard dependency the way a
`find_package(... REQUIRED)` mistake would.

#### Acceptance Criteria

1. A verification step SHALL configure and build the project twice from a
   clean build directory: once with `-DKYTHIRA_ENABLE_CCACHE=OFF` and once
   with `-DKYTHIRA_ENABLE_CCACHE=ON` on a machine where `ccache` is installed,
   and confirm both produce the same set of build targets (this follows the
   same "optional dependency isolation" methodology already established by
   this project for the `stdexec` backend, `scripts/verify-optional-dependency-isolation.sh`,
   adapted here to a `find_program` toggle rather than a `find_package` one
   since ccache is invoked as a launcher, not linked as a library).
2. Neither `ctest` output, produced binaries, nor `coverage_floor.txt`
   enforcement SHALL differ between a ccache-enabled and ccache-disabled
   build of the same commit — ccache is required to be a pure build-time
   accelerant with zero effect on compiled output correctness (this is an
   inherent ccache guarantee — bitwise-identical object files, mtimes aside
   — but SHALL be explicitly checked once as part of this rollout rather than
   assumed).

---

### Requirement 3: CI — install ccache in every build config

**User Story:** As a CI job that builds this project, I want `ccache`
installed before `cmake` configures, so that Requirement 1's auto-detection
actually activates instead of silently building without it.

#### Acceptance Criteria

1. `ccache` SHALL be added to the `apt-get install` package list of every
   step in `.github/workflows/ci.yml` and `.github/workflows/real-cloud-tests.yml`
   that currently installs `cmake`/`ninja-build` for a build this project
   compiles — concretely: `build-and-test`'s "Install system dependencies"
   (`ci.yml` line 32, shared by both matrix legs), `coverage`'s "Install
   system dependencies" (`ci.yml` line 171), and `real-cloud-tests.yml`'s
   `aws` job's "Install system dependencies" step.
2. No `-DCMAKE_C_COMPILER_LAUNCHER=ccache`/`-DCMAKE_CXX_COMPILER_LAUNCHER=ccache`
   flag SHALL be added to any workflow's `cmake -B build` invocation —
   Requirement 1's auto-detection already activates as soon as `ccache` is on
   `PATH`, so no workflow file needs to know ccache exists beyond installing
   the package. This keeps the launcher logic in exactly one place
   (`CMakeLists.txt`) rather than duplicated across four `cmake` invocations
   in two workflow files.
3. The `packer-ca-cluster-node` job (`ci.yml`) and the `ami-build`/`packer`
   jobs (`real-cloud-tests.yml`) SHALL NOT be touched by this requirement —
   neither runs `cmake --build` against this repository (the AMI pipeline's
   only compile step happens inside `docker/ca_cluster_node/Dockerfile`'s
   `builder` stage, a separate mechanism out of scope per Requirement 8).

---

### Requirement 4: CI — persist the ccache directory across runs

**User Story:** As a CI job, I need my ccache cache directory to survive
between separate workflow runs (not just within one job), so that a small
follow-up push to a PR — the common case demonstrated by this project's own
recent history of iterative CI-driven fixes — gets the ~59% measured speedup
instead of starting from an empty cache every single time.

#### Acceptance Criteria

1. Each of the three build configs identified in Requirement 3.1 SHALL gain
   its own `actions/cache/restore@v4` step (immediately before the `cmake -B`
   configure step) and matching `actions/cache/save@v4` step (after the
   `Build` step, `if: always()` so a save still happens even when the build
   itself fails partway — a partial cache is still useful for the next run).
   The combined `actions/cache@v4` action SHALL NOT be used for ccache (see
   AC 3 for why).
2. Each restore/save pair's cache path SHALL be `~/.ccache` (ccache's default
   `CCACHE_DIR` when unset — no workflow needs to set `CCACHE_DIR` explicitly).
3. Per Glossary's "restore-broad, save-unique" pattern: the **restore** step's
   `key` SHALL be unique per run (e.g. including `github.run_id`, which will
   never match an existing cache and is only present to satisfy the action's
   required `key` input) with `restore-keys` set to a prefix that matches the
   most recent prior cache for that exact build config and branch — e.g. for
   `build-and-test`'s matrix: `ccache-${{ runner.os }}-${{ matrix.compiler }}-${{ github.ref_name }}-`.
   The **save** step's `key` SHALL be that same prefix plus `github.run_id`,
   guaranteeing a fresh, never-colliding key every run so `actions/cache`
   always performs the save (its default behavior skips saving when the key
   already has an exact hit — using a combined action here would silently
   stop accumulating new objects after the first successful run).
4. `coverage` and `real-cloud-tests.yml`'s `aws` job SHALL use the same
   pattern with their own distinct key prefixes (`ccache-coverage-...`,
   `ccache-realcloud-...`) — each build config's cache SHALL NOT share a key
   prefix with any other, even though ccache's own internal hashing would
   safely tolerate a shared directory (Glossary, "build config") — separate
   prefixes keep each `actions/cache` entry's size scoped to what one build
   config actually produces.
5. GitHub Actions cache scoping means a branch's own cache is restored on
   subsequent runs of that branch, and (per GitHub's documented cache
   scoping rules) a PR branch additionally falls back to its base branch's
   cache and `main`'s cache is visible to every branch — this SHALL NOT
   require any explicit configuration in this spec's `restore-keys`; it is
   inherent to how `actions/cache` resolves keys across refs.

---

### Requirement 5: CI — bounded ccache size, no disk-budget regression

**User Story:** As the maintainer who already had to widen `coverage`'s
"Free disk space" prune list twice for "No space left on device" failures
from ~148 statically-linked binaries, I need ccache's own storage to be
explicitly bounded so this feature cannot reintroduce that failure mode.

#### Acceptance Criteria

1. Every restore/save pair (Requirement 4) SHALL be immediately followed (on
   the restore side, after the configure step activates ccache) by an
   explicit `ccache --max-size=<N>` call, not ccache's 5 GB default:
   `build-and-test` and `real-cloud-tests.yml`'s `aws` job SHALL use `2G`
   each; `coverage` — the job with documented, repeated disk pressure —
   SHALL use `1G`.
2. `coverage`'s existing "Free disk space" step (`ci.yml` line 160) SHALL run
   BEFORE the ccache restore step, unchanged, so the reclaimed headroom is
   available before the cache download consumes any of it.
3. A comment SHALL be added at the `coverage` job's ccache steps cross-
   referencing the "Free disk space" step's own comment (which already
   documents the "No space left on device" history) so a future reader
   sizing either one understands both are competing for the same constrained
   budget.
4. This requirement does not apply to a local developer machine — no
   `ccache --max-size` call is added to `CMakeLists.txt` or any local
   documentation; ccache's own interactive `--max-size`/default 5 GB
   behavior on a developer's own disk is left to the developer, consistent
   with this being an optional, non-invasive tool per Requirement 1.

---

### Requirement 6: Documentation

**User Story:** As a developer setting up this project for the first time, I
want to know ccache is supported and how to get the speedup, without it
being presented as a required step.

#### Acceptance Criteria

1. `DEPENDENCIES.md`'s "Optional Dependencies" section SHALL gain a `ccache`
   entry following the exact format of its existing entries (libfiu,
   OpenSSL, etc.): Status, Purpose, installation command
   (`apt install ccache` / `brew install ccache`), and a Notes line stating
   that its absence changes nothing about the build.
2. The Notes line SHALL cite the measured ~59% figure from this document's
   Introduction (with the same "best case, nothing changed" caveat) rather
   than an unqualified "makes builds faster" claim, so a reader isn't misled
   into expecting that reduction on every build regardless of what changed.
3. No other documentation file (`README.md`, `packer/ca_cluster_node/README.md`,
   etc.) SHALL be touched by this spec — ccache is a build-tool concern
   scoped to `DEPENDENCIES.md`, not a deployment or usage concern.

---

### Requirement 7: Verify the real wiring, not just the plan

**User Story:** As the person who ran the throwaway PR #49 experiment
specifically because an *estimate* wasn't good enough, I want the actual
shipped CI wiring to be measured too, not merely assumed to reproduce the
experiment's numbers.

#### Acceptance Criteria

1. Once Requirements 1, 3, and 4 are implemented and merged, the first two
   consecutive pushes to any branch that don't touch `include/`, `src/`,
   `cmd/`, `tests/`, or `examples/` (i.e., a same-shape scenario to the PR
   #49 experiment: nothing in the compiled tree changes between the two
   runs) SHALL be used to read the `build-and-test` (`clang++-18` leg) job's
   real "Build" step duration on each of the two runs directly from the
   Actions UI/API.
2. The second run's Build-step duration SHALL be recorded in this spec's
   `tasks.md` (or a follow-up note in `doc/TODO.md`) alongside the original
   29m07s/11m59s baseline from PR #49, as either a confirmation or a
   discrepancy to investigate — this requirement exists specifically to
   catch a misconfigured cache key (Requirement 4.3) that would silently
   leave every run cold despite the feature "being wired up."
3. This verification is explicitly informational/observational — it SHALL
   NOT be encoded as a hard-failing CI assertion (build-time varies with
   runner contention, and this project already gives its own coverage-floor
   check a tolerance band for exactly that kind of noise).

---

### Requirement 8: Explicit non-goals

**User Story:** As a future reader of this spec, I want to know what was
deliberately left out so I don't assume it was overlooked.

#### Acceptance Criteria

1. The `docker/ca_cluster_node/Dockerfile` `builder` stage's compile (which
   `packer/ca_cluster_node/scripts/extract-binary.sh` drives via `docker
   build`) SHALL NOT be modified by this spec. It is a separate build
   mechanism (a container image build, not a host `cmake` invocation); Docker
   layer caching already provides some of the same benefit for that path
   independently, and wiring ccache into it (e.g. via a BuildKit cache mount)
   is a candidate follow-up, not part of this spec.
2. A remote/shared ccache backend (e.g. an S3-backed `sccache`, or a
   ccache secondary storage server shared across contributors' machines and
   CI) SHALL NOT be implemented by this spec. Every cache in this spec is
   local-disk-only, either a developer's own filesystem or one CI job's
   `actions/cache`-restored directory — this is deliberately the simplest
   version that captures the measured benefit; a shared backend is a
   candidate follow-up if the per-branch/per-runner cache isolation this
   spec produces turns out to be too narrow in practice.
3. `ci.yml`'s `docs` job (Doxygen generation) and `packer-ca-cluster-node`
   job SHALL NOT be touched — neither compiles this project's C++ code.
