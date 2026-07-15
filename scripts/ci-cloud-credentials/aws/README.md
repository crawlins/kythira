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
`kythira-ci-real-cloud-tests` IAM role trusted only by
`repo:<org>/<repo>:*`, and an inline policy scoped to exactly the bundles
given. Prints the resulting role ARN and the exact `gh variable set`
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

## Adding or removing a bundle later

Re-run `provision-oidc-role.sh` with the full new `--bundles` list — it
replaces the CI role's policy content wholesale, so a bundle left out of a
later run genuinely loses that bundle's permissions, not merely stops using
them (verify with `aws iam get-role-policy --role-name
kythira-ci-real-cloud-tests --policy-name kythira-ci-real-cloud-tests-policy`
if you want to confirm). Then flip the corresponding
`REAL_CLOUD_TESTS_AWS_<BUNDLE>_ENABLED` repository variable with
`gh variable set`.

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
