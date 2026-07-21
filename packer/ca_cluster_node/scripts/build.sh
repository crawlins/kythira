#!/usr/bin/env bash
# Orchestrates a full ca_cluster_node AMI build: extract the binary, then
# packer init/validate/build. See .kiro/specs/ca-cluster-node-ami/.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TEMPLATE_DIR="${REPO_ROOT}/packer/ca_cluster_node"

ARCH=""
REGION="${AWS_REGION:-us-east-1}"
SKIP_BINARY_BUILD=0

usage() {
    cat <<'EOF'
Usage: build.sh --arch amd64|arm64 [--region REGION] [--skip-binary-build]

Builds a golden ca_cluster_node AMI: extracts the binary from
docker/ca_cluster_node/Dockerfile's builder stage (unless
--skip-binary-build), then runs packer init/validate/build. Prints the
resulting AMI ID as the last line of stdout on success.

  --arch ARCH            amd64 or arm64 (required)
  --region REGION        AWS region (default: $AWS_REGION or us-east-1)
  --skip-binary-build     Reuse an already-extracted binary at
                          build/packer-artifacts/ca_cluster_node-{arch}
                          instead of rebuilding it
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --region) REGION="$2"; shift 2 ;;
        --skip-binary-build) SKIP_BINARY_BUILD=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ "$ARCH" != "amd64" && "$ARCH" != "arm64" ]]; then
    echo "ERROR: --arch amd64|arm64 required" >&2
    usage >&2
    exit 1
fi

BINARY_PATH="${REPO_ROOT}/build/packer-artifacts/ca_cluster_node-${ARCH}"
if [[ "${SKIP_BINARY_BUILD}" -eq 0 ]]; then
    "${TEMPLATE_DIR}/scripts/extract-binary.sh" --arch "${ARCH}" --out "${BINARY_PATH}"
fi
if [[ ! -s "${BINARY_PATH}" ]]; then
    echo "ERROR: no binary at ${BINARY_PATH} (run extract-binary.sh first, or drop --skip-binary-build)" >&2
    exit 1
fi

echo "[build] checking AWS credentials"
if ! aws sts get-caller-identity --region "${REGION}" >/dev/null; then
    echo "ERROR: AWS credentials not configured for region ${REGION}." >&2
    exit 1
fi

GIT_SHA="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
PACKER_VARS=(
    -var "arch=${ARCH}"
    -var "region=${REGION}"
    -var "binary_path=${BINARY_PATH}"
    -var "git_sha=${GIT_SHA}"
)

# Pointed at TEMPLATE_DIR (not the single ca_cluster_node.pkr.hcl file):
# Packer's HCL2 loader only reads the exact file it's given when that file
# is passed directly, so variables.pkr.hcl's declarations in the same
# directory were never being loaded - every -var above failed real runs
# with "Undefined -var variable ... place this block in one of your .pkr
# files, such as variables.pkr.hcl", the block it was already in.
echo "[build] packer init"
packer init "${TEMPLATE_DIR}"

echo "[build] packer validate"
packer validate "${PACKER_VARS[@]}" "${TEMPLATE_DIR}"

echo "[build] packer build"
packer build "${PACKER_VARS[@]}" "${TEMPLATE_DIR}"

AMI_ID="$(python3 -c "
import json
with open('packer-manifest.json') as f:
    data = json.load(f)
print(data['builds'][-1]['artifact_id'].split(':')[-1])
")"
echo "${AMI_ID}"
