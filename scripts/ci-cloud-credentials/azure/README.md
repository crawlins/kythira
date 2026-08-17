# Azure real-cloud-tests setup

Sets up short-lived, OIDC-federated Azure credentials (Workload Identity
Federation — no client secret ever stored anywhere) for
`.github/workflows/real-cloud-tests.yml`'s `azure` job. See
[`../README.md`](../README.md) for the three-level toggle model and
service-bundle concept this document assumes.

## Prerequisites

- An Azure subscription, billing enabled, and a pre-existing resource group
  the test fixtures will provision VMs/VMSS instances/NICs into
  (`AZURE_TEST_RESOURCE_GROUP`). Unlike AWS's VPC (which the test fixture
  creates and destroys itself), this resource group is never created or
  deleted by anything in this repo — provision it once, out of band.
- A pre-existing VNet with three zonal subnets and an NSG (or let the
  `azure_quorum_manager_real_test` fixture's `AZURE_TEST_VNET_ID`/
  `AZURE_TEST_NSG_ID`/`AZURE_TEST_SUBNET_ID_ZONE{1,2,3}` env vars point at
  them — see that file's header comment).
- For the `key-vault` bundle: a pre-existing Key Vault + RSA key, and the
  corresponding CA certificate PEM. Key Vault's soft-delete-by-default and
  optional purge-protection make automated per-run vault lifecycle
  management unsuitable, so — unlike the quorum-manager VMs/VMSS instances —
  nothing in this repo ever creates or deletes the vault or key either.
- The `az` CLI installed and logged in **locally** (not in CI) as a user
  with Azure AD Application Administrator + User Access Administrator (or
  Owner) rights. These credentials are only ever used locally, once, by the
  operator running `provision-federated-identity.sh`. Neither CI nor that
  script's output grants CI itself any Azure AD write permission.
- The GitHub CLI (`gh`) installed and authenticated, with admin access to
  this repository (to set repository variables).
- `python3` on `PATH` (used internally to substitute placeholders in the
  bundle role-assignment fragments below — no third-party packages
  required).

## Why no custom role definitions, unlike AWS's custom IAM policy

AWS's bundles need a hand-written IAM policy because EC2's fine-grained
actions (`ec2:CreateVpc`, `ec2:RunInstances`, etc.) don't map onto any
single built-in AWS managed policy at the right scope. Azure's equivalent
permissions **do** map onto built-in RBAC roles at resource-group scope —
**Virtual Machine Contributor**, **Network Contributor**, and (for the
`key-vault` bundle, scoped to just the vault) **Key Vault Crypto User** — so
`policies/*.json` here are role-assignment *fragments* (role name + scope
template), not custom role definitions the way AWS's `policies/*.json` are
full IAM policy documents.

## Service bundles

| Bundle | CTest binary | Built-in roles (scoped to the resource group unless noted) |
|---|---|---|
| `quorum-manager` | `azure_quorum_manager_real_test` | Virtual Machine Contributor, Network Contributor |
| `key-vault` | `azure_key_vault_ca_provider_real_test` | Key Vault Crypto User (scoped to the vault only) |

## First-time setup

### 1. Provision the CI identity and role assignments

```sh
scripts/ci-cloud-credentials/azure/provision-federated-identity.sh \
    --github-org <org> --github-repo <repo> \
    --subscription-id <subscription-id> --resource-group <resource-group> \
    --bundles quorum-manager,key-vault \
    --key-vault-name <vault-name>
```

Pass only the bundles you actually want CI to be able to run. Creates (if
absent) the `kythira-ci-real-cloud-tests` Azure AD app registration + service
principal, a federated identity credential trusting
`repo:<org>/<repo>:ref:refs/heads/main` (no client secret), and the RBAC role
assignments for the given bundles. Run with `--dry-run` first to see the
exact `az` calls without making them. Safe to re-run — every step checks for
existing state first.

### 2. Set repository variables

The script prints the exact `gh variable set` commands to run — set
`AZURE_CI_CLIENT_ID`, `AZURE_CI_TENANT_ID`, `AZURE_CI_SUBSCRIPTION_ID`, and
`REAL_CLOUD_TESTS_AZURE_ENABLED`.

### 3. Set the per-bundle and test-fixture variables/secrets

- `REAL_CLOUD_TESTS_AZURE_QUORUM_MANAGER_ENABLED` / `REAL_CLOUD_TESTS_AZURE_KEY_VAULT_ENABLED`
  (repository variables) — per-bundle toggles.
- `AZURE_TEST_RESOURCE_GROUP`, `AZURE_TEST_VNET_ID`, `AZURE_TEST_NSG_ID`,
  `AZURE_TEST_SUBNET_ID_ZONE1`/`2`/`3` (repository variables) —
  `quorum-manager` bundle.
- `AZURE_TEST_KEY_VAULT_URL`, `AZURE_TEST_KEY_VAULT_KEY_NAME` (repository
  variables) and a repository secret holding the CA certificate PEM,
  written to a file the workflow points
  `AZURE_TEST_KEY_VAULT_CA_CERT_FILE` at — `key-vault` bundle.

## Monitoring-config test

The Azure Monitor monitoring-config test (`azure-monitoring` job;
`scripts/real-cloud-monitoring/azure-monitor.sh`; doc/TODO.md "Metrics
Backends") reuses the same federated CI identity but has its own toggle,
`REAL_CLOUD_TESTS_AZURE_MONITORING_ENABLED`, and needs two extra values:

- an **Application Insights resource** (create one once, any workspace-based
  resource is fine):
  - its connection string → repository secret
    `AZURE_MONITORING_CONNECTION_STRING` (it authorizes ingestion);
  - its Application ID (API Access blade) → repository variable
    `AZURE_MONITORING_APP_ID` (used for the query-side assertion);
- a role assignment letting the CI service principal *read* the resource
  (`Monitoring Reader` on it, or Reader on its resource group).

Cost per run is effectively zero (one custom metric datapoint); ingestion
into the resource is billed by volume, and nothing needs teardown.

## Object-persistence container (cloud key-object persistence spec)

`provision-object-persistence-container.sh` creates the storage account and
blob container the object-persistence real tier writes to.

```sh
scripts/ci-cloud-credentials/azure/provision-object-persistence-container.sh \
    [--account NAME] [--container NAME] [--grant-caller-data-role]
```

**Provisioned August 16, 2026:** account `kythirarealtestobj` (container
`kythira-raft`) in `kythira-realtest-rg`/`eastus`, **Standard_ZRS**, TLS 1.2
minimum, HTTPS only, public blob access disabled.

**Two things this script exists to stop you rediscovering:**

1. **`Standard_ZRS` is a durability decision, not a default.** Azure is the one
   provider whose "a 2xx write is durable" claim is account configuration the
   engine does not control. ZRS is the only mode documented as writing
   synchronously to all three zone replicas before returning success. LRS is
   single-datacenter; GRS's cross-region copy is asynchronous.
2. **Owner does not grant blob-data access.** A subscription Owner can create
   the account *and the container* and still not write a single blob —
   containers are management-plane resources. The script therefore probes the
   data plane with a real write/read/delete round trip rather than inferring
   success from container creation, and `--grant-caller-data-role` assigns
   `Storage Blob Data Contributor` on the account if that probe fails. RBAC is
   eventually consistent; propagation took ~45 s when this was written.

A subscription that has never held a storage account also has
`Microsoft.Storage` unregistered, and every storage call then fails with
`SubscriptionNotFound` — which reads like the subscription is gone. The script
registers it (one-time, free) and waits.

**Cost.** A ZRS StorageV2 account with a few kilobytes in it is cents per
month; the lifecycle of these tests is create-and-delete. ZRS costs more per
GB than LRS, which is irrelevant at this volume and is the point of the
choice.

**Still required before CI can use it:** `Storage Blob Data Contributor` for
the CI federated identity (`AZURE_CI_CLIENT_ID`), scoped to this account and
no wider.
