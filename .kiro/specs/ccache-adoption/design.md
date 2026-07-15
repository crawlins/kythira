# Design Document

## Overview

This document describes the design for wiring [ccache](https://ccache.dev/)
into this project's CMake build so that both a local developer machine and
every relevant CI job get it automatically, with zero effect when ccache
isn't installed.

The design has exactly one moving part that matters for "does this work at
all" — a `find_program` auto-detection block in the root `CMakeLists.txt`
(Component 1) — and one moving part that matters for "does CI actually see
the benefit" — persisting each build config's ccache directory across
otherwise-stateless CI runs (Component 3). Everything else (documentation,
sizing, verification) is secondary to those two.

## Architecture

```
Local developer machine                     CI (GitHub Actions)
┌─────────────────────────┐                 ┌──────────────────────────────┐
│ ccache installed?        │                 │ apt-get install ... ccache    │
│  (apt/brew, out-of-band) │                 │  (Requirement 3)              │
└──────────┬────────────────┘                 └──────────────┬────────────────┘
           │                                                  │
           ▼                                                  ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ CMakeLists.txt: KYTHIRA_ENABLE_CCACHE (default ON) + find_program(ccache) │
│   found  → CMAKE_C/CXX_COMPILER_LAUNCHER = ccache   (Requirement 1)      │
│   absent → build proceeds exactly as today                              │
└──────────┬────────────────────────────────────────────────┬─────────────┘
           │                                                  │
           ▼                                                  ▼
   ~/.ccache persists                          actions/cache/restore (Requirement 4)
   naturally on disk                                    │
   between builds —                                      ▼
   nothing else needed                          cmake configure + build
                                                          │
                                                          ▼
                                                actions/cache/save, if: always()
                                                (captures this run's new
                                                 objects for the NEXT run)
```

## Components and Interfaces

### 1. `CMakeLists.txt` — auto-detection (Requirement 1)

Placed immediately after the existing C++ standard / `CMAKE_EXPORT_COMPILE_COMMANDS`
block (root `CMakeLists.txt`, currently ending at line 10) and before the
`ENABLE_COVERAGE` block (currently starting at line 12) — early enough that
`CMAKE_C_COMPILER_LAUNCHER`/`CMAKE_CXX_COMPILER_LAUNCHER` are set before any
compiler-touching logic runs, matching CMake's own guidance that launcher
variables must be set before the first target that uses them is defined
(this project's targets are all defined in subdirectories added later via
`add_subdirectory`, so "early in the top-level file" is sufficient — no
`project()` re-ordering is needed since `project()` on line 2 already ran).

```cmake
# ccache: speeds up rebuilds by skipping recompilation of translation units
# whose preprocessed content + compiler flags exactly match a prior compile.
# Purely opt-in acceleration — absent or disabled, the build is identical to
# today. See .kiro/specs/ccache-adoption/ for the measured impact (~59%
# reduction in a from-scratch "nothing changed" rebuild;  real numbers from
# a throwaway CI experiment, not a guess).
option(KYTHIRA_ENABLE_CCACHE "Use ccache to speed up rebuilds when available" ON)

if(KYTHIRA_ENABLE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "" FORCE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "" FORCE)
        message(STATUS "ccache: enabled (${CCACHE_PROGRAM})")
    else()
        message(STATUS "ccache: not found, building without it")
    endif()
else()
    message(STATUS "ccache: disabled (KYTHIRA_ENABLE_CCACHE=OFF)")
endif()
```

Nothing downstream references `CCACHE_PROGRAM` or `KYTHIRA_ENABLE_CCACHE` —
this is the entire mechanism. No target, test, or `find_package` call
changes. This mirrors the project's existing optional-dependency pattern
(`KYTHIRA_HAS_OPENSSL`, `KYTHIRA_HAS_AWS_SDK`, etc.) except that ccache is a
*launcher*, not a linked library, so the detection is `find_program` rather
than `find_package` and there is no corresponding `#ifdef` anywhere in C++
source — its absence changes build *speed*, never build *graph*.

### 2. `DEPENDENCIES.md` — documentation (Requirement 6)

New entry in the existing "Optional Dependencies" section, following the
established format exactly:

```markdown
### ccache — faster rebuilds
- **Status**: Optional — auto-detected via `find_program(ccache)`; absent,
  the build is identical to today (`KYTHIRA_ENABLE_CCACHE=OFF` to force off
  even when installed)
- **Purpose**: Skips recompiling a translation unit whose preprocessed
  content and compiler flags exactly match a prior compile. Measured on this
  project's own CI: a from-scratch rebuild where nothing changed since the
  last build dropped from 29m07s to 11m59s (~59% reduction) — the remaining
  time is link time, which ccache cannot cache; a build that touches a
  widely-included header will see less benefit than this best case.
- **Installation**:
  ```bash
  # Ubuntu/Debian
  sudo apt install ccache

  # macOS (Homebrew)
  brew install ccache
  ```
- **Notes**: No `CCACHE_DIR`/`--max-size` configuration is required for
  local use — ccache's own defaults (`~/.ccache`, 5 GB) apply. CI uses
  smaller, explicit `--max-size` values scoped to each job's own disk budget
  (`.kiro/specs/ccache-adoption/`); that sizing is CI-only and not relevant
  to local use.
```

### 3. CI persistence — `build-and-test` (Requirement 3, 4, 5)

`.github/workflows/ci.yml`, `build-and-test` job. Two changes:

**a. Package install** — add `ccache` to the existing list (line 32-40):

```yaml
      - name: Install system dependencies
        run: |
          sudo apt-get update -q
          sudo apt-get install -y --no-install-recommends \
            g++-13 \
            clang-18 \
            cmake \
            ninja-build \
            libfiu-dev \
            ccache
```

**b. Cache restore/save**, inserted around the existing "Configure (Release)"
/ "Build" steps (currently lines 92-101). The restore step comes *before*
Configure (so the launcher activates against a warm cache); the save step
comes *after* Build, `if: always()`:

```yaml
      - name: Restore ccache
        uses: actions/cache/restore@v4
        with:
          path: ~/.ccache
          key: ccache-${{ runner.os }}-${{ matrix.compiler }}-${{ github.ref_name }}-${{ github.run_id }}
          restore-keys: |
            ccache-${{ runner.os }}-${{ matrix.compiler }}-${{ github.ref_name }}-
            ccache-${{ runner.os }}-${{ matrix.compiler }}-

      - name: ccache size limit
        run: ccache --max-size=2G

      - name: Configure (Release)
        run: |
          cmake -B build -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_COMPILER=${{ matrix.compiler }} \
            -DCMAKE_PREFIX_PATH=${{ github.workspace }}/vcpkg_installed/x64-linux \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
      # (unchanged — no -DCMAKE_..._LAUNCHER flag; CMakeLists.txt auto-detects)

      - name: Build
        run: cmake --build build -j$(nproc)
      # (unchanged)

      - name: Save ccache
        if: always()
        uses: actions/cache/save@v4
        with:
          path: ~/.ccache
          key: ccache-${{ runner.os }}-${{ matrix.compiler }}-${{ github.ref_name }}-${{ github.run_id }}
```

The `restore-keys` fallback chain is: exact branch's most recent cache →
any cache for this OS+compiler (picks up `main`'s cache on a brand-new
branch, or a differently-named branch's cache — `actions/cache` matches the
most recently created entry with a matching prefix). The **save** key is
always `...-${{ github.run_id }}` — a value that has never existed before —
so `actions/cache/save` never skips the save (unlike the combined
`actions/cache@v4` action used for `vcpkg_installed/`, which intentionally
*does* skip saving on an exact-key hit, correct for vcpkg since that cache
should only change when `vcpkg.json`'s hash changes).

Second matrix leg (`g++-13`) gets the identical treatment — the `key`
already includes `${{ matrix.compiler }}`, so the two legs never share or
collide on cache entries.

### 4. CI persistence — `coverage` (Requirement 3, 4, 5)

Same shape as Component 3, applied to `coverage`'s "Install system
dependencies" (line 171) and around "Configure (Coverage)"/"Build
(Coverage)" (lines 204-213), with two differences:

- Key prefix is `ccache-coverage-${{ runner.os }}-${{ github.ref_name }}-`
  (no compiler matrix here — always `clang++-18` — but still namespaced
  separately from `build-and-test`'s cache since the Debug + coverage-
  instrumentation flags produce entirely different object files that would
  never hit against a Release cache anyway; separate prefixes just keep the
  two `actions/cache` entries' sizes independently bounded).
- `ccache --max-size=1G` (not `2G`) — this job has documented, repeated
  "No space left on device" history (see the "Free disk space" step's own
  comment at line 160, predating this spec). The restore step is placed
  *after* "Free disk space" so the reclaimed headroom exists before the
  cache download consumes any of it, and a comment cross-references the two
  steps' shared, constrained budget.

### 5. CI persistence — `real-cloud-tests.yml`, `aws` job (Requirement 3, 4, 5)

Same shape again, applied to the `aws` job's "Install system dependencies"
and around its "Configure (Release, real-cloud tests enabled)"/"Build"
steps. Key prefix: `ccache-realcloud-${{ runner.os }}-${{ github.ref_name }}-`,
`--max-size=2G`. Lower priority than Components 3-4 in the implementation
order (Requirement 3.1 lists it last) since this workflow only runs weekly
or on manual dispatch — the cache-reuse window between runs is much longer,
so the practical benefit is smaller, but the mechanism is identical and
costs nothing extra to wire up consistently.

## Data Models

### Cache key namespaces

```
build-and-test (matrix):  ccache-{os}-{compiler}-{ref}-{run_id}     max-size 2G
                           restore-keys: ccache-{os}-{compiler}-{ref}-
                                         ccache-{os}-{compiler}-

coverage:                  ccache-coverage-{os}-{ref}-{run_id}       max-size 1G
                           restore-keys: ccache-coverage-{os}-{ref}-
                                         ccache-coverage-{os}-

real-cloud-tests (aws):    ccache-realcloud-{os}-{ref}-{run_id}      max-size 2G
                           restore-keys: ccache-realcloud-{os}-{ref}-
                                         ccache-realcloud-{os}-
```

No key includes a source-content hash (unlike `vcpkg_installed/`'s
`hashFiles('vcpkg.json', ...)` key) — deliberately, since the entire point is
for the cache to survive source changes and accumulate useful entries over
time, evicting only via ccache's own internal LRU once `--max-size` is
reached.

## Correctness Properties

### Property 1: Absence is a no-op
**Validates: Requirements 1.3, 2**

A build with `ccache` not installed, or with `KYTHIRA_ENABLE_CCACHE=OFF`,
produces the identical `build/` target graph, `ctest` results, and coverage
percentage as a build with ccache active — the only difference is wall-clock
time. This follows directly from `CMAKE_..._COMPILER_LAUNCHER` being a
pure pass-through wrapper: `ccache <compiler> <args>` either serves a cached
object file that is byte-identical to what `<compiler> <args>` alone would
have produced, or invokes `<compiler> <args>` itself and caches the result —
there is no code path where ccache's presence changes what gets compiled.

### Property 2: The 59% figure is a ceiling, not a guarantee
**Validates: Requirement 7**

The measured 29m07s → 11m59s reduction (PR #49) is specifically the
*best case*: a rebuild where zero files changed. 53% of every compiler-
launcher invocation is a link step that ccache cannot accelerate at all
(`Uncacheable calls: 212/399`), and any push that touches a widely-included
header (e.g. `include/raft/types.hpp`) invalidates every translation unit
that includes it, pushing the realized speedup toward 0% for that specific
push. Requirement 7's re-measurement step exists to catch the failure mode
where the feature *looks* wired up (ccache installed, `CMAKE_..._LAUNCHER`
set) but the CI cache key never actually restores anything (e.g. a typo in
`restore-keys`, or `actions/cache`'s per-repository 10 GB total eviction
silently dropping entries) — in which case every run stays cold and 0% of
this spec's benefit materializes despite every requirement above being
"implemented."

### Property 3: CI cache growth is bounded independent of vcpkg's cache
**Validates: Requirement 5**

ccache's cache and `vcpkg_installed/`'s cache are separate `actions/cache`
paths with independent size budgets (`--max-size` for ccache; vcpkg's cache
size is whatever `vcpkg install` produces, unbounded by this spec). A
regression in one cannot silently consume the other's headroom — this
matters specifically because `coverage`'s prior "No space left on device"
incidents were caused by disk pressure from statically-linked binaries
*during the build itself* (not from `actions/cache`'s restore step), so
this property is scoped narrowly: it guarantees ccache's own restored
directory has a known upper bound (1-2 GB depending on job), not that the
build's peak binary-output disk usage is unaffected (it isn't — that's
orthogonal to this spec, already addressed by the existing "Free disk
space" step).

## Error Handling

- **`ccache` binary present but broken/misconfigured** (e.g. a corrupted
  cache directory): `find_program` only checks for the binary's existence,
  not that it's functional. If a broken ccache install actually causes build
  failures rather than just cache misses, the fix is
  `-DKYTHIRA_ENABLE_CCACHE=OFF` (Requirement 1.4) as an immediate escape
  hatch — no code change required to work around a broken local install.
- **`actions/cache/restore` finds nothing** (first run ever for a given key
  prefix, or the cache was evicted): the step simply completes with no cache
  restored; ccache starts cold (equivalent to today's behavior) and the
  subsequent `save` step populates the cache for next time. Not an error.
- **`actions/cache/save` fails** (e.g. GitHub Actions cache service
  transient error): `if: always()` on the save step means this is
  best-effort — a failed save just means the next run starts from whatever
  the last *successful* save produced, degrading gracefully back toward
  Requirement 7 Property 2's "cold" case rather than failing the job.
- **ccache's `--max-size` is exceeded mid-job**: ccache evicts its own
  least-recently-used entries automatically; this is invisible to the CI job
  (no error, just a lower future hit rate if the working set genuinely
  exceeds the configured size).

## Testing Strategy

There is no new C++ test surface — this spec changes build tooling, not
library or application behavior. Verification is:

1. **Property 1 (Requirement 2)**: a one-time manual/CI check — configure
   and build twice (`KYTHIRA_ENABLE_CCACHE=OFF` then `=ON`) from clean build
   directories on a machine with `ccache` installed, diff the resulting
   `build/` target list (e.g. `ninja -C build -t targets` output) and
   confirm `ctest` results are unchanged. This is a one-off confirmation,
   not a permanent CTest target — there is nothing here that could regress
   silently without someone editing `CMakeLists.txt`'s ccache block directly,
   at which point the reviewer of that change is the check.
2. **Requirement 7's re-measurement**: read real Actions UI/API timing from
   the first two eligible consecutive pushes after this spec merges (same
   methodology as the PR #49 experiment, but on the real, permanent CI
   configuration rather than a throwaway job) and record the result.
3. **Static config validation**: `python3 -c "import yaml; yaml.safe_load(...)"`
   against both modified workflow files (same check already used for the
   `ca-cluster-node-ami` spec's workflow changes) before pushing — catches
   YAML syntax errors without needing a real CI round-trip.

## Dependencies

```
ccache ≥ 4.0        Optional. apt/brew. Auto-detected via find_program;
                     absent, the build is identical to today.
actions/cache@v4     Already used (vcpkg_installed/); this spec uses its
                     split restore/save form (actions/cache/restore@v4,
                     actions/cache/save@v4) for ccache specifically, not the
                     combined action.
```

No new C++ library dependency, no `vcpkg.json` change.
