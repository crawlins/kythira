# Design Document

## Overview

This document describes the design for GCP support in Kythira: two
`quorum_manager` implementations and one `certificate_provider`
implementation. All three satisfy existing concepts
(`include/raft/quorum_management.hpp`,
`include/raft/certificate_provider.hpp`) with no changes to those concepts
themselves — this is purely an additive backend, structured the same way
`aws_ec2_quorum_manager`/`aws_asg_quorum_manager`/`aws_acm_pca_provider` are.

| Class | Mechanism | Best for |
|---|---|---|
| `gcp_compute_quorum_manager` | Direct `instances.insert` / `.delete` | Dev / staging, simple deployments |
| `gcp_mig_quorum_manager` | `instanceGroupManagers.resize` target-size changes | Production (Spot VMs, instance templates, MIG-managed replacement of transient VM failures) |
| `gcp_privateca_certificate_provider` | Google Cloud Certificate Authority Service | Managed-CA certificate issuance, GCP analogue of `aws_acm_pca_provider` |

All three are header-only. The quorum managers compile behind `#ifdef
KYTHIRA_HAS_GCP_SDK`; the certificate provider compiles behind a separate
`#ifdef KYTHIRA_HAS_GCP_PRIVATECA`, mirroring the independence between
`KYTHIRA_HAS_AWS_SDK` and `KYTHIRA_HAS_AWS_ACM_PCA`. All use
`google-cloud-cpp` ≥ 2.20.

## The Core Design Decision: Node Identity

This is the one place where the GCP design cannot simply mirror the AWS
design, and it shapes almost every other decision in this document, so it is
worth stating up front before the component-by-component breakdown.

**AWS**: the EC2 instance ID (`i-0abc...`) is simultaneously (a) the
argument every EC2 API call addresses the instance by, and (b) a ready-made
large integer once its hex suffix is parsed. AWS's quorum managers exploit
this by setting `NodeId := ec2_id_to_node_id(ec2_instance_id)`. Every
subsequent call (`assess_quorum`, `decommission_node`) recomputes the EC2 ID
from the `NodeId` with a pure string/integer function — no lookup, no state.

**GCP**: the two identifiers are split. `instances.get`/`.delete`/
`.setLabels` address an instance by its **name** (a string chosen at
creation time and immutable thereafter); GCE also assigns a numeric `id`
field, but *no* Compute Engine API accepts that numeric ID as an addressing
parameter. So even if `NodeId` were defined as the numeric `id`, resolving
it back to a name for a subsequent `instances.delete` call would still
require an `instances.list` scan — the exact lookup AWS's scheme exists to
avoid.

This spec resolves the split by having kythira, not GCP, be the source of
the `NodeId`:

```
                    AWS                                GCP

  RunInstances  ──▶ i-0abc123...         random 63-bit ──▶ NodeId = 0x1a2b...
        │              │                  NodeId              │
        │              ▼                     │                ▼
        │        NodeId = parse_hex(id)      │      name = "kythira-{cluster}-{NodeId}"
        │              │                     │                │
        ▼              ▼                     ▼                ▼
  DescribeInstances(i-0abc...)          instances.insert(name = "kythira-...-0x1a2b...")
  TerminateInstances(i-0abc...)         instances.get/.delete(name = "kythira-...-0x1a2b...")
   ▲ address BY the ID  ▲                 ▲ address BY the name kythira chose ▲
```

For `gcp_compute_quorum_manager`, this makes `node_id_to_instance_name`/
`instance_name_to_node_id` pure functions exactly like AWS's
`node_id_to_ec2_id`/`ec2_id_to_node_id` — the statelessness property is
preserved, just via a different mechanism (kythira mints the ID and encodes
it in a name it also chooses, rather than parsing an ID GCP handed back).

For `gcp_mig_quorum_manager`, the trick breaks down: the MIG — not
kythira — assigns the instance name when it creates a replica during a
`resize`. Kythira cannot pre-choose that name. So this manager must, after
detecting a newly created (unlabelled) instance, generate a `NodeId` and
*write* the association as a `kythira-node-id` label, then *read that label
back* via `instances.list` whenever it needs to resolve `NodeId → name`
(`assess_quorum`, `decommission_node`). This is a real, unavoidable
asymmetry versus `aws_asg_quorum_manager` (which stays fully stateless
because ASG-created instances are still addressed by the same EC2-ID-as-
NodeId scheme, regardless of which AWS component created them). It is called
out here, in Requirement 5, and again at each affected call site, rather
than left as a silent inconsistency between the two managers.

## Architecture

```
quorum_manager concept (quorum_management.hpp)
  │
  ├── gcp_compute_quorum_manager                (include/raft/gcp_compute_quorum_manager.hpp)
  │     ├── InstancesClient, ZoneOperationsClient
  │     ├── assess_quorum     → instances.list (per zone, label filter)
  │     ├── provision_node    → instances.insert + wait_for_zone_operation + poll instances.get
  │     └── decommission_node → instances.delete + wait_for_zone_operation
  │
  └── gcp_mig_quorum_manager                   (include/raft/gcp_mig_quorum_manager.hpp)
        ├── InstanceGroupManagersClient, InstancesClient, ZoneOperationsClient
        ├── assess_quorum     → instances.list (per zone, label filter) keyed by kythira-node-id label
        ├── provision_node    → instanceGroupManagers.resize + poll listManagedInstances + instances.setLabels
        └── decommission_node → instances.list (label lookup) + instanceGroupManagers.deleteInstances

certificate_provider concept (certificate_provider.hpp)
  │
  └── gcp_privateca_certificate_provider        (include/raft/gcp_privateca_certificate_provider.hpp)
        ├── CertificateAuthorityServiceClient
        ├── root_certificate_pem → GetCertificateAuthority (cached)
        └── sign_csr             → CreateCertificate (synchronous — no poll loop, unlike ACM PCA)

Shared:
  gcp_client_config       (include/raft/gcp_client_config.hpp)        — project / credentials / endpoint / timeout
  wait_for_zone_operation  (include/raft/gcp_operation_wait.hpp)      — zone-operation polling, shared by both quorum managers
```

## Data Models

### Label Schema (GCE instances)

GCP labels are more restricted than AWS tags: keys and values must both
match `^[a-z][-a-z0-9_]{0,62}$` — lowercase only, no colons, 63-char cap.
Every AWS `kythira:xxx` tag key becomes `kythira-xxx` here:

```
kythira-cluster       = {cluster_name}
kythira-node-id       = {node_id}              ; decimal string
kythira-group         = {zone_name}            ; e.g. "us-central1-a"
kythira-managed-by    = kythira-gce-quorum-manager   ; or kythira-mig-quorum-manager
kythira-market        = {market}               ; "spot" or "standard"
kythira-placement     = {placement}            ; "none" or "compact"
```

Labels are written at creation for `gcp_compute_quorum_manager` (as part of
the `instances.insert` request body) and via a follow-up `instances.setLabels`
call for `gcp_mig_quorum_manager` (the MIG creates the instance; kythira
labels it after the fact — see the node-identity discussion above).

### Instance Metadata (distinct from labels)

```
startup-script              = {rendered startup_script_template}   ; written at creation
enable-guest-attributes     = TRUE                                  ; written at creation, gates the guest attribute below
```

Both keys are set once, by `provision_node`, as part of the `instances.insert`
request body — nothing writes to plain instance metadata after creation.

### Guest Attributes (a write-enabled subset of metadata, distinct from both labels and plain metadata)

```
kythira/last-heartbeat    = {unix_timestamp}    ; namespace "kythira", key "last-heartbeat"; written by the kythira process itself, from inside the guest
```

The heartbeat is a **guest attribute**, not a plain metadata key, because of
an asymmetry in GCE's permission model that plain metadata doesn't resolve
cleanly:

- A local read of any instance metadata (plain or guest-attribute) from
  inside the guest needs no IAM permission at all — just the metadata
  server's `Metadata-Flavor: Google` header.
- A local **write**, however, only bypasses IAM for the guest-attribute
  namespace. A write to plain metadata has to go through the real
  `instances.setMetadata` API call, which requires
  `compute.instances.setMetadata` on the instance's *own* attached service
  account — a permission that has to be granted to every fleet instance,
  is easy to over-scope (it lets a compromised instance rewrite its own
  `startup-script`, not just the heartbeat key), and was the original design
  in an earlier revision of this document. A write to the
  `instance/guest-attributes/{namespace}/{key}` path, by contrast, is
  answered entirely by the local metadata server with no API call and no IAM
  check — the guest doesn't need `compute.instances.setMetadata` or any
  other permission on itself to update its own heartbeat.

The tradeoff moves to the *reader*: `assess_quorum`/`maintain_quorum` read
the value back via `instances.getGuestAttributes`, which does require
`compute.instances.getGuestAttributes` on the quorum manager's own
credentials. That's not a new operational burden — the manager already holds
broad `compute.instances.*` permissions to create, list, and delete
instances in the first place — whereas requiring `setMetadata` on every
fleet instance's service account would have been a permission granted
per-node, for a write only that node itself ever needed to make. This is
the closest GCP equivalent to AWS's `--ec2-heartbeat-tag` flag writing an EC2
tag from inside the instance via its IAM role, but AWS's EC2 tag write has no
comparably scoped-down local-write channel — `CreateTags` there is a normal,
fully-IAM-checked API call regardless of whether it's invoked from inside the
instance or externally, so the AWS design accepts granting the (narrower)
`ec2:CreateTags` permission to the fleet role. GCP's guest attributes let
this design avoid granting the fleet any write permission at all.

`provision_node` sets `enable-guest-attributes = TRUE` at creation
(Requirement 8 AC 6); omitting it makes the guest's local write to
`instance/guest-attributes/...` fail with no error visible to kythira — the
key simply never appears — which is why this spec treats the flag as a
required part of every `instances.insert` request, not an optional
enhancement.

### Instance Name Grammar (`gcp_compute_quorum_manager` only)

```
kythira-{cluster_name}-{node_id}
```

`{node_id}` is the decimal representation of a random 63-bit value. Because
GCE instance names are DNS-1035 labels (`^[a-z]([-a-z0-9]{0,61}[a-z0-9])?$`,
≤63 chars), `cluster_name` length is validated at construction against the
worst case (`node_id` at its widest, ~19 decimal digits) so a `provision_node`
call can never fail on a name-too-long error introduced by an unlucky random
draw.

## Components and Interfaces

### 1. `include/raft/gcp_client_config.hpp`

A plain aggregate, compiled unconditionally, plus the two label/name
validators:

```cpp
#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace kythira {

struct gcp_client_config {
    std::string project_id;
    std::string credentials_json;       // empty = Application Default Credentials
    std::string endpoint_override;
    std::chrono::seconds api_timeout{30};
    std::chrono::milliseconds operation_poll_interval{2000};
};

// ^[a-z][-a-z0-9_]{0,62}$
[[nodiscard]] bool is_valid_gcp_label(std::string_view value) noexcept;

// ^[a-z]([-a-z0-9]{0,61}[a-z0-9])?$   (GCE instance-name-shaped DNS-1035 label)
[[nodiscard]] bool is_valid_gcp_resource_name(std::string_view value) noexcept;

}  // namespace kythira
```

### 2. `include/raft/gcp_operation_wait.hpp`

```cpp
#pragma once

#ifdef KYTHIRA_HAS_GCP_SDK

#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>
#include <google/cloud/compute/zone_operations/v1/zone_operations_client.h>

#include <chrono>
#include <string>

namespace kythira {

// Polls a zone operation to completion. Shared by gcp_compute_quorum_manager
// and gcp_mig_quorum_manager so insert/delete/resize call sites don't each
// reimplement polling and error-unwrapping.
[[nodiscard]] auto wait_for_zone_operation(
    google::cloud::compute_zone_operations_v1::ZoneOperationsClient& client,
    std::string project, std::string zone, std::string operation_name,
    std::chrono::seconds timeout, std::chrono::milliseconds poll_interval)
    -> kythira::future_default<void>;

}  // namespace kythira

#endif  // KYTHIRA_HAS_GCP_SDK
```

The implementation loops: `fiu_do_on("raft/gcp/zone_operation/poll", throw
...)`, then `client.GetZoneOperation(project, zone, operation_name)`; if
`status() == DONE`, inspect `error()` — empty means success, non-empty means
an exceptional Future built from the error's `errors[].code`/`.message`
entries; otherwise sleep `poll_interval` and loop until `timeout` elapses, at
which point the Future is rejected with a distinguishable
`gcp_operation_timeout` exception type (so callers can tell "GCP said no"
apart from "we gave up waiting").

### 3. `include/raft/gcp_compute_quorum_manager.hpp`

```cpp
#pragma once

#ifdef KYTHIRA_HAS_GCP_SDK

#include <raft/gcp_client_config.hpp>
#include <raft/gcp_operation_wait.hpp>
#include <raft/quorum_management.hpp>
#include <raft/fault_injection.hpp>

#include <google/cloud/compute/instances/v1/instances_client.h>
#include <google/cloud/compute/zone_operations/v1/zone_operations_client.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace kythira {

enum class gcp_placement_policy_kind : std::uint8_t {
    none,        // no resourcePolicies entry
    collocated,  // COLLOCATED placement policy
};

struct gcp_placement_policy_config {
    std::string self_link;   // resource policy self-link; empty = none
    bool collocation{false}; // informational
};

template<typename GroupId = std::string>
struct gcp_compute_quorum_manager_config {
    gcp_client_config gcp{};
    std::string cluster_name;
    std::string machine_type{"e2-medium"};
    std::string boot_disk_image;
    std::uint32_t boot_disk_size_gb{20};
    std::string network{"default"};
    std::map<GroupId, std::string> subnetwork_by_group{};
    std::string service_account_email{};
    std::vector<std::string> service_account_scopes{};
    std::uint16_t node_port{7000};
    std::string startup_script_template{};
    desired_topology<GroupId> topology{};
    std::chrono::seconds provision_timeout{300};
    std::chrono::milliseconds poll_interval{5000};
    std::map<GroupId, gcp_placement_policy_config> placement_by_group{};
    bool spot{false};
    std::map<std::string, std::string> extra_labels{};
};

template<typename NodeId, typename Address>
class gcp_compute_quorum_manager {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;

    explicit gcp_compute_quorum_manager(gcp_compute_quorum_manager_config<std::string> config);

    gcp_compute_quorum_manager(gcp_compute_quorum_manager&&) noexcept = default;
    gcp_compute_quorum_manager& operator=(gcp_compute_quorum_manager&&) noexcept = default;
    gcp_compute_quorum_manager(const gcp_compute_quorum_manager&) = delete;
    gcp_compute_quorum_manager& operator=(const gcp_compute_quorum_manager&) = delete;

    [[nodiscard]] auto assess_quorum(
        const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>>;

    [[nodiscard]] auto provision_node(std::string target_group, std::optional<NodeId> replacing)
        -> kythira::future_default<peer_info<NodeId, Address>>;

    [[nodiscard]] auto decommission_node(const NodeId& node_id)
        -> kythira::future_default<void>;

    [[nodiscard]] auto topology() const -> desired_topology<std::string> { return _config.topology; }

    [[nodiscard]] auto maintain_quorum(
        const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>>;

    // Pure computation — no API call. Public for unit testing (Requirement 23 AC 10).
    [[nodiscard]] static std::string node_id_to_instance_name(
        const std::string& cluster_name, NodeId node_id);
    [[nodiscard]] static std::optional<NodeId> instance_name_to_node_id(
        const std::string& cluster_name, const std::string& instance_name);

private:
    gcp_compute_quorum_manager_config<std::string> _config;
    google::cloud::compute_instances_v1::InstancesClient _instances;
    google::cloud::compute_zone_operations_v1::ZoneOperationsClient _zone_ops;
};

static_assert(quorum_manager<gcp_compute_quorum_manager<std::uint64_t, std::string>,
                              std::uint64_t, std::string, std::string>);

}  // namespace kythira

#endif  // KYTHIRA_HAS_GCP_SDK
```

`node_id_to_instance_name` renders `"kythira-{cluster_name}-{node_id}"`;
`instance_name_to_node_id` strips the `"kythira-{cluster_name}-"` prefix and
parses the remainder as the `NodeId`'s underlying integer type, returning
`std::nullopt` on any mismatch (wrong prefix, non-numeric suffix) rather than
throwing, since it is also used defensively when scanning `instances.list`
results that could in principle include instances this manager did not
create (guarded further by the `kythira-cluster` label filter in the list
request itself).

### 4. `include/raft/gcp_mig_quorum_manager.hpp`

```cpp
#pragma once

#ifdef KYTHIRA_HAS_GCP_SDK

#include <raft/gcp_client_config.hpp>
#include <raft/gcp_operation_wait.hpp>
#include <raft/quorum_management.hpp>
#include <raft/fault_injection.hpp>

#include <google/cloud/compute/instances/v1/instances_client.h>
#include <google/cloud/compute/instance_group_managers/v1/instance_group_managers_client.h>
#include <google/cloud/compute/zone_operations/v1/zone_operations_client.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace kythira {

template<typename GroupId = std::string>
struct gcp_mig_quorum_manager_config {
    gcp_client_config gcp{};
    std::string cluster_name;
    std::map<GroupId, std::string> mig_by_group;  // required, >= 1 entry
    std::uint16_t node_port{7000};
    std::chrono::seconds provision_timeout{300};
    std::chrono::milliseconds poll_interval{5000};
    desired_topology<GroupId> topology{};
};

template<typename NodeId, typename Address>
class gcp_mig_quorum_manager {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;

    explicit gcp_mig_quorum_manager(gcp_mig_quorum_manager_config<std::string> config);

    gcp_mig_quorum_manager(gcp_mig_quorum_manager&&) noexcept = default;
    gcp_mig_quorum_manager(const gcp_mig_quorum_manager&) = delete;

    [[nodiscard]] auto assess_quorum(
        const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>>;

    [[nodiscard]] auto provision_node(std::string target_group, std::optional<NodeId> replacing)
        -> kythira::future_default<peer_info<NodeId, Address>>;

    [[nodiscard]] auto decommission_node(const NodeId& node_id)
        -> kythira::future_default<void>;

    [[nodiscard]] auto topology() const -> desired_topology<std::string> { return _config.topology; }

    [[nodiscard]] auto maintain_quorum(
        const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>>;

private:
    // Resolves NodeId -> instance name via a labelled instances.list lookup.
    // Unlike gcp_compute_quorum_manager's equivalent, this is a real API
    // call, not a pure function — see "The Core Design Decision" above.
    [[nodiscard]] auto find_instance_by_node_id(const std::string& zone, NodeId node_id)
        -> kythira::future_default<std::optional<std::string>>;

    gcp_mig_quorum_manager_config<std::string> _config;
    google::cloud::compute_instance_group_managers_v1::InstanceGroupManagersClient _migs;
    google::cloud::compute_instances_v1::InstancesClient _instances;
    google::cloud::compute_zone_operations_v1::ZoneOperationsClient _zone_ops;
};

static_assert(quorum_manager<gcp_mig_quorum_manager<std::uint64_t, std::string>,
                              std::uint64_t, std::string, std::string>);

}  // namespace kythira

#endif  // KYTHIRA_HAS_GCP_SDK
```

### 5. `include/raft/gcp_privateca_certificate_provider.hpp`

```cpp
#pragma once

/// certificate_provider implementation backed by Google Cloud Certificate
/// Authority Service. Structural analogue of aws_acm_pca_provider: a
/// gcp_client_config embedded for project/credentials/endpoint/timeout,
/// fiu_do_on() fault points around every API call, errors surfaced as
/// rejected futures. Does NOT create or delete a CA pool or CA — that is an
/// out-of-band operator action, same scope decision as aws_acm_pca_provider.

#include <raft/gcp_client_config.hpp>
#include <raft/certificate_provider.hpp>
#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>

#ifdef KYTHIRA_HAS_GCP_PRIVATECA

#include <google/cloud/privateca/v1/certificate_authority_service_client.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace raft::testing {

struct gcp_privateca_certificate_provider_config {
    kythira::gcp_client_config gcp;
    std::string location;
    std::string ca_pool_id;
    std::string certificate_authority_id{};
    std::string certificate_template{};
    std::chrono::seconds validity{std::chrono::hours(24 * 30)};
};

class gcp_privateca_certificate_provider {
public:
    explicit gcp_privateca_certificate_provider(gcp_privateca_certificate_provider_config config);

    // GetCertificateAuthority (or ListCertificateAuthorities when
    // certificate_authority_id is empty), cached after the first successful call.
    [[nodiscard]] auto root_certificate_pem() -> kythira::future_default<std::string>;

    // CreateCertificate — synchronous issuance, no poll loop (see design notes).
    [[nodiscard]] auto sign_csr(std::string csr_pem, csr_signing_options options)
        -> kythira::future_default<pem_material>;

    // Optional: RevokeCertificate.
    [[nodiscard]] auto revoke(std::string serial) -> kythira::future_default<void>;

private:
    gcp_privateca_certificate_provider_config _config;
    google::cloud::privateca_v1::CertificateAuthorityServiceClient _client;
    std::mutex _root_cache_mutex;
    std::optional<std::string> _root_cache;
};

static_assert(certificate_provider<gcp_privateca_certificate_provider>);

}  // namespace raft::testing

#endif  // KYTHIRA_HAS_GCP_PRIVATECA
```

`sign_csr` builds a `google::cloud::cpp::privateca::v1::Certificate` message
with `pem_csr` set to `csr_pem`, `lifetime` from `config.validity`, and
`certificate_config.x509_config` left unset (the CSR's own extensions and
subject drive the issued certificate, matching how `aws_acm_pca_provider`
passes the CSR through with a template ARN rather than re-deriving a subject
from `csr_signing_options`). It calls `CreateCertificate` against
`projects/{project}/locations/{location}/caPools/{ca_pool_id}` and reads the
`pem_certificate` field directly from the response — no `GetCertificate`
poll loop, because CAS's `CreateCertificate` is synchronous for the
non-managed (`CA_MANAGED`, i.e. self-managed key) issuance flow this
component uses; this is a deliberate simplification versus
`aws_acm_pca_provider`, not an oversight, and is called out in Requirement
21 AC 4.

## Sequence: `gcp_compute_quorum_manager::provision_node`

```
orchestrator          gcp_compute_quorum_manager           GCP Compute Engine
     │                          │                                  │
     │  provision_node(zone,·) │                                  │
     │ ────────────────────────▶                                  │
     │                          │  generate NodeId (random 63-bit) │
     │                          │  name = "kythira-{cl}-{id}"      │
     │                          │  instances.insert(name, ...)     │
     │                          │ ─────────────────────────────────▶
     │                          │      ◀──── operation (RUNNING) ──│
     │                          │  wait_for_zone_operation(op)     │
     │                          │ ─────────────────────────────────▶  (poll zoneOperations.get)
     │                          │      ◀──── operation (DONE) ─────│
     │                          │  poll instances.get until RUNNING│
     │                          │ ─────────────────────────────────▶
     │                          │      ◀──── status = RUNNING ─────│
     │   ◀── peer_info{id, ip:port} ──│                            │
```

On a `NAME_IN_USE` operation error (NodeId collision) the manager loops back
to "generate NodeId" up to 5 times before rejecting; on `provision_timeout`
elapsing during the polling step, it issues `instances.delete` (best-effort)
before rejecting.

## Sequence: `gcp_mig_quorum_manager::provision_node`

```
orchestrator          gcp_mig_quorum_manager                MIG / GCE
     │                          │                                  │
     │ provision_node(zone,·)  │                                  │
     │ ────────────────────────▶                                  │
     │                          │  instanceGroupManagers.get       │
     │                          │ ─────────────────────────────────▶ (read targetSize)
     │                          │  instanceGroupManagers.resize(+1)│
     │                          │ ─────────────────────────────────▶
     │                          │  wait_for_zone_operation(op)     │
     │                          │  poll listManagedInstances       │
     │                          │  for an unlabelled instance      │
     │                          │ ─────────────────────────────────▶
     │                          │      ◀── new instance (no label)─│
     │                          │  generate NodeId                 │
     │                          │  instances.get (fingerprint)     │
     │                          │  instances.setLabels(kythira-…)  │
     │                          │ ─────────────────────────────────▶
     │   ◀── peer_info{id, ip:port} ──│                            │
```

The label write is the step with no AWS equivalent: `aws_asg_quorum_manager`
never needs to write an identifying tag after the fact because the identity
(EC2 instance ID) already exists the moment `RunInstances`/the ASG creates
the instance. Here, the identity (`NodeId`) exists only in kythira's memory
until `setLabels` durably records it — a window during which a `provision_node`
Future is still pending is expected and does not affect correctness (no
other code path reads `kythira-node-id` before this call is awaited), but is
worth naming explicitly for a future reader auditing crash-safety: if the
process crashes between the `resize` and the `setLabels`, the new instance
is a labelled-as-`kythira-cluster`-but-not-`kythira-node-id` orphan.
`assess_quorum`'s label-keyed scan silently excludes such an instance from
both the live and unreachable sets (it never matches a `node_placement`'s
generated name), so it is neither miscounted as live nor spuriously flagged;
manual operator cleanup (or a future `maintain_quorum` reconciliation pass —
out of scope here) is required to notice and label or delete it. This is
documented as a known limitation, not resolved by this spec.

## Testing Strategy

Mirrors `aws_quorum_manager_unit_test.cpp`'s shape but only two tiers
(Requirement 23 AC 3 explains the LocalStack-tier omission):

1. **Unit tests** — a hand-written fake connection layer per client type
   (`InstancesClient`, `InstanceGroupManagersClient`, `ZoneOperationsClient`,
   `CertificateAuthorityServiceClient`) implementing just the RPC surface
   this code calls, injected through a constructor overload that bypasses
   real credential/endpoint setup. Fast, deterministic, covers construction
   validation, the pure name/label functions, and fault-injection paths.
2. **Real-GCP integration tests** — gated behind `KYTHIRA_GCP_REAL_TESTS=1`,
   excluded from the default `ctest` run, following the same
   skip-don't-fail-on-missing-credentials discipline as
   `aws_quorum_manager_real_ec2_test.cpp`.

### CI wiring (Requirement 24)

`.github/workflows/real-cloud-tests.yml`'s `gcp` job (scaffolded as a
no-op by `.kiro/specs/ci-real-cloud-tests/`) is replaced with real steps
mirroring the `aws` job's shape: a `google-github-actions/auth@v2` step
using Workload Identity Federation to impersonate a dedicated service
account (no service-account JSON key), then a per-bundle `ctest -R`
invocation gated by `REAL_CLOUD_TESTS_GCP_QUORUM_MANAGER_ENABLED` /
`REAL_CLOUD_TESTS_GCP_PRIVATECA_ENABLED`.
`tests/gcp_quorum_manager_real_gce_test.cpp` gains the same
`TestCostReport`/`CostAccumulator`/`CostSummaryFixture` apparatus
`aws_real_ec2_test_support.hpp` already provides for AWS, re-priced against
published GCE on-demand machine-type pricing, and the same
signal-driven (`SIGTERM`/`SIGINT`/`SIGHUP`/`SIGQUIT`/`SIGPIPE`)
teardown-on-cancel handlers, so a canceled or killed CI run still tears
down instances/MIG capacity rather than leaking them.
`scripts/ci-cloud-credentials/gcp/` mirrors the AWS directory's shape
(provisioning script, per-bundle `policies/*.json` IAM-binding fragments,
`README.md`) — see Requirement 24 for the full acceptance criteria.

## Open Questions / Follow-ups (out of scope for this spec)

- **Regional MIGs**: this spec only covers zonal MIGs (Requirement
  "Glossary — Managed Instance Group (MIG)"). Regional MIG support, which
  would let a single MIG span kythira's per-zone `GroupId` topology instead
  of one MIG per zone, is a plausible follow-up but changes the
  `GroupId`-to-MIG mapping fundamentally enough to warrant its own spec.
- **GKE-based node hosting**: this spec covers Compute Engine VMs only, not
  a Kubernetes-pod-based quorum manager. Out of scope, matching the AWS
  spec's EC2/ASG-only scope (no ECS/EKS quorum manager exists either).
- **Orphaned-instance reconciliation** for the crash window described in the
  `gcp_mig_quorum_manager::provision_node` sequence above: not addressed by
  this spec; flagged for a future `maintain_quorum` enhancement.
