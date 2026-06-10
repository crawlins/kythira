# Design Document

## Overview

This document describes the design for integrating `clang-tidy` static
analysis into the Kythira build system and pre-commit hook. The integration
follows the same layered approach as `clang-format` — a root-level config
file, CMake targets for ad-hoc use, and a pre-commit step — but differs
in two structural ways driven by performance:

1. **Pre-commit is opt-in.** `clang-tidy` on a template-heavy C++23 file
   takes 10–60 seconds; even a small staged set can take minutes. The hook
   runs the tidy step only when `TIDY_CHECK=1` is set, so routine WIP commits
   are not penalised.

2. **CMake targets use parallelism.** `run-clang-tidy` (when available)
   distributes work across all cores; the fallback uses `xargs -P$(nproc)`.

## Architecture

```
Developer workflow (opt-in, per-commit)
────────────────────────────────────────
TIDY_CHECK=1 git commit
    └── .git/hooks/pre-commit
            ├── [format]   staged *.cpp/*.hpp  → clang-format --dry-run    (~1s)
            ├── [tidy]     staged *.cpp only   → clang-tidy -p build/       (~1–5min)
            │                   ├── findings → print + exit 1
            │                   └── clean    → "[tidy] N file(s) OK"
            └── [coverage] full build + tests                               (~6min)

Developer ad-hoc
────────────────
cmake --build build --target static-analysis       (full tree, parallel)
cmake --build build --target static-analysis-fix   (full tree, --fix)
```

## Component Design

### 1. `.clang-tidy` (repository root)

```yaml
---
Checks: >
  bugprone-*,
  modernize-avoid-bind,
  modernize-deprecated-headers,
  modernize-loop-convert,
  modernize-make-shared,
  modernize-make-unique,
  modernize-pass-by-value,
  modernize-raw-string-literal,
  modernize-redundant-void-arg,
  modernize-replace-auto-ptr,
  modernize-replace-disallow-copy-and-assign-macro,
  modernize-return-braced-init-list,
  modernize-shrink-to-fit,
  modernize-unary-static-assert,
  modernize-use-bool-literals,
  modernize-use-emplace,
  modernize-use-equals-default,
  modernize-use-equals-delete,
  modernize-use-nodiscard,
  modernize-use-noexcept,
  modernize-use-nullptr,
  modernize-use-override,
  modernize-use-using,
  -modernize-use-trailing-return-type,
  -modernize-use-default-member-init,
  performance-*,
  readability-braces-around-statements,
  readability-const-return-type,
  readability-container-contains,
  readability-container-size-empty,
  readability-delete-null-pointer,
  readability-else-after-return,
  readability-implicit-bool-conversion,
  readability-inconsistent-declaration-parameter-name,
  readability-make-member-function-const,
  readability-misplaced-array-index,
  readability-non-const-parameter,
  readability-qualified-auto,
  readability-redundant-access-specifiers,
  readability-redundant-control-flow,
  readability-redundant-declaration,
  readability-redundant-function-ptr-dereference,
  readability-redundant-member-init,
  readability-redundant-preprocessor,
  readability-redundant-smartptr-get,
  readability-redundant-string-cstr,
  readability-redundant-string-init,
  readability-simplify-boolean-expr,
  readability-simplify-subscript-expr,
  readability-static-accessed-through-instance,
  readability-static-definition-in-anonymous-namespace,
  readability-string-compare,
  readability-uniqueptr-delete-release,
  readability-use-anyofallof,
  -readability-identifier-length,
  -readability-magic-numbers,
  -readability-named-parameter,
  cppcoreguidelines-avoid-goto,
  cppcoreguidelines-init-variables,
  cppcoreguidelines-interfaces-global-init,
  cppcoreguidelines-no-malloc,
  cppcoreguidelines-prefer-member-initializer,
  cppcoreguidelines-pro-type-const-cast,
  cppcoreguidelines-pro-type-cstyle-cast,
  cppcoreguidelines-pro-type-member-init,
  cppcoreguidelines-pro-type-reinterpret-cast,
  cppcoreguidelines-pro-type-static-downcast,
  cppcoreguidelines-pro-type-union-access,
  -cppcoreguidelines-avoid-magic-numbers,
  -cppcoreguidelines-pro-bounds-array-to-pointer-decay,
  -cppcoreguidelines-pro-bounds-constant-array-index,
  -cppcoreguidelines-pro-bounds-pointer-arithmetic,
  clang-analyzer-*

# Do not show diagnostics from third-party or system headers.
# Only report issues in project-owned code.
HeaderFilterRegex: "^.*(include/raft|include/network_simulator|include/concepts|tests|src)/.*\\.hpp$"

# Treat all enabled checks as errors so the exit code is non-zero on findings.
WarningsAsErrors: "*"

CheckOptions:
  # modernize-use-nodiscard: only annotate non-trivial return types
  - key: modernize-use-nodiscard.AllowedReturnTypes
    value: "void,bool"

  # performance-move-const-arg: don't warn on trivially-copyable types
  - key: performance-move-const-arg.CheckTriviallyCopyableMove
    value: "false"
...
```

#### Check selection rationale

| Included group / check | Why |
|---|---|
| `bugprone-*` | Catches real latent bugs (dangling references, incorrect round, signed integer overflow, etc.) with low false-positive rate |
| `modernize-use-override` | Enforces `override` on all virtual overrides — critical in a polymorphic concept-based design |
| `modernize-use-nullptr` | Replaces any remaining `NULL` / `0` pointer comparisons |
| `modernize-use-using` | Replaces `typedef` with `using` throughout |
| `performance-*` | Catches unnecessary copies, redundant `std::move`, slow containers |
| `readability-*` (selected) | Structural clarity without being prescriptive about naming |
| `cppcoreguidelines-pro-type-*` | Bans C-style casts, `const_cast`, `reinterpret_cast` except where justified |
| `clang-analyzer-*` | Deep data-flow analysis: null dereference, use-after-free, double-free |

| Excluded check | Why excluded |
|---|---|
| `modernize-use-trailing-return-type` | The codebase already uses trailing returns where appropriate; forcing them everywhere is noise |
| `modernize-use-default-member-init` | Too aggressive with non-trivial constructors; high false-positive rate in concept-heavy code |
| `readability-magic-numbers` / `cppcoreguidelines-avoid-magic-numbers` | Extremely noisy in protocol implementations and test assertions |
| `readability-identifier-length` | Single-letter template parameters are idiomatic C++23 |
| `readability-named-parameter` | Many concept definitions use unnamed parameters intentionally |
| `cppcoreguidelines-pro-bounds-*` | Raw pointer arithmetic is used internally in some Folly paths |

### 2. CMake Targets (`CMakeLists.txt`)

#### Source list

The tidy targets operate on `.cpp` files only. Headers are checked
transitively when their translation units are compiled. This avoids the
false-positive storm that arises when clang-tidy parses template headers
without a concrete instantiation context.

```cmake
file(GLOB_RECURSE TIDY_SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_SOURCE_DIR}/examples/*.cpp"
)
```

#### `static-analysis` target

```
cmake --build build --target static-analysis
```

Checks: `clang-tidy` reads `.clang-tidy` from the repo root automatically.
The `-p` flag points to `build/compile_commands.json`.

When `run-clang-tidy` is available it is preferred because it parallelises
across all cores and handles the `compile_commands.json` filter natively:

```bash
run-clang-tidy -p build/ -j $(nproc) \
    $(find src/ tests/ examples/ -name '*.cpp')
```

Fallback (sequential):
```bash
clang-tidy -p build/ $(find src/ tests/ examples/ -name '*.cpp')
```

Both routes fail the target if any finding is reported (`WarningsAsErrors: "*"`
in `.clang-tidy` ensures non-zero exit).

#### `static-analysis-fix` target

Same as `static-analysis` but passes `--fix` and `--fix-errors` to apply
machine-fixable suggestions in-place. Runs sequentially (fixes are not safe
to apply in parallel to the same file).

### 3. Pre-Commit Hook

#### Opt-in design rationale

The format check (step 1) adds ~1s and is always active. The coverage ratchet
(step 3) adds ~6min but has been the pre-commit contract since the coverage
spec. The tidy check falls between these in cost but with much higher
variance — a single file can take 10–60s depending on template depth. Forcing
it on every commit would add an unbounded penalty to routine WIP saves.

The chosen pattern is:

```
TIDY_CHECK=1 git commit -m "..."     → runs tidy on staged .cpp files
git commit -m "..."                  → prints notice, skips tidy
SKIP_TIDY_CHECK=1 git commit -m "..." → suppresses even the notice
```

The "skipped" notice reminds developers that the check exists and how to
invoke it, without hard-blocking on every commit.

#### Staged-files-only, `.cpp` only

```bash
STAGED_CPP=$(git diff --cached --name-only --diff-filter=ACMR \
             | grep -E '\.cpp$' || true)
```

`.hpp` files are deliberately excluded. Running clang-tidy on a header in
isolation — without a specific translation unit — causes it to analyse the
header with no includes resolved, producing many spurious findings. The
correct analysis happens when the header's `.cpp` translation unit is checked.

#### Compilation database requirement

Unlike the format check (which needs no build artefacts), clang-tidy requires
`compile_commands.json`. The hook checks for `build/compile_commands.json` and
prints an actionable message if absent. It does NOT auto-configure the build
directory (unlike the coverage check) because a bare `cmake -S . -B build`
without the vcpkg prefix path would produce an incomplete database.

#### Hook section (inserted between format and coverage)

```bash
# ── Static analysis / clang-tidy (staged .cpp files, opt-in) ─────────────────
if [[ "${SKIP_TIDY_CHECK:-0}" == "1" ]]; then
    echo "  [tidy] Skipped (SKIP_TIDY_CHECK=1)"
elif [[ "${TIDY_CHECK:-0}" != "1" ]]; then
    echo "  [tidy] Skipped (set TIDY_CHECK=1 to enable)"
else
    CLANG_TIDY=$(command -v clang-tidy 2>/dev/null \
        || command -v "${HOME}/.local/bin/clang-tidy" 2>/dev/null \
        || true)

    if [[ -z "$CLANG_TIDY" ]]; then
        echo "  [tidy] WARNING: clang-tidy not found — skipping."
        echo "         Install with: apt install clang-tidy"
    elif [[ ! -f "${REPO}/build/compile_commands.json" ]]; then
        echo "  [tidy] WARNING: build/compile_commands.json not found — skipping."
        echo "         Run: cmake -S . -B build <prefix-path>"
    else
        STAGED_CPP=$(git diff --cached --name-only --diff-filter=ACMR \
                     | grep -E '\.cpp$' || true)

        if [[ -n "$STAGED_CPP" ]]; then
            TIDY_FAILED=0
            for f in $STAGED_CPP; do
                if ! "$CLANG_TIDY" -p "${REPO}/build" "$f" 2>/dev/null; then
                    TIDY_FAILED=1
                fi
            done

            N=$(echo "$STAGED_CPP" | wc -w)
            if [[ "$TIDY_FAILED" == "1" ]]; then
                echo ""
                echo "  [tidy] FAILED — fix findings above or suppress with:"
                echo "    // NOLINT(check-name)  at the end of the offending line"
                echo "  Run full analysis:  cmake --build build --target static-analysis"
                echo "  (To skip: SKIP_TIDY_CHECK=1 git commit)"
                exit 1
            fi
            echo "  [tidy] ${N} file(s) OK"
        fi
    fi
fi
```

Note: unlike the format check — which runs `clang-format` in a loop and
collects all failures before reporting — the tidy check exits as soon as any
file reports findings (since clang-tidy streams output per-finding and there
is no clean way to accumulate without silencing live output). This is
intentional: the developer sees the findings from the first bad file and can
address them before re-running.

## Implementation Notes

- clang-tidy versioning: the `.clang-tidy` file will include a
  `# clang-tidy version: X.Y` comment. Some checks are version-specific;
  the spec targets the `clang-tidy-18` package available in Ubuntu 24.04.
- `WarningsAsErrors: "*"` in `.clang-tidy` ensures `clang-tidy` exits
  non-zero when any check fires. Without this, findings are printed but the
  exit code is 0.
- `run-clang-tidy` is included in the `clang-tidy` Ubuntu package as
  `/usr/lib/llvm-XX/bin/run-clang-tidy`. The CMake target will also search
  versioned paths.
- The `file(GLOB_RECURSE TIDY_SOURCES ...)` list is captured at configure
  time; adding a new `.cpp` requires a `cmake` re-run to include it in the
  targets.
- Auto-fix (`static-analysis-fix`) should always be run on a clean working
  tree (no uncommitted changes) so the diff can be reviewed before staging.
