# Implementation Plan — Azure Cloud Services

## Status: Not started

This plan has not yet been implemented. `include/raft/azure_client_config.hpp`,
`include/raft/azure_vm_quorum_manager.hpp`,
`include/raft/azure_vmss_quorum_manager.hpp`, and
`include/raft/azure_key_vault_ca_provider.hpp` do not exist yet. Update this
status line and the checkboxes below as work lands, following the same
tracking discipline the `aws-quorum-manager` spec's own status header
documents (`.kiro/specs/aws-quorum-manager/tasks.md`'s note about the doc
previously drifting from the real implementation).

## Overview

Implement two Azure-based `quorum_manager` classes and one
`certificate_provider` class:
- `azure_vm_quorum_manager` — direct Azure VM management (primary)
- `azure_vmss_quorum_manager` — Virtual Machine Scale Set management (production-grade)
- `azure_key_vault_ca_provider` — CSR signing backed by an Azure Key Vault key

All three are header-only, gated behind `KYTHIRA_HAS_AZURE_SDK` (quorum
managers) or `KYTHIRA_HAS_AZURE_KEY_VAULT` (CA provider), and use the Azure
SDK for C++.

Reference implementations to study before starting:
- `include/raft/aws_ec2_quorum_manager.hpp` — closest structural analogue to
  `azure_vm_quorum_manager`, though NodeId derivation runs in the *opposite*
  direction (design.md's "NodeId derivation" section) — do not copy the
  `ec2_id_to_node_id` pattern verbatim.
- `include/raft/aws_asg_quorum_manager.hpp` — closest structural analogue to
  `azure_vmss_quorum_manager`; its tag-scan `next_node_id()`-style bookkeeping
  is the pattern `azure_vm_quorum_manager` *also* needs (both Azure managers
  use it, unlike AWS where only the ASG variant needs a fallback for it).
- `include/raft/aws_acm_pca_provider.hpp` / `_impl.hpp` — closest structural
  analogue to `azure_key_vault_ca_provider`'s public/impl split and its
  `certificate_provider` conformance.
- `include/raft/certificate_authority.hpp` — the CSR-parsing/TBSCertificate-
  assembly logic `azure_key_vault_ca_provider::sign_csr` needs to duplicate
  the relevant subset of (design.md's "Sharing the X.509-building code path",
  option (a)).
- `include/raft/quorum_management.hpp` — concept definition and shared types.
- `include/raft/fault_injection.hpp` — `fiu_do_on` macro usage.
- `cmd/ca_service/` — the `any_certificate_provider` type-erasure pattern to
  extend with a third provider option (Requirement 17 AC 7).

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 2],
      "description": "Build system detection and shared azure_client_config — prerequisite for everything else"
    },
    {
      "wave": 2,
      "tasks": [3],
      "description": "azure_vm_quorum_manager — no dependency on azure_vmss_quorum_manager"
    },
    {
      "wave": 3,
      "tasks": [4],
      "description": "azure_vmss_quorum_manager — shares next_node_id()/tag helpers with task 3 but is independently implementable"
    },
    {
      "wave": 4,
      "tasks": [5],
      "description": "azure_key_vault_ca_provider — independent of tasks 3-4; only needs task 2's azure_client_config"
    },
    {
      "wave": 5,
      "tasks": [6, 7],
      "description": "Unit tests for the quorum managers and the CA provider"
    },
    {
      "wave": 6,
      "tasks": [8],
      "description": "Real-Azure integration tests — independent per-suite, but grouped since they share one fixture pattern"
    },
    {
      "wave": 7,
      "tasks": [9],
      "description": "ca_service --provider azure-key-vault wiring and documentation updates"
    }
  ]
}
```

## Tasks

- [ ] 1. Add Azure SDK for C++ CMake detection
  - In `CMakeLists.txt` (root), add, parallel to the existing "AWS SDK"
    section (lines 547–555):
    ```cmake
    # ── Azure SDK (azure_vm_quorum_manager, azure_vmss_quorum_manager) ──────────
    kythira_find_optional(AZURE_SDK azure-core-cpp CONFIG)
    kythira_find_optional(AZURE_IDENTITY azure-identity-cpp CONFIG)
    if(azure-core-cpp_FOUND AND azure-identity-cpp_FOUND)
        set(KYTHIRA_HAS_AZURE_SDK TRUE)
        target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_AZURE_SDK)
        target_link_libraries(network_simulator INTERFACE Azure::azure-core Azure::azure-identity)
    else()
        message(STATUS "Azure SDK not found — azure_vm_quorum_manager and azure_vmss_quorum_manager disabled")
    endif()

    # ── Azure Key Vault Keys (azure_key_vault_ca_provider) ───────────────────────
    # Independent of KYTHIRA_HAS_AZURE_SDK, mirroring AWS_ACM_PCA's independence
    # from AWS_SDK (CMakeLists.txt lines 557-582): an environment with the
    # quorum-manager SDK components but not Key Vault Keys (or vice versa)
    # still builds everything except the component it lacks.
    kythira_find_optional(AZURE_KEY_VAULT azure-security-keyvault-keys-cpp CONFIG)
    if(azure-security-keyvault-keys-cpp_FOUND)
        set(KYTHIRA_HAS_AZURE_KEY_VAULT TRUE)
        target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_AZURE_KEY_VAULT)
        target_link_libraries(network_simulator INTERFACE Azure::azure-security-keyvault-keys)
    else()
        message(STATUS "Azure Key Vault Keys SDK not found — azure_key_vault_ca_provider disabled")
    endif()
    ```
    Confirm the exact `find_package` config-mode target names
    (`azure-core-cpp_FOUND` vs. an `AZURESDK_FOUND`-style aggregate variable)
    against whichever `azure-core-cpp`/`azure-identity-cpp`/
    `azure-security-keyvault-keys-cpp` vcpkg port versions land in
    `vcpkg.json` — the AWS section's `AWSSDK_FOUND` is a single aggregate
    variable `find_package(AWSSDK COMPONENTS ...)` produces; Azure's CMake
    config packages are typically one `find_package()` per library with no
    umbrella aggregate, so the conditional structure above may need
    adjusting once the real `.cmake` config files are inspected.
  - Add `vcpkg.json` entries for `azure-core-cpp`, `azure-identity-cpp`,
    `azure-security-keyvault-keys-cpp` (as `"default-features": false"` ports
    with only needed features enabled, matching this project's existing
    minimal-feature-set convention for its other optional vcpkg deps).
  - Add a `"Azure Integration"` menu to `Kconfig` (parallel to the existing
    `"AWS Integration"` menu, `Kconfig` lines 91–110):
    ```kconfig
    menu "Azure Integration"

    config AZURE_SDK
    	bool "Azure SDK for C++ (VM/VMSS quorum managers)"
    	default y
    	help
    	  find_package(azure-core-cpp CONFIG) + find_package(azure-identity-cpp CONFIG).
    	  Backs KYTHIRA_HAS_AZURE_SDK.

    config AZURE_KEY_VAULT
    	bool "Azure Key Vault Keys (CA provider)"
    	depends on AZURE_SDK
    	default y
    	help
    	  find_package(azure-security-keyvault-keys-cpp CONFIG) — independently
    	  found even though it depends on AZURE_SDK for credential types.
    	  Backs KYTHIRA_HAS_AZURE_KEY_VAULT.

    endmenu # Azure Integration
    ```
  - Add `DEPENDENCIES.md` entries per requirements.md Requirement 1 AC 7,
    following the AWS SDK / AWS ACM Private CA entries' Status/Purpose/Notes
    format (`DEPENDENCIES.md` lines 79–87).
  - Verify: `cmake --build build` succeeds with and without the Azure SDK
    components present.
  - _Requirements: 1.1–1.7_

- [ ] 2. Define `azure_client_config` and shared placement/priority types
  - Create `include/raft/azure_client_config.hpp` (no SDK guard on the struct
    itself; the `credential` field and `make_default_credential_chain()` free
    function are guarded by `#ifdef KYTHIRA_HAS_AZURE_SDK`), per design.md's
    "Components and Interfaces" § 1.
  - `azure_client_config` SHALL be an aggregate (no user-declared
    constructors), matching `aws_client_config`.
  - Implement `make_default_credential_chain()` building a
    `Azure::Identity::ChainedTokenCredential` from `EnvironmentCredential`,
    `ManagedIdentityCredential`, `AzureCliCredential`, in that order (Req 2.2).
  - Verify: header compiles cleanly with and without the SDK present.
  - _Requirements: 2.1–2.4_

- [ ] 3. Implement `azure_vm_quorum_manager`
  - Create `include/raft/azure_vm_quorum_manager.hpp` with the full class
    body inside `#ifdef KYTHIRA_HAS_AZURE_SDK`:

  **Shared placement/priority/image types** (Req 16.1–16.2, 18.1–18.2, 3.2):
  - `azure_placement_kind` enum, `azure_placement_config` struct
  - `azure_vm_priority` enum, `azure_eviction_policy` enum, `azure_spot_options` struct
  - `azure_image_reference` struct (Marketplace 4-tuple XOR shared-gallery ID)

  **Configuration struct** (Req 3.1): `azure_vm_quorum_manager_config` per
  design.md's "Configuration struct" § 2, including `placement_by_group`,
  `priority`/`spot_options`.

  **Constructor** (Req 3.3):
  - Validate `cluster_name`, `azure.subscription_id`, `azure.resource_group`,
    `azure.location` non-empty; `node_port` non-zero; `image_reference`
    well-formed (exactly one of the two forms populated); every
    `topology.groups[i].group_id` has an entry in `subnet_id_by_group` →
    throw `std::invalid_argument` on violation
  - Build `_arm_base` from `azure.subscription_id`/`azure.resource_group`/
    `azure.arm_endpoint_override`
  - Construct `_pipeline`: bearer-token policy wrapping
    `azure.credential` (or `make_default_credential_chain()` when null),
    retry policy, `azure.api_timeout`

  **Private helpers** (design.md § "Class sketch" + "NodeId derivation"):
  - `node_id_to_vm_name(nid)` / `vm_name_to_node_id(name)`: pure string
    computations, no ARM call (Req 5.3)
  - `next_node_id()`: `GET .../virtualMachines?$filter=tagName eq
    'kythira:cluster' and tagValue eq '{cluster_name}'` (all power states);
    scan `kythira:node-id` tags; return max-parsed + 1 (or 1 if empty) (Req 5.4)
  - `arm_get`/`arm_put`/`arm_delete` (delete treats 404 as success)/`poll_lro`
  - `apply_placement_fields(body, target_group)`: sets `zones` or
    `properties.proximityPlacementGroup`/`availabilitySet` per
    `placement_by_group[target_group]` (Req 16.4–16.6)
  - `apply_priority_fields(body)`: sets `priority`/`evictionPolicy`/
    `billingProfile.maxPrice` when `priority == spot` (Req 18.4)
  - `render_custom_data(nid, group)`: substitutes `{NODE_ID}`, `{NODE_PORT}`,
    `{CLUSTER}`, `{GROUP}`; base64-encodes
  - `compute_quorum_status(live, total)`: identical logic to
    `aws_ec2_quorum_manager`'s helper of the same name

  **`assess_quorum`** (Req 6.1–6.9, design.md sequence):
  - `fiu_do_on("raft/azure/vm/get_instance_view", throw ...;)`
  - Empty cluster → immediate healthy Future, no ARM call
  - Bounded-concurrency `GET .../instanceView` per node; 404 → unreachable;
    other error → abort whole call with exceptional Future; else check for
    `PowerState/running` in `statuses`
  - Build `live_map`/`group_live`; construct `placement_group_health` entries
    from `_cfg.topology`; return `quorum_health`

  **`maintain_quorum`** (Req 19.1–19.3):
  - `fiu_do_on("raft/azure/vm/maintain_quorum", throw ...;)`
  - Same six-step assess/decommission/deficit-compute/provision pattern as
    `aws_ec2_quorum_manager::maintain_quorum`

  **`provision_node`** (Req 7.1–7.9, 16.4–16.7, 18.4–18.5, design.md sequence):
  - `fiu_do_on("raft/azure/vm/create_vm", throw ...;)`
  - Validate `target_group` in `subnet_id_by_group`
  - `next_node_id()` → `new_id`; `vm_name = node_id_to_vm_name(new_id)`
  - `PUT` NIC; on failure return exceptional Future (nothing else created)
  - Render + base64-encode `custom_data_template`
  - Build VM `PUT` body: hardware/storage/os/network profiles, tags,
    placement fields, priority fields
  - `PUT` VM; on failure best-effort `DELETE` the NIC, return exceptional Future
  - Poll `instanceView` until `PowerState/running` or `provision_timeout`;
    on timeout best-effort `DELETE` VM then NIC, return exceptional Future
  - Re-`GET` NIC for `privateIPAddress`; return `peer_info{new_id, ip+":"+port}`

  **`decommission_node`** (Req 8.1–8.7, design.md sequence):
  - `fiu_do_on("raft/azure/vm/delete_vm", throw ...;)`
  - `vm_name = node_id_to_vm_name(node_id)` (no ARM call)
  - `DELETE` VM; 404 → treat as success; other failure → exceptional Future
  - `poll_lro` the `Azure-AsyncOperation` URL (30s budget)
  - `DELETE` NIC; log (don't propagate) any failure including 404-is-fine
  - Return resolved Future

  **`topology()`** (Req 9.1–9.2): return `_cfg.topology`; synchronous, no ARM calls.

  **`static_assert`** (Req 4.1):
  ```cpp
  static_assert(quorum_manager<azure_vm_quorum_manager<std::uint64_t, std::string>,
                               std::uint64_t, std::string, std::string>);
  ```

  - Verify: `cmake --build build` succeeds with the Azure SDK present;
    coverage build does not regress the coverage floor.
  - _Requirements: 3.1–3.3, 4.1–4.5, 5.1–5.5, 6.1–6.9, 7.1–7.9, 8.1–8.7,
    9.1–9.2, 15.1–15.4, 15.11, 16.1–16.8, 18.1–18.6, 19.1–19.3_

- [ ] 4. Implement `azure_vmss_quorum_manager`
  - Create `include/raft/azure_vmss_quorum_manager.hpp` with full class body
    inside `#ifdef KYTHIRA_HAS_AZURE_SDK`:

  **Configuration struct** (Req 10.1): `azure_vmss_quorum_manager_config`
  per design.md § 3.

  **Constructor** (Req 10.2–10.3):
  - Validate `cluster_name` non-empty, `scale_set_by_group` non-empty,
    `node_port` non-zero, every `topology.groups[i].group_id` in
    `scale_set_by_group`
  - For each scale set in `scale_set_by_group`: `GET
    .../virtualMachineScaleSets/{name}`; verify `upgradePolicy.mode !=
    "Automatic"`; throw `std::invalid_argument` otherwise
  - Construct `_pipeline` (same shape as task 3; may share a helper
    constructor function with `azure_vm_quorum_manager` if convenient, though
    no shared base class per design.md's "Shared Private Helpers" note)

  **Private helpers**:
  - `next_node_id()`: identical cluster-wide tag-scan as
    `azure_vm_quorum_manager` (Req 13.4) — copy, do not attempt to share via
    inheritance (matches the AWS design's explicit non-sharing decision)
  - `find_instance(node_id)`: scans every scale set in `scale_set_by_group`
    for an instance whose `kythira:node-id` tag matches; returns
    `optional<pair<scale_set_name, instance_id>>`
  - `compute_quorum_status`: same as task 3

  **`assess_quorum`** (Req 12.1–12.7, design.md sequence):
  - `fiu_do_on("raft/azure/vmss/list_instances", throw ...;)`
  - Empty cluster → immediate healthy Future
  - Per scale set: `GET .../virtualMachines?$expand=instanceView`; build
    `live_map` keyed by each instance's `kythira:node-id` tag (absent tag →
    skip, not yet claimed by a kythira node)
  - Classify cluster vector nodes; build per-group health; return `quorum_health`

  **`maintain_quorum`** (Req 19.1–19.3): same six-step pattern, fault point
  `"raft/azure/vmss/maintain_quorum"`.

  **`provision_node`** (Req 13.1–13.7, design.md sequence):
  - `fiu_do_on("raft/azure/vmss/update_capacity", throw ...;)`
  - Validate `target_group` in `scale_set_by_group`
  - `GET` scale set → `orig_capacity`; `PATCH sku.capacity = orig_capacity + 1`
  - Poll instance list for a `PowerState/running` instance lacking
    `kythira:node-id`; on timeout `PATCH` capacity back to `orig_capacity`
    (best-effort), return exceptional Future
  - `next_node_id()`; `PATCH` the found instance's tags
  - Read the instance's NIC private IP; return `peer_info{new_node_id, ip+":"+port}`

  **`decommission_node`** (Req 14.1–14.6, design.md sequence):
  - `fiu_do_on("raft/azure/vmss/delete_instance", throw ...;)`
  - `find_instance(node_id)` → not found → resolved Future (idempotent)
  - `POST .../delete {instanceIds:[...]}`; "not found"-shaped error →
    resolved Future; other failure → exceptional Future
  - Poll instance list (30s budget) until the instance ID no longer appears

  **`topology()`**: return `_cfg.topology`.

  **`static_assert`**:
  ```cpp
  static_assert(quorum_manager<azure_vmss_quorum_manager<std::uint64_t, std::string>,
                               std::uint64_t, std::string, std::string>);
  ```

  - Verify: `cmake --build build` succeeds with the Azure SDK present.
  - _Requirements: 10.1–10.3, 11.1–11.5, 12.1–12.7, 13.1–13.7, 14.1–14.6,
    15.5–15.8, 15.11, 19.1–19.3_

- [ ] 5. Implement `azure_key_vault_ca_provider`
  - Create `include/raft/azure_key_vault_ca_provider.hpp` (public
    declaration) and `include/raft/azure_key_vault_ca_provider_impl.hpp`
    (implementation), both inside `#ifdef KYTHIRA_HAS_AZURE_KEY_VAULT`,
    following the `aws_acm_pca_provider.hpp`/`_impl.hpp` split.

  **Types and config** (Req 17.1): `azure_key_vault_signing_algorithm` enum,
  `azure_key_vault_ca_provider_config` struct per design.md § 4.

  **Constructor**:
  - Validate `vault_url`, `key_name`, `ca_certificate_pem` non-empty → throw
    `std::invalid_argument` otherwise
  - Construct `Azure::Security::KeyVault::Keys::KeyClient` against
    `vault_url` with `azure.credential` (or
    `make_default_credential_chain()` when null)

  **`root_certificate_pem()`** (Req 17.3):
  - `fiu_do_on("raft/azure/keyvault/get_key", throw ...;)`
  - Return `config.ca_certificate_pem` wrapped in an immediately-resolved
    Future (no network call, per the requirement's explicit carve-out)

  **`sign_csr(csr_pem, options)`** (Req 17.4–17.5, design.md sequence,
  option (a) from "Sharing the X.509-building code path"):
  - `fiu_do_on("raft/azure/keyvault/sign", throw ...;)`
  - Parse and validate the CSR (OpenSSL `X509_REQ`), reusing/duplicating the
    minimal subset of `certificate_authority::sign_csr()`'s CSR-validation
    logic needed here (design.md explicitly scopes this as duplication, not
    a shared refactor, for this iteration)
  - Build and DER-encode the leaf TBSCertificate (issuer from
    `ca_certificate_pem`'s subject, subject/SAN/keyUsage from `options`,
    validity from `config.validity`, public key from the CSR)
  - Hash the TBSCertificate DER per `config.signing_algorithm`
  - Call `_client.Sign(config.key_name, config.key_version, <algorithm>,
    digest)`; on failure return exceptional Future with the Key Vault error
  - Assemble the final certificate (TBS DER + algorithm identifier +
    returned signature) → DER → PEM
  - Return `pem_material{.certificate_pem = ..., .private_key_pem = ""}`

  **No `revoke()`** (Req 17.6): do not implement; document the scope
  decision in a header comment referencing `aws_acm_pca_provider`'s
  identical carve-out.

  **`static_assert`** (Req 17.2):
  ```cpp
  static_assert(certificate_provider<azure_key_vault_ca_provider>);
  ```

  - Verify: `cmake --build build` succeeds with Key Vault Keys SDK present;
    a manually-constructed CSR/cert round-trip validates against a
    self-signed test CA key held in a real or stubbed vault.
  - _Requirements: 17.1–17.7, 15.9–15.11_

- [ ] 6. Unit tests for both quorum managers
  - Create `tests/azure_quorum_manager_unit_test.cpp`
  - Register in `tests/CMakeLists.txt` as `azure_quorum_manager_unit_test`,
    guarded by `if(KYTHIRA_HAS_AZURE_SDK)`, labels `unit;raft;quorum;azure`,
    30s per-test timeout
  - Implement `stub_http_transport_policy` (design.md "Unit tests (no Azure
    dependency)"): a small `Azure::Core::Http::Policies::HttpPolicy` matching
    on URL/method, returning canned JSON bodies/status codes; used by any
    test needing to assert request shape without live credentials

  **Concept satisfaction**:
  - `concept_vm_satisfied` / `concept_vmss_satisfied`: instantiate each type
    (the `static_assert`s already do the real compile-time check)

  **Construction validation** (Req 20.3):
  - `vm_empty_cluster_name_throws`, `vm_empty_subscription_id_throws`,
    `vm_empty_resource_group_throws`, `vm_empty_location_throws`,
    `vm_zero_node_port_throws`
  - `vm_image_reference_both_forms_set_throws`,
    `vm_image_reference_neither_form_set_throws`
  - `vm_missing_subnet_for_topology_group_throws`
  - `vmss_empty_cluster_name_throws`, `vmss_empty_scale_set_by_group_throws`,
    `vmss_missing_scale_set_for_topology_group_throws`

  **Unknown-group provision futures** (Req 20.3):
  - `vm_provision_unknown_group_returns_exceptional_future`
  - `vmss_provision_unknown_group_returns_exceptional_future`

  **NodeId ↔ name round-trip** (Req 20.3):
  - `node_id_to_vm_name_round_trip`: verify
    `vm_name_to_node_id(node_id_to_vm_name(id)) == id` for representative IDs
    (0, 1, a large value near `uint64_t` max)

  **Placement/priority config** (Req 16.8, 18):
  - `placement_config_ppg_valid`: construct `azure_placement_config{.kind =
    proximity_placement_group, .resource_id = "..."}`, verify fields
  - `spot_options_default_is_regular`: verify `priority == regular` and
    `spot_options == std::nullopt` by default
  - `spot_options_populates_correctly`: construct
    `azure_spot_options{.max_price = 0.05, .eviction_policy = delete_vm}`,
    verify fields

  **Fault injection** (Req 20.5):
  - `vm_assess_quorum_fault`, `vm_provision_node_fault`,
    `vm_decommission_node_fault`, `vm_maintain_quorum_fault`
  - `vmss_assess_quorum_fault`, `vmss_provision_node_fault`,
    `vmss_decommission_node_fault`, `vmss_maintain_quorum_fault`
  - Each: `fiu_enable("raft/azure/.../...")`, call the method, verify
    exceptional Future, `fiu_disable`

  - Verify: `ctest -R azure_quorum_manager_unit_test` passes; no regression
    in any existing test.
  - _Requirements: 20.1–20.3, 20.5_

- [ ] 7. Unit tests for `azure_key_vault_ca_provider`
  - Create `tests/azure_key_vault_ca_provider_unit_test.cpp`
  - Register in `tests/CMakeLists.txt` as
    `azure_key_vault_ca_provider_unit_test`, guarded by
    `if(KYTHIRA_HAS_AZURE_KEY_VAULT)`, labels
    `unit;certificate_authority;ca;azure;keyvault`

  **Concept satisfaction**: instantiate `azure_key_vault_ca_provider`
  (`static_assert` already covers the real check).

  **Construction validation**: empty `vault_url`/`key_name`/
  `ca_certificate_pem` each throw `std::invalid_argument`.

  **`root_certificate_pem`**: returns the configured PEM unmodified,
  wrapped in a resolved Future.

  **`sign_csr` happy path**: using `generate_key_and_csr` from
  `certificate_provider.hpp` to build a test CSR, stub the Key Vault `Sign`
  call (via the same `stub_http_transport_policy` approach as task 6, since
  `KeyClient` is itself built over `Azure::Core::Http::HttpPipeline`) to
  return a signature produced by a locally-held test key matching
  `ca_certificate_pem`'s public key, and verify the assembled certificate's
  signature validates via OpenSSL.

  **Fault injection**: `keyvault_sign_fault`, `keyvault_get_key_fault` per
  Requirement 15 AC 9–10.

  - Verify: `ctest -R azure_key_vault_ca_provider_unit_test` passes.
  - _Requirements: 20.1, 20.4–20.6_

- [ ] 8. Real-Azure integration tests
  - Create `tests/azure_quorum_manager_real_test.cpp` (VM + VMSS suites) and
    `tests/azure_key_vault_ca_provider_real_test.cpp`, both guarded by
    `#ifdef KYTHIRA_AZURE_REAL_TESTS`
  - Register in `tests/CMakeLists.txt`, guarded by the relevant
    `KYTHIRA_HAS_AZURE_*` flag, labels `integration;azure;real-azure;slow`,
    600s per-test timeout

  **Implement `AzureIntegrationFixture`** (design.md § "Integration test
  fixture"):
  - Credential pre-flight: `GET .../resourceGroups/{AZURE_TEST_RESOURCE_GROUP}`;
    skip the entire suite (not fail) on any failure, including missing
    `AZURE_SUBSCRIPTION_ID`/`AZURE_TEST_RESOURCE_GROUP` env vars
  - Generate UUID; derive `test_run`, `cluster_name` from it
  - Setup: VNet/subnets (3 zones)/NSG per design.md's setup sequence, each
    using the corresponding `AZURE_TEST_*` env var override when present
  - Teardown (destructor, unconditional, best-effort, errors → `std::cerr`):
    VMs/VMSS instances + NICs → VMSS resource(s) → subnets → NSG → VNet, in
    that order; never touches the resource group itself

  **`azure_vm_quorum_manager` test cases** (Req 20.13):
  - `provision_and_assess_single_zone`
  - `provision_multi_zone_topology`
  - `deallocate_one_node_degraded` (external `POST .../deallocate`, verify
    `degraded`)
  - `decommission_idempotent`
  - `placement_availability_zone`, `placement_proximity_placement_group`,
    `placement_availability_set` (one test per `azure_placement_kind`)
  - `provision_timeout_cleanup`
  - `spot_provision_and_decommission` (Req 18)
  - `zone_outage_during_rolling_deployment`: 3-zone × 3-node topology;
    deallocate all zone-3 nodes plus one zone-2 node; verify `critical`;
    call `maintain_quorum`; verify topology-correct per-zone replacement and
    a subsequent `healthy` assessment — structurally the VM-manager analogue
    of the AWS spec's `az_outage_during_rolling_deployment`

  **`azure_vmss_quorum_manager` test cases** (mirroring the AWS ASG
  LocalStack-tier cases, but against real scale sets since no emulator
  tier exists here):
  - `vmss_provision_increments_capacity`
  - `vmss_assess_detects_not_running`
  - `vmss_decommission_removes_instance`
  - `vmss_decommission_idempotent`
  - `vmss_rejects_automatic_upgrade_mode` (Req 10.3): construct against a
    scale set with `upgradePolicy.mode = "Automatic"`; verify
    `std::invalid_argument`

  **`azure_key_vault_ca_provider` test case** (Req 20.14):
  - `sign_csr_against_real_vault`: generate CSR, sign via the real
    `AZURE_TEST_KEY_VAULT_URL`/`AZURE_TEST_KEY_VAULT_KEY_NAME`, verify the
    resulting certificate's signature with OpenSSL
  - `fault_points_short_circuit_before_network_call`: enabling
    `"raft/azure/keyvault/sign"`/`"raft/azure/keyvault/get_key"` rejects the
    Future without needing the vault to actually be reachable

  - Verify: when `KYTHIRA_AZURE_REAL_TESTS` is unset, both binaries compile
    but are not registered in CTest; all other tests pass unmodified.
  - _Requirements: 20.8–20.14_

- [ ] 9. Wire `ca_service --provider azure-key-vault` and update documentation
  - In `cmd/ca_service/` source, add `--provider azure-key-vault` alongside
    `local`/`aws-acm-pca`, plus `--key-vault-url`, `--key-vault-key-name`,
    `--ca-cert-file` flags, following the existing
    `any_certificate_provider` type-erasure pattern (Req 17.7)
  - Update `README.md`:
    - Move Azure out of the "What's In Progress" bullet
      ("Additional cloud providers: Azure, GCP, OCI, and Alibaba Cloud
      quorum managers / certificate providers — AWS is implemented today")
      and into a new "What's Ready" bullet once this plan's tasks 1–8 are
      complete
    - Add an "Azure Quorum Managers & Certificate Provider" subsection
      summarizing all three components at the level of detail the
      "Certificate Authority & ACME" section gives AWS ACM Private CA
  - Update `DEPENDENCIES.md` per task 1 (if not already done there)
  - Verify: `cmake --build build --target format-check` passes on all new
    files; existing AWS-related README sections are left unmodified except
    for the "What's In Progress"/"What's Ready" bullet move
  - _Requirements: 17.7, 21.1–21.3_

## Notes

- **Azure SDK for C++ has no process-wide init/shutdown call** (unlike
  `Aws::InitAPI()`/`Aws::ShutdownAPI()`), so unlike the AWS spec's tasks.md
  Notes, no `AzureSdkFixture`-equivalent construct is needed purely for SDK
  lifecycle; test fixtures still need their own setup/teardown for the
  Azure *resources* they create, per design.md's `AzureIntegrationFixture`.

- **`next_node_id()` has the same TOCTOU race as its AWS counterpart** if two
  leaders simultaneously call `provision_node`. The quorum management spec's
  concurrency rules (`.kiro/specs/quorum-management/`, Requirements
  14.3–14.4 in the AWS spec's terms) already prevent this: a single manager
  instance is never called concurrently for the same slot. No additional
  locking is required here either.

- **Confirm exact Azure SDK for C++ CMake target/variable names before
  finalizing task 1.** This spec's CMake snippets are written against the
  documented/typical shape of Azure SDK for C++ vcpkg config-mode packages
  (`find_package(azure-core-cpp CONFIG)` → `Azure::azure-core`), but the
  precise `_FOUND` variable names and whether an umbrella
  `find_package(AzureSDK COMPONENTS ...)` similar to `AWSSDK` exists should
  be verified against the actual vcpkg port contents at implementation time,
  the same "verify against reality, don't trust the spec draft blindly"
  discipline the AWS spec's tasks.md itself called out needing after the
  fact for its own tracking doc.

- **The `tbs_certificate_builder` refactor (design.md option (b)) is
  intentionally deferred.** Task 5 implements option (a) (duplicate the
  minimal CSR-parsing/TBSCertificate-assembly subset). If a second
  remote-signing backend (e.g. a future GCP Cloud KMS-backed provider) is
  ever added, revisit extracting a shared signing-backend seam in
  `certificate_authority.hpp` at that point rather than now, when there is
  only one consumer to design the seam's shape around.

- **No LocalStack-equivalent emulator tier exists for this spec** (design.md
  "No emulator tier"). Do not attempt to add one as part of implementing
  this plan; if a credible ARM Compute/VMSS/Key Vault emulator becomes
  available later, adding a middle tier is a separate, follow-up spec.

- **IAM/RBAC permissions needed for real-Azure integration tests.** The
  service principal or user identity running the real-Azure suite needs, at
  minimum, the built-in **Virtual Machine Contributor**, **Network
  Contributor**, and (for the CA provider suite) **Key Vault Crypto User**
  roles scoped to `AZURE_TEST_RESOURCE_GROUP` (and the pre-existing vault,
  for the last one). A sample role-assignment script should be added to
  `doc/azure-test-rbac-setup.md`, mirroring the AWS spec's
  `doc/aws-test-iam-policy.json` companion document.

- **Cost estimation reporting** (mirroring the AWS spec's
  `aws_quorum_manager_real_ec2_test.cpp` `TestCostReport`/`CostAccumulator`
  machinery, Req 20 in that spec's tasks.md): once task 8 is implemented and
  real-world VM-hour costs for the test matrix are known, consider adding an
  equivalent `doc/azure_test_cost_estimate.md` and in-test cost reporting.
  Not required for this plan's initial landing; flagged here so it isn't
  forgotten the way the AWS spec's tracking doc admits its own status line
  briefly was.
