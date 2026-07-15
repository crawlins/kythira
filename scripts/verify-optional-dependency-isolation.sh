#!/usr/bin/env bash
# Property 5: Optional Dependency Isolation
# (.kiro/specs/stdexec-future-backend/, Requirements 4.2, 4.3)
#
# For any build configuration where stdexec is not found, configuration
# and build of all Folly-backed and backend-independent targets should
# succeed unaffected.
#
# This is deliberately a CI/CMake-level check rather than a C++ test
# (tasks.md's own note on Task 1.1): it configures a fresh build directory
# with stdexec hidden from find_package() via CMake's own
# CMAKE_DISABLE_FIND_PACKAGE_<pkg> mechanism (no vcpkg_installed/ mutation
# needed — see feedback-vcpkg-installed-caution in project memory for why
# that would be the wrong way to do this), then builds a representative
# target to confirm the rest of the project is unaffected.
#
# Usage: scripts/verify-optional-dependency-isolation.sh
# Exit code 0 on success, non-zero otherwise.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

case "$(uname -m)" in
    aarch64|arm64) VCPKG_TRIPLET="arm64-linux" ;;
    *)             VCPKG_TRIPLET="x64-linux" ;;
esac

echo "[verify-optional-dependency-isolation] Configuring with stdexec hidden from find_package() ..."
CONFIGURE_LOG="$(mktemp)"
if ! cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
      -DCMAKE_PREFIX_PATH="$REPO_ROOT/vcpkg_installed/$VCPKG_TRIPLET" \
      -DCMAKE_DISABLE_FIND_PACKAGE_stdexec=ON \
      > "$CONFIGURE_LOG" 2>&1; then
    echo "[verify-optional-dependency-isolation] FAILED — configure step itself failed:"
    cat "$CONFIGURE_LOG"
    rm -f "$CONFIGURE_LOG"
    exit 1
fi

if ! grep -q "stdexec not found" "$CONFIGURE_LOG"; then
    echo "[verify-optional-dependency-isolation] FAILED — expected the graceful"
    echo "  'stdexec not found' warning; CMAKE_DISABLE_FIND_PACKAGE_stdexec may not"
    echo "  be taking effect (or the warning message changed — update this script)."
    rm -f "$CONFIGURE_LOG"
    exit 1
fi
rm -f "$CONFIGURE_LOG"
echo "[verify-optional-dependency-isolation] Configure OK — stdexec_FOUND is false as expected."

echo "[verify-optional-dependency-isolation] Building a representative Folly-backed target ..."
if ! cmake --build "$BUILD_DIR" -j"$(nproc)" --target kythira_test_pch; then
    echo "[verify-optional-dependency-isolation] FAILED — build broke when stdexec is unavailable."
    exit 1
fi

echo "[verify-optional-dependency-isolation] PASSED — configure and build succeed with stdexec hidden."
