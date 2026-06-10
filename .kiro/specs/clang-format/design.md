# Design Document

## Overview

This document describes the design for integrating `clang-format` into the
Kythira build system and pre-commit hook. The integration follows the same
pattern established by the code-coverage spec: a CMake option/target pair for
ad-hoc developer use, and a pre-commit hook step that enforces compliance
automatically at commit time with a fast, staged-files-only check.

## Architecture

```
Developer workflow (every commit)
──────────────────────────────────
git commit
    └── .git/hooks/pre-commit
            ├── [format] git diff --cached → *.cpp / *.hpp files
            │       ├── none staged  → skip silently
            │       └── some staged  → clang-format --dry-run --Werror
            │               ├── all pass → "[format] N file(s) OK"
            │               └── any fail → print paths + fix cmd → exit 1
            └── [coverage] (existing ratchet — unchanged)

Developer ad-hoc
────────────────
cmake --build build --target format        (reformat all project sources)
cmake --build build --target format-check  (check all; non-zero on diff)
```

## Component Design

### 1. `.clang-format` (repository root)

A YAML configuration file picked up automatically by `clang-format` when run
from any project subdirectory. The base style is `Google` with a small set of
overrides chosen to match existing project conventions:

```yaml
---
BasedOnStyle: Google
ColumnLimit: 100
IndentWidth: 4
TabWidth: 4
UseTab: Never
IncludeBlocks: Regroup
SortIncludes: CaseSensitive
AllowShortFunctionsOnASingleLine: Inline
AllowShortLambdasOnASingleLine: All
BraceWrapping:
  AfterFunction: false
CompactNamespaces: false
...
```

Key decisions:

- **`BasedOnStyle: Google`** — the most widely used C++ style base; avoids
  contentious "tabs vs spaces" choices being hardcoded in the repo.
- **`ColumnLimit: 100`** — wider than Google's 80 to accommodate the longer
  template-heavy identifiers common in this codebase.
- **`IndentWidth: 4`** — the existing codebase uses 4-space indentation.
- **`SortIncludes: CaseSensitive`** — preserves the existing grouping of system
  headers vs. project headers without reordering within groups.
- **`AllowShortLambdasOnASingleLine: All`** — necessary for the
  `folly::Future`-heavy code patterns in this project.

The exact overrides SHALL be tuned during task 1 by running `clang-format` over
a representative sample and adjusting until the diff is minimal.

### 2. CMake Targets (`CMakeLists.txt`)

The format targets are defined unconditionally (not gated by a CMake option)
because formatting is always relevant, regardless of build type. They are
disabled with a clear error if `clang-format` is not found.

```cmake
find_program(CLANG_FORMAT_EXECUTABLE clang-format)

if(CLANG_FORMAT_EXECUTABLE)
    # Collect all project C++ sources
    file(GLOB_RECURSE FORMAT_SOURCES
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/*.hpp"
        "${CMAKE_SOURCE_DIR}/include/*.cpp"
        "${CMAKE_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_SOURCE_DIR}/tests/*.cpp"
        "${CMAKE_SOURCE_DIR}/tests/*.hpp"
        "${CMAKE_SOURCE_DIR}/examples/*.cpp"
        "${CMAKE_SOURCE_DIR}/examples/*.hpp"
    )

    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${FORMAT_SOURCES}
        COMMAND ${CMAKE_COMMAND} -E echo
            "format: reformatted ${FORMAT_SOURCES_COUNT} file(s)"
        COMMENT "Reformatting C++ sources"
    )

    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${FORMAT_SOURCES}
        COMMENT "Checking C++ formatting"
    )
else()
    message(WARNING "clang-format not found — 'format' and 'format-check' targets unavailable. "
                    "Install clang-format (e.g. apt install clang-format) to enable them.")
    add_custom_target(format
        COMMAND ${CMAKE_COMMAND} -E echo "ERROR: clang-format not found."
        COMMAND ${CMAKE_COMMAND} -E false
    )
    add_custom_target(format-check
        COMMAND ${CMAKE_COMMAND} -E echo "ERROR: clang-format not found."
        COMMAND ${CMAKE_COMMAND} -E false
    )
endif()
```

Using `file(GLOB_RECURSE ...)` at configure time is acceptable here because the
source tree is stable (headers and sources do not appear or disappear during a
normal developer workflow). The glob is re-evaluated on every `cmake` re-run,
which is sufficient.

### 3. Pre-Commit Hook

The existing `.git/hooks/pre-commit` is extended to run a format check on
staged C++ files before the coverage ratchet. The format check is
self-contained and does not depend on the CMake build directory, so it runs
even before the coverage build is configured.

#### Staged-files-only strategy

Checking only staged files keeps the format step at O(changed files) rather
than O(all project files). In practice this is 1–20 files and completes in
under a second even on a slow machine.

```bash
# Identify staged C++ files
STAGED=$(git diff --cached --name-only --diff-filter=ACMR \
         | grep -E '\.(cpp|hpp)$')
```

`--diff-filter=ACMR` includes Added, Copied, Modified, and Renamed files;
Deleted files are excluded because they no longer exist on disk.

#### Format check flow

```bash
if [[ -z "$STAGED" ]]; then
    # no C++ files staged — skip silently
else
    BAD_FILES=()
    for f in $STAGED; do
        if ! clang-format --dry-run --Werror "$f" 2>/dev/null; then
            BAD_FILES+=("$f")
        fi
    done

    if [[ ${#BAD_FILES[@]} -gt 0 ]]; then
        echo "  [format] FAILED — the following file(s) need formatting:"
        printf '    %s\n' "${BAD_FILES[@]}"
        echo ""
        echo "  Fix with:"
        echo "    clang-format -i ${BAD_FILES[*]}"
        echo "  or reformat the whole project:"
        echo "    cmake --build build --target format"
        echo ""
        exit 1
    fi

    echo "  [format] ${#STAGED_ARRAY[@]} file(s) OK"
fi
```

#### Hook ordering

```
format check (fast, staged-only, ~1s)
    ↓ pass
coverage ratchet (slow, full build + tests, ~6min)
    ↓ pass
commit proceeds
```

Putting format first ensures the developer sees a formatting error immediately
without investing 6 minutes of build time only to fail on style.

#### Escape hatch

```bash
if [[ "${SKIP_FORMAT_CHECK:-0}" == "1" ]]; then
    echo "  [format] Skipped (SKIP_FORMAT_CHECK=1)"
    # fall through to coverage check
fi
```

### 4. `scripts/install-hooks.sh` Update

The existing script installs the coverage hook. It SHALL be updated so that it
either installs a new combined pre-commit script or replaces the hook with one
that sources both checks in order. The simplest approach is to make the single
`.git/hooks/pre-commit` the authoritative location for the full hook chain, and
have `install-hooks.sh` regenerate it from `scripts/pre-commit-coverage.sh`
plus a new `scripts/pre-commit-format.sh`.

Alternatively, the format check can be added directly to the existing
`.git/hooks/pre-commit` as a new section at the top, keeping the hook
self-contained. This is the preferred approach because it avoids maintaining
a separate dispatcher.

## Implementation Notes

- `clang-format` version variability: the `.clang-format` file will include a
  `# clang-format version: X.Y` comment documenting the version used to
  generate it. Older versions may silently ignore unknown keys; this is
  acceptable.
- `--dry-run --Werror` was introduced in clang-format 10 (2020). Any version
  in a recent Ubuntu/Debian release (20.04+) will satisfy this.
- The `format-check` CMake target does not produce a build artifact, so it
  cannot be depended upon by other targets. It is a standalone verification
  step only.
- The `file(GLOB_RECURSE ...)` list is captured at configure time. Adding a new
  source file requires a `cmake` re-run to include it in the format targets;
  this is standard CMake behavior and does not warrant special handling.
