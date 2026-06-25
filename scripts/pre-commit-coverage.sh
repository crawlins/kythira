#!/usr/bin/env bash
# Pre-commit hook: format check, static analysis, and coverage ratchet.
#
# Step 1 — clang-format (fast, staged *.cpp/*.hpp):
#   Rejects the commit if any staged file differs from its formatted form.
#   Fix: clang-format -i <file>  or  cmake --build build --target format
#
# Step 2 — clang-tidy (slow, opt-in, staged *.cpp only):
#   Runs static analysis on staged translation units.
#   Enable: TIDY_CHECK=1 git commit -m "..."
#   Fix:    cmake --build build --target static-analysis-fix
#
# Step 3 — coverage ratchet (slow, full instrumented build + test run):
#   Builds the instrumented tree, runs a label-filtered test subset, measures
#   line coverage, and rejects the commit if coverage would fall below the value
#   stored in coverage_floor.txt.
#
# Escape hatches for WIP commits:
#   SKIP_FORMAT_CHECK=1   git commit -m "..."  (skip format check only)
#   SKIP_TIDY_CHECK=1     git commit -m "..."  (hard-skip tidy even if TIDY_CHECK=1)
#   SKIP_COVERAGE_CHECK=1 git commit -m "..."  (skip coverage check only)
#
# To run the full test suite for coverage (slow):
#   COVERAGE_FULL_SUITE=1 git commit -m "..."
set -euo pipefail

REPO=$(git rev-parse --show-toplevel)

# ── Format check (staged files only) ─────────────────────────────────────────
if [[ "${SKIP_FORMAT_CHECK:-0}" == "1" ]]; then
    echo "  [format] Skipped (SKIP_FORMAT_CHECK=1)"
else
    CLANG_FORMAT=$(command -v clang-format 2>/dev/null \
        || command -v "${HOME}/.local/bin/clang-format" 2>/dev/null \
        || true)

    if [[ -z "$CLANG_FORMAT" ]]; then
        echo "  [format] WARNING: clang-format not found — skipping format check."
        echo "           Install with: pip3 install clang-format"
    else
        STAGED=$(git diff --cached --name-only --diff-filter=ACMR \
                 | grep -E '\.(cpp|hpp)$' || true)

        if [[ -n "$STAGED" ]]; then
            BAD_FILES=()
            for f in $STAGED; do
                if ! "$CLANG_FORMAT" --dry-run --Werror "$f" 2>/dev/null; then
                    BAD_FILES+=("$f")
                fi
            done

            N=$(echo "$STAGED" | wc -w)
            if [[ ${#BAD_FILES[@]} -gt 0 ]]; then
                echo ""
                echo "  [format] FAILED — the following file(s) need formatting:"
                printf '    %s\n' "${BAD_FILES[@]}"
                echo ""
                echo "  Fix with:"
                # shellcheck disable=SC2145
                echo "    clang-format -i ${BAD_FILES[*]}"
                echo "  or reformat the whole project:"
                echo "    cmake --build build --target format"
                echo ""
                echo "  (To skip: SKIP_FORMAT_CHECK=1 git commit)"
                exit 1
            fi
            echo "  [format] ${N} file(s) OK"
        fi
    fi
fi

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
        echo "         Run: cmake -S . -B build <prefix-path-flags>"
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
                echo "  [tidy] FAILED — fix findings above, or suppress with:"
                echo "    // NOLINT(check-name)  at the end of the offending line"
                echo "  Run full analysis: cmake --build build --target static-analysis"
                echo "  Apply auto-fixes:  cmake --build build --target static-analysis-fix"
                echo "  (To skip: SKIP_TIDY_CHECK=1 git commit)"
                exit 1
            fi
            echo "  [tidy] ${N} file(s) OK"
        fi
    fi
fi

START=$(date +%s)
FLOOR_FILE="${REPO}/coverage_floor.txt"
COVERAGE_BUILD="${REPO}/build-coverage"

# ── Coverage escape hatch ─────────────────────────────────────────────────────
if [[ "${SKIP_COVERAGE_CHECK:-0}" == "1" ]]; then
    echo "  [coverage] Skipped (SKIP_COVERAGE_CHECK=1)"
    exit 0
fi

# ── Require LLVM coverage tools ───────────────────────────────────────────────
LLVM_PROFDATA=$(command -v llvm-profdata-18 2>/dev/null \
    || command -v llvm-profdata 2>/dev/null \
    || true)
LLVM_COV=$(command -v llvm-cov-18 2>/dev/null \
    || command -v llvm-cov 2>/dev/null \
    || true)
CLANGXX=$(command -v clang++-18 2>/dev/null \
    || command -v clang++ 2>/dev/null \
    || true)
if [[ -z "$LLVM_PROFDATA" || -z "$LLVM_COV" || -z "$CLANGXX" ]]; then
    echo "  [coverage] WARNING: llvm-profdata/llvm-cov/clang++ not found — skipping check."
    echo "             Install with: apt-get install clang llvm"
    exit 0
fi
# Derive C compiler from C++ compiler: clang++-18 -> clang-18
CLANGC="${CLANGXX/++/}"

# ── Configure coverage build if needed ────────────────────────────────────────
# Force reconfigure if the existing cache was built with a different compiler.
_NEEDS_CONFIGURE=0
if [[ ! -f "${COVERAGE_BUILD}/CMakeCache.txt" ]]; then
    _NEEDS_CONFIGURE=1
else
    _CACHED_CXX=$(grep "^CMAKE_CXX_COMPILER:FILEPATH=" \
                  "${COVERAGE_BUILD}/CMakeCache.txt" 2>/dev/null | cut -d= -f2 || true)
    if [[ ! "${_CACHED_CXX}" =~ clang ]]; then
        echo "  [coverage] Reconfiguring build-coverage/ for clang source-based coverage ..."
        rm -rf "${COVERAGE_BUILD}"
        _NEEDS_CONFIGURE=1
    fi
fi

if [[ "$_NEEDS_CONFIGURE" == "1" ]]; then
    echo "  [coverage] Configuring build-coverage/ ..."
    # Inherit the vcpkg prefix path from the main build cache if present.
    _VCPKG_PREFIX=$(grep "^CMAKE_PREFIX_PATH:" \
                    "${REPO}/build/CMakeCache.txt" 2>/dev/null \
                    | cut -d= -f2 || true)
    _PREFIX_ARG=()
    if [[ -n "$_VCPKG_PREFIX" ]]; then
        _PREFIX_ARG=(-DCMAKE_PREFIX_PATH="${_VCPKG_PREFIX}")
    fi
    cmake -S "${REPO}" -B "${COVERAGE_BUILD}" \
          -DENABLE_COVERAGE=ON \
          -DCMAKE_BUILD_TYPE=Debug \
          -DCMAKE_CXX_COMPILER="${CLANGXX}" \
          -DCMAKE_C_COMPILER="${CLANGC}" \
          "${_PREFIX_ARG[@]}" \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF \
          > /dev/null 2>&1
fi

# ── Incremental build ─────────────────────────────────────────────────────────
echo "  [coverage] Building ..."
cmake --build "${COVERAGE_BUILD}" -j"$(nproc)" > /dev/null 2>&1

# ── Reset stale counters ──────────────────────────────────────────────────────
find "${COVERAGE_BUILD}" -name "*.profraw" -delete 2>/dev/null || true

# ── Run test subset (exclude slow/performance/verbose labels) ─────────────────
echo "  [coverage] Running tests ..."
if [[ "${COVERAGE_FULL_SUITE:-0}" == "1" ]]; then
    CTEST_LABEL_ARGS=""
else
    CTEST_LABEL_ARGS="-LE slow|performance|verbose|benchmark"
fi

# Each test process writes its own profraw file keyed by PID + module hash.
# shellcheck disable=SC2086
if ! LLVM_PROFILE_FILE="${COVERAGE_BUILD}/%p-%m.profraw" \
     ctest --test-dir "${COVERAGE_BUILD}" \
           -j"$(nproc)" \
           ${CTEST_LABEL_ARGS} \
           --repeat until-pass:3 \
           --output-on-failure \
           --quiet 2>/dev/null; then
    echo ""
    echo "  [coverage] ERROR: Tests failed. Fix failing tests before committing."
    echo "             (To skip the coverage check: SKIP_COVERAGE_CHECK=1 git commit)"
    exit 1
fi

# ── Merge per-process profiles ────────────────────────────────────────────────
echo "  [coverage] Measuring ..."
COVERAGE_PROFDATA="${COVERAGE_BUILD}/merged.profdata"
mapfile -t _PROFRAW < <(find "${COVERAGE_BUILD}" -name "*.profraw" 2>/dev/null)
if [[ ${#_PROFRAW[@]} -eq 0 ]]; then
    echo "  [coverage] WARNING: no .profraw files generated — skipping ratchet."
    exit 0
fi
"$LLVM_PROFDATA" merge -sparse "${_PROFRAW[@]}" -o "${COVERAGE_PROFDATA}" 2>/dev/null

# ── Collect test binaries for llvm-cov ────────────────────────────────────────
mapfile -t _TEST_BINS < <(find "${COVERAGE_BUILD}/tests" -maxdepth 1 \
                               -executable -type f 2>/dev/null | sort)
if [[ ${#_TEST_BINS[@]} -eq 0 ]]; then
    echo "  [coverage] WARNING: no test binaries found — skipping ratchet."
    exit 0
fi
_MAIN_BIN="${_TEST_BINS[0]}"
_EXTRA_BINS=()
for _b in "${_TEST_BINS[@]:1}"; do
    _EXTRA_BINS+=(-object "$_b")
done

# ── Report line coverage ───────────────────────────────────────────────────────
# llvm-cov report columns: Filename Regions Missed Cover% Lines Missed LineCover% …
# $7 on the TOTAL row is the line coverage percentage.
LLVM_COV_OUT=$("$LLVM_COV" report \
    --instr-profile="${COVERAGE_PROFDATA}" \
    "${_MAIN_BIN}" \
    "${_EXTRA_BINS[@]}" \
    --ignore-filename-regex='.*/build-coverage/.*' \
    --ignore-filename-regex='.*/vcpkg_installed/.*' \
    --ignore-filename-regex='.*/usr/.*' \
    --ignore-filename-regex='.*/cmd/.*' \
    --ignore-filename-regex='.*/tests/docker_chaos/.*' \
    --ignore-filename-regex='.*/examples/.*' \
    2>&1)

NEW_PCT=$(printf '%s\n' "$LLVM_COV_OUT" \
    | awk '/^TOTAL/ { gsub(/%/, "", $7); print $7; exit }')

if [[ -z "$NEW_PCT" ]]; then
    echo "  [coverage] WARNING: could not parse coverage percentage — skipping ratchet."
    printf '%s\n' "$LLVM_COV_OUT"
    exit 0
fi

# ── Read floor (default 0.0) ──────────────────────────────────────────────────
OLD_FLOOR=$(cat "${FLOOR_FILE}" 2>/dev/null || echo "0.0")

# ── Compare ───────────────────────────────────────────────────────────────────
RESULT=$(awk -v new="$NEW_PCT" -v old="$OLD_FLOOR" \
    'BEGIN {
        if (new+0 < old+0) print "below"
        else if (new+0 > old+0) print "above"
        else print "same"
    }')

ELAPSED=$(( $(date +%s) - START ))

case "$RESULT" in
    below)
        SHORTFALL=$(awk -v n="$NEW_PCT" -v o="$OLD_FLOOR" \
            'BEGIN { printf "%.1f", o-n }')
        echo ""
        echo "  ╔══════════════════════════════════════╗"
        echo "  ║      COVERAGE RATCHET FAILED         ║"
        echo "  ╠══════════════════════════════════════╣"
        echo "  ║  Floor  : ${OLD_FLOOR}%"
        echo "  ║  Current: ${NEW_PCT}%"
        echo "  ║  Shortfall: -${SHORTFALL}%"
        echo "  ╠══════════════════════════════════════╣"
        echo "  ║  Add tests to bring coverage up,     ║"
        echo "  ║  or use SKIP_COVERAGE_CHECK=1 for    ║"
        echo "  ║  a WIP commit.                       ║"
        echo "  ╚══════════════════════════════════════╝"
        echo ""
        exit 1
        ;;
    above)
        printf '%s\n' "$NEW_PCT" > "${FLOOR_FILE}"
        git -C "${REPO}" add "${FLOOR_FILE}"
        echo "  [coverage] Floor raised: ${OLD_FLOOR}% → ${NEW_PCT}%  (${ELAPSED}s)"
        ;;
    same)
        echo "  [coverage] Unchanged at ${NEW_PCT}%  (${ELAPSED}s)"
        ;;
esac

exit 0
