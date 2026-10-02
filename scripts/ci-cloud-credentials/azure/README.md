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
`repo:<org>/<repo>:environment:real-cloud-tests` (no client secret), and the RBAC role
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
- `AZURE_TEST_VMSS_NAME`, `AZURE_TEST_VMSS_AUTOMATIC_UPGRADE_NAME`,
  `AZURE_TEST_VMSS_VM_SIZE`, `AZURE_TEST_PPG_ID`,
  `AZURE_TEST_AVAILABILITY_SET_ID` (repository variables) — the placement
  fixtures the `quorum-manager` bundle's seven placement-dependent cases need. See [Placement fixtures](#placement-fixtures-quorum-manager-bundle);
  the job now **fails closed** if the first two are unset.
- `AZURE_TEST_KEY_VAULT_URL`, `AZURE_TEST_KEY_VAULT_KEY_NAME` (repository
  variables) and a repository secret holding the CA certificate PEM,
  written to a file the workflow points
  `AZURE_TEST_KEY_VAULT_CA_CERT_FILE` at — `key-vault` bundle.

## Token scopes and the five-minute client assertion

Under workload identity federation there is no client secret, so `az` presents
the GitHub-issued **client assertion** to AAD — and it must present it again for
every token **scope** it has not already cached. That assertion lives **five
minutes**, `azure/login@v2` fetches its OIDC token once at its own execution
time, and nothing refreshes it.

The consequence is counter-intuitive and cost a run: a bundle can fail to
authenticate *because it is the first to need a particular scope*, no matter how
recently another bundle succeeded. In run `36612317090` the quorum-manager
bundle passed off the ARM token cached at login, ran 731s, and the key-vault
suite then failed in **0.91s** with

```text
AADSTS700024: Client assertion is not within its valid time range.
```

thirteen minutes after a five-minute assertion. Key Vault's data plane is a
different scope (`https://vault.azure.net/.default`) than ARM, so
`AzureCliCredential` could not serve it from that cache;
`EnvironmentCredential` is always unavailable here, so the chain had nothing
left.

The job therefore **pre-warms every scope it can need**, immediately after
login, while the assertion is seconds old:

| Scope | Needed by |
|---|---|
| `https://management.azure.com/.default` | both quorum managers |
| `https://vault.azure.net/.default` | `azure_key_vault_ca_provider` |
| `https://storage.azure.com/.default` | `azure_blob_object_persistence` |

A bundle that needs a fourth scope adds a line to that step and nothing else.
The step fails the job if any scope cannot be cached, because a scope that
cannot be obtained seconds after login will not start working later, and finding
that out after a 700-second bundle is how this was discovered.

The targeted re-login before the key-vault bundle is kept as belt-and-braces —
it still helps if a token outlives its own one-hour lifetime rather than merely
being a scope nothing cached — but it is no longer the primary protection.

**Diagnosing the next one.** `ChainedTokenCredential`'s exception says only
`Failed to get token from ChainedTokenCredential.`, naming neither the
credential that failed nor why; the AADSTS code was only ever in `az`'s stderr.
Both real-Azure binaries now install an Azure SDK log listener at `Verbose`
(`tests/azure_sdk_log_fixture.hpp`), which is the level at which the chain
records a reason per source, so that reason reaches the test log instead of
being discarded.

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

## vCPU quota

`request-quota-increase.sh` raises the two Compute quotas the `azure` job
needs in `eastus`, and reports what Microsoft did with the request.

```sh
scripts/ci-cloud-credentials/azure/request-quota-increase.sh [--dry-run]
```

**Why it exists.** `azure_vm_quorum_manager_real_test` failed on every
scheduled run from August 24, 2026 on quota, not on code. Measured on run
34841010171 (September 14, 2026), one test case hit both ceilings in turn:
the spot attempt was refused by `LowPriorityCores` (limit 3, usage 2,
required +2), and the on-demand escalation it correctly fell back to was
refused by `Total Regional Cores` (limit 10, usage 10, required +2). The
suite provisions five 2-vCPU VMs per cluster, so a run consumes the entire
regional allowance before it asks for anything else.

The defaults ask for 20 and 10 rather than the 12 and 4 the 409s cite,
because `zone_outage_during_rolling_deployment` provisions replacements
while the originals are still being deleted — a run's peak is above its
steady state, and each request is a round trip through Microsoft.

Safe to re-run: a target at or below the current limit is reported and never
submitted, so running it with no arguments is also how to read the current
limits.

## Placement fixtures (quorum-manager bundle)

`provision-quorum-manager-fixtures.sh` creates the four placement targets the
`quorum-manager` bundle needs an operator to have made first: two Virtual
Machine Scale Sets, a Proximity Placement Group and an Availability Set.

```sh
scripts/ci-cloud-credentials/azure/provision-quorum-manager-fixtures.sh [--vm-size SIZE] [--dry-run]
```

**Why the tests cannot create them.** The scale set's model — SKU, image,
network, zones, spot priority, upgrade policy — is an operator input by
design, mirroring the AWS spec's launch template/mixed-instances policy being
out of scope for `aws_asg_quorum_manager_config`. `provision_node` only
changes `sku.capacity` and tags the instance that appears; it never touches
the model. So there is no code path that could have created these, and no
self-correcting failure when they were missing.

**Why this section exists.** Every real-cloud run up to and including
`36641739288` printed, five times, `Skipping: preflight failed or
AZURE_TEST_VMSS_NAME unset` followed by `did not check any assertions` — and
reported the quorum-manager bundle green off the VM cases alone. The variable
was never passed to the job. The workflow now fails closed instead, so the
same gap cannot reappear as a silent green.

It is **seven** cases, not five. Making the VMSS cases run (run `36718807855`)
put their output in front of a reader for the first time, and the same log
showed `placement_proximity_placement_group` and `placement_availability_set`
also checking no assertions — in the VM half that everyone, including the
handoff notes, treated as fully covered. `AZURE_TEST_PPG_ID` and
`AZURE_TEST_AVAILABILITY_SET_ID` were likewise in no workflow and no repository
variable. Fixing the visible instance of a fault is how you find out how many
instances it had.

**What the script creates**, both at capacity 0 in `AZURE_TEST_RESOURCE_GROUP`:

| Resource | Shape | Used by |
|---|---|---|
| `kythira-realtest-vmss` | Flexible, `Manual`, capacity 0 | the four VMSS scaling cases |
| `kythira-realtest-vmss-auto` | Flexible, `Automatic`, capacity 0 | `vmss_rejects_automatic_upgrade_mode`, which asserts the constructor refuses it (Requirement 10.3) |
| `kythira-realtest-ppg` | Standard PPG | `placement_proximity_placement_group` |
| `kythira-realtest-avset` | Aligned, 2 fault / 5 update domains | `placement_availability_set` |

The PPG and the availability set are free to hold — neither carries a charge of
its own, and only the VM a case places in them bills, for the length of that
case. The availability set must be **Aligned** (managed): an unmanaged set
cannot hold the managed-disk VMs this manager creates.

Tags go on the PPG and availability set in a **second** `az resource tag` call
rather than via `--tags` on the create. `az ppg create` parses `--tags` with the
newer shorthand syntax, which reads the colon in `kythira:fixture` as its own
separator and fails with `Shorthand Syntax Error: Redundant tail`, while
`az vmss create` accepts the identical string. Renaming the key to suit one
command's parser would have been the wrong fix.

Both scale sets are **Flexible** orchestration, and that is a hard requirement
of `azure_vmss_quorum_manager` rather than a preference. The manager identifies
each node by a `kythira:node-id` tag written onto the instance, and a **Uniform**
scale set's members cannot hold tags of their own — they only reflect the scale
set's, and every write is accepted, reports success, and applies nothing.
Measured against a real Uniform set: an ARM `PUT`, `az vmss update
--instance-id --set tags` and `az resource tag` each returned success and left
the tag absent, still absent 439 seconds later. Provisioning would look like it
worked and every later `assess_quorum` and `decommission_node` would fail to
find the node. The manager refuses a Uniform scale set at construction, and this
script refuses to reuse one, because orchestration mode cannot be changed in
place — a wrong one has to be deleted and recreated.

`POST .../delete` and `.../deallocate` still work under Flexible; they take the
member's VM *name* in `instanceIds`, since Flexible has no separate numeric
instance id. No `--disable-overprovision`: overprovisioning is Uniform-only.
No load balancer and no public IPs — the tests read the member's private IP off
its NIC.

**Cost.** Nothing between runs: an empty scale set is free, and only instances
bill. A run launches one instance per case for a few minutes. The script's
probe (scale to 1, wait for a running instance with a private IP, scale back to
0) exists because creating a set at capacity 0 proves nothing about whether
scaling it works — quota, SKU restrictions and zone capacity are only consulted
when an instance is actually placed.

**Quota.** Each VMSS case adds one instance of `AZURE_TEST_VMSS_VM_SIZE`
(2 vCPU at the default `Standard_D2s_v7`) to `Total Regional Cores` usage while
it runs. The cases are sequential and the VM cases have finished by then, so
this does not raise the run's peak above what
[vCPU quota](#vcpu-quota) already covers — but it does mean a run that was
scraping the ceiling now has one more claimant.

**Teardown.** Three layers, because none of them is sufficient alone: each case
decommissions its own node; the fixture's teardown deletes *every* instance in
the scale set and returns it to capacity 0 (a case that dies between the
capacity increment and the tagging PATCH leaves an untagged instance that the
next case's `provision_node` would adopt); and the job's `Audit for leaked VMSS
instances` / `Sweep VMSS instances that outlived teardown` steps cover a ctest
SIGKILL, which runs no destructor. The audit is separate from the VM one
because it catches what the VM audit cannot: a scale set left at non-zero
`sku.capacity` with no member launched yet — a leak that has not started billing
but will. (Under Flexible a leaked *member* is an ordinary VM, so the VM audit
sees that one too. This was the other way round under Uniform, whose members are
invisible to `az vm list` entirely.)

The sweep never deletes the scale sets themselves. They are tagged
`kythira:fixture=vmss-quorum-manager`, not `kythira:managed-by`, which is what
distinguishes an operator fixture that must survive from an instance that must
not.

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

**Grant, and the CI switches.** The container alone is not enough — the CI
federated identity (`AZURE_CI_CLIENT_ID`) needs `Storage Blob Data
Contributor`, which is the `object-persistence` bundle here. It is assigned at
**container** scope, not account scope: a data-plane role on the account would
also cover any other container that account later grows.

```sh
# List every bundle you want assigned; role assignments are additive, so
# re-running with a subset does NOT revoke the others (unlike AWS).
scripts/ci-cloud-credentials/azure/provision-federated-identity.sh \
    --github-org crawlins --github-repo kythira \
    --subscription-id <sub> --resource-group kythira-realtest-rg \
    --bundles quorum-manager,key-vault,object-persistence \
    --storage-account kythirarealtestobj --storage-container kythira-raft
```

The three `--storage-*` options default to what
`provision-object-persistence-container.sh` creates, and
`--storage-resource-group` defaults to `--resource-group`. The resolved scope
is printed before the assignment, because a typo in a container scope produces
an assignment that succeeds and grants nothing.

Then:

```sh
gh variable set AZURE_OBJECT_PERSISTENCE_ACCOUNT   --body kythirarealtestobj
gh variable set AZURE_OBJECT_PERSISTENCE_CONTAINER --body kythira-raft
gh variable set REAL_CLOUD_TESTS_AZURE_OBJECT_PERSISTENCE_ENABLED --body true
```

For a single run, the `azure_bundle_object_persistence` `workflow_dispatch`
input. The bundle runs immediately after `azure/login`, ahead of the other
two: the AAD session this job mints is short-lived and this suite finishes in
under a minute.

Remember point 2 above when the suite exits 77 or 403s — **Owner is not
blob-data access**, and that is the single most likely cause.
