# Design Document

## Overview

This document describes the design for a Packer-based build pipeline that
produces a golden, secret-free AMI with `/usr/local/bin/ca_cluster_node` and
its systemd unit pre-installed. It replaces the placeholder "(baked into an
AMI, e.g. via Packer)" language in `docker/ca_cluster_node/README.md` and
gives `tests/ca_cluster_node_real_ec2_test.cpp`'s `KYTHIRA_EC2_TEST_AMI`
environment variable a real, scripted producer.

The binary itself is never recompiled by this pipeline — it is extracted
from `docker/ca_cluster_node/Dockerfile`'s existing `builder` stage, so
there remains exactly one place in the repository that knows how to compile
`ca_cluster_node` from source.

## Architecture

```
                          ┌───────────────────────────────────────┐
                          │  docker/ca_cluster_node/Dockerfile     │
                          │  (existing, unmodified)                │
                          │  stage "builder": compiles the binary  │
                          └───────────────┬─────────────────────────┘
                                          │ docker build --target builder
                                          ▼
packer/ca_cluster_node/scripts/extract-binary.sh
   docker create → docker cp /src/build/ca_cluster_node → docker rm
                                          │
                                          ▼
                          ./build/ca_cluster_node-{arch}   (local artifact)
                                          │
                                          ▼
packer/ca_cluster_node/scripts/build.sh
   packer init → packer validate → packer build \
       -var arch={arch} -var binary_path=... -var git_sha=$(git rev-parse HEAD)
                                          │
                                          ▼
              packer/ca_cluster_node/ca_cluster_node.pkr.hcl
   ┌──────────────────────────────────────────────────────────────┐
   │ data "amazon-parameterstore" "ubuntu"                        │
   │   → resolves current Canonical Ubuntu 24.04 AMI for {arch}   │
   │                                                                │
   │ source "amazon-ebs" "ca_cluster_node"                         │
   │   → launches a temporary EC2 instance from that AMI           │
   │                                                                │
   │ provisioner "file"   → uploads binary_path, ca_cluster_node.service │
   │ provisioner "shell"  → runs scripts/provision.sh              │
   │                                                                │
   │ → snapshots root volume, registers new AMI, tags it           │
   │ post-processor "manifest" → writes packer-manifest.json       │
   └──────────────────────────────────────────────────────────────┘
                                          │
                                          ▼
                          ami-xxxxxxxxxxxxxxxxx  (new AMI)
                     consumed by: aws_ec2_quorum_manager_config.image_id
                                  KYTHIRA_EC2_TEST_AMI (real-EC2 tests)
```

## File Layout

```
packer/ca_cluster_node/
├── ca_cluster_node.pkr.hcl        # main Packer template
├── variables.pkr.hcl              # variable declarations + defaults
├── scripts/
│   ├── extract-binary.sh          # docker build --target builder + docker cp
│   ├── build.sh                   # orchestrates the full build
│   └── provision.sh               # in-instance provisioning (uploaded, then run)
└── README.md                      # usage, tagging scheme, cleanup commands
```

No changes to `docker/ca_cluster_node/Dockerfile` or `ca_cluster_node.service`
are required — both are consumed as-is.

## Components and Interfaces

### 1. `packer/ca_cluster_node/variables.pkr.hcl`

```hcl
variable "arch" {
  type        = string
  description = "Target architecture: \"amd64\" or \"arm64\"."

  validation {
    condition     = contains(["amd64", "arm64"], var.arch)
    error_message = "arch must be \"amd64\" or \"arm64\"."
  }
}

variable "region" {
  type    = string
  default = "us-east-1"
}

variable "binary_path" {
  type        = string
  description = "Local path to the extract-binary.sh output for this arch."
}

variable "git_sha" {
  type        = string
  description = "Full commit SHA the binary was built from (tags + Name)."
}

variable "builder_instance_type" {
  type    = string
  default = ""  # "" => resolved from arch in locals (t3.micro / t4g.micro)
}

variable "ssh_username" {
  type    = string
  default = "ubuntu"
}
```

### 2. `packer/ca_cluster_node/ca_cluster_node.pkr.hcl`

```hcl
packer {
  required_plugins {
    amazon = {
      version = ">= 1.3.0"
      source  = "github.com/hashicorp/amazon"
    }
  }
}

locals {
  timestamp = formatdate("YYYYMMDD-hhmmss", timestamp())
  short_sha = substr(var.git_sha, 0, 7)
  ami_name  = "kythira-ca-cluster-node-${local.short_sha}-${var.arch}-${local.timestamp}"

  instance_type = var.builder_instance_type != "" ? var.builder_instance_type : (
    var.arch == "arm64" ? "t4g.micro" : "t3.micro"
  )

  common_tags = {
    "kythira:component" = "ca-cluster-node"
    "kythira:git-sha"   = var.git_sha
    "kythira:arch"      = var.arch
    "kythira:built-by"  = "packer"
  }
}

# Requirement 3.2 — Canonical's published SSM parameter, never a literal AMI ID.
data "amazon-parameterstore" "ubuntu" {
  name   = "/aws/service/canonical/ubuntu/server/24.04/stable/current/${var.arch}/hvm/ebs-gp3/ami-id"
  region = var.region
}

source "amazon-ebs" "ca_cluster_node" {
  region        = var.region
  source_ami    = data.amazon-parameterstore.ubuntu.value
  instance_type = local.instance_type
  ssh_username  = var.ssh_username
  ami_name      = local.ami_name

  ami_description = "kythira ca_cluster_node — commit ${var.git_sha}, ${var.arch}"

  tags        = merge(local.common_tags, { "kythira:base-ami" = data.amazon-parameterstore.ubuntu.value, "Name" = local.ami_name })
  run_tags    = merge(local.common_tags, { "Name" = "kythira-ca-cluster-node-ami-build-${var.arch}" })
  snapshot_tags = local.common_tags

  # 8 GiB is comfortably more than the binary + runtime libs need; matches
  # the default Ubuntu 24.04 root volume size rather than shrinking it.
  launch_block_device_mappings {
    device_name           = "/dev/sda1"
    volume_size           = 8
    volume_type           = "gp3"
    delete_on_termination = true
  }
}

build {
  sources = ["source.amazon-ebs.ca_cluster_node"]

  provisioner "file" {
    source      = var.binary_path
    destination = "/tmp/ca_cluster_node"
  }

  provisioner "file" {
    source      = "${path.root}/../../docker/ca_cluster_node/ca_cluster_node.service"
    destination = "/tmp/ca_cluster_node.service"
  }

  provisioner "file" {
    source      = "${path.root}/scripts/provision.sh"
    destination = "/tmp/provision.sh"
  }

  provisioner "shell" {
    inline = ["chmod +x /tmp/provision.sh", "sudo /tmp/provision.sh"]
  }

  post-processor "manifest" {
    output     = "packer-manifest.json"
    strip_path = true
    custom_data = {
      arch    = var.arch
      git_sha = var.git_sha
    }
  }
}
```

`${path.root}/../../docker/ca_cluster_node/...` reaches the repository's
existing `docker/ca_cluster_node/` directory from
`packer/ca_cluster_node/` without copying the unit file — the same file
Packer uploads is the one already reviewed and tested for Path 1's manual
systemd install.

### 3. `packer/ca_cluster_node/scripts/provision.sh`

```bash
#!/usr/bin/env bash
# Runs INSIDE the Packer builder instance (uploaded by the "file"
# provisioner, executed by the "shell" provisioner as root via sudo).
# Installs software only — no secrets, no per-node configuration
# (.kiro/specs/ca-cluster-node-ami/requirements.md Requirement 5).
set -euo pipefail

echo "[provision] installing runtime packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y --no-install-recommends libssl3 curl
rm -rf /var/lib/apt/lists/*

echo "[provision] installing ca_cluster_node binary"
install -m 0755 -o root -g root /tmp/ca_cluster_node /usr/local/bin/ca_cluster_node
rm -f /tmp/ca_cluster_node

echo "[provision] creating ca-cluster-node system user"
if ! id -u ca-cluster-node >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /usr/sbin/nologin ca-cluster-node
fi

echo "[provision] creating data and config directories"
install -d -m 0750 -o ca-cluster-node -g ca-cluster-node /var/lib/ca_cluster_node
install -d -m 0750 -o ca-cluster-node -g ca-cluster-node /etc/ca_cluster_node
# Deliberately empty: unseal.key / rpc_bootstrap.{crt,key} are installed
# per-instance at launch time, never baked into the AMI (Requirement 5).

echo "[provision] installing systemd unit"
install -m 0644 -o root -g root /tmp/ca_cluster_node.service \
    /etc/systemd/system/ca_cluster_node.service
rm -f /tmp/ca_cluster_node.service
systemctl daemon-reload
systemctl enable ca_cluster_node
# Deliberately NOT started here — /etc/default/ca_cluster_node (the unit's
# EnvironmentFile) does not exist yet; starting now would just crash-loop.

echo "[provision] AMI hygiene"
# Standard golden-AMI cleanup so every instance launched from this AMI gets
# a fresh identity rather than inheriting the builder instance's.
rm -f /etc/machine-id
touch /etc/machine-id
rm -f /etc/ssh/ssh_host_*_key /etc/ssh/ssh_host_*_key.pub
cloud-init clean --logs || true
rm -rf /home/ubuntu/.bash_history /root/.bash_history

echo "[provision] done"
```

### 4. `packer/ca_cluster_node/scripts/extract-binary.sh`

```bash
#!/usr/bin/env bash
# Builds docker/ca_cluster_node/Dockerfile's "builder" stage and copies the
# compiled binary out — never runs the container, only creates+copies+removes
# it, so nothing built for ca_service or ca_cluster_node ever needs to
# actually execute in this step.
set -euo pipefail

RUNTIME="${KYTHIRA_CONTAINER_RUNTIME:-docker}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

ARCH=""
OUT=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done
[[ -n "$ARCH" ]] || { echo "ERROR: --arch amd64|arm64 required" >&2; exit 1; }
[[ -n "$OUT" ]] || OUT="${REPO_ROOT}/build/packer-artifacts/ca_cluster_node-${ARCH}"

DOCKER_PLATFORM="linux/${ARCH}"
IMAGE_TAG="kythira-ca-cluster-node-builder:${ARCH}"
CONTAINER_NAME="kythira-ca-cluster-node-extract-${ARCH}"

echo "[extract-binary] building ${IMAGE_TAG} (--platform ${DOCKER_PLATFORM})"
"${RUNTIME}" build --platform "${DOCKER_PLATFORM}" \
    -f "${REPO_ROOT}/docker/ca_cluster_node/Dockerfile" \
    --target builder -t "${IMAGE_TAG}" "${REPO_ROOT}"

echo "[extract-binary] extracting binary"
mkdir -p "$(dirname "${OUT}")"
"${RUNTIME}" rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
"${RUNTIME}" create --name "${CONTAINER_NAME}" "${IMAGE_TAG}"
"${RUNTIME}" cp "${CONTAINER_NAME}:/src/build/ca_cluster_node" "${OUT}"
"${RUNTIME}" rm "${CONTAINER_NAME}" >/dev/null

if [[ ! -s "${OUT}" ]]; then
    echo "ERROR: extracted binary is missing or empty: ${OUT}" >&2
    exit 1
fi

echo "[extract-binary] wrote ${OUT}"
echo "${OUT}"
```

`--platform` cross-building relies on Docker Buildx + QEMU when the host
architecture differs from `--arch`; Requirement 9.3 prefers running this
script natively on a matching-architecture host/CI runner instead, since
compiling Folly under QEMU emulation is dramatically slower than native.

### 5. `packer/ca_cluster_node/scripts/build.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TEMPLATE_DIR="${REPO_ROOT}/packer/ca_cluster_node"

ARCH=""
REGION="${AWS_REGION:-us-east-1}"
SKIP_BINARY_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --region) REGION="$2"; shift 2 ;;
        --skip-binary-build) SKIP_BINARY_BUILD=1; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done
[[ -n "$ARCH" ]] || { echo "ERROR: --arch amd64|arm64 required" >&2; exit 1; }

BINARY_PATH="${REPO_ROOT}/build/packer-artifacts/ca_cluster_node-${ARCH}"
if [[ "${SKIP_BINARY_BUILD}" -eq 0 ]]; then
    "${TEMPLATE_DIR}/scripts/extract-binary.sh" --arch "${ARCH}" --out "${BINARY_PATH}"
fi
[[ -s "${BINARY_PATH}" ]] || { echo "ERROR: no binary at ${BINARY_PATH} (use extract-binary.sh or drop --skip-binary-build)" >&2; exit 1; }

echo "[build] checking AWS credentials"
if ! aws sts get-caller-identity --region "${REGION}" >/dev/null; then
    echo "ERROR: AWS credentials not configured for region ${REGION}." >&2
    exit 1
fi

GIT_SHA="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
PACKER_VARS=(-var "arch=${ARCH}" -var "region=${REGION}" -var "binary_path=${BINARY_PATH}" -var "git_sha=${GIT_SHA}")

packer init "${TEMPLATE_DIR}/ca_cluster_node.pkr.hcl"
packer validate "${PACKER_VARS[@]}" "${TEMPLATE_DIR}/ca_cluster_node.pkr.hcl"
packer build "${PACKER_VARS[@]}" "${TEMPLATE_DIR}/ca_cluster_node.pkr.hcl"

AMI_ID="$(python3 -c "
import json
with open('packer-manifest.json') as f:
    data = json.load(f)
print(data['builds'][-1]['artifact_id'].split(':')[-1])
")"
echo "${AMI_ID}"
```

## CI Integration

### `scripts/ci-cloud-credentials/aws/policies/ami-build.json`

```json
[
  {
    "Sid": "AmiBuildEc2Lifecycle",
    "Effect": "Allow",
    "Action": [
      "ec2:RunInstances",
      "ec2:TerminateInstances",
      "ec2:DescribeInstances",
      "ec2:DescribeInstanceStatus",
      "ec2:CreateKeyPair",
      "ec2:DeleteKeyPair",
      "ec2:CreateSecurityGroup",
      "ec2:DeleteSecurityGroup",
      "ec2:AuthorizeSecurityGroupIngress",
      "ec2:CreateImage",
      "ec2:DeregisterImage",
      "ec2:DescribeImages",
      "ec2:CreateSnapshot",
      "ec2:DeleteSnapshot",
      "ec2:DescribeSnapshots",
      "ec2:CreateTags",
      "ec2:GetPasswordData"
    ],
    "Resource": "*"
  },
  {
    "Sid": "AmiBuildSourceAmiLookup",
    "Effect": "Allow",
    "Action": "ssm:GetParameter",
    "Resource": "arn:aws:ssm:*::parameter/aws/service/canonical/ubuntu/*"
  }
]
```

This follows exactly the shape of the existing
`policies/ca-cluster-node.json` / `policies/ec2-quorum-manager.json` files —
`provision-oidc-role.sh` needs no code change (it already reads
`policies/{bundle}.json` generically per bundle name).

### `.github/workflows/real-cloud-tests.yml` — new `ami-build` job

```yaml
  ami-build:
    name: Build ca_cluster_node AMI (${{ matrix.arch }})
    strategy:
      matrix:
        include:
          - arch: amd64
            runs-on: ubuntu-24.04
          - arch: arm64
            runs-on: ubuntu-24.04-arm
    runs-on: ${{ matrix.runs-on }}
    timeout-minutes: 60
    environment: real-cloud-tests
    permissions:
      id-token: write
      contents: read
    if: |
      (github.event.inputs.run_real_cloud_tests == 'true'
        || (github.event.inputs.run_real_cloud_tests == null && vars.REAL_CLOUD_TESTS_ENABLED == 'true'))
      && (github.event.inputs.aws_enabled == 'true'
        || (github.event.inputs.aws_enabled == null && vars.REAL_CLOUD_TESTS_AWS_ENABLED == 'true'))
      && (github.event.inputs.aws_bundle_ami_build == 'true'
        || (github.event.inputs.aws_bundle_ami_build == null && vars.REAL_CLOUD_TESTS_AWS_AMI_BUILD_ENABLED == 'true'))
    steps:
      - uses: actions/checkout@v4

      - name: Configure AWS credentials (OIDC)
        uses: aws-actions/configure-aws-credentials@v4
        with:
          role-to-assume: ${{ vars.AWS_CI_ROLE_ARN }}
          role-session-name: kythira-ci-ami-build
          aws-region: ${{ vars.AWS_REAL_CLOUD_TESTS_REGION || 'us-east-1' }}

      - name: Install Packer
        run: |
          curl -fsSL https://apt.releases.hashicorp.com/gpg | sudo apt-key add -
          sudo apt-add-repository "deb [arch=$(dpkg --print-architecture)] https://apt.releases.hashicorp.com $(lsb_release -cs) main"
          sudo apt-get update && sudo apt-get install -y packer

      - name: Build AMI
        run: packer/ca_cluster_node/scripts/build.sh --arch ${{ matrix.arch }}

      - uses: actions/upload-artifact@v4
        with:
          name: ca-cluster-node-ami-manifest-${{ matrix.arch }}
          path: packer-manifest.json
```

added alongside the existing `run_real_cloud_tests` / `aws_enabled` /
`aws_bundle_*` `workflow_dispatch` inputs, plus a new
`aws_bundle_ami_build` input mirroring the existing three
`aws_bundle_ca_cluster*` inputs.

## Data Models

### AMI tag schema (Requirement 6)

```
Name                = kythira-ca-cluster-node-{short_sha}-{arch}-{timestamp}
kythira:component   = ca-cluster-node
kythira:git-sha      = {full 40-char commit SHA}
kythira:arch         = amd64 | arm64
kythira:base-ami     = {resolved Ubuntu 24.04 source AMI ID}
kythira:built-by     = packer
```

### `packer-manifest.json` (Packer's standard manifest post-processor format)

```json
{
  "builds": [
    {
      "name": "ca_cluster_node",
      "builder_type": "amazon-ebs",
      "build_time": 1735689600,
      "artifact_id": "us-east-1:ami-0123456789abcdef0",
      "custom_data": { "arch": "amd64", "git_sha": "a1b2c3d4..." }
    }
  ]
}
```

`build.sh` parses `builds[-1].artifact_id` (format `{region}:{ami_id}`) to
print the bare AMI ID.

## Correctness Properties

### Property 1: Single source of truth for the compiled binary
**Validates: Requirement 2**

`extract-binary.sh` never independently recompiles `ca_cluster_node` — it
builds `docker/ca_cluster_node/Dockerfile`'s `builder` stage exactly as the
production Docker image build does, then copies the resulting file out
without executing it. A change to the compile flags, dependencies, or C++
standard in the Dockerfile is automatically picked up by the next AMI build
with no template change required.

### Property 2: No secret ever leaves the operator's own machine/CI runner
**Validates: Requirement 5**

The Packer build's `file` and `shell` provisioners reference exactly three
inputs: the compiled binary, the unmodified `ca_cluster_node.service` unit
file, and `provision.sh` itself. None of these three inputs contain or
reference an unseal passphrase, auth token, or RPC TLS credential — grepping
the entire `packer/ca_cluster_node/` tree for those terms (a straightforward
CI lint, Requirement 5 AC 2) finds nothing. An attacker who obtains the AMI
ID and permission to launch from it gets a stock, unconfigured node that
cannot join any cluster or serve any request until an operator separately
supplies `/etc/default/ca_cluster_node` and the files under
`/etc/ca_cluster_node/`.

### Property 3: Base-OS/binary ABI compatibility
**Validates: Requirement 3**

Because the AMI's source image (Ubuntu 24.04) is identical to the OS the
binary is compiled and dynamically linked for
(`docker/ca_cluster_node/Dockerfile`'s stage 2, also Ubuntu 24.04), every
shared library the binary's dynamic linker resolves at instance boot
(`libssl.so.3`, `libc.so.6`, etc.) is guaranteed present at a compatible
version — the same guarantee the existing Docker image already relies on
for its own two-stage build.

### Property 4: Idempotent, re-runnable provisioning
**Validates: Requirement 4 AC 2**

`provision.sh`'s `useradd` is guarded by an `id -u` check and its directory
creation uses `install -d` (which is a no-op if the directory already
exists with different ownership only in the sense that `install` reapplies
the requested mode/owner rather than erroring) — running the script twice
against the same instance leaves the system in the same end state as
running it once, which matters for local `docker run` smoke-testing
(Requirement 10 AC 3) where a developer may iterate on the script inside one
long-lived container.

## Error Handling

- **`extract-binary.sh` produces an empty/missing file**: hard failure
  before Packer ever runs (Requirement 2 AC 3) — a Packer build with a
  0-byte binary would otherwise fail much later and more confusingly, deep
  inside the `shell` provisioner's `install` step.
- **AWS credentials missing**: `build.sh` fails fast via `aws sts
  get-caller-identity` (Requirement 8 AC 4) before invoking `packer build`,
  giving a clear error instead of Packer's own less specific
  `NoCredentialProviders` failure.
- **`packer validate` failure**: `build.sh` stops before `packer build`
  (Requirement 8 AC 3) — no EC2 instance is ever launched for a template
  that fails static validation.
- **Provisioning failure mid-build**: Packer's default behavior on a
  provisioner error is to leave the temporary build instance running for
  inspection (`-on-error=abort` is the default, but `-on-error=ask` can be
  passed manually for debugging). This is Packer's existing, documented
  behavior and is not overridden by this template; `packer/ca_cluster_node/README.md`
  documents that a failed build's leftover instance/security-group/key-pair
  must be cleaned up manually (searchable via the `run_tags` from
  Requirement 6.3).
- **Cross-arch Docker Buildx unavailable**: `extract-binary.sh`'s
  `--platform` build fails with Docker's own "multiple platforms feature is
  currently not supported" or similar error; the script does not catch or
  reinterpret this — Requirement 9.3 already documents the fix (use a
  native-architecture host).

## Testing Strategy

Packer templates cannot be meaningfully unit-tested without either a real
AWS account or a full LocalStack EC2 image-build emulation (which
LocalStack's Community edition does not provide for `CreateImage`). Testing
is therefore split into a fast, free static tier and a real-build tier
gated the same way as every other real-cloud test in this repository.

### Static tier (runs in ordinary CI, no AWS credentials)

- `packer fmt -check -diff` (Requirement 10 AC 1) on every push/PR touching
  `packer/ca_cluster_node/**`.
- `packer validate` (Requirement 10 AC 2) — catches variable type errors,
  missing required variables, and HCL syntax errors without contacting AWS.
- A shell-script lint (`shellcheck packer/ca_cluster_node/scripts/*.sh`) —
  matches the level of rigor already implied by this project's use of
  `set -euo pipefail` throughout its existing `scripts/` tree.
- A grep-based secret-absence check (Property 2) run as a CI step:
  `! grep -riE 'unseal|auth.?token|bootstrap.*key' packer/ca_cluster_node/ca_cluster_node.pkr.hcl packer/ca_cluster_node/scripts/provision.sh`.
- `provision.sh`'s filesystem-level effects (Requirement 10 AC 3) can be
  smoke-tested in a plain `ubuntu:24.04` Docker container: `docker run --rm
  -v $PWD:/src ubuntu:24.04 bash -c "apt-get update && apt-get install -y
  sudo && cp /src/.../ca_cluster_node.service /tmp/ && touch
  /tmp/ca_cluster_node && /src/packer/ca_cluster_node/scripts/provision.sh"`
  followed by assertions on `/usr/local/bin/ca_cluster_node`,
  `/etc/ca_cluster_node` (exists, empty), `id ca-cluster-node`, and
  `systemctl is-enabled ca_cluster_node` (best-effort — no PID 1 systemd is
  running inside a plain container, so this specific assertion is skipped
  there and left to the real-build tier).

### Real-build tier (gated, `ami-build` CI job, Requirement 13)

- `packer/ca_cluster_node/scripts/build.sh --arch amd64` and `--arch arm64`
  each produce a real AMI in the configured test account/region, verified
  by the job succeeding and `packer-manifest.json` containing a
  well-formed `ami-...` artifact ID.
- The already-existing `ca_cluster_node_real_ec2_test.cpp` (unchanged by
  this spec) provides the actual end-to-end proof that an AMI built this
  way boots and forms a working cluster — an operator points
  `KYTHIRA_EC2_TEST_AMI` at the manifest's AMI ID and runs that suite
  manually (or, per Requirement 13 AC 4, this could be wired to run
  automatically in a follow-up).

## Dependencies

```
packer >= 1.10          HashiCorp CLI; installed via apt.releases.hashicorp.com in CI
amazon plugin >= 1.3.0  Packer plugin providing amazon-ebs builder +
                        amazon-parameterstore data source (`packer init`
                        installs it automatically from the required_plugins block)
docker or podman         For extract-binary.sh; honors $KYTHIRA_CONTAINER_RUNTIME
aws-cli v2                For build.sh's credential preflight check
```

No new C++ dependency, CMake target, or change to `vcpkg.json` is
introduced by this spec.
