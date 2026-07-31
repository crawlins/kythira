# GCP Quorum Managers & Certificate Provider

Kythira ships three Google Cloud Platform backends, structured as additive
implementations of existing concepts — no changes to the `quorum_manager` or
`certificate_provider` concepts themselves. They are the GCP analogues of the
AWS backends (`aws_ec2_quorum_manager`, `aws_asg_quorum_manager`,
`aws_acm_pca_provider`).

| Class | Mechanism | Best for |
|---|---|---|
| `gcp_compute_quorum_manager` | Direct `instances.insert` / `.delete` | Dev / staging, simple deployments |
| `gcp_mig_quorum_manager` | `instanceGroupManagers.resize` target-size changes | Production (Spot VMs, instance templates, MIG-managed replacement) |
| `gcp_privateca_certificate_provider` | Google Cloud Certificate Authority Service (CAS) | Managed-CA certificate issuance |

All three are header-only. The quorum managers compile behind
`#ifdef KYTHIRA_HAS_GCP_SDK`; the certificate provider compiles behind the
independent `#ifdef KYTHIRA_HAS_GCP_PRIVATECA` (mirroring how
`KYTHIRA_HAS_AWS_ACM_PCA` is independent of `KYTHIRA_HAS_AWS_SDK`). All use
`google-cloud-cpp` ≥ 2.20.

## Build detection

- `find_package(google_cloud_cpp_compute QUIET COMPONENTS compute_instances
  compute_instance_group_managers compute_zone_operations)` sets
  `KYTHIRA_HAS_GCP_SDK` when all three components are present.
- `find_package(google_cloud_cpp_privateca QUIET)` sets
  `KYTHIRA_HAS_GCP_PRIVATECA` independently.
- The `google-cloud-cpp` vcpkg port with features `["compute", "privateca"]`
  supplies both.
- The `"GCP Integration"` Kconfig menu exposes `GCP_SDK` and
  `GCP_PRIVATECA` (`depends on GCP_SDK`), both `default y`.

## The core design decision: node identity

This is the one place the GCP design cannot simply mirror the AWS design, and it
shapes almost everything else.

**AWS** reuses the EC2 instance ID as the `NodeId`: the ID is simultaneously the
addressing key for every EC2 API call *and* a ready-made integer. So
`assess_quorum`/`decommission_node` recompute the EC2 ID from a `NodeId` with a
pure function — no lookup, no state.

**GCP** splits the two identifiers. `instances.get`/`.delete`/`.setLabels`
address an instance by its **name** (a string chosen at creation time); GCE also
assigns a numeric `id`, but *no* Compute Engine API accepts that numeric ID as an
addressing parameter. Resolving a numeric ID back to a name would still require
an `instances.list` scan — the exact lookup AWS's scheme exists to avoid.

Kythira resolves this by being the source of the `NodeId` itself:

```
                    AWS                                GCP

  RunInstances  ──▶ i-0abc123...         random 63-bit ──▶ NodeId = 0x1a2b...
        │              │                  NodeId              │
        ▼              ▼                     ▼                ▼
  DescribeInstances(i-0abc...)          instances.insert(name="kythira-{cl}-{NodeId}")
  TerminateInstances(i-0abc...)         instances.get/.delete(name="kythira-{cl}-{NodeId}")
   ▲ address BY the ID  ▲                 ▲ address BY the name kythira chose ▲
```

`provision_node` draws a cryptographically-strong **63-bit** random `NodeId`
(top bit clear, so it is representable whether the instantiated `node_id_type` is
signed or unsigned) *before* calling `instances.insert`.

- For **`gcp_compute_quorum_manager`**, the `NodeId` is encoded into a
  deterministic instance name (`kythira-{cluster}-{node_id}`), so
  `node_id_to_instance_name` / `instance_name_to_node_id` are **pure functions**
  requiring no API call — the AWS statelessness property, via a different
  mechanism.
- For **`gcp_mig_quorum_manager`**, the MIG (not kythira) chooses the instance
  name during a `resize`, so kythira instead writes the `NodeId` as a
  `kythira-node-id` **instance label** after the instance appears, and reads it
  back via a labelled `instances.list` to resolve `NodeId → name`. This is the
  one respect in which the MIG manager is *not* purely stateless — an unavoidable
  consequence of MIGs owning instance naming.

On a name/label collision (practically unreachable given the 63-bit keyspace),
`provision_node` regenerates a fresh `NodeId` and retries, up to 5 attempts.

## Label, metadata, and guest-attribute scheme

GCP labels are more restricted than AWS tags: both key and value must match
`^[a-z][-a-z0-9_]{0,62}$` — lowercase only, no colons, 63-char cap. Every AWS
`kythira:xxx` tag key becomes `kythira-xxx` here.

Every managed GCE instance carries these labels:

| Label key | Value |
|---|---|
| `kythira-cluster` | `{cluster_name}` |
| `kythira-node-id` | decimal string of the generated `NodeId` |
| `kythira-group` | `{zone_name}` (e.g. `us-central1-a`) |
| `kythira-managed-by` | `kythira-gce-quorum-manager` or `kythira-mig-quorum-manager` |
| `kythira-market` | `spot` or `standard` |
| `kythira-placement` | `none` or `compact` |

`extra_labels` are merged in but never override the six standard labels. For
`gcp_compute_quorum_manager`, `kythira-node-id` is for operator visibility only
(the name already encodes the id); for `gcp_mig_quorum_manager` it is load
bearing (it *is* the reverse mapping).

Instance **metadata** (distinct from labels), written once at creation:

```
startup-script          = {rendered startup_script_template}
enable-guest-attributes = TRUE
```

The **heartbeat** is a *guest attribute* (`kythira/last-heartbeat`), written by
the running kythira process from inside the guest via a local, unauthenticated
`PUT` to its own metadata server — no IAM permission on the guest's service
account required. `provision_node` must set `enable-guest-attributes = TRUE` at
creation (it does, unconditionally) or the guest write silently never appears.
The quorum manager reads it back externally via `instances.getGuestAttributes`,
which needs `compute.instances.getGuestAttributes` on the *manager's own*
credentials — a permission it already holds alongside
`compute.instances.get`/`.list`/`.insert`/`.delete`.

## MIG autohealing guard

`gcp_mig_quorum_manager` refuses to operate against a MIG with a non-empty
`autoHealingPolicies` (checked at construction via `instanceGroupManagers.get`,
throwing `std::invalid_argument`). Two independent systems replacing instances
based on different liveness signals can each act on a node the other still
considers healthy, producing split-brain — the same risk AWS's
`HealthCheckType == "EC2"` requirement guards against. Operators wanting
MIG-driven autohealing for transient VM failures must leave `autoHealingPolicies`
unset and rely on kythira's `maintain_quorum` loop exclusively.

## Configuration examples

### `gcp_compute_quorum_manager`

```cpp
#include <raft/gcp_compute_quorum_manager.hpp>

kythira::gcp_compute_quorum_manager_config<std::string> cfg{
    .gcp = {.project_id = "my-project"},           // ADC unless credentials_json set
    .cluster_name = "raft-prod",                    // must be a valid GCP label
    .machine_type = "e2-standard-4",
    .boot_disk_image = "projects/my-project/global/images/kythira-node-v3",
    .node_port = 7000,
    .subnetwork_by_group = {
        {"us-central1-a", "projects/my-project/regions/us-central1/subnetworks/kythira"},
        {"us-central1-b", "projects/my-project/regions/us-central1/subnetworks/kythira"},
        {"us-central1-c", "projects/my-project/regions/us-central1/subnetworks/kythira"},
    },
    .service_account_email = "kythira-node@my-project.iam.gserviceaccount.com",
    .service_account_scopes = {"https://www.googleapis.com/auth/cloud-platform"},
    .startup_script_template =
        "#!/bin/bash\n/opt/kythira/node --id {NODE_ID} --port {NODE_PORT} "
        "--cluster {CLUSTER} --zone {ZONE}\n",
    .topology = {.groups = {
        {.group_id = "us-central1-a", .target_count = 1},
        {.group_id = "us-central1-b", .target_count = 1},
        {.group_id = "us-central1-c", .target_count = 1},
    }},
    .spot = false,
};

kythira::gcp_compute_quorum_manager<std::uint64_t, std::string> mgr{cfg};
```

### `gcp_mig_quorum_manager`

```cpp
#include <raft/gcp_mig_quorum_manager.hpp>

kythira::gcp_mig_quorum_manager_config<std::string> cfg{
    .gcp = {.project_id = "my-project"},
    .cluster_name = "raft-prod",
    .mig_by_group = {                               // one zonal MIG per zone
        {"us-central1-a", "kythira-mig-a"},
        {"us-central1-b", "kythira-mig-b"},
        {"us-central1-c", "kythira-mig-c"},
    },
    .node_port = 7000,
    .topology = {.groups = {
        {.group_id = "us-central1-a", .target_count = 1},
        {.group_id = "us-central1-b", .target_count = 1},
        {.group_id = "us-central1-c", .target_count = 1},
    }},
};

// Throws std::invalid_argument if any MIG has an autoHealingPolicies entry.
kythira::gcp_mig_quorum_manager<std::uint64_t, std::string> mgr{cfg};
```

### `gcp_privateca_certificate_provider`

```cpp
#include <raft/gcp_privateca_certificate_provider.hpp>
#include <raft/gcp_privateca_certificate_provider_impl.hpp>

raft::testing::gcp_privateca_certificate_provider_config cfg{
    .gcp = {.project_id = "my-project"},
    .location = "us-central1",
    .ca_pool_id = "kythira-pool",                   // pre-existing, operator-provisioned
    // .certificate_authority_id = "kythira-ca-1",  // optional: pin a specific CA
    .validity = std::chrono::hours(24 * 30),
};

raft::testing::gcp_privateca_certificate_provider provider{cfg};
```

Or via `ca_service`:

```
ca_service --serve 0.0.0.0:8443 --provider gcp-privateca \
    --gcp-project my-project --gcp-location us-central1 --gcp-ca-pool kythira-pool \
    --auth-token "$CA_SERVICE_AUTH_TOKEN" --tls-cert cert.pem --tls-key key.pem
```

Unlike ACM Private CA, CAS's `CreateCertificate` issues synchronously — there is
no `GetCertificate` poll loop. Routes CAS cannot serve (`/v1/certificates/revoke`
without a configured revocation setup, `/v1/crl`) return `501 Not Implemented`,
the same convention as `--provider aws-acm-pca`.

## Fault injection

Every API call site is gated by a `fiu_do_on()` point (compiled to a no-op when
`FIU_ENABLE` is undefined):

| Point | Checked before |
|---|---|
| `raft/gcp/zone_operation/poll` | each `zoneOperations.get` poll (shared helper) |
| `raft/gcp/compute/list_instances` | each `instances.list` in `assess_quorum` |
| `raft/gcp/compute/insert_instance` | `instances.insert` in `provision_node` |
| `raft/gcp/compute/delete_instance` | `instances.delete` in `decommission_node` |
| `raft/gcp/compute/maintain_quorum` | the assessment step of `maintain_quorum` |
| `raft/gcp/mig/list_instances` | each `instances.list` in `assess_quorum` |
| `raft/gcp/mig/resize` | `instanceGroupManagers.resize` in `provision_node` |
| `raft/gcp/mig/delete_instances` | `instanceGroupManagers.deleteInstances` |
| `raft/gcp/mig/maintain_quorum` | the assessment step of `maintain_quorum` |
| `raft/gcp/privateca/get_certificate_authority` | `GetCertificateAuthority` |
| `raft/gcp/privateca/create_certificate` | `CreateCertificate` |
| `raft/gcp/privateca/revoke_certificate` | `RevokeCertificate` |

## Testing

Following the two-tier model (GCP has no widely available Compute Engine
emulator equivalent to LocalStack, so there is no middle tier):

- **Unit tests** (`tests/gcp_quorum_manager_unit_test.cpp`,
  `tests/gcp_privateca_provider_unit_test.cpp`) — construction validation, the
  pure name/label functions, `target_group` rejection, and fault-injection
  paths. The `is_valid_gcp_label` / `is_valid_gcp_resource_name` suites run on
  every build, including one without `google-cloud-cpp`.
- **Real-GCP integration tests** (`tests/gcp_quorum_manager_real_gce_test.cpp`,
  `tests/gcp_privateca_provider_real_test.cpp`) — gated behind
  `KYTHIRA_GCP_REAL_TESTS=1`, excluded from the default `ctest` run, and skipped
  (not failed) when credentials or required env vars are absent. Wired into the
  `gcp` job of `.github/workflows/real-cloud-tests.yml` via Workload Identity
  Federation; see `scripts/ci-cloud-credentials/gcp/README.md`.
