# Implementation Plan — OCI Cloud Provider Support

## Status: Tasks 0-5 and 7 complete; Task 6 code-complete, pending one console-side step

**Task 6's code is done and live-verified** — both real-OCI suites pass against
a real tenancy (4/4 and 3/3), the shared harness encodes Findings 14/15's
classifier and pricing, and the `oci` CI job body is written. **The checkbox
stays open for one reason**: the job's credential step needs an OCI IAM
identity-domain trust for this repository (Workload Identity Federation,
Finding 17), which is console-side work, and until it exists the step fails by
design rather than falling back to the long-lived key Requirement 14.2
forbids. Task 6 — and with it `doc/TODO.md`'s bullet, per Requirement 15.3 —
should be ticked after that trust exists and the job has produced one green
run. A task whose CI job cannot yet execute is not complete, however finished
its code is.

**August 11, 2026.** Both components, the mock server, the test tier and the
documentation are on `main`, and every one of them has been exercised against a
**live OCI tenancy** — see `spike-notes.md`'s second, third and fourth passes.

**Task 6 — the real-OCI integration tier and CI wiring — is the only task left,
and it is no longer blocked on anything.** Task 0's last four sub-questions were
closed on the same day (Findings 13-17), including both of the ones Task 6
depended on: the out-of-host-capacity classifier and the CI federation
mechanism.

Requirement 1.6 (Instance Principal) is still a deliberate throwing stub, but
the reason has changed: the contract is now known (Finding 16), so what remains
is writing a federation client rather than discovering how one would work.

What landed for Tasks 2-5 and 7, August 11:

- `include/raft/oci_instance_pool_quorum_manager.hpp` (Task 2) — the full
  class: constructor validation plus a single `GetInstancePool` to read the
  pool's Availability Domains, `assess_quorum` with the `lifecycleState` +
  heartbeat/grace-period rule, `provision_node` with the size bump,
  untagged-instance poll, tag write and VNIC lookup, `decommission_node` with
  its idempotent early exits and post-detach consistency poll,
  `maintain_quorum`, `topology`, and all four fault points.
- `include/raft/oci_certificates_provider.hpp` (Task 3) — `sign_csr` via
  `MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA`, `root_certificate_pem` with its
  cache, `revoke` via `RevokeCertificateVersion`, three fault points.
- `tests/oci_mock_server.hpp` (Task 4) — an `httplib::Server` modelling the
  IaaS and Certificates routes with in-memory state. It replaces
  `freeformTags` rather than merging them, faithfully to OCI, which is what
  makes Property 1 testable at all.
- `tests/oci_quorum_manager_unit_test.cpp` (12 cases),
  `tests/oci_quorum_manager_mock_test.cpp` (10),
  `tests/oci_certificates_provider_mock_test.cpp` (9) — Task 5. All pass;
  `ctest -R '^oci'` is 5/5 including the two Task-1 suites.
- `docker/oci_quorum_manager/` (Task 7) — env example and README. This is the
  **first** provider in the tree to satisfy `doc/TODO.md`'s standing
  "every provider ships an example config" requirement; AWS, Azure and GCP
  still do not.
- `doc/TODO.md`'s OCI bullet is updated but **deliberately still `[ ]`**,
  because Requirement 15.3 conditions the tick on `tasks.md` being complete
  and Task 6 is not. The bullet says exactly what is missing.

**Verified, not assumed.** The kconfig gates were checked by configuring with
them off and counting: 5 targets and 5 CTest entries on, **0 and 0** off, and
turning only `CONFIG_OCI_QUORUM_MANAGER` off leaves the certificates target
and the two shared suites building — so the two flags are genuinely
independent rather than jointly gating. Three behavioural claims were
mutation-tested rather than only asserted: dropping the read half of
`merged_tags` fails exactly the read-merge-write case; making `is_live` ignore
the heartbeat fails exactly the stale-heartbeat case; deleting
`provision_node`'s rollback fails exactly the timeout case. Each mutation
killed its own case and no other.

### Four more places the spec did not match the tree or the API

Recorded in the same spirit as Requirement 11.3's correction below, because
each was found by checking rather than by assuming.

1. **Requirement 12.2's `subject`/`certificateRules` premise is wrong for the
   config type this spec uses.** It says `options`' subject/SAN fields
   populate `CreateCertificate`'s `subject`/`certificateRules`. Those fields
   belong to `ISSUED_BY_INTERNAL_CA` — the config type that generates the key
   internally. `MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA` carries
   `issuerCertificateAuthorityId` and `csrPem` and nothing about the name
   being certified, because the CSR *is* the name. Sending a second source of
   truth alongside it would be ignored at best and a 400 at worst.
   `sign_csr` therefore uses `options` for the validity window and for the
   certificate *resource's* display name only; the header says so at the
   function.
2. **`aws_asg_quorum_manager::next_node_id()` does not exist.** `design.md`
   and Requirements 6.4/7.1 all point at it as the pattern being mirrored.
   The real `aws_asg_quorum_manager` derives its NodeId from the EC2 instance
   ID via `aws_ec2_quorum_manager::ec2_id_to_node_id` and has no tag scan at
   all. The *reasoning* the spec attributes to it is still sound — an OCID is
   not a usable numeric seed — so the implementation does the max-of-parsed-
   tags-plus-one scan the spec describes. It is simply not a port of existing
   code, and a reader sent to that function to compare will not find one.
3. **NodeIds are reused after a decommission**, and cannot not be. The scan
   only sees instances the pool still lists, and `decommission_node` detaches
   its target, so removing the highest-numbered node hands that number to the
   next provision. Requirement 6.4 scopes the scan to
   `ListInstancePoolInstances` and no OCI call exposes a detached instance's
   tags, so this is a property of the design rather than a defect in the
   implementation of it. Documented on `next_node_id` and pinned by
   `maintain_quorum_replaces_a_dead_node_and_reports_pre_remediation_health`
   so it stays a known limitation instead of a future surprise.
4. **Requirement 13.5's mock route list is short by two.** `revoke` takes a
   serial, and OCI addresses a certificate version by `(certificate OCID,
   version number)` — a serial is neither. Resolving one to the other needs
   `ListCertificates` and `ListCertificateVersions`, which the list omits, so
   the mock implements them. The alternative was caching what `sign_csr`
   issued, which works only until the process restarts — precisely when a
   revocation is most likely to be needed.

**One deliberate deviation from the spec's letter.** Requirements 13.2/13.6
name the CTest targets `oci-quorum-manager-unit-tests`,
`oci-quorum-manager-mock-tests` and `oci-certificates-provider-mock-tests`,
with hyphens. **Zero** of this tree's existing `add_test` names use a hyphen,
including `oci_signing_unit_test` and `oci_http_client_unit_test` from Task 1.
They are registered as `oci_quorum_manager_unit_test`,
`oci_quorum_manager_mock_test` and `oci_certificates_provider_mock_test`
instead, matching the tree. Task 5's verify command becomes
`ctest -R '^oci'` rather than `ctest -R oci-`. The labels are as specified.

## Historical status: Task 1 (August 10, 2026)

**Done, August 10, 2026** — the API-key signing path, which `spike-notes.md`
Finding 1 unblocked:

- `include/raft/oci_client_config.hpp` — the aggregate, unconditionally
  compiled (Requirements 1.2–1.3).
- `include/raft/oci_signing.hpp` — `sign_request` for API-key auth
  (Requirements 1.4–1.5), plus `build_signing_string` and
  `signed_header_names` split out so the canonical form is directly
  assertable rather than only observable through a signature.
  `instance_principal_signer` is the named stub Requirement 1.6 asks for
  and throws pointing at `spike-notes.md` Task 0(b).
- `tests/oci_signing_unit_test.cpp` — 8 cases. This is Task 5 work pulled
  forward on purpose: nothing else in the build compiles these headers, so
  without it they would sit in the tree unbuilt by CI, which is
  indistinguishable from not having written them.

**Task 1 completed later the same day**:

- `include/raft/oci_http_client.hpp` — signed JSON requests, per-service host
  derivation, OCI `code`/`message` surfaced on non-2xx, and the single bounded
  429 retry (Requirements 1.7–1.9), with `tests/oci_http_client_unit_test.cpp`
  (10 cases) driving a real local `httplib::Server` rather than a stub.
- `CONFIG_OCI_QUORUM_MANAGER` / `CONFIG_OCI_CERTIFICATES_PROVIDER` in
  `Kconfig`, both defaulting to `y`, gated in `CMakeLists.txt` via
  `kythira_kconfig_gate` (Requirements 11.1–11.2). **No `kythira_kconfig_require`
  and no `find_package`**, deliberately: there is no OCI SDK to probe, so these
  answer "is OCI support wanted?" rather than "was a dependency found?", and a
  `require` would assert a condition that cannot be false.
- `DEPENDENCIES.md` (Requirement 11.3) — see the correction below.

**Requirement 11.3's premise was wrong, and the fix is broader than it asked
for.** It says to add a note "under the existing `httplib`/OpenSSL/`boost::json`
entries ... mirroring the existing note calling out
`acme_certificate_provider.hpp`". None of that existed: `DEPENDENCIES.md` had
**no cpp-httplib entry at all**, Boost's component list omitted `json`, and the
string "acme" appeared **zero** times in the file. So the cross-reference had
nothing to attach to. Rather than skip it or invent a mirror of a note that was
never written, the missing entries were added — cpp-httplib as the required
dependency it has always been, `json` added to Boost's components with the note
that it is a compiled library — and the OCI consumers recorded against those and
against OpenSSL. That is a documentation fix this spec happened to surface, not
OCI-specific work.

*(That paragraph was written when Tasks 2-7 were untouched; see the status
block at the top of this file for where they actually stand now.)*

## Overview

Implement `oci_instance_pool_quorum_manager` (`quorum_manager`) and
`oci_certificates_provider` (`certificate_provider`), both speaking the OCI
REST API directly over `httplib`/`boost::json`/OpenSSL — no vendor SDK
exists to wrap, unlike the AWS work this spec otherwise mirrors structurally.

Reference implementations to study before starting:
- `include/raft/aws_asg_quorum_manager.hpp` — closest structural analogue
  for the quorum manager (tag-based NodeId assignment, size-driven scaling)
- `include/raft/aws_acm_pca_provider.hpp` — closest structural analogue for
  the certificate provider
- `include/raft/acme_certificate_provider.hpp` /
  `include/raft/acme_certificate_provider_impl.hpp` — the existing precedent
  for "speak a cloud/CA wire protocol directly over `httplib`, no SDK"
- `include/raft/acme_jws.hpp` — existing precedent for hand-rolled request
  signing with its own golden-vector unit tests
- `include/raft/quorum_management.hpp` / `include/raft/certificate_provider.hpp`
  — the two concepts being satisfied
- `include/raft/fault_injection.hpp` — `fiu_do_on()` macro usage
- `.kiro/specs/aws-quorum-manager/` — requirements/design/tasks for the
  closest full sibling spec
- `.kiro/specs/ci-real-cloud-tests/` — the CI toggle pattern this spec's
  Task 6 plugs into; its own design.md names this exact future spec as the
  place to fill in the `oci` job skeleton

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 0,
      "tasks": [0],
      "description": "Spike — MUST run first and produce a real, dated spike-notes.md before any other task starts. Confirms OCI Request Signing Version 1's exact wire format, the Instance Principal metadata-service contract, and whether OCI Certificates Management accepts a caller-supplied CSR."
    },
    {
      "wave": 1,
      "tasks": [1],
      "description": "Shared foundation: oci_client_config, oci_signing, oci_http_client — everything else depends on this"
    },
    {
      "wave": 2,
      "tasks": [2, 3],
      "description": "The two components are independent of each other once the foundation exists"
    },
    {
      "wave": 3,
      "tasks": [4],
      "description": "oci_mock_server — needed before mock-backed tests in wave 4"
    },
    {
      "wave": 4,
      "tasks": [5],
      "description": "Unit tests + mock-server integration tests for both components"
    },
    {
      "wave": 5,
      "tasks": [6],
      "description": "Real-OCI integration tests + CI wiring — depends on everything above existing and passing"
    },
    {
      "wave": 6,
      "tasks": [7],
      "description": "Documentation, example config, doc/TODO.md checkbox"
    }
  ]
}
```

## Tasks

- [x] 0. **Spike: confirm OCI signing scheme, Instance Principal
      contract, and Certificates Management issuance model — partially
      **CLOSED, August 11, 2026 — all 7 sub-questions resolved.** The first
      pass (2026-07-28, desk research) closed (a)/(c)/(e); the fourth pass
      (2026-08-11, live tenancy + SDK source) closed (b)/(d)/(f)/(g). Three of
      those four contradicted what this spec assumed — see `spike-notes.md`
      Findings 13-17 and the corrections applied to `requirements.md` 1.6,
      4.1, 13.14 and 14.2, and to `design.md`'s Instance Principal section.**

  This task produces a dated `spike-notes.md` in this spec directory,
  mirroring `.kiro/specs/boost-beast-http-transport/spike-notes.md`'s format
  (date, method, findings with concrete evidence, a Conclusions section
  listing what changed vs. what `requirements.md`/`design.md` assumed). It
  MUST contain genuine findings against current OCI API documentation
  and/or a live OCI tenancy — not a restatement of this spec's own
  assumptions. A first pass (desk research against
  `oracle/oci-go-sdk`/`oracle/oci-python-sdk`/`oracle/terraform-provider-oci`
  source, no live tenancy) resolved four of six sub-questions; the
  remaining two block Requirement 1.6 and Requirement 14.2 specifically and
  should be picked up before those two requirements' code is written
  (nothing else in Task 1–7 is blocked on them):

  a. [x] **Request Signing Version 1**: CONFIRMED — `spike-notes.md`
     Finding 1, sourced from `oci-go-sdk`'s `common/http_signer.go`. Exact
     canonical-string construction, header order (`date`,
     `(request-target)`, `host`, then body headers), and `authorization`
     header format are all recorded there and applied to `design.md`/
     `requirements.md` Requirement 1.1 in this same pass, correcting an
     earlier header-order assumption. Minor edge cases (multi-value
     headers, exact whitespace) remain unconfirmed but low-risk.
  b. [x] **Instance Principal auth**: CLOSED — `spike-notes.md` Finding 16.
     Metadata paths under `http://169.254.169.254/opc/v2`, and — the part the
     design had wrong — a **federation exchange is required**: an ephemeral
     session key, a token from the Auth service's X.509 endpoint, and
     `keyId = "ST$" + token`. Requirement 1.6 and `design.md` corrected. WAS: Exact instance metadata
     service endpoints/paths, refresh cadence, and whether a federation
     token exchange step is required were not investigated in the first
     pass. Next step: read `oci-go-sdk`'s/`oci-python-sdk`'s
     `auth`/`instanceprincipal` (or equivalent) packages the same way
     Finding 1 read `http_signer.go`, before implementing
     `instance_principal_signer` (Requirement 1.6) as more than a stub.
  c. [x] **Instance Pool per-AD growth**: CONFIRMED absent —
     `spike-notes.md` Finding 2, sourced from `terraform-provider-oci`'s
     `oci_core_instance_pool` resource docs. `size` is pool-wide; OCI's own
     algorithm distributes across placement configs. The one-pool-per-AD
     design (Requirement 6.2, `design.md` Non-Goals) is confirmed required,
     not a contingency.
  d. [x] **Freeform tag key character set**: CLOSED — `spike-notes.md`
     Finding 13. The colon **is** accepted; only `.` and space are rejected.
     Requirement 4.1's premise was false and is corrected; the hyphens stay,
     since the tag key is the manager's lookup key. WAS: Not investigated in
     the first pass.
  e. [x] **OCI Certificates Management CSR support**: CONFIRMED supported —
     `spike-notes.md` Finding 3, sourced from `oci-go-sdk`'s
     `create_certificate_managed_externally_issued_by_internal_ca_config_details.go`
     struct, cross-confirmed via the OCI CLI command reference. Config type
     `MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA` takes `issuerCertificateAuthorityId`
     + `csrPem`; the private key never leaves the caller. Requirement 12 is
     rewritten to this confirmed design; the Vault-export fallback is no
     longer needed. The certificate-revocation operation name is also
     corrected (Finding 4): `RevokeCertificateVersion`, not
     `ScheduleCertificateDeletion`.
  f. [x] **CI federation**: CLOSED — `spike-notes.md` Finding 17. OCI IAM
     Workload Identity Federation exchanges a GitHub OIDC JWT for a
     short-lived UPST, so **no long-lived API key and no sign-off are
     needed**. Not a Dynamic Group mechanism, as Requirement 14.2 assumed.
     WAS: Whether OCI supports federating
     GitHub Actions' OIDC tokens directly to a Dynamic Group was not
     investigated in the first pass. Needed before Requirement 14.2 can be
     implemented as designed rather than falling back to a long-lived API
     key (with the explicit sign-off that fallback requires).
  g. [x] **Capacity-error shape and preemptible pricing**: CLOSED —
     `spike-notes.md` Findings 14 and 15. The live error is `status 500`,
     `code InternalError`, `message "Out of host capacity."` — the code is
     generic and useless as a classifier, so the match must be on the message.
     Per-AD stockout confirmed directly (AD-2 refused while AD-1/AD-3
     launched). On-demand pricing is queryable from a public API; preemptible
     is not a separate SKU. WAS:
     2026-08-05 alongside Requirements 13.12–13.15. Record two things the
     escalation ladder cannot be written without:
     - The **exact** error code and message text OCI returns when a shape is
       unavailable in an Availability Domain (expected to be
       `Out of host capacity` / `OutOfHostCapacity`, but this is the
       classifier's entire basis and it is a string match — AWS's own
       `is_insufficient_capacity()` documents that the SDK had no mapped
       enum for its equivalent, so inspection alone is not sufficient here
       either). Record whether it arrives as an HTTP status, a structured
       `code` field, or only in the message body.
     - Whether OCI publishes preemptible **and** on-demand hourly shape
       pricing through a queryable API (the analogue of AWS's
       `DescribeSpotPriceHistory`), or whether the harness must carry a
       static price table as the Azure work does. This determines whether
       Requirement 13.12's "cheapest-first" ordering can be computed at
       runtime or must be maintained by hand.
     Blocks the escalation portion of Task 6 only; nothing else depends on it.

  - Verify: `spike-notes.md` exists, is dated, and each of (a)–(g) above has
    an explicit "confirmed as documented" or "corrected: <what's different>"
    finding, OR is explicitly still open with a concrete next step recorded
    (as (b)/(d)/(f) are above). Any correction updates
    `requirements.md`/`design.md` in the same commit, per Requirement 1.1 —
    already done for (a)/(c)/(e)/revocation-naming in this pass. A follow-up
    pass MUST close (b)/(d)/(f)/(g) before Task 1 (for (b)) or Task 6 (for
    (f) and (g)) reach their respective Instance-Principal / CI-federation /
    launch-escalation code; Tasks 1–5 and the rest of Task 6 are not blocked
    by (b)/(d)/(f)/(g).
  - _Requirements: 1.1, 12.1, 13.14, 14.2_

- [x] 1. **Shared foundation**: `oci_client_config`, `oci_signing`,
      `oci_http_client`

  - Create `include/raft/oci_client_config.hpp` (unconditional compilation,
    aggregate struct, per Requirement 1.2–1.3).
  - Create `include/raft/oci_signing.hpp`:
    - `sign_request()` per the scheme Task 0 confirmed (`spike-notes.md`
      Finding 1); throws `std::invalid_argument` when required API-key
      fields are missing and `use_instance_principal` is false (Req 1.5).
      This — the API-key signing path — is fully unblocked.
    - `instance_principal_signer` class: fetches, caches, and refreshes the
      short-lived certificate/key per the metadata-service contract
      (Req 1.6). **Blocked on Task 0(b)**, still open — implement as a
      stub that throws `std::runtime_error("not yet implemented — see
      spike-notes.md Task 0(b)")` until that sub-item is resolved, so the
      rest of this task and Tasks 2–5 (which don't require
      `use_instance_principal=true` to build or unit-test) are not blocked
      on it.
  - Create `include/raft/oci_http_client.hpp`:
    - `request()` derives the per-service host, signs via `oci_signing`,
      sends via `httplib::Client` (mirroring
      `acme_detail::make_client`'s timeout/TLS setup), parses the JSON
      response body, and throws `std::runtime_error` with the OCI error's
      `code`/`message` on non-2xx (Req 1.7–1.8).
    - One bounded retry on HTTP 429 honoring `retry-after` (Req 1.9).
  - Add the `KYTHIRA_OCI_QUORUM_MANAGER` / `KYTHIRA_OCI_CERTIFICATES_PROVIDER`
    kconfig flags (default enabled) via the existing
    `kythira_kconfig_gate`/`kythira_kconfig_require` machinery
    (`.kiro/specs/kconfig-integration/`); no `find_package` call is added.
  - Add the `DEPENDENCIES.md` cross-reference note (Req 11.3).
  - Verify: `cmake --build build` succeeds; `oci_signing`'s pure
    `sign_request` path is exercised against the golden vectors Task 0's
    spike produced (a throwaway scratch program is sufficient at this
    stage — the real unit test lands in Task 5).
  - _Requirements: 1.1–1.9, 11.1–11.3_

- [x] 2. **Implement `oci_instance_pool_quorum_manager`**

  Create `include/raft/oci_instance_pool_quorum_manager.hpp` with the full
  class body (always compiled; no SDK guard):

  **Constructor** (Req 2.2–2.3):
  - Validate `compartment_id`/`cluster_name`/`instance_pool_id` non-empty,
    `node_port` non-zero, and (when `!use_instance_principal`) the four
    API-key fields non-empty → `std::invalid_argument` on violation.
  - `GetInstancePool(instance_pool_id)` once; read `placementConfigurations`
    into `_availability_domains`; throw `std::runtime_error` on failure or
    not-found.

  **Private helpers** (design.md Components §3):
  - `node_id_str`, `next_node_id` (tag scan across
    `ListInstancePoolInstances`, all lifecycle states, max-parsed + 1),
    `find_instance_id` (tag scan for a matching `kythira-node-id`),
    `merged_tags` (read-merge-write per Property 1), `apply_tags`,
    `private_ip_of` (`ListVnicAttachments` + `GetVnic`),
    `compute_quorum_status` (same majority-threshold logic as the AWS
    managers).

  **`assess_quorum`** (Req 5.1–5.8, design.md sequence):
  - Empty cluster → resolved Future immediately, no API call.
  - `ListInstancePoolInstances` + per-instance `GetInstance` (concurrent,
    bounded worker pool).
  - Liveness rule: `lifecycleState == RUNNING` AND heartbeat/grace-period
    check (Req 5.3).
  - Build `live_map`/`group_live`; classify supplied cluster vector; build
    `placement_group_health`; compute `quorum_status`.

  **`maintain_quorum`** (Req 9.1–9.3):
  - Six-step sequence identical to `aws_ec2_quorum_manager::maintain_quorum`
    (assess → decommission unreachable → compute per-AD deficit → provision
    → return pre-remediation health), fault point
    `"raft/oci/instance_pool/maintain_quorum"`.

  **`provision_node`** (Req 6.1–6.9, design.md sequence):
  - Validate `target_group ∈ _availability_domains`.
  - `GetInstancePool` → `orig_size`; `UpdateInstancePool(size+1)`.
  - Poll `ListInstancePoolInstances` for an untagged instance until found or
    `provision_timeout`; on timeout, best-effort
    `UpdateInstancePool(size=orig_size)` rollback + exceptional Future.
  - `next_node_id()`; `merged_tags` + `apply_tags` (Req 4.1–4.3); resolve
    private IP via VNIC lookup; return `peer_info`.

  **`decommission_node`** (Req 7.1–7.8, design.md sequence):
  - `find_instance_id` → resolved Future if not found (idempotent).
  - Check `lifecycleState`; resolved Future if already
    `TERMINATING`/`TERMINATED`.
  - `DetachInstancePoolInstance(isDecrementSize=true, isAutoTerminate=true)`;
    post-detach consistency poll (30s) on `lifecycleState`.

  **`topology()`** (Req 8.1–8.2): return `_cfg.topology`, no API call.

  **`static_assert`** (Req 3.1): concept satisfaction check at file scope.

  - Verify: `cmake --build build` succeeds; headers included in the
    coverage build do not regress the coverage floor.
  - _Requirements: 2.1–2.3, 3.1–3.4, 4.1–4.4, 5.1–5.8, 6.1–6.9, 7.1–7.8,
    8.1–8.2, 9.1–9.3, 10.1–10.4_

- [x] 3. **Implement `oci_certificates_provider`**

  Create `include/raft/oci_certificates_provider.hpp`. **Task 0(e)
  confirmed** OCI accepts a caller-supplied CSR (`spike-notes.md` Finding
  3), so this task implements the single confirmed design (Requirement
  12.2), not a Task-0-gated branch:

  - `oci_certificates_provider_config` struct (Req 12.4): `oci`,
    `compartment_id`, `certificate_authority_id`, `validity`.
  - `root_certificate_pem()`: `GetCertificateAuthorityBundle`, cached after
    first success (Req 12.5).
  - `sign_csr(csr_pem, options)`: `CreateCertificate` with
    `certificateConfig.configType = "MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA"`,
    `issuerCertificateAuthorityId = config.certificate_authority_id`,
    `csrPem = csr_pem`; poll `GetCertificate`/`GetCertificateBundle` until
    `lifecycleState == "ACTIVE"` or `config.oci.api_timeout` elapses; return
    `pem_material` with `private_key_pem` empty (Req 12.2). No Vault/
    `secrets` call — the private key never leaves the caller.
  - `revoke(certificate_serial)`: call `RevokeCertificateVersion`
    (confirmed by `spike-notes.md` Finding 4 — corrects this spec's
    original `ScheduleCertificateDeletion` guess, which is a different
    operation — resource deletion, not version revocation).
  - Fault points: `"raft/oci/certificates/create_certificate"`,
    `"raft/oci/certificates/get_bundle"`, `"raft/oci/certificates/revoke"`
    (Req 12.7).
  - `static_assert(certificate_provider<oci_certificates_provider>)`.
  - Verify: `cmake --build build` succeeds.
  - _Requirements: 12.1–12.7_

- [x] 4. **Build `tests/oci_mock_server.hpp`**

  - An `httplib::Server`-based mock implementing exactly the routes Task 2
    and Task 3 call: `GetInstancePool`, `ListInstancePoolInstances`,
    `UpdateInstancePool`, `GetInstance`, `UpdateInstance`,
    `DetachInstancePoolInstance`, `ListVnicAttachments`, `GetVnic`,
    `CreateCertificate`, `GetCertificateBundle`,
    `GetCertificateAuthorityBundle`, and (if Task 3 needs it)
    `secrets.GetSecretBundle`.
  - In-memory state: instance pool size, per-instance `lifecycleState` +
    `freeformTags`, synchronous `RUNNING` transition (no simulated boot
    delay), matching the LocalStack tier's instantaneous-transition
    behavior in the AWS spec.
  - ~~Request-signature verification in the mock is OPTIONAL for the initial
    version (accept any well-formed `authorization` header) — the signing
    logic itself is already covered by Task 1's golden-vector unit tests;
    the mock server's job is exercising the two components' call sequences
    and state machines, not re-verifying signing.~~ **Reversed, August 11,
    2026.** That decision is what let two real defects ship past a green
    suite (`spike-notes.md` Findings 5 and 6, found on the first live call).
    Its reasoning was wrong in one specific way: a mock need not re-derive
    what the client *meant* to sign — it can rebuild the canonical string
    **from the bytes that arrived** and verify those, which answers a
    question golden vectors structurally cannot: *does what we sent match
    what we signed?* Both defects lived on exactly that axis. Verification
    is now on whenever `set_signing_key_pem()` has been called, which every
    test in the tree does.
  - Verify: mock server starts/stops cleanly in a standalone smoke test.
  - _Requirements: 13.5_

- [x] 5. **Unit tests + mock-server tests**

  - `tests/oci_quorum_manager_unit_test.cpp` (CTest target
    `oci-quorum-manager-unit-tests`, labels `unit;oci;quorum_manager`):
    - `static_assert` concept-satisfaction smoke test.
    - Construction validation: empty `compartment_id`/`cluster_name`/
      `instance_pool_id` each throw; missing API-key field with
      `use_instance_principal=false` throws.
    - `oci_signing::sign_request` golden-vector tests using Task 0's
      confirmed values.
    - Tag read-merge-write test (Property 1): canned `GetInstance` response
      with a pre-existing unrelated tag → merged `UpdateInstance` body
      retains it (Req 13.4).
    - Fault-injection tests for all four points in Requirement 10.
  - Mock-server tests (labels `integration;oci;mock`,
    `integration;oci;mock;certificates`), enabled by default:
    - `oci_provision_three_nodes`, `oci_assess_detects_stopped_node`,
      `oci_decommission_all_nodes`, `oci_decommission_idempotent`
      (mirroring the LocalStack tier's AWS test names, Req 13.6–13.7).
    - `oci_certificates_issue_and_cache_root`,
      `oci_certificates_revoke_idempotent`.
  - Verify: `ctest -R '^oci'` passes (see the CTest-naming deviation in the
    status block at the top of this file); all existing tests pass unmodified.
  - _Requirements: 13.1–13.7_

- [ ] 6. **Real-OCI integration tests + CI wiring**

  - `tests/oci_quorum_manager_real_test.cpp` /
    `tests/oci_certificates_provider_real_test.cpp`, guarded by
    `KYTHIRA_OCI_REAL_TESTS`, labels `integration;oci;real`, excluded from
    the default CTest run (Req 13.8).
  - Fixture: pre-flight identity check (skip, not fail, on any failure,
    Req 13.9); env-var-supplied pre-existing resource OCIDs, no
    auto-creation fallback (Req 13.11); `kythira-test-run` tagging and
    unconditional best-effort teardown (Req 13.10).
  - Port `aws_real_ec2_test_support.hpp`'s cost-tracking
    (`BilledResource`/`TestCostReport`/`CostAccumulator`/
    `CostSummaryFixture`) and signal-driven-cleanup apparatus to an
    OCI-flavored equivalent using published OCI shape pricing (Req 14.4–14.5).
  - **Preemptible-first launch escalation** (Req 13.12–13.15), porting
    `tests/aws_quorum_manager_real_ec2_test.cpp`'s
    `spot_first_launch_options()`/`is_insufficient_capacity()` pair — note
    those live in the AWS *test file*, not in `aws_asg_quorum_manager`, and
    this port keeps that split: `oci_instance_pool_quorum_manager` is not
    touched.
    - Rank candidates cheapest-first over **(shape, Availability Domain)**
      pairs, not shape alone (Req 13.13) — OCI stockouts are per-AD, so a
      shape-only ladder retries into the same shortage.
    - Truncate the preemptible portion at the cheapest reliably-available
      on-demand fallback and append that fallback last (Req 13.12).
    - Walk to the next option on a capacity error; abort immediately on any
      other error so real defects are not masked as stockouts (Req 13.14).
      Use the exact `Out of host capacity`/`OutOfHostCapacity` code and
      message shape Task 0's spike recorded — this classifier is a string
      match and cannot be written correctly from inspection alone.
    - Record the chosen shape/AD/market/price in the cost report so a run
      that degraded to on-demand is visible in the output (Req 13.15).
    - Verify the way the AWS and CoAP fixes were: exercise the escalation
      **under the failure condition** (force the first candidate to be
      unavailable) and confirm it advances, rather than only observing a
      green run where the first choice happened to succeed.
  - Replace the no-op `oci` job body in
    `.github/workflows/real-cloud-tests.yml` with real steps, following the
    `aws` job's shape: new `REAL_CLOUD_TESTS_OCI_INSTANCE_POOL_ENABLED` /
    `REAL_CLOUD_TESTS_OCI_CERTIFICATES_ENABLED` bundle variables (Req 14.1).
  - Add `scripts/ci-cloud-credentials/oci/` (provisioning script + IAM
    Policy fragments + `README.md`), using the federation mechanism Task
    0(f) confirmed, or the documented API-key fallback with explicit
    sign-off if federation isn't available (Req 14.2–14.3).
  - Verify: when `KYTHIRA_OCI_REAL_TESTS` is unset, both binaries compile
    but are not registered in CTest; all other tests pass unmodified.
  - _Requirements: 13.8–13.15, 14.1–14.5_

- [x] 7. **Documentation and example configuration**

  - `docker/oci_quorum_manager/oci_quorum_manager.env.example`, mirroring
    `docker/ca_cluster_node/ca_cluster_node.env.example`'s convention
    (Req 15.1).
  - `docker/oci_quorum_manager/README.md`: prerequisite-resource setup
    (compartment, VCN/subnet, Instance Configuration, Instance Pool,
    Certificate Authority), API-key vs. Instance Principal auth guidance,
    worked end-to-end example (Req 15.2).
  - Update `doc/TODO.md`'s OCI bullet from `[ ]` to `[x]` with a one-line
    summary mirroring the AWS bullet's parenthetical (Req 15.3).
  - Verify: example env file's variable names match
    `oci_client_config`/`oci_instance_pool_quorum_manager_config` fields
    exactly.
  - _Requirements: 15.1–15.3_

## Notes

- **Task 0 no longer gates anything** — all seven sub-questions closed as of
  2026-08-11. `instance_principal_signer` can now be implemented (Finding 16
  gives the full contract: metadata paths, the federation exchange, and the
  `ST$` keyId scheme) and real CI federation can be wired (Finding 17: OCI IAM
  Workload Identity Federation exchanges a GitHub OIDC JWT for a UPST — no
  long-lived key, and none of the sign-off Requirement 14.2 would have
  demanded). Nothing in Tasks 1-7 is blocked on a spike question any more.
- The one-pool-per-AD operational constraint (design.md Non-Goals,
  confirmed by `spike-notes.md` Finding 2) means Task 2 needs no special
  workaround code: each configured manager instance already only ever
  calls `UpdateInstancePool` on its own single-AD pool; the constraint is
  enforced by how the *caller* deploys (one manager + one pool per AD),
  documented in Task 7's README, not by new logic in the manager itself.
- Task 3's CSR-forwarding design (confirmed by `spike-notes.md` Finding 3)
  needs no Vault/`secrets` service dependency — an earlier draft of this
  task anticipated a Vault-key-export fallback for the case where OCI
  generates the key internally; that case does not apply to the confirmed
  `MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA` config type, so
  `oci_certificates_provider_config` needs no Vault-specific field.
- Mirror `aws-quorum-manager/tasks.md`'s closing note about
  `next_node_id()`'s TOCTOU race: the same reasoning applies here
  unchanged — `quorum_management.hpp`'s leader-side pending-provision
  tracking (Requirements 14.3–14.4 in the quorum-management spec) already
  prevents concurrent `provision_node` calls for the same slot, so no
  additional locking is needed in `oci_instance_pool_quorum_manager`.
