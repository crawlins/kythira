# Implementation Plan — `ca_cluster_node` AMI Build

## Status: Not Started

**Last Updated**: July 15, 2026

## Overview

Add a Packer template and orchestration scripts that produce a golden,
secret-free AMI with `ca_cluster_node` and its systemd unit pre-installed,
resolving the "(baked into an AMI, e.g. via Packer)" placeholder in
`docker/ca_cluster_node/README.md` and giving
`tests/ca_cluster_node_real_ec2_test.cpp`'s `KYTHIRA_EC2_TEST_AMI` a real
producer.

Reference material to read before starting:
- `docker/ca_cluster_node/Dockerfile` — the `builder` stage this spec
  extracts a binary from; do not change it.
- `docker/ca_cluster_node/ca_cluster_node.service` — the unit file this spec
  installs verbatim; do not change it.
- `cmd/ca_cluster_node/config.hpp` — confirms `--data-dir` default
  (`/var/lib/ca_cluster_node`) and the `User=`/`Group=ca-cluster-node`
  identity the unit file expects.
- `scripts/ci-cloud-credentials/aws/provision-oidc-role.sh` and
  `policies/ca-cluster-node.json` — the bundle pattern this spec's new
  `ami-build` bundle follows.
- `.github/workflows/real-cloud-tests.yml` — the toggle pattern (master
  switch / per-provider / per-bundle) the new `ami-build` job follows.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 2],
      "description": "Binary extraction script and Packer template skeleton — no interdependency"
    },
    {
      "wave": 2,
      "tasks": [3],
      "description": "Provisioning script — consumed by the template from wave 1"
    },
    {
      "wave": 3,
      "tasks": [4],
      "description": "Build orchestration script — wraps tasks 1-3"
    },
    {
      "wave": 4,
      "tasks": [5, 6],
      "description": "Static CI checks and documentation — depend on the template/scripts existing"
    },
    {
      "wave": 5,
      "tasks": [7, 8],
      "description": "CI credential bundle and real-build workflow job — depend on the build script"
    }
  ]
}
```

## Tasks

- [ ] 1. `extract-binary.sh`
  - Create `packer/ca_cluster_node/scripts/extract-binary.sh` per
    design.md's Component 4:
    - `--arch amd64|arm64` (required), `--out PATH` (optional, defaults to
      `build/packer-artifacts/ca_cluster_node-{arch}`)
    - `${KYTHIRA_CONTAINER_RUNTIME:-docker} build --platform linux/${arch}
      -f docker/ca_cluster_node/Dockerfile --target builder -t
      kythira-ca-cluster-node-builder:${arch} <repo_root>`
    - `create` + `cp /src/build/ca_cluster_node` + `rm` (never `run`)
    - Fail non-zero with a clear message if the output file is missing or
      empty (`[[ ! -s "${OUT}" ]]`)
  - `chmod +x` the script; verify: run it once with `--arch amd64` on an
    x86_64 dev machine/CI runner and confirm a non-empty ELF file appears
    at the default output path (`file build/packer-artifacts/ca_cluster_node-amd64`
    should report `ELF 64-bit ... x86-64`).
  - _Requirements: 2.2, 2.3, 9.3_

- [ ] 2. Packer template skeleton
  - Create `packer/ca_cluster_node/variables.pkr.hcl` and
    `packer/ca_cluster_node/ca_cluster_node.pkr.hcl` per design.md's
    Components 1-2:
    - `required_plugins` pinning `amazon >= 1.3.0`
    - `variable "arch"` with a `validation` block restricting to
      `["amd64", "arm64"]`
    - `data "amazon-parameterstore" "ubuntu"` resolving Canonical's SSM
      alias for Ubuntu 24.04, parameterized by `var.arch`
    - `local.instance_type` = `t3.micro` (amd64) / `t4g.micro` (arm64),
      overridable via `var.builder_instance_type`
    - `local.ami_name` = `kythira-ca-cluster-node-{short_sha}-{arch}-{timestamp}`
    - `source "amazon-ebs" "ca_cluster_node"` with `tags`/`run_tags`/
      `snapshot_tags` per the tag schema in design.md's Data Models section
    - `build` block: three `file` provisioners (binary, `ca_cluster_node.service`,
      `provision.sh`) + one `shell` provisioner invoking `provision.sh`
    - `post-processor "manifest"` writing `packer-manifest.json` with
      `custom_data = { arch = var.arch, git_sha = var.git_sha }`
  - Verify: `packer fmt -check -diff packer/ca_cluster_node/*.pkr.hcl` and
    `packer init packer/ca_cluster_node/ca_cluster_node.pkr.hcl && packer
    validate -var arch=amd64 -var binary_path=/dev/null -var
    git_sha=0000000000000000000000000000000000000000
    packer/ca_cluster_node/ca_cluster_node.pkr.hcl` both succeed with no AWS
    credentials present.
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 3.1, 3.2, 3.3, 6.1, 6.2, 6.3, 7.1, 7.2, 9.1, 9.2_

- [ ] 3. `provision.sh`
  - Create `packer/ca_cluster_node/scripts/provision.sh` per design.md's
    Component 3 — runtime packages (`libssl3 curl`), binary install,
    `ca-cluster-node` system user creation (idempotent — guarded by `id -u`
    check), `/var/lib/ca_cluster_node` and `/etc/ca_cluster_node` directory
    creation (mode `0750`, correct ownership, `/etc/ca_cluster_node` left
    empty), systemd unit install + `daemon-reload` + `enable` (NOT `start`),
    AMI hygiene cleanup (`/etc/machine-id`, SSH host keys, cloud-init
    clean, bash history).
  - `chmod +x` the script.
  - Verify: run the manual `docker run` smoke test from design.md's Testing
    Strategy (static tier) against a plain `ubuntu:24.04` container;
    confirm `/usr/local/bin/ca_cluster_node` is mode `0755`,
    `/etc/ca_cluster_node` exists and is empty, `/etc/default/ca_cluster_node`
    does NOT exist, and `id ca-cluster-node` succeeds.
  - _Requirements: 4.1, 4.2, 5.1, 5.2_

- [ ] 4. `build.sh`
  - Create `packer/ca_cluster_node/scripts/build.sh` per design.md's
    Component 5:
    - `--arch` (required), `--region` (default `us-east-1` or `$AWS_REGION`),
      `--skip-binary-build`
    - Calls `extract-binary.sh` unless `--skip-binary-build`
    - `aws sts get-caller-identity` preflight → clear error, not a raw
      Packer stack trace, on failure
    - `packer init` → `packer validate` → `packer build`, stopping before
      `build` if `validate` fails
    - Parses `packer-manifest.json`'s last build's `artifact_id`
      (`{region}:{ami_id}`) and prints the bare AMI ID as the script's final
      stdout line
  - `chmod +x` the script.
  - Verify (requires a scratch AWS account/region): `build.sh --arch amd64`
    completes, prints a valid `ami-...` ID as its last line, and the AMI is
    visible via `aws ec2 describe-images --image-ids <id>` with the tags
    from Requirement 6.2 present.
  - _Requirements: 7.3, 8.1, 8.2, 8.3, 8.4_

- [ ] 5. Static CI checks
  - Add a job (or steps within an existing job) to `ci.yml` that runs, on
    every push/PR touching `packer/ca_cluster_node/**`:
    - `packer fmt -check -diff`
    - `packer init` + `packer validate` (no AWS credentials required —
      dummy `-var` values for `binary_path`/`git_sha` are fine since
      `validate` only type-checks)
    - `shellcheck packer/ca_cluster_node/scripts/*.sh`
    - The grep-based secret-absence check from design.md's Property 2:
      `! grep -riE 'unseal|auth.?token|bootstrap.*key'
      packer/ca_cluster_node/ca_cluster_node.pkr.hcl
      packer/ca_cluster_node/scripts/provision.sh`
  - Verify: the new CI job passes on this branch and fails if you
    temporarily introduce a syntax error or a forbidden string to confirm
    it actually catches problems (revert the temporary break afterward).
  - _Requirements: 10.1, 10.2_

- [ ] 6. Documentation
  - Create `packer/ca_cluster_node/README.md` per Requirement 11 AC 1:
    prerequisites, `build.sh` usage, tag/naming scheme, manifest location,
    explicit no-secrets guarantee, and the manual `docker run` smoke-test
    recipe from design.md's Testing Strategy (static tier), plus manual
    `aws ec2 deregister-image` / `aws ec2 delete-snapshot` cleanup commands
    filtered by `kythira:component=ca-cluster-node` (Requirement 13 AC 5).
  - Update `docker/ca_cluster_node/README.md`'s "Path 3" section: replace
    the `// AMI running ca_cluster_node` / "(baked into an AMI, e.g. via
    Packer)" placeholder with a pointer to `packer/ca_cluster_node/README.md`
    and the `build.sh --arch amd64 --region us-east-1` example.
  - Update `docker/ca_cluster_node/ecs-task-definitions/README.md`'s
    "Automated alternative" section with the same pointer.
  - Update `tests/ca_cluster_node_real_ec2_test.cpp`'s header comment for
    `KYTHIRA_EC2_TEST_AMI` to reference `packer/ca_cluster_node/README.md`
    (comment-only change; the env var's contract is unchanged).
  - Verify: grep the four updated files for the old placeholder text to
    confirm it's gone; `ctest` still passes (comment-only source change).
  - _Requirements: 11.1, 11.2, 11.3, 11.4_

- [ ] 7. CI credential bundle
  - Create `scripts/ci-cloud-credentials/aws/policies/ami-build.json` per
    design.md's CI Integration section (EC2 lifecycle + `CreateImage`/
    `DeregisterImage`/snapshot actions + scoped `ssm:GetParameter` on
    Canonical's public parameter path).
  - Update `scripts/ci-cloud-credentials/aws/provision-oidc-role.sh`'s usage
    text (the `--bundles` help string) to list `ami-build` alongside the
    three existing bundle names — no logic change needed since the script
    already loads `policies/{bundle}.json` generically.
  - Update `scripts/ci-cloud-credentials/README.md`'s bundle table with the
    new `ami-build` row.
  - Verify: `provision-oidc-role.sh --dry-run --github-org x --github-repo y
    --bundles ami-build` succeeds and prints the dry-run policy document
    containing every action from `ami-build.json`.
  - _Requirements: 12.1, 12.2, 12.3_

- [ ] 8. `ami-build` CI workflow job
  - Add the `ami-build` job to `.github/workflows/real-cloud-tests.yml` per
    design.md's CI Integration section: matrix over `arch: [amd64, arm64]`
    with native runners (`ubuntu-24.04`, `ubuntu-24.04-arm`), the same
    three-level toggle pattern as the existing `aws` job (new
    `aws_bundle_ami_build` `workflow_dispatch` input mapped to
    `REAL_CLOUD_TESTS_AWS_AMI_BUILD_ENABLED`), OIDC credential
    configuration, Packer install step, `build.sh` invocation, and
    `actions/upload-artifact@v4` for `packer-manifest.json`.
  - Do NOT add a `needs:` dependency from the existing `aws` job's
    `ca-cluster-node`/`ca-cluster-node-rpc-tls` steps onto `ami-build`
    (Requirement 13 AC 4 — explicitly out of scope).
  - Verify: `workflow_dispatch` with `aws_bundle_ami_build: true` (and the
    other two toggle levels enabled/overridden) produces a successful run
    with a downloadable `ca-cluster-node-ami-manifest-{arch}` artifact for
    both matrix legs, in a scratch AWS account.
  - _Requirements: 13.1, 13.2, 13.3, 13.4_

## Notes

- This spec deliberately does not touch `include/raft/aws_ec2_quorum_manager.hpp`,
  `aws_ec2_quorum_manager_config`, or any C++ code — `cfg.image_id` already
  accepts any AMI ID string; this spec only produces one.
- Task 1's `extract-binary.sh` uses `${KYTHIRA_CONTAINER_RUNTIME:-docker}`
  per `CLAUDE.md`'s container-runtime-compatibility steering, even though
  this script is a packaging script rather than test harness code — kept
  consistent with every other `docker`-invoking script in the repository so
  a Podman-only environment can still build the AMI.
- `arm64` builds in CI (task 8) require a GitHub-hosted `ubuntu-24.04-arm`
  runner (or an equivalent self-hosted arm64 runner) to be available to the
  repository; if it is not, fall back to `--platform linux/arm64` QEMU
  cross-emulation in `extract-binary.sh` for that leg only, accepting the
  slower build time (Requirement 9.3).
- Wiring a freshly built AMI's ID automatically into
  `KYTHIRA_EC2_TEST_AMI` for the existing `ca-cluster-node` /
  `ca-cluster-node-rpc-tls` real-EC2 test bundles, and any AMI
  retention/cleanup automation, are explicitly out of scope (Requirement
  13 AC 4-5) — candidate follow-up specs, not part of this one.
