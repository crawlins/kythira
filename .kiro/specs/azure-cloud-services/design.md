# Design Document

## Overview

This document describes the design for Kythira's Azure support: two
`quorum_manager<Q, NodeId, Address, GroupId>` implementations
(`GroupId = std::string`, an Azure placement identifier — a zone number, a
Proximity Placement Group name, or an Availability Set name) and one
`certificate_provider` implementation.

| Class | Mechanism | Best for |
|---|---|---|
| `azure_vm_quorum_manager` | Direct ARM `PUT`/`DELETE` on `Microsoft.Compute/virtualMachines` | Dev / staging, simple deployments |
| `azure_vmss_quorum_manager` | `sku.capacity` PATCH on a Virtual Machine Scale Set | Production (Spot, zone-balancing, model-based replacement) |
| `azure_key_vault_ca_provider` | CSR parsed/assembled locally; final signature via Key Vault `Sign` | Any `ca_service`/`ca_cluster_node` deployment wanting an HSM-protected CA key |

All three are header-only. The quorum managers are compiled behind
`#ifdef KYTHIRA_HAS_AZURE_SDK` (`azure-core-cpp` + `azure-identity-cpp`); the
CA provider is compiled behind `#ifdef KYTHIRA_HAS_AZURE_KEY_VAULT`
(`azure-security-keyvault-keys-cpp`), independently.

Unlike the AWS spec, there is no generated ARM management-plane client for
Compute/Network in the C++ SDK ecosystem. The quorum managers therefore build
and send ARM REST requests directly over `Azure::Core::Http::HttpPipeline`,
authenticated with a bearer token obtained from a `TokenCredential`, with
JSON bodies handled via `boost::json` (already a `vcpkg.json` dependency —
see `network_simulator`'s existing serializers). `azure_key_vault_ca_provider`
is the one component with access to a real generated SDK client
(`Azure::Security::KeyVault::Keys::KeyClient`) and uses it directly, the same
way `aws_acm_pca_provider` uses `Aws::ACMPCA::ACMPCAClient`.

## Architecture

```
quorum_manager concept (quorum_management.hpp)
  │
  ├── azure_vm_quorum_manager                  (include/raft/azure_vm_quorum_manager.hpp)
  │     ├── Azure::Core::Http::HttpPipeline (bearer-token authenticated)
  │     ├── assess_quorum  → GET .../virtualMachines/{name}/instanceView  (per node, bounded concurrency)
  │     ├── provision_node → PUT NIC, PUT VM, poll instanceView, read NIC private IP
  │     └── decommission_node → DELETE VM, DELETE NIC, poll LRO to completion
  │
  └── azure_vmss_quorum_manager                (include/raft/azure_vmss_quorum_manager.hpp)
        ├── Azure::Core::Http::HttpPipeline (shared client shape with azure_vm_quorum_manager)
        ├── assess_quorum  → GET .../virtualMachineScaleSets/{name}/virtualMachines?$expand=instanceView (one call per scale set)
        ├── provision_node → PATCH sku.capacity, poll instance list for new untagged running instance, PATCH instance tags
        └── decommission_node → find owning scale set + instanceId by tag scan, POST .../delete {instanceIds:[...]}

certificate_provider concept (certificate_provider.hpp)
  │
  └── azure_key_vault_ca_provider               (include/raft/azure_key_vault_ca_provider.hpp)
        ├── Azure::Security::KeyVault::Keys::KeyClient
        ├── root_certificate_pem → returns configured ca_certificate_pem (no network call)
        └── sign_csr → parse CSR + build TBSCertificate locally (shared code path with
                        certificate_authority::sign_csr) → KeyClient.Sign(digest) → assemble cert

Shared:
  azure_client_config (include/raft/azure_client_config.hpp) — subscription/resource group/location/credential
```

## Data Models

### ARM Tag Schema

Every VM (or VMSS instance) managed by either quorum manager carries six tags:

```
kythira:cluster     = {cluster_name}
kythira:node-id     = {node_id}              ; decimal string; see "NodeId derivation" below
kythira:group       = {group_id}             ; e.g. "2" (zone), a PPG name, or an AvSet name
kythira:managed-by  = kythira-azure-vm-quorum-manager | kythira-azure-vmss-quorum-manager
kythira:priority    = "spot" | "regular"
kythira:placement   = "availability-zone" | "proximity-placement-group" | "availability-set" | "none"
```

Unlike the AWS design (where `kythira:last-heartbeat` is written by the
kythira process itself and read by `assess_quorum` for application-level
liveness), Azure liveness in this spec is determined **solely** from
`instanceView` power state — there is no heartbeat tag and no
`heartbeat_timeout`/`heartbeat_grace_period` configuration field. This is a
deliberate scope reduction relative to the AWS spec's fully-elaborated
heartbeat design: it can be added later as a straightforward extension (an
additional tag write from the kythira process plus the same staleness check
already implemented for EC2) without changing any interface in this spec, but
is not required for the quorum manager to be useful, and keeping it out
shrinks the initial implementation surface substantially. Every acceptance
criterion in requirements.md that references "live"/"unreachable" is written
against power state alone for this reason.

### NodeId derivation — the mirror image of the AWS scheme

AWS's `aws_ec2_quorum_manager` derives `NodeId` *from* an AWS-assigned
identifier (the EC2 instance ID), because `RunInstances` chooses that ID and
hands it back. Azure's `PUT .../virtualMachines/{vmName}` requires the
*caller* to choose `vmName` up front — there is no "create, then learn the
name" step. This flips the derivation direction:

```
AWS:    RunInstances() → ec2_id (AWS-assigned)  --ec2_id_to_node_id()-->  NodeId
Azure:  next_node_id() (tag scan)  --node_id_to_vm_name()-->  vm_name  →  PUT .../virtualMachines/{vm_name}
```

`next_node_id()` lists every VM tagged `kythira:cluster = {cluster_name}`
(all power states, so a VM mid-deletion is still counted), reads each
`kythira:node-id` tag, and returns `max + 1` (or `1` when none exist) — this
is exactly `aws_asg_quorum_manager`'s bookkeeping style (tag-scan-based ID
assignment), reused here because it is the *only* option available, not a
fallback. `node_id_to_vm_name(nid) = "kythira-" + cluster_name + "-" +
std::to_string(nid)` and its inverse `vm_name_to_node_id(name)` (strip the
prefix, parse the trailing integer) are both pure string computations —
no ARM call needed for either direction once `next_node_id()` has run once
per `provision_node` call.

`azure_vmss_quorum_manager` cannot reuse a scale-set-local instance ID as
`NodeId` (unique only within one scale set, not across the several scale sets
one Raft cluster's placement groups use), so it reuses the exact same
cluster-wide tag-scan `next_node_id()` as `azure_vm_quorum_manager`, applying
the resulting tag via a `PATCH` on the specific VMSS instance rather than
encoding it into a resource name (VMSS instance names are Azure-assigned and
not renameable).

## Components and Interfaces

### 1. `include/raft/azure_client_config.hpp`

```cpp
#pragma once

#include <chrono>
#include <string>

#ifdef KYTHIRA_HAS_AZURE_SDK
#include <azure/core/credentials/credentials.hpp>
#include <memory>
#endif

namespace kythira {

struct azure_client_config {
    std::string subscription_id;
    std::string resource_group;
    std::string location;
    std::string arm_endpoint_override;   // empty = https://management.azure.com
    std::chrono::seconds api_timeout{30};

#ifdef KYTHIRA_HAS_AZURE_SDK
    std::shared_ptr<Azure::Core::Credentials::TokenCredential> credential;
#endif
};

}  // namespace kythira
```

When `credential` is `nullptr`, both quorum managers and the CA provider
build:

```cpp
auto make_default_credential_chain()
    -> std::shared_ptr<Azure::Core::Credentials::TokenCredential> {
    std::vector<std::shared_ptr<Azure::Core::Credentials::TokenCredential>> sources{
        std::make_shared<Azure::Identity::EnvironmentCredential>(),
        std::make_shared<Azure::Identity::ManagedIdentityCredential>(),
        std::make_shared<Azure::Identity::AzureCliCredential>(),
    };
    return std::make_shared<Azure::Identity::ChainedTokenCredential>(std::move(sources));
}
```

This is a small free function in `azure_client_config.hpp` (guarded by
`KYTHIRA_HAS_AZURE_SDK`), shared by both quorum managers. It is Kythira's
explicit stand-in for the `DefaultAzureCredential` type other language SDKs
ship but the C++ SDK does not.

### 2. `include/raft/azure_vm_quorum_manager.hpp`

#### Placement and priority types

```cpp
enum class azure_placement_kind : std::uint8_t {
    availability_zone,
    proximity_placement_group,
    availability_set,
};

struct azure_placement_config {
    azure_placement_kind kind{azure_placement_kind::availability_zone};
    std::string zone;          // "1"/"2"/"3", used when kind == availability_zone
    std::string resource_id;   // PPG or AvSet ARM resource ID, otherwise
};

enum class azure_vm_priority : std::uint8_t { regular, spot };

enum class azure_eviction_policy : std::uint8_t { deallocate, delete_vm };

struct azure_spot_options {
    double max_price{-1.0};    // -1.0 = uncapped below on-demand price (Azure's own sentinel)
    azure_eviction_policy eviction_policy{azure_eviction_policy::deallocate};
};

struct azure_image_reference {
    // Marketplace form (all four set) XOR shared_gallery_image_id (only it set).
    std::string publisher, offer, sku, version;
    std::string shared_gallery_image_id;
};
```

#### Configuration struct

```cpp
struct azure_vm_quorum_manager_config {
    azure_client_config azure{};
    std::string cluster_name;                                   // required
    azure_image_reference image_reference;                      // required
    std::string vm_size{"Standard_D2s_v5"};
    std::string admin_username{"kythira"};
    std::string ssh_public_key;
    std::map<std::string, std::string> subnet_id_by_group;       // GroupId → subnet ARM ID
    std::string network_security_group_id;
    std::uint16_t node_port{7000};
    std::string custom_data_template;
    desired_topology<std::string> topology{};
    std::map<std::string, azure_placement_config> placement_by_group{};
    azure_vm_priority priority{azure_vm_priority::regular};
    std::optional<azure_spot_options> spot_options{};
    std::chrono::seconds provision_timeout{300};
    std::chrono::milliseconds poll_interval{5000};
    std::map<std::string, std::string> extra_tags{};
};
```

#### Class sketch

```cpp
#ifdef KYTHIRA_HAS_AZURE_SDK

template<typename NodeId = std::uint64_t, typename Address = std::string>
requires kythira::node_id<NodeId>
class azure_vm_quorum_manager {
public:
    using node_id_type            = NodeId;
    using address_type            = Address;
    using placement_group_id_type = std::string;

    explicit azure_vm_quorum_manager(azure_vm_quorum_manager_config cfg);

    azure_vm_quorum_manager(const azure_vm_quorum_manager&)            = delete;
    azure_vm_quorum_manager& operator=(const azure_vm_quorum_manager&) = delete;
    azure_vm_quorum_manager(azure_vm_quorum_manager&&)                 = default;
    azure_vm_quorum_manager& operator=(azure_vm_quorum_manager&&)      = default;

    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>>;

    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>>;

    auto provision_node(std::string target_group, std::optional<NodeId> replacing)
        -> kythira::future_default<peer_info<NodeId, Address>>;

    auto decommission_node(const NodeId& node)
        -> kythira::future_default<void>;

    [[nodiscard]] auto topology() const -> kythira::desired_topology<std::string>;

    // Pure computations — no ARM call, unlike aws_ec2_quorum_manager's
    // ec2_id_to_node_id (which also needs none, but in the opposite direction).
    [[nodiscard]] auto node_id_to_vm_name(const NodeId&) const -> std::string;
    [[nodiscard]] auto vm_name_to_node_id(const std::string&) const -> std::optional<NodeId>;

private:
    azure_vm_quorum_manager_config _cfg;
    std::shared_ptr<Azure::Core::Http::HttpPipeline> _pipeline;
    std::string _arm_base;   // https://management.azure.com/subscriptions/{sub}/resourceGroups/{rg}

    [[nodiscard]] auto next_node_id() const -> NodeId;
    [[nodiscard]] auto arm_get(std::string_view path) const -> boost::json::value;
    [[nodiscard]] auto arm_put(std::string_view path, const boost::json::value& body) const
        -> boost::json::value;
    void arm_delete(std::string_view path) const;   // treats 404 as success
    void poll_lro(std::string_view async_operation_url, std::chrono::seconds timeout) const;
    [[nodiscard]] auto apply_placement_fields(boost::json::object& body,
                                              const std::string& target_group) const -> void;
    [[nodiscard]] auto apply_priority_fields(boost::json::object& body) const -> void;
    [[nodiscard]] auto render_custom_data(const NodeId&, const std::string& group) const
        -> std::string;
    static auto compute_quorum_status(std::size_t live, std::size_t total) -> quorum_status;
};

static_assert(quorum_manager<azure_vm_quorum_manager<std::uint64_t, std::string>,
                             std::uint64_t, std::string, std::string>);

#endif  // KYTHIRA_HAS_AZURE_SDK
```

#### `assess_quorum` sequence

```
1. If cluster.empty(): return healthy Future immediately (no ARM call).
2. Check fault point "raft/azure/vm/get_instance_view".
3. For each node_placement in cluster (bounded concurrency, e.g. 16 in flight):
     vm_name = node_id_to_vm_name(np.node_id)
     GET {arm_base}/providers/Microsoft.Compute/virtualMachines/{vm_name}/instanceView
     on 404               → unreachable
     on other HTTP error  → abort: whole assess_quorum call returns exceptional Future
     on 200:
       live = any(status.code == "PowerState/running" for status in body["statuses"])
4. Build live_map (node_id → bool), group_live (group_id → count) from step 3.
5. Build placement_group_health entries from _cfg.topology's groups.
6. Return quorum_health with status from compute_quorum_status(live_count, total).
```

Unlike `DescribeInstanceStatus` (one AWS API call covers an arbitrary batch of
instance IDs), ARM's `instanceView` is a per-resource `GET`. There is no
Azure Compute batch-read endpoint for arbitrary VM names across a resource
group in a single call that returns power state, so `assess_quorum` issues
one request per node, bounded by a small concurrency limit to avoid
overwhelming ARM's per-subscription throttling budget. This is the one place
where the Azure design is structurally less efficient than its AWS
counterpart; `azure_vmss_quorum_manager::assess_quorum` does not have this
limitation (VMSS's `?$expand=instanceView` list call *is* a batch read).

#### `provision_node` sequence

```
1. Check fault point "raft/azure/vm/create_vm".
2. Validate target_group in subnet_id_by_group → std::invalid_argument if absent.
3. new_id = next_node_id()
   vm_name = node_id_to_vm_name(new_id)
4. PUT {arm_base}/providers/Microsoft.Network/networkInterfaces/{vm_name}-nic
     { location, properties: { ipConfigurations: [{ subnet: {id: subnet_id_by_group[target_group]} }],
                                networkSecurityGroup: {id: nsg_id} (when non-empty) } }
   on failure → return exceptional Future (nothing else created yet).
5. Render custom_data_template: {NODE_ID}, {NODE_PORT}, {CLUSTER}, {GROUP} substituted; base64-encode.
6. body = {
     location, tags: (Requirement 5 AC 1-2),
     properties: {
       hardwareProfile: { vmSize },
       storageProfile:  { imageReference: image_reference-derived },
       osProfile:       { computerName: vm_name, adminUsername,
                           linuxConfiguration: { ssh: { publicKeys: [...] } } (when ssh_public_key set),
                           customData: <base64 from step 5> },
       networkProfile:  { networkInterfaces: [{ id: nic_id_from_step_4 }] },
     },
     zones: [...] OR properties.proximityPlacementGroup/availabilitySet   (apply_placement_fields, Req 16)
     priority/evictionPolicy/billingProfile.maxPrice                       (apply_priority_fields, Req 18)
   }
   PUT {arm_base}/providers/Microsoft.Compute/virtualMachines/{vm_name}  body
   on failure → best-effort DELETE the NIC from step 4, return exceptional Future.
7. Poll GET .../virtualMachines/{vm_name}/instanceView every poll_interval:
     PowerState/running present → break
     elapsed > provision_timeout → best-effort DELETE VM then NIC, return exceptional Future
8. Re-GET the NIC to read ipConfigurations[0].properties.privateIPAddress.
9. Return peer_info{new_id, "{private_ip}:{node_port}"}.
```

#### `decommission_node` sequence

```
1. Check fault point "raft/azure/vm/delete_vm".
2. vm_name = node_id_to_vm_name(node_id)   // pure computation, no ARM call
3. DELETE {arm_base}/providers/Microsoft.Compute/virtualMachines/{vm_name}
   404 → treat as success, skip to step 5 without polling.
   other failure → return exceptional Future (no NIC deletion attempted).
4. Poll the Azure-AsyncOperation URL from the 202 response until status == "Succeeded"
   or 30s elapse.
5. DELETE {arm_base}/providers/Microsoft.Network/networkInterfaces/{vm_name}-nic
   404 or any other failure → log to std::cerr, do not affect the returned Future.
6. Return resolved Future.
```

---

### 3. `include/raft/azure_vmss_quorum_manager.hpp`

#### Configuration struct

```cpp
struct azure_vmss_quorum_manager_config {
    azure_client_config azure{};
    std::string cluster_name;                          // required
    std::map<std::string, std::string> scale_set_by_group;   // required, ≥ 1 entry
    std::uint16_t node_port{7000};
    std::chrono::seconds provision_timeout{300};
    std::chrono::milliseconds poll_interval{5000};
    desired_topology<std::string> topology{};
};
```

Spot/regular priority, zones, and custom data are all configured on the
scale set's own model (its launch-template equivalent) at scale-set creation
time — an out-of-band operator action, exactly like the AWS spec's ASG
`launch template`/mixed-instances policy being out of scope for
`aws_asg_quorum_manager_config`. `provision_node` only changes `sku.capacity`
and tags the resulting instance; it does not touch the scale set's model.

#### Class sketch

```cpp
#ifdef KYTHIRA_HAS_AZURE_SDK

template<typename NodeId = std::uint64_t, typename Address = std::string>
requires kythira::node_id<NodeId>
class azure_vmss_quorum_manager {
public:
    using node_id_type            = NodeId;
    using address_type            = Address;
    using placement_group_id_type = std::string;

    explicit azure_vmss_quorum_manager(azure_vmss_quorum_manager_config cfg);

    azure_vmss_quorum_manager(const azure_vmss_quorum_manager&)            = delete;
    azure_vmss_quorum_manager& operator=(const azure_vmss_quorum_manager&) = delete;
    azure_vmss_quorum_manager(azure_vmss_quorum_manager&&)                 = default;
    azure_vmss_quorum_manager& operator=(azure_vmss_quorum_manager&&)      = default;

    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>>;

    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>>;

    auto provision_node(std::string target_group, std::optional<NodeId> replacing)
        -> kythira::future_default<peer_info<NodeId, Address>>;

    auto decommission_node(const NodeId& node)
        -> kythira::future_default<void>;

    [[nodiscard]] auto topology() const -> kythira::desired_topology<std::string>;

private:
    azure_vmss_quorum_manager_config _cfg;
    std::shared_ptr<Azure::Core::Http::HttpPipeline> _pipeline;
    std::string _arm_base;

    [[nodiscard]] auto next_node_id() const -> NodeId;
    [[nodiscard]] auto find_instance(const NodeId&) const
        -> std::optional<std::pair<std::string /*scale_set_name*/, std::string /*instanceId*/>>;
    static auto compute_quorum_status(std::size_t live, std::size_t total) -> quorum_status;
};

static_assert(quorum_manager<azure_vmss_quorum_manager<std::uint64_t, std::string>,
                             std::uint64_t, std::string, std::string>);

#endif  // KYTHIRA_HAS_AZURE_SDK
```

#### `assess_quorum` sequence

```
1. Check fault point "raft/azure/vmss/list_instances".
2. If cluster.empty(): return healthy Future immediately.
3. For each (group_id, scale_set_name) in scale_set_by_group:
     GET {arm_base}/.../virtualMachineScaleSets/{scale_set_name}/virtualMachines?$expand=instanceView
     on failure → return exceptional Future
     for each returned instance:
       node_id_tag = instance.tags["kythira:node-id"]  (absent → skip; belongs to no kythira cluster node yet)
       live = any(status.code == "PowerState/running" for status in instance.properties.instanceView.statuses)
       live_map[node_id_tag] = live
4. Iterate cluster vector; classify via live_map; build per-group health from _cfg.topology.
5. Return quorum_health.
```

#### `provision_node` sequence

```
1. Check fault point "raft/azure/vmss/update_capacity".
2. Validate target_group in scale_set_by_group → std::invalid_argument if absent.
3. scale_set_name = scale_set_by_group[target_group]
4. GET  .../virtualMachineScaleSets/{scale_set_name}  → orig_capacity = body.sku.capacity
5. PATCH .../virtualMachineScaleSets/{scale_set_name}  { sku: { capacity: orig_capacity + 1 } }
   on failure → return exceptional Future
6. Poll GET .../virtualMachineScaleSets/{scale_set_name}/virtualMachines?$expand=instanceView
   every poll_interval:
     find an instance with PowerState/running AND no "kythira:node-id" tag → break (found)
     elapsed > provision_timeout →
       PATCH sku.capacity back to orig_capacity (best-effort rollback)
       return exceptional Future
7. new_id = next_node_id()
8. PATCH .../virtualMachineScaleSets/{scale_set_name}/virtualMachines/{instanceId}
     { tags: (Requirement 5 AC 1-2, group = target_group) }
9. Read the instance's NIC private IP (via its networkProfile → the scale-set network config's
   referenced NIC for that instanceId).
10. Return peer_info{new_id, "{private_ip}:{node_port}"}.
```

The "no `kythira:node-id` tag yet" heuristic in step 6 mirrors
`aws_asg_quorum_manager::provision_node`'s identical technique for
identifying the newly launched instance among an ASG's members.

#### `decommission_node` sequence

```
1. Check fault point "raft/azure/vmss/delete_instance".
2. (scale_set_name, instance_id) = find_instance(node_id)   -- scans every
   scale_set_by_group entry's instance list for a matching kythira:node-id tag
3. if not found → return resolved Future (idempotent)
4. POST {arm_base}/.../virtualMachineScaleSets/{scale_set_name}/delete
     { "instanceIds": ["{instance_id}"] }
   "instance not found"-shaped error → return resolved Future (idempotent)
   other failure → return exceptional Future
5. Poll the scale set's instance list every 2s (30s budget) until instance_id no longer appears.
6. Return resolved Future.
```

Unlike AWS's `TerminateInstanceInAutoScalingGroup(ShouldDecrementDesiredCapacity=true)`,
VMSS's per-instance delete does not have a "also permanently lower the
capacity count" flag — deleting an instance ID leaves `sku.capacity` counting
a now-empty slot, freeing room for the next `provision_node` capacity
increment to land a genuinely new instance rather than colliding with a
phantom one. No corrective `PATCH` is needed after `decommission_node`; the
next `provision_node` call's capacity increment is exactly the corrective
action `maintain_quorum` already issues.

---

### 4. `include/raft/azure_key_vault_ca_provider.hpp` / `_impl.hpp`

Following the same public-declaration/impl-detail split
`aws_acm_pca_provider.hpp`/`aws_acm_pca_provider_impl.hpp` use:

```cpp
// azure_key_vault_ca_provider.hpp
#pragma once

#include <raft/azure_client_config.hpp>
#include <raft/certificate_provider.hpp>
#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>

#ifdef KYTHIRA_HAS_AZURE_KEY_VAULT

#include <chrono>
#include <string>

namespace raft::testing {

enum class azure_key_vault_signing_algorithm : std::uint8_t {
    rs256, rs384, rs512, ps256, es256, es384,
};

struct azure_key_vault_ca_provider_config {
    kythira::azure_client_config azure{};
    std::string vault_url;                 // required
    std::string key_name;                  // required
    std::string key_version;               // empty = latest
    std::string ca_certificate_pem;        // required
    azure_key_vault_signing_algorithm signing_algorithm{azure_key_vault_signing_algorithm::rs256};
    std::chrono::seconds validity{std::chrono::hours(24 * 30)};
};

class azure_key_vault_ca_provider {
public:
    explicit azure_key_vault_ca_provider(azure_key_vault_ca_provider_config config);

    [[nodiscard]] auto root_certificate_pem() -> kythira::future_default<std::string>;

    [[nodiscard]] auto sign_csr(std::string csr_pem, csr_signing_options options)
        -> kythira::future_default<pem_material>;

private:
    Azure::Security::KeyVault::Keys::KeyClient _client;
    azure_key_vault_ca_provider_config _config;
};

static_assert(certificate_provider<azure_key_vault_ca_provider>);

}  // namespace raft::testing

#endif  // KYTHIRA_HAS_AZURE_KEY_VAULT
```

#### `sign_csr` sequence

```
1. Check fault point "raft/azure/keyvault/sign".
2. Parse csr_pem (OpenSSL X509_REQ); verify its self-signature (proves the CSR
   holder controls the matching private key — the same check
   certificate_authority::sign_csr() already performs today).
3. Validate options against the parsed CSR's subject/SANs per the existing
   csr_signing_options contract (shared code path — see "Sharing the
   X.509-building code path" below).
4. Build the leaf TBSCertificate: issuer = ca_certificate_pem's subject,
   subject/SAN/keyUsage from options, validity window from config.validity,
   public key copied from the CSR. DER-encode it.
5. digest = hash(tbs_der) using the hash implied by config.signing_algorithm
   (SHA-256 for rs256/ps256/es256, SHA-384 for rs384/es384, SHA-512 for rs512).
6. result = _client.Sign(config.key_name, config.key_version, config.signing_algorithm, digest)
   on failure → return exceptional Future with the Key Vault error.
7. Assemble the final certificate: tbs_der + { algorithm identifier, result.Signature }
   → DER → PEM.
8. Return pem_material{ .certificate_pem = <PEM from step 7>, .private_key_pem = "" }.
```

#### Sharing the X.509-building code path

`certificate_authority::sign_csr()` (`certificate_authority.hpp`) already
implements CSR parsing, `csr_signing_options` validation, and TBSCertificate
assembly, but signs the result with an in-process `EVP_PKEY` immediately
afterward — there is currently no seam between "build the TBSCertificate" and
"sign it" in that class. Two implementation strategies are viable for step
2–4 above; this design recommends (a) as the minimal, non-invasive starting
point, with (b) as a natural follow-up once a second remote-signing backend
existed to justify the refactor:

- **(a) Duplicate the minimal subset.** `azure_key_vault_ca_provider` links
  against OpenSSL directly (already an unconditional-when-detected project
  dependency; see `certificate_authority`'s own `KYTHIRA_HAS_OPENSSL` gate)
  and reimplements only the CSR-parse-and-TBSCertificate-build steps it
  needs, calling the same `X509_REQ`/`X509`/`X509V3` OpenSSL APIs
  `certificate_authority.cpp` already uses. This duplicates a few hundred
  lines but touches no existing class.
- **(b) Extract a `tbs_certificate_builder` seam.** Refactor
  `certificate_authority::sign_csr()` into "build TBSCertificate" +
  "sign it with a pluggable signer" (a small
  `requires(S& s, std::span<const std::byte> digest) { s.sign(digest) ->
  std::vector<std::byte>; }`-shaped concept), so both the existing in-process
  path and `azure_key_vault_ca_provider` share one code path. This is more
  invasive (touches `certificate_authority.hpp`, a component with its own
  spec and existing test suite) and is deferred out of this spec's scope;
  tracked as a follow-up in tasks.md's Notes section.

This spec's tasks.md implements (a). Choosing (a) first and documenting (b)
as a known refactor opportunity matches this project's own precedent: the
stdexec Future backend spec explicitly scoped out converting existing call
sites in its first iteration (`README.md`'s stdexec section: "this feature
itself converted no existing production call site").

---

## Shared Private Helpers

`next_node_id`, `compute_quorum_status`, and the ARM HTTP helpers
(`arm_get`/`arm_put`/`arm_delete`/`poll_lro`) have near-identical signatures
and semantics across `azure_vm_quorum_manager` and `azure_vmss_quorum_manager`.
As with the AWS spec's equivalent helpers, they are NOT refactored into a
shared base class or header in this initial implementation — the small
duplication is intentional, matching the AWS design's own stated rationale
("If a third AWS manager is added in the future, a
`detail/aws_quorum_helpers.hpp` private header can absorb them"). A
`detail/azure_quorum_helpers.hpp` is the natural landing spot if a third
Azure-backed manager is ever added.

---

## Client Initialization and Authentication

Both quorum managers construct an `Azure::Core::Http::HttpPipeline` at
construction time, configured with:
- Base URL: `config.azure.arm_endpoint_override` when non-empty, else
  `https://management.azure.com`.
- A bearer-token authentication policy wrapping `config.azure.credential`
  (or the constructed `ChainedTokenCredential`, Requirement 2 AC 2),
  requesting the `https://management.azure.com/.default` scope.
- Retry policy: exponential backoff on `429`/`5xx`, matching the SDK's
  default `Azure::Core::Http::Policies::RetryPolicy` — no additional
  application-level retry logic is added, the same choice the AWS design
  makes for the AWS SDK's own retry policy.
- Per-call timeout from `config.azure.api_timeout`.

`azure_key_vault_ca_provider` constructs its `KeyClient` directly against
`config.vault_url` with the same credential resolution, requesting the
`https://vault.azure.net/.default` scope (Key Vault's own resource scope,
distinct from ARM's).

No global SDK init/shutdown call is required (unlike `Aws::InitAPI()`/
`Aws::ShutdownAPI()` in the AWS design) — the Azure SDK for C++ has no
process-wide initialization step.

---

## Fault Injection

```cpp
// azure_vm_quorum_manager::assess_quorum
fiu_do_on("raft/azure/vm/get_instance_view", throw std::runtime_error("injected"););

// azure_vm_quorum_manager::provision_node
fiu_do_on("raft/azure/vm/create_vm", throw std::runtime_error("injected"););

// azure_vm_quorum_manager::decommission_node
fiu_do_on("raft/azure/vm/delete_vm", throw std::runtime_error("injected"););

// azure_vmss_quorum_manager: list_instances / update_capacity / delete_instance — same pattern

// azure_key_vault_ca_provider::sign_csr
fiu_do_on("raft/azure/keyvault/sign", throw std::runtime_error("injected"););
```

All fault points are guarded by `#ifdef FIU_ENABLE` via the `fiu_do_on` macro
from `include/raft/fault_injection.hpp`, identical in structure to the AWS
implementations' fault points.

---

## Correctness Properties

### Property 1: Naming-based statelessness (mirror of AWS Property 1)
**Validates: Requirements 5.3, 8.1**

Both managers reconstruct the `NodeId` ↔ Azure-resource mapping without
in-memory state: `azure_vm_quorum_manager` via a pure name computation in
both directions (Requirement 5 AC 3), `azure_vmss_quorum_manager` via a tag
scan (`find_instance`). Neither requires surviving a process restart with
any local record of which `NodeId` maps to which Azure resource.

### Property 2: Idempotency of decommission (mirror of AWS Property 2)
**Validates: Requirements 8.2, 14.3**

`decommission_node` returns a resolved Future when the target resource does
not exist (404 for VMs; "not found" for a VMSS instance ID), matching the
AWS design's identical idempotency guarantee.

### Property 3: Monotonically increasing NodeId assignment (mirror of AWS Property 3)
**Validates: Requirements 5.4, 13.4**

`next_node_id()` scans all `kythira:node-id` tags across all power states
(including VMs mid-deletion) and returns `max + 1`, preventing a
recently-decommissioned node's ID from being reused while some Raft peer
might still reference it.

### Property 4: No autonomous replacement outside the quorum manager's control
**Validates: Requirements 10.3, 14.2**

`azure_vmss_quorum_manager`'s constructor rejects scale sets in
`upgradePolicy.mode == "Automatic"` (Requirement 10 AC 3), and
`decommission_node`'s per-instance delete does not trigger a scale-set
health-check-driven replacement (VMSS has no equivalent of an ASG health
check policy driving autonomous replacement the way EC2/ELB health checks
do) — so, as with AWS Property 4, only the quorum manager decides when a
replacement is provisioned.

### Property 5: No heartbeat dependency — pure infrastructure-layer liveness
**Validates: Requirements 6.3–6.4, 12.2–12.3**

Unlike the AWS design's Property 5 (application-level heartbeat detects a
crashed-but-still-`running` process), this spec's liveness signal is Azure
power state alone. A kythira process that crashes but leaves its VM in
`PowerState/running` is **not** detected as unreachable by this design — a
known, explicitly scoped limitation (see "ARM Tag Schema" above and
requirements.md Requirement 6). Extending this later to a heartbeat tag,
written by the kythira process and checked the same way the AWS design's
Property 5 already validates, is a compatible, additive follow-up.

### Property 6: Spot eviction requires no dedicated code path (mirror of AWS Property 6)
**Validates: Requirement 18.6**

An evicted Spot VM's power state transitions to `stopped`/`deallocated` (or
the resource disappears, for `evictionPolicy: delete`), which the existing
liveness rule already classifies as unreachable. No Spot-specific branch
exists in `assess_quorum` or `maintain_quorum`.

### Property 7: Topology-invariant replacement (mirror of AWS Property 7)
**Validates: Requirement 19.3**

`maintain_quorum` computes per-group deficits from `config.topology` and
provisions replacements into each deficit group independently, identical to
the AWS design's Property 7.

---

## Error Handling

- **ARM error responses**: ARM returns errors as JSON bodies
  (`{"error": {"code": ..., "message": ...}}`) with a non-2xx HTTP status.
  The implementations parse this body and wrap `code` + `message` in a
  `std::runtime_error` before rejecting the Future — the direct analogue of
  the AWS design's `outcome.GetError().GetMessage()` handling.
- **Long-running operations (LRO)**: ARM `PUT`/`DELETE` on compute resources
  is asynchronous — a `202 Accepted` with an `Azure-AsyncOperation` header
  is normal, not an error. `poll_lro` follows that URL until the operation
  resource reports `"status": "Succeeded"` (or `"Failed"`/`"Canceled"`, both
  treated as errors) or a timeout elapses. This has no AWS equivalent
  (`RunInstances`/`TerminateInstances` are synchronous acknowledgements); it
  is the main new error-handling shape this spec introduces relative to the
  AWS design.
- **Transient throttling**: ARM's `429 Too Many Requests` includes a
  `Retry-After` header; the SDK's default retry policy honors it. No
  additional application-level retry logic is added, matching the AWS
  design's choice not to duplicate the SDK's own retry handling.
- **Provisioning timeout**: When `provision_timeout` elapses, both managers
  perform best-effort cleanup (delete the VM and/or NIC, or roll back
  `sku.capacity`) before rejecting the Future, mirroring the AWS design's
  identical choice to avoid leaving orphaned billable resources.
- **Constructor validation**: Required-but-missing config fields throw
  `std::invalid_argument` synchronously at construction, before any ARM or
  Key Vault call is attempted — identical to the AWS design.

---

## Testing Strategy

### Unit tests (no Azure dependency)

Unit tests exercise constructor validation, config error paths, fault
injection points (Requirement 15), and `node_id_to_vm_name`/
`vm_name_to_node_id` round-tripping without any live subscription. Where a
test needs to verify the *shape* of a request (correct ARM URL, correct JSON
body, correct tag values) without a live subscription, a
`stub_http_transport_policy` — a small test-only
`Azure::Core::Http::Policies::HttpPolicy` implementation — is inserted at the
front of the pipeline's policy chain. It matches on URL/method and returns a
canned `Azure::Core::Http::RawResponse`, the same technique
`Azure::Core::Http::HttpPipeline`'s own test suite uses upstream, letting
these unit tests run with zero network access and zero credentials.

### No emulator tier

The AWS spec has three test tiers (unit, LocalStack, real-EC2) because
LocalStack credibly emulates EC2/ASG/IAM/S3/STS. No equivalent free/local
emulator exists for ARM Compute, VMSS, or Key Vault Keys as of this writing
(Azurite covers Storage and Cosmos DB only). This spec therefore has exactly
two tiers — unit (with the stub HTTP policy above) and real-Azure — and that
gap is a deliberate, documented scope decision (Requirement 20 AC 7), not an
oversight to be filled in later by porting the AWS spec's LocalStack fixture
pattern.

### Integration test fixture: `AzureIntegrationFixture`

Mirrors the AWS spec's `IntegrationFixture` shape (RAII setup/teardown,
UUID-tagged resources, credential pre-flight check causing a suite-wide
skip rather than a failure) but is considerably smaller because:
- The resource group itself is never created or deleted by the fixture
  (Azure resource groups are commonly pre-provisioned by a subscription's
  landing-zone tooling, and deleting one is a much higher-blast-radius
  action than deleting an AWS VPC the fixture created itself — this spec
  does not attempt it).
- There is no NAT Gateway/Internet Gateway/bastion-instance equivalent
  required: the fixture's own environment (a CI runner or developer machine
  with `az login` or a service principal) reaches ARM directly over the
  public internet without needing an SSH hop into the test subnet — the
  fixture never needs inbound connectivity to the VMs it creates, only
  ARM-level visibility (`instanceView`, tag reads), so no bastion is
  provisioned at all.
- Key Vault resources (vault + CA key) are never created or deleted by the
  fixture (Requirement 20 AC 9) — Key Vault's soft-delete-by-default and
  optional purge-protection make automated per-run vault lifecycle
  management unsuitable; operators pre-provision one vault + key and supply
  its coordinates via env vars, reused across all CA provider test runs.

Setup (only for the VM/VMSS suite; the CA provider suite needs none of this):

```
Credential pre-flight: GET .../resourceGroups/{AZURE_TEST_RESOURCE_GROUP} using the
                        configured credential chain; skip suite on any failure.
uuid, test_run = "kythira-test-" + uuid, cluster_name = "kythira-realtest-" + uuid

VNet:              use AZURE_TEST_VNET_ID if set, else
                        PUT Microsoft.Network/virtualNetworks/{test_run}-vnet
                        (addressSpace 10.88.0.0/16)
Subnets (zone1-3):  use AZURE_TEST_SUBNET_ID_ZONE{1,2,3} if set, else
                        PUT .../virtualNetworks/{vnet}/subnets/{test_run}-zone{n}
                        (10.88.{n}.0/24)
NSG:               use AZURE_TEST_NSG_ID if set, else
                        PUT Microsoft.Network/networkSecurityGroups/{test_run}-nsg
                        (inbound: port 7000 from 10.88.0.0/16)
```

Teardown (destructor, unconditional, best-effort, errors collected and
written to `std::cerr`, same pattern as the AWS design):

```
1. DELETE every VM/VMSS provisioned during the test; poll each LRO to completion
   (or 120s per resource, whichever first).
2. DELETE every NIC created for step 1's VMs.
3. DELETE the VMSS resource(s), if the fixture created any (not pre-existing
   scale sets referenced via env var).
4. DELETE any subnets created during setup.
5. DELETE the NSG, if created during setup.
6. DELETE the VNet, if created during setup.
```

The pre-existing resource group is never touched by teardown.

### Integration tests (real Azure)

Real-Azure tests provision actual VMs/VMSS instances and a real Key Vault
`Sign` call. `KYTHIRA_NODE_BINARY`'s bootstrap approach mirrors the AWS
design's `user_data_template`/S3-download pattern, adapted to
`customData`/a test storage account: the fixture uploads the kythira-node
binary to a test storage account (or reuses an operator-supplied blob URL)
and the rendered `custom_data_template` downloads and starts it, waits for
`localhost:{NODE_PORT}` to accept connections, discovers peers by listing
VMs/VMSS instances tagged `kythira:cluster = {CLUSTER}`, and tags itself
`kythira:status = ready` — functionally identical to the AWS bootstrap
script's steps, substituting Azure CLI/ARM calls for the AWS CLI ones.

### CI wiring (Requirement 21)

`.github/workflows/real-cloud-tests.yml`'s `azure` job (scaffolded as a
no-op by `.kiro/specs/ci-real-cloud-tests/`) is replaced with real steps
mirroring the `aws` job's shape: an `azure/login@v2` step using Workload
Identity Federation (no client secret), then a per-bundle `ctest -R`
invocation gated by `REAL_CLOUD_TESTS_AZURE_QUORUM_MANAGER_ENABLED` /
`REAL_CLOUD_TESTS_AZURE_KEY_VAULT_ENABLED`. `tests/azure_quorum_manager_real_test.cpp`
gains the same `TestCostReport`/`CostAccumulator`/`CostSummaryFixture`
apparatus `aws_real_ec2_test_support.hpp` already provides for AWS,
re-priced against Azure's published `AZURE_TEST_VM_SIZE` on-demand rate,
and the same signal-driven (`SIGTERM`/`SIGINT`/`SIGHUP`/`SIGQUIT`/`SIGPIPE`)
teardown-on-cancel handlers, so a canceled or killed CI run still tears
down VMs/VMSS instances rather than leaking them. `scripts/ci-cloud-credentials/azure/`
mirrors the AWS directory's shape (provisioning script, per-bundle
`policies/*.json` role-assignment fragments, `README.md`) — see
Requirement 21 for the full acceptance criteria.

---

## Dependencies

```
azure-core-cpp     ≥ 1.10   HTTP pipeline, credentials base types, retry policy
azure-identity-cpp ≥ 1.6    EnvironmentCredential, ManagedIdentityCredential,
                            AzureCliCredential, ChainedTokenCredential
                            find_package: find_package(azure-core-cpp CONFIG),
                                          find_package(azure-identity-cpp CONFIG)
                            Link: Azure::azure-core, Azure::azure-identity

azure-security-keyvault-keys-cpp ≥ 4.3   KeyClient, Sign/GetKey operations
                            find_package: find_package(azure-security-keyvault-keys-cpp CONFIG)
                            Link: Azure::azure-security-keyvault-keys

boost-json          (existing project dependency; see vcpkg.json)   ARM request/response bodies
```

`azure-core-cpp`/`azure-identity-cpp` back `KYTHIRA_HAS_AZURE_SDK`
(`azure_vm_quorum_manager`, `azure_vmss_quorum_manager`).
`azure-security-keyvault-keys-cpp` backs `KYTHIRA_HAS_AZURE_KEY_VAULT`
(`azure_key_vault_ca_provider`) independently — an environment with the core
SDK but not the Key Vault component builds everything except the CA
provider, matching the AWS SDK / AWS ACM Private CA independence
(Requirement 1 AC 3).
