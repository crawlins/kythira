# Requirements Document

## Introduction

This document specifies the requirements for integrating code coverage measurement
into the Kythira build system and enforcing a non-decreasing coverage threshold
through a Git pre-commit hook.

The goal is twofold: provide developers with actionable coverage data during
development, and guarantee that coverage never silently regresses as new code is
committed. The ratchet mechanism achieves this by recording the current coverage
percentage in a version-controlled file and rejecting any commit that would lower
it.

Coverage is measured with gcov/lcov over the GCC-instrumented test build. The
instrumented build lives in a separate directory (`build-coverage/`) so it does not
interfere with the normal optimised build in `build/`.

## Glossary

- **Coverage_Build**: A CMake build configured with `ENABLE_COVERAGE=ON` that
  compiles all sources with `--coverage` instrumentation.
- **Coverage_Floor**: The minimum acceptable line-coverage percentage, stored in
  `coverage_floor.txt` at the repository root and committed alongside source code.
- **Ratchet**: The rule that the Coverage_Floor may only move upward; a commit
  that would reduce it is rejected by the pre-commit hook.
- **Line_Coverage**: The percentage of executable source lines exercised by at
  least one test, as reported by lcov.
- **Coverage_Report**: The lcov HTML report tree generated under
  `build-coverage/coverage-report/`.
- **Pre_Commit_Hook**: The Git hook script installed at `.git/hooks/pre-commit`
  that enforces the ratchet before each commit.

## Requirements

### Requirement 1

**User Story:** As a developer, I want a dedicated coverage build so that I can
measure test coverage without affecting the normal optimised build.

#### Acceptance Criteria

1. WHEN CMake is configured with `-DENABLE_COVERAGE=ON` THEN the system SHALL
   compile all library and test sources with the GCC `--coverage` flag and link
   with `-lgcov`.
2. WHEN `ENABLE_COVERAGE=ON` THEN the default build type SHALL be `Debug` so that
   inlining and optimisation do not distort coverage data.
3. WHEN coverage is disabled (default) THEN the build SHALL produce no coverage
   instrumentation and no performance penalty.
4. WHEN a coverage build is configured THEN the system SHALL warn if the compiler
   is not GCC or Clang, as coverage instrumentation is toolchain-specific.

### Requirement 2

**User Story:** As a developer, I want CMake targets to run the tests and generate
coverage reports so that I can measure coverage with a single command.

#### Acceptance Criteria

1. WHEN the `coverage` target is invoked THEN the system SHALL reset stale gcov
   counters, run the full CTest suite, capture coverage data with lcov, filter
   out third-party and system paths, and print a line-coverage summary to stdout.
2. WHEN the `coverage-html` target is invoked THEN the system SHALL perform all
   steps of the `coverage` target and additionally produce a full HTML report tree
   under `build-coverage/coverage-report/` using genhtml.
3. WHEN the `coverage-reset` target is invoked THEN the system SHALL zero all gcov
   counter files without rebuilding or re-running tests.
4. WHEN lcov captures coverage data THEN the system SHALL exclude the following
   paths from the report:
   - `build-coverage/` (generated files)
   - `vcpkg_installed/` (third-party libraries)
   - `/usr/` (system and compiler headers)
5. WHEN required tools (`lcov`, `genhtml`) are not found THEN CMake SHALL emit a
   clear error message and skip defining the coverage targets.

### Requirement 3

**User Story:** As a developer, I want a committed ratchet file so that the
minimum acceptable coverage is visible in version history and enforced
consistently across all contributors.

#### Acceptance Criteria

1. WHEN the repository is cloned THEN the file `coverage_floor.txt` SHALL exist at
   the repository root and SHALL contain a single floating-point number representing
   the current Coverage_Floor as a percentage (e.g., `78.5`).
2. WHEN a commit raises coverage above the current floor THEN the pre-commit hook
   SHALL update `coverage_floor.txt` to the new percentage and stage the updated
   file as part of the commit.
3. WHEN a commit leaves coverage unchanged THEN the pre-commit hook SHALL allow the
   commit to proceed and SHALL NOT modify `coverage_floor.txt`.
4. WHEN a commit would lower coverage below the current floor THEN the pre-commit
   hook SHALL print the old floor, the new measurement, and the shortfall, and
   SHALL exit with a non-zero status to abort the commit.
5. WHEN `coverage_floor.txt` is absent THEN the pre-commit hook SHALL treat the
   floor as `0.0` and create the file with the measured coverage before allowing
   the commit.

### Requirement 4

**User Story:** As a developer, I want a pre-commit hook that automatically
enforces the ratchet so that coverage regressions are caught locally before push.

#### Acceptance Criteria

1. WHEN `scripts/install-hooks.sh` is executed THEN the system SHALL install the
   pre-commit hook at `.git/hooks/pre-commit` and make it executable.
2. WHEN the pre-commit hook runs THEN it SHALL build the coverage-instrumented
   binary, run the CTest suite, capture coverage, and apply the ratchet check —
   all without requiring manual steps from the developer.
3. WHEN the hook detects that `build-coverage/` does not exist THEN it SHALL
   configure it automatically using the current CMake settings before building.
4. WHEN the CTest suite fails THEN the pre-commit hook SHALL abort the commit
   regardless of coverage percentage, because broken tests must be fixed before
   coverage is meaningful.
5. WHEN the environment variable `SKIP_COVERAGE_CHECK=1` is set THEN the
   pre-commit hook SHALL skip the coverage check and exit with status 0, allowing
   WIP commits without running the full instrumented suite.
6. WHEN the hook is run on a machine without `lcov` THEN it SHALL print a clear
   warning and skip the coverage check rather than blocking the commit with a
   confusing error.

### Requirement 5

**User Story:** As a developer, I want the coverage check to run quickly enough
to be practical as a pre-commit step.

#### Acceptance Criteria

1. WHEN the pre-commit hook runs the test suite THEN it SHALL use all available
   CPU cores (`-j$(nproc)`) to minimise wall-clock time.
2. WHEN the pre-commit hook runs the test suite THEN it SHALL exclude tests labelled
   `slow`, `performance`, or `verbose` by default, as these are covered by CI and
   their execution time is prohibitive in a commit workflow.
3. WHEN a developer wants to verify full-suite coverage locally THEN they SHALL be
   able to run `cmake --build build-coverage --target coverage` directly, which
   runs without label filtering.
4. WHEN the hook finishes THEN it SHALL print the elapsed time so developers can
   monitor hook overhead over time.

### Requirement 6

**User Story:** As a project maintainer, I want the coverage infrastructure to be
documented so that new contributors can understand and use it.

#### Acceptance Criteria

1. WHEN a developer runs `cmake --build build-coverage --target help` THEN the
   `coverage`, `coverage-html`, and `coverage-reset` targets SHALL appear in the
   output.
2. WHEN the coverage spec is implemented THEN the project README SHALL include a
   "Code Coverage" section explaining how to generate reports, how the ratchet
   works, and how to install the pre-commit hook.
3. WHEN `coverage_floor.txt` is updated by the hook THEN the hook SHALL print a
   message such as `Coverage floor raised: 78.5% → 79.2%` so developers see the
   improvement acknowledged.
