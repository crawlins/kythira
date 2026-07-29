# Design Document

## Overview

This document describes the design for code coverage measurement and ratchet
enforcement in Kythira. Coverage is collected via **Clang's LLVM source-based
instrumentation** (`-fprofile-instr-generate -fcoverage-mapping`) against a
separate build (`build-coverage/`), reported via `llvm-cov`, and enforced both
by a Git pre-commit hook and by a dedicated CI job — both compare against a
non-decreasing line-coverage floor recorded in `coverage_floor.txt`.

> **Note on mechanism**: this spec originally targeted gcov/lcov over a
> GCC-instrumented build. That approach was replaced in commit `bd5e1bb`
> ("build: switch coverage measurement to LLVM source-based instrumentation").
> gcov attributes template function bodies to whichever translation unit wins
> the COMDAT merge, leaving clones in every other TU at zero count — for a
> template-heavy header like `raft.hpp` this understated coverage by roughly
> 10 points despite the code being well-exercised. LLVM's source-based
> coverage instruments at the AST level, before inlining and before the
> linker runs, so every template instantiation in every TU is tracked
> independently. Everything below describes the current, implemented
> mechanism; see git history for the retired lcov design if it's ever needed
> for reference.

## Architecture

```
Developer workflow
──────────────────
git commit
    └── .git/hooks/pre-commit  (scripts/pre-commit-coverage.sh)
            ├── require clang++/llvm-profdata/llvm-cov (skip check if absent)
            ├── configure build-coverage/ with -DENABLE_COVERAGE=ON,
            │   forcing the clang toolchain (reconfigures if the cache was
            │   built with a different compiler)
            ├── cmake --build build-coverage   (incremental instrumented build)
            ├── delete stale *.profraw
            ├── LLVM_PROFILE_FILE=%p-%m.profraw ctest -LE "^(slow|performance|verbose|benchmark|docker)$"
            │       --repeat until-pass:3   (fast subset; retries absorb flakes)
            ├── llvm-profdata merge -sparse *.profraw -o merged.profdata
            ├── llvm-cov report --instr-profile=merged.profdata <bins> --ignore-filename-regex=...
            ├── compare TOTAL line-coverage % to coverage_floor.txt
            │       ├── [lower]  → abort commit, print shortfall
            │       ├── [equal]  → allow commit unchanged
            │       └── [higher] → update coverage_floor.txt, git add, allow commit
            └── exit 0 / exit 1

Developer ad-hoc
────────────────
cmake --build build-coverage --target coverage        (full suite, text summary via llvm-cov report)
cmake --build build-coverage --target coverage-html   (full suite, HTML report via llvm-cov show)
cmake --build build-coverage --target coverage-reset  (delete *.profraw only)

CI (.github/workflows/ci.yml, "Coverage (clang++-18)" job)
───────────────────────────────────────────────────────────
configure build-coverage (clang++-18, ENABLE_COVERAGE=ON)
    → build → ctest (full suite, JUnit output)
    → llvm-profdata merge → llvm-cov report (+ llvm-cov show for HTML artifact)
    → compare to coverage_floor.txt (soft-fail tolerance for measurement noise)
    → job summary + PR comment with the coverage table
```

## Components and Interfaces

### 1. CMake Integration (`CMakeLists.txt`)

A CMake option `ENABLE_COVERAGE` (default `OFF`, pre-seedable from Kconfig via
`KCONFIG_COVERAGE`) gates the coverage instrumentation. When `ON`:

- The compiler is required to be Clang (`CMAKE_CXX_COMPILER_ID MATCHES
  "Clang"`); configure fails with a `FATAL_ERROR` pointing at
  `-DCMAKE_CXX_COMPILER=clang++` otherwise. Source-based coverage is a
  Clang/LLVM feature — there is no GCC equivalent path.
- The build type defaults to `Debug` if none is specified.
- Compile flags gain `-fprofile-instr-generate -fcoverage-mapping`; the
  executable/shared linker flags gain `-fprofile-instr-generate` plus
  `-latomic` (Clang against GCC's libstdc++ does not provide
  `__atomic_is_lock_free` as a compiler builtin the way GCC does, so
  `libatomic` must be linked explicitly).
- `find_program` locates `llvm-profdata`/`llvm-cov` (preferring the
  versioned `-18` binaries, falling back to unversioned); if either is
  missing, the three coverage targets are replaced with stubs that print an
  actionable error and fail (`${CMAKE_COMMAND} -E false`).

Three custom targets are defined (`CMakeLists.txt:857-939`):

- **`coverage-reset`** — deletes all `*.profraw` files under the build
  directory without rebuilding or re-running tests.
- **`coverage`** — deletes stale `*.profraw`; runs the full CTest suite with
  `LLVM_PROFILE_FILE=<dir>/%p-%m.profraw` so every test process (including
  multi-process fixtures) writes its own uniquely named raw profile; invokes
  `cmake/llvm_coverage.cmake` in `MODE=report`, which merges profiles with
  `llvm-profdata merge -sparse` and prints a filtered `llvm-cov report`
  summary.
- **`coverage-html`** — same as `coverage`, but `MODE=html` runs `llvm-cov
  show --format=html` to produce a browsable report tree under
  `build-coverage/coverage-report/`.

### 2. `cmake/llvm_coverage.cmake`

A standalone `cmake -P` script (not inlined into `CMakeLists.txt`) so that
shell-variable escaping never has to cross a `add_custom_target(COMMAND ...)`
boundary. Takes `BUILD_DIR`, `LLVM_PROFDATA`, `LLVM_COV`, `PROFDATA`, `MODE`,
`HTML_DIR`, and `IGNORE_LIST` (semicolon-separated regexes) as `-D`
arguments. Collects `*.profraw` via `file(GLOB_RECURSE)`, merges them,
collects test binaries by filtering extensionless files out of
`${BUILD_DIR}/tests/`, and dispatches to `llvm-cov report` or `llvm-cov show`
per `MODE`.

### 3. Pre-Commit Hook (`scripts/pre-commit-coverage.sh`)

The canonical hook implementation, combined with format (`clang-format`) and
opt-in static-analysis (`clang-tidy`) checks ahead of the coverage stage.
`scripts/install-hooks.sh` symlinks it to `.git/hooks/pre-commit`.

Coverage stage flow:
1. `SKIP_COVERAGE_CHECK=1` escape hatch — skip entirely, exit 0.
2. Require `llvm-profdata`, `llvm-cov`, and `clang++` (preferring `-18`
   suffixed binaries); warn and skip (exit 0) if any are absent, so
   contributors without LLVM installed are never blocked.
3. Configure `build-coverage/` if its cache is missing, or reconfigure it
   from scratch if the existing cache was built with a non-Clang compiler
   (detected by grepping `CMAKE_CXX_COMPILER` out of `CMakeCache.txt`).
   Inherits `CMAKE_PREFIX_PATH` from the primary `build/` cache so it finds
   the same vcpkg-resolved dependencies.
4. Incremental `cmake --build build-coverage`.
5. Delete stale `*.profraw`, then run
   `ctest -LE '^(slow|performance|verbose|benchmark|docker)$' --repeat
   until-pass:3` (label-anchored to avoid accidental substring matches —
   see commit `089927a`) with `LLVM_PROFILE_FILE` set per-process.
6. Merge profiles with `llvm-profdata merge -sparse` (with
   `DEBUGINFOD_URLS=""` to prevent network stalls from an environment-wide
   debuginfod configuration — see commit `01fb9d6`), then run `llvm-cov
   report` and extract the `TOTAL` row's line-coverage column (`$7`) via
   `awk`.
7. Compare against `coverage_floor.txt` (default `0.0` if absent) using
   `awk` for portable float comparison; raise-and-stage, allow-unchanged, or
   abort-with-shortfall-box as appropriate.
8. Print elapsed time; exit 0 or 1.

### 4. `scripts/install-hooks.sh`

Symlinks `scripts/pre-commit-coverage.sh` to `.git/hooks/pre-commit`, making
it executable. Refuses to overwrite a pre-existing non-symlink hook (prints a
warning and exits 1) so it never silently clobbers a developer's own hook.

### 5. CI Integration (`.github/workflows/ci.yml`, job `coverage`)

A dedicated `coverage` job (name: "Coverage (clang++-18)") runs alongside the
matrix build/test jobs:
1. Configure `build-coverage` with `-DENABLE_COVERAGE=ON` and
   `clang++-18`/`clang-18` (ccache-backed, keyed separately from the main
   build's cache).
2. Build, then run the **full** CTest suite (no label filtering — CI budget
   is not constrained the way a pre-commit hook is) with per-process
   `.profraw` output; publish JUnit results as a GitHub check and artifact;
   fail the job outright if any test failed, independent of coverage.
3. Merge profiles, run `llvm-cov report` for the summary percentage and
   `llvm-cov show --format=html` for a downloadable HTML artifact.
4. Compare the measured percentage to `coverage_floor.txt`, write a job
   summary table, and post/update a PR comment (keyed by the bot's own prior
   comment, so re-runs edit in place rather than piling up).

## Data Models

### `coverage_floor.txt`

A plain-text file at the repository root containing a single floating-point
line, e.g.:
```
88.99
```
One decimal of precision (matching `llvm-cov report`'s own output), stored in
version control so the threshold is visible in code review and travels with
every branch. Read with a default of `0.0` if the file is absent.

### Build Directory Layout

```
build/                        # normal optimised build (unchanged)
build-coverage/               # coverage-instrumented debug build (Clang only)
    ├── CMakeCache.txt
    ├── *.profraw             # one per test process, deleted before each run
    ├── merged.profdata       # llvm-profdata merge output
    └── coverage-report/      # llvm-cov show --format=html tree (coverage-html target)
        └── index.html
```
`build-coverage/` is listed in `.gitignore`.

## Correctness Properties

*A property is a characteristic or behavior that should hold true across
all valid executions of a system.*

**Property 1: Floor Monotonicity**
*For any* sequence of commits made through the pre-commit hook,
`coverage_floor.txt`'s value should never decrease — a measurement below
the floor aborts the commit before the file is touched; a measurement at or
above it either leaves the file untouched or raises it to the new value
**Validates: Requirements 3.2, 3.3, 3.4**

**Property 2: No Silent Skip on Real Regressions**
*For any* commit, the coverage stage should only pass without measuring
when `SKIP_COVERAGE_CHECK=1` is explicitly set or the LLVM toolchain is
genuinely absent — in the latter case CI should still measure
unconditionally on every push, so a regression cannot merge unnoticed even
if every local contributor skips the hook
**Validates: Requirements 4.5, 4.6**

**Property 3: Instrumentation Isolation**
*For any* build configured without `-DENABLE_COVERAGE=ON`, no coverage
instrumentation flags or profiling overhead should appear in its compiler or
linker invocations — coverage flags are only ever added to
`build-coverage/`'s configuration, never to `build/`, and the two
directories should share no object files
**Validates: Requirement 1.3**

**Property 4: Test-Failure Precedence**
*For any* commit where the coverage-stage test run fails, the commit should
be aborted before coverage is measured at all, regardless of what the
resulting percentage would have been — a red build should never "buy" a
passing commit via a lucky coverage number
**Validates: Requirement 4.4**

## Error Handling

- **Missing LLVM toolchain** (`clang++`/`llvm-profdata`/`llvm-cov` not
  found): the hook prints a warning naming the missing tool and the install
  command, then exits 0 (allows the commit). CMake's coverage targets take
  the analogous path — stub targets that print an actionable error and fail
  only if a developer explicitly invokes them.
- **Wrong-compiler cache reuse**: if `build-coverage/CMakeCache.txt` exists
  but was configured with a non-Clang compiler, the hook detects this (by
  reading the cached `CMAKE_CXX_COMPILER` entry) and transparently deletes
  and reconfigures the directory, rather than failing with a confusing
  Clang-flag-on-GCC compile error.
- **Failing tests**: `ctest` failure aborts the commit immediately with a
  clear message and the skip-hatch reminder; coverage is never measured in
  this case.
- **Unparseable coverage output**: if the `TOTAL` row can't be extracted
  from `llvm-cov report` (e.g., a tool version change reformats the table),
  the hook warns and skips the ratchet rather than failing closed on a
  parsing bug unrelated to actual coverage.
- **debuginfod network stalls**: `DEBUGINFOD_URLS=""` is forced for every
  `llvm-profdata`/`llvm-cov` invocation, since an environment-wide
  debuginfod configuration otherwise turns a ~2s local report into a
  60+ minute hang (see commit `01fb9d6`) — these are locally built binaries
  with embedded debug info, so remote symbol fetching is never needed.

## Testing Strategy

- **Ratchet rejection path**: manually verified by setting the floor above
  the measured percentage and confirming the hook prints the shortfall box
  and exits non-zero (spec Task 16).
- **Ratchet raise path**: exercised for real on essentially every commit
  that adds test coverage — `coverage_floor.txt`'s own git history (e.g.
  `2c16503`, `791ae6a`, `82fab61`, `f616679`) is a continuous record of the
  hook's raise-and-stage branch running correctly in production, a stronger
  guarantee than a single synthetic test (spec Task 15).
- **Unchanged path**: verified by committing with no coverage-affecting
  changes and confirming the hook reports "Unchanged at N%" and exits 0
  (spec Task 14).
- **CMake target discoverability**: `cmake --build build-coverage --target
  help` confirmed to list `coverage`, `coverage-html`, and `coverage-reset`
  (spec Task 20).
- **CI parity**: the CI `coverage` job runs the identical measurement
  pipeline (llvm-profdata/llvm-cov) against the full test suite, providing
  an independent, unconditional check that doesn't depend on any local
  hook being installed or its escape hatches being left alone.
