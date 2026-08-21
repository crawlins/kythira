#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

#
# Run one real-cloud integration suite via CTest, failing closed when the suite
# skips instead of running.
#
# Every real-cloud suite exits 77 when its preflight cannot reach the provider
# — absent credentials, a revoked role, a disabled API, an unset required env
# var. CTest treats that (via each test's SKIP_RETURN_CODE 77 property) as
# "Not Run" and exits 0, printing "100% tests passed" for a run that executed
# no assertions. For a job that spends real money to prove an integration
# works, a skip is a misconfigured run, not a passing one, and must not report
# as a green tick.
#
# `--no-tests=error` closes the other silent pass: a `-R` filter that matches
# nothing at all, which CTest would likewise report as success.
#
# Usage: run-real-cloud-suite.sh <ctest-label> <test-name-regex> [hint]
#
#   hint  optional provider-specific remediation text appended to the error
#         message emitted when the suite skips.
#
# Env: BUILD_DIR overrides the CMake build directory (default: build).

set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "usage: $(basename "$0") <ctest-label> <test-name-regex> [hint]" >&2
    exit 2
fi

LABEL="$1"
TEST_REGEX="$2"
HINT="${3:-}"
BUILD_DIR="${BUILD_DIR:-build}"

LOG="$(mktemp -t real-cloud-ctest.XXXXXX.log)"
trap 'rm -f "$LOG"' EXIT

# pipefail (set above) matters here: without it the pipeline would report tee's
# exit status and a genuine test failure would pass. GitHub's default `run:`
# shell is `bash -e`, which does not enable pipefail on its own, so this script
# sets it rather than relying on the caller.
# -V, not --output-on-failure. These suites cost real money and real minutes,
# and a pass that shows nothing is not evidence: with --output-on-failure a
# green run printed no test output at all, so there was no way to tell whether
# a case had done its work or taken an internal skip path. The whole point of
# a real-cloud run is the record of what it actually did against the provider.
#
# -V also streams as the test runs rather than buffering to the end, which
# matters on a timeout: ctest SIGKILLs the child, and anything still sitting in
# a buffer dies with it. A GCP run burned its full 3600s timeout and produced
# not one line of output for exactly that reason.
#
# BOOST_TEST_LOG_LEVEL=message is the third piece of the same problem, and the
# one that made -V worth less than it looked. Boost's default log level is
# `error`, which discards every BOOST_TEST_MESSAGE record — so all the
# diagnostics these suites write that way (which rung was refused, which zone
# a node landed in, why a case took an internal skip) were dropped before ctest
# ever saw them. Only the std::cerr trace() markers survived, and they were the
# sole reason the Aug 5 2026 escalation run could be interpreted at all.
#
# Set as an env var rather than as a `--log_level=message` argument because
# ctest invokes each test through its registered command line; there is no
# per-invocation way to append an argument to it. Boost.Test reads this
# variable itself, so it reaches every case in every real-cloud suite.
export BOOST_TEST_LOG_LEVEL="${BOOST_TEST_LOG_LEVEL:-message}"

ctest --test-dir "$BUILD_DIR" -L "$LABEL" -R "$TEST_REGEX" \
    --no-tests=error -V 2>&1 | tee "$LOG"

if grep -qE '\*\*\*Skipped|tests did not run' "$LOG"; then
    echo "::error::${TEST_REGEX} was SKIPPED, not run — the suite's preflight could not reach the provider, so nothing was verified. ${HINT}" >&2
    exit 1
fi
