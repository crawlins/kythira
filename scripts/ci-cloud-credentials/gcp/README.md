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
for existing state first. Optionally narrow trust to a single ref with
`--ref-restriction refs/heads/main`.

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

The real-GCE fixture creates its own VPC subnetworks, service account (if
`GCP_TEST_SERVICE_ACCOUNT` is unset), test instances/MIGs, and a Cloud Storage
object holding the uploaded `KYTHIRA_NODE_BINARY`; the real-CAS fixture creates a
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
