# AWS real-cloud-tests setup

Sets up short-lived, OIDC-federated AWS credentials for
`.github/workflows/real-cloud-tests.yml`'s `aws` job, and (for the
`ec2-quorum-manager` bundle only) the static IAM identity its test EC2
instances launch with. See [`../README.md`](../README.md) for the
three-level toggle model and service-bundle concept this document assumes.

## Prerequisites

- An AWS account, billing enabled.
- The `aws` CLI installed and configured **locally** (not in CI) with
  credentials that have IAM-admin permissions — `iam:CreateRole`,
  `iam:PutRolePolicy`, `iam:CreateInstanceProfile`,
  `iam:CreateOpenIDConnectProvider`, and related read actions. These
  credentials are only ever used locally, by the operator running the
  scripts below, once. Neither script grants CI itself any of these
  permissions.
- The GitHub CLI (`gh`) installed and authenticated, with admin access to
  this repository (to set repository variables).
- `python3` on `PATH` (used internally by `provision-oidc-role.sh` to merge
  bundle policy JSON — no third-party packages required).

## First-time setup

**Order matters if you're enabling `ec2-quorum-manager`**: its bundle policy
references the static node role's ARN by well-known name, so that role must
exist before you provision the CI role. If you're only enabling
`ca-cluster-node` and/or `ca-cluster-node-rpc-tls`, skip straight to step 2.

### 1. Provision the static quorum-test-node role (only if enabling `ec2-quorum-manager`)

```sh
scripts/ci-cloud-credentials/aws/provision-quorum-test-node-role.sh
```

Creates `kythira-aws-quorum-test-node-role` and
`kythira-aws-quorum-test-node-profile` with the defaults `RunInstances`
expects. Run with `--dry-run` first if you want to see the exact AWS CLI
calls without making them. Safe to re-run — every step checks for existing
state first.

### 2. Provision the CI identity

```sh
scripts/ci-cloud-credentials/aws/provision-oidc-role.sh \
    --github-org <org> --github-repo <repo> \
    --bundles ec2-quorum-manager,ca-cluster-node,ca-cluster-node-rpc-tls
```

Pass only the bundles you actually want CI to be able to run — a bundle
left out of `--bundles` grants the CI role none of that bundle's
permissions. Creates (if absent) the GitHub Actions OIDC provider, the
`kythira-ci-real-cloud-tests` IAM role trusted only by the exact OIDC
subject `repo:<org>/<repo>:environment:real-cloud-tests`, and an inline
policy scoped to exactly the bundles given. Only a job that declares
`environment: real-cloud-tests` can assume the role, so the environment's
protection rules (required reviewers, deployment branches) gate every use of
it; pass `--environment NAME` to trust a different environment. Prints the resulting role ARN and the exact `gh variable set`
commands to run next.

### 3. Set repository variables

Run the `gh variable set` commands the previous step printed, e.g.:

```sh
gh variable set AWS_CI_ROLE_ARN --body 'arn:aws:iam::123456789012:role/kythira-ci-real-cloud-tests'
gh variable set REAL_CLOUD_TESTS_ENABLED --body true
gh variable set REAL_CLOUD_TESTS_AWS_ENABLED --body true
gh variable set REAL_CLOUD_TESTS_AWS_EC2_QUORUM_ENABLED --body true
```

(one `REAL_CLOUD_TESTS_AWS_<BUNDLE>_ENABLED` per bundle you provisioned).
Optionally also set `AWS_REAL_CLOUD_TESTS_REGION` (defaults to
`us-east-1` if unset).

The CloudWatch monitoring-config test (`aws-monitoring` job;
`scripts/real-cloud-monitoring/aws-cloudwatch.sh`) uses the same CI role
but its own toggle, `REAL_CLOUD_TESTS_AWS_MONITORING_ENABLED` — include
`cloudwatch-monitoring` in `--bundles` when provisioning if you enable it.
Its permissions are CloudWatch Logs ingest/read scoped to the
`/kythira/chaos-node/*` log groups plus `cloudwatch:ListMetrics`; cost per
run is effectively zero (a handful of log events, deleted afterwards).

## Adding or removing a bundle later

Re-run `provision-oidc-role.sh` with the full new `--bundles` list — it
replaces the CI role's policy content wholesale, so a bundle left out of a
later run genuinely loses that bundle's permissions, not merely stops using
them (verify with `aws iam get-role-policy --role-name
kythira-ci-real-cloud-tests --policy-name kythira-ci-real-cloud-tests-policy`
if you want to confirm). Then flip the corresponding
`REAL_CLOUD_TESTS_AWS_<BUNDLE>_ENABLED` repository variable with
`gh variable set`.

## Re-applying the policy from GitHub, with approval

A bundle edit merged to main reaches AWS through the **Reprovision AWS CI
role** workflow (`.github/workflows/reprovision-aws-ci-role.yml`), with no
local admin credentials. Each run waits for a required reviewer's approval.

One-time setup, as an IAM admin:

```sh
scripts/ci-cloud-credentials/aws/provision-ci-role-updater.sh \
    --github-org crawlins --github-repo kythira --reviewer <your-login>
```

It creates or updates:

- `kythira-ci-real-cloud-tests-boundary`, set as the CI role's permissions
  boundary. It allows everything except `iam:*`, `organizations:*` and
  `account:*`, plus `iam:PassRole` on the quorum-test node role. Whatever
  inline policy is applied, the CI role cannot gain IAM rights.
- `kythira-ci-role-updater`, an OIDC role trusted only by jobs in the
  `ci-role-admin` environment. It may read and replace the CI role's inline
  policy, and it is explicitly denied changing the role's boundary, trust
  policy or attached policies.
- The `ci-role-admin` environment, which admits only `main` and requires
  the given reviewers, and the `AWS_CI_ROLE_UPDATER_ARN` repository
  variable.

After that, run the workflow from main. With `bundles` left empty it keeps
exactly the bundles the role already carries (`render-ci-policy.py
--detect-bundles` reads them from the live policy's Sids), so it cannot
revoke a bundle by omission. It prints a statement-level diff, applies the
policy, and reads it back. Tick `dry_run` to see only the diff.
`update-ci-role-policy.sh` is the same step, runnable locally.

The workflow cannot create the role, change its trust policy, or touch the
developer user or the node role: those still go through the scripts above.

## The `asg-quorum-manager` bundle

Grants what `tests/aws_asg_quorum_manager_real_test.cpp` needs to drive
`aws_asg_quorum_manager` against real Auto Scaling
(`.kiro/specs/aws-asg-real-cloud-tests/`, Requirement 8). No other
fragment grants any `autoscaling:*` action, which is why this is a bundle
of its own rather than an addition to `ec2-quorum-manager`.

```sh
scripts/ci-cloud-credentials/aws/provision-oidc-role.sh \
    --github-org <org> --github-repo <repo> \
    --bundles <every bundle the role already has>,asg-quorum-manager
gh variable set REAL_CLOUD_TESTS_AWS_ASG_QUORUM_ENABLED --body true
```

The bundle has **no `workflow_dispatch` input of its own**: the workflow is
at GitHub's cap of 25 inputs, so a manual dispatch selects it through
`aws_bundle_ec2_quorum` (which then runs both quorum-manager suites), and
only the scheduled run controls it independently, through the variable
above.

The `aws-ec2-launch-options` job (`tests/aws_ec2_launch_options_real_test.cpp`:
placement groups, spot vs on-demand, provision-timeout cleanup) needs no
bundle of its own. It runs on `ec2-quorum-manager`, which grants the
placement-group and `DescribeAvailabilityZones` / `DescribeSubnets` /
`DescribeSecurityGroups` actions it uses. A manual dispatch selects it
through `aws_bundle_ec2_quorum` too; its schedule toggle is separate:

```sh
gh variable set REAL_CLOUD_TESTS_AWS_EC2_LAUNCH_OPTIONS_ENABLED --body true
```

Policy JSON cannot carry comments, so the reasoning behind each statement
lives here. Each `Sid` names the rule it follows.

| `Sid` | Resource | Why it is that wide |
|---|---|---|
| `…DescribeNoResourceLevelPermissions` | `*` | Auto Scaling and EC2 `Describe*` actions do not support resource-level permissions at all; a narrower `Resource` is not "tighter", it is a policy that never matches. |
| `…MutateKythiraGroupsOnly` | ASGs named `kythira-asgtest-*` | Auto Scaling *does* support resource-level permissions for these actions, so the suite can only create, resize, terminate into, tag or delete groups under its own prefix. The test fixture MUST name its groups with that prefix. |
| `…CreateFixtureResourcesNotYetTaggable` | `*` | The resource does not exist yet, so there is nothing to scope to. `ec2:RunInstances` is here because `CreateAutoScalingGroup` with a launch template is authorised against the **caller's** permission to launch from that template, across every resource type a launch touches (image, subnet, security group, network interface, volume). |
| `…StopAndTearDownSuiteTaggedOnly` | `*`, conditioned on `kythira:suite=aws-asg-quorum-manager` | Destructive EC2 actions, and retagging (the manager's own `CreateTags` on the instances it adopts), only reach resources the fixture tagged. The fixture MUST tag its VPC, subnets, security group, launch template and (via the launch template's tag specification and ASG tag propagation) its instances. The key is deliberately **not** `kythira:managed-by`: the manager itself overwrites that tag on every instance it provisions (`asg_quorum_manager`), which would put those instances outside the condition. |

Not granted, on purpose:

- **No internet gateway or route-table actions.** Whether an instance
  needs egress to reach `running` is task 1c of the spec. Add them only if
  that measurement says so.
- **No `iam:CreateServiceLinkedRole`.** The first `CreateAutoScalingGroup`
  in an account creates `AWSServiceRoleForAutoScaling`. Make that first call
  as the admin operator (the spec's task 1 probe does), not as CI.
- **No `iam:PassRole`.** The suite's instances run nothing, so the launch
  template carries no instance profile.

Verify as the CI principal, not as admin: a probe made with the admin
credentials proves nothing about this role. After provisioning, assume
`kythira-ci-real-cloud-tests` (or dispatch a run) and confirm one
`DescribeAutoScalingGroups` call succeeds.

## Instance actions are tag-scoped

No bundle lets CI stop, start, terminate or retag an EC2 instance it did not
launch. Each bundle's `…InstancesOwnedOnly` statement grants those actions on
instances only, under a condition on a tag the suite applies **in the
`RunInstances` request itself** (a tag added by a later `CreateTags` would
leave a window in which the suite could not clean up its own instance):

| Bundle | Instance actions | Condition |
|---|---|---|
| `ec2-quorum-manager` | `CreateTags`, `StartInstances`, `StopInstances`, `TerminateInstances` | `kythira:managed-by` is `ec2_quorum_manager` (every node the manager launches) or `aws_quorum_manager_real_ec2_test` (that suite's bastion) |
| `ca-cluster-node`, `ca-cluster-node-rpc-tls` | `CreateTags`, `TerminateInstances` | `kythira:managed-by` is `ec2_quorum_manager`; both suites launch every instance through the manager |
| `ami-build` | `CreateTags`, `StopInstances`, `TerminateInstances` | `kythira:built-by` is `packer`, from the template's `run_tags` |
| `perf-cloud` | `CreateTags`, `TerminateInstances` | `kythira-perf-run` is present; its value is per-run, so the key is what is checked |
| `asg-quorum-manager` | `CreateTags`, `StopInstances`, `TerminateInstances` | `kythira:suite` is `aws-asg-quorum-manager` (see the table above) |

The condition only means something if CI cannot put the tag on someone
else's instance, so `ec2:CreateTags` is the one action that is scoped
everywhere, not only in the statements above. Every EC2 bundle carries the
same two tagging statements:

- `Ec2TagOnCreate`: tags applied by the request that creates the resource
  (`ec2:CreateAction`), on any resource type. A resource CI is creating is
  CI's by definition.
- `Ec2TagExistingNonInstanceResources`: later tags on anything that is
  **not** an instance (`NotResource`), as long as the request does not set
  `kythira:suite`. That key is what `asg-quorum-manager` conditions its
  VPC, subnet, security group and launch template deletes on, so letting CI
  add it to an existing resource would let CI delete that resource.

Retagging an existing instance is allowed only through an
`…InstancesOwnedOnly` statement, that is, only on an instance already in
scope. `render-ci-policy.py` collapses the shared statements to one copy
when it merges bundles (IAM rejects a repeated `Sid`, and the role's inline
policy has 10,240 characters for every bundle together), and its `--check`
mode, run by the `aws-ci-policies` CI job, fails on any statement that
grants an instance-changing action on instances without a condition.

What is still `Resource: "*"`: `RunInstances` and the other create
actions, which have no existing resource to scope to; the `Describe*`
actions, which support no resource-level permissions; and the deletes of
VPCs, subnets, security groups, key pairs, volumes, snapshots and AMIs in
every bundle but `asg-quorum-manager`. `ami-build`'s `CreateImage` and
`CreateSnapshot` can still read any instance's or volume's disk, though no
bundle can share the result outside the account. The account is a CI-only
account, which is what keeps those Low; the next step, if that changes, is
a dedicated account rather than more conditions.

### Applying this change

Editing these files changes nothing in AWS. Re-run the provisioning
scripts with every bundle the identity already has, because each run
replaces the policy wholesale:

```sh
scripts/ci-cloud-credentials/aws/provision-oidc-role.sh \
    --github-org crawlins --github-repo kythira \
    --bundles <every bundle the role already has>
# and, if a developer user exists:
scripts/ci-cloud-credentials/aws/provision-developer-user.sh \
    --bundles <every bundle the user already has>
```

Then dispatch one real-cloud run per bundle you use. A suite whose
instance lacks the expected tag fails its teardown with
`UnauthorizedOperation` on `TerminateInstances` (or on `CreateTags` for a
standalone retag); the fix is to tag the instance at launch, not to widen
the condition.

## Verifying setup worked

Trigger `.github/workflows/real-cloud-tests.yml` manually via
`workflow_dispatch` (Actions tab, or `gh workflow run real-cloud-tests.yml`)
with only the cheapest bundle enabled —
`aws_bundle_ca_cluster_rpc_tls: false`, `aws_bundle_ec2_quorum: false`,
`aws_bundle_ca_cluster: true` — and confirm the `aws` job's
"Configure AWS credentials (OIDC)" step succeeds (proves the trust policy
and OIDC provider are correct) and its `ca-cluster-node` `ctest` step passes
(proves the bundle's permissions are sufficient). A `--dry-run` pass of
either provisioning script is also a good pre-check before touching real
AWS state.

## Cost per run

Real EC2 instances, NAT Gateways, and EIPs are provisioned and torn down by
each test case. `tests/aws_quorum_manager_real_ec2_test.cpp` already prints
its own per-run `[aws-cost]` breakdown at teardown. Using the same
methodology as `doc/aws_acm_pca_test_cost_estimate.md`'s EC2 section
(on-demand ceiling; actual spend is typically 40-70% lower since the suite
prefers spot pricing where available):

| Bundle | Approx. cost per full run |
|---|---|
| `ca-cluster-node` (1 case, 3-node cluster + bastion, ~12 min) | ≈ $0.02 |
| `ca-cluster-node-rpc-tls` (same shape + NACL setup, which AWS doesn't bill for) | ≈ $0.02 |
| `ec2-quorum-manager` (10 cases, 3-9 node clusters + bastion, ~157 min total) | ≈ $0.10 - $0.30 |
| `asg-quorum-manager` (one to three `t3.micro`s per case, every ASG at desired capacity 0 between cases) | ≈ cents; not yet measured |
| `aws-ec2-launch-options` job, on `ec2-quorum-manager` (6 cases, a few instances each for minutes; `c5.large`/`c6g.medium` for the cluster placement group, else `t3.micro`/`t4g.micro`) | ≈ cents; not yet measured |

IAM itself (roles, policies, instance profiles, the OIDC provider) carries
no AWS charge. At the weekly `schedule` trigger with all three bundles
enabled, expect on the order of a few cents to ~$1.50/month.

## Tearing down

Deleting either IAM role is safe at any time — with `AWS_CI_ROLE_ARN`
pointing at a role that no longer exists, or the CI role's own policy
missing a permission, the workflow's fail-closed checks (Requirement 7)
produce a clear `::error::` and stop before attempting any AWS call that
would otherwise fail confusingly partway through a test.

```sh
# CI identity (also detaches the OIDC provider only if you delete that too;
# leaving the OIDC provider in place is harmless if you plan to re-provision
# the CI role later)
aws iam delete-role-policy --role-name kythira-ci-real-cloud-tests --policy-name kythira-ci-real-cloud-tests-policy
aws iam delete-role --role-name kythira-ci-real-cloud-tests

# Static quorum-test-node role (only if you provisioned it)
aws iam remove-role-from-instance-profile --instance-profile-name kythira-aws-quorum-test-node-profile --role-name kythira-aws-quorum-test-node-role
aws iam delete-instance-profile --instance-profile-name kythira-aws-quorum-test-node-profile
aws iam delete-role-policy --role-name kythira-aws-quorum-test-node-role --policy-name kythira-aws-quorum-test-node-policy
aws iam delete-role --role-name kythira-aws-quorum-test-node-role
```

Then unset the repository variables with `gh variable delete` (or just set
`REAL_CLOUD_TESTS_AWS_ENABLED` to `false` to disable without deleting
anything).

## Object-persistence bucket (cloud key-object persistence spec)

`provision-object-persistence-bucket.sh` creates the S3 bucket the
object-persistence real tier writes to. Separate from the identity scripts
above: buckets are operator-owned prerequisites, and the engine never
administers storage.

```sh
scripts/ci-cloud-credentials/aws/provision-object-persistence-bucket.sh \
    [--profile PROFILE] [--bucket NAME] [--region us-east-1]
```

Creates `kythira-ci-<account-id>` with public access blocked (all four
switches), SSE-S3 default encryption, and a lifecycle rule expiring objects
under `kythira-real-test/` after 7 days. Safe to re-run.

**Provisioned August 16, 2026:** `kythira-ci-827617851594` in `us-east-1`.

**Cost.** Effectively zero at rest — the suites write a handful of small
objects and delete them in teardown, and the lifecycle rule catches anything a
crashed run leaves behind. Request charges for these suites are a rounding
error against any storage minimum (S3 PUTs are ~$0.005/1,000). The real cost
warning in this spec is about *production* append rates, not about CI.

**Grant, and the CI switches.** The bucket alone is not enough — the CI role
needs the `object-persistence` bundle, which is
`policies/object-persistence.json`: get/put/delete on
`arn:aws:s3:::<bucket>/kythira-real-test/*` plus `ListBucket` on the bucket
itself, conditioned on that same prefix. Object operations only; no bucket
administration, and no access to anything outside the prefix.

```sh
# List EVERY bundle you want the role to keep: put-role-policy replaces the
# inline policy wholesale, so omitting a bundle revokes it.
scripts/ci-cloud-credentials/aws/provision-oidc-role.sh \
    --github-org crawlins --github-repo kythira \
    --bundles ec2-quorum-manager,ca-cluster-node,ca-cluster-node-rpc-tls,object-persistence \
    --bucket kythira-ci-827617851594
```

`--bucket` defaults to `kythira-ci-<account-id>`, which is the same name
`provision-object-persistence-bucket.sh` creates by default; the resolved
value is echoed so a mismatch between the two scripts shows up in the output
rather than as a 403 later.

Then:

```sh
gh variable set AWS_OBJECT_PERSISTENCE_BUCKET --body kythira-ci-827617851594
gh variable set REAL_CLOUD_TESTS_AWS_OBJECT_PERSISTENCE_ENABLED --body true
```

For a single run without touching the variables, use the
`aws_bundle_object_persistence` `workflow_dispatch` input.

The bundle runs **first** among the AWS job's bundles, ahead of the
real-EC2 ones: the job mints one-hour session credentials at the top and the
EC2 suites re-federate as they go, whereas this one takes those credentials
as they are. It also runs on both the x64 and arm64 matrix legs — it launches
nothing, so it costs the job no meaningful wall-clock.

## The `ami-build` bundle: what it leaves out

The bundle grants what `packer/ca_cluster_node/` needs and not the rest of
Packer's documented example policy. In particular it does **not** grant:

- `ec2:ModifyImageAttribute` / `ec2:ModifySnapshotAttribute`. These are how
  an AMI or snapshot is shared with another AWS account, and with
  `ec2:CreateImage` on any instance they would let CI copy any instance's
  disk out of the account. The template sets no `ami_users`,
  `snapshot_users` or `ami_description`, which are the only things Packer
  calls them for.
- `ec2:ModifyInstanceAttribute`. Together with `ec2:StopInstances` and the
  `ec2-quorum-manager` bundle's `ec2:StartInstances`, it would let CI
  rewrite any instance's user data and reboot it into that code. Packer
  calls it only for `ena_support` / `sriov_support`, which the template
  leaves unset.
- `ec2:GetPasswordData`, which only Windows builds use.

If a future template change needs one of these, Packer fails with
`UnauthorizedOperation` naming it; add it back then, conditioned as narrowly
as the action allows, rather than restoring the whole list.
