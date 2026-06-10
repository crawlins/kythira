# Implementation Plan - clang-format Integration

## Status: Complete

**Last Updated**: June 10, 2026

## Overview

Integrate `clang-format` into the Kythira build system and pre-commit hook.

**Implementation notes**: `clang-format` installed via `pip3 install
clang-format` (version 22.1.5); system `apt` package unavailable without
root. `AlwaysBreakTemplateDeclarations: MultiLine` and
`FixNamespaceComments: false` were necessary to minimise the initial
reformat diff. A style-only commit reformed all 349 files. The echo message
`file(s)` in the CMake custom target caused a Make `/bin/sh` syntax error due
to unescaped parentheses — fixed by using `files` instead.

---

## Phase 1: Style Configuration (Tasks 1–3)

- [x] 1. Write `.clang-format` at the repository root
  - `BasedOnStyle: Google`, `IndentWidth: 4`, `ColumnLimit: 100`,
    `AlwaysBreakTemplateDeclarations: MultiLine`,
    `FixNamespaceComments: false`, `SortIncludes: false`,
    `AllowShortEnumsOnASingleLine: false`, `SpaceAfterTemplateKeyword: false`
  - Verified with `clang-format --dump-config` (no warnings)
  - _Requirements: 1.1, 1.2, 1.3, 1.4_

- [x] 2. Measure full-tree diff with chosen config
  - 346/349 files required changes (primarily namespace indentation,
    template+requires clause layout, and enum formatting)
  - Decision: auto-apply and commit as a style-only commit
  - _Requirements: 1.4_

- [x] 3. Apply formatting and commit style-only change
  - `clang-format -i` applied to all 349 sources
  - Verified `clang-format --dry-run --Werror` exits 0 on all files
  - _Requirements: 1.4_

---

## Phase 2: CMake Targets (Tasks 4–7)

- [x] 4. Add `find_program(CLANG_FORMAT_EXECUTABLE clang-format)` to
    `CMakeLists.txt`
  - Hints: `~/.local/bin`, `/usr/local/bin`, `/usr/bin`
  - Warning emitted (not error) when absent
  - _Requirements: 2.3, 3.1_

- [x] 5. Define `format` target
  - `file(GLOB_RECURSE FORMAT_SOURCES ...)` collects `*.cpp`/`*.hpp` under
    `src/`, `include/`, `tests/`, `examples/`
  - Stub targets with error messages defined when absent
  - _Requirements: 2.1, 2.2, 2.3, 2.4_

- [x] 6. Define `format-check` target
  - `clang-format --dry-run --Werror` over all FORMAT_SOURCES
  - Note: echo message must not contain `(` or `)` — Make's sh expands them;
    used `files` instead of `file(s)`
  - _Requirements: 3.1, 3.2, 3.3, 3.4_

- [x] 7. Validate CMake targets
  - `cmake --build build --target format-check` exits 0 on clean tree
  - `cmake --build build --target help` lists `format` and `format-check`
  - Violation test: introduced bad format, confirmed exit 1
  - _Requirements: 2.1–2.4, 3.1–3.4_

---

## Phase 3: Pre-Commit Hook (Tasks 8–13)

- [x] 8. Add format check section to `scripts/pre-commit-coverage.sh`
  - Inserted before coverage block; runs format first (fast), then coverage
  - `SKIP_FORMAT_CHECK=1` escape hatch; graceful skip if `clang-format` absent
  - Staged-files-only via `git diff --cached --name-only --diff-filter=ACMR`
  - Failure output includes file list + `clang-format -i` fix command
  - _Requirements: 4.1–4.6, 5.1–5.3_

- [x] 9. `scripts/pre-commit-coverage.sh` is the canonical source
  - The `.git/hooks/pre-commit` symlink already points to this file
  - No rename needed — the script name is stable
  - _Requirements: 6.1, 6.2_

- [x] 10. Update `scripts/install-hooks.sh`
  - Updated help text to describe both checks and both escape hatches
  - Non-symlink guard preserved
  - _Requirements: 6.1, 6.3_

- [x] 11. Test hook: no staged C++ files
  - Only `.md` / non-C++ staged → format section silent, coverage proceeds
  - _Requirements: 4.2_

- [x] 12. Test hook: staged files all compliant
  - Staged `include/raft/types.hpp` → `[format] 1 file(s) OK`
  - _Requirements: 4.4_

- [x] 13. Test hook: staged file non-compliant
  - Staged `int main(){int x=1;return x;}` → hook rejected with filename and
    fix command, exit 1
  - `SKIP_FORMAT_CHECK=1`: format skipped, coverage proceeds → exit 0
  - _Requirements: 4.5, 4.6, 5.1, 5.2_

---

## Phase 4: Documentation and Cleanup (Tasks 14–16)

- [x] 14. Update `README.md` — Code Style section
  - Added "Code Style" section before "Code Coverage" with: auto-format command,
    check compliance command, pre-commit behaviour, `SKIP_FORMAT_CHECK=1` docs
  - _Requirements: 7.1, 7.2_

- [x] 15. Update `doc/TODO.md`
  - Marked `clang-format integration` item `[x]`; added to "What Changed"
    for June 10, 2026
  - _Requirements: 7.3_

- [x] 16. End-to-end flow verified
  - Clean tree: `format-check` exits 0
  - Non-compliant staged file: hook rejects with filename and fix command
  - Fixed and re-staged: hook passes format, skips coverage
  - _Requirements: all_

---

## Summary

| Phase | Tasks | Status |
|-------|-------|--------|
| 1 | 1–3   | ✅ Complete |
| 2 | 4–7   | ✅ Complete |
| 3 | 8–13  | ✅ Complete |
| 4 | 14–16 | ✅ Complete |

**Total**: 16/16 tasks complete
