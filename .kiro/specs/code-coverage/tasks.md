# Implementation Plan - Code Coverage

## Status: Complete

**Last Updated**: June 9, 2026

## Overview

Implement gcov/lcov-based code coverage measurement with CMake integration,
HTML reporting, and a Git pre-commit hook that enforces a non-decreasing
coverage floor (ratchet). Work is divided into four phases.

---

## Phase 1: CMake Coverage Build (Tasks 1–6)

### Core instrumentation and build isolation

- [ ] 1. Add `ENABLE_COVERAGE` CMake option
  - Add `option(ENABLE_COVERAGE "Build with gcov coverage instrumentation" OFF)` to
    `CMakeLists.txt`
  - When `ON`, force `CMAKE_BUILD_TYPE` to `Debug` if unset
  - Emit a CMake warning if compiler is not GCC or Clang
  - _Requirements: 1.1, 1.2, 1.3, 1.4_
  - _Priority: Critical_

- [ ] 2. Apply coverage compile and link flags
  - When `ENABLE_COVERAGE=ON`, append `-fprofile-arcs -ftest-coverage` to
    `CMAKE_CXX_FLAGS` and `--coverage` to `CMAKE_EXE_LINKER_FLAGS` and
    `CMAKE_SHARED_LINKER_FLAGS`
  - Verify flags propagate to the `network_simulator` interface library and all
    test targets
  - _Requirements: 1.1_
  - _Priority: Critical_

- [ ] 3. Locate required tools with `find_program`
  - Use `find_program(LCOV_EXECUTABLE lcov)` and `find_program(GENHTML_EXECUTABLE genhtml)`
  - If either is missing when `ENABLE_COVERAGE=ON`, issue `message(WARNING ...)` and
    define stub targets that print an actionable error
  - _Requirements: 2.5_
  - _Priority: Critical_

- [ ] 4. Define `coverage-reset` target
  - Custom target that runs `lcov --zerocounters --directory ${CMAKE_BINARY_DIR}`
  - Depends on nothing; can be run at any time
  - _Requirements: 2.3_
  - _Priority: High_

- [ ] 5. Define `coverage` target
  - Depends on all test executables (use a custom target that depends on the
    `tests` umbrella target or enumerate test targets)
  - Steps: zerocounters → `ctest -j$(nproc)` → `lcov --capture` → `lcov --remove`
    for exclusions → `lcov --summary`
  - Exclusion patterns: `'*/build-coverage/*'`, `'*/vcpkg_installed/*'`, `'/usr/*'`
  - Output: `${CMAKE_BINARY_DIR}/coverage_filtered.info`
  - _Requirements: 2.1, 2.4_
  - _Priority: Critical_

- [ ] 6. Define `coverage-html` target
  - Depends on `coverage` target
  - Runs `genhtml coverage_filtered.info --output-directory coverage-report/`
  - Prints path to `coverage-report/index.html` on completion
  - _Requirements: 2.2_
  - _Priority: High_

---

## Phase 2: Ratchet File and Initial Baseline (Tasks 7–10)

### Establish the floor and commit it to version control

- [ ] 7. Add `build-coverage/` to `.gitignore`
  - Append `build-coverage/` to `.gitignore` so instrumented build artifacts are
    never committed
  - _Requirements: (design)_
  - _Priority: High_

- [ ] 8. Run initial coverage measurement
  - Configure `build-coverage/`: `cmake -S . -B build-coverage -DENABLE_COVERAGE=ON`
  - Build and run: `cmake --build build-coverage --target coverage`
  - Record the reported line-coverage percentage
  - _Requirements: 3.1_
  - _Priority: Critical_

- [ ] 9. Create `coverage_floor.txt`
  - Write the measured percentage from task 8 (one decimal, e.g. `78.5`) into
    `coverage_floor.txt` at the repository root
  - Commit the file: this is the starting floor that the ratchet will protect
  - _Requirements: 3.1_
  - _Priority: Critical_

- [ ] 10. Verify ratchet logic manually
  - Run coverage again and confirm the percentage matches `coverage_floor.txt`
  - Temporarily lower the value in the file and confirm a comparison script
    detects the regression before writing the hook
  - _Requirements: 3.2, 3.3, 3.4_
  - _Priority: High_

---

## Phase 3: Pre-Commit Hook (Tasks 11–16)

### Scripts that automate the ratchet at commit time

- [ ] 11. Write `scripts/pre-commit-coverage.sh`
  - Implement the full hook flow from the design doc:
    1. Check `SKIP_COVERAGE_CHECK` escape hatch
    2. Graceful skip if `lcov` is absent
    3. Auto-configure `build-coverage/` if `CMakeCache.txt` is missing
    4. Incremental `cmake --build`
    5. `lcov --zerocounters` + `ctest -LE "slow|performance|verbose" -j$(nproc)`
    6. `lcov --capture` + `lcov --remove` (exclusions)
    7. Extract percentage from `lcov --summary`
    8. Read `coverage_floor.txt` (default `0.0` if absent)
    9. Compare with `awk`; print result; update file and `git add` if raised
    10. Print elapsed time; exit 0 or 1
  - Make the script executable (`chmod +x`)
  - _Requirements: 4.2, 4.3, 4.4, 4.5, 4.6, 5.1, 5.2, 5.4_
  - _Priority: Critical_

- [ ] 12. Write `scripts/install-hooks.sh`
  - Symlink `scripts/pre-commit-coverage.sh` to `.git/hooks/pre-commit`
  - Detect and warn on existing non-symlink hook to avoid silently overwriting
    custom hooks
  - Make both scripts executable
  - _Requirements: 4.1_
  - _Priority: Critical_

- [ ] 13. Add `SKIP_COVERAGE_CHECK` escape hatch documentation
  - Add a comment block at the top of `pre-commit-coverage.sh` explaining the
    escape hatch and when it is appropriate to use it
  - _Requirements: 4.5_
  - _Priority: Medium_

- [ ] 14. Test the hook: coverage unchanged path
  - Install the hook with `scripts/install-hooks.sh`
  - Make a trivial source-comment change and commit
  - Confirm hook runs, reports "Coverage unchanged", and the commit succeeds
  - _Requirements: 3.3_
  - _Priority: High_

- [ ] 15. Test the hook: coverage raised path
  - Add a new test that covers a previously uncovered branch
  - Commit and confirm the hook raises the floor, stages `coverage_floor.txt`,
    and prints "Coverage floor raised: X% → Y%"
  - _Requirements: 3.2_
  - _Priority: High_

- [ ] 16. Test the hook: ratchet rejection path
  - Temporarily lower `coverage_floor.txt` above the current measurement
  - Attempt a commit and confirm it is rejected with a clear shortfall message
  - Restore `coverage_floor.txt` to the real floor
  - _Requirements: 3.4_
  - _Priority: High_

---

## Phase 4: Documentation and Cleanup (Tasks 17–20)

### Make the system discoverable and maintainable

- [ ] 17. Update `README.md` — Code Coverage section
  - Add a "Code Coverage" section after the "Running Tests" section covering:
    - How to configure and run the coverage build
    - How to generate the HTML report
    - How the ratchet works and where the floor is stored
    - How to install the pre-commit hook
    - The `SKIP_COVERAGE_CHECK` escape hatch
  - _Requirements: 6.2_
  - _Priority: High_

- [ ] 18. Update `doc/TODO.md`
  - Mark the code-coverage TODO item complete once all tasks are done
  - _Requirements: (housekeeping)_
  - _Priority: Low_

- [ ] 19. Add CI workflow step (optional, if CI exists)
  - Document the recommended CI steps (configure, build, test, enforce ratchet)
    in a comment block in `CMakeLists.txt` or a `doc/ci-coverage.md` file
  - _Requirements: (design — CI integration section)_
  - _Priority: Low_

- [ ] 20. Validate `cmake --build build-coverage --target help` output
  - Confirm `coverage`, `coverage-html`, and `coverage-reset` appear
  - _Requirements: 6.1_
  - _Priority: Medium_

---

## Summary

| Phase | Tasks | Description |
|-------|-------|-------------|
| 1 | 1–6 | CMake option, flags, tool detection, and three targets |
| 2 | 7–10 | `.gitignore`, initial measurement, and `coverage_floor.txt` |
| 3 | 11–16 | Hook script, install script, and three acceptance tests |
| 4 | 17–20 | README, TODO, CI notes, and target-list validation |

**Total tasks**: 20
**Estimated effort**: 1–2 days
