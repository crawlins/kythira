# Design Document

## Overview

This document describes the design for code coverage measurement and ratchet
enforcement in Kythira. Coverage is collected with gcov/lcov against a separate
instrumented build, reported as an HTML tree, and enforced by a Git pre-commit
hook that prevents any commit from lowering the recorded line-coverage floor.

## Architecture

```
Developer workflow
──────────────────
git commit
    └── .git/hooks/pre-commit
            ├── cmake --build build-coverage   (incremental instrumented build)
            ├── ctest -LE "slow|performance|verbose"  (fast subset)
            ├── lcov --capture → filter → summary
            ├── compare to coverage_floor.txt
            │       ├── [lower]  → abort commit, print shortfall
            │       ├── [equal]  → allow commit unchanged
            │       └── [higher] → update coverage_floor.txt, git add, allow commit
            └── exit 0 / exit 1

Developer ad-hoc
────────────────
cmake --build build-coverage --target coverage        (full suite, text summary)
cmake --build build-coverage --target coverage-html   (full suite, HTML report)
```

## Component Design

### 1. CMake Integration (`CMakeLists.txt`)

A new CMake option `ENABLE_COVERAGE` (default `OFF`) gates the coverage
instrumentation. When `ON`:

- The build type defaults to `Debug` if none is specified.
- All targets receive the compile and link flags:
  ```cmake
  -fprofile-arcs -ftest-coverage   # legacy gcov flags (also accepted by Clang)
  --coverage                        # equivalent shorthand; used on link step
  ```
- `find_program` locates `lcov` and `genhtml`; if either is missing, the coverage
  targets are replaced with stubs that print a clear error.

Three custom targets are defined:

#### `coverage-reset`
Zeroes all `.gcda` counter files without rebuilding:
```cmake
add_custom_target(coverage-reset
    COMMAND lcov --zerocounters --directory ${CMAKE_BINARY_DIR}
    COMMENT "Resetting coverage counters"
)
```

#### `coverage`
Depends on building all test targets, then:
1. `lcov --zerocounters` — reset stale data
2. `ctest -j$(nproc)` — run the full suite
3. `lcov --capture --directory . --output-file coverage.info`
4. `lcov --remove coverage.info` to strip exclusions
5. `lcov --summary coverage.info` — print line/branch/function totals

#### `coverage-html`
Extends `coverage` by calling `genhtml coverage.info --output-directory
coverage-report/` to produce a browsable HTML tree. A convenience message
prints the path to `index.html`.

### Exclusion Patterns

lcov `--remove` strips the following path patterns:
```
'*/build-coverage/*'
'*/vcpkg_installed/*'
'/usr/*'
'*/tests/*'        # optional: include or exclude test code per preference
```

The spec leaves test-file inclusion as a build option
(`COVERAGE_INCLUDE_TESTS`, default `OFF`) since some teams prefer to measure only
production code.

### 2. Coverage Floor File (`coverage_floor.txt`)

A plain-text file at the repository root containing a single line:
```
78.5
```

This file is committed to version control. It moves only upward:
- When a new contributor pushes and coverage has grown, they update the file.
- CI rejects PRs where the file would decrease.
- The pre-commit hook automates local enforcement.

The file uses one decimal place (e.g., `78.5`) so that small improvements are
recorded. The hook truncates to one decimal before comparing to avoid
floating-point noise from run-to-run measurement variation (lcov reports to one
decimal).

### 3. Pre-Commit Hook (`scripts/pre-commit-coverage.sh`)

This script is the canonical hook implementation. `scripts/install-hooks.sh`
symlinks or copies it to `.git/hooks/pre-commit`.

```
scripts/
├── install-hooks.sh          # one-time setup; run after clone
└── pre-commit-coverage.sh    # the hook itself
```

#### Hook Flow

```bash
#!/usr/bin/env bash
set -euo pipefail
START=$(date +%s)

# 1. Allow escape hatch for WIP commits
[[ "${SKIP_COVERAGE_CHECK:-0}" == "1" ]] && exit 0

# 2. Require lcov; warn-and-skip if absent
command -v lcov >/dev/null 2>&1 || { echo "WARNING: lcov not found, skipping coverage check"; exit 0; }

# 3. Configure coverage build if needed
COVERAGE_BUILD="$(git rev-parse --show-toplevel)/build-coverage"
if [[ ! -f "$COVERAGE_BUILD/CMakeCache.txt" ]]; then
    cmake -S "$(git rev-parse --show-toplevel)" -B "$COVERAGE_BUILD" \
          -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
fi

# 4. Incremental build
cmake --build "$COVERAGE_BUILD" -j"$(nproc)"

# 5. Run fast test subset (exclude slow/performance/verbose labels)
pushd "$COVERAGE_BUILD" >/dev/null
lcov --zerocounters --directory .
ctest -j"$(nproc)" -LE "slow|performance|verbose" --output-on-failure || {
    echo "ERROR: Tests failed. Fix failing tests before committing."; exit 1
}

# 6. Capture and filter coverage
lcov --capture --directory . \
     --output-file coverage.info --quiet
lcov --remove coverage.info \
     '*/build-coverage/*' '*/vcpkg_installed/*' '/usr/*' \
     --output-file coverage_filtered.info --quiet
popd >/dev/null

# 7. Extract line coverage percentage (one decimal)
NEW_PCT=$(lcov --summary "$COVERAGE_BUILD/coverage_filtered.info" 2>&1 \
    | grep -oP 'lines\.*: \K[0-9]+\.[0-9]')

# 8. Read floor (default 0.0 if absent)
FLOOR_FILE="$(git rev-parse --show-toplevel)/coverage_floor.txt"
OLD_FLOOR=$(cat "$FLOOR_FILE" 2>/dev/null || echo "0.0")

# 9. Compare using awk for portable float arithmetic
RESULT=$(awk -v new="$NEW_PCT" -v old="$OLD_FLOOR" \
    'BEGIN { if (new+0 < old+0) print "below"; else if (new+0 > old+0) print "above"; else print "same" }')

ELAPSED=$(( $(date +%s) - START ))

case "$RESULT" in
  below)
    echo ""
    echo "  COVERAGE RATCHET FAILED"
    echo "  Floor  : ${OLD_FLOOR}%"
    echo "  Current: ${NEW_PCT}%"
    echo "  Shortfall: $(awk -v n="$NEW_PCT" -v o="$OLD_FLOOR" 'BEGIN{printf "%.1f", o-n}')%"
    echo ""
    echo "  Add tests to bring coverage back up before committing."
    echo "  To skip this check for a WIP commit: SKIP_COVERAGE_CHECK=1 git commit"
    echo ""
    exit 1
    ;;
  above)
    echo "$NEW_PCT" > "$FLOOR_FILE"
    git add "$FLOOR_FILE"
    echo "  Coverage floor raised: ${OLD_FLOOR}% → ${NEW_PCT}%"
    ;;
  same)
    echo "  Coverage unchanged at ${NEW_PCT}%"
    ;;
esac

echo "  Coverage check passed in ${ELAPSED}s"
exit 0
```

#### `scripts/install-hooks.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail
REPO=$(git rev-parse --show-toplevel)
HOOK_SRC="$REPO/scripts/pre-commit-coverage.sh"
HOOK_DST="$REPO/.git/hooks/pre-commit"

if [[ -f "$HOOK_DST" && ! -L "$HOOK_DST" ]]; then
    echo "WARNING: $HOOK_DST already exists and is not a symlink."
    echo "Rename or remove it, then re-run this script."
    exit 1
fi

ln -sf "$HOOK_SRC" "$HOOK_DST"
chmod +x "$HOOK_SRC"
echo "Pre-commit coverage hook installed."
```

### 4. Build Directory Layout

```
build/                        # normal optimised build (unchanged)
build-coverage/               # coverage-instrumented debug build
    ├── CMakeCache.txt
    ├── coverage.info          # raw lcov capture
    ├── coverage_filtered.info # after exclusions
    └── coverage-report/       # genhtml HTML tree (from coverage-html target)
        └── index.html
```

`build-coverage/` is listed in `.gitignore`.

### 5. CI Integration

For CI (e.g., GitHub Actions), the full suite without label filtering should run:

```yaml
- name: Configure coverage build
  run: cmake -S . -B build-coverage -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug

- name: Build
  run: cmake --build build-coverage -j$(nproc)

- name: Run tests with coverage
  run: |
    cd build-coverage
    lcov --zerocounters --directory .
    ctest -j$(nproc) --output-on-failure
    lcov --capture --directory . --output-file coverage.info
    lcov --remove coverage.info '*/build-coverage/*' '*/vcpkg_installed/*' '/usr/*' \
         --output-file coverage_filtered.info
    lcov --summary coverage_filtered.info

- name: Enforce ratchet
  run: |
    NEW=$(lcov --summary build-coverage/coverage_filtered.info 2>&1 \
          | grep -oP 'lines\.*: \K[0-9]+\.[0-9]')
    OLD=$(cat coverage_floor.txt 2>/dev/null || echo "0.0")
    awk -v n="$NEW" -v o="$OLD" \
        'BEGIN { if (n+0 < o+0) { print "Coverage decreased: "o"% -> "n"%"; exit 1 } }'
```

## Key Design Decisions

### Separate build directory
The instrumented build and the normal build must not share object files. Using
`build-coverage/` keeps them fully isolated and avoids any chance of accidentally
shipping coverage-instrumented binaries.

### Floor stored in version control
Storing `coverage_floor.txt` in the repository rather than in CI configuration
ensures the threshold is visible in code review and follows every branch. When a
feature branch adds code and tests, it should raise the floor as part of that PR.

### One decimal precision
lcov reports line coverage to one decimal place (e.g., `78.5%`). Storing and
comparing at the same precision avoids spurious floor updates from measurement
noise (e.g., `78.49` vs `78.51` from run-to-run ordering variation).

### Label-filtered pre-commit run
The full Kythira suite takes ~18 minutes. Excluding tests labelled `slow`,
`performance`, and `verbose` reduces this to approximately 3–5 minutes on a modern
workstation — acceptable for a pre-commit gate. The CI job runs the full suite to
enforce the floor on the complete test population.

### Graceful degradation
The hook exits 0 (allowing the commit) if `lcov` is not installed. This prevents
blocking contributors on machines without the tooling, while still enforcing
coverage in environments where the tool is present and in CI.
