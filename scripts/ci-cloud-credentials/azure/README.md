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
