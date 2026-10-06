#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Build only what clang-tidy needs before it can analyse a configured tree:
# every file a CUSTOM_COMMAND generates (Protobuf/gRPC sources and headers)
# and every precompiled header the compile commands reference. Requirement
# 3.2 of .kiro/specs/ci-build-matrix-coverage/: without them a translation
# unit fails on a missing include, and that failure is not a real finding.
#
# This is far cheaper than a full build: it compiles no project translation
# units, only protoc's output inputs and the PCH(s).
#
# Usage: scripts/build-tidy-prerequisites.sh <ninja build dir>
set -euo pipefail

build_dir=${1:?usage: $0 <ninja build dir>}
[[ -f "$build_dir/build.ninja" ]] || {
    echo "error: $build_dir has no build.ninja (configure with -G Ninja)" >&2
    exit 1
}

# `ninja -t targets all` prints "<output>: <rule>". Custom-command outputs
# under CMakeFiles/ are utility-target stamps (format, static-analysis, ...),
# not generated sources, and building them would run those targets.
mapfile -t targets < <(
    ninja -C "$build_dir" -t targets all |
        awk -F': ' '
            $2 == "CUSTOM_COMMAND" && $1 !~ /(^|\/)CMakeFiles\// { print $1; next }
            $1 ~ /\.pch$/                                      { print $1 }
        ' | sort -u
)

if ((${#targets[@]} == 0)); then
    echo "No generated sources or precompiled headers to build."
    exit 0
fi

printf 'Building %d clang-tidy prerequisite(s):\n' "${#targets[@]}"
printf '  %s\n' "${targets[@]}"
ninja -C "$build_dir" "${targets[@]}"
