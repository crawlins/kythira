#!/usr/bin/env bash
# Pre-commit hook: enforce the coverage ratchet.
#
# Builds the instrumented tree, runs a label-filtered test subset, measures
# line coverage, and rejects the commit if coverage would fall below the value
# stored in coverage_floor.txt.
#
# Escape hatch for WIP commits (skips the check entirely):
#   SKIP_COVERAGE_CHECK=1 git commit -m "..."
#
# To run the full suite (slow) instead of the fast subset:
#   COVERAGE_FULL_SUITE=1 git commit -m "..."
set -euo pipefail

START=$(date +%s)
REPO=$(git rev-parse --show-toplevel)
FLOOR_FILE="${REPO}/coverage_floor.txt"
COVERAGE_BUILD="${REPO}/build-coverage"

# ── Escape hatch ──────────────────────────────────────────────────────────────
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
           --repeat until-pass:2 \
           --output-on-failure \
           --quiet 2>/dev/null; then
    echo ""
    echo "  [coverage] ERROR: Tests failed. Fix failing tests before committing."
    echo "             (To skip the coverage check: SKIP_COVERAGE_CHECK=1 git commit)"
    exit 1
fi

# ── Measure coverage ──────────────────────────────────────────────────────────
echo "  [coverage] Measuring ..."
GCOVR_OUT=$("$GCOVR" \
    --root "${REPO}" \
    --object-directory "${COVERAGE_BUILD}" \
    --exclude ".*build-coverage.*" \
    --exclude ".*vcpkg_installed.*" \
    --exclude ".*/usr/.*" \
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
