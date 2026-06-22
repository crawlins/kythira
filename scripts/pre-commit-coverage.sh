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

# ── Require gcovr ─────────────────────────────────────────────────────────────
GCOVR=$(command -v gcovr 2>/dev/null \
    || command -v "${HOME}/.local/bin/gcovr" 2>/dev/null \
    || true)
if [[ -z "$GCOVR" ]]; then
    echo "  [coverage] WARNING: gcovr not found — skipping coverage check."
    echo "             Install with: pip3 install gcovr"
    exit 0
fi

# ── Configure coverage build if needed ────────────────────────────────────────
if [[ ! -f "${COVERAGE_BUILD}/CMakeCache.txt" ]]; then
    echo "  [coverage] Configuring build-coverage/ ..."
    cmake -S "${REPO}" -B "${COVERAGE_BUILD}" \
          -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF \
          > /dev/null 2>&1
fi

# ── Incremental build ─────────────────────────────────────────────────────────
echo "  [coverage] Building ..."
cmake --build "${COVERAGE_BUILD}" -j"$(nproc)" > /dev/null 2>&1

# ── Reset stale counters ──────────────────────────────────────────────────────
find "${COVERAGE_BUILD}" -name "*.gcda" -delete 2>/dev/null || true

# ── Run test subset (exclude slow/performance/verbose labels) ─────────────────
echo "  [coverage] Running tests ..."
if [[ "${COVERAGE_FULL_SUITE:-0}" == "1" ]]; then
    CTEST_LABEL_ARGS=""
else
    CTEST_LABEL_ARGS="-LE slow|performance|verbose|benchmark"
fi

# shellcheck disable=SC2086
if ! ctest --test-dir "${COVERAGE_BUILD}" \
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

# ── Measure coverage ──────────────────────────────────────────────────────────
echo "  [coverage] Measuring ..."
GCOVR_OUT=$(cd "${COVERAGE_BUILD}" && "$GCOVR" \
    --root "${REPO}" \
    --object-directory "${COVERAGE_BUILD}" \
    --exclude ".*build-coverage.*" \
    --exclude ".*vcpkg_installed.*" \
    --exclude ".*/usr/.*" \
    --exclude ".*cmd/.*" \
    --exclude ".*tests/docker_chaos/.*" \
    --gcov-ignore-parse-errors=negative_hits.warn \
    --print-summary 2>&1)

# Extract line coverage percentage (e.g. "lines:  82.5% (1234 out of 1495)")
NEW_PCT=$(echo "$GCOVR_OUT" \
    | awk '/^lines:/ { match($2, /^[0-9]+\.[0-9]+/, m); if (m[0] != "") { print m[0]; exit } }' \
    | head -1)

if [[ -z "$NEW_PCT" ]]; then
    echo "  [coverage] WARNING: could not parse coverage percentage — skipping ratchet."
    echo "$GCOVR_OUT"
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
