# Requirements Document

## Introduction

This document specifies the requirements for Kythira's Azure support: two
`quorum_manager` implementations and one `certificate_provider` implementation,
mirroring the shape of the existing AWS support (`.kiro/specs/aws-quorum-manager/`,
`.kiro/specs/certificate-authority/`) closely enough that an engineer familiar
with the AWS classes can read this spec as a diff against them, but adapted
wherever Azure's control-plane shape genuinely differs from AWS's.

`azure_vm_quorum_manager` directly manages Azure Virtual Machines through the
Azure Resource Manager (ARM) `Microsoft.Compute/virtualMachines` REST API —
provisioning by calling `PUT .../virtualMachines/{vmName}`, assessing liveness
via `GET .../virtualMachines/{vmName}/instanceView`, and decommissioning via
`DELETE .../virtualMachines/{vmName}`. This is the closest Azure analogue to
`aws_ec2_quorum_manager` and `docker_quorum_manager`, and is the primary
implementation.

`azure_vmss_quorum_manager` manages one Virtual Machine Scale Set (VMSS) per
placement group. It drives cluster size by adjusting the scale set's
`sku.capacity` rather than creating individual VM resources directly. This is
the production-grade option: it inherits model-based instance replacement,
Spot/mixed-priority capacity, and zone-balancing from the scale set, while the
quorum manager retains control over *when* capacity changes occur and *which*
instance is decommissioned — the same division of responsibility
`aws_asg_quorum_manager` has with its Auto Scaling Group.

`azure_key_vault_ca_provider` is a `certificate_provider` (see
`include/raft/certificate_provider.hpp` and
`.kiro/specs/certificate-authority/`) that signs CSRs using a CA private key
held in Azure Key Vault as a **Key** (not a Key Vault *Certificate*). Unlike
AWS ACM Private CA — which accepts a CSR directly via `IssueCertificate` and
returns a finished certificate — Key Vault's certificate-issuance APIs are
built around Key Vault generating its own key material and either
self-signing or handing off to an integrated public CA (DigiCert/GlobalSign);
neither shape accepts an arbitrary external CSR as input. `certificate_provider`
requires exactly that shape (CSR in, certificate out — see
`certificate_provider.hpp`'s file comment: "the deliberate seam between local
and cloud issuance"), so `azure_key_vault_ca_provider` instead reuses
`certificate_authority`'s existing CSR-parsing and X.509-assembly logic and
substitutes only the final private-key signing operation with a call to Key
Vault's `Sign` REST operation (`POST /keys/{name}/{version}/sign`) against a
CA key that never leaves Key Vault (optionally HSM-protected). This keeps the
CA's private key out of process memory and disk entirely, the same guarantee
`ca_cluster_node`'s Raft-replicated ledger gives for *history*, applied here to
the *key material* instead.

The implementations live in:
- `include/raft/azure_client_config.hpp`
- `include/raft/azure_vm_quorum_manager.hpp`
- `include/raft/azure_vmss_quorum_manager.hpp`
- `include/raft/azure_key_vault_ca_provider.hpp` /
  `include/raft/azure_key_vault_ca_provider_impl.hpp`

They are compiled behind `#ifdef KYTHIRA_HAS_AZURE_SDK` (quorum managers) and
`#ifdef KYTHIRA_HAS_AZURE_KEY_VAULT` (CA provider), set by the build system
when the corresponding Azure SDK for C++ components are detected.

## Glossary

- **`azure_vm_quorum_manager`**: A `quorum_manager` implementation that manages
  individual Azure VM resources directly via the ARM Compute REST API.
- **`azure_vmss_quorum_manager`**: A `quorum_manager` implementation that
  drives one Virtual Machine Scale Set per placement group.
- **`azure_key_vault_ca_provider`**: A `certificate_provider` implementation
  that signs CSRs locally (parsing/X.509 assembly reused from
  `certificate_authority`) but performs the private-key signature via Azure
  Key Vault's `Sign` operation on a Key Vault-resident key.
- **placement group**: In this spec, a logical failure domain, expressed as one
  of three Azure placement primitives (see Requirement 16): an **Availability
  Zone** (`"1"`, `"2"`, `"3"` within a region), a **Proximity Placement Group**,
  or an **Availability Set**. Not to be confused with the AWS EC2 Placement
  Group concept from the AWS spec, though it fills the same role.
- **ARM (Azure Resource Manager)**: Azure's control-plane REST API, reached at
  `https://management.azure.com/subscriptions/{sub}/resourceGroups/{rg}/...`.
  Every create/read/update/delete operation in this spec is an ARM REST call.
- **resource group**: The ARM container scoping every resource
  (`cluster_name`'s Azure analogue for scoping, alongside tags). A single
  resource group is configured per manager instance; it is not created or
  deleted by the manager (an operator-provisioned prerequisite, same as an AWS
  VPC in the AWS spec's integration fixture).
- **kythira node ID**: The `NodeId` used at the Raft layer. Unlike AWS (where
  the EC2 instance ID is Azure/AWS-assigned and the `NodeId` is *derived* from
  it), Azure requires the caller to choose the VM resource name at creation
  time — there is no post-hoc "AWS gives you an ID, derive `NodeId` from it"
  step available for `azure_vm_quorum_manager`. The manager therefore
  *assigns* `NodeId` values itself (a monotonically increasing counter
  reconstructed by scanning the `kythira:node-id` tag across existing VMs in
  the resource group — the same `next_node_id()` scan-based approach
  `aws_asg_quorum_manager`'s tag bookkeeping uses) and *derives the VM resource
  name from it*: `kythira-{cluster_name}-{node_id}`. This is the mirror image
  of the AWS scheme but yields the same property — the `NodeId` ↔ Azure
  resource identity mapping is a pure computation in both directions, with no
  API call needed for either `node_id_to_vm_name` or `vm_name_to_node_id`.
- **`vmId`**: An immutable GUID Azure assigns to every VM (readable via
  `instanceView.vmId` or the VM resource's top-level `properties.vmId`). Not
  used for `NodeId` derivation (a 128-bit GUID does not fit a `uint64_t`
  `NodeId` losslessly the way AWS's 68-bit EC2 instance ID suffix does); used
  only as an idempotency/audit signal (Requirement 5.4).
- **instance ID (VMSS)**: The small, scale-set-scoped monotonic integer Azure
  assigns to each VMSS instance (e.g. `"0"`, `"1"`, `"2"`, distinct from
  `NodeId`). Unique only within one scale set, not across the scale sets of
  different placement groups sharing one Raft cluster — `NodeId` derivation for
  `azure_vmss_quorum_manager` therefore uses the same tag-scan scheme as
  `azure_vm_quorum_manager`, not the VMSS instance ID directly.
- **Azure SDK for C++**: `Azure::Core` (`azure-core-cpp`) and
  `Azure::Identity` (`azure-identity-cpp`) provide HTTP pipeline, retry policy,
  and credential (`TokenCredential`) primitives. There is no generated ARM
  management-plane client for Compute/VMSS in the C++ SDK (unlike the
  Python/Go/.NET/Java SDKs); `azure_vm_quorum_manager` and
  `azure_vmss_quorum_manager` therefore issue hand-built ARM REST requests
  over `Azure::Core::Http`, authenticated with a bearer token from a
  `TokenCredential`, with request/response bodies handled via `boost::json`
  (already a project dependency; see `vcpkg.json`). `Azure::Security::KeyVault::Keys`
  (`azure-security-keyvault-keys-cpp`) *is* a generated, supported client and is
  used as-is by `azure_key_vault_ca_provider`.
- **`DefaultAzureCredential`**-equivalent chain: `Azure::Identity` provides
  `EnvironmentCredential`, `ManagedIdentityCredential`, and
  `AzureCliCredential` individually; there is no single "default chain" type
  in the C++ SDK the way there is in .NET/Java/Python/Go. `azure_client_config`
  papers over this (Requirement 2).
- **`desired_topology`**: The `desired_topology<GroupId>` struct from
  `quorum_management.hpp`, expressing the target node count per placement
  group, reused unmodified from the AWS spec.

---

## Requirements

### Requirement 1: Build System Detection

**User Story:** As a developer building Kythira, I want the Azure
implementations to be compiled only when the required Azure SDK for C++
components are available so that the project builds cleanly on machines
without them.

#### Acceptance Criteria

1. `CMakeLists.txt` SHALL call
   `kythira_find_optional(AZURE_SDK azure-core-cpp CONFIG)` and
   `kythira_find_optional(AZURE_IDENTITY azure-identity-cpp CONFIG)` (both
   required together) and set `KYTHIRA_HAS_AZURE_SDK` only when both are
   found, following the same `kythira_find_optional`/`kythira_kconfig_gate`
   pattern the AWS SDK section already uses (`CMakeLists.txt` lines
   547–555).
2. `azure_vm_quorum_manager.hpp` and `azure_vmss_quorum_manager.hpp` SHALL be
   wrapped in `#ifdef KYTHIRA_HAS_AZURE_SDK` / `#endif` so they compile to
   nothing when either SDK component is absent.
3. `CMakeLists.txt` SHALL separately call
   `kythira_find_optional(AZURE_KEY_VAULT azure-security-keyvault-keys-cpp CONFIG)`
   and set `KYTHIRA_HAS_AZURE_KEY_VAULT`, independent of `KYTHIRA_HAS_AZURE_SDK`
   — an environment with the quorum managers but not Key Vault (or vice versa)
   still builds everything except the component it lacks, mirroring the
   `AWS_ACM_PCA`-independent-of-`AWS_SDK` pattern (`CMakeLists.txt` lines
   557–582).
4. `azure_key_vault_ca_provider.hpp` SHALL be wrapped in
   `#ifdef KYTHIRA_HAS_AZURE_KEY_VAULT` / `#endif`.
5. Both `KYTHIRA_HAS_AZURE_SDK` and `KYTHIRA_HAS_AZURE_KEY_VAULT` SHALL be
   propagated to consuming targets via `target_compile_definitions`.
6. `Kconfig` SHALL gain an `"Azure Integration"` menu (parallel to the
   existing `"AWS Integration"` menu) with `config AZURE_SDK` (default `y`,
   `depends on` nothing) and `config AZURE_KEY_VAULT` (default `y`,
   `depends on AZURE_SDK` for menu grouping only — the CMake detection
   itself is independent per AC 3).
7. `DEPENDENCIES.md` SHALL gain entries: `azure-core-cpp + azure-identity-cpp
   ≥ 1.10 — Azure VM and VMSS quorum managers` and
   `azure-security-keyvault-keys-cpp ≥ 4.3 — azure_key_vault_ca_provider`.

---

### Requirement 2: Shared Azure Configuration

**User Story:** As a library user, I want a single credential/subscription
configuration struct I can fill once and pass to any of the three Azure
components, rather than duplicating fields across each.

#### Acceptance Criteria

1. An `azure_client_config` struct SHALL be defined in
   `include/raft/azure_client_config.hpp` (compiled unconditionally) with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `subscription_id` | `std::string` | `""` | Azure subscription GUID; required at first use |
   | `resource_group` | `std::string` | `""` | ARM resource group scoping every managed resource; required |
   | `location` | `std::string` | `""` | Azure region (e.g. `"eastus2"`); required for `provision_node` |
   | `arm_endpoint_override` | `std::string` | `""` | Override the ARM base URL (e.g. an Azure Stack Hub or test double); empty = `https://management.azure.com` |
   | `api_timeout` | `std::chrono::seconds` | `30s` | Per-call timeout applied to the HTTP pipeline |
   | `credential` | `std::shared_ptr<Azure::Core::Credentials::TokenCredential>` | `nullptr` | Token credential; only available when `KYTHIRA_HAS_AZURE_SDK` (or `KYTHIRA_HAS_AZURE_KEY_VAULT`, for `azure_client_config` users that only need the CA provider) is defined |

2. When `credential` is `nullptr`, the implementations SHALL construct a
   `Azure::Identity::ChainedTokenCredential` composed of, in order:
   `EnvironmentCredential` (`$AZURE_TENANT_ID`/`$AZURE_CLIENT_ID`/
   `$AZURE_CLIENT_SECRET` or `$AZURE_CLIENT_CERTIFICATE_PATH`),
   `ManagedIdentityCredential` (system- or user-assigned, on an Azure VM/AKS
   pod), and `AzureCliCredential` (`az login` session, for local development).
   This chain is Kythira's equivalent of the other language SDKs'
   `DefaultAzureCredential`, built explicitly since `Azure::Identity` does not
   ship a single type with that name. When `credential` is non-null, it is
   used directly instead and the chain is never constructed.
3. `azure_client_config` SHALL be an aggregate (no user-declared
   constructors), matching `aws_client_config`.
4. There are no `client_id`, `client_secret`, or `tenant_id` fields on
   `azure_client_config` — those are supplied to the credential chain via
   environment variables (AC 2) or by constructing a credential type directly
   and passing it as `credential`, never as plaintext config fields. This
   mirrors `aws_client_config`'s decision to have no `access_key_id`/
   `secret_access_key` fields.

---

### Requirement 3: `azure_vm_quorum_manager` Configuration

**User Story:** As a library user deploying a multi-zone Kythira cluster on
Azure VMs, I want a configuration struct that captures all required VM
creation parameters so that I can construct the manager with a single
designated initializer and have the constructor validate the configuration.

#### Acceptance Criteria

1. An `azure_vm_quorum_manager_config` struct SHALL be defined with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `azure` | `azure_client_config` | `{}` | Subscription, resource group, location, credential |
   | `cluster_name` | `std::string` | *(required)* | Scope for the `kythira:cluster` tag and VM name prefix |
   | `image_reference` | `azure_image_reference` | *(required)* | Marketplace or shared-gallery image to deploy (see AC 2) |
   | `vm_size` | `std::string` | `"Standard_D2s_v5"` | Azure VM size (SKU) name |
   | `admin_username` | `std::string` | `"kythira"` | OS-profile admin account created on the VM |
   | `ssh_public_key` | `std::string` | `""` | SSH public key for `admin_username`; empty = password auth disallowed and no key provisioned (Linux images require at least one of the two) |
   | `subnet_id_by_group` | `std::map<std::string, std::string>` | `{}` | GroupId (zone/PPG/AvSet name) → ARM subnet resource ID used for the VM's NIC |
   | `network_security_group_id` | `std::string` | `""` | NSG resource ID attached to each provisioned NIC; empty = no NSG |
   | `node_port` | `std::uint16_t` | `7000` | Port the kythira process listens on |
   | `custom_data_template` | `std::string` | `""` | cloud-init/bash script, base64-encoded into `osProfile.customData`; supports `{NODE_ID}`, `{NODE_PORT}`, `{CLUSTER}`, `{GROUP}` substitutions |
   | `topology` | `desired_topology<std::string>` | `{}` | Target counts per placement group |
   | `placement_by_group` | `std::map<std::string, azure_placement_config>` | `{}` | GroupId → placement configuration (Requirement 16) |
   | `priority` | `azure_vm_priority` | `regular` | `regular` or `spot` (Requirement 18) |
   | `spot_options` | `std::optional<azure_spot_options>` | `std::nullopt` | Present only when `priority == spot` |
   | `provision_timeout` | `std::chrono::seconds` | `300s` | Max time to wait for the VM to reach `PowerState/running` |
   | `poll_interval` | `std::chrono::milliseconds` | `5000ms` | Interval between `instanceView` polls during provisioning |
   | `extra_tags` | `std::map<std::string, std::string>` | `{}` | Additional ARM tags applied to every managed VM |

2. An `azure_image_reference` struct SHALL be defined with fields
   `publisher`, `offer`, `sku`, `version` (Marketplace image four-part
   reference; `version = "latest"` is valid) OR a single `shared_gallery_image_id`
   field (mutually exclusive with the four Marketplace fields — the
   constructor throws `std::invalid_argument` if both forms are populated, or
   neither).
3. `azure_vm_quorum_manager` SHALL validate at construction time that
   `cluster_name`, `azure.subscription_id`, `azure.resource_group`, and
   `azure.location` are non-empty, `node_port` is non-zero, `image_reference`
   is well-formed per AC 2, and every group in `topology.groups` has a
   corresponding entry in `subnet_id_by_group`. Violations SHALL throw
   `std::invalid_argument`.

---

### Requirement 4: `azure_vm_quorum_manager` Class Interface

**User Story:** As a library user, I want `azure_vm_quorum_manager` to satisfy
`quorum_manager<azure_vm_quorum_manager<NodeId, Address>, NodeId, Address,
std::string>` so that I can wire it into a Raft node without any glue code.

#### Acceptance Criteria

1. `azure_vm_quorum_manager<NodeId, Address>` SHALL satisfy
   `quorum_manager<azure_vm_quorum_manager<NodeId, Address>, NodeId, Address,
   std::string>` with `placement_group_id_type = std::string`, verified by a
   `static_assert` in its header.
2. The class SHALL be defined in `include/raft/azure_vm_quorum_manager.hpp`.
3. The class SHALL be move-constructible and move-assignable; copy is
   deleted (it holds an `Azure::Core::Http::HttpPipeline` bound to a
   credential, which is non-copyable by the SDK's own design).
4. The class SHALL construct its HTTP pipeline and resolve a credential (per
   Requirement 2 AC 2) at construction time; no ARM call is made until the
   first `assess_quorum`/`provision_node`/`decommission_node` call.
5. The class SHALL provide `maintain_quorum(cluster)` returning
   `kythira::future_default<quorum_health<NodeId, std::string>>` with the
   same assess-then-remediate semantics as `aws_ec2_quorum_manager::maintain_quorum`
   (Requirement 19).

---

### Requirement 5: `azure_vm_quorum_manager` Node Naming and Tagging Scheme

**User Story:** As an operator, I want every managed VM to carry well-known
tags and a predictable name so that I can audit which VMs belong to a
cluster, and so that the quorum manager can reconstruct its `NodeId`
assignment after a process restart without keeping in-memory records.

#### Acceptance Criteria

1. Every VM created by `provision_node` SHALL be named
   `kythira-{cluster_name}-{node_id}` and SHALL carry these ARM tags:

   | Tag key | Value |
   |---|---|
   | `kythira:cluster` | `{cluster_name}` |
   | `kythira:node-id` | decimal string of the `NodeId` |
   | `kythira:group` | `{target_group}` |
   | `kythira:managed-by` | `kythira-azure-vm-quorum-manager` |
   | `kythira:priority` | `"spot"` when `config.priority == spot`; `"regular"` otherwise |

2. Any tags in `config.extra_tags` SHALL be merged in; they SHALL NOT
   override the five tags above.
3. Because the VM name is `kythira-{cluster_name}-{node_id}`, both
   `node_id_to_vm_name(nid)` and `vm_name_to_node_id(name)` SHALL be pure
   string computations (prefix concatenation / suffix parsing) requiring no
   ARM API call. This is the mirror image of
   `aws_ec2_quorum_manager::ec2_id_to_node_id`/`node_id_to_ec2_id`: AWS
   derives `NodeId` *from* a cloud-assigned identifier; Azure derives the
   *resource name* from a manager-assigned `NodeId`.
4. `NodeId` assignment (the "manager-assigned" half of AC 3) SHALL use a
   `next_node_id()` helper: list VMs via `GET
   .../virtualMachines?$filter=tagName eq 'kythira:cluster' and tagValue eq
   '{cluster_name}'` (all power states), parse every `kythira:node-id` tag,
   and return `max + 1` (or `1` when none exist). This is the same
   scan-and-increment strategy the AWS *design* document's earlier draft used
   for EC2 before the implementation switched to ID-derivation (see
   `.kiro/specs/aws-quorum-manager/design.md`'s "Shared Private Helpers"
   section) — Azure's caller-chosen-name requirement makes that strategy the
   natural (not fallback) choice here.
5. The VM's `vmId` (Glossary) SHALL be read back after `provision_node`
   succeeds and logged at debug level for audit purposes; it plays no role in
   `NodeId` derivation or lookup (Glossary).

---

### Requirement 6: `azure_vm_quorum_manager::assess_quorum`

**User Story:** As an orchestrator, I need `assess_quorum` to report which
nodes are live at the Azure infrastructure layer so that stopped or
deallocated VMs are detected without relying on an application-level
heartbeat.

#### Acceptance Criteria

1. `assess_quorum` SHALL accept the caller-supplied `cluster` vector (a list
   of `node_placement<NodeId, std::string>`). When the vector is empty, it
   SHALL return a healthy result immediately without making any ARM API
   call.
2. For each entry, `assess_quorum` SHALL call
   `GET .../virtualMachines/{node_id_to_vm_name(np.node_id)}/instanceView`.
   Calls SHALL be issued concurrently (bounded by a small fixed concurrency,
   e.g. 16 in-flight requests) rather than one ARM round-trip per node
   sequentially, since ARM has no multi-instance-view batch endpoint
   analogous to `DescribeInstanceStatus`.
3. A node is **live** when the `instanceView.statuses` array contains an
   entry with `code == "PowerState/running"`.
4. A node is **unreachable** when:
   - The `GET .../instanceView` call returns HTTP 404 (VM does not exist —
     e.g. already deleted), OR
   - `PowerState/running` is absent from `statuses` (`stopped`,
     `deallocated`, `starting`, or any other power state).
5. `quorum_status` SHALL be derived from the ratio of live to total nodes in
   the cluster vector, using the standard four-level mapping from
   `quorum_management.hpp`, applied globally.
6. Per-group health SHALL be computed from the `group_id` field of each
   `node_placement` in the cluster vector. The `target_count` for each group
   SHALL come from `config.topology`.
7. WHEN any `instanceView` call fails with a non-404 error (throttling,
   auth failure, network error) THEN `assess_quorum` SHALL return an
   exceptional Future — a single failed lookup aborts the whole assessment
   rather than silently treating that node as unreachable, since a 429/5xx
   is not evidence the node is actually down.
8. `assess_quorum` SHALL NOT modify any Azure resources.
9. `assess_quorum` SHALL check the fault injection point
   `"raft/azure/vm/get_instance_view"` before issuing any `instanceView` call.

---

### Requirement 7: `azure_vm_quorum_manager::provision_node`

**User Story:** As an orchestrator that has detected a degraded placement
group, I need `provision_node` to create a new Azure VM in the target group,
wait for it to reach the running power state, and return its kythira node ID
and address so that the node can join the cluster via the normal
`ClusterJoin` flow.

#### Acceptance Criteria

1. `provision_node(target_group, replacing)` SHALL call `next_node_id()`
   (Requirement 5 AC 4) to obtain `new_id`, then derive
   `vm_name = node_id_to_vm_name(new_id)`.
2. The new VM SHALL be attached to a NIC created in the subnet given by
   `config.subnet_id_by_group.at(target_group)`. When `target_group` is
   absent from `subnet_id_by_group` the Future SHALL be rejected with
   `std::invalid_argument` before any ARM call is made.
3. `provision_node` SHALL first `PUT` a network interface resource
   (`Microsoft.Network/networkInterfaces/{vm_name}-nic`) referencing the
   target subnet and, when `config.network_security_group_id` is non-empty,
   the configured NSG, then `PUT` the VM resource
   (`Microsoft.Compute/virtualMachines/{vm_name}`) with:
   - `location`: `config.azure.location`
   - `hardwareProfile.vmSize`: `config.vm_size`
   - `storageProfile.imageReference`: from `config.image_reference`
   - `osProfile.computerName`/`adminUsername`/`linuxConfiguration.ssh`: from
     `config.admin_username`/`config.ssh_public_key`
   - `osProfile.customData`: base64-encoded `config.custom_data_template`
     after placeholder substitution (`{NODE_ID}`, `{NODE_PORT}`, `{CLUSTER}`,
     `{GROUP}`)
   - `networkProfile.networkInterfaces`: the NIC created above
   - `zones` / `properties.proximityPlacementGroup` /
     `properties.availabilitySet`: per Requirement 16, based on
     `config.placement_by_group[target_group]`
   - `priority`/`evictionPolicy`/`billingProfile.maxPrice`: per
     Requirement 18, when `config.priority == spot`
   - `tags`: per Requirement 5 AC 1–2
4. WHEN the NIC or VM `PUT` fails THEN `provision_node` SHALL best-effort
   delete any resource that *did* get created in this call (NIC before VM
   creation was attempted, or the VM itself if the NIC succeeded but the VM
   `PUT` failed) and return an exceptional Future with the ARM error.
5. `provision_node` SHALL then poll
   `GET .../virtualMachines/{vm_name}/instanceView` at `config.poll_interval`
   intervals until `PowerState/running` appears or `config.provision_timeout`
   elapses.
6. Once running, `provision_node` SHALL read the VM's private IP address
   (via the NIC's `ipConfigurations[0].properties.privateIPAddress`,
   re-fetched after the VM reaches `running` — the address is not always
   populated at NIC-creation time on some subnet configurations) and return
   `peer_info{new_id, "{private_ip}:{node_port}"}`.
7. WHEN `provision_timeout` elapses before `running` state is reached THEN
   `provision_node` SHALL delete both the VM and its NIC (best-effort
   cleanup, errors logged to `std::cerr`, not propagated) and return an
   exceptional Future.
8. The `replacing` hint, when non-null, SHALL be logged for diagnostic
   purposes only; this implementation does not copy attributes from the
   replaced node.
9. `provision_node` SHALL check the fault injection point
   `"raft/azure/vm/create_vm"` before issuing the VM `PUT` request.

---

### Requirement 8: `azure_vm_quorum_manager::decommission_node`

**User Story:** As an orchestrator removing a broken node, I need
`decommission_node` to delete the VM (and its NIC) so that it cannot rejoin
and the subscription is not billed for a permanently broken node.

#### Acceptance Criteria

1. `decommission_node(node_id)` SHALL derive `vm_name =
   node_id_to_vm_name(node_id)` (a pure computation; no ARM call needed) and
   call `DELETE .../virtualMachines/{vm_name}`.
2. WHEN the `DELETE` call returns HTTP 404 (VM never existed or was already
   deleted) THEN `decommission_node` SHALL treat this as success (idempotent)
   and proceed to AC 3.
3. `decommission_node` SHALL then call
   `DELETE .../networkInterfaces/{vm_name}-nic`, also treating a 404 as
   success. NIC deletion failures for any other reason SHALL be logged to
   `std::cerr` but SHALL NOT cause the overall operation to report failure —
   an orphaned NIC is a minor cost/cleanup issue, not a correctness issue (it
   cannot rejoin the Raft cluster).
4. WHEN the VM `DELETE` fails for any reason other than 404 THEN
   `decommission_node` SHALL return an exceptional Future with the ARM error
   and SHALL NOT attempt NIC deletion.
5. After a successful VM `DELETE` (HTTP 202 Accepted, ARM's async pattern),
   `decommission_node` SHALL poll the ARM long-running-operation URL
   returned in the `Azure-AsyncOperation` response header until it reports
   `"status": "Succeeded"` or 30 seconds elapse, so that a subsequent
   `assess_quorum` call reliably sees the VM as unreachable (404) rather
   than observing a brief window where deletion is in progress but the
   resource still resolves.
6. `decommission_node` SHALL NOT remove the node from the Raft cluster
   configuration — that is done by the `remove_server()` / `ClusterLeave`
   path.
7. `decommission_node` SHALL check the fault injection point
   `"raft/azure/vm/delete_vm"` before issuing the VM `DELETE` request.

---

### Requirement 9: `azure_vm_quorum_manager::topology`

**User Story:** As an orchestrator, I need `topology()` to return the
desired node count per placement group so that I can compute per-group
deficits.

#### Acceptance Criteria

1. `topology()` SHALL return `config.topology` unmodified.
2. `topology()` SHALL be synchronous and make no ARM API calls.

---

### Requirement 10: `azure_vmss_quorum_manager` Configuration

**User Story:** As a library user running a production cluster, I want a
VMSS-backed quorum manager so that I can leverage scale-set instance
replacement, mixed Spot/regular capacity, and zone-balancing, while the
quorum manager retains authority over *when* new nodes are added or removed.

#### Acceptance Criteria

1. An `azure_vmss_quorum_manager_config` struct SHALL be defined with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `azure` | `azure_client_config` | `{}` | Subscription, resource group, location, credential |
   | `cluster_name` | `std::string` | *(required)* | Scope for tag filters; identifies which instances belong to this cluster |
   | `scale_set_by_group` | `std::map<std::string, std::string>` | *(required, ≥1 entry)* | GroupId → Virtual Machine Scale Set name |
   | `node_port` | `std::uint16_t` | `7000` | Port the kythira process listens on |
   | `provision_timeout` | `std::chrono::seconds` | `300s` | Max time to wait for a new instance to reach `PowerState/running` |
   | `poll_interval` | `std::chrono::milliseconds` | `5000ms` | Interval between VMSS instance-list polls during provision |
   | `topology` | `desired_topology<std::string>` | `{}` | Desired counts per group (used by `topology()` and validation only; scale sets are not resized at construction time) |

2. `azure_vmss_quorum_manager` SHALL validate at construction that
   `cluster_name` is non-empty, `scale_set_by_group` is non-empty, `node_port`
   is non-zero, and every group in `topology.groups` has a corresponding
   entry in `scale_set_by_group`. Violations SHALL throw
   `std::invalid_argument`.
3. At construction, the manager SHALL call
   `GET .../virtualMachineScaleSets/{name}` for every scale set in
   `scale_set_by_group` and verify `upgradePolicy.mode != "Automatic"`. If
   any scale set uses automatic OS/model upgrades, the constructor SHALL
   throw `std::invalid_argument`.

   **Rationale:** kythira assesses node liveness via `instanceView` power
   state (Requirement 12). Automatic-mode VMSS upgrades can roll instances in
   the background outside the quorum manager's remediation loop, causing a
   node kythira still considers live to be replaced without the manager's
   knowledge, and racing `maintain_quorum`'s own provision/decommission
   calls. Requiring `Manual` (or `Rolling`, operator-triggered) upgrade mode
   keeps membership changes solely under the quorum manager's control, the
   same rationale `aws_asg_quorum_manager`'s `HealthCheckType == "EC2"`
   check applies to ASG-driven replacement.

---

### Requirement 11: `azure_vmss_quorum_manager` Class Interface

**User Story:** As a library user, I want `azure_vmss_quorum_manager` to
satisfy the `quorum_manager` concept with the same NodeId/Address/GroupId
type parameters as `azure_vm_quorum_manager` so that the two classes are
interchangeable at the type level.

#### Acceptance Criteria

1. `azure_vmss_quorum_manager<NodeId, Address>` SHALL satisfy
   `quorum_manager<azure_vmss_quorum_manager<NodeId, Address>, NodeId,
   Address, std::string>` with `placement_group_id_type = std::string`,
   verified by a `static_assert` in its header.
2. The class SHALL be defined in `include/raft/azure_vmss_quorum_manager.hpp`.
3. The class SHALL be move-constructible; copy is deleted.
4. The class SHALL hold one shared `Azure::Core::Http::HttpPipeline` (there
   is no separate "VMSS client" vs "VM client" the way AWS has distinct
   `EC2Client`/`AutoScalingClient` types — both are `Microsoft.Compute` ARM
   resource types reached through the same pipeline).
5. The class SHALL provide `maintain_quorum(cluster)` returning
   `kythira::future_default<quorum_health<NodeId, std::string>>` per
   Requirement 19, with the same six-step assess-then-remediate sequence as
   `azure_vm_quorum_manager::maintain_quorum`.

---

### Requirement 12: `azure_vmss_quorum_manager::assess_quorum`

**User Story:** As an orchestrator, I need `assess_quorum` to determine
which kythira nodes are live at the Azure layer, using per-instance power
state rather than application-level heartbeats.

#### Acceptance Criteria

1. `assess_quorum` SHALL call
   `GET .../virtualMachineScaleSets/{name}/virtualMachines?$expand=instanceView`
   once per scale set in `scale_set_by_group` (not once per node — this is
   VMSS's batch equivalent of AWS's per-cluster `DescribeInstanceStatus`
   call), then match returned instances to the supplied `cluster` vector via
   each instance's `tags["kythira:node-id"]`.
2. A node is **live** when its matching VMSS instance's `instanceView`
   contains `PowerState/running` (same rule as Requirement 6 AC 3).
3. A node is **unreachable** when no VMSS instance in the response carries
   its `kythira:node-id` tag value, or the matching instance lacks
   `PowerState/running`.
4. When the cluster vector is empty, `assess_quorum` SHALL return a healthy
   result immediately without making any ARM API call.
5. `quorum_status` and per-group health SHALL be computed using the same
   rules as `azure_vm_quorum_manager::assess_quorum` (Requirement 6 AC 5–6).
6. WHEN any scale-set listing call fails THEN `assess_quorum` SHALL return
   an exceptional Future.
7. `assess_quorum` SHALL check the fault injection point
   `"raft/azure/vmss/list_instances"` before calling the listing endpoint.

---

### Requirement 13: `azure_vmss_quorum_manager::provision_node`

**User Story:** As an orchestrator, I need `provision_node` to increase the
target scale set's capacity by one, wait for the new instance to become
`running`, tag it with its assigned `NodeId`, and return that instance's
kythira node ID and address.

#### Acceptance Criteria

1. `provision_node(target_group, replacing)` SHALL identify the scale set
   name from `config.scale_set_by_group.at(target_group)`. When
   `target_group` is absent the Future SHALL be rejected with
   `std::invalid_argument`.
2. `provision_node` SHALL `GET .../virtualMachineScaleSets/{name}` to read
   the current `sku.capacity`, then `PATCH` the same resource with
   `sku.capacity = current + 1`.
3. After increasing capacity, `provision_node` SHALL poll
   `GET .../virtualMachineScaleSets/{name}/virtualMachines?$expand=instanceView`
   at `config.poll_interval` intervals until an instance with
   `PowerState/running` appears that does NOT yet carry a `kythira:node-id`
   tag, or until `config.provision_timeout` elapses.
4. Once found, `provision_node` SHALL call `next_node_id()` (the same
   cluster-wide tag scan as `azure_vm_quorum_manager`, since a VMSS instance
   ID is only unique within its own scale set — Glossary) and apply the tags
   from Requirement 5 AC 1 to that instance via
   `PATCH .../virtualMachineScaleSets/{name}/virtualMachines/{instanceId}`.
5. `provision_node` SHALL then read the instance's private IP address (via
   its `networkProfile`/NIC reference) and return
   `peer_info{new_node_id, "{private_ip}:{node_port}"}`.
6. WHEN `provision_timeout` elapses THEN `provision_node` SHALL `PATCH`
   `sku.capacity` back to its original value (best-effort rollback) and
   return an exceptional Future.
7. `provision_node` SHALL check the fault injection point
   `"raft/azure/vmss/update_capacity"` before issuing the capacity `PATCH`.

---

### Requirement 14: `azure_vmss_quorum_manager::decommission_node`

**User Story:** As an orchestrator, I need `decommission_node` to remove a
specific instance from its scale set, reducing capacity so the scale set
does not immediately replace it.

#### Acceptance Criteria

1. `decommission_node(node_id)` SHALL locate the owning scale set and
   instance ID by scanning `scale_set_by_group`'s scale sets for an instance
   whose `kythira:node-id` tag matches (there is no reversible computation
   here, unlike VM-name derivation — a decommissioned node's group is not
   otherwise known to this method — so this is the one lookup call
   `azure_vmss_quorum_manager` makes that `azure_vm_quorum_manager` does
   not need). WHEN no matching instance is found THEN `decommission_node`
   SHALL return a successfully-resolved Future (idempotent).
2. `decommission_node` SHALL call
   `POST .../virtualMachineScaleSets/{name}/delete` with
   `{"instanceIds": ["{instanceId}"]}`, which both removes the instance and
   implicitly reduces the scale set's effective running count by one — VMSS
   does not have a separate "decrement desired capacity" flag the way ASG's
   `TerminateInstanceInAutoScalingGroup` does; deleting a specific instance
   ID this way already leaves `sku.capacity` at its pre-deletion count of
   *slots*, of which one is now empty and available for the next
   `provision_node` capacity increment rather than being backfilled
   automatically.
3. WHEN the delete call fails with an error indicating the instance ID no
   longer exists THEN `decommission_node` SHALL return a
   successfully-resolved Future (idempotent).
4. WHEN the call fails for any other reason THEN `decommission_node` SHALL
   return an exceptional Future.
5. After a successful delete, `decommission_node` SHALL apply the same
   30-second consistency poll as `azure_vm_quorum_manager::decommission_node`
   (Requirement 8 AC 5), polling the scale set's instance list until the
   instance ID no longer appears.
6. `decommission_node` SHALL check the fault injection point
   `"raft/azure/vmss/delete_instance"` before issuing the delete call.

---

### Requirement 15: Fault Injection

**User Story:** As a developer writing chaos tests, I need fault injection
points in both managers and the Key Vault CA provider so that I can simulate
Azure API failures without actually interacting with Azure.

#### Acceptance Criteria

1. `azure_vm_quorum_manager::assess_quorum` SHALL check
   `"raft/azure/vm/get_instance_view"` before calling `instanceView`.
2. `azure_vm_quorum_manager::provision_node` SHALL check
   `"raft/azure/vm/create_vm"` before the VM `PUT`.
3. `azure_vm_quorum_manager::decommission_node` SHALL check
   `"raft/azure/vm/delete_vm"` before the VM `DELETE`.
4. `azure_vm_quorum_manager::maintain_quorum` SHALL check
   `"raft/azure/vm/maintain_quorum"` before executing the assessment step.
5. `azure_vmss_quorum_manager::assess_quorum` SHALL check
   `"raft/azure/vmss/list_instances"` before listing scale-set instances.
6. `azure_vmss_quorum_manager::provision_node` SHALL check
   `"raft/azure/vmss/update_capacity"` before the capacity `PATCH`.
7. `azure_vmss_quorum_manager::decommission_node` SHALL check
   `"raft/azure/vmss/delete_instance"` before the instance delete call.
8. `azure_vmss_quorum_manager::maintain_quorum` SHALL check
   `"raft/azure/vmss/maintain_quorum"` before executing the assessment step.
9. `azure_key_vault_ca_provider::sign_csr` SHALL check
   `"raft/azure/keyvault/sign"` before calling Key Vault's `Sign` operation.
10. `azure_key_vault_ca_provider::root_certificate_pem` SHALL check
    `"raft/azure/keyvault/get_key"` before calling Key Vault's `GetKey`
    operation.
11. All fault points SHALL compile to no-ops when `FIU_ENABLE` is not
    defined, using the `fiu_do_on()` macro from
    `include/raft/fault_injection.hpp`.

---

### Requirement 16: Azure Placement Support

**User Story:** As a library user running a latency-sensitive or HA Kythira
cluster on Azure, I want to specify per-group placement using whichever of
Azure's three placement primitives fits my topology, so that the quorum
manager places new nodes for optimal fault isolation or co-location.

#### Acceptance Criteria

1. An `azure_placement_kind` enum SHALL be defined with three values:

   | Value | ARM field set on the VM/VMSS | Notes |
   |---|---|---|
   | `availability_zone` | `zones: ["{zone}"]` | Spreads nodes across up to 3 physical datacenters in-region; the strongest isolation, and the direct analogue of the AWS spec's per-AZ `subnet_by_group` grouping |
   | `proximity_placement_group` | `properties.proximityPlacementGroup.id` | Co-locates instances on low-latency, physically-near hardware within a region; analogous to EC2's `cluster` placement-group strategy |
   | `availability_set` | `properties.availabilitySet.id` | Spreads instances across distinct fault domains and update domains within a single non-zonal region; used for regions without Availability Zone support |

2. An `azure_placement_config` struct SHALL be defined with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `kind` | `azure_placement_kind` | `availability_zone` | Which placement primitive applies |
   | `zone` | `std::string` | `""` | Zone number (`"1"`, `"2"`, `"3"`) when `kind == availability_zone` |
   | `resource_id` | `std::string` | `""` | Proximity Placement Group or Availability Set ARM resource ID, when `kind` is one of the other two values |

3. `azure_vm_quorum_manager_config`/`azure_vmss_quorum_manager_config` SHALL
   each contain `std::map<std::string, azure_placement_config>
   placement_by_group{}` mapping each kythira `GroupId` to its placement
   config. Absent entries mean no explicit placement constraint (Azure
   chooses freely within the resource group's region).
4. WHEN `placement_by_group[target_group].kind == availability_zone` THEN
   `provision_node` SHALL set `zones: [config.zone]` on the VM/VMSS create
   request.
5. WHEN `kind == proximity_placement_group` or `kind == availability_set`
   THEN `provision_node` SHALL set the corresponding `properties.*.id`
   reference field and SHALL NOT set `zones`.
6. WHEN `placement_by_group` has no entry for `target_group` THEN
   `provision_node` SHALL omit all placement fields.
7. The placement kind SHALL be recorded in the tag `kythira:placement` (value
   `"availability-zone"`, `"proximity-placement-group"`, `"availability-set"`,
   or `"none"`).
8. A unit test SHALL verify that constructing `azure_placement_config` with
   `kind = proximity_placement_group` and a non-empty `resource_id` correctly
   populates the struct (no validation error).

---

### Requirement 17: `azure_key_vault_ca_provider`

**User Story:** As an operator running a `ca_service`/`ca_cluster_node`
instance on Azure, I want the CA's signing key to live in Azure Key Vault
(optionally HSM-backed) rather than in process memory or on local disk, so
that a compromised host cannot exfiltrate the CA private key.

#### Acceptance Criteria

1. An `azure_key_vault_ca_provider_config` struct SHALL be defined with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `azure` | `azure_client_config` | `{}` | Subscription and credential (`resource_group`/`location` are unused by this component — Key Vault is addressed by vault URL, not ARM resource path) |
   | `vault_url` | `std::string` | *(required)* | e.g. `"https://my-vault.vault.azure.net"` |
   | `key_name` | `std::string` | *(required)* | Name of the pre-existing CA signing key in the vault |
   | `key_version` | `std::string` | `""` | Specific key version; empty = latest enabled version |
   | `ca_certificate_pem` | `std::string` | *(required)* | PEM of the CA's own certificate (containing the public key matching the Key Vault key); provisioning the CA cert itself is an out-of-band operator action, the same scoping decision `aws_acm_pca_provider` makes for the Private CA resource itself |
   | `signing_algorithm` | `azure_key_vault_signing_algorithm` | `rs256` | Maps to Key Vault's `SignatureAlgorithm` values (`RS256`, `RS384`, `RS512`, `PS256`, `ES256`, `ES384`, depending on the key's type/curve) |
   | `validity` | `std::chrono::seconds` | `30 days` | Issued-certificate validity period |

2. `azure_key_vault_ca_provider` SHALL satisfy `certificate_provider`
   (`include/raft/certificate_provider.hpp`), verified by a `static_assert`.
3. `root_certificate_pem()` SHALL return `config.ca_certificate_pem`
   directly (no Key Vault call needed — the certificate is supplied at
   construction, not fetched). Despite needing no network call, it SHALL
   still check the fault injection point `"raft/azure/keyvault/get_key"` and
   still return `kythira::future_default<std::string>`, matching the
   concept's required signature.
4. `sign_csr(csr_pem, options)` SHALL:
   a. Parse `csr_pem` and validate its signature and requested
      SAN/`options` exactly as `certificate_authority::sign_csr()` does
      today (same X.509/CSR-parsing code path, factored so both can share
      it — see design.md).
   b. Build the leaf certificate's TBSCertificate (issuer = subject of
      `config.ca_certificate_pem`, subject/SAN/validity from `options` and
      `config.validity`, public key from the CSR) exactly as
      `certificate_authority::sign_csr()` does, stopping short of the final
      signature.
   c. Compute the TBSCertificate's DER encoding and its digest per
      `config.signing_algorithm`.
   d. Call Key Vault's `Sign` operation (`POST
      /keys/{key_name}/{key_version}/sign`) with that digest, obtaining the
      raw signature bytes. The CA private key never leaves Key Vault; only a
      digest is sent and a signature returned.
   e. Assemble the final DER/PEM certificate from the TBSCertificate plus
      the returned signature, and return it as `pem_material` with
      `private_key_pem` left empty (per `pem_material`'s existing contract
      — "empty for `sign_csr()` results").
5. WHEN the Key Vault `Sign` call fails (auth failure, key not found, key
   disabled) THEN `sign_csr` SHALL return an exceptional Future with the Key
   Vault error message.
6. `azure_key_vault_ca_provider` SHALL NOT implement `revoke()` — CRL/OCSP
   publication is out of scope for this component, the same scoping
   decision `aws_acm_pca_provider` makes ("Requires the target CA to already
   have a CRL/OCSP configuration").
7. `ca_service` (`cmd/ca_service/`) SHALL gain a `--provider azure-key-vault`
   option alongside the existing `local`/`aws-acm-pca` options, taking
   `--key-vault-url`, `--key-vault-key-name`, and `--ca-cert-file` flags,
   following the same `any_certificate_provider` type-erasure pattern
   `ca_service` already uses to switch between `local_certificate_provider`
   and `aws_acm_pca_provider` (`cmd/ca_service/*.cpp`, `any_certificate_provider`
   class).

---

### Requirement 18: Azure Spot VM Support

**User Story:** As a library user running cost-sensitive, interruption-tolerant
cluster capacity on Azure, I want to launch nodes as Spot VMs so that I pay
the Azure Spot discount, accepting that Azure may evict the instance with
short notice.

#### Acceptance Criteria

1. An `azure_vm_priority` enum SHALL be defined with two values: `regular`
   (default) and `spot`.
2. An `azure_spot_options` struct SHALL be defined with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `max_price` | `double` | `-1.0` | Maximum hourly price in USD; `-1.0` means "up to the regular/on-demand price" (Azure's documented sentinel for "no cap below on-demand"), matching `RunInstances`' empty-`max_price` semantics in the AWS spec but expressed as Azure's own sentinel rather than an empty string |
   | `eviction_policy` | `azure_eviction_policy` | `deallocate` | `deallocate` (VM stopped, disk retained, billing for compute stops) or `delete` (VM and disks removed) |

3. `azure_vm_quorum_manager_config`/`azure_vmss_quorum_manager_config` SHALL
   each contain `azure_vm_priority priority{regular}` and
   `std::optional<azure_spot_options> spot_options{}`; `spot_options` SHALL
   be ignored (and MAY be validated as unset) when `priority == regular`.
4. WHEN `priority == spot` THEN `provision_node` SHALL set
   `priority: "Spot"`, `evictionPolicy` from `spot_options.eviction_policy`,
   and `billingProfile.maxPrice` from `spot_options.max_price` on the VM/VMSS
   create/update request.
5. The `kythira:priority` tag (Requirement 5 AC 1) SHALL record `"spot"` or
   `"regular"` to match the requested priority.
6. No special handling is required in `assess_quorum`/`maintain_quorum` for
   eviction: an evicted Spot VM's `instanceView` power state transitions
   away from `PowerState/running` (to `PowerState/stopped` for
   `deallocate`, or the VM disappears entirely for `delete`), which the
   existing liveness rule (Requirement 6 AC 3–4) already classifies as
   unreachable — mirroring Property 6 of the AWS spec's design
   (spot interruption needs no dedicated code path).

---

### Requirement 19: `maintain_quorum` Semantics

**User Story:** As an orchestrator, I need one call that assesses cluster
health, decommissions unreachable nodes, and provisions replacements to
restore the desired topology, matching the semantics
`aws_ec2_quorum_manager`/`aws_asg_quorum_manager` already provide.

#### Acceptance Criteria

1. `azure_vm_quorum_manager::maintain_quorum` and
   `azure_vmss_quorum_manager::maintain_quorum` SHALL each: call
   `assess_quorum(cluster)` (propagating any exceptional Future immediately);
   for each node in `unreachable_nodes`, call `decommission_node` (logging,
   not propagating, individual failures) while recording which group each
   belonged to; compute each group's deficit as `target_count − live_count`
   from `config.topology`; call `provision_node(group, replacing_hint)` the
   required number of times per group with deficit > 0 (logging, not
   propagating, individual failures); and return the pre-remediation
   `quorum_health` snapshot.
2. This mirrors `aws_ec2_quorum_manager::maintain_quorum`'s sequence exactly
   (`include/raft/aws_ec2_quorum_manager.hpp`, `maintain_quorum`); no Azure-
   specific deviation is required at this level, since the difference between
   the two clouds is fully absorbed by `assess_quorum`/`provision_node`/
   `decommission_node` beneath it.
3. Topology-invariant replacement (a group's deficit is always filled from
   that same group, never rebalanced across groups) SHALL hold, matching
   Property 7 of the AWS design.

---

### Requirement 20: Tests

**User Story:** As a developer, I need automated tests for both Azure
quorum managers and the Key Vault CA provider that can run without an Azure
subscription, plus optional integration tests that run against a real
subscription.

#### Acceptance Criteria

##### Concept satisfaction

1. `static_assert`s in each header SHALL verify: `quorum_manager` for
   `azure_vm_quorum_manager<std::uint64_t, std::string>` and
   `azure_vmss_quorum_manager<std::uint64_t, std::string>`; `certificate_provider`
   for `azure_key_vault_ca_provider`.

##### Unit tests (no Azure dependency)

2. A unit test file `tests/azure_quorum_manager_unit_test.cpp` SHALL be added
   and registered as CTest target `azure_quorum_manager_unit_test`, guarded
   by `if(KYTHIRA_HAS_AZURE_SDK)`, with labels `unit;raft;quorum;azure`.
3. Unit tests SHALL cover the same category of cases the AWS spec's
   Requirement 16 unit tests cover, adapted to Azure's fields: empty
   `cluster_name`/`resource_group`/`subscription_id`/`location` throws;
   `image_reference` with both or neither of the Marketplace/shared-gallery
   forms throws; missing `subnet_id_by_group`/`scale_set_by_group` entry for
   a topology group throws; `provision_node` with an unknown `target_group`
   returns an exceptional Future without any ARM call; the `node_id_to_vm_name`/
   `vm_name_to_node_id` round-trip is correct for representative IDs;
   `azure_placement_config` and `azure_spot_options` aggregate-initialize
   correctly (Requirements 16 AC 8, 18).
4. A unit test file `tests/azure_key_vault_ca_provider_unit_test.cpp` SHALL
   be added, guarded by `if(KYTHIRA_HAS_AZURE_KEY_VAULT)`, labels
   `unit;certificate_authority;ca;azure;keyvault`. It SHALL verify: the
   config validation in Requirement 17 AC 1 (missing `vault_url`/`key_name`/
   `ca_certificate_pem` throws); `root_certificate_pem()` returns the
   configured PEM unmodified; a `sign_csr` call against a CSR signed by a
   locally-generated test key produces a certificate that verifies against
   the CA cert when the test stubs the Key Vault `Sign` call (see AC 8).
5. Chaos (fault injection) unit tests SHALL verify that enabling each fault
   point from Requirement 15 causes the corresponding method to return an
   exceptional Future.

##### Test doubles for ARM/Key Vault calls

6. Because there is no Azure equivalent of LocalStack for the ARM Compute/
   Network control plane (Azurite emulates Storage/Cosmos DB only, not ARM
   Compute or Key Vault), unit-level HTTP-call verification SHALL use a
   `stub_http_transport_policy` inserted into the `Azure::Core::Http::HttpPipeline`
   (the SDK's own policy-chain extension point, analogous to how
   `Aws::Client::ClientConfiguration` in the AWS spec is not mocked but the
   *fault injection points* are exercised instead). The stub intercepts
   requests by URL pattern and returns canned JSON bodies/status codes,
   letting unit tests exercise request-building logic (correct ARM URL,
   correct JSON body fields, correct tag values) without a live subscription
   or an emulator. This applies to both the quorum manager unit tests (AC 2)
   and the Key Vault CA provider unit tests (AC 4).
7. There is intentionally no "integration test (emulator)" tier analogous
   to the AWS spec's LocalStack tier — Requirement 20 has exactly two tiers
   (unit, real-Azure), not three, because no accurate free/local emulator
   exists for ARM Compute, VMSS, or Key Vault Keys as of this writing. This
   is a deliberate scope decision, not an oversight.

##### Integration tests (real Azure)

8. Real-Azure integration tests SHALL be placed in
   `tests/azure_quorum_manager_real_test.cpp` (VM + VMSS managers) and
   `tests/azure_key_vault_ca_provider_real_test.cpp` (CA provider),
   guarded by `#ifdef KYTHIRA_AZURE_REAL_TESTS`, excluded from the default
   CTest run, labels `integration;azure;real-azure;slow`.
9. Real-Azure tests SHALL read the following env vars; the fixture SHALL
   skip (not fail) the entire suite when `AZURE_SUBSCRIPTION_ID` or
   `AZURE_TEST_RESOURCE_GROUP` is absent:

   | Variable | Required | Purpose |
   |---|---|---|
   | `AZURE_SUBSCRIPTION_ID` | Yes | Target subscription |
   | `AZURE_TEST_RESOURCE_GROUP` | Yes | Pre-existing resource group; never created or deleted by the fixture |
   | `AZURE_TEST_LOCATION` | No | Region (default `"eastus2"`) |
   | `AZURE_TEST_VNET_ID` / `AZURE_TEST_SUBNET_ID_ZONE1/2/3` | No | Pre-existing VNet/subnets; created if absent (mirroring the AWS spec's VPC/subnet auto-provisioning, Requirement 16 AC 10 in that spec) |
   | `AZURE_TEST_NSG_ID` | No | Pre-existing NSG; created if absent |
   | `AZURE_TEST_IMAGE_*` | No | Marketplace image publisher/offer/sku/version override; defaults to the latest Ubuntu LTS Gen2 image |
   | `AZURE_TEST_VM_SIZE` | No | Default `Standard_D2s_v5` |
   | `KYTHIRA_NODE_BINARY` | Yes (VM/VMSS suite only) | Local path to the kythira-node binary, uploaded to a test storage account for `customData` bootstrap to download |
   | `AZURE_TEST_KEY_VAULT_URL` / `AZURE_TEST_KEY_VAULT_KEY_NAME` | Yes (CA provider suite only) | Pre-existing vault and CA signing key; never created by the fixture, since Key Vault soft-delete/purge-protection makes vault lifecycle management unsuitable for per-test-run automation |

10. As the first action, before any resource creation, each fixture SHALL
    resolve a token via the configured credential chain and call
    `GET .../resourceGroups/{AZURE_TEST_RESOURCE_GROUP}` to confirm access.
    If this fails for any reason, the suite SHALL skip (mirroring the AWS
    spec's `sts:GetCallerIdentity` pre-flight check, Requirement 16 AC 9 in
    that spec).
11. Every resource the fixture creates SHALL be tagged
    `kythira:test-run = {uuid}` and named with the UUID, so concurrent runs
    do not collide and leaked resources are identifiable.
12. Fixture teardown (destructor, unconditional, best-effort, errors
    collected and written to `std::cerr`) SHALL delete, in order: VMs/VMSS
    instances, NICs, the VMSS resource (if created), subnets/VNet (only if
    created by this fixture), and the NSG (only if created). The pre-existing
    resource group itself is never deleted.
13. Real-Azure `azure_vm_quorum_manager` test cases SHALL cover the same
    scenario categories as the AWS spec's real-EC2 suite (Requirement 16 AC
    19 in that spec), adapted to Azure concepts: single-zone provision and
    assess; multi-zone topology; one node stopped externally (via
    `POST .../virtualMachines/{name}/deallocate`) → degraded; idempotent
    decommission; each of the three placement kinds (Requirement 16);
    provision-timeout cleanup; Spot provision/decommission
    (Requirement 18); and an availability-zone-outage scenario structurally
    equivalent to the AWS spec's `az_outage_during_rolling_deployment` (all
    instances in one zone deallocated plus one single-instance failure in
    another zone; verify `critical` then verify `maintain_quorum` restores
    `healthy` with topology-correct per-zone replacement).
14. A real-Azure `azure_key_vault_ca_provider` test case SHALL: generate a
    CSR via `generate_key_and_csr` (`certificate_provider.hpp`); call
    `sign_csr` against the real vault/key from AC 9; verify the returned
    certificate's signature validates against `ca_certificate_pem`'s public
    key using OpenSSL; and verify the fault-injection points from
    Requirement 15 AC 9–10 against the same real vault (enabling the fault
    point still short-circuits before the network call, so this does not
    require the vault to actually fail).

---

### Requirement 21: Documentation

**User Story:** As a contributor, I want the top-level README and
`DEPENDENCIES.md` updated so that Azure support is discoverable the same way
AWS support is.

#### Acceptance Criteria

1. `README.md`'s "What's In Progress" bullet listing "Additional cloud
   providers: Azure, GCP, OCI, and Alibaba Cloud" SHALL be updated once this
   spec is implemented to move Azure out of that bullet and into a new
   "What's Ready" bullet, following the same phrasing style as the existing
   AWS-related "What's Ready" bullets.
2. `README.md` SHALL gain a "Azure Quorum Managers & Certificate Provider"
   subsection (parallel structure to any future consolidated AWS subsection,
   or as a new top-level subsection if none exists yet) summarizing
   `azure_vm_quorum_manager`, `azure_vmss_quorum_manager`, and
   `azure_key_vault_ca_provider` at the same level of detail the existing
   Certificate Authority & ACME section gives AWS ACM Private CA.
3. `DEPENDENCIES.md` SHALL document the three Azure SDK components per
   Requirement 1 AC 7, following the existing AWS SDK / AWS ACM Private CA
   entries' format (Status / Purpose / Notes).
