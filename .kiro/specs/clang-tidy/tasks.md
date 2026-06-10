# Implementation Plan - clang-tidy Integration

## Status: Not Started

**Last Updated**: June 10, 2026

## Overview

Integrate `clang-tidy` static analysis into the Kythira build system and
pre-commit hook. Work is divided into four phases: `.clang-tidy`
configuration, CMake targets, pre-commit hook wiring, and documentation.

---

## Phase 1: `.clang-tidy` Configuration (Tasks 1–4)

### Select checks and verify they pass on the current codebase

- [ ] 1. Install `clang-tidy` on the build machine
  - `sudo apt install clang-tidy` or verify it is available at a versioned
    path (e.g. `/usr/bin/clang-tidy-18`)
  - Record the installed version in a `# clang-tidy version: X.Y` comment
    in `.clang-tidy`
  - _Requirements: 1.1_

- [ ] 2. Write initial `.clang-tidy` at the repository root
  - Enable the check groups from the design doc:
    `bugprone-*`, selected `modernize-*`, `performance-*`, selected
    `readability-*`, selected `cppcoreguidelines-*`, `clang-analyzer-*`
  - Disable the noisy / inapplicable checks listed in the design doc
    (magic-numbers, trailing-return-type, etc.) with explanatory comments
  - Set `HeaderFilterRegex` to project-owned headers only
  - Set `WarningsAsErrors: "*"`
  - Verify `clang-tidy --dump-config` produces no errors
  - _Requirements: 1.1–1.6_

- [ ] 3. Run `clang-tidy` over a representative sample of source files
  - Pick 5–10 files spanning `include/raft/`, `src/`, and `tests/`
  - Command: `clang-tidy -p build/ <files>`
  - Collect all findings; for each either:
    a. Fix the code (if the finding is a genuine issue), or
    b. Disable the check in `.clang-tidy` with an explanatory comment, or
    c. Add a `// NOLINT(check-name)` suppression if narrowly scoped
  - Goal: zero findings on the sample set before expanding to the full tree
  - _Requirements: 1.4, 1.5, 6.1–6.4_

- [ ] 4. Run `clang-tidy` over the full tree; reach zero findings
  - Command: `clang-tidy -p build/ $(find src tests examples -name '*.cpp')`
    (or use the CMake target once it exists in task 6)
  - Iterate: disable noisy checks globally, fix genuine issues, add narrow
    suppressions as a last resort
  - Commit any code fixes as a separate `fix:` commit; commit any
    suppressions in the `.clang-tidy` commit
  - Document any suppressed checks and the reason in the `.clang-tidy` file
  - _Requirements: 1.4, 1.5, 2.7_

---

## Phase 2: CMake Targets (Tasks 5–8)

### Add `static-analysis` and `static-analysis-fix` to CMakeLists.txt

- [ ] 5. Add `find_program` calls for `clang-tidy` and `run-clang-tidy`
  - Search standard paths and versioned paths (`clang-tidy-18`, etc.)
  - Emit `message(WARNING ...)` if `clang-tidy` is absent; do not fail
    configure
  - `run-clang-tidy` is optional; its absence triggers the sequential fallback
  - Place after the `CLANG_FORMAT_EXECUTABLE` block for consistency
  - _Requirements: 2.5_

- [ ] 6. Collect TIDY_SOURCES with `file(GLOB_RECURSE ...)`
  - `*.cpp` under `src/`, `tests/`, `examples/` only (no `*.hpp`)
  - Add `list(LENGTH ...)` for the count variable used in messages
  - _Requirements: 2.1_

- [ ] 7. Define `static-analysis` target
  - When `run-clang-tidy` found: `COMMAND ${RUN_CLANG_TIDY} -p ${CMAKE_BINARY_DIR} -j $(nproc) ${TIDY_SOURCES}`
  - When only `clang-tidy` found: sequential loop over TIDY_SOURCES
    (CMake `foreach` generating multiple COMMAND lines, or a shell `for` loop
    via `COMMAND bash -c "..."`)
  - Guard: check for `${CMAKE_BINARY_DIR}/compile_commands.json` existence;
    if absent, print actionable error and fail
  - Stub targets (print error + `cmake -E false`) when `clang-tidy` absent
  - _Requirements: 2.1–2.7_

- [ ] 8. Define `static-analysis-fix` target
  - Same as `static-analysis` but with `--fix --fix-errors` appended
  - Runs sequentially (parallel `--fix` is unsafe due to race conditions on
    shared headers)
  - Same tool-availability and compile_commands guards as task 7
  - _Requirements: 3.1–3.3_

- [ ] 9. Validate CMake targets
  - `cmake --build build --target static-analysis` — should exit 0 after
    task 4 brings tree to zero findings
  - `cmake --build build --target help` — should list `static-analysis`
    and `static-analysis-fix`
  - Introduce a deliberate tidy violation; confirm `static-analysis` exits
    non-zero and prints the finding; revert
  - _Requirements: 2.1–2.7, 3.1–3.3_

---

## Phase 3: Pre-Commit Hook (Tasks 10–14)

### Wire tidy step between format check and coverage ratchet

- [ ] 10. Add tidy section to `scripts/pre-commit-coverage.sh`
  - Insert between the `[format]` section and the `START=` variable
    that begins the coverage block
  - Section structure (from design doc):
    1. `SKIP_TIDY_CHECK=1` hard-skip (prints skip message, falls through)
    2. `TIDY_CHECK` not set → print notice, fall through (opt-in default)
    3. Locate `clang-tidy` via `command -v`; warn + skip if absent
    4. Check for `${REPO}/build/compile_commands.json`; warn + skip if absent
    5. Collect staged `.cpp` files (exclude `.hpp`)
    6. If none: skip silently
    7. Loop: run `clang-tidy -p "${REPO}/build"` on each file
    8. If any fail: print fix hint + `SKIP_TIDY_CHECK=1` reminder; `exit 1`
    9. If all pass: print `[tidy] N file(s) OK`
  - _Requirements: 4.1–4.7, 5.1–5.3_

- [ ] 11. Update hook header comment block
  - Add Step 2 description: `clang-tidy (slow, opt-in with TIDY_CHECK=1)`
  - Document `TIDY_CHECK=1` and `SKIP_TIDY_CHECK=1` escape hatches
  - Renumber existing steps (format becomes Step 1, tidy Step 2, coverage
    Step 3)
  - _Requirements: 4.1, 5.1_

- [ ] 12. Update `scripts/install-hooks.sh`
  - Add `TIDY_CHECK=1` to the escape-hatch summary section
  - _Requirements: (documentation)_

- [ ] 13. Test hook: `TIDY_CHECK` not set (default)
  - Run `SKIP_COVERAGE_CHECK=1 bash scripts/pre-commit-coverage.sh`
  - Confirm: format check runs, tidy prints notice, coverage skipped
  - _Requirements: 4.1_

- [ ] 14. Test hook: `TIDY_CHECK=1` with staged `.cpp`
  - Stage a `.cpp` file that the full tree already passes tidy for
  - Run `TIDY_CHECK=1 SKIP_COVERAGE_CHECK=1 bash scripts/pre-commit-coverage.sh`
  - Confirm: `[tidy] 1 file(s) OK`
  - Introduce a tidy violation in the file; confirm hook exits 1 with the
    finding and fix hint
  - Restore the file; confirm hook passes
  - _Requirements: 4.2–4.7_

- [ ] 15. Test hook: `SKIP_TIDY_CHECK=1`
  - Run `TIDY_CHECK=1 SKIP_TIDY_CHECK=1 SKIP_COVERAGE_CHECK=1 bash scripts/pre-commit-coverage.sh`
  - Confirm `[tidy] Skipped (SKIP_TIDY_CHECK=1)` is printed
  - _Requirements: 4.7_

---

## Phase 4: Documentation and Cleanup (Tasks 16–18)

- [ ] 16. Update `README.md` — Static Analysis section
  - Add a "Static Analysis" section after the "Code Style" section
  - Cover: `cmake --build build --target static-analysis`,
    `static-analysis-fix`, `TIDY_CHECK=1 git commit`,
    `SKIP_TIDY_CHECK=1 git commit`, `// NOLINT(check-name)` suppressions
  - _Requirements: 7.1, 7.2_

- [ ] 17. Update `doc/TODO.md`
  - Mark `clang-tidy integration` item `[x]`
  - Add entry to "What Changed" with the date
  - _Requirements: 7.3_

- [ ] 18. End-to-end verification
  - `cmake --build build --target static-analysis` → exit 0 on clean tree
  - `TIDY_CHECK=1 git commit` on a compliant staged file → all three checks
    pass; commit proceeds
  - `TIDY_CHECK=1 git commit` on a file with a tidy violation → commit
    blocked with finding and fix instruction
  - _Requirements: all_

---

## Summary

| Phase | Tasks  | Status |
|-------|--------|--------|
| 1 | 1–4   | Not started |
| 2 | 5–9   | Not started |
| 3 | 10–15 | Not started |
| 4 | 16–18 | Not started |

**Total**: 18 tasks
