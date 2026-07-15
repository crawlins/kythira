# `ca_cluster_node` AMI build (Packer)

Produces a golden, secret-free AMI with `/usr/local/bin/ca_cluster_node` and
its systemd unit pre-installed, for use with
[Path 3 — automated EC2 replacement via `aws_ec2_quorum_manager`](../../docker/ca_cluster_node/README.md#path-3--automated-ec2-replacement-via-aws_ec2_quorum_manager)
and `tests/ca_cluster_node_real_ec2_test.cpp`'s `KYTHIRA_EC2_TEST_AMI`. Full
design at `.kiro/specs/ca-cluster-node-ami/`.

## Prerequisites

- [Packer](https://developer.hashicorp.com/packer) ≥ 1.10
- Docker or Podman (honors `$KYTHIRA_CONTAINER_RUNTIME`, default `docker`)
- AWS credentials (any standard provider-chain source) with permission to
  launch/terminate an EC2 instance and create/tag an AMI + snapshot — see
  `scripts/ci-cloud-credentials/aws/policies/ami-build.json` for the exact
  action list
- `aws-cli` v2 and `python3` (used by `scripts/build.sh`)

## Building an AMI

```
packer/ca_cluster_node/scripts/build.sh --arch amd64 --region us-east-1
```

This: (1) builds `docker/ca_cluster_node/Dockerfile`'s `builder` stage and
extracts the compiled `ca_cluster_node` binary — no source is recompiled by
Packer itself; (2) runs `packer init`/`validate`/`build` against
`ca_cluster_node.pkr.hcl`; (3) prints the resulting AMI ID as the last line
of stdout.

Pass `--arch arm64` for a Graviton-compatible AMI. Prefer running each
architecture natively (an `arm64` host/runner for `--arch arm64`) — cross-arch
Docker Buildx/QEMU emulation works but is much slower to compile.

Already have an extracted binary and just want to iterate on the Packer
template itself? `--skip-binary-build` reuses whatever is at
`build/packer-artifacts/ca_cluster_node-{arch}` instead of rebuilding it.

## What's in the AMI

- Ubuntu 24.04 base (matching `docker/ca_cluster_node/Dockerfile`'s runtime
  stage exactly, so the compiled binary's glibc/OpenSSL ABI always matches
  what it runs on — Amazon Linux 2023's older glibc is deliberately not
  used).
- `/usr/local/bin/ca_cluster_node` (mode `0755`).
- `ca-cluster-node` system user/group; `/var/lib/ca_cluster_node` (the
  default `--data-dir`) and `/etc/ca_cluster_node` (empty), both mode `0750`
  and owned by that user.
- `/etc/systemd/system/ca_cluster_node.service`, installed verbatim from
  `docker/ca_cluster_node/ca_cluster_node.service`, `enable`d but **not**
  started (its `EnvironmentFile`, `/etc/default/ca_cluster_node`, doesn't
  exist yet).

**No secrets are ever baked in.** The unseal passphrase, `CA_SERVICE_AUTH_TOKEN`,
RPC-TLS bootstrap credential, `NODE_ID`, and `--peers` all remain supplied
per-instance at launch time, exactly as `ca_cluster_node.service`'s own
header comment and `ca_cluster_node.env.example` already document. This AMI
only removes the "install the software" step from that flow — every launched
instance still needs its own configuration before the unit can start.

## Naming and tags

```
Name                = kythira-ca-cluster-node-{short_sha}-{arch}-{timestamp}
kythira:component   = ca-cluster-node
kythira:git-sha      = {full 40-char commit SHA}
kythira:arch         = amd64 | arm64
kythira:base-ami     = {resolved Ubuntu 24.04 source AMI ID}
kythira:built-by     = packer
```

## Manifest

Every build writes `packer-manifest.json` (Packer's standard manifest
post-processor format) to the current working directory, `custom_data`
carrying `arch` and `git_sha`. `scripts/build.sh` parses this to print the
bare AMI ID; CI uploads it as a workflow artifact.

## Local smoke test (no Packer, no AWS)

`scripts/provision.sh`'s filesystem-level effects can be checked directly
against a plain Ubuntu 24.04 container, without running Packer or touching
AWS:

```
docker run --rm -it -v "$PWD":/src ubuntu:24.04 bash -c '
  apt-get update -q && apt-get install -y --no-install-recommends sudo
  cp /src/docker/ca_cluster_node/ca_cluster_node.service /tmp/
  touch /tmp/ca_cluster_node   # stand-in for the real binary
  /src/packer/ca_cluster_node/scripts/provision.sh
  ls -l /usr/local/bin/ca_cluster_node
  ls -la /etc/ca_cluster_node
  id ca-cluster-node
  test ! -e /etc/default/ca_cluster_node && echo "no EnvironmentFile baked in, as expected"
'
```

`systemctl is-enabled ca_cluster_node` isn't exercised by this recipe — a
plain container has no PID 1 systemd — that part is only proven by an actual
launched EC2 instance.

## Static validation (no AWS credentials, runs in ordinary CI)

```
packer fmt -check -diff packer/ca_cluster_node/*.pkr.hcl
packer init packer/ca_cluster_node/ca_cluster_node.pkr.hcl
packer validate -syntax-only packer/ca_cluster_node/ca_cluster_node.pkr.hcl
shellcheck packer/ca_cluster_node/scripts/*.sh
```

(`-syntax-only` is required here specifically because a full `packer validate`
would otherwise try to resolve the template's `amazon-parameterstore` data
source via a real AWS SSM API call.)

## Cleanup

Packer's own temporary build instance/security-group/key-pair are cleaned up
automatically on both success and failure. The AMI and its snapshot are
**not** — deleting them is a deliberate operator action:

```
aws ec2 deregister-image --image-id ami-XXXXXXXXXXXXXXXXX
aws ec2 delete-snapshot --snapshot-id snap-XXXXXXXXXXXXXXXXX
```

Find old builds by the `kythira:component=ca-cluster-node` tag:

```
aws ec2 describe-images --owners self \
  --filters Name=tag:kythira:component,Values=ca-cluster-node \
  --query 'Images[].[ImageId,Name,CreationDate]' --output table
```

## Out of scope

- Wiring a freshly built AMI's ID automatically into `KYTHIRA_EC2_TEST_AMI`
  for the real-EC2 test bundles — set it manually from the manifest/CI
  artifact today.
- AMI/snapshot retention or lifecycle cleanup automation.

Both are candidate follow-up work, not part of this pipeline.
