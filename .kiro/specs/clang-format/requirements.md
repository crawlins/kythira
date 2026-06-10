# Requirements Document

## Introduction

This document specifies the requirements for integrating `clang-format` into
the Kythira build system and pre-commit hook. The goal is to enforce a
consistent, machine-verified code style across all C++ sources (`*.cpp`,
`*.hpp`) without imposing a manual formatting step on contributors.

Formatting compliance is checked automatically on every commit by examining
only the staged files, keeping the pre-commit overhead near zero. Developers
can also reformat the entire tree or check compliance project-wide via CMake
targets. The pre-commit format check runs before the coverage ratchet so
formatting errors are reported immediately without waiting for the slow
instrumented build.

## Glossary

- **Format_Config**: The `.clang-format` file at the repository root that
  defines the canonical style for all project C++ sources.
- **Format_Target**: The CMake `format` target that reformats all project C++
  sources in-place using `clang-format -i`.
- **Format_Check_Target**: The CMake `format-check` target that exits with a
  non-zero status if any project C++ source differs from its formatted
  counterpart, without modifying files.
- **Staged_Files**: The set of `.cpp` and `.hpp` files in the Git index at
  commit time, as reported by `git diff --cached --name-only --diff-filter=ACMR`.
- **Pre_Commit_Hook**: The Git hook script at `.git/hooks/pre-commit` that
  checks formatting compliance on staged files and runs the coverage ratchet.
- **Escape_Hatch**: The `SKIP_FORMAT_CHECK=1` environment variable that
  bypasses the format step for WIP commits, mirroring the existing
  `SKIP_COVERAGE_CHECK=1` mechanism.

## Requirements

### Requirement 1

**User Story:** As a developer, I want a single canonical `.clang-format`
configuration at the repository root so that all contributors and tools
reference the same style rules automatically.

#### Acceptance Criteria

1. WHEN the repository is cloned THEN a `.clang-format` file SHALL exist at
   the repository root.
2. WHEN `clang-format` is run from any subdirectory of the repository THEN it
   SHALL pick up the root `.clang-format` automatically (via the standard
   parent-directory search).
3. WHEN the `.clang-format` file is parsed THEN it SHALL be valid YAML
   accepted by `clang-format --dump-config` without warnings.
4. WHEN the style is defined THEN it SHALL be based on a named built-in base
   style (`BasedOnStyle: Google` or `LLVM`) with a minimum set of overrides
   necessary to match existing project conventions (e.g., column limit,
   include sorting, brace wrapping).

### Requirement 2

**User Story:** As a developer, I want a CMake `format` target that reformats
every C++ source file in one command so that I can bring the whole tree into
compliance without running `find` manually.

#### Acceptance Criteria

1. WHEN the `format` target is invoked THEN it SHALL apply `clang-format -i`
   to every `*.cpp` and `*.hpp` file under `src/`, `include/`, `tests/`, and
   `examples/`.
2. WHEN `format` runs THEN it SHALL process all matching files regardless of
   whether they are staged or modified in the working tree.
3. WHEN `clang-format` is not found on `PATH` THEN CMake SHALL emit a clear
   warning at configure time and define a stub `format` target that prints an
   actionable error message instead of silently succeeding or failing
   cryptically.
4. WHEN the `format` target completes successfully THEN it SHALL print the
   count of files processed.

### Requirement 3

**User Story:** As a developer, I want a CMake `format-check` target that
verifies compliance project-wide without modifying files so that I can run it
in CI or before opening a pull request.

#### Acceptance Criteria

1. WHEN the `format-check` target is invoked THEN it SHALL run
   `clang-format --dry-run --Werror` on every `*.cpp` and `*.hpp` file under
   `src/`, `include/`, `tests/`, and `examples/`.
2. WHEN all files are compliant THEN `format-check` SHALL exit 0.
3. WHEN one or more files differ from their formatted form THEN `format-check`
   SHALL print the offending file paths and exit with a non-zero status.
4. WHEN `format-check` fails THEN it SHALL print a one-line hint directing the
   developer to run `cmake --build <build-dir> --target format` to fix all
   violations at once.

### Requirement 4

**User Story:** As a developer, I want the pre-commit hook to check only the
staged C++ files so that the format step adds minimal overhead to every commit.

#### Acceptance Criteria

1. WHEN a commit is attempted THEN the pre-commit hook SHALL identify staged
   `*.cpp` and `*.hpp` files via
   `git diff --cached --name-only --diff-filter=ACMR`.
2. WHEN no staged C++ files are present THEN the format check SHALL be skipped
   silently and the hook SHALL proceed to the coverage step.
3. WHEN one or more staged files are present THEN the hook SHALL run
   `clang-format --dry-run --Werror` on each of them.
4. WHEN all staged files are compliant THEN the hook SHALL print a one-line
   confirmation (e.g., `[format] N file(s) OK`) and proceed to the coverage
   step.
5. WHEN one or more staged files are non-compliant THEN the hook SHALL print
   the offending paths, print a fix command the developer can copy-paste, and
   abort the commit with exit status 1.
6. WHEN the fix command is printed THEN it SHALL be a `clang-format -i` command
   operating only on the files that failed, not a broad project-wide reformat.

### Requirement 5

**User Story:** As a developer, I want an escape hatch to skip the format check
for WIP commits so that a half-finished refactor does not block a save point.

#### Acceptance Criteria

1. WHEN the environment variable `SKIP_FORMAT_CHECK=1` is set THEN the
   pre-commit hook SHALL skip the format check entirely and print
   `[format] Skipped (SKIP_FORMAT_CHECK=1)`.
2. WHEN `SKIP_FORMAT_CHECK=1` is set THEN the hook SHALL still run the coverage
   check unless `SKIP_COVERAGE_CHECK=1` is also set.
3. WHEN neither escape hatch is set THEN both checks SHALL run in sequence:
   format first, then coverage.

### Requirement 6

**User Story:** As a project maintainer, I want `scripts/install-hooks.sh` to
install a hook that runs both the format check and the coverage ratchet so that
new contributors get both checks automatically.

#### Acceptance Criteria

1. WHEN `scripts/install-hooks.sh` is executed THEN it SHALL install a
   pre-commit hook at `.git/hooks/pre-commit` that runs both the format check
   (first) and the coverage ratchet (second).
2. WHEN the installed hook is invoked THEN the format check SHALL run before
   the coverage build, so that a formatting error is reported without waiting
   for the slow instrumented compilation.
3. WHEN a non-symlink `.git/hooks/pre-commit` already exists THEN
   `install-hooks.sh` SHALL warn the developer and exit non-zero rather than
   overwriting it silently.
4. WHEN `clang-format` is not found on `PATH` THEN the format step in the hook
   SHALL print a warning and skip gracefully rather than blocking the commit
   with an opaque error.

### Requirement 7

**User Story:** As a project maintainer, I want the clang-format integration
documented so that new contributors understand both how to fix violations and
how the pre-commit check works.

#### Acceptance Criteria

1. WHEN the spec is implemented THEN the project `README.md` SHALL include a
   "Code Style" section explaining: how to auto-format (`cmake --build ...
   --target format`), how to check compliance (`format-check`), and how the
   pre-commit hook enforces style on staged files.
2. WHEN the "Code Style" section is written THEN it SHALL document the
   `SKIP_FORMAT_CHECK=1` escape hatch alongside the existing
   `SKIP_COVERAGE_CHECK=1` documentation.
3. WHEN the spec is complete THEN `doc/TODO.md` SHALL mark the
   `clang-format integration` item as done and add it to the "What Changed"
   summary.
