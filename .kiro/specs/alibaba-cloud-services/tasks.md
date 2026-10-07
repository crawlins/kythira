# Implementation Plan — Alibaba Cloud Services

## Status: complete — audited task by task (October 2, 2026)

Every box below is now ticked, and each was ticked against its own **Verify**
line rather than on the strength of files existing. Task 4 stays closed by
descope. The September 12 note this replaces said the checklist and the tree
disagreed and that a per-task pass was owed; this is that pass.

| Task | Evidence |
| --- | --- |
| 0.2 | Live PutObject chain, spike-notes Finding 2; read-after-write, Finding 13 |
| 0.3 | spike-notes Finding 11. Pagination past one page is the one thing still documentation-and-mock only, on purpose |
| 0.5 | spike-notes Finding 12, via the vendor's action |
| 3 | 33 unit cases incl. the four fault points; `static_assert` at the foot of the header. **The audit found the "NodeId max+1 incl. boundary" case the test list names was not covered, and the boundary was broken** — see below |
| 5 | `alibaba_oss_persistence_unit_test` (CI-green on `main`); live in Finding 13 |
| 6 | Mock verifies both schemes from received bytes; `a_tampered_object_signature_is_rejected` is the corrupted-signature control |
| 7 | Both real suites exit 77 naming the missing `KYTHIRA_ALIBABA_*` values (re-run October 2); neither is `add_test`-registered |
| 8 | `Fail closed if Alibaba CI variables are unset` names each missing variable; the zero-bundles guard; YAML parses |
| 9 | Req 18 artifacts present; README provider list added October 2 (it was the one missing item) |
| 10 | Account, role, OIDC provider, scaling group and bucket recorded in `scripts/ci-cloud-credentials/alibaba/README.md`; the script's idempotency was run three times (`89259dc`) |
| 11 | Two real suites, not the three the text names (the CA suite went with task 4). Scheduled run 36426191450: both bundles green, audit clean; dispatch 36936632218 green; CI-runner latency in the operator README |

**What the audit fixed rather than ticked (task 3).** `next_node_id_from`
parsed tags with `std::stoull`, which reads `"-1"` as 2^64-1, so one stray
tag made max+1 wrap to NodeId 0 — and kept doing so, giving every later
provision the same identity. A tag already at the ceiling did the same. The
scan now parses strict decimal and refuses to wrap, and because the NodeId is
computed *after* ESS has launched the instance, a refusal there now removes
the instance instead of leaving it running untagged. Three unit cases pin
it; all three fail against the previous header.

**Not verifiable from the repository:** task 10's root-account hardening (MFA
on root, no root AccessKey). It is an operator attestation, recorded here as
such rather than as checked.

## Status: Wave 1 complete (August 13, 2026) — superseded, see above

Tasks 0 (partially — see spike-notes.md for what is documentation-deep vs
live-verified), 1 and 2 are done: the shared control-plane foundation
(`alibaba_client_config`, `alibaba_signing`, `alibaba_http_client`), the OSS
data-plane client, the Kconfig menu + CMake gates, and 33 unit cases across
three binaries, all green locally. The certificate provider was descoped the
same day on cost grounds — see requirements.md Requirement 12, which also
records the revival conditions.

No Alibaba Cloud account exists for this project yet
(`scripts/ci-cloud-credentials/alibaba/README.md`). Task 10 is the
**[operator]** step that creates it (account signup needs payment/identity
verification no script can perform); task 11 (live verification) cannot be
truthfully checked off until task 10 is done — checking it off requires a
live run, per Requirement 16.11. Everything before task 10 (foundation,
components, unit + mock tiers, CI wiring shipped fail-closed) is
implementable and verifiable without an account.

## Overview

Reference implementations to study before starting:
- `.kiro/specs/oci-cloud-provider/` — the no-SDK shape this spec follows
  (spike-first, hand-rolled signing, signature-verifying mock, exit-77
  real tier, Kconfig gates without `KYTHIRA_HAS_*`).
- `include/raft/aws_asg_quorum_manager.hpp` — the scaling-group semantics
  this manager mirrors (capacity+1 provision, tag-scan NodeId,
  zone-placement caveat).
- `include/raft/file_persistence.hpp` + `tests/file_persistence_unit_test.cpp`
  — the persistence mirror/layout/test pattern.
- `tests/oci_mock_server.hpp` header comment — why the mock verifies
  signatures from received bytes.

## Task Dependency Graph

```json
{
  "waves": [
    { "wave": 0, "tasks": [0], "description": "Spike: confirm signing schemes, API semantics, SDK decision" },
    { "wave": 1, "tasks": [1, 2], "description": "Foundations: V3 signing/http client; OSS V4 client (parallelizable)" },
    { "wave": 2, "tasks": [3, 5], "description": "Components: quorum manager; OSS engine (parallelizable; task 4 descoped)" },
    { "wave": 3, "tasks": [6], "description": "Mock server + mock-tier tests" },
    { "wave": 4, "tasks": [7, 8], "description": "Real-tier suites (compile + skip paths); CI wiring + credential scripts" },
    { "wave": 5, "tasks": [9], "description": "Docs, example config, TODO/CHANGELOG close-out" },
    { "wave": 6, "tasks": [10], "description": "[operator] Provision the Alibaba Cloud account + CI prerequisites" },
    { "wave": 7, "tasks": [11], "description": "Live verification (unblocked by task 10)" }
  ]
}
```

## Tasks

- [x] 0. **Spike: pin the vendor facts this spec paraphrases**

  Run against vendor documentation and (once any account exists — a
  personal/trial account suffices for signing captures) live endpoints.
  Record findings in `spike-notes.md` with the OCI spec's
  CONFIRMED/CORRECTED/WAS format, folding corrections back into
  requirements.md/design.md in place.

  - [x] 0.1 **V3 signing canonical form** — CONFIRMED, fully reproducible (spike-notes.md Finding 1); golden vectors pinned in alibaba_signing_unit_test. — capture one known-good signed
        exchange (any trivial RPC call, e.g. `DescribeRegions`); derive
        golden vectors (canonical request, string-to-sign, Authorization)
        for the unit tests. Confirm percent-encoding rules and the signed
        header set with/without `x-acs-security-token`.
  - [x] 0.2 **OSS V4 signing canonical form** — CONFIRMED LIVE: the first live PutObject found the unsigned Content-Type defect, fixed and re-verified end to end (Finding 2); read-after-write seen live (Finding 13). — same treatment for
        PutObject/GetObject/ListObjectsV2; confirm the path-style
        addressing behavior the mock tier depends on (Requirement 2.3),
        and confirm the documented durability/consistency statements cited
        by Requirement 15.2.
  - [x] 0.3 **ESS semantics** — CONFIRMED LIVE except pagination past one page (Finding 11). — confirm `ModifyScalingGroup`
        DesiredCapacity+1 vs `ScaleWithAdjustment` for provision
        (idempotency under retry) and `RemoveInstances` vs
        `DetachInstances`+decrement for decommission (must terminate AND
        shrink capacity atomically); confirm `DescribeScalingInstances`
        pagination and lifecycle-state vocabulary; confirm ECS
        `DescribeInstances` batch ID limit (Requirement 6.1).
  - [x] 0.4 **SDK decision checkpoint** — CONFIRMED no-SDK (Finding 5). — if 0.1/0.2 surface material
        problems with hand-rolled signing, record the fallback decision to
        adopt vcpkg `aliyun-oss-cpp-sdk` for the data plane only, and why;
        otherwise record CONFIRMED for the no-SDK path.
  - [x] 0.5 **AssumeRoleWithOIDC exchange** — CONFIRMED LIVE through the vendor action (Finding 12). — confirm the STS request shape
        (unauthenticated call carrying the OIDC token + role ARN + OIDC
        provider ARN) and the returned credential triple, for Requirement
        17.2's workflow step.
  - Verify: spike-notes.md exists with a finding per sub-item; every
    CORRECTED item's requirements/design text updated in place.
  - _Requirements: 1.3, 2.2, 6.1, 7.1, 8.1, 17.2_

- [x] 1. **Shared control-plane foundation**: `alibaba_client_config`,
      `alibaba_signing`, `alibaba_http_client`

  - Create `include/raft/alibaba_client_config.hpp` (unconditional
    compilation, aggregate, Requirement 1.1).
  - Create `include/raft/alibaba_signing.hpp` per design sketch; golden
    vectors from Task 0.1; injectable time/nonce; empty-credential throws.
  - Create `include/raft/alibaba_http_client.hpp`: endpoint table,
    `endpoint_override` global takeover, fresh-client-per-request,
    error unwrapping, single throttling retry, empty-body → null.
  - Add the Kconfig menu + two gate symbols and the
    `KYTHIRA_ALIBABA_SHARED` CMake shape (Requirement 11.1–11.2) so the
    new tests build behind gates from the first commit.
  - Write `tests/alibaba_signing_unit_test.cpp` and
    `tests/alibaba_http_client_unit_test.cpp` (Requirement 16.2–16.3),
    registered with labels `unit;alibaba;cloud`.
  - Verify: both test binaries green locally; a gate toggled off skips
    them with a configure-time STATUS message (the OCI CMake pattern).
  - _Requirements: 1.1–1.9, 11.1–11.2, 16.2–16.3_

- [x] 2. **OSS data-plane client**: `alibaba_oss_client`

  - Create `include/raft/alibaba_oss_client.hpp` per design: V4 signing
    (Task 0.2 vectors), four operations, path-style under override, XML
    listing helper that throws on structural surprise, no write
    auto-retry.
  - Write `tests/alibaba_oss_client_unit_test.cpp` including pagination
    (multi-page mock listing), 404 → nullopt, truncated-XML → throw.
  - Verify: unit test green; golden V4 vector case pinned.
  - _Requirements: 2.1–2.6, 16.2_

- [x] 3. **`alibaba_ess_quorum_manager`**

  - **Config + constructor** (Req 3): validation, fail-fast
    `DescribeScalingGroups` probe.
  - **`assess_quorum`** (Req 6, design sequence): pagination, ECS
    batching, cluster-tag filter, per-zone aggregation.
  - **`provision_node`** (Req 7): snapshot → capacity+1 → poll →
    tag/adopt → peer_info; zone-mismatch proceed-and-log.
  - **`decommission_node`** (Req 8): tag-resolve → remove →
    poll-out; absent-node idempotent success.
  - **`topology` / `maintain_quorum`** (Req 4.3, 9): sibling-identical.
  - Fault points (Req 10.1) + file-scope `static_assert` (Req 4.2).
  - Write `tests/alibaba_quorum_manager_unit_test.cpp` (Req 16.4):
    constructor validation, NodeId max+1 incl. boundary, foreign-instance
    exclusion, decommission idempotency, every fault point, transport
    failure via dead-port override.
  - Verify: unit test green; `static_assert` compiles.
  - _Requirements: 3.1–3.3, 4.1–4.3, 5.1–5.4, 6.1–6.5, 7.1–7.6, 8.1–8.4,
    9.1–9.2, 10.1–10.2_

- [x] 4. **`alibaba_ca_certificate_provider` — DESCOPED** (checked as
      closed-by-descope, not as done): the certificate provider was
      removed from scope on cost grounds — no Alibaba CA, private or
      public, will be purchased. Revivable under the right conditions;
      requirements.md Requirement 12 records both the rationale and where
      the drafted requirement set lives. No work happens under this
      number.
  - _Requirements: 12 (descope record)_

- [x] 5. **`alibaba_oss_persistence_engine`**

  - Engine per Requirement 15 + design: layout (Req 14), in-memory
    mirror, synchronous writes with single idempotent-PUT retry,
    parse-or-throw load, single-slot snapshot, fault points,
    `static_assert`.
  - Write `tests/alibaba_oss_persistence_unit_test.cpp` (Req 16.5):
    round-trips, reload-survival per field, binary commands, snapshot
    overwrite, corruption → constructor throw, PUT-500 → throw with
    mirror unchanged (design Property 1), zero-padding boundary (design
    Property 5), fault points.
  - Verify: unit test green, including the durability-ordering case.
  - _Requirements: 14.1–14.3, 15.1–15.9, 16.5_

- [x] 6. **Mock server + mock-tier tests**

  - `tests/alibaba_mock_server.hpp`: ESS/ECS routes + OSS path-style
    object store, in-memory state, additive TagResources semantics,
    **signature verification from received bytes** for both signing
    schemes (Req 16.6 — copy the oci_mock_server rationale comment).
  - `tests/alibaba_{quorum_manager,oss_persistence}_mock_test.cpp`
    driving full flows; CTest labels
    `integration;alibaba;mock;<component>;cloud` (Req 16.7).
  - Verify: mock suites green in a default build; a deliberately
    corrupted signature in a one-off test build is rejected by the mock
    (proving verification is live).
  - _Requirements: 16.6–16.7_

- [x] 7. **Real-tier suites (compiled, gated, skip-correct)**

  - `tests/alibaba_{quorum_manager,oss_persistence}_real_test.cpp`
    under `KYTHIRA_ALIBABA_REAL_TESTS`; never CTest-registered; exit-77
    skip fixture naming each missing `KYTHIRA_ALIBABA_*` value; read-only
    `DescribeRegions` pre-flight; cost + signal fixtures per Req 16.10.
  - Verify: suites build; running with no env exits 77 with the SKIP
    lines; `ctest -N` does not list them.
  - _Requirements: 16.8–16.10_

- [x] 8. **CI wiring + credential provisioning scripts (ships fail-closed)**

  - Replace the `alibaba` stub job in real-cloud-tests.yml per Req 17:
    bundle toggles + dispatch inputs, zero-bundles guard,
    AssumeRoleWithOIDC exchange step (shape from Task 0.5), named-target
    builds, exit-77 → loud failure.
  - `scripts/ci-cloud-credentials/alibaba/provision-oidc-role.sh` +
    `policies/{ess-quorum-manager,oss-persistence}.json` +
    README walkthrough w/ cost estimates (Req 17.3), extending the
    existing monitoring-deviation README.
  - Verify: workflow YAML parses; a dispatch with the alibaba toggles off
    skips the job; **a dispatch with a bundle on fails closed naming the
    missing variables** (this is verifiable today, without an account).
  - _Requirements: 17.1–17.5_

- [x] 9. **Docs, example config, close-out**

  - `docker/alibaba_quorum_manager/alibaba_quorum_manager.env.example` +
    `README.md` per Req 18.1–18.2 (prerequisites, credential modes, zone
    caveat, election-timeout sizing guidance).
  - README.md provider list; DEPENDENCIES.md cross-reference note (Req
    11.3); doc/TODO.md Alibaba entry → `[x]` with blocked-on-account
    honesty + persistence-entry note (Req 18.3); CHANGELOG entry.
  - Verify: every Req 18 artifact exists; TODO text states what has and
    has not run live.
  - _Requirements: 11.3, 18.1–18.4_

- [x] 10. **[operator] Provision the Alibaba Cloud account for validation
      and CI**

  The one task on this list an operator must at least initiate by hand
  (account signup requires payment/identity verification no script can
  perform). Everything after signup is scripted or documented.

  - Create the Alibaba Cloud account (international console,
    alibabacloud.com) with billing enabled; record the account ID and
    chosen home region in `scripts/ci-cloud-credentials/alibaba/README.md`.
  - Immediately harden the root identity: MFA on the root account, no
    root AccessKey; create a RAM admin user for all further provisioning
    (the lesson from this repo's AWS setup, where the CI account's local
    profile turned out to be root).
  - Run Task 8's `provision-oidc-role.sh` to create the RAM OIDC IdP
    (GitHub issuer), the CI role, and per-bundle policies; set the
    repository variables/secrets its output names (region, role ARN,
    OIDC provider ARN) in the `real-cloud-tests` environment.
  - Provision the test prerequisites the code deliberately does not
    create (Requirement 18.2's list): an ESS scaling group spanning ≥2
    zones' vSwitches with a scaling configuration and security group, and
    an OSS bucket for the persistence suite; set the matching
    `KYTHIRA_ALIBABA_*` repository variables. (No CA: the certificate
    provider is descoped on cost grounds — task 4.)
  - Spike accounts count: the signing captures Task 0.1/0.2 want (a
    trivial `DescribeRegions` + OSS PutObject exchange) need only this
    account's AK — once it exists, close out any Task 0 sub-items that
    were deferred for want of one.
  - Estimate and record standing cost in the README (the scaling group
    at MinSize 0 and the OSS bucket are the candidates for idle cost —
    both should be near zero; the one materially priced resource, a
    Private CA, is exactly what the descope avoided).
  - Verify: `provision-oidc-role.sh` run twice is idempotent; a
    `workflow_dispatch` of the `alibaba` job with one bundle enabled gets
    past the fail-closed guards to the suite invocation (whatever its
    verdict — that is task 11's concern).
  - _Requirements: 16.9, 17.2–17.3, 17.5, 18.2_

- [x] 11. **Live verification** (unblocked by task 10)

  - Run all three real suites against the provisioned account; fold every
    live correction back into spike-notes.md/requirements/design in place
    (the OCI doctrine); measure Requirement 15.2's PUT latency and put
    the figure in the README's election-timeout guidance.
  - Flip the CI toggles; one green dispatched run of the `alibaba` job
    per bundle.
  - Verify: green run URLs recorded in spike-notes.md; leak audit clean.
  - _Requirements: 16.11, 17.5, 18.2_

## Notes

- **NodeId TOCTOU**: tag-scan max+1 races another leader's manager the
  same way the other pool managers do (ESS tags after launch, with no
  arbiter for a tag). Leader-side pending-provision tracking is per leader
  and does not close it; see the "Node ids across leaders" note on the
  `quorum_manager` concept in `quorum_management.hpp`.
- **Zone placement**: strict per-zone provisioning needs one scaling
  group per zone (Requirement 7.3). If a deployment needs it, that is a
  config topology (N managers) — not a code change.
- **The two-signers decision** (design Overview) means golden vectors
  exist per scheme; resist any refactor that merges the canonical-form
  builders — the schemes evolve independently on the vendor side.
- **Mock fidelity**: TagResources is additive; a mock that models
  map-replacement would invert design Property 4's test value. The OSS
  mock must honor path-style addressing only (that is what the client
  emits under override).
