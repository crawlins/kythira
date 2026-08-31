#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Build the Tier E host image. `.kiro/specs/multi-raft-host-binary/` task 8.
#
# This script exists for one reason that is not obvious and cost a round of
# verification to find: **Podman must be told `--format docker`, or it silently
# drops the `HEALTHCHECK`.** Its default output is the OCI image format, which
# has no healthcheck field; it prints
#
#     HEALTHCHECK is not supported for OCI image format and will be ignored
#
# to stderr among the build noise and produces a working image with no health
# check in it. The compose stack's readiness gate is that health check, so a
# stack built the obvious way comes up "running" and never "healthy", and
# nothing fails — it just stops meaning anything. Docker's builder emits the
# Docker format by default and does not take the flag at all, so it is added
# only for Podman.
#
# The binary is COPIED, never rebuilt in the container.
# `.kiro/specs/multi-raft-performance/` doctrine 134: a container that rebuilt
# would fold a different compiler and a different dependency resolution into
# the very delta the tier exists to isolate.

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: build-tier-e-image.sh [options]

  --build-dir DIR   Where multi_raft_node was built (default build-default).
  --tag TAG         Image tag (default kythira-multi-raft-node:dev).
  --rootful         Build into the root container store (sudo). Rootful bridge
                    networking is the closest local analogue to Docker's, which
                    is the runtime CLAUDE.md names as CI's default.
  --help

Honours $KYTHIRA_CONTAINER_RUNTIME (default "docker"), like the rest of this
project's container tooling — see `container_runtime()` in
tests/docker_chaos/os_faults.hpp.
USAGE
}

BUILD_DIR="build-default"
TAG="kythira-multi-raft-node:dev"
ROOTFUL=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --tag) TAG="$2"; shift 2 ;;
        --rootful) ROOTFUL=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "build-tier-e-image.sh: unknown option $1" >&2; usage >&2; exit 2 ;;
    esac
done

RUNTIME="${KYTHIRA_CONTAINER_RUNTIME:-docker}"
if ! command -v "${RUNTIME}" >/dev/null 2>&1; then
    echo "build-tier-e-image.sh: container runtime '${RUNTIME}' not found. Set" >&2
    echo "  \$KYTHIRA_CONTAINER_RUNTIME to one that is installed (docker, podman)." >&2
    exit 2
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

BINARY="${BUILD_DIR}/multi_raft_node"
if [[ ! -x "${BINARY}" ]]; then
    echo "build-tier-e-image.sh: ${BINARY} is not built." >&2
    echo "  cmake --build ${BUILD_DIR} --target multi_raft_node" >&2
    exit 2
fi

mkdir -p docker/multi_raft_node/dist
cp "${BINARY}" docker/multi_raft_node/dist/multi_raft_node

# See the header: this flag is why the script exists.
FORMAT_ARGS=()
if [[ "$(basename "${RUNTIME}")" == "podman" ]]; then
    FORMAT_ARGS=(--format docker)
fi

PREFIX=()
if [[ ${ROOTFUL} -eq 1 ]]; then
    PREFIX=(sudo)
fi

"${PREFIX[@]}" "${RUNTIME}" build "${FORMAT_ARGS[@]}" \
    -f docker/multi_raft_node/Dockerfile -t "${TAG}" .

# Verified rather than assumed: an image whose health check was dropped looks
# identical from the outside until the compose stack never reports healthy.
if ! "${PREFIX[@]}" "${RUNTIME}" inspect "${TAG}" 2>/dev/null | grep -qi '"healthcheck"\|"Healthcheck"'; then
    echo "build-tier-e-image.sh: the built image has NO health check." >&2
    echo "  The compose stack's readiness gate depends on it, and a stack" >&2
    echo "  without one comes up 'running' and never 'healthy' while nothing" >&2
    echo "  reports an error. Refusing to hand over an image that would do that." >&2
    exit 1
fi

echo "build-tier-e-image.sh: built ${TAG} with ${RUNTIME}$([[ ${ROOTFUL} -eq 1 ]] && echo ' (rootful)'), health check present"
