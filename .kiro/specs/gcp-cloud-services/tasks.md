# Implementation Plan — GCP Cloud Services Support

## Status: Complete — 13/13

**Last reconciled: August 4, 2026.** This header read "Not Started — this is
a design-only spec; no implementation exists yet" until that date, long after
every task had actually shipped *and* been verified against live GCP. It is
recorded here rather than quietly overwritten because this spec is now the
third documented instance of the same drift pattern (see
`doc/TODO.md`'s "A note on how this table stays honest", and the
`proxygen-http-transport` and `ion-rpc-serializer` cases): a status header is
not evidence, and should be re-derived from `include/`, `tests/` and a real
CI run before being trusted.

What exists, task by task, is marked ✅ below. Summary of the evidence:

- **Implementation**: `include/raft/gcp_client_config.hpp`,
  `gcp_operation_wait.hpp`, `gcp_compute_quorum_manager.hpp`,
  `gcp_mig_quorum_manager.hpp`, `gcp_privateca_certificate_provider{,_impl}.hpp`.
- **Build wiring**: the `"GCP Integration"` Kconfig menu (`GCP_SDK`,
  `GCP_PRIVATECA`), and a `gcp` vcpkg feature pulling `google-cloud-cpp`'s
  `compute` + `privateca` components. The feature is **opt-in**
  (`--x-feature=gcp`): the `compute` component builds the entire Compute API
  surface, so it is deliberately kept out of the default dependency set and
  exercised by a dedicated `GCP SDK Build (clang++-18, x64)` CI job.
- **`ca_service` integration**: `--provider gcp-privateca` (`cmd/ca_service/main.cpp`).
- **Tests**: `gcp_quorum_manager_unit_test` builds and runs *unconditionally*
  — its fake-client and pure-function suites give real coverage even without
  google-cloud-cpp, with the SDK-dependent suites becoming skip-only — and is
  present in the default CI build (run 30947491385's JUnit artifacts).
  `gcp_privateca_provider_unit_test` is gated on `_KYTHIRA_GCP_PRIVATECA_FOUND`
  and so runs only in the GCP SDK Build job, not the default one.
- **Live-GCP verification**: `gcp_quorum_manager_real_gce_test` 11/11 and
  `gcp_privateca_provider_real_test` passing against a real project via
  Workload Identity Federation (`scripts/ci-cloud-credentials/gcp/`), with a
  post-run audit showing no leaked instances, disks, MIGs or CA pools. That
  first live run surfaced three real defects, all since fixed: a bare network
  short name rejected by `instances.insert`, the CAS suite silently skipping
  while CTest reported the skip as a pass, and `provision_timeout_cleanup`
  never timing out while leaking its instance.
- **Documentation**: `doc/gcp_quorum_manager_README.md`.

**One gap that is *not* closed**, tracked in `doc/TODO.md`'s Cloud Provider
Support section rather than here: GCP ships no example configuration file
(`.env.example` or equivalent), which that section states as a requirement
for every provider. AWS and Azure have the same gap.

## Overview

Implement:
- `gcp_compute_quorum_manager` — direct GCE instance management (primary)
- `gcp_mig_quorum_manager` — Managed Instance Group management (production-grade)
- `gcp_privateca_certificate_provider` — `certificate_provider` backed by
  Google Cloud Certificate Authority Service

All three satisfy existing concepts (`quorum_manager` /
`certificate_provider`) unchanged. The quorum managers are header-only,
gated behind `KYTHIRA_HAS_GCP_SDK`; the certificate provider is gated
behind the independent `KYTHIRA_HAS_GCP_PRIVATECA`. All use `google-cloud-cpp`.

Reference implementations to study before starting:
- `include/raft/aws_ec2_quorum_manager.hpp` / `aws_asg_quorum_manager.hpp` —
  closest structural analogues for the two quorum managers
- `include/raft/aws_acm_pca_provider.hpp` — closest structural analogue for
  the certificate provider
- `include/raft/quorum_management.hpp` — concept definition and shared types
- `include/raft/certificate_provider.hpp` — concept definition
- `include/raft/fault_injection.hpp` — `fiu_do_on` macro usage
- `tests/acme_test_server.hpp` — precedent for a hand-written test double
  standing in for a cloud API the project has no LocalStack-equivalent for
  (used here as the model for GCP's fake-client unit-test layer, since GCP
  has no widely available Compute Engine emulator — see `design.md`'s
  Testing Strategy section)
- `.kiro/specs/ci-real-cloud-tests/` — the CI toggle pattern this spec's
  Task 11 plugs into; its own design.md names this exact future spec as the
  place to fill in the `gcp` job skeleton.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 2, 3],
      "description": "Build system, shared config, and the zone-operation wait helper — prerequisites for both quorum managers"
    },
    {
      "wave": 2,
      "tasks": [4],
      "description": "gcp_compute_quorum_manager — no dependency on gcp_mig_quorum_manager"
    },
    {
      "wave": 3,
      "tasks": [5],
      "description": "gcp_mig_quorum_manager — shares helpers with task 4 but is independent of it"
    },
    {
      "wave": 4,
      "tasks": [6],
      "description": "gcp_privateca_certificate_provider — independent of both quorum managers, only depends on wave 1's build-system and gcp_client_config work"
    },
    {
      "wave": 5,
      "tasks": [7],
      "description": "ca_service --provider gcp-privateca integration — depends on task 6"
    },
    {
      "wave": 6,
      "tasks": [8, 9, 10],
      "description": "Unit tests for each of the three components — can run in parallel once their respective wave completes"
    },
    {
      "wave": 7,
      "tasks": [11, 12],
      "description": "Real-GCP integration test fixtures and test cases, plus CI wiring for the gcp job (task 11)"
    },
    {
      "wave": 8,
      "tasks": [13],
      "description": "Documentation"
    }
  ]
}
```

---

## Tasks

- [x] 1. Build System Detection

  - Add `find_package(google_cloud_cpp_compute QUIET COMPONENTS
    compute_instances compute_instance_group_managers compute_zone_operations)`
    to `CMakeLists.txt`; define `KYTHIRA_HAS_GCP_SDK` and propagate via
    `target_compile_definitions`.
  - Add a separate `find_package(google_cloud_cpp_privateca QUIET)` check;
    define `KYTHIRA_HAS_GCP_PRIVATECA` independently.
  - Add `google-cloud-cpp` to `vcpkg.json` with features `["compute",
    "privateca"]`.
  - Add the `"GCP Integration"` `Kconfig` menu (`GCP_SDK`, `GCP_PRIVATECA`
    depends-on `GCP_SDK`), mirroring the existing `"AWS Integration"` menu.
  - _Requirements: 1_

- [x] 2. `gcp_client_config` and Validators

  - Implement `include/raft/gcp_client_config.hpp`: the `gcp_client_config`
    aggregate, `is_valid_gcp_label`, `is_valid_gcp_resource_name`.
  - Unconditionally compiled (no SDK guard) — pure string/regex logic and
    standard-library types only.
  - _Requirements: 2, 3_

- [x] 3. Zone-Operation Wait Helper

  - Implement `include/raft/gcp_operation_wait.hpp`'s `wait_for_zone_operation`:
    poll `zoneOperations.get`, unwrap `error()`, distinguish timeout from a
    GCP-reported operation error, `fiu_do_on("raft/gcp/zone_operation/poll")`
    on every poll iteration.
  - _Requirements: 4, 20 (AC 9)_

- [x] 4. `gcp_compute_quorum_manager`

  - Implement `include/raft/gcp_compute_quorum_manager.hpp`:
    `gcp_placement_policy_config`, `gcp_compute_quorum_manager_config`,
    the manager class, `node_id_to_instance_name`/`instance_name_to_node_id`.
  - Construction validation (cluster_name/boot_disk_image non-empty, node_port
    non-zero, label/name-length validation, topology↔subnetwork_by_group
    coverage).
  - `assess_quorum`: per-zone `instances.list` with `kythira-cluster` label
    filter, live/unreachable classification, quorum_status + per-group health.
  - `provision_node`: NodeId generation + collision retry, `instances.insert`
    request construction (including placement policy and spot scheduling),
    `wait_for_zone_operation`, poll-to-`RUNNING`, timeout cleanup via
    `instances.delete`.
  - `decommission_node`: `instances.delete` + idempotent `NOT_FOUND` handling +
    `wait_for_zone_operation`.
  - `topology()`, `maintain_quorum()`.
  - Fault injection points per Requirement 20 ACs 1–3, 7.
  - `static_assert(quorum_manager<...>)`.
  - _Requirements: 5, 6, 7, 8, 9, 10, 11, 12, 19, 20 (ACs 1–3, 7)_

- [x] 5. `gcp_mig_quorum_manager`

  - Implement `include/raft/gcp_mig_quorum_manager.hpp`:
    `gcp_mig_quorum_manager_config`, the manager class,
    `find_instance_by_node_id`.
  - Construction validation (cluster_name/mig_by_group non-empty, node_port
    non-zero, topology↔mig_by_group coverage, autohealing-policy rejection via
    `instanceGroupManagers.get`).
  - `assess_quorum`: per-zone `instances.list` with `kythira-cluster` label
    filter, `kythira-node-id`-keyed live/unreachable classification.
  - `provision_node`: `instanceGroupManagers.get` (read targetSize) +
    `.resize(+1)` + `wait_for_zone_operation` + poll
    `listManagedInstances` for an unlabelled instance + NodeId generation +
    fingerprint-safe `instances.setLabels` (with retry on
    `FAILED_PRECONDITION`) + timeout rollback via `.resize` back down.
  - `decommission_node`: label-keyed lookup via `find_instance_by_node_id` +
    `instanceGroupManagers.deleteInstances` + idempotent `NOT_FOUND` handling +
    `wait_for_zone_operation`.
  - `topology()`, `maintain_quorum()`.
  - Fault injection points per Requirement 20 ACs 4–6, 8.
  - `static_assert(quorum_manager<...>)`.
  - _Requirements: 5, 13, 14, 15, 16, 17, 18, 19, 20 (ACs 4–6, 8)_

- [x] 6. `gcp_privateca_certificate_provider`

  - Implement `include/raft/gcp_privateca_certificate_provider.hpp` /
    `_impl.hpp`: `gcp_privateca_certificate_provider_config`, the provider
    class.
  - `root_certificate_pem()`: `GetCertificateAuthority` (or
    `ListCertificateAuthorities` + first `ENABLED` CA when
    `certificate_authority_id` is empty), cached after first success.
  - `sign_csr()`: `CreateCertificate` against the configured CA pool, template,
    and validity; no poll loop (synchronous issuance — see `design.md`).
  - Optional `revoke()`: `RevokeCertificate`.
  - Fault injection: `"raft/gcp/privateca/create_certificate"`,
    `"raft/gcp/privateca/get_certificate_authority"`.
  - `static_assert(certificate_provider<...>)`.
  - _Requirements: 21_

- [x] 7. `ca_service --provider gcp-privateca`

  - Extend `ca_service`'s `--provider` flag to accept `gcp-privateca`
    (compiled conditionally on `KYTHIRA_HAS_GCP_PRIVATECA`, with a clear error
    when selected on a build without it).
  - Add `--gcp-project`/`--gcp-location`/`--gcp-ca-pool`/
    `--gcp-endpoint-override` CLI arguments, mapped onto
    `gcp_privateca_certificate_provider_config`.
  - Apply the same `501 Not Implemented` convention as `aws-acm-pca` for routes
    the backend cannot serve.
  - _Requirements: 22_

- [x] 8. `gcp_compute_quorum_manager` Unit Tests

  - `tests/gcp_quorum_manager_unit_test.cpp` (shared file with task 9,
    `gcp-quorum-manager-unit-tests` CTest target, labels
    `unit;gcp;quorum_manager`).
  - Fake `InstancesClient`/`ZoneOperationsClient` connection layer.
  - Construction-validation tests, `node_id_to_instance_name`/
    `instance_name_to_node_id` round trip, `target_group` rejection, fault
    point coverage, `is_valid_gcp_label`/`is_valid_gcp_resource_name` tests.
  - _Requirements: 23 (ACs 1–5, 8–11)_

- [x] 9. `gcp_mig_quorum_manager` Unit Tests

  - Same file/target as task 8.
  - Fake `InstanceGroupManagersClient` in addition to task 8's fakes.
  - Construction-validation tests including the autohealing-policy rejection
    case, `target_group` rejection, fault point coverage.
  - _Requirements: 23 (ACs 1, 6–9)_

- [x] 10. `gcp_privateca_certificate_provider` Unit Tests

  - `tests/gcp_privateca_provider_unit_test.cpp`
    (`gcp-privateca-provider-unit-tests` CTest target, labels
    `unit;gcp;certificate_authority`).
  - Fake `CertificateAuthorityServiceClient` connection layer.
  - `sign_csr`/`root_certificate_pem` behavior, fault point coverage.
  - _Requirements: 23 (AC 12)_

- [x] 11. Real-GCP Quorum Manager Integration Tests + CI Wiring

  - `tests/gcp_quorum_manager_real_gce_test.cpp`, guarded by
    `KYTHIRA_GCP_REAL_TESTS=1`, excluded from default `ctest`, labels
    `integration;gcp;real-gce`.
  - Fixture: pre-flight credential/permission check (skip, not fail, on
    failure), per-run resource labelling (`kythira-test-run`), env-var-driven
    resource creation/reuse (project/region/image/network/subnet/service
    account), ordered teardown with error collection.
  - Test cases per Requirement 23 ACs 18–19 (both managers).
  - _Requirements: 23 (ACs 13–19)_

  **CI wiring** (design.md § "CI wiring"):
  - Port `aws_real_ec2_test_support.hpp`'s cost-tracking
    (`BilledResource`/`TestCostReport`/`CostAccumulator`/`CostSummaryFixture`)
    and signal-driven-cleanup apparatus to a GCP-flavored equivalent, re-priced
    against published GCE on-demand machine-type pricing (Req 24.4–24.5).
  - Replace the no-op `gcp` job body in `.github/workflows/real-cloud-tests.yml`
    with real steps: `google-github-actions/auth@v2` via Workload Identity
    Federation (impersonated service account, no JSON key), then
    `REAL_CLOUD_TESTS_GCP_QUORUM_MANAGER_ENABLED`/
    `REAL_CLOUD_TESTS_GCP_PRIVATECA_ENABLED`-gated `ctest -R` steps for
    `gcp_quorum_manager_real_gce_test`/`gcp_privateca_provider_real_test`,
    following the `aws` job's shape (Req 24.1–24.2).
  - Add `scripts/ci-cloud-credentials/gcp/` (provisioning script + IAM-binding
    `policies/*.json` fragments + `README.md`) (Req 24.3).
  - Verify: `workflow_dispatch` manual runs exercise the master-off,
    provider-off, all-bundles-off, and one-bundle-on toggle combinations,
    matching `ci-real-cloud-tests`'s own verification approach for the `aws`
    job.
  - _Requirements: 24.1–24.5_

- [x] 12. Real-GCP Certificate Provider Integration Test

  - `tests/gcp_privateca_provider_real_test.cpp`, guarded by
    `KYTHIRA_GCP_REAL_TESTS=1`, labels `integration;gcp;real-privateca`.
  - Fixture: creates a CA pool + self-signed root CA when `GCP_TEST_CA_POOL`
    is absent; tears it down after.
  - Test cases per Requirement 23 AC 20.
  - _Requirements: 23 (ACs 13–17, 20)_

- [x] 13. Documentation

  - Update `README.md`'s Production Readiness / Contributing sections.
  - Add `google-cloud-cpp` (compute + privateca) entries to `DEPENDENCIES.md`.
  - Write `doc/gcp_quorum_manager_README.md` covering the label/metadata
    scheme, the node-identity design decision, and config construction
    examples.
  - _Requirements: 25_

## Notes

- **Task format.** Until August 4, 2026 this file used `### Task N: Title`
  headings rather than the `- [x] N. Title` checkbox convention every other
  spec in `.kiro/specs/` uses. That is why automated sweeps that count
  `- [x]` / `- [ ]` lines reported this spec as `0/0` — indistinguishable
  from an empty spec, and a contributing reason the "Not Started" header
  survived as long as it did. Converted in place; the task text and numbering
  are unchanged.
- **The `gcp` vcpkg feature is opt-in.** `google-cloud-cpp`'s `compute`
  component builds the entire Compute API surface, which is large enough that
  adding it to the default dependency set would slow every build. Enable with
  `vcpkg install --x-feature=gcp`; CI exercises it in the dedicated
  `GCP SDK Build (clang++-18, x64)` job rather than the four default
  `Build & Test` legs.
- **Two different unit-test gating rules**, which is easy to misread as an
  inconsistency: `gcp_quorum_manager_unit_test` is built unconditionally
  (its fake-client and pure-function suites are real coverage without the
  SDK; SDK-dependent suites become skip-only), whereas
  `gcp_privateca_provider_unit_test` is gated on `_KYTHIRA_GCP_PRIVATECA_FOUND`
  and therefore appears only in the GCP SDK Build job.
- **Real-cloud tests cost money and are opt-in**
  (`-DKYTHIRA_GCP_REAL_TESTS=ON` plus the `.kiro/specs/ci-real-cloud-tests/`
  toggle). They self-provision and tear down their own instances, MIGs and CA
  pools; the August 2026 live run's post-run audit found no leaks, but re-run
  that audit after any change to the provisioning paths.
