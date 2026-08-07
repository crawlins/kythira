#!/usr/bin/env bash
# Assert that a ctest run actually did the work its exit code implies.
#
# A green ctest exit code is not evidence that any particular test ran, and a
# red one is not evidence that any test failed. This project has now been
# bitten by both directions repeatedly:
#
#   * `ctest -R <pattern>` that matches nothing exits 0. A renamed target or a
#     typo'd filter silently converts a job into a no-op that reports success.
#   * A target that matches the filter but was never built is reported
#     "Not Run" and, on its own, does not always surface as a failure.
#   * `--repeat until-pass:N` absorbs first-attempt failures completely. On one
#     observed run the true first-attempt failure count was 3 and the visible
#     count was 1 — the other two passed on retry and left no trace in the job
#     status. They were only found by grepping the raw log.
#   * A test that exits with SKIP_RETURN_CODE (77) reports "Skipped" and counts
#     as success, so a fixture that stops being satisfiable silently stops
#     testing anything.
#
# This script re-reads the raw ctest log and asserts what the exit code cannot:
# that the expected set of tests existed, ran, and were not quietly skipped —
# and it reports retry-absorbed failures that the job status hides.
#
# Usage:
#   scripts/check-test-run.sh --build-dir DIR --log FILE [options] [-- CTEST_FILTER_ARGS...]
#
#   --build-dir DIR   ctest --test-dir used for the run (required)
#   --log FILE        file the ctest run's stdout+stderr was captured to (required)
#   --expect N        expected test count. Omit to derive it from `ctest -N`
#                     with the same filter args, which is self-maintaining and
#                     preferred; pass it explicitly only where the count is
#                     itself the thing under test.
#   --allow-skip RE   extended regex of test names allowed to report "Skipped".
#                     Repeatable. Any skip outside the allowlist fails.
#   --strict-retries  also fail when --repeat absorbed a first-attempt failure.
#                     Off by default: those runs have a legitimate final
#                     verdict, and the point here is that they stop being
#                     invisible, not that they stop being tolerated.
#
# Everything after `--` is passed verbatim to `ctest -N` to derive the expected
# count, and must be the same filter arguments the real run used.
#
# Exit code 0 if the run did the work, non-zero otherwise.

set -euo pipefail

BUILD_DIR=""
LOG_FILE=""
EXPECTED=""
STRICT_RETRIES=0
ALLOW_SKIP=()
CTEST_FILTER_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)      BUILD_DIR="$2"; shift 2 ;;
        --log)            LOG_FILE="$2"; shift 2 ;;
        --expect)         EXPECTED="$2"; shift 2 ;;
        --allow-skip)     ALLOW_SKIP+=("$2"); shift 2 ;;
        --strict-retries) STRICT_RETRIES=1; shift ;;
        --)               shift; CTEST_FILTER_ARGS=("$@"); break ;;
        *) echo "[check-test-run] unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$BUILD_DIR" || -z "$LOG_FILE" ]]; then
    echo "[check-test-run] --build-dir and --log are both required" >&2
    exit 2
fi
if [[ ! -f "$LOG_FILE" ]]; then
    echo "[check-test-run] FAILED — log file not found: $LOG_FILE" >&2
    echo "                 The ctest step did not produce output at all." >&2
    exit 1
fi

# GitHub Actions renders ::error:: / ::warning:: as annotations; outside CI they
# are just prefixes, so this stays readable when run locally.
note()  { echo "[check-test-run] $*"; }
warn()  { echo "::warning::$*"; echo "[check-test-run] WARNING: $*"; }
fail()  { echo "::error::$*"; echo "[check-test-run] FAILED: $*" >&2; }

# ── Derive the expected count ────────────────────────────────────────────────
# `ctest -N` lists without running, so the expectation tracks the CMake test
# registry automatically. A hardcoded number is the thing that silently stops
# covering tests added later, which is the same class of bug this script exists
# to catch.
if [[ -z "$EXPECTED" ]]; then
    LISTING="$(ctest --test-dir "$BUILD_DIR" -N "${CTEST_FILTER_ARGS[@]+"${CTEST_FILTER_ARGS[@]}"}" 2>/dev/null || true)"
    EXPECTED="$(printf '%s\n' "$LISTING" | awk '/^Total Tests: / { print $3; exit }')"
    if [[ -z "$EXPECTED" ]]; then
        fail "could not derive an expected test count from 'ctest -N' in $BUILD_DIR"
        exit 1
    fi
    note "expected count derived from 'ctest -N': $EXPECTED"
else
    note "expected count given explicitly: $EXPECTED"
fi

# ── Parse the run log ────────────────────────────────────────────────────────
# ctest result lines look like:
#     12/404 Test  #12: foo_test ...............   Passed    0.15 sec
#     13/404 Test  #13: bar_test ...............***Failed    1.23 sec
# Under --repeat, a retried test emits one line per attempt, so the set of
# distinct test numbers is what "how many tests ran" means -- counting lines
# would over-count exactly the runs this script is meant to scrutinise.
RESULT_LINES="$(grep -E '^ *[0-9]+/[0-9]+ +Test +#[0-9]+: ' "$LOG_FILE" || true)"

if [[ -z "$RESULT_LINES" ]]; then
    fail "no test result lines in $LOG_FILE — the run executed 0 tests"
    echo "                 Expected $EXPECTED. A ctest filter that matches" >&2
    echo "                 nothing still exits 0, so this is the failure mode" >&2
    echo "                 that otherwise looks exactly like success." >&2
    exit 1
fi

RAN="$(printf '%s\n' "$RESULT_LINES" | sed -E 's/^ *[0-9]+\/[0-9]+ +Test +#([0-9]+):.*/\1/' | sort -u | wc -l)"

# ── Check 1: everything that should have run, ran ────────────────────────────
if [[ "$RAN" -ne "$EXPECTED" ]]; then
    fail "expected $EXPECTED tests to run, but $RAN did"
    echo "                 Likely causes: a target in the filter was never" >&2
    echo "                 built (ctest reports it 'Not Run'), the filter no" >&2
    echo "                 longer matches a renamed target, or ctest died" >&2
    echo "                 partway through." >&2
    printf '%s\n' "$RESULT_LINES" | grep -E '\*\*\*Not Run' || true
    exit 1
fi
note "ran $RAN/$EXPECTED tests"

# ── Check 2: skips are allowlisted ───────────────────────────────────────────
SKIPPED="$(printf '%s\n' "$RESULT_LINES" \
    | grep -E '\*\*\*Skipped' \
    | sed -E 's/^ *[0-9]+\/[0-9]+ +Test +#[0-9]+: +([^ .]+).*/\1/' \
    | sort -u || true)"

if [[ -n "$SKIPPED" ]]; then
    UNEXPECTED_SKIPS=()
    while IFS= read -r t; do
        [[ -z "$t" ]] && continue
        allowed=0
        for re in ${ALLOW_SKIP[@]+"${ALLOW_SKIP[@]}"}; do
            if [[ "$t" =~ $re ]]; then allowed=1; break; fi
        done
        if [[ "$allowed" -eq 1 ]]; then
            note "skipped (allowlisted): $t"
        else
            UNEXPECTED_SKIPS+=("$t")
        fi
    done <<< "$SKIPPED"

    if [[ ${#UNEXPECTED_SKIPS[@]} -gt 0 ]]; then
        fail "${#UNEXPECTED_SKIPS[@]} test(s) reported Skipped without being allowlisted"
        printf '                 %s\n' "${UNEXPECTED_SKIPS[@]}" >&2
        echo "                 A skipped test counts as success but tests" >&2
        echo "                 nothing. Either fix the unsatisfied fixture, or" >&2
        echo "                 add --allow-skip '<regex>' to record that the" >&2
        echo "                 skip is intended in this job." >&2
        exit 1
    fi
fi

# ── Check 3: report first-attempt failures --repeat absorbed ─────────────────
# For each test number, the first result line is its first attempt. If that
# attempt failed but the run as a whole passed, --repeat until-pass hid it.
FIRST_ATTEMPT_FAILURES="$(printf '%s\n' "$RESULT_LINES" | awk '
    match($0, /Test +#[0-9]+:/) {
        num = substr($0, RSTART, RLENGTH)
        if (num in seen) next
        seen[num] = 1
        if ($0 ~ /\*\*\*(Failed|Timeout|Exception|Not Run)/) print
    }')"

if [[ -n "$FIRST_ATTEMPT_FAILURES" ]]; then
    N_FIRST="$(printf '%s\n' "$FIRST_ATTEMPT_FAILURES" | wc -l)"
    warn "$N_FIRST test(s) failed on their first attempt"
    printf '%s\n' "$FIRST_ATTEMPT_FAILURES"
    echo ""
    echo "[check-test-run] These are invisible in the job status when"
    echo "                 --repeat until-pass lets them pass on a later"
    echo "                 attempt. Each one is either a flake worth fixing or"
    echo "                 a real failure worth seeing."

    # Surface in the job summary too, so it survives log rotation and is
    # visible without opening the raw log -- which is how these went unnoticed.
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
        {
            echo "### First-attempt test failures (${N_FIRST})"
            echo ""
            echo 'Absorbed by `--repeat until-pass`; not visible in the job status.'
            echo ""
            echo '```'
            printf '%s\n' "$FIRST_ATTEMPT_FAILURES"
            echo '```'
        } >> "$GITHUB_STEP_SUMMARY"
    fi

    if [[ "$STRICT_RETRIES" -eq 1 ]]; then
        fail "--strict-retries is set and $N_FIRST test(s) failed on first attempt"
        exit 1
    fi
else
    note "no first-attempt failures"
fi

note "OK — $RAN test(s) ran, none unexpectedly skipped"
