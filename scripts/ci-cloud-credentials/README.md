# CI real-cloud-tests credential provisioning

This directory holds the scripts and documentation for running Kythira's
real-cloud integration tests (as opposed to LocalStack/mocked variants) from
GitHub Actions, authenticated via short-lived, OIDC-federated credentials —
never a long-lived access key stored as a GitHub secret. See
`.github/workflows/real-cloud-tests.yml` for the workflow itself and
`.kiro/specs/ci-real-cloud-tests/` for the full requirements/design.

## Three-level toggle model

Whether any given real-cloud test actually runs is the AND of three
independent on/off levels, each a GitHub Actions repository variable with a
matching `workflow_dispatch` boolean input that overrides it for one manual
run only:

1. **Whole-feature**: `REAL_CLOUD_TESTS_ENABLED` — the master switch. Off by
   default; nothing in this workflow runs at all when this is false,
   regardless of the other two levels.
2. **Per-provider**: `REAL_CLOUD_TESTS_<PROVIDER>_ENABLED` (e.g.
   `REAL_CLOUD_TESTS_AWS_ENABLED`) — gates an entire provider's job.
3. **Per-service-bundle**: `REAL_CLOUD_TESTS_<PROVIDER>_<BUNDLE>_ENABLED`
   (e.g. `REAL_CLOUD_TESTS_AWS_EC2_QUORUM_ENABLED`) — gates one `ctest`
   invocation within a provider's job.

A repo admin can additionally require manual approval on top of all three
toggles via the `real-cloud-tests` GitHub Environment.

## Service bundles

A "bundle" is a named group of cloud-provider permissions mapped 1:1 to one
real-cloud CTest binary. Splitting by bundle rather than granting a
provider's CI role every permission every real-cloud test might ever need
means enabling one bundle never grants blast radius for another. AWS's three
bundles today:

| Bundle | CTest binary | What it needs |
|---|---|---|
| `ec2-quorum-manager` | `aws_quorum_manager_real_ec2_test` | Broad EC2 lifecycle + one scoped `iam:PassRole` |
| `ca-cluster-node` | `ca_cluster_node_real_ec2_test` | EC2 lifecycle only |
| `ca-cluster-node-rpc-tls` | `ca_cluster_node_rpc_tls_real_ec2_test` | EC2 lifecycle + Network ACL actions |

## Providers

| Provider | Status | Setup doc |
|---|---|---|
| AWS | Implemented | [`aws/README.md`](aws/README.md) |
| Azure | Not yet implemented; see `doc/TODO.md` Cloud Provider Support | — |
| GCP | Not yet implemented; see `doc/TODO.md` Cloud Provider Support | — |
| OCI | Not yet implemented; see `doc/TODO.md` Cloud Provider Support | — |
| Alibaba Cloud | Not yet implemented; see `doc/TODO.md` Cloud Provider Support | — |

## Why AWS needs two provisioning scripts, not one

Most bundles need only one script: `aws/provision-oidc-role.sh` creates the
CI identity itself (the OIDC-federated IAM role GitHub Actions assumes) with
permissions scoped to whichever bundles you pass it.

`ec2-quorum-manager` is the exception. The EC2 instances that test launches
need their own IAM identity (an instance profile), separate from the CI
role's identity. Rather than have the CI role (or the test, at run time)
dynamically create and destroy that instance-profile role — which would
require granting the CI role broad, risky IAM-write permissions
(`iam:CreateRole`, `iam:PutRolePolicy`, `iam:CreateInstanceProfile`, etc.) —
`aws/provision-quorum-test-node-role.sh` creates that role **once, in
advance, run by an operator with full IAM rights**. The CI role then only
ever needs `iam:PassRole` scoped to that one static role's ARN — it never
holds an IAM-write permission at all. See `aws/README.md` for the exact
setup order (the node role must exist before you provision the CI role, if
`ec2-quorum-manager` is one of the bundles you're enabling).
