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

The cloud-vendor **monitoring-config tests** (doc/TODO.md "Metrics
Backends"; `scripts/real-cloud-monitoring/`) are two-level rather than
three: each is a single-purpose job gated by the master switch plus its own
`REAL_CLOUD_TESTS_<PROVIDER>_MONITORING_ENABLED` toggle (with a matching
`<provider>_monitoring_enabled` dispatch input). There is no bundle layer
because each job runs exactly one test, and they sit outside the provider
test jobs because they build no C++ at all — a real OpenTelemetry Collector
(or, for OCI, the vendor's own Management Agent) runs the unmodified
example config from `docker/cloud-monitoring/` and the vendor's query API
confirms one synthetic metric arrived.

## Service bundles

A "bundle" is a named group of cloud-provider permissions mapped 1:1 to one
real-cloud test binary. Splitting by bundle rather than granting a
provider's CI role every permission every real-cloud test might ever need
means enabling one bundle never grants blast radius for another.

Most bundles name a CTest binary, selected by label. The
`object-persistence` family below does not: those suites are deliberately
never registered with CTest (a `ctest -N` listing that included them would
eventually run them, against a real bucket, for real money), so their jobs
invoke the binary by name. The toggle layering is identical either way.

AWS's bundles today:

| Bundle | CTest binary | What it needs |
|---|---|---|
| `ec2-quorum-manager` | `aws_quorum_manager_real_ec2_test` | Broad EC2 lifecycle + one scoped `iam:PassRole` |
| `ca-cluster-node` | `ca_cluster_node_real_ec2_test` | EC2 lifecycle only |
| `ca-cluster-node-rpc-tls` | `ca_cluster_node_rpc_tls_real_ec2_test` | EC2 lifecycle + Network ACL actions |
| `ami-build` | *(no dedicated ctest binary — runs `packer build`)* | EC2 instance lifecycle + AMI/snapshot creation ([`packer/ca_cluster_node/`](../../packer/ca_cluster_node/)) |
| `cloudwatch-monitoring` | *(no ctest binary — runs `scripts/real-cloud-monitoring/aws-cloudwatch.sh`)* | CloudWatch Logs ingest/read scoped to `/kythira/chaos-node/*` + `cloudwatch:ListMetrics` |
| `object-persistence` | `aws_s3_object_persistence_real_test` *(not CTest-registered)* | S3 object get/put/delete under `kythira-real-test/*` + prefix-conditioned `ListBucket`. **No bucket administration.** |

### The object-persistence bundle, on all five providers

One bundle per provider, one suite each, all running the same five checks
over a different `key_object_store`
(`.kiro/specs/cloud-object-persistence/`). Every one of them reads and
writes objects under a single prefix of a bucket an operator provisioned in
advance; none of them administers storage, and no policy here grants bucket
creation, deletion or configuration.

| Provider | Bundle | Toggle | Bucket/container variable | Grant |
|---|---|---|---|---|
| AWS | `object-persistence` | `REAL_CLOUD_TESTS_AWS_OBJECT_PERSISTENCE_ENABLED` | `AWS_OBJECT_PERSISTENCE_BUCKET` | inline policy, prefix-scoped |
| Azure | `object-persistence` | `REAL_CLOUD_TESTS_AZURE_OBJECT_PERSISTENCE_ENABLED` | `AZURE_OBJECT_PERSISTENCE_ACCOUNT` + `_CONTAINER` | `Storage Blob Data Contributor` at **container** scope |
| GCP | `gcp-object-persistence` | `REAL_CLOUD_TESTS_GCP_OBJECT_PERSISTENCE_ENABLED` | `GCP_OBJECT_PERSISTENCE_BUCKET` | `roles/storage.objectUser` bound **on the bucket**, not the project |
| OCI | `object-persistence` | `REAL_CLOUD_TESTS_OCI_OBJECT_PERSISTENCE_ENABLED` | `OCI_OBJECT_PERSISTENCE_BUCKET` | `manage objects in compartment` — see the file's `where`-clause warning |
| Alibaba | `oss-persistence` | `REAL_CLOUD_TESTS_ALIBABA_OSS_PERSISTENCE_ENABLED` | `ALIBABA_OSS_BUCKET` | RAM policy scoped to the bucket |

Every provider takes a `workflow_dispatch` input
(`<provider>_bundle_object_persistence`, and `alibaba_bundle_oss_persistence`)
for enabling the bundle for a single run without touching repository state.

**Passing an input is not the same as omitting one.** The expression is
`(input == 'true') || (input == null && vars.X == 'true')`, so an *omitted*
input falls back to the repository variable — and several of those are `true`
for bundles that launch real instances. A dispatch meant to run one bundle
must pass every other bundle an explicit `false`, not merely leave it out. On a
`schedule` event `github.event.inputs` is null entirely, so the weekly run
always reads the variables.

**Cost.** These are the cheapest bundles in this directory by a wide margin,
and the honest counterpart to the production cost warning in the spec's
Requirement 6. A full run of one provider's suite is on the order of a few
hundred object operations and a few hundred kilobytes held for the length of
the run — comfortably inside every provider's free tier for requests, and
rounding error against the storage minimums even outside it. The recurring
cost is the *bucket*, not the tests: an empty bucket is free or near-free
everywhere here, which is why each provisioning script sets a lifecycle rule
expiring the test prefix rather than relying on teardown alone. Two settings
that are cost decisions and are made in the scripts: GCS soft-delete is
**disabled** (its 7-day default bills deleted objects for a week, which on a
create-and-delete workload is the dominant line item), and the S3 lifecycle
rule aborts incomplete multipart uploads after a day.

By contrast, the quorum-manager bundles launch real instances. Nothing in
this family does.

## Providers

| Provider | Status | Setup doc |
|---|---|---|
| AWS | Implemented | [`aws/README.md`](aws/README.md) |
| Azure | Implemented | [`azure/README.md`](azure/README.md) |
| GCP | Implemented | [`gcp/README.md`](gcp/README.md) |
| OCI | Implemented (quorum/certificates/object-persistence suites + monitoring config) | [`oci/README.md`](oci/README.md) |
| Alibaba Cloud | Implemented (ESS quorum manager + OSS object persistence, both live-verified; no cert provider — `doc/TODO.md` Cloud Provider Support) | [`alibaba/README.md`](alibaba/README.md) |

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
