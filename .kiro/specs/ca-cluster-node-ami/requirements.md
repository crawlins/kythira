# Requirements Document

## Introduction

This document specifies the requirements for a reproducible build pipeline
that produces a custom, pre-baked Amazon Machine Image (AMI) with the
`ca_cluster_node` binary and its systemd unit already installed.

Today this AMI is a placeholder. Three places in the codebase already assume
it exists:

- `docker/ca_cluster_node/README.md` ("Path 3 — automated EC2 replacement via
  `aws_ec2_quorum_manager`") tells operators to set `cfg.image_id = "ami-...";
  // AMI running ca_cluster_node` and says only "baked into an AMI, e.g. via
  Packer" — with no template, script, or procedure behind that sentence.
- `docker/ca_cluster_node/ecs-task-definitions/README.md`'s "Automated
  alternative" section repeats the same unresolved pointer.
- `tests/ca_cluster_node_real_ec2_test.cpp` requires a
  `KYTHIRA_EC2_TEST_AMI` environment variable — "AMI ID with
  `/usr/local/bin/ca_cluster_node` installed ... bake it into an AMI via
  Packer or a launch script" — and today an operator must build that AMI by
  hand before the real-EC2 test suite can run at all.

This spec closes that gap with a Packer template, a thin build-orchestration
script, and CI wiring so that a `ca_cluster_node` AMI can be produced
on-demand, from a known git commit, without hand-editing an EC2 instance.

This spec covers **building and publishing the AMI only**. It does not
change `aws_ec2_quorum_manager` (`.kiro/specs/aws-quorum-manager/`, already
implemented and unaffected), the systemd unit or environment file (already
implemented in `docker/ca_cluster_node/`), or `ca_cluster_node` itself.

## Glossary

- **AMI**: Amazon Machine Image — an EC2-launchable disk image snapshot.
- **Packer**: HashiCorp's machine-image build tool. Templates are HCL2
  (`.pkr.hcl`) files describing a source image, provisioning steps, and
  post-processors.
- **`amazon-ebs` builder**: The Packer builder plugin that launches a
  temporary EC2 instance from a source AMI, runs provisioners against it,
  snapshots the resulting root volume, and registers a new AMI from that
  snapshot.
- **Builder-stage binary**: The `ca_cluster_node` ELF binary produced by
  stage 1 (`FROM ubuntu:24.04 AS builder`) of the existing
  `docker/ca_cluster_node/Dockerfile`. This spec reuses that stage as the
  single source of truth for how the binary is compiled; it does not
  duplicate the compile steps.
- **Golden AMI**: An AMI with application software pre-installed but no
  per-instance secrets or configuration baked in — those are supplied at
  launch time (env file, `--peers`, unseal key, etc.), exactly as
  `docker/ca_cluster_node/ca_cluster_node.service`'s existing manual-install
  steps already do for a hand-provisioned instance.
- **Bundle** (CI sense): the existing `scripts/ci-cloud-credentials/`
  convention of one named IAM permission set mapped to one CI job/step, per
  `.kiro/specs/ci-real-cloud-tests/`.

---

## Requirements

### Requirement 1: Packer template location and structure

**User Story:** As a developer, I want the AMI build defined as a versioned
Packer template in the repository so that the AMI's contents are
reproducible from source and reviewable like any other code change.

#### Acceptance Criteria

1. A Packer HCL2 template SHALL be added at
   `packer/ca_cluster_node/ca_cluster_node.pkr.hcl`, with variables factored
   into `packer/ca_cluster_node/variables.pkr.hcl`.
2. The template SHALL declare a `required_plugins` block pinning the
   `amazon` Packer plugin to a minimum version (`>= 1.3.0`, the first
   version with `amazon-parameterstore` data source support).
3. The template SHALL define exactly one `source "amazon-ebs" "ca_cluster_node"`
   block, parameterized by `var.arch` (`"amd64"` or `"arm64"`) so the same
   template builds either architecture — no per-architecture template
   duplication.
4. `packer validate packer/ca_cluster_node/ca_cluster_node.pkr.hcl` SHALL
   succeed with no AWS credentials present (Packer's `validate` subcommand
   performs only local syntax/type checking; it must not require network
   access to AWS for the variables' default values to type-check).

---

### Requirement 2: Binary provenance — reuse the existing Dockerfile builder stage

**User Story:** As a maintainer, I want the AMI's `ca_cluster_node` binary
built by the exact same steps already validated for the Docker image, so
that the AMI and the container image never drift apart and there is only
one place that knows how to compile `ca_cluster_node`.

#### Acceptance Criteria

1. The Packer template SHALL NOT contain any `apt-get install
   g++/cmake/ninja-build/...` or `cmake --build` provisioning step. Compiling
   `ca_cluster_node` from source SHALL happen exactly once, in
   `docker/ca_cluster_node/Dockerfile`'s existing `builder` stage.
2. A new script `packer/ca_cluster_node/scripts/extract-binary.sh` SHALL:
   a. Build the Docker image through the `builder` stage only:
      `${KYTHIRA_CONTAINER_RUNTIME:-docker} build -f
      docker/ca_cluster_node/Dockerfile --target builder -t
      kythira-ca-cluster-node-builder:${ARCH} .`
   b. Extract the compiled binary from that stage without ever running the
      container (`create` + `cp` + `rm`, not `run`):
      `${KYTHIRA_CONTAINER_RUNTIME:-docker} create --name
      kythira-ca-cluster-node-extract-${ARCH}
      kythira-ca-cluster-node-builder:${ARCH}`, then `cp
      kythira-ca-cluster-node-extract-${ARCH}:/src/build/ca_cluster_node
      <output_path>`, then `rm` the extraction container.
   c. Read the container runtime from `$KYTHIRA_CONTAINER_RUNTIME`
      (default `docker`), per this project's `CLAUDE.md` container-runtime
      rule — even though this script is a packaging script rather than a
      test harness, honoring the same override keeps every
      `docker`-invoking script in the repository consistent and lets a
      Podman-only CI runner build the AMI too.
   d. Accept `--arch amd64|arm64` and pass the corresponding
      `--platform linux/${arch}` to the `build` invocation, so an operator
      can cross-build the `arm64` binary from an `amd64` host via QEMU
      emulation (slow) or, preferably, run the script natively on a
      matching-architecture host/runner (fast — see Requirement 9).
3. `extract-binary.sh` SHALL exit non-zero and print a clear error if the
   extracted file does not exist or is empty (0 bytes) after `cp`, rather
   than silently producing a Packer build with a missing binary.

---

### Requirement 3: Base OS parity with the Docker runtime image

**User Story:** As an operator launching instances from this AMI, I want the
binary to run without a dynamic-linker or ABI failure, so I don't discover a
`glibc` version mismatch only after paging myself at 2am.

#### Acceptance Criteria

1. The AMI's base (source) image SHALL be **Ubuntu 24.04**, matching
   `docker/ca_cluster_node/Dockerfile`'s stage 2 (`FROM ubuntu:24.04`)
   exactly. Amazon Linux 2023 (glibc 2.34) SHALL NOT be used as the source
   AMI, because the binary is compiled against Ubuntu 24.04's glibc 2.39 and
   is not guaranteed backward-compatible with an older glibc.
2. The source AMI SHALL be resolved via the `amazon-parameterstore` Packer
   data source against Canonical's published SSM parameter:
   `/aws/service/canonical/ubuntu/server/24.04/stable/current/{arch}/hvm/ebs-gp3/ami-id`,
   where `{arch}` is `amd64` or `arm64` — never a hand-copied AMI ID literal,
   so the template always resolves to Canonical's current 24.04 image at
   build time.
3. The resolved source AMI ID SHALL be recorded as a `kythira:base-ami` tag
   on the produced AMI (Requirement 6) so a later audit can determine
   exactly which upstream image a given build started from.

---

### Requirement 4: Provisioning steps

**User Story:** As an operator, I want the AMI to contain the binary, its
runtime dependencies, and the systemd unit — but no secrets — so that
launching an instance from it is safe to do at any time without leaking
credentials baked at build time.

#### Acceptance Criteria

1. A shell provisioner script `packer/ca_cluster_node/scripts/provision.sh`
   SHALL run on the Packer builder instance and:
   a. Install exactly the runtime packages `docker/ca_cluster_node/Dockerfile`'s
      stage 2 installs: `libssl3` and `curl` (via `apt-get update &&
      DEBIAN_FRONTEND=noninteractive apt-get install -y libssl3 curl`).
   b. Install the binary uploaded by the `file` provisioner (Requirement 2)
      to `/usr/local/bin/ca_cluster_node`, mode `0755`, owner `root:root`.
   c. Create a dedicated system user and group `ca-cluster-node`
      (`useradd --system --no-create-home --shell /usr/sbin/nologin
      ca-cluster-node`), matching `ca_cluster_node.service`'s
      `User=ca-cluster-node` / `Group=ca-cluster-node`.
   d. Create `/var/lib/ca_cluster_node` (mode `0750`, owned by
      `ca-cluster-node:ca-cluster-node`) — the default `--data-dir`
      (`cmd/ca_cluster_node/config.hpp`).
   e. Create `/etc/ca_cluster_node` (mode `0750`, owned by
      `ca-cluster-node:ca-cluster-node`) as an empty directory — the
      location `ca_cluster_node.service`'s header comment installs
      `unseal.key` and `rpc_bootstrap.{crt,key}` into. The directory SHALL
      be created empty; those three files SHALL NOT be created or copied by
      this provisioner (Requirement 5).
   f. Install `docker/ca_cluster_node/ca_cluster_node.service` (uploaded by
      the `file` provisioner, unmodified — byte-identical to the file
      already in the repository) to
      `/etc/systemd/system/ca_cluster_node.service`.
   g. Run `systemctl daemon-reload` and `systemctl enable ca_cluster_node`
      (enable, but explicitly do NOT `systemctl start`) — the unit cannot
      start yet because `/etc/default/ca_cluster_node` (its
      `EnvironmentFile`) does not exist until an operator or launch-time
      tooling installs it, per Requirement 5.
2. The provisioner SHALL be idempotent (safe to re-run against the same
   instance without erroring) — `useradd` and `mkdir` calls SHALL check for
   existence first or tolerate "already exists" via `|| true` with a
   distinguishing comment.

---

### Requirement 5: No secrets or per-node configuration baked into the AMI

**User Story:** As a security-conscious operator, I want every instance
launched from a given AMI build to be identical and secret-free, so that one
compromised or leaked AMI never exposes another cluster's unseal passphrase,
auth token, or RPC bootstrap credential.

#### Acceptance Criteria

1. The Packer build context SHALL NOT include, upload, or reference any of:
   an unseal passphrase, `CA_SERVICE_AUTH_TOKEN`, RPC bootstrap
   `crt`/`key` material, `NODE_ID`, or `--peers` values. These remain
   supplied per-instance exactly as `ca_cluster_node.service`'s existing
   header comment and `ca_cluster_node.env.example` already document
   (`/etc/default/ca_cluster_node` + `/etc/ca_cluster_node/{unseal.key,
   rpc_bootstrap.crt,rpc_bootstrap.key}`, installed after launch).
2. A unit test of the provisioning script (Requirement 10) SHALL assert that
   `/etc/ca_cluster_node` exists and is empty and that
   `/etc/default/ca_cluster_node` does not exist, after `provision.sh` runs.
3. The design doc and `packer/ca_cluster_node/README.md` (Requirement 11)
   SHALL state explicitly that `aws_ec2_quorum_manager`'s
   `user_data_template` (see `docker/ca_cluster_node/README.md` Path 3) or
   an equivalent launch-time mechanism remains the place secrets and
   per-node values are injected — this AMI only removes the "install the
   software" step from that flow, not the "configure this instance" step.

---

### Requirement 6: AMI naming and tagging

**User Story:** As an operator managing multiple AMI builds over time, I
want each AMI's name and tags to identify what it contains so that I can
tell builds apart in the EC2 console without decoding an opaque `ami-`
identifier.

#### Acceptance Criteria

1. The produced AMI's `Name` SHALL be
   `kythira-ca-cluster-node-{git_sha}-{arch}-{timestamp}`, where `git_sha`
   is the short (7+ char) commit hash the build ran from, `arch` is
   `amd64`/`arm64`, and `timestamp` is a Packer-supplied build timestamp
   (guarantees uniqueness across repeated builds of the same commit).
2. The AMI (and its backing snapshot) SHALL carry these tags:

   | Tag key | Value |
   |---|---|
   | `kythira:component` | `ca-cluster-node` |
   | `kythira:git-sha` | full 40-character commit SHA the build ran from |
   | `kythira:arch` | `amd64` or `arm64` |
   | `kythira:base-ami` | the resolved source AMI ID (Requirement 3.3) |
   | `kythira:built-by` | `packer` |

3. Tags SHALL be applied via the `amazon-ebs` builder's `tags`,
   `run_tags`, and `snapshot_tags` blocks so that the temporary build
   instance, the final AMI, and its EBS snapshot are all identifiable and
   attributable to the same build.

---

### Requirement 7: Manifest output

**User Story:** As CI or a downstream script, I want a machine-readable
record of which AMI ID a build produced, per architecture and region, so
that I can feed it into `aws_ec2_quorum_manager_config.image_id` or a test's
environment variable without scraping Packer's human-readable log output.

#### Acceptance Criteria

1. The template SHALL include a `post-processor "manifest"` block writing
   to `packer-manifest.json` in the current working directory (Packer's
   default relative-path behavior), `strip_path = true`.
2. `packer-manifest.json`'s `custom_data` map SHALL include `arch` and
   `git_sha` (sourced from the corresponding template variables), so a
   consumer parsing the manifest does not need to separately track which
   build produced which entry.
3. `packer/ca_cluster_node/scripts/build.sh` (Requirement 8) SHALL, after a
   successful build, print the produced AMI ID to stdout on its own final
   line (in addition to the full Packer log), so a CI step can capture it
   with a simple `tail -n1`.

---

### Requirement 8: Build orchestration script

**User Story:** As a developer or CI job, I want a single command that
builds the binary, validates the template, and runs Packer, so that I don't
have to remember or hand-type the multi-step sequence (extract binary →
`packer init` → `packer validate` → `packer build`).

#### Acceptance Criteria

1. A script `packer/ca_cluster_node/scripts/build.sh` SHALL accept
   `--arch amd64|arm64` (required), `--region REGION` (default
   `us-east-1`), and `--skip-binary-build` (optional — reuse an
   already-extracted binary at a fixed path, for iterating on the Packer
   template itself without rebuilding the Docker image each time).
2. `build.sh` SHALL, in order: (a) invoke `extract-binary.sh` for the
   requested `--arch` unless `--skip-binary-build` is given; (b) run
   `packer init packer/ca_cluster_node/ca_cluster_node.pkr.hcl`; (c) run
   `packer validate` with the resolved variables; (d) run `packer build`
   with the resolved variables, `-var git_sha=$(git rev-parse HEAD)`.
3. `build.sh` SHALL exit non-zero and stop before invoking `packer build` if
   `packer validate` fails, and SHALL propagate Packer's own exit code
   unchanged on a build failure.
4. `build.sh` SHALL require `AWS_REGION` or `--region` and standard AWS
   credentials (any provider-chain source) to be resolvable before
   attempting `packer build`; it SHALL run `aws sts get-caller-identity`
   first and print a clear "AWS credentials not configured" error (not a
   raw Packer stack trace) if that call fails.

---

### Requirement 9: Multi-architecture support

**User Story:** As an operator running `aws_ec2_quorum_manager` on
Graviton (`arm64`) instances for cost or performance reasons, I want an
`arm64` AMI available with the same guarantees as the `amd64` one.

#### Acceptance Criteria

1. `build.sh --arch arm64` SHALL produce a working AMI using the identical
   template and provisioning script as `--arch amd64`, differing only in
   the resolved source AMI, builder instance type, and the uploaded
   binary's target architecture.
2. The `amazon-ebs` builder's `instance_type` SHALL default to `t3.micro`
   for `amd64` and `t4g.micro` for `arm64` (matching the existing
   architecture-conditional default pattern already used for
   `AWS_TEST_INSTANCE_TYPE` in
   `.kiro/specs/aws-quorum-manager/requirements.md` Requirement 16 AC 16)
   — both overridable via a `builder_instance_type` template variable.
3. `extract-binary.sh --arch arm64` SHALL either (a) run natively on an
   `arm64` host/CI runner, or (b) cross-build via Docker Buildx/QEMU
   emulation when no native `arm64` runner is available; the script SHALL
   NOT hard fail on an `amd64` host — it SHALL attempt the
   `--platform linux/arm64` build and only fail if that build itself fails
   (e.g. due to Buildx being unavailable), with an error message suggesting
   a native `arm64` runner as the fix.

---

### Requirement 10: Local/offline validation without AWS spend

**User Story:** As a developer iterating on the Packer template, I want a
fast, free check that catches syntax and configuration errors before I
spend the several minutes and small EC2/EBS cost of a real build.

#### Acceptance Criteria

1. `packer fmt -check -diff packer/ca_cluster_node/*.pkr.hcl` SHALL pass
   (canonical HCL formatting) and SHALL be runnable with no AWS
   credentials and no network access.
2. `packer validate` (Requirement 1 AC 4) SHALL be added as a check in
   `ci.yml` (or a new lightweight job) that runs on every push/PR touching
   `packer/ca_cluster_node/**`, gated on Packer being available (install via
   the official HashiCorp apt repository or a pinned release tarball — the
   spec does not mandate which, left to the implementation).
3. `packer/ca_cluster_node/scripts/provision.sh` SHALL additionally be
   exercisable directly (outside Packer) against a plain Ubuntu 24.04
   Docker container for a fast local smoke test of the provisioning logic
   itself (file placement, user/group creation, systemd unit installed —
   `systemctl enable` inside a container without an init system does not
   fully exercise startup, but does exercise every filesystem-level step).
   This SHALL be documented as a manual `docker run` recipe in
   `packer/ca_cluster_node/README.md`; it is not required to run in CI.

---

### Requirement 11: Documentation

**User Story:** As an operator following the existing
`docker/ca_cluster_node/README.md` "Path 3" instructions, I want a concrete,
working procedure for producing the AMI referenced by
`cfg.image_id = "ami-...";`, not a placeholder sentence.

#### Acceptance Criteria

1. A new `packer/ca_cluster_node/README.md` SHALL document: prerequisites
   (Packer ≥ 1.10, Docker or Podman, AWS credentials), the `build.sh`
   command line, the tag/naming scheme (Requirement 6), the manifest output
   location (Requirement 7), and the explicit no-secrets guarantee
   (Requirement 5).
2. `docker/ca_cluster_node/README.md`'s "Path 3" section SHALL be updated to
   replace `// AMI running ca_cluster_node` and the parenthetical "(baked
   into an AMI, e.g. via Packer)" with a pointer to
   `packer/ca_cluster_node/README.md` and the `build.sh --arch amd64 --region
   us-east-1` example invocation.
3. `docker/ca_cluster_node/ecs-task-definitions/README.md`'s "Automated
   alternative" section SHALL gain the same pointer.
4. `tests/ca_cluster_node_real_ec2_test.cpp`'s header comment for
   `KYTHIRA_EC2_TEST_AMI` SHALL be updated to reference
   `packer/ca_cluster_node/README.md` in place of the current "bake it into
   an AMI via Packer or a launch script" placeholder text (code comment
   change only — the environment-variable contract itself is unchanged).

---

### Requirement 12: CI credential bundle for AMI builds

**User Story:** As a repo admin, I want AMI builds run from CI to use the
same least-privilege, OIDC-federated credential model as every other
real-cloud test, scoped to only the permissions `packer build` actually
needs — not the CI role's existing `ca-cluster-node` or `ec2-quorum-manager`
bundle permissions, which are unrelated and should not implicitly grant AMI
publishing rights.

#### Acceptance Criteria

1. A new bundle policy file `scripts/ci-cloud-credentials/aws/policies/ami-build.json`
   SHALL be added, granting exactly: `ec2:RunInstances`,
   `ec2:TerminateInstances`, `ec2:DescribeInstances`,
   `ec2:DescribeInstanceStatus`, `ec2:CreateKeyPair`, `ec2:DeleteKeyPair`,
   `ec2:CreateSecurityGroup`, `ec2:DeleteSecurityGroup`,
   `ec2:AuthorizeSecurityGroupIngress`, `ec2:CreateImage`,
   `ec2:DeregisterImage`, `ec2:DescribeImages`, `ec2:CreateSnapshot`,
   `ec2:DeleteSnapshot`, `ec2:DescribeSnapshots`, `ec2:CreateTags`,
   `ec2:GetPasswordData`, `ssm:GetParameter` (needed for the
   `amazon-parameterstore` data source, Requirement 3.2).
2. `scripts/ci-cloud-credentials/aws/provision-oidc-role.sh`'s `--bundles`
   option and its usage text SHALL accept `ami-build` as a valid bundle
   name alongside the three existing ones, requiring no other change to the
   script (it already loads `policies/{bundle}.json` generically).
3. `scripts/ci-cloud-credentials/README.md`'s bundle table SHALL gain a row
   for `ami-build` → (no dedicated ctest binary — `packer build`) → "EC2
   instance lifecycle + AMI/snapshot creation".

---

### Requirement 13: CI workflow integration

**User Story:** As a repo admin, I want AMI builds to run as an explicitly
toggled, independently-gated job — not silently bundled into the existing
weekly `ca-cluster-node` real-cloud-tests run — because every build creates
a new AMI and EBS snapshot that persists (and accrues storage cost) until
someone deregisters it.

#### Acceptance Criteria

1. `.github/workflows/real-cloud-tests.yml` SHALL gain a new job
   `ami-build`, following the exact same three-level toggle pattern
   (`run_real_cloud_tests` / `aws_enabled` / a new
   `aws_bundle_ami_build` input mapped to
   `REAL_CLOUD_TESTS_AWS_AMI_BUILD_ENABLED`) as the existing `aws` job's
   bundle inputs.
2. The `ami-build` job SHALL run as a matrix over `arch: [amd64, arm64]`,
   using `runs-on: ubuntu-24.04` for `amd64` and a native `arm64` runner
   (e.g. `ubuntu-24.04-arm`) for `arm64` — not QEMU emulation — per
   Requirement 9.3's preference for native builds in CI.
3. The `ami-build` job SHALL run `packer/ca_cluster_node/scripts/build.sh`
   and upload `packer-manifest.json` as a workflow artifact
   (`actions/upload-artifact@v4`) so an operator can retrieve the produced
   AMI ID(s) after the run without re-parsing the job log.
4. The `ami-build` job SHALL NOT be a dependency (`needs:`) of the existing
   `aws` job's `ca-cluster-node` or `ca-cluster-node-rpc-tls` bundle steps —
   wiring the freshly built AMI ID automatically into `KYTHIRA_EC2_TEST_AMI`
   for those bundles is explicitly out of scope for this spec (an operator
   sets `KYTHIRA_EC2_TEST_AMI` manually from the artifact today; automatic
   wiring is a candidate follow-up, not a requirement here).
5. Old AMIs/snapshots produced by prior `ami-build` runs SHALL NOT be
   auto-deleted by the workflow — this spec does not implement AMI
   lifecycle/retention cleanup. `packer/ca_cluster_node/README.md`
   (Requirement 11) SHALL document the manual `aws ec2 deregister-image` +
   `aws ec2 delete-snapshot` commands, filtered by the `kythira:component =
   ca-cluster-node` tag, as an operational note.
