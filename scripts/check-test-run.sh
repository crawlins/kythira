#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

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
#   * The same one level down: a Boost.Test case whose precondition() is false
#     is "skipped" inside a binary that still exits 0, so ctest says Passed
#     with none of that case's assertions run. Only the binary's own report
#     shows it (--case-reports below).
#
# This script re-reads the raw ctest log and asserts what the exit code cannot:
# that the expected set of tests existed, ran, and were not quietly skipped —
# and it reports retry-absorbed failures that the job status hides.
#
# ── Why a floor as well as a derived count ───────────────────────────────────
#
# Deriving the expectation from `ctest -N` is self-maintaining, and it catches a
# filter that matches nothing and a target that failed to build. It cannot catch
# the case that motivated this script's own TODO entry: a *configure* change
# that silently drops targets — an `if(TARGET Folly::folly)` that stops
# matching, a `find_package` that quietly fails. `ctest -N` reads the same
# broken registry the run did, so the expectation shrinks in lockstep with the
# damage and every remaining test passes. 100% of a diminished suite reports
# exactly like 100% of the whole one.
#
# That is this repo's signature failure — machinery reporting success while
# doing nothing — wearing the costume of the check meant to prevent it. The
# floor is the fixed point: a number that lives in the workflow rather than in
# the build, so a configure regression has to argue with a value it cannot
# influence.
#
# The floor is a lower bound, not an equality. Tests are added far more often
# than removed, so an exact count would be a merge conflict on every PR; a
# deliberate removal that crosses the floor is meant to be a visible, justified
# edit, exactly like coverage_floor.txt.
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
#   --floor N         minimum number of tests the filter must *match*. Unlike
#                     --expect, this is an absolute lower bound checked into
#                     the repo rather than derived from the build, and it is
#                     the only check here that a broken configure cannot move.
#                     See "Why a floor as well as a derived count" below.
#   --allow-skip RE   extended regex of test names allowed to report "Skipped".
#                     Repeatable. Any skip outside the allowlist fails.
#   --strict-retries  also fail when --repeat absorbed a first-attempt failure
#                     of a test that is not on the retry allowlist. Off by
#                     default; ci.yml passes it when the repository variable
#                     CTEST_STRICT_RETRIES is "true" (see "Retry enforcement"
#                     below).
#   --allow-retry RE  extended regex of test names allowed to pass only on a
#                     retry under --strict-retries. Repeatable. The match is
#                     anchored: RE must match the whole test name.
#   --case-reports DIR
#                     directory of per-test Boost.Test reports, written when
#                     the build was configured with -DKYTHIRA_TEST_REPORT_DIR
#                     (cmake/TestReports.cmake). Fails if no test that ran left
#                     a report (the setting did not take effect), and on any
#                     case or suite a report lists as skipped unless it is
#                     allowlisted with --allow-case-skip.
#   --allow-case-skip RE
#                     extended regex for a skipped case or suite allowed under
#                     --case-reports, matched against "<ctest name>:<unit
#                     path>" (e.g. "foo_test:suite/case"). Repeatable, and
#                     anchored like --allow-retry.
#   --allow-retry-file FILE
#                     read --allow-retry patterns from FILE, one per line.
#                     Blank lines and lines starting with '#' are ignored, as
#                     is anything after whitespace following the pattern, so
#                     each entry can carry its own reason on the same line.
#
# ── Retry enforcement ────────────────────────────────────────────────────────
#
# Check 3 started as report-only, on the theory that a visible warning was
# enough. It was not: the warning and the job-summary table sat on green jobs
# where nobody opens them. The allowlist (.github/ctest-retry-allowlist.txt)
# splits the report into known flakes, each tied to the TODO.md entry tracking
# it, and new ones, so a new flake stands out instead of hiding among the
# familiar ones. --strict-retries turns a new one into a failure; adding a test
# to the allowlist is then a reviewed edit with a reason, like lowering
# --floor. Retries still run either way, so a known flake costs no CI cycle.
#
# Everything after `--` is passed verbatim to `ctest -N` to derive the expected
# count, and must be the same filter arguments the real run used.
#
# Exit code 0 if the run did the work, non-zero otherwise.

set -euo pipefail

BUILD_DIR=""
LOG_FILE=""
EXPECTED=""
FLOOR=""
STRICT_RETRIES=0
ALLOW_SKIP=()
ALLOW_RETRY=()
CASE_REPORTS=""
ALLOW_CASE_SKIP=()
CTEST_FILTER_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)      BUILD_DIR="$2"; shift 2 ;;
        --log)            LOG_FILE="$2"; shift 2 ;;
        --expect)         EXPECTED="$2"; shift 2 ;;
        --floor)          FLOOR="$2"; shift 2 ;;
        --allow-skip)     ALLOW_SKIP+=("$2"); shift 2 ;;
        --strict-retries) STRICT_RETRIES=1; shift ;;
        --allow-retry)    ALLOW_RETRY+=("$2"); shift 2 ;;
        --case-reports)   CASE_REPORTS="$2"; shift 2 ;;
        --allow-case-skip) ALLOW_CASE_SKIP+=("$2"); shift 2 ;;
        --allow-retry-file)
            if [[ ! -f "$2" ]]; then
                echo "[check-test-run] --allow-retry-file not found: $2" >&2
                exit 2
            fi
            while IFS= read -r line || [[ -n "$line" ]]; do
                line="${line#"${line%%[![:space:]]*}"}"
                line="${line%%[[:space:]]*}"
                [[ -z "$line" || "$line" == \#* ]] && continue
                ALLOW_RETRY+=("$line")
            done < "$2"
            shift 2 ;;
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

# ── Check 0: the registry itself is not diminished ───────────────────────────
# Deliberately *before* the ran-vs-expected check. If configure dropped targets,
# both numbers agree at the lower value and Check 1 passes; this is the only
# assertion that notices. Ordering it first also means the error message a
# reader sees names the real cause rather than a downstream symptom.
if [[ -n "$FLOOR" ]]; then
    if ! [[ "$FLOOR" =~ ^[0-9]+$ ]]; then
        echo "[check-test-run] --floor must be a non-negative integer, got: $FLOOR" >&2
        exit 2
    fi
    if [[ "$EXPECTED" -lt "$FLOOR" ]]; then
        fail "only $EXPECTED tests are registered, below the floor of $FLOOR"
        echo "                 The suite got SMALLER. Nothing failed, because" >&2
        echo "                 the tests that vanished cannot fail -- which is" >&2
        echo "                 exactly why this check exists and why the" >&2
        echo "                 derived count cannot catch it." >&2
        echo "                 Likely causes: a find_package() that stopped" >&2
        echo "                 finding, an if(TARGET ...) guard that stopped" >&2
        echo "                 matching, or a dependency missing from the" >&2
        echo "                 runner image, so CMake skipped registering a" >&2
        echo "                 whole family of tests." >&2
        echo "                 Compare 'cmake' configure output against a" >&2
        echo "                 known-good run: the '-- <thing>: skipped'" >&2
        echo "                 lines are the ones that changed." >&2
        echo "                 If the removal is deliberate, lower the" >&2
        echo "                 --floor in .github/workflows/ci.yml in the same" >&2
        echo "                 commit, so it is a reviewed edit." >&2
        exit 1
    fi
    note "registered $EXPECTED tests, at or above the floor of $FLOOR"
fi

# ── Parse the run log ────────────────────────────────────────────────────────
# ctest result lines look like:
#     12/404 Test  #12: foo_test ...............   Passed    0.15 sec
#     13/404 Test  #13: bar_test ...............***Failed    1.23 sec
# Under --repeat, a retried test emits another line per attempt, without the
# "N/M" prefix (see Check 3), so these lines are first attempts. The set of
# distinct test numbers is still what "how many tests ran" means, in case a
# ctest version ever prefixes retries too.
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

# ── Check 2b: no Boost case or suite was skipped inside a passing binary ─────
# ctest sees one exit code per binary. Boost reports a case whose
# precondition() failed as "skipped" and still exits 0, so the checks above see
# a pass. The detailed report each test wrote (cmake/TestReports.cmake) lists
# those units by name:
#     Test case "suite/case" was skipped
#     Test suite "suite" was skipped
# Cases left out by a --run_test filter or marked disabled() are not listed
# that way, which is right: both are decided in the source, not at run time.
if [[ -n "$CASE_REPORTS" ]]; then
    if [[ ! -d "$CASE_REPORTS" ]]; then
        fail "--case-reports directory not found: $CASE_REPORTS"
        echo "                 The build was not configured with" >&2
        echo "                 -DKYTHIRA_TEST_REPORT_DIR=$CASE_REPORTS, so no" >&2
        echo "                 test wrote a case report." >&2
        exit 1
    fi
    RAN_NAMES="$(printf '%s\n' "$RESULT_LINES" \
        | sed -E 's/^ *[0-9]+\/[0-9]+ +Test +#[0-9]+: +([^ ]+) .*/\1/' \
        | sort -u)"
    N_REPORTS=0
    UNEXPECTED_CASE_SKIPS=()
    while IFS= read -r t; do
        [[ -z "$t" ]] && continue
        # Same rule as cmake/TestReports.cmake.
        f="$CASE_REPORTS/$(printf '%s' "$t" | sed -E 's/[^A-Za-z0-9_.-]/_/g').txt"
        [[ -f "$f" ]] || continue
        N_REPORTS=$((N_REPORTS + 1))
        while IFS= read -r unit; do
            [[ -z "$unit" ]] && continue
            allowed=0
            for re in ${ALLOW_CASE_SKIP[@]+"${ALLOW_CASE_SKIP[@]}"}; do
                if [[ "$t:$unit" =~ ^($re)$ ]]; then allowed=1; break; fi
            done
            if [[ "$allowed" -eq 1 ]]; then
                note "skipped case (allowlisted): $t:$unit"
            else
                UNEXPECTED_CASE_SKIPS+=("$t:$unit")
            fi
        done < <(sed -nE 's/^ *Test (case|suite) "(.*)" was skipped.*/\2/p' "$f")
    done <<< "$RAN_NAMES"

    if [[ "$N_REPORTS" -eq 0 ]]; then
        fail "none of the $RAN tests that ran left a Boost report in $CASE_REPORTS"
        echo "                 Every Boost.Test binary writes one when the build" >&2
        echo "                 is configured with -DKYTHIRA_TEST_REPORT_DIR, so" >&2
        echo "                 that setting did not take effect and no skipped" >&2
        echo "                 case could have been seen." >&2
        exit 1
    fi
    if [[ ${#UNEXPECTED_CASE_SKIPS[@]} -gt 0 ]]; then
        fail "${#UNEXPECTED_CASE_SKIPS[@]} Boost test case(s) or suite(s) were skipped inside passing tests"
        printf '                 %s\n' "${UNEXPECTED_CASE_SKIPS[@]}" >&2
        echo "                 ctest counts these tests as Passed, but the" >&2
        echo "                 listed units never ran: their precondition() was" >&2
        echo "                 false. Fix what the precondition needs, or add" >&2
        echo "                 --allow-case-skip '<test>:<unit>' to record that" >&2
        echo "                 the skip is expected in this job." >&2
        if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
            {
                echo "### Boost test units skipped inside passing tests (${#UNEXPECTED_CASE_SKIPS[@]})"
                echo ""
                echo '```'
                printf '%s\n' "${UNEXPECTED_CASE_SKIPS[@]}"
                echo '```'
            } >> "$GITHUB_STEP_SUMMARY"
        fi
        exit 1
    fi
    note "read $N_REPORTS Boost case report(s), no unexpected skipped case"
fi

# ── Check 3: first-attempt failures --repeat absorbed ───────────────────────
# ctest prints a retry's result line without the "N/M" progress prefix:
#      4/404 Test  #4: bar_test ...............***Failed    1.23 sec
#            Test  #4: bar_test ...............   Passed    0.98 sec
# so RESULT_LINES above holds first attempts only, and the verdict has to come
# from every result line. For each test number, the first such line is its
# first attempt and the last is its verdict. A test whose first attempt failed but whose verdict is Passed
# was rescued by --repeat until-pass, and nothing in the job status says so. A
# test that failed every attempt is not listed here: ctest already fails the
# run for it, and calling it a retry would understate it.
ALL_RESULT_LINES="$(grep -E '^ *([0-9]+/[0-9]+ +)?Test +#[0-9]+: ' "$LOG_FILE" || true)"
RETRY_RESCUED="$(printf '%s\n' "$ALL_RESULT_LINES" | awk '
    match($0, /Test +#[0-9]+:/) {
        num = substr($0, RSTART, RLENGTH)
        if (!(num in first)) { first[num] = $0; order[++n] = num }
        last[num] = $0
    }
    END {
        for (i = 1; i <= n; i++) {
            num = order[i]
            if (first[num] ~ /\*\*\*(Failed|Timeout|Exception|Not Run)/ &&
                last[num] ~ / Passed /)
                print first[num]
        }
    }')"

if [[ -n "$RETRY_RESCUED" ]]; then
    ALLOWED_LINES=()
    UNALLOWED_LINES=()
    while IFS= read -r l; do
        [[ -z "$l" ]] && continue
        t="$(printf '%s\n' "$l" | sed -E 's/^ *[0-9]+\/[0-9]+ +Test +#[0-9]+: +([^ .]+).*/\1/')"
        allowed=0
        for re in ${ALLOW_RETRY[@]+"${ALLOW_RETRY[@]}"}; do
            if [[ "$t" =~ ^($re)$ ]]; then allowed=1; break; fi
        done
        if [[ "$allowed" -eq 1 ]]; then
            ALLOWED_LINES+=("$l")
        else
            UNALLOWED_LINES+=("$l")
        fi
    done <<< "$RETRY_RESCUED"
    N_ALLOWED=${#ALLOWED_LINES[@]}
    N_UNALLOWED=${#UNALLOWED_LINES[@]}
    N_FIRST=$((N_ALLOWED + N_UNALLOWED))

    if [[ "$N_UNALLOWED" -gt 0 && "$STRICT_RETRIES" -eq 1 ]]; then
        fail "$N_UNALLOWED test(s) failed on their first attempt and passed only on retry"
    elif [[ "$N_UNALLOWED" -gt 0 ]]; then
        warn "$N_UNALLOWED test(s) failed on their first attempt and passed only on retry"
    fi
    if [[ "$N_UNALLOWED" -gt 0 ]]; then
        printf '%s\n' "${UNALLOWED_LINES[@]}"
        echo ""
        echo "[check-test-run] These are invisible in the job status when"
        echo "                 --repeat until-pass lets them pass on a later"
        echo "                 attempt. Each one is either a flake worth fixing or"
        echo "                 a real failure worth seeing. Fix the test, or, if"
        echo "                 the flake is understood and tracked, add it to"
        echo "                 .github/ctest-retry-allowlist.txt with the TODO.md"
        echo "                 entry that tracks it."
    fi
    if [[ "$N_ALLOWED" -gt 0 ]]; then
        note "$N_ALLOWED allowlisted test(s) passed only on retry:"
        printf '%s\n' "${ALLOWED_LINES[@]}"
    fi

    # Surface in the job summary too, so it survives log rotation and is
    # visible without opening the raw log -- which is how these went unnoticed.
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
        {
            echo "### Tests that passed only on retry (${N_FIRST})"
            echo ""
            echo 'Rescued by `--repeat until-pass`; the first attempt failed.'
            if [[ "$N_UNALLOWED" -gt 0 ]]; then
                echo ""
                echo "Not allowlisted (${N_UNALLOWED}):"
                echo ""
                echo '```'
                printf '%s\n' "${UNALLOWED_LINES[@]}"
                echo '```'
            fi
            if [[ "$N_ALLOWED" -gt 0 ]]; then
                echo ""
                echo "Allowlisted in \`.github/ctest-retry-allowlist.txt\` (${N_ALLOWED}):"
                echo ""
                echo '```'
                printf '%s\n' "${ALLOWED_LINES[@]}"
                echo '```'
            fi
        } >> "$GITHUB_STEP_SUMMARY"
    fi

    if [[ "$N_UNALLOWED" -gt 0 && "$STRICT_RETRIES" -eq 1 ]]; then
        exit 1
    fi
else
    note "no first-attempt failures"
fi

note "OK — $RAN test(s) ran, none unexpectedly skipped"
