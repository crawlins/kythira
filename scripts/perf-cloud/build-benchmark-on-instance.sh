#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Builds `multi_raft_http_benchmark_test` on a bare Ubuntu 24.04 cloud
# instance and leaves it at /tmp/multi_raft_http_benchmark_test.
#
# This is the FALLBACK path of run-aws-shape-1.sh, not its normal one.
# Shipping a prebuilt binary is preferred, because Requirement 18.7 wants
# the local-vs-cloud delta to be the hardware and rebuilding adds a second
# compiler to it. The case this exists for is an architecture with no build
# host: there is no arm64 machine on the development side of this project,
# so Requirement 18.12's Graviton row is either built here or not measured.
#
# The recipe is .github/workflows/ci.yml's, step for step and pin for pin:
# g++-13, Release, configs/ci_full_defconfig, the --x-feature=edhoc feature
# set, and the same pinned vcpkg commit. Diverging from it would make the
# cloud row a measurement of a different program.
#
# Usage:  build-benchmark-on-instance.sh <git-ref>
set -euo pipefail

REF="${1:?usage: build-benchmark-on-instance.sh <git-ref>}"
REPO_URL="https://github.com/crawlins/kythira.git"
SRC=/home/ubuntu/kythira
# The same commit ci.yml pins. Pinning the vcpkg *tool and scripts*, not
# just the port versions via builtin-baseline, is what keeps a cold build
# reproducible: an upstream vcpkg-tool regression has broken every port
# build in this project before, on a branch that touched no dependency.
VCPKG_COMMIT=9a7f7340a6c5f11f24c3d59f85e07143feb84e06

echo "=== $(date -u +%FT%TZ) apt ==="
export DEBIAN_FRONTEND=noninteractive
for attempt in 1 2 3; do
    if sudo apt-get update -q \
       && sudo apt-get install -y --no-install-recommends \
            build-essential gcc-13 g++-13 cmake ninja-build libfiu-dev ccache \
            autoconf autoconf-archive automake libtool \
            git curl zip unzip tar pkg-config python3-pip python3-venv; then
        break
    fi
    echo "apt attempt ${attempt} failed; retrying in 30s"
    sleep 30
done

echo "=== $(date -u +%FT%TZ) rust (lakers/edhoc overlay port builds from source) ==="
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | sh -s -- -y --default-toolchain stable --profile minimal
# shellcheck disable=SC1091
. "$HOME/.cargo/env"

echo "=== $(date -u +%FT%TZ) source ==="
rm -rf "$SRC"
git clone --filter=blob:none "$REPO_URL" "$SRC"
git -C "$SRC" checkout "$REF"
git -C "$SRC" rev-parse HEAD

echo "=== $(date -u +%FT%TZ) python tools ==="
# --break-system-packages: Ubuntu 24.04 marks the system interpreter
# EXTERNALLY-MANAGED, and this is a throwaway instance that is about to be
# destroyed. A venv would work equally well and would then have to be on
# PATH for the cmake invocation below; this is the smaller moving part.
pip3 install --break-system-packages -r "$SRC/scripts/kconfig/requirements.txt"

# vcpkg's detect_compiler port configures a tiny CMake project of its own,
# which needs a C compiler as well as a C++ one. `g++-13` pulls in gcc-13
# as a library dependency but installs no `cc` or `gcc` on PATH, and a bare
# Ubuntu cloud image has neither -- unlike a GitHub runner image, which is
# where this recipe came from. The first arm64 run failed here with
# "vcpkg was unable to detect the active compiler's information", which
# names the symptom and not the cause. build-essential above supplies the
# aliases; CC/CXX below make the version explicit rather than leaving it to
# whichever gcc the alias points at.
export CC=gcc-13
export CXX=g++-13
echo "  CC=$(command -v "$CC") CXX=$(command -v "$CXX") cc=$(command -v cc || echo MISSING)"

echo "=== $(date -u +%FT%TZ) vcpkg ==="
rm -rf /tmp/vcpkg
git clone --filter=blob:none https://github.com/microsoft/vcpkg.git /tmp/vcpkg
git -C /tmp/vcpkg checkout "$VCPKG_COMMIT"
/tmp/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ARCH=$(uname -m)
case "$ARCH" in
    aarch64) TRIPLET=arm64-linux ;;
    x86_64)  TRIPLET=x64-linux ;;
    *) echo "unsupported architecture $ARCH" >&2; exit 1 ;;
esac
echo "triplet: $TRIPLET"
cd "$SRC"
/tmp/vcpkg/vcpkg install --triplet "$TRIPLET" --no-print-usage --x-feature=edhoc

echo "=== $(date -u +%FT%TZ) configure ==="
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++-13 \
    -DKYTHIRA_KCONFIG=configs/ci_full_defconfig \
    -DKYTHIRA_KCONFIG_STRICT=ON \
    -DCMAKE_PREFIX_PATH="$SRC/vcpkg_installed/$TRIPLET"

echo "=== $(date -u +%FT%TZ) build ==="
# One target. The tree is hundreds of test binaries and this needs one.
cmake --build build --target multi_raft_http_benchmark_test -j"$(nproc)"

cp "$SRC/build/tests/multi_raft_http_benchmark_test" /tmp/
chmod +x /tmp/multi_raft_http_benchmark_test
echo "=== $(date -u +%FT%TZ) done ==="
ls -l /tmp/multi_raft_http_benchmark_test
file /tmp/multi_raft_http_benchmark_test || true
