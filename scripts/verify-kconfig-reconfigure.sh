#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Checks that turning Kconfig symbols off in an existing build tree gives the
# same build as configuring a fresh tree with the same .config.
#
# Why: hand-written detection blocks in the root CMakeLists.txt run
# pkg_check_modules(), which keeps <PREFIX>_FOUND in the CMake *cache*. Before
# kythira_forget_pkg_found() (cmake/Kconfig.cmake), a tree configured once with
# CONFIG_DNS_DISCOVERY=y kept libldns and KYTHIRA_HAS_LDNS on every target
# after menuconfig set the symbol to n, while a fresh tree dropped them. Any
# detection result that outlives its own gate fails this check the same way.
#
# Three configures, no build:
#   1. tree A with the "on" config (minimal_defconfig plus the symbols below)
#   2. tree A again with the "off" config (minimal_defconfig as is)
#   3. fresh tree B with the "off" config
# then compares every target's defines, include dirs, link libraries and flags
# between A and B, with each tree's own path normalised out. It also requires
# step 1 to differ from step 3, so a host on which none of the toggled
# features was found fails loudly instead of passing without testing anything.
#
# Usage: scripts/verify-kconfig-reconfigure.sh [CONFIG_SYMBOL ...]
#   Symbols to switch on for step 1. Default: the optional features whose
#   detection uses pkg-config (CHAOS_TESTS, DNS_DISCOVERY, LIBSSH2_TESTS,
#   COAP_TRANSPORT_LIBNYOCI). At least one must be installed.
#
# Environment:
#   KYTHIRA_PREFIX_PATH       dependency prefix (default:
#                             $REPO_ROOT/vcpkg_installed/<triplet>)
#   KYTHIRA_BASE_DEFCONFIG    the "off" config (default:
#                             configs/minimal_defconfig)
#   KYTHIRA_EXTRA_CMAKE_ARGS  extra arguments for every configure, split on
#                             whitespace
# Requires ninja and python3 with kconfiglib, like any -DKYTHIRA_KCONFIG= run.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if [ "$#" -gt 0 ]; then
    SYMBOLS=("$@")
else
    SYMBOLS=(CHAOS_TESTS DNS_DISCOVERY LIBSSH2_TESTS COAP_TRANSPORT_LIBNYOCI)
fi

case "$(uname -m)" in
    aarch64|arm64) VCPKG_TRIPLET="arm64-linux" ;;
    *)             VCPKG_TRIPLET="x64-linux" ;;
esac
PREFIX_PATH="${KYTHIRA_PREFIX_PATH:-$REPO_ROOT/vcpkg_installed/$VCPKG_TRIPLET}"
BASE_DEFCONFIG="${KYTHIRA_BASE_DEFCONFIG:-$REPO_ROOT/configs/minimal_defconfig}"
read -r -a EXTRA_ARGS <<< "${KYTHIRA_EXTRA_CMAKE_ARGS:-}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

log() { echo "[verify-kconfig-reconfigure] $*"; }

# The "on" config: the base with each symbol's "is not set" line dropped and
# an explicit =y appended. kconfiglib takes the last assignment of a symbol.
OFF_CONFIG="$WORK/off.config"
ON_CONFIG="$WORK/on.config"
cp "$BASE_DEFCONFIG" "$OFF_CONFIG"
cp "$BASE_DEFCONFIG" "$ON_CONFIG"
for sym in "${SYMBOLS[@]}"; do
    sed -i "/^# CONFIG_${sym} is not set$/d; /^CONFIG_${sym}=/d" "$ON_CONFIG"
    echo "CONFIG_${sym}=y" >> "$ON_CONFIG"
done

configure() {  # <build dir> <config> <log>
    if ! cmake -S "$REPO_ROOT" -B "$1" -G Ninja \
          -DCMAKE_PREFIX_PATH="$PREFIX_PATH" \
          -DKYTHIRA_COMPILER_LAUNCHER=none \
          -DKYTHIRA_KCONFIG="$2" \
          "${EXTRA_ARGS[@]}" > "$3" 2>&1; then
        log "FAILED: configure of $1 with $(basename "$2") failed:"
        cat "$3"
        exit 1
    fi
}

# What a target is built with, minus the tree's own absolute path.
summarise() {  # <build dir> <output>
    sed "s#$1#<build>#g" "$1/build.ninja" \
        | grep -E '^\s+(DEFINES|INCLUDES|FLAGS|LINK_FLAGS|LINK_LIBRARIES|LINK_PATH) =|^build [^ ]+: CXX_(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY)_LINKER' \
        > "$2" || true
}

log "1/3 configuring tree A with ${SYMBOLS[*]} on ..."
configure "$WORK/a" "$ON_CONFIG" "$WORK/a-on.log"
summarise "$WORK/a" "$WORK/a-on.txt"

log "2/3 reconfiguring tree A with them off ..."
configure "$WORK/a" "$OFF_CONFIG" "$WORK/a-off.log"
summarise "$WORK/a" "$WORK/a-off.txt"

log "3/3 configuring fresh tree B with them off ..."
configure "$WORK/b" "$OFF_CONFIG" "$WORK/b-off.log"
summarise "$WORK/b" "$WORK/b-off.txt"

if [ ! -s "$WORK/b-off.txt" ]; then
    log "FAILED: found no targets in build.ninja; the summary pattern needs updating."
    exit 1
fi

if cmp -s "$WORK/a-on.txt" "$WORK/b-off.txt"; then
    log "FAILED: switching ${SYMBOLS[*]} on changed nothing, so none of their"
    log "  dependencies is installed here and this check would test nothing."
    log "  Install one (e.g. libfiu-dev for CHAOS_TESTS) or name other symbols."
    exit 1
fi

if ! diff -u "$WORK/b-off.txt" "$WORK/a-off.txt" > "$WORK/diff.txt"; then
    log "FAILED: the reconfigured tree (+) still differs from a fresh one (-)."
    log "  Some detection result outlived its Kconfig gate; see"
    log "  kythira_forget_pkg_found() in cmake/Kconfig.cmake."
    head -n 80 "$WORK/diff.txt"
    exit 1
fi

log "PASSED: turning ${SYMBOLS[*]} off in an existing tree matches a fresh configure."
