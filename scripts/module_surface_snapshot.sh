#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Record the Redis gateway's build surface, so the move into services/redis/
# can show it changed nothing (.kiro/specs/redis-gateway-module/ task 1,
# Requirement 7.2-7.3, design "Proof of no change").
#
# For configs/ci_full_defconfig with the module on, and again with
# CONFIG_REDIS_GATEWAY=n, it configures a build tree and writes to OUT_DIR:
#
#   on/targets.txt     every target name (`ninja -t targets all`, sorted,
#                      without paths and per-directory utility targets)
#   on/tests.json      `ctest --show-only=json-v1`, reduced to each test's
#                      name, labels, timeout and command, with the source
#                      and build roots replaced by <src> and <build>
#   on/help.txt        `redis_gateway_node --help`, which lists every
#                      variable with its default (the default-config dump)
#   off/targets.txt, off/tests.json   the same with the module off
#
# Run it on main before the move and on the move's branch after, then
# `diff -r` the two outputs. Module on: targets and tests equal, commands
# differing only in moved source paths, --help identical. Module off: equal.
#
# Usage: scripts/module_surface_snapshot.sh OUT_DIR [BUILD_ROOT]
#
#   BUILD_ROOT  where the two build trees go (default: OUT_DIR/build). Pass
#               the same vcpkg toolchain arguments CI uses through
#               SNAPSHOT_CMAKE_ARGS, e.g.
#               SNAPSHOT_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=/tmp/vcpkg/scripts/buildsystems/vcpkg.cmake"
#   SNAPSHOT_BUILD_HELP=0  skip building redis_gateway_node (no help.txt).
#
# Needs cmake, ninja, ctest, python3 with kconfiglib.

set -euo pipefail

if [[ $# -lt 1 || "$1" == "-h" || "$1" == "--help" ]]; then
    echo "usage: $0 OUT_DIR [BUILD_ROOT]" >&2
    echo "  records targets, ctest JSON and redis_gateway_node --help for" >&2
    echo "  ci_full_defconfig with the Redis module on and off." >&2
    exit 2
fi

SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(mkdir -p "$1" && cd "$1" && pwd)
BUILD_ROOT=${2:-"${OUT}/build"}
mkdir -p "${BUILD_ROOT}"
BUILD_ROOT=$(cd "${BUILD_ROOT}" && pwd)
read -r -a EXTRA <<<"${SNAPSHOT_CMAKE_ARGS:-}"

# Reduce ctest's JSON (in file $2) to what the move must preserve, roots
# normalised. The JSON comes in as a file: the program itself is stdin.
normalise_tests() {
    local build="$1" json="$2"
    python3 - "${SRC}" "${build}" "${json}" <<'PY'
import json, sys
src, build, path = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path) as fh:
    doc = json.load(fh)

def norm(s):
    return s.replace(build, "<build>").replace(src, "<src>")

out = []
for t in doc.get("tests", []):
    props = {p["name"]: p["value"] for p in t.get("properties", [])}
    out.append({
        "name": t["name"],
        "labels": sorted(props.get("LABELS", [])),
        "timeout": props.get("TIMEOUT"),
        "command": [norm(c) for c in t.get("command", [])],
    })
out.sort(key=lambda t: t["name"])
json.dump(out, sys.stdout, indent=1, sort_keys=True)
sys.stdout.write("\n")
PY
}

snapshot() {
    local mode="$1"
    local build="${BUILD_ROOT}/${mode}"
    local dest="${OUT}/${mode}"
    local config="${build}/.config"
    mkdir -p "${build}" "${dest}"

    if [[ "${mode}" == "off" ]]; then
        # Rewritten rather than appended to: genconfig.py treats a symbol set
        # twice as an error. The REDIS_GATEWAY_* symbols depend on the
        # module, so they go too.
        sed -e 's/^CONFIG_REDIS_GATEWAY=y$/# CONFIG_REDIS_GATEWAY is not set/' \
            -e '/^CONFIG_REDIS_GATEWAY_[A-Z_]*=/d' \
            "${SRC}/configs/ci_full_defconfig" >"${config}"
    else
        cp "${SRC}/configs/ci_full_defconfig" "${config}"
    fi

    echo "== ${mode}: configuring ${build}" >&2
    cmake -S "${SRC}" -B "${build}" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DKYTHIRA_KCONFIG="${config}" "${EXTRA[@]}" >"${dest}/configure.log" 2>&1 || {
        echo "configure failed; see ${dest}/configure.log" >&2
        exit 1
    }
    if grep -q '^set(KCONFIG_REDIS_GATEWAY ON)$' "${build}/generated/autoconf.cmake"; then
        [[ "${mode}" == "on" ]] || { echo "module still on in the off build" >&2; exit 1; }
    else
        [[ "${mode}" == "off" ]] || { echo "module off in the on build" >&2; exit 1; }
    fi

    # Target names only: ninja also lists every output file and the CMake
    # files a re-run depends on, by path, and the per-directory utility
    # targets (<dir>/all, <dir>/test, ...), all of which name directories
    # rather than say what the build makes.
    ninja -C "${build}" -t targets all | sed 's/:.*$//' | grep -v / |
        LC_ALL=C sort -u >"${dest}/targets.txt"
    (cd "${build}" && ctest --show-only=json-v1) >"${build}/ctest.json"
    normalise_tests "${build}" "${build}/ctest.json" >"${dest}/tests.json"

    if [[ "${mode}" == "on" && "${SNAPSHOT_BUILD_HELP:-1}" != "0" ]]; then
        echo "== on: building redis_gateway_node for --help" >&2
        ninja -C "${build}" redis_gateway_node >"${dest}/build.log" 2>&1 || {
            echo "build failed; see ${dest}/build.log" >&2
            exit 1
        }
        "${build}/redis_gateway_node" --help >"${dest}/help.txt"
    fi
    rm -f "${dest}/configure.log" "${dest}/build.log"
}

snapshot on
snapshot off
echo "surface snapshot written to ${OUT}" >&2
