# GCP real-cloud-tests setup

Sets up short-lived, Workload-Identity-Federated GCP credentials for
`.github/workflows/real-cloud-tests.yml`'s `gcp` job. This is the GCP analogue
of [`../aws/`](../aws/), using GCP's native federation mechanism (a Workload
Identity Pool + OIDC provider impersonating a dedicated service account) rather
than a long-lived service-account JSON key — no key is ever generated or stored
as a CI secret. See [`../README.md`](../README.md) for the three-level toggle
model and service-bundle concept this document assumes.

## Bundles

| Bundle | Binary gated | Toggle variable |
|---|---|---|
| `gcp-quorum-manager` | `gcp_quorum_manager_real_gce_test` (Compute Engine + MIG) | `REAL_CLOUD_TESTS_GCP_QUORUM_MANAGER_ENABLED` |
| `gcp-privateca` | `gcp_privateca_provider_real_test` (Certificate Authority Service) | `REAL_CLOUD_TESTS_GCP_PRIVATECA_ENABLED` |

Each bundle's IAM role bindings live in `policies/<bundle>.json`. The
provisioning script grants the service account exactly the bundles you pass to
`--bundles`.

## Prerequisites

- A GCP project with billing enabled.
- The `gcloud` CLI installed and authenticated **locally** (not in CI) as a
  principal with project IAM-admin permissions (`resourcemanager.projectIamAdmin`
  or `owner`), plus `iam.workloadIdentityPoolAdmin` and
  `iam.serviceAccountAdmin`. These credentials are only ever used locally, once,
  by the operator running the script — CI never receives them.
- The GitHub CLI (`gh`) installed and authenticated, with admin access to this
  repository (to set repository variables).
- `python3` on `PATH` (used to read the bundle policy JSON — no third-party
  packages required).

## First-time setup

### 1. Provision the CI identity

```sh
scripts/ci-cloud-credentials/gcp/provision-workload-identity.sh \
    --project <project-id> --github-org <org> --github-repo <repo> \
    --bundles gcp-quorum-manager,gcp-privateca
```

Pass only the bundles you want CI to be able to run — a bundle left out grants
the service account none of its permissions. Run with `--dry-run` first to see
the exact `gcloud` calls without making them. Safe to re-run; every step checks
for existing state first.

The provider accepts a token only when it comes from this repository **and**
from a job that declares `environment: real-cloud-tests` (`--environment` to
change the name), the same trust the AWS, Azure, OCI and Alibaba identities
use, so that environment's protection rules gate every use of the service
account. Re-running the script updates an existing provider's condition in
place.

`roles/iam.serviceAccountUser` (actAs) is bound only on the account named by
`--test-service-account`, the `GCP_TEST_SERVICE_ACCOUNT` the quorum-manager
fixture attaches to its instances, and skipped when that flag is absent: CI
leaves `GCP_TEST_SERVICE_ACCOUNT` unset, so its instances run with no service
account and need no actAs. The script also revokes the project-wide
`roles/iam.serviceAccountUser` and `roles/storage.admin` grants earlier
versions made: project-wide actAs let CI act as any service account in the
project, and storage.admin served a node-binary upload the fixture never
implemented.

The script prints the `gh variable set` commands to run next.

### 2. Set repository variables

The script emits these (fill in from its output):

```sh
gh variable set GCP_CI_WORKLOAD_IDENTITY_PROVIDER --body "projects/<num>/locations/global/workloadIdentityPools/kythira-ci-pool/providers/kythira-ci-github"
gh variable set GCP_CI_SERVICE_ACCOUNT           --body "kythira-ci-real-cloud-tests@<project>.iam.gserviceaccount.com"
gh variable set GCP_REAL_CLOUD_TESTS_PROJECT     --body "<project-id>"
# optional (defaults to us-central1):
# gh variable set GCP_REAL_CLOUD_TESTS_REGION    --body "us-central1"
```

### 3. Enable the toggles

```sh
gh variable set REAL_CLOUD_TESTS_ENABLED                    --body "true"
gh variable set REAL_CLOUD_TESTS_GCP_ENABLED               --body "true"
gh variable set REAL_CLOUD_TESTS_GCP_QUORUM_MANAGER_ENABLED --body "true"
gh variable set REAL_CLOUD_TESTS_GCP_PRIVATECA_ENABLED     --body "true"
```

## Monitoring-config test

The Cloud Monitoring monitoring-config test (`gcp-monitoring` job;
`scripts/real-cloud-monitoring/gcp-cloud-monitoring.sh`; doc/TODO.md
"Metrics Backends") reuses the same workload-identity CI service account
and project variables but has its own toggle,
`REAL_CLOUD_TESTS_GCP_MONITORING_ENABLED`. Grant the service account three
extra project-level roles (not part of any bundle above):
`roles/monitoring.metricWriter` and `roles/logging.logWriter` (the example
config's ingest path) plus `roles/monitoring.viewer` (the query-side
assertion). Cost per run is effectively zero — one
`workload.googleapis.com/kythira_ci_monitoring_probe` datapoint and one log
entry; nothing needs teardown.

## What the tests create (and clean up)

The real-GCE fixture creates its own VPC subnetworks and test instances
(with no service account attached unless `GCP_TEST_SERVICE_ACCOUNT` names one).
The MIG cases are the exception: they grow and shrink two long-lived groups
that `provision-quorum-manager-migs.sh` creates (see below);
the real-CAS fixture creates a
CA pool + self-signed root CA (if `GCP_TEST_CA_POOL` is unset). Every fixture
labels the resources it creates with `kythira-test-run=<run-id>` and tears them
down in reverse dependency order at the end, executing every teardown step
regardless of earlier failures and printing (not failing on) cleanup errors.
Resources supplied via env vars are used as-is and never deleted. Signal handlers
(`SIGTERM`/`SIGINT`/`SIGHUP`/`SIGQUIT`/`SIGPIPE`) run the active fixture's
teardown before re-raising, so a cancelled CI run still cleans up.

If credentials are missing or lack the required roles, the suites **skip** (they
do not fail): the fixture's first action is a read-only `projects.get`
pre-flight, mirroring the AWS `sts:GetCallerIdentity` pre-check.

## Object-persistence bucket (cloud key-object persistence spec)

`provision-object-persistence-bucket.sh` creates the GCS bucket the
object-persistence real tier writes to.

```sh
scripts/ci-cloud-credentials/gcp/provision-object-persistence-bucket.sh \
    [--bucket NAME] [--project ID] [--location us-central1]
```

**Provisioned August 16, 2026:** `kythira-ci-prefab-sky-500619-s9` in
`us-central1`, uniform bucket-level access, public access prevention
**enforced**, soft delete **off**, lifecycle expiring `kythira-real-test/`
after 7 days.

**Soft delete is disabled deliberately.** GCS defaults to a 7-day soft-delete
retention that bills deleted objects for a week — on a bucket whose entire
workload is create-and-delete test objects that is the dominant cost, and it
is invisible in a bucket listing. The design also takes "no dependence on
provider-native versioning or soft-delete" as a non-goal.

If the gcloud user credential has expired but application-default credentials
still work, the script runs unchanged with:

```sh
CLOUDSDK_AUTH_ACCESS_TOKEN="$(gcloud auth application-default print-access-token)" \
    scripts/ci-cloud-credentials/gcp/provision-object-persistence-bucket.sh
```

**Cost.** Effectively zero: a few small objects, deleted in teardown, with the
lifecycle rule as the backstop. GCS Class A operations are ~$0.005/1,000.

**Grant, and the CI switches.** The bucket alone is not enough — the CI
service account needs the `gcp-object-persistence` bundle, which binds
`roles/storage.objectUser` **on the bucket** rather than at project level.
`objectUser` rather than `objectAdmin`: the latter additionally carries
`storage.objects.setIamPolicy`, which this engine never calls. Bucket scope
rather than project scope because a project-level binding would also grant
object access to every other bucket in the project, including the ones the
real-GCE fixture creates for node binaries.

This is the only bundle in `policies/` that is not project-scoped, and the
policy entry says so with `"scope": "bucket"`; entries without that key mean
project, which is what every pre-existing entry meant.

```sh
scripts/ci-cloud-credentials/gcp/provision-workload-identity.sh \
    --project prefab-sky-500619-s9 \
    --github-org crawlins --github-repo kythira \
    --bundles gcp-quorum-manager,gcp-privateca,gcp-object-persistence \
    --object-persistence-bucket kythira-ci-prefab-sky-500619-s9
```

`--object-persistence-bucket` defaults to `kythira-ci-<project>`, the same
name the bucket script creates by default.

Then:

```sh
gh variable set GCP_OBJECT_PERSISTENCE_BUCKET --body kythira-ci-prefab-sky-500619-s9
gh variable set REAL_CLOUD_TESTS_GCP_OBJECT_PERSISTENCE_ENABLED --body true
```

For a single run, the `gcp_bundle_object_persistence` `workflow_dispatch`
input. The bundle runs before the two long ones: this job authenticates at the
top of the job, ahead of a build that can take an hour from cold, so the
shortest suite goes first while the WIF credentials are freshest.

**GCS rate-limits mutations of a single object to roughly 1/s**, where S3 took
the identical pattern unthrottled. The suite's latency case already spaces its
samples for this; it is recorded here because it is a GCS fact, not a
Kythira one, and the next person to write a GCS test will meet it again.

## Managed instance groups (`gcp_mig_real_gce` cases)

The six MIG cases in `tests/gcp_quorum_manager_real_gce_test.cpp` drive
existing groups rather than creating their own, and skip when
`GCP_TEST_MIG_A` or `GCP_TEST_MIG_AUTOHEAL` is unset. The quorum-manager
step fails on a skipped case, so the GCP job stays red until both are set.
Create the groups once:

```sh
scripts/ci-cloud-credentials/gcp/provision-quorum-manager-migs.sh --project <project-id>
gh variable set GCP_TEST_MIG_A        --body kythira-it-mig-a
gh variable set GCP_TEST_MIG_AUTOHEAL --body kythira-it-mig-autoheal
```

The script creates an `e2-micro` instance template with no external address
and no service account, a TCP health check, and two zonal MIGs at size 0 in
`<GCP_REAL_CLOUD_TESTS_REGION>-a`. None of these bill while idle.
`kythira-it-mig-autoheal` carries an autohealing policy only so the
construction case has a group to reject, and is never resized. Instances
are named `kythira-kythira-it-mig-*`, so the job's leak audit lists any
that a failed case leaves behind. The job then returns `kythira-it-mig-a`
to size 0 and still fails. The gcp-quorum-manager bundle's
`roles/compute.instanceAdmin.v1` already covers resizing the groups,
labelling their instances, and the `deleteInstances` call
(`compute.instanceGroupManagers.update`) that the timeout-rollback case
exercises.

## Diagnosing the `privateca` 403 (`probe-id-token.sh`)

`gcp_privateca_provider_real_test` has failed on every scheduled run since
September 14, 2026, seconds into its step, with an `actions-run-service` 403
reading `runner does not have permissions to generate id token`. **The message
names the wrong cause.** The `gcp` job does grant `permissions: id-token:
write`, and the same credentials succeed twice in the minutes before the
failure. Two fixes written from that line have already failed; do not write a
third from it.

What the step timings show is that both failures land ~85 minutes after **job
start**, while the exchanges that succeeded were at ~69 minutes of the same
jobs. Nothing there is measured from the `auth` step — which is the point,
because re-authenticating before the tests did not help. `auth` rewrites the
credential file, but the `credential_source` inside it carries
`ACTIONS_ID_TOKEN_REQUEST_TOKEN`, set by the runner once at job start and
refreshable by nothing.

`probe-id-token.sh` runs at three points in the `gcp` job — job start, after
the build, and between the two bundles — and prints, for each:

- the request token's own `iat`/`exp` claims, its total lifetime and its
  remaining lifetime;
- the live HTTP status of an ID-token exchange made right then, with the error
  body when it fails.

It never prints a token, and it never exits non-zero — a probe that can redden
an otherwise-green real-cloud run would be the fourth piece of machinery in
this repo to report a failure it did not find. When it cannot measure it says
`PROBE DID NOT RUN` rather than staying quiet, for the same reason the audits
do.

Read the three probes together. An expired or near-expired request token before
the privateca step confirms the lifetime hypothesis; a *working* exchange
immediately before a step that fails two seconds later refutes it and moves the
investigation into the test itself. Full write-up in `doc/TODO.md`, Known
Follow-ups.

Run it by hand only inside a job that grants `id-token: write`; there is no
local equivalent, because the two environment variables it reads exist only on
a runner.
