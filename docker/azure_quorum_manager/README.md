# Running kythira's Azure quorum managers

Setup for `kythira::azure_vm_quorum_manager`
(`include/raft/azure_vm_quorum_manager.hpp`) and
`kythira::azure_vmss_quorum_manager`
(`include/raft/azure_vmss_quorum_manager.hpp`). The companion
[`azure_quorum_manager.env.example`](azure_quorum_manager.env.example) lists
every configuration field; this file is about the Azure resources those
fields point at, which the library deliberately does **not** create.

Both are built when the Azure SDK is found (`KYTHIRA_HAS_AZURE_SDK`;
`azure-core-cpp` and `azure-identity-cpp`). They speak ARM REST through the
SDK's HTTP pipeline; there is no Compute SDK dependency.

## Which manager

| | `azure_vm_quorum_manager` | `azure_vmss_quorum_manager` |
|---|---|---|
| Provisions by | ARM `PUT` of a NIC and a VM | `PATCH` of the scale set's `sku.capacity` |
| Launch settings live in | the config (image, size, subnet, NSG, zone/PPG/AvSet, Spot, custom data) | the scale set's model |
| Node identity | Chosen by kythira, encoded in the VM name `kythira-<cluster>-<id>` | Chosen by kythira, written to the member's `kythira:node-id` tag |
| Best for | Dev, staging, simple deployments | Production: one place to change the image or size |

Both pick the next node id by scanning the resource group's
`kythira:node-id` tags (ARM makes the caller choose a VM name up front, so
the id cannot come from Azure the way it does on AWS), and both judge
liveness by `instanceView` power state. A running VM whose kythira process
has crashed still looks live to the manager; the Raft leader that owns it
catches that case instead, counting a voter that answers no RPC for
`quorum_peer_dead_after` as unreachable.

## What kythira does and does not create

| Resource | Created by |
|---|---|
| Resource group, VNet, subnet, NSG | **you** |
| Proximity Placement Group or Availability Set (optional, VM manager) | **you** |
| One Flexible scale set per zone (VMSS manager) | **you** |
| VMs, their NICs and OS disks, and their `kythira:*` tags | kythira |

The VM manager creates each OS disk with `deleteOption=Delete`, so
decommissioning a node does not orphan a billing disk.

## 1. Network

One resource group holds everything the manager touches. An Azure subnet
spans every zone in its region, so one subnet can serve all three groups;
`subnet_id_by_group` exists for deployments that want one per zone.

```sh
az group create -n kythira-prod -l eastus
az network vnet create -g kythira-prod -n kythira \
    --address-prefixes 10.30.0.0/16 --subnet-name nodes --subnet-prefixes 10.30.1.0/24
az network nsg create -g kythira-prod -n kythira-nodes
az network nsg rule create -g kythira-prod --nsg-name kythira-nodes -n raft \
    --priority 100 --protocol Tcp --destination-port-ranges 7000 \
    --source-address-prefixes VirtualNetwork --access Allow
```

## 2. For the VMSS manager: one Flexible scale set per zone

`scale_set_by_group` maps each topology group to a scale set. The
constructor refuses a scale set that:

- uses **Uniform** orchestration. A Uniform member cannot hold a tag of its
  own; ARM accepts the tag write, applies nothing, and every later lookup
  misses the node. Orchestration mode is immutable, so a Uniform set has to
  be deleted and recreated.
- has `upgradePolicy.mode=Automatic`, which replaces instances behind the
  manager's back.

Pin each scale set to one zone, so the group id means where the node lands.
Capacity 0 at rest costs nothing: the scale set itself is free, only its
members bill.

```sh
az vmss create -g kythira-prod -n kythira-prod-z1 -l eastus \
    --orchestration-mode Flexible --upgrade-policy-mode Manual \
    --instance-count 0 --zones 1 \
    --vm-sku Standard_D2s_v7 --image Canonical:0001-com-ubuntu-server-jammy:22_04-lts-gen2:latest \
    --admin-username kythira --ssh-key-values ~/.ssh/id_ed25519.pub \
    --subnet <subnet-id> --nsg <nsg-id> --load-balancer '' --priority Regular
# …and kythira-prod-z2 (--zones 2), kythira-prod-z3 (--zones 3).
```

`--load-balancer ''` matters: without it `az vmss create` builds a Standard
Load Balancer, which bills hourly whether or not it carries traffic.
`scripts/ci-cloud-credentials/azure/provision-quorum-manager-fixtures.sh` is
a commented, working example of all of the above.

## 3. Picking a VM size and image

The struct's default `vm_size`, `Standard_D2s_v5`, is restricted
(`NotAvailableForSubscription`) in some subscriptions, and most cheap
modern sizes are Hyper-V **Gen2-only**, so a Gen1 image fails the create
with `BadRequest`. Check both before you deploy:

```sh
az vm list-skus -l eastus --size Standard_D2s_v7 -o table   # empty or "NotAvailable…" = no
```

and pick a `-gen2` image SKU, as the example file does.

## 4. Role assignments and credentials

On the resource group: **Virtual Machine Contributor** (VMs, scale sets)
and **Network Contributor** (the VM manager creates and deletes NICs). This
is exactly what `scripts/ci-cloud-credentials/azure/policies/quorum-manager.json`
grants CI.

Virtual Machine Contributor also covers the VMSS manager's scale-in
protection write. A custom role needs
`Microsoft.Compute/virtualMachineScaleSets/virtualMachines/write` for it,
besides `Microsoft.Compute/virtualMachineScaleSets/delete/action`, which the
timeout rollback and `decommission_node` use to delete a member by name.
Without the member `write` action the constructor throws as soon as a scale
set holds an adopted member that is not protected.

Credentials come from `azure_client_config::credential`, or, left null,
`make_default_credential_chain()`: a service principal from
`AZURE_TENANT_ID`/`AZURE_CLIENT_ID`/`AZURE_CLIENT_SECRET`, then an
`az login` session. Managed identity is **not** in that chain, because
azure-identity-cpp 1.13.2's `ManagedIdentityCredential` segfaults when IMDS
is unreachable. On Azure-hosted compute, construct a
`ManagedIdentityCredential` yourself and set it in the config.

## 5. Worked example: three voters in three zones

```sh
cp azure_quorum_manager.env.example /etc/default/azure_quorum_manager
$EDITOR /etc/default/azure_quorum_manager   # subscription, RG, subnet, SSH key
chmod 0600 /etc/default/azure_quorum_manager
```

Then, in the loader that builds the config struct (the VM manager shown):

```cpp
#include <raft/azure_vm_quorum_manager.hpp>

const std::string subnet_id =
    "/subscriptions/00000000-0000-0000-0000-000000000000/resourceGroups/kythira-prod"
    "/providers/Microsoft.Network/virtualNetworks/kythira/subnets/nodes";
kythira::azure_vm_quorum_manager_config cfg{
    .azure = {.subscription_id = "00000000-0000-0000-0000-000000000000",
              .resource_group = "kythira-prod",
              .location = "eastus"},
    .cluster_name = "kythira-prod",
    .image_reference = {.publisher = "Canonical",
                        .offer = "0001-com-ubuntu-server-jammy",
                        .sku = "22_04-lts-gen2",
                        .version = "latest"},
    .vm_size = "Standard_D2s_v7",
    .ssh_public_key = "ssh-ed25519 AAAA… ops@example.com",
    .subnet_id_by_group = {{"1", subnet_id}, {"2", subnet_id}, {"3", subnet_id}},
    .node_port = 7000,
    .topology = {.groups = {
        {.group_id = "1", .target_count = 1},
        {.group_id = "2", .target_count = 1},
        {.group_id = "3", .target_count = 1},
    }},
    .placement_by_group = {
        {"1", {.kind = kythira::azure_placement_kind::availability_zone, .zone = "1"}},
        {"2", {.kind = kythira::azure_placement_kind::availability_zone, .zone = "2"}},
        {"3", {.kind = kythira::azure_placement_kind::availability_zone, .zone = "3"}},
    },
};
kythira::azure_vm_quorum_manager<std::uint64_t, std::string> mgr{cfg};
```

Without `placement_by_group`, the group ids are labels only and Azure places
every VM wherever it likes in the region.

## 6. Verifying

`tests/azure_quorum_manager_real_test.cpp` runs 10 VM and 5 VMSS cases
against a real subscription. It is registered with CTest only when the CMake
option `KYTHIRA_AZURE_REAL_TESTS` is `ON`, and it launches real VMs and costs
money (the full VM suite has cost about four cents). It reads its own test
variables (`AZURE_SUBSCRIPTION_ID`, `AZURE_TEST_RESOURCE_GROUP`,
`AZURE_TEST_SUBNET_ID_ZONE{1,2,3}`, `AZURE_TEST_VMSS_NAME`, …, see the file
and `.github/workflows/real-cloud-tests.yml`), not this file's.

## 7. VMSS manager: timeout rollback and scale-in protection

When `provision_timeout` expires before the new member is adoptable, the
manager undoes the scale-up without letting the scale set's `scaleInPolicy`
pick a victim (`.kiro/specs/group-scale-up-rollback/`). It deletes every
member that was not in the scale set before the capacity `PATCH`, by name,
with the scale set's `POST .../delete` action, which also lowers
`sku.capacity`. Only when the scale set lists no new member does it `PATCH`
the capacity back to its old value, and it then re-lists the scale set and
names any existing member that left. The timeout error ends with what the
rollback did.

Every member the manager adopts gets `protectionPolicy.protectFromScaleIn`
(a `PUT` on the scale set's member at api-version 2023-09-01), and the
constructor protects any adopted member of the cluster that is not, so no
capacity change, from this manager or anything else, makes the scale set
choose a voter. `decommission_node` still deletes a protected node: ARM does
not block user-initiated deletes on protected members. Members the manager
has not tagged are never protected. To scale in by hand, clear protection on
the members you want removed first:

```sh
az rest --method put \
    --url "https://management.azure.com/subscriptions/<sub>/resourceGroups/kythira-prod/providers/Microsoft.Compute/virtualMachineScaleSets/kythira-prod-z1/virtualMachines/<vm-name>?api-version=2023-09-01" \
    --body '{"properties":{"protectionPolicy":{"protectFromScaleIn":false}}}'
```

Run one manager per scale set, and do not scale it with other tooling while
the manager runs. A member something else adds during a provision looks new
to the manager and is deleted if that provision times out.
