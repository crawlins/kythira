#!/usr/bin/env bash
# Builds docker/ca_cluster_node/Dockerfile's "builder" stage and copies the
# compiled ca_cluster_node binary out. Never runs the container — only
# creates, copies from, and removes it — so this script's only dependency on
# the Dockerfile is the "builder" stage's own compile steps, which it does
# not duplicate. See .kiro/specs/ca-cluster-node-ami/ Requirement 2.
set -euo pipefail

RUNTIME="${KYTHIRA_CONTAINER_RUNTIME:-docker}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

ARCH=""
OUT=""

usage() {
    cat <<'EOF'
Usage: extract-binary.sh --arch amd64|arm64 [--out PATH]

Builds docker/ca_cluster_node/Dockerfile's "builder" stage for the given
architecture and copies the compiled ca_cluster_node binary to PATH
(default: build/packer-artifacts/ca_cluster_node-{arch}).

Honors $KYTHIRA_CONTAINER_RUNTIME (default "docker"), per this project's
CLAUDE.md container-runtime-compatibility steering.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ "$ARCH" != "amd64" && "$ARCH" != "arm64" ]]; then
    echo "ERROR: --arch amd64|arm64 required" >&2
    usage >&2
    exit 1
fi
[[ -n "$OUT" ]] || OUT="${REPO_ROOT}/build/packer-artifacts/ca_cluster_node-${ARCH}"

DOCKER_PLATFORM="linux/${ARCH}"
IMAGE_TAG="kythira-ca-cluster-node-builder:${ARCH}"
CONTAINER_NAME="kythira-ca-cluster-node-extract-${ARCH}"

echo "[extract-binary] building ${IMAGE_TAG} (--platform ${DOCKER_PLATFORM})"
"${RUNTIME}" build --platform "${DOCKER_PLATFORM}" \
    -f "${REPO_ROOT}/docker/ca_cluster_node/Dockerfile" \
    --target builder -t "${IMAGE_TAG}" "${REPO_ROOT}"

echo "[extract-binary] extracting binary to ${OUT}"
mkdir -p "$(dirname "${OUT}")"
"${RUNTIME}" rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
"${RUNTIME}" create --name "${CONTAINER_NAME}" "${IMAGE_TAG}" >/dev/null
"${RUNTIME}" cp "${CONTAINER_NAME}:/src/build/ca_cluster_node" "${OUT}"
"${RUNTIME}" rm "${CONTAINER_NAME}" >/dev/null

if [[ ! -s "${OUT}" ]]; then
    echo "ERROR: extracted binary is missing or empty: ${OUT}" >&2
    exit 1
fi

echo "[extract-binary] wrote ${OUT}"
echo "${OUT}"
