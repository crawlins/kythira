#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Prove the alternate CoAP backends (cantcoap, libnyoci) were really built and
# really tested, rather than compiled to their stubs.
#
# Every coap_cantcoap_* and coap_libnyoci_* test target is registered and built
# whether or not its library was found. Without the library, each one compiles
# to a single "backend unavailable, skipped" case that passes. A green ctest run
# over those targets therefore says nothing on its own: before the
# alt-coap-backends CI job existed, all of them ran green in every leg while
# testing nothing (ci-build-matrix-coverage Requirement 1.3).
#
# Two subcommands, run at different points in the job so a missing backend
# fails before the build rather than after it:
#
#   configured --build-dir DIR
#       After configure. Asserts the Kconfig resolution turned both backend
#       symbols on (generated/autoconf.cmake) and that the compile commands for
#       each backend's test sources carry its *_AVAILABLE define. The second
#       check is the one that matters: it is the flag the stubs key off, and it
#       is set only when CMake actually found the library.
#
#   suites --build-dir DIR
#       After the build. Runs each alternate-backend test binary with
#       --list_content and fails if it lists a stub case, or lists fewer than
#       two cases. This catches a backend silently compiling to its stub,
#       which neither the configure check nor a test-count floor does.
#
# Usage:
#   scripts/check-coap-alt-backends.sh configured --build-dir build
#   scripts/check-coap-alt-backends.sh suites --build-dir build
#
# Exit code 0 when every check passes, 1 on a failed check, 2 on bad usage.

set -euo pipefail

# The stub cases' names, as they appear in tests/coap_{cantcoap,libnyoci}_*.cpp
# under `#else // *_AVAILABLE`, plus the `#if !defined(LAKERS_AVAILABLE)` cases:
# the job installs the edhoc feature, so those must not be compiled in either.
STUB_CASE_RE='(unavailable_is_skipped|skipped_without_|_without_lakers)'

# backend name -> compile definition the stubs key off.
declare -A BACKEND_DEFINE=(
    [cantcoap]=CANTCOAP_AVAILABLE
    [libnyoci]=LIBNYOCI_AVAILABLE
)
declare -A BACKEND_SYMBOL=(
    [cantcoap]=COAP_TRANSPORT_CANTCOAP
    [libnyoci]=COAP_TRANSPORT_LIBNYOCI
)

note() { echo "[check-coap-alt-backends] $*"; }
fail() { echo "::error::$*"; echo "[check-coap-alt-backends] FAILED: $*" >&2; FAILED=1; }

usage() {
    echo "usage: $0 {configured|suites} --build-dir DIR" >&2
    exit 2
}

[[ $# -ge 1 ]] || usage
MODE="$1"; shift
BUILD_DIR=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        *) usage ;;
    esac
done
[[ -n "$BUILD_DIR" ]] || usage
[[ "$MODE" == configured || "$MODE" == suites ]] || usage

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAILED=0

# The alternate-backend test sources, by backend. Read from the tree rather
# than hard-coded so a new coap_<backend>_*_test.cpp is covered automatically.
backend_sources() {
    local backend="$1"
    find "$SOURCE_DIR/tests" -maxdepth 1 -name "coap_${backend}_*_test.cpp" | sort
}

check_configured() {
    local autoconf="$BUILD_DIR/generated/autoconf.cmake"
    local commands="$BUILD_DIR/compile_commands.json"
    if [[ ! -f "$autoconf" ]]; then
        fail "$autoconf not found: was the tree configured with -DKYTHIRA_KCONFIG?"
        return
    fi
    if [[ ! -f "$commands" ]]; then
        fail "$commands not found: configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
        return
    fi

    local backend
    for backend in "${!BACKEND_DEFINE[@]}"; do
        local symbol="${BACKEND_SYMBOL[$backend]}"
        local define="${BACKEND_DEFINE[$backend]}"
        if grep -qx "set(KCONFIG_${symbol} ON)" "$autoconf"; then
            note "$backend: Kconfig resolved CONFIG_${symbol}=y"
        else
            fail "$backend: Kconfig did not resolve CONFIG_${symbol}=y (see $autoconf)"
        fi

        local sources
        mapfile -t sources < <(backend_sources "$backend")
        if [[ ${#sources[@]} -eq 0 ]]; then
            fail "$backend: no tests/coap_${backend}_*_test.cpp sources found"
            continue
        fi
        local src
        for src in "${sources[@]}"; do
            # One compile command per translation unit; ask for the one whose
            # "file" is this source and check its command line for the define.
            if python3 - "$commands" "$src" "$define" <<'EOF'
import json, os, shlex, sys
commands, src, define = sys.argv[1:]
src = os.path.realpath(src)
entries = [
    e for e in json.load(open(commands))
    if os.path.realpath(os.path.join(e.get("directory", ""), e.get("file", ""))) == src
]
if not entries:
    print(f"  no compile command for {src}", file=sys.stderr)
    sys.exit(1)
args = []
for e in entries:
    args += e["arguments"] if "arguments" in e else shlex.split(e.get("command", ""))
sys.exit(0 if f"-D{define}" in args else 1)
EOF
            then
                note "$backend: $(basename "$src") compiles with -D$define"
            else
                fail "$backend: $(basename "$src") does not compile with -D$define, so it builds as a stub"
            fi
        done
    done
}

check_suites() {
    local backend
    for backend in "${!BACKEND_DEFINE[@]}"; do
        local sources
        mapfile -t sources < <(backend_sources "$backend")
        local src
        for src in "${sources[@]}"; do
            local name binary listing count stubs
            name="$(basename "$src" .cpp)"
            binary="$(find "$BUILD_DIR" -type f -perm -u+x -name "$name" -print -quit)"
            if [[ -z "$binary" ]]; then
                fail "$name: binary not found under $BUILD_DIR (was it built?)"
                continue
            fi
            # Boost.Test prints --list_content to stderr: suites flush left,
            # cases indented, each enabled one marked with a trailing '*'.
            if ! listing="$("$binary" --list_content 2>&1)"; then
                fail "$name: --list_content exited non-zero"
                printf '%s\n' "$listing" >&2
                continue
            fi
            count="$(printf '%s\n' "$listing" | grep -cE '^[[:space:]]+[A-Za-z_][A-Za-z0-9_]*\*?$' || true)"
            stubs="$(printf '%s\n' "$listing" | grep -E "$STUB_CASE_RE" || true)"
            if [[ -n "$stubs" ]]; then
                fail "$name: lists a stub case, so it was built without its backend:"
                printf '%s\n' "$stubs" | sed 's/^/    /' >&2
            elif [[ "$count" -lt 2 ]]; then
                fail "$name: lists only $count test case(s); expected the real suite"
            else
                note "$name: $count real test cases"
            fi
        done
    done
}

case "$MODE" in
    configured) check_configured ;;
    suites)     check_suites ;;
esac

if [[ "$FAILED" -ne 0 ]]; then
    exit 1
fi
note "all checks passed ($MODE)"
