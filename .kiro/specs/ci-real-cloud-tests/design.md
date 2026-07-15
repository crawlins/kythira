# Design Document

## Overview

A new, decoupled GitHub Actions workflow
(`.github/workflows/real-cloud-tests.yml`) runs this project's existing
real-AWS test binaries on a schedule and on demand, authenticated via
OIDC-federated, short-lived AWS credentials. Two provisioning scripts
handle two separate concerns: `provision-oidc-role.sh` creates the IAM
role CI itself assumes (scoped — via a `--bundles` argument — to exactly
the service permissions the currently-enabled test suites need), and
`provision-quorum-test-node-role.sh` creates, once and in advance, the
static IAM role/instance-profile that `aws_quorum_manager_real_ec2_test`'s
launched EC2 instances use. Splitting these means the CI role's own IAM
footprint never exceeds one scoped `iam:PassRole` statement — it never
holds `iam:CreateRole` or any other IAM-write permission, closing off the
privilege-escalation risk that permission would otherwise carry entirely,
rather than merely constraining it. Three independent repository-variable
layers (whole-feature, per-provider, per-bundle) gate every stage, each
with a `workflow_dispatch` input override for one-off manual runs.

One small test-code change: `tests/aws_quorum_manager_real_ec2_test.cpp`
no longer creates and tears down its own IAM role/instance-profile per
run; it resolves an existing one by name instead (Requirement 3). Every
other file this spec touches is CI/CD infrastructure or documentation.
`tests/aws_real_ec2_test_support.hpp`'s existing cost-tracking and
signal-driven-cleanup apparatus (`.kiro/specs/ca-cluster-rpc-mtls-real-aws/`)
is reused unmodified.

## Architecture

```
.github/workflows/real-cloud-tests.yml   (NEW)
  triggers: workflow_dispatch (inputs mirror all vars below), schedule (weekly)
  │
  ├── job: aws
  │     if: master switch AND REAL_CLOUD_TESTS_AWS_ENABLED
  │     environment: real-cloud-tests   (id-token: write)
  │     steps:
  │       1. configure-aws-credentials (OIDC, role-to-assume: vars.AWS_CI_ROLE_ARN)
  │       2. build (same build steps as ci.yml's build-and-test job,
  │          KYTHIRA_AWS_REAL_EC2_TESTS=ON)
  │       3. for each enabled bundle (sequential):
  │            ctest -L "<bundle's labels>" --output-on-failure
  │
  ├── job: azure   (skeleton only — no-ops with a clear message; see Non-Goals)
  ├── job: gcp     (skeleton only — no-ops with a clear message; see Non-Goals)
  ├── job: oci     (skeleton only — no-ops with a clear message; see Non-Goals)
  └── job: alibaba (skeleton only — no-ops with a clear message; see Non-Goals)

scripts/ci-cloud-credentials/
  README.md                              (NEW — top-level overview + provider table)
  aws/
    provision-oidc-role.sh               (NEW — CI's own OIDC identity)
    provision-quorum-test-node-role.sh   (NEW — static node role/profile, run once)
    policies/
      ec2-quorum-manager.json            (NEW — IAM statement fragment)
      ca-cluster-node.json               (NEW — IAM statement fragment)
      ca-cluster-node-rpc-tls.json       (NEW — IAM statement fragment)
    README.md                            (NEW — setup walkthrough + cost estimate)

tests/aws_quorum_manager_real_ec2_test.cpp   (MODIFIED — Requirement 3.2)
```

## Data Models

### Repository Variables and Workflow Inputs

All boolean-valued; absent is equivalent to `false`.

| Variable | Requirement | `workflow_dispatch` input |
|---|---|---|
| `REAL_CLOUD_TESTS_ENABLED` | 1.1 | `run_real_cloud_tests` |
| `REAL_CLOUD_TESTS_AWS_ENABLED` | 1.2 | `aws_enabled` |
| `REAL_CLOUD_TESTS_AZURE_ENABLED` | 1.2 | `azure_enabled` |
| `REAL_CLOUD_TESTS_GCP_ENABLED` | 1.2 | `gcp_enabled` |
| `REAL_CLOUD_TESTS_OCI_ENABLED` | 1.2 | `oci_enabled` |
| `REAL_CLOUD_TESTS_ALIBABA_ENABLED` | 1.2 | `alibaba_enabled` |
| `REAL_CLOUD_TESTS_AWS_EC2_QUORUM_ENABLED` | 1.3 | `aws_bundle_ec2_quorum` |
| `REAL_CLOUD_TESTS_AWS_CA_CLUSTER_ENABLED` | 1.3 | `aws_bundle_ca_cluster` |
| `REAL_CLOUD_TESTS_AWS_CA_CLUSTER_RPC_TLS_ENABLED` | 1.3 | `aws_bundle_ca_cluster_rpc_tls` |
| `AWS_CI_ROLE_ARN` | 4.6, 7.1 | *(not overridable — a fixed identity, not a per-run choice)* |

Resolution pattern for every gated value, at the top of each job:

```yaml
env:
  RUN_ENABLED: ${{ inputs.run_real_cloud_tests != null && inputs.run_real_cloud_tests || vars.REAL_CLOUD_TESTS_ENABLED }}
  AWS_ENABLED: ${{ inputs.aws_enabled != null && inputs.aws_enabled || vars.REAL_CLOUD_TESTS_AWS_ENABLED }}
  # ... one line per toggle, same shape
```

(`inputs.x != null` distinguishes "operator explicitly passed `false`" from
"operator left the input at its unset default," since a bare `inputs.x ||
vars.X` would incorrectly fall through to the variable when an operator
deliberately disables something that's normally on.)

### IAM Policy Bundle Shape

Each bundle file (`scripts/ci-cloud-credentials/aws/policies/<bundle>.json`)
is a bare JSON array of IAM policy `Statement` objects (not a full policy
document — `build_bundle_policy()`, Component 2 below, wraps them).
`policies/ca-cluster-node.json` (no IAM statement at all — this bundle
never attaches an instance profile):

```json
[
  {
    "Sid": "CaClusterNodeEc2Lifecycle",
    "Effect": "Allow",
    "Action": [
      "ec2:CreateVpc", "ec2:DeleteVpc",
      "ec2:CreateSubnet", "ec2:DeleteSubnet",
      "ec2:CreateSecurityGroup", "ec2:DeleteSecurityGroup",
      "ec2:AuthorizeSecurityGroupIngress",
      "ec2:CreateInternetGateway", "ec2:DeleteInternetGateway",
      "ec2:AttachInternetGateway", "ec2:DetachInternetGateway",
      "ec2:CreateRouteTable", "ec2:DeleteRouteTable",
      "ec2:CreateRoute", "ec2:AssociateRouteTable", "ec2:DisassociateRouteTable",
      "ec2:ModifySubnetAttribute",
      "ec2:CreateKeyPair", "ec2:DeleteKeyPair",
      "ec2:RunInstances", "ec2:TerminateInstances",
      "ec2:DescribeInstances", "ec2:CreateTags"
    ],
    "Resource": "*"
  }
]
```

`policies/ec2-quorum-manager.json` — the EC2 statement mirrors the shape
above (with its own additional NAT-gateway/EIP/NACL/spot-price/console-
output/instance-type/image actions per Requirement 2.1), plus exactly one
IAM statement:

```json
[
  {
    "Sid": "Ec2QuorumManagerEc2Lifecycle",
    "Effect": "Allow",
    "Action": [ "...", "..." ],
    "Resource": "*"
  },
  {
    "Sid": "Ec2QuorumManagerPassRole",
    "Effect": "Allow",
    "Action": "iam:PassRole",
    "Resource": "arn:aws:iam::{{ACCOUNT_ID}}:role/kythira-aws-quorum-test-node-role"
  }
]
```

(Most EC2 actions have no resource-level permission support in IAM at all
— AWS's own service authorization reference documents this; `Resource:
"*"` for these specific actions is the ceiling of achievable scoping, not
a shortcut. `iam:PassRole`, by contrast, *is* scoped tightly, to the one
static role Requirement 3 creates — `{{ACCOUNT_ID}}` is substituted by
`build_bundle_policy()`, Component 2 step 6, from the operator's own
`aws sts get-caller-identity` output.)

## Components and Interfaces

### 1. `.github/workflows/real-cloud-tests.yml` — AWS job sketch

```yaml
name: Real Cloud Tests

on:
  workflow_dispatch:
    inputs:
      run_real_cloud_tests: { type: boolean, required: false }
      aws_enabled: { type: boolean, required: false }
      aws_bundle_ec2_quorum: { type: boolean, required: false }
      aws_bundle_ca_cluster: { type: boolean, required: false }
      aws_bundle_ca_cluster_rpc_tls: { type: boolean, required: false }
      # azure_enabled / gcp_enabled / oci_enabled / alibaba_enabled: same shape
  schedule:
    - cron: '0 6 * * 1'   # Monday 06:00 UTC — see Requirement 5.2 for rationale

permissions:
  contents: read

jobs:
  aws:
    name: Real Cloud Tests (AWS)
    runs-on: ubuntu-24.04
    timeout-minutes: 180
    environment: real-cloud-tests
    permissions:
      id-token: write
      contents: read
    if: |
      (github.event.inputs.run_real_cloud_tests != null && github.event.inputs.run_real_cloud_tests == 'true'
       || github.event.inputs.run_real_cloud_tests == null && vars.REAL_CLOUD_TESTS_ENABLED == 'true')
      && (github.event.inputs.aws_enabled != null && github.event.inputs.aws_enabled == 'true'
          || github.event.inputs.aws_enabled == null && vars.REAL_CLOUD_TESTS_AWS_ENABLED == 'true')
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Fail closed if AWS_CI_ROLE_ARN is unset
        if: ${{ vars.AWS_CI_ROLE_ARN == '' }}
        run: |
          echo "::error::REAL_CLOUD_TESTS_AWS_ENABLED is true but the AWS_CI_ROLE_ARN repository variable is unset. See scripts/ci-cloud-credentials/aws/README.md." >&2
          exit 1

      - name: Configure AWS credentials (OIDC)
        uses: aws-actions/configure-aws-credentials@v4
        with:
          role-to-assume: ${{ vars.AWS_CI_ROLE_ARN }}
          role-session-name: kythira-ci-real-cloud-tests
          aws-region: us-east-1

      # ... system deps / vcpkg / Rust toolchain steps identical to
      # ci.yml's build-and-test job, omitted here for brevity — see
      # tasks.md for the exact reuse-vs-duplicate decision.

      - name: Configure (Release, real-cloud tests enabled)
        run: |
          cmake -B build -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_COMPILER=clang++-18 \
            -DCMAKE_PREFIX_PATH=${{ github.workspace }}/vcpkg_installed/x64-linux \
            -DKYTHIRA_AWS_REAL_EC2_TESTS=ON

      - name: Build
        run: cmake --build build -j$(nproc)

      - name: Fail closed if a real-EC2 test binary is missing
        run: |
          for t in aws_quorum_manager_real_ec2_test ca_cluster_node_real_ec2_test ca_cluster_node_rpc_tls_real_ec2_test; do
            [ -x "build/tests/$t" ] || { echo "::error::$t was not built — check LIBSSH2_FOUND/KYTHIRA_HAS_AWS_SDK at configure time." >&2; exit 1; }
          done

      - name: Run ec2-quorum-manager bundle
        if: ${{ env.BUNDLE_EC2_QUORUM == 'true' }}
        env:
          KYTHIRA_TEST_IAM_INSTANCE_PROFILE: kythira-aws-quorum-test-node-profile
        run: ctest --test-dir build -L 'real-ec2' -R '^aws_quorum_manager_real_ec2_test$' --output-on-failure

      - name: Run ca-cluster-node bundle
        if: ${{ env.BUNDLE_CA_CLUSTER == 'true' }}
        run: ctest --test-dir build -L 'real-ec2' -R '^ca_cluster_node_real_ec2_test$' --output-on-failure

      - name: Run ca-cluster-node-rpc-tls bundle
        if: ${{ env.BUNDLE_CA_CLUSTER_RPC_TLS == 'true' }}
        run: ctest --test-dir build -L 'real-ec2' -R '^ca_cluster_node_rpc_tls_real_ec2_test$' --output-on-failure
```

`ctest -R '^<exact-binary-name>$'` (test *name*, not just `-L` label
selection) is used for the per-bundle steps rather than relying on labels
alone, since `ca_cluster_node_real_ec2_test` and
`ca_cluster_node_rpc_tls_real_ec2_test` share every label except `rpc_tls`
(Requirement 2.5) — combining `-L 'real-ec2'` with an exact `-R` name
anchor is simpler and more obviously correct than constructing a label
expression that positively selects one and negatively excludes the other.
The `ec2-quorum-manager` step's explicit
`KYTHIRA_TEST_IAM_INSTANCE_PROFILE` is set here for clarity even though
it matches Requirement 3.2's own well-known default — an operator who
later renames the static role/profile only needs to update this one line,
not `tests/aws_quorum_manager_real_ec2_test.cpp` itself.

### 2. `scripts/ci-cloud-credentials/aws/provision-oidc-role.sh` — CLI contract

```
Usage: provision-oidc-role.sh --github-org ORG --github-repo REPO \
           --bundles BUNDLE[,BUNDLE...] \
           [--role-name NAME] [--session-duration-seconds N] \
           [--ref-restriction REF] \
           [--dry-run]

  --github-org/--github-repo   e.g. crawlins / kythira
  --bundles                    ec2-quorum-manager,ca-cluster-node,ca-cluster-node-rpc-tls
                                (any non-empty subset, comma-separated)
  --role-name                  default: kythira-ci-real-cloud-tests
  --session-duration-seconds   default: 3600
  --ref-restriction            default: unset (trust policy subject is
                                repo:ORG/REPO:*); when given, e.g.
                                "ref:refs/heads/main", further restricts
                                the subject condition
  --dry-run                    print the AWS CLI calls that would run
                                without executing them
```

Implementation shape (bash + `aws` CLI, matching
`scripts/pre-commit-coverage.sh`'s existing style — `set -euo pipefail`,
functions per logical step, clear `[step]`-prefixed progress output):

1. `require aws` / `aws sts get-caller-identity` sanity check (fail fast
   with a clear message if the operator's own local AWS credentials aren't
   configured — this script is run by a human operator with admin-ish
   local credentials, not by CI itself). Captures the account ID for step
   6 below.
2. `ensure_oidc_provider()`: `aws iam list-open-id-connect-providers`,
   grep for `token.actions.githubusercontent.com`; if absent,
   `aws iam create-open-id-connect-provider` (Requirement 4.3).
3. `build_trust_policy()`: renders the trust-policy JSON from
   `--github-org`/`--github-repo`/`--ref-restriction` (Requirement 4.4)
   into a temp file.
4. `ensure_role()`: `aws iam get-role` / `create-role` /
   `update-assume-role-policy` (idempotent — Requirement 4.2). No
   permissions boundary is attached or needed — this role never holds
   `iam:CreateRole`, so there is nothing for a boundary to constrain
   (Requirement 3.3).
5. `build_bundle_policy()`: concatenates the `Statement` arrays from each
   `policies/<bundle>.json` named in `--bundles` into one policy document,
   substituting step 1's account ID into `ec2-quorum-manager.json`'s
   `{{ACCOUNT_ID}}` placeholder (Data Models' "IAM Policy Bundle Shape").
6. `ensure_role_policy()`: `aws iam put-role-policy` with the bundle
   policy from step 5 — `put-role-policy` is naturally idempotent/
   replace-in-place, satisfying Requirement 4.5's "disabling a bundle and
   re-running revokes that bundle's permissions" without extra logic.
7. Print the role ARN (Requirement 4.6) and the exact
   `gh variable set AWS_CI_ROLE_ARN --body <arn>` /
   `gh variable set REAL_CLOUD_TESTS_AWS_ENABLED --body true` follow-up
   commands the operator still needs to run (this script only touches
   AWS; setting GitHub repository variables is a separate, explicit step
   the operator takes with their own GitHub permissions).

### 3. `scripts/ci-cloud-credentials/aws/provision-quorum-test-node-role.sh` — CLI contract

```
Usage: provision-quorum-test-node-role.sh [--role-name NAME] [--profile-name NAME] [--dry-run]

  --role-name      default: kythira-aws-quorum-test-node-role
  --profile-name   default: kythira-aws-quorum-test-node-profile
  --dry-run        print the AWS CLI calls that would run without executing them
```

Run once by an operator with full IAM rights, entirely independent of
Component 2's script — this role is attached to *test-launched EC2
instances*, not to CI itself, and is never involved in the OIDC exchange.
Implementation shape:

1. Same sanity check as Component 2 step 1.
2. `ensure_role()`: `aws iam get-role` / `create-role`, trust policy fixed
   to `{"Effect":"Allow","Principal":{"Service":"ec2.amazonaws.com"},"Action":"sts:AssumeRole"}`
   — identical to `tests/aws_quorum_manager_real_ec2_test.cpp`'s existing
   `create_iam_role()` trust policy (Requirement 3.1).
3. `ensure_role_policy()`: `aws iam put-role-policy` granting only
   `sts:GetCallerIdentity` — identical to that same existing method's
   policy content.
4. `ensure_instance_profile()`: `aws iam get-instance-profile` /
   `create-instance-profile` / `add-role-to-instance-profile` (idempotent
   — `add-role-to-instance-profile` is a no-op, not an error, when the
   role is already attached).
5. Print the role ARN — this is the value Component 2's
   `ec2-quorum-manager.json` bundle statement references (Data Models).

### 4. `tests/aws_quorum_manager_real_ec2_test.cpp` — resolve, don't create

`create_iam_role()` (today: `CreateRole` → `PutRolePolicy` →
`CreateInstanceProfile` → `AddRoleToInstanceProfile`, with a matching
teardown sequence) is replaced by `resolve_iam_instance_profile()`: reads
`KYTHIRA_TEST_IAM_INSTANCE_PROFILE` from the environment, falling back to
the literal `kythira-aws-quorum-test-node-profile` when unset
(Requirement 3.2), and does nothing else — no IAM API call, no
`iam_role_name`/`iam_policy_name` generation, no teardown step for either.
`RealEc2Fixture`'s `iam` member (`Aws::IAM::IAMClient`) becomes unused by
this specific flow; whether to remove it entirely or keep it for a future
IAM-touching test case is an implementation-time judgment call, not
specified further here.

## Correctness Properties

### Property 1: Bundle-permission correspondence stays mechanically verifiable
**Validates: Requirements 2.1, 2.2, 2.5**

Requirement 2's mapping from bundle → CTest label/name → IAM statements is
recorded in exactly three places designed to be diffed against each other
whenever a real-EC2 test file changes: the bundle's `grep -oE
"Aws::[A-Za-z0-9]+::Model::[A-Za-z]+Request"` output against its `.cpp`
file (the ground truth this spec's own requirements.md was written from),
the corresponding `policies/<bundle>.json`, and the workflow's `ctest -R`
step. Task list item (see tasks.md) includes re-running that same grep
against each bundle's test file as an explicit verification step, not
just trusting requirements.md's already-derived list to stay accurate
forever.

### Property 2: Disabling a bundle actually revokes its permissions
**Validates: Requirements 4.5**

Because `put-role-policy` (not additively across multiple calls, and not
a managed-policy `attach` that would need a matching `detach`) replaces
the named inline policy's content wholesale, re-running the provisioning
script with a bundle removed from `--bundles` produces a policy document
that no longer contains that bundle's statements — including, for
`ec2-quorum-manager`, its `iam:PassRole` statement, the only IAM
permission the CI role ever holds. Verified by Property 1's task-list item
additionally diffing `aws iam get-role-policy` output before and after a
bundle-removal re-run during implementation.

### Property 3: No credential ever touches GitHub as a stored secret
**Validates: Requirements 4.6, 3.5**

`aws-actions/configure-aws-credentials@v4`'s OIDC mode exports session
credentials only as job-scoped environment variables for the remainder of
that job — GitHub never stores them, they are not visible in logs (the
action masks them), and they expire per Requirement 3.5's session-duration
cap regardless of whether the job cleans up normally or is canceled. This
is a structural property of OIDC federation, not something this design
needs to separately implement or test.

### Property 4: The CI role can never expand its own IAM footprint
**Validates: Requirements 2.3, 3.4**

The CI role's only possible IAM action, across every bundle in every
combination, is `iam:PassRole` scoped to one fixed, pre-existing role ARN
it did not create and cannot modify (it holds no `iam:PutRolePolicy` on
that role, or on any role). There is no code path — compromised test
binary, malicious dependency, or operator error in `--bundles` — by which
running this workflow could result in the CI role holding more IAM
permission after a run than before it, since nothing in its policy can
create, attach a policy to, or modify any IAM principal. This is a
stronger property than a permissions boundary would provide (a boundary
constrains what a *creatable* role could do; this design has no creatable
role at all for the CI role's own credential).

## Error Handling

Every fail-closed case (Requirement 7) is an explicit, early
`if:`-gated step emitting `::error::`-prefixed output (GitHub Actions'
own annotation syntax, surfaced directly in the workflow run's summary UI)
followed by `exit 1` — no case relies on a downstream step's own generic
failure message to explain *why* the job failed. `configure-aws-credentials`'s
own error surface (Requirement 7.3) is left as-is, unwrapped, since AWS's
STS error messages for a trust-policy mismatch are already specific and
actionable (e.g. naming which condition key failed). Requirement 3.3's
case (the static node role/profile doesn't exist) is deliberately *not*
given its own early check — `RunInstances`' own "instance profile not
found" error is already specific enough, and adding a redundant pre-flight
`aws iam get-instance-profile` call would just be one more thing that
could itself fail for an unrelated reason (a transient IAM read error)
and produce a *less* accurate message than the real failure.

## Testing Strategy

This spec's deliverable is CI/CD infrastructure, documentation, and one
small test-fixture change — there is no new C++ test *suite* to write (the
change to `tests/aws_quorum_manager_real_ec2_test.cpp` removes code, it
doesn't add a new test case). Its own correctness is verified by:

- **Both provisioning scripts**: run against a real (or a disposable/
  sandbox) AWS account during implementation, twice in a row each
  (idempotency — Requirements 3.1/4.2), and `provision-oidc-role.sh` once
  more with a bundle removed (Property 2's revocation check).
- **`tests/aws_quorum_manager_real_ec2_test.cpp`'s resolved change**: run
  once against a real AWS account with `provision-quorum-test-node-role.sh`
  already applied and `KYTHIRA_TEST_IAM_INSTANCE_PROFILE` unset (exercises
  the well-known-default fallback), and once with it explicitly set to a
  differently-named profile (exercises the override path).
- **Workflow YAML**: `workflow_dispatch`-triggered manual runs during
  implementation with each toggle combination exercised at least once —
  master off (job skipped entirely), AWS off (AWS job skipped, no other
  provider affected once those exist), all AWS bundles off (job runs,
  no test step executes, no credential even acquired — Requirement 1.3),
  one bundle on (only that bundle's `ctest` step runs and it passes
  against real AWS).
- **Fail-closed cases** (Requirement 7): deliberately misconfigure each
  (unset `AWS_CI_ROLE_ARN` with AWS enabled; enable a bundle whose binary
  isn't built; enable `ec2-quorum-manager` without having run
  `provision-quorum-test-node-role.sh` first, confirming `RunInstances`'
  own error surfaces clearly per this design's Error Handling section)
  during implementation and confirm the job fails with the intended
  message, not a confusing downstream error.

## Non-Goals

- **Azure, GCP, OCI, Alibaba Cloud real-cloud test execution** is
  explicitly out of scope for this spec's own implementation — none of
  those providers has a real-cloud test suite to run yet (`doc/TODO.md`'s
  "Cloud Provider Support" section lists all four as unimplemented). Their
  workflow jobs and repository variables are scaffolded (Requirement 1.5)
  so the *pattern* is established, but each job's body is a no-op message
  until a future spec adds that provider's quorum manager/certificate
  provider AND a corresponding real-cloud test suite for it — at which
  point that future spec's own tasks.md is the right place to fill in that
  provider's provisioning script(s) and `policies/*.json`, following this
  spec's AWS implementation (including the pre-provisioned-identity
  pattern of Requirement 3, where applicable — Azure federated identities
  and GCP Workload Identity Federation have their own native equivalents
  of "attach an existing, narrowly-scoped identity rather than create one
  dynamically") as the template.
- **A managed-policy alternative to inline `put-role-policy`** was
  considered and rejected: a managed policy has a version history and a
  hard limit of five saved versions per policy, which would eventually
  force the provisioning script to prune old versions on every re-run —
  extra complexity with no benefit here, since this role is single-purpose
  and its policy is never meant to be attached to any other role.
- **Automatic AWS account bootstrapping (creating the AWS account itself,
  or enabling AWS SSO/Organizations)** is out of scope — both provisioning
  scripts assume an AWS account and admin-capable local credentials
  already exist, matching every other AWS setup document already in this
  project (`docker/ca_cluster_node/README.md`,
  `docker/ca_cluster_node/ecs-task-definitions/README.md`).
- **Rotating or auditing either role after initial provisioning** (e.g. an
  automated periodic re-run of a provisioning script, or AWS Config rules
  alerting on drift) is out of scope — both scripts are designed to be
  safely re-run by hand whenever bundles or naming change (Property 2),
  but nothing in this spec schedules that re-run automatically.
- **Extending the pre-provisioned-role pattern to a bundle that doesn't
  need it** — `ca-cluster-node`/`ca-cluster-node-rpc-tls` never attach an
  instance profile at all (Requirement 2.2), so Requirement 3's script and
  test-code change apply only to `ec2-quorum-manager`; there is no
  equivalent to add for the other two bundles.
