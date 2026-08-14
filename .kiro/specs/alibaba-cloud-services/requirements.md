# Requirements Document

## Introduction

This spec adds Alibaba Cloud as the fifth cloud provider (doc/TODO.md, Cloud
Provider Support): a `kythira::quorum_manager` backed by an Auto Scaling
(ESS) scaling group and — per doc/TODO.md's binding mandate ("whichever
spec introduces Alibaba Cloud support SHALL include an Alibaba OSS
key-object persistence engine in scope") — a `kythira::persistence_engine`
backed by Object Storage Service (OSS), so the provider lands with parity
instead of joining the example-config debt that section documents.

The certificate provider the TODO entry originally named (backed by the
Certificate Management Service's Private CA) is **descoped by operator
decision (August 13, 2026): no Alibaba CA — private or public — will be
purchased for this project.** CAS Private CA carries a real annual price
for a resource the cluster does not need: Raft mTLS material is
deployment-agnostic and already obtainable from the four existing
certificate providers (local, AWS ACM-PCA, GCP CAS, Azure Key Vault).
Requirements 12–13 below record the descope in place so numbering stays
stable; nothing else in this spec depends on them. The descope is a cost
decision, not a technical one, and is revivable under the right
conditions — see Requirement 12 for both.


Alibaba Cloud publishes no C++ SDK usable from this project's build: the
general `aliyun-openapi-cpp-sdk` is not in the vcpkg registry, and while
`aliyun-oss-cpp-sdk` is (1.10.1), adopting it would add a second in-process
HTTP stack (libcurl) beside httplib, put the persistence engine's
durability-critical retry behavior inside a vendor black box, and leave the
mock-server test tier fighting the SDK's virtual-host addressing. So this
spec follows `.kiro/specs/oci-cloud-provider/`'s established no-SDK shape
wholesale: hand-rolled request signing on OpenSSL primitives, httplib +
boost::json for transport and parsing, a Kconfig gate per component with no
`KYTHIRA_HAS_*` counterpart, no vcpkg or find_package changes, and a
signature-verifying mock server in place of an emulator (Alibaba has no
LocalStack analogue). The SDK question is nonetheless Task 0 spike
material — the decision above is recorded as refutable, not assumed.

Two Alibaba-specific facts shape the design and were verified against
vendor documentation (August 13, 2026; Task 0 re-confirms them live):

- The control plane (ESS, ECS, STS) speaks RPC-style OpenAPI with V3
  request signing (`ACS3-HMAC-SHA256`) — pure HMAC over a canonical
  request, no RSA. This is strictly simpler than OCI's Signature v1.
- OSS is a separate, S3-like REST API with its own V4 header signing
  (`OSS4-HMAC-SHA256`); it is not an RPC-style OpenAPI product, so the
  provider carries two small signers sharing HMAC/hash helpers.

Real-cloud verification is wired but cannot run yet: **no Alibaba Cloud
account exists for this project** (`scripts/ci-cloud-credentials/alibaba/README.md`).
Every real-tier deliverable fails closed naming what is missing, exactly
like the pre-provisioning state the OCI and monitoring specs shipped in.

## Glossary

- **ESS**: Auto Scaling (Elastic Scaling Service), OpenAPI version
  2014-08-28, endpoint `ess.aliyuncs.com`. A **scaling group** maintains a
  fleet of ECS instances between MinSize/MaxSize at DesiredCapacity,
  distributing them across the vSwitches (and therefore zones) it is
  configured with.
- **ECS**: Elastic Compute Service, OpenAPI version 2014-05-26, endpoint
  `ecs.aliyuncs.com`. Source of instance status, IPs, zone IDs, and tags.
- **OSS**: Object Storage Service. Strong read-after-write consistency for
  PUT/GET/DELETE/List; a 2xx PUT response indicates the object is durably
  stored (multi-replica) before the response is sent.
- **RAM**: Resource Access Management — users, roles, AccessKey pairs.
  **AssumeRoleWithOIDC** (STS, endpoint `sts.aliyuncs.com`) exchanges an
  OIDC token from a registered IdP for short-lived role credentials
  (AccessKeyId/AccessKeySecret/SecurityToken) without any stored long-lived
  key — the CI federation path, analogous to AWS OIDC role assumption.
- **V3 signing**: `Authorization: ACS3-HMAC-SHA256
  Credential=<ak>,SignedHeaders=<h1;h2;...>,Signature=<hex>` computed over
  a canonical request of method, path, query, signed `host`/`x-acs-*`
  headers, and the SHA-256 of the body; `x-acs-action`/`x-acs-version`
  name the RPC operation.
- **Zone ID**: e.g. `cn-hangzhou-b` — this provider's
  `placement_group_id_type`, the analogue of an AWS AZ / OCI AD.

## Requirements

### Requirement 1: Alibaba OpenAPI V3 Request Signing and HTTP Client Foundation

**User Story:** As a developer, I want a shared, hand-rolled Alibaba
OpenAPI client foundation, so that the quorum manager signs and transports
its own requests the way the OCI provider does, with no vendor SDK
dependency.

#### Acceptance Criteria

1. An `alibaba_client_config` struct SHALL be defined in
   `include/raft/alibaba_client_config.hpp` (compiled unconditionally — its
   only dependencies are `<chrono>`/`<string>`/`<optional>`, mirroring
   `oci_client_config.hpp`) with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `region` | `std::string` | `""` | Region ID, e.g. `cn-hangzhou` |
   | `access_key_id` | `std::string` | `""` | RAM AccessKeyId (or STS temporary key) |
   | `access_key_secret` | `std::string` | `""` | Matching secret |
   | `security_token` | `std::string` | `""` | STS token; when non-empty, sent as `x-acs-security-token` and signed |
   | `endpoint_override` | `std::string` | `""` | Replaces every derived endpoint (the mock-server hook, analogous to `oci_client_config::endpoint_override`) |
   | `api_timeout` | `std::chrono::seconds` | `30s` | Per-request connect/read timeout |

   Validation SHALL live in the signer, not the aggregate, for the same
   build-it-field-by-field reason `oci_client_config` documents.
2. `include/raft/alibaba_signing.hpp` SHALL implement V3 signing
   (`ACS3-HMAC-SHA256`) as pure functions over OpenSSL EVP/HMAC:
   `sign_request(cfg, method, host, path, query_params, action, version,
   body, when, nonce)` returning the full header map to attach
   (`host`, `x-acs-action`, `x-acs-version`, `x-acs-date`,
   `x-acs-signature-nonce`, `x-acs-content-sha256`,
   `x-acs-security-token` when set, `Authorization`). `when` and `nonce`
   SHALL be injectable parameters so unit tests can pin a signature against
   golden vectors.
3. The canonical request and string-to-sign SHALL follow the vendor's
   documented V3 scheme (percent-encoding rules, sorted query, sorted
   lowercase signed headers, `ACS3-HMAC-SHA256\n` + SHA-256 of canonical
   request), confirmed by Task 0 against a captured known-good exchange —
   golden vectors in the unit test SHALL come from that confirmation, not
   from this spec's paraphrase.
4. WHEN `access_key_id` or `access_key_secret` is empty THEN `sign_request`
   SHALL throw `std::invalid_argument` naming the missing field.
5. `include/raft/alibaba_http_client.hpp` SHALL define
   `class alibaba_http_client` holding an `alibaba_client_config` by value,
   with `rpc(service, action, version, params, body) const ->
   boost::json::value`: derive the endpoint per service
   (`ess`/`ecs`/`sts` → `<service>.aliyuncs.com`, region-qualified
   forms where the vendor requires them — exact per-service endpoint rules
   are Task 0 spike material), sign via Requirement 1.2, POST via a fresh
   `httplib::Client` per request (the `oci_http_client` idiom and
   rationale), and parse the JSON response.
6. `endpoint_override` SHALL win over every derived endpoint for all
   services at once, so one mock server can stand in for ESS, ECS, and STS
   together — mirroring `oci_http_client::host_for`.
7. Non-2xx responses SHALL throw `std::runtime_error` carrying the
   vendor's `Code`/`Message`/`RequestId` fields when present; throttling
   responses (HTTP 429, or the vendor's `Throttling*` error codes) SHALL be
   retried exactly once after honoring `Retry-After` when present, else a
   fixed short delay — mirroring `oci_http_client`'s single-retry policy.
8. An empty 2xx body SHALL parse as JSON `null`, not throw.
9. All of the above SHALL be header-only, in namespace `kythira`
   (`alibaba_signing` helpers under `kythira::alibaba_signing`), with
   OpenSSL RAII via the `openssl_deleter` pattern `oci_signing.hpp` uses.

---

### Requirement 2: OSS Request Signing and Object Client

**User Story:** As a developer, I want a minimal hand-rolled OSS client,
so that the persistence engine can PUT/GET/DELETE/List objects with owned
retry-and-durability semantics and mock-server testability.

#### Acceptance Criteria

1. `include/raft/alibaba_oss_client.hpp` SHALL implement OSS V4 header
   signing (`OSS4-HMAC-SHA256`) and a client over the same
   `alibaba_client_config`, exposing exactly the operations the
   persistence engine needs: `put_object(bucket, key, bytes)`,
   `get_object(bucket, key) -> std::optional<std::string>` (`nullopt` on
   404), `delete_object(bucket, key)`, and
   `list_keys(bucket, prefix) -> std::vector<std::string>` (paginated
   ListObjectsV2, continuation tokens followed to exhaustion).
2. The V4 canonical form (date scope, `x-oss-*` header set,
   `x-oss-content-sha256`, bucket/key resource path) SHALL be confirmed by
   Task 0 the same way as Requirement 1.3, with golden vectors captured
   from the confirmation.
3. Endpoint derivation SHALL default to virtual-host style
   (`<bucket>.oss-<region>.aliyuncs.com`); WHEN `endpoint_override` is set
   THEN the client SHALL switch to path-style addressing
   (`<override>/<bucket>/<key>`) so a local mock server needs no wildcard
   DNS — this deviation from the vendor default SHALL be documented in the
   header comment.
4. `list_keys` responses are XML; the client SHALL extract `<Key>`,
   `<IsTruncated>`, and `<NextContinuationToken>` values with a small
   contained helper rather than a new XML dependency, and SHALL treat any
   structurally unexpected response as an error (throw), never as an empty
   listing — a truncated-listing bug in a persistence engine is silent data
   loss.
5. `put_object` SHALL treat only a 2xx response as success and SHALL NOT
   retry writes automatically; retry policy for persistence writes belongs
   to the engine (Requirement 15), which knows which operations are
   idempotent. Reads (`get_object`, `list_keys`) MAY retry once on 5xx.
6. `security_token` support and error mapping SHALL match Requirement
   1.7's conventions (OSS errors are XML — `Code`/`Message` extracted with
   the same helper as 2.4).

---

### Requirement 3: `alibaba_ess_quorum_manager` Configuration

**User Story:** As an operator, I want to configure the ESS-backed quorum
manager against an existing scaling group, so that quorum management works
with resources my organization provisions and controls.

#### Acceptance Criteria

1. `include/raft/alibaba_ess_quorum_manager.hpp` SHALL define
   `struct alibaba_ess_quorum_manager_config` with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `alibaba` | `alibaba_client_config` | — | Credentials/region/endpoint |
   | `cluster_name` | `std::string` | — | Tag value isolating this cluster's instances |
   | `scaling_group_id` | `std::string` | — | Existing ESS scaling group (the manager never creates one) |
   | `node_port` | `std::uint16_t` | `7000` | Port composed into `peer_info` addresses |
   | `topology` | `desired_topology<std::string>` | `{}` | Per-zone targets |
   | `provision_timeout` | `std::chrono::seconds` | `300s` | Bound on waiting for a new InService instance |
   | `poll_interval` | `std::chrono::milliseconds` | `5000ms` | Poll cadence during provision/decommission |
   | `extra_tags` | `std::map<std::string,std::string>` | `{}` | Operator tags added to managed instances |

2. The constructor SHALL throw `std::invalid_argument` naming the field
   when `region` (absent `endpoint_override`), `cluster_name`, or
   `scaling_group_id` is empty, and SHALL validate the scaling group exists
   via `DescribeScalingGroups` — a fail-fast probe mirroring
   `oci_instance_pool_quorum_manager`'s constructor `GetInstancePool`.
3. Like every other cloud quorum manager, the class SHALL NOT create or
   destroy the scaling group, its scaling configuration, vSwitches, or
   security groups — prerequisite resources are the operator's
   (documented per Requirement 18).

---

### Requirement 4: `alibaba_ess_quorum_manager` Class Interface

**User Story:** As a developer, I want the manager to satisfy
`kythira::quorum_manager`, so that it is a drop-in sibling of the AWS,
Azure, GCP, and OCI managers.

#### Acceptance Criteria

1. `template<typename NodeId = std::uint64_t, typename Address =
   std::string> class alibaba_ess_quorum_manager` SHALL define
   `node_id_type`/`address_type`/`placement_group_id_type = std::string`
   (Zone ID) and provide `assess_quorum`, `provision_node`,
   `decommission_node`, `topology`, and `maintain_quorum` with the exact
   signatures `quorum_manager` (include/raft/quorum_management.hpp:168)
   requires, returning `kythira::future`s produced the same way the OCI
   manager produces them.
2. A file-scope `static_assert(quorum_manager<
   alibaba_ess_quorum_manager<std::uint64_t, std::string>, std::uint64_t,
   std::string, std::string>)` SHALL appear at the bottom of the header,
   mirroring every sibling (e.g. oci_instance_pool_quorum_manager.hpp:1097).
3. `topology()` SHALL return `config.topology` synchronously with no I/O.

---

### Requirement 5: Node Tagging Scheme

**User Story:** As a developer, I want cluster membership and node
identity carried on ECS tags, so that NodeIds survive process restarts and
the manager stays stateless.

#### Acceptance Criteria

1. Managed instances SHALL carry ECS tags `kythira-cluster =
   config.cluster_name` and `kythira-node-id = <decimal NodeId>`, written
   via ECS `TagResources` after a provisioned instance is identified, plus
   `config.extra_tags`.
2. ECS `TagResources` is additive (it creates or overwrites only the keys
   passed, unlike OCI's whole-map `freeformTags` replacement), so no
   read-merge-write cycle is required; the header comment SHALL state this
   contrast with the OCI manager explicitly so a future reader does not
   "fix" it into one.
3. NodeId assignment SHALL be tag-scan max+1 over the scaling group's
   current instances (the `aws_asg_quorum_manager` scheme — ESS names give
   nothing derivable), with the same known TOCTOU caveat deferred to the
   leader-side pending-provision tracking note in
   `quorum_management.hpp`.
4. Instances in the group lacking a `kythira-node-id` tag SHALL be treated
   as not-yet-adopted (visible in health accounting per Requirement 6 but
   never returned as peers), matching the sibling managers' handling.

---

### Requirement 6: `assess_quorum`

**User Story:** As the quorum-management layer, I want an accurate
per-zone health picture, so that remediation decisions are grounded in
live cloud state.

#### Acceptance Criteria

1. `assess_quorum(cluster)` SHALL list the scaling group's instances via
   ESS `DescribeScalingInstances` (paginated to exhaustion), then resolve
   status, zone, private IP, and tags via ECS
   `DescribeInstances`/`DescribeInstanceStatus` — batched by the APIs'
   documented ID-list limits rather than per-instance calls where the API
   allows it (exact batch limits are Task 0 spike material).
2. An instance SHALL count as live when its ESS lifecycle state is
   `InService` AND its ECS status is `Running`; there is no heartbeat-tag
   freshness check (deliberate contrast with OCI, whose
   Requirement-4.4-style heartbeat is out of scope here — see design
   Non-Goals).
3. Health SHALL be aggregated per Zone ID into
   `quorum_health<NodeId, std::string>` exactly the way the sibling
   managers do: per-group live/target counts from `config.topology`,
   `quorum_status` thresholds identical to `aws_asg_quorum_manager`'s.
4. Instances belonging to other clusters (different `kythira-cluster` tag
   value, or untagged instances the operator placed in the same group)
   SHALL never be counted as cluster members.
5. Transport or API failure SHALL surface as an exceptional future, never
   a fabricated "everything is down" health object.

---

### Requirement 7: `provision_node`

**User Story:** As the quorum-management layer, I want to add a node by
growing the scaling group, so that replacement capacity appears without
the manager owning instance-launch mechanics.

#### Acceptance Criteria

1. `provision_node(target_group, replacing)` SHALL snapshot current member
   instance IDs, then request one more instance via ESS
   `ModifyScalingGroup` raising `DesiredCapacity` by one (the
   `aws_asg_quorum_manager` `SetDesiredCapacity`+1 analogue; Task 0
   confirms `ModifyScalingGroup` semantics vs `ScaleWithAdjustment` and
   picks whichever is idempotency-safer, recording why).
2. The manager SHALL poll `DescribeScalingInstances` at
   `config.poll_interval` until a new instance (not in the snapshot)
   reaches `InService` and its ECS status is `Running`, or
   `config.provision_timeout` elapses (exceptional future on timeout,
   naming the scaling group and elapsed time).
3. ESS chooses the zone (the group balances across its configured
   vSwitches); WHEN the new instance's zone differs from `target_group`
   THEN the manager SHALL proceed and report the actual zone in the
   returned `peer_info`/logs rather than fail — the same
   capacity-over-placement trade the ASG manager documents. The header
   SHALL document that strict per-zone placement requires one scaling
   group per zone, and that this mirrors the AWS ASG limitation.
4. The new instance SHALL be tagged per Requirement 5 (NodeId assigned
   tag-scan max+1) and its private IP resolved via `DescribeInstances`;
   the returned `peer_info` SHALL be `{node_id, "<private-ip>:<node_port>"}`.
5. `replacing` SHALL only influence NodeId selection the way the sibling
   managers use it (never reuse a live node's ID); it does not decommission
   — that is the caller's responsibility.
6. Scale-out failure surfaced by ESS (capacity, stockout, quota) SHALL be
   an exceptional future carrying the vendor's error code/message; a
   stockout-classification ladder like OCI's is out of scope until real
   Alibaba failures teach us its actual error shapes (design Non-Goals).

---

### Requirement 8: `decommission_node`

**User Story:** As the quorum-management layer, I want node removal to
shrink the group and terminate the right instance, idempotently.

#### Acceptance Criteria

1. `decommission_node(node_id)` SHALL resolve the instance by
   `kythira-node-id` tag within the cluster's instances and remove it via
   ESS `RemoveInstances` with the group's DesiredCapacity decremented by
   the removal (Task 0 confirms `RemoveInstances`' exact
   capacity/termination semantics vs `DetachInstances` +
   `DecrementDesiredCapacity`, and picks the call that both terminates the
   instance and shrinks capacity atomically).
2. The future SHALL resolve once ESS accepts the removal and the instance
   leaves the group's `DescribeScalingInstances` listing (bounded by
   `provision_timeout`, same poll cadence as Requirement 7.2).
3. WHEN no instance carries the given NodeId THEN `decommission_node`
   SHALL resolve successfully (idempotency — quorum_management.hpp:158's
   documented contract; a second delete of an already-gone node is the
   common remediation race, not an error).
4. Removal of an instance that is the group's last SHALL be attempted, not
   special-cased; ESS MinSize constraint failures surface as exceptional
   futures naming the vendor error (the operator's MinSize is the
   authority on floor semantics, not this class).

---

### Requirement 9: `maintain_quorum`

**User Story:** As an operator, I want one-call remediation with the same
semantics every other provider has.

#### Acceptance Criteria

1. `maintain_quorum(cluster)` SHALL run the identical six-step sequence
   every sibling implements: assess → decommission unreachable members →
   compute per-group deficit from `config.topology` → provision into
   deficit groups (subject to Requirement 7.3's zone caveat) → return the
   **pre**-remediation health.
2. Failures in individual remediation steps SHALL be handled exactly as
   `aws_asg_quorum_manager` handles them (continue best-effort, surface
   via the returned health/logs), so cross-provider behavior stays
   predictable.

---

### Requirement 10: Fault Injection

**User Story:** As a test author, I want libfiu fault points at the
manager's API boundaries, so unit tests can exercise failure paths without
a network.

#### Acceptance Criteria

1. The manager SHALL expose libfiu fault points
   `raft/alibaba/ess/{list_instances,modify_capacity,remove_instances,maintain_quorum}`,
   following the `raft/oci/...` naming scheme.
2. Fault-point behavior (throw on enabled) and unit coverage SHALL mirror
   `oci_quorum_manager_unit_test.cpp`'s fault cases.
3. The OSS engine's fault points are Requirement 15.9.

---

### Requirement 11: Build Integration (No Vendor SDK to Detect)

**User Story:** As a build maintainer, I want Alibaba support gated like
OCI's — want-based Kconfig symbols, no find_package — so the build stays
simple and default-on.

#### Acceptance Criteria

1. `Kconfig` SHALL gain `menu "Alibaba Cloud Integration"` after the OCI
   menu with two sibling symbols (neither depending on the other,
   mirroring the OCI rationale): `ALIBABA_QUORUM_MANAGER` and
   `ALIBABA_OSS_PERSISTENCE`, each `default y`, each
   `depends on HTTP_TRANSPORT_TLS` with the same load-bearing https
   comment the OCI menu carries (every Alibaba endpoint is https;
   `CPPHTTPLIB_OPENSSL_SUPPORT` comes only from that symbol).
2. CMake SHALL use `kythira_kconfig_gate` for each symbol and define
   `KYTHIRA_ALIBABA_SHARED` when either is enabled (gating the shared
   signing/http/oss-client tests), mirroring `KYTHIRA_OCI_SHARED`
   (CMakeLists.txt:357-366). No `find_package`, no `vcpkg.json` change, no
   new `KYTHIRA_HAS_*` define.
3. `DEPENDENCIES.md` SHALL gain a cross-reference note under the existing
   httplib/OpenSSL/boost::json entries (the Requirement-11.3-of-OCI
   treatment), not a new dependency section.

---

### Requirement 12: `alibaba_ca_certificate_provider` — DESCOPED

**Descoped by operator decision, August 13, 2026: no Alibaba CA — private
or public — will be purchased for this project.** The TODO entry's
certificate-provider half is therefore not implemented: CAS Private CA is
a purchased resource with a real annual price, only sub-CAs may issue
end-entity certificates (so the minimum footprint is a root + sub-CA
purchase), and the cluster does not need a fifth issuance backend — Raft
mTLS material is deployment-agnostic and available from the existing
local, AWS ACM-PCA, GCP CAS, and Azure Key Vault providers.

**This is a cost decision, not a technical one, and it is revivable under
the right conditions** — for example: a deployment standardizing on
Alibaba Cloud that requires in-cloud issuance for compliance or
key-custody reasons; an organization that already owns a CAS Private CA
(the annual cost is then sunk); or the vendor introducing a
pay-per-certificate tier that removes the standing price. Nothing
architectural blocks revival: the foundation this spec builds
(`alibaba_client_config`, V3 signing, `alibaba_http_client`,
`endpoint_override` mocking, the CI federation path) is exactly what the
provider would sit on, and the pre-descope revision of this document (git
history of this file) holds the full drafted requirement set, including
the EKU-preference spike question, ready to restore.

The requirement number is retained so cross-references stay stable.

---

### Requirement 13: `ca_service --provider alibaba-ca` Integration — DESCOPED

Descoped with Requirement 12 (its only purpose was exposing that
provider). `cmd/ca_service` is untouched by this spec.

---

### Requirement 14: OSS Bucket Layout for Persistence

**User Story:** As a developer, I want a documented object layout, so the
engine's on-bucket format is inspectable, debuggable, and stable.

#### Acceptance Criteria

1. The engine SHALL store, under an operator-supplied `prefix`:
   `"<prefix>/term"` (decimal text), `"<prefix>/voted_for"` (decimal or
   `"none"`), `"<prefix>/log/<index>"` — one object per log entry, key
   zero-padded to 20 digits so lexicographic key order equals numeric
   index order — each holding the same one-line JSON record
   `file_persistence_engine` uses (`{term, index, command_b64, type}`),
   and `"<prefix>/snapshot"` (the same JSON codec).
2. One object per log entry (vs the file engine's single rewritten log
   file) SHALL be the layout precisely because it makes `append_log_entry`
   one PUT, `truncate_log`/`delete_log_entries_before` bounded batches of
   DELETEs, and recovery a keyed List — the header comment SHALL record
   this rationale and the contrast.
3. The format SHALL round-trip byte-arbitrary commands (base64, as the
   file engine does) and be covered by the same reload-survival test
   pattern (Requirement 16).

---

### Requirement 15: `alibaba_oss_persistence_engine`

**User Story:** As an operator, I want Raft persistent state in OSS, so
that node state survives instance loss and lives in a durable,
region-replicated store.

#### Acceptance Criteria

1. `include/raft/alibaba_oss_persistence.hpp` SHALL define
   `template<typename NodeId = std::uint64_t, typename TermId =
   std::uint64_t, typename LogIndex = std::uint64_t> class
   alibaba_oss_persistence_engine` satisfying `kythira::persistence_engine`
   (persistence.hpp:26) with a file-scope `static_assert`, constructed
   from `{alibaba_client_config, bucket, prefix}` with empty-field
   validation.
2. **Durability contract, stated head-on** (doc/TODO.md's mandate): every
   mutating method SHALL issue its PUT/DELETE synchronously on the calling
   thread and return only after OSS acknowledges with 2xx. OSS documents
   that a successful PUT response is sent only after the object is durably
   stored across replicas with strong read-after-write consistency; the
   header comment SHALL cite this as the fsync-equivalence argument, note
   that it is *stronger* than `file_persistence_engine`'s no-fsync
   write-then-rename, and — honestly — that `save_current_term`/
   `save_voted_for` now cost a network round trip (~tens of ms) on the
   election hot path, so election timeouts MUST be sized accordingly
   (documented per Requirement 18, with the measured figure from the real
   tier when an account exists).
3. WHEN a mutating OSS call fails (non-2xx, transport error) THEN the
   method SHALL throw after at most one same-call retry **only for
   idempotent full-overwrite PUTs** (term/voted_for/snapshot/log-entry
   records are all full-object overwrites keyed deterministically, so one
   retry is safe); the raft layer's existing treatment of persistence
   exceptions applies unchanged.
4. Like `file_persistence_engine`, the engine SHALL hold an in-memory
   mirror (mutex-guarded) loaded once at construction (List + GETs), so
   every read method answers from memory with no network I/O; only
   mutations touch OSS.
5. Construction over an empty prefix SHALL initialize the standard empty
   state (term 0, no vote, empty log, no snapshot) without writing any
   object until the first mutation.
6. WHEN objects under the prefix fail to parse at load THEN construction
   SHALL throw with the offending key named — a deliberate contrast with
   `file_persistence_engine`'s silent `catch (...)` tolerance, which the
   file engine's own header lists as a limitation; a cloud engine
   swallowing corruption would turn a truncated upload into silent state
   loss.
7. `save_snapshot` SHALL overwrite the single `"<prefix>/snapshot"` object
   (parity with the file engine's single-slot semantics); snapshot
   retention, backup catalogs, and restore-into-fresh-cluster flows are
   explicitly deferred to the forthcoming cloud key-object
   persistence-engine spec (doc/TODO.md's separate entry), for which this
   engine is the first instance — this spec SHALL NOT front-run that
   spec's cross-provider decisions.
8. **Single-writer requirement**: exactly one process may own a
   `{bucket, prefix}` pair, exactly as one process owns the file engine's
   directory; the engine provides no fencing, and the header SHALL say so.
   (OSS conditional-PUT fencing is a candidate for the cross-provider
   spec, not this one.)
9. libfiu fault points
   `raft/alibaba/oss/{put_object,get_object,delete_object,list_keys}`
   SHALL wrap each client call, following `memory_persistence_engine`'s
   `raft/persistence/...` precedent at the OSS boundary.

---

### Requirement 16: Tests

**User Story:** As a maintainer, I want the OCI-shaped test pyramid —
concept, unit, signature-verifying mock, gated real — so Alibaba support
is trustworthy without an account and verifiable with one.

#### Acceptance Criteria

##### Concept satisfaction

1. File-scope `static_assert`s per Requirements 4.2 and 15.1, plus a
   `mock`-style satisfaction case in the unit tests mirroring
   `persistence_concept_test.cpp`.

##### Unit tests (no network)

2. `tests/alibaba_signing_unit_test.cpp` SHALL pin V3 signing against the
   Task 0 golden vectors (canonical request, string-to-sign, final
   Authorization header) with injected time/nonce, plus
   empty-credential validation cases; `tests/alibaba_oss_client_unit_test.cpp`
   likewise for V4 signing, path-style override addressing, and the XML
   listing helper (including a truncated → error case per 2.4).
3. `tests/alibaba_http_client_unit_test.cpp` SHALL cover endpoint
   derivation, `endpoint_override` global takeover, throttling
   single-retry, error unwrapping (Code/Message/RequestId), and empty-body
   null — against a local `httplib::Server` via `endpoint_override`, the
   established no-seam mocking idiom (oci_quorum_manager_unit_test.cpp:134).
4. `tests/alibaba_quorum_manager_unit_test.cpp` SHALL cover constructor
   validation, tag scanning/NodeId assignment, foreign-instance exclusion
   (6.4), decommission idempotency (8.3), and every Requirement 10 fault
   point.
5. `tests/alibaba_oss_persistence_unit_test.cpp` SHALL run the
   `file_persistence_unit_test.cpp` pattern against a local OSS mock:
   every field's round trip AND reload-survival (destroy + reconstruct
   over the same mock state), binary command bytes, snapshot single-slot
   overwrite, corrupted-object → constructor throw (15.6), fault points
   (15.9), and a durability-ordering case asserting the PUT completed
   before the method returned (observable via the mock's request log).

##### Mock-server tests (no Alibaba account)

6. `tests/alibaba_mock_server.hpp` SHALL model the ESS/ECS routes the
   components call plus the OSS object operations, with in-memory state —
   and SHALL **verify request signatures from the bytes that actually
   arrived** (reconstructing each canonical request from the received
   Host/headers/body and recomputing the HMAC), the hard-won
   oci_mock_server.hpp rule whose header comment records why a
   non-verifying mock shipped two real defects.
7. `tests/alibaba_quorum_manager_mock_test.cpp` and
   `tests/alibaba_oss_persistence_mock_test.cpp` SHALL drive full
   provision/decommission/maintain and persistence flows
   against it, registered with CTest under labels
   `integration;alibaba;mock;<component>;cloud`. This tier is enabled by
   default (it needs no credentials), satisfying the house two-tier
   convention's "self-provisioned" tier the way OCI's mock tier does —
   Alibaba has no LocalStack-analogue emulator to round-trip against.

##### Real-Alibaba integration tests (gated, disabled by default)

8. `tests/alibaba_quorum_manager_real_test.cpp` and
   `tests/alibaba_oss_persistence_real_test.cpp` SHALL be compiled
   whenever the gates are on, guarded by a `KYTHIRA_ALIBABA_REAL_TESTS`
   compile definition, and **never registered with CTest** — run by name,
   the OCI Tier-3 contract exactly.
9. Exit code 77 SHALL mean skip, decided in a fixture constructor that
   prints `[alibaba-real] SKIP: ...` naming each missing value; env vars:
   `KYTHIRA_ALIBABA_REGION`, `KYTHIRA_ALIBABA_ACCESS_KEY_ID`,
   `KYTHIRA_ALIBABA_ACCESS_KEY_SECRET`, `KYTHIRA_ALIBABA_SECURITY_TOKEN`
   (optional), `KYTHIRA_ALIBABA_SCALING_GROUP_ID`, `KYTHIRA_ALIBABA_OSS_BUCKET`. A read-only
   pre-flight (`DescribeRegions`) failure SHALL skip, not fail — the
   OCI Requirement 13.9 rationale applies verbatim.
10. The real suites SHALL adopt the shared cost-reporting
    (`CostSummaryFixture`) and signal-driven-teardown
    (`SignalHandlerFixture`) patterns from `oci_real_test_support.hpp`,
    including unconditional best-effort teardown of anything provisioned
    and a post-run leak check.
11. **These suites have never run** — no account exists. Each real-test
    task in tasks.md that requires live verification SHALL be marked
    blocked-on-account rather than checked off against the mock tier;
    checking them off requires a live run, the doctrine every provider
    spec in this repo follows.

---

### Requirement 17: CI Wiring — Replace the `alibaba` Job Skeleton

**User Story:** As a maintainer, I want the real-cloud-tests `alibaba` job
to run these suites under keyless federation once an account exists.

#### Acceptance Criteria

1. The no-op `alibaba` job in `.github/workflows/real-cloud-tests.yml`
   SHALL be replaced with a real job gated by
   `REAL_CLOUD_TESTS_ALIBABA_ENABLED` plus per-bundle toggles
   `REAL_CLOUD_TESTS_ALIBABA_{ESS_QUORUM,OSS_PERSISTENCE}_ENABLED`
   (each with `workflow_dispatch` input overrides), including the
   zero-bundles fail-closed guard every provider job carries.
2. Credentials SHALL come from RAM **AssumeRoleWithOIDC** — a RAM OIDC
   IdP trusting GitHub's issuer and a role scoped per bundle — exchanged
   for temporary AccessKeyId/Secret/SecurityToken and exported as
   `KYTHIRA_ALIBABA_*`; no stored long-lived key for these suites.

   **The exchange SHALL use the vendor's official
   `aliyun/configure-aliyun-credentials-action`, not a hand-rolled
   in-workflow exchange.** This is a deliberate departure from the OCI
   job, which does hand-roll its UPST exchange — and the reason that
   precedent does not transfer is that OCI's only option was an
   unaudited *third-party* action, so owning ~10 lines of curl beat
   auditing someone else's implementation. Alibaba publishes a
   *first-party* action, which inverts the trade: it is maintained by
   the vendor whose API contract it implements, so it tracks changes to
   the exchange that a hand-rolled copy in this repo would not.

   Provisioning notes confirmed against the live account (August 13,
   2026 — spike-notes.md Finding 6):
   - OIDC identity-provider operations live under the **`ims`** product
     (Identity Management Service), **not `ram`**;
     `ram CreateOIDCProvider` does not exist, and the server rejects it
     on `ram.aliyuncs.com/2015-05-01` with `InvalidAction.NotFound`.
   - **`Fingerprints` is mandatory** on `CreateOIDCProvider`, unlike
     AWS's equivalent where the thumbprint is optional/auto-derived;
     `provision-oidc-role.sh` computes it from the issuer's live TLS
     chain rather than hardcoding a constant that a CA rotation would
     silently stale.
   - Trust-policy documents use `"Version": "1"`, Alibaba's own
     versioning — not AWS's `"2012-10-17"` date.

   (The existing `ALIBABA_CLOUD_ACCESS_KEY_*` secrets for CloudMonitor
   remote-write remain a documented, separate deviation — Basic-auth
   ingestion cannot carry an STS token; control-plane APIs can.)
3. `scripts/ci-cloud-credentials/alibaba/` SHALL grow the aws-shaped
   provisioning layout: `provision-oidc-role.sh` (idempotent; creates the
   OIDC IdP + role + per-bundle policies), `policies/<bundle>.json`
   fragments, and README setup walkthrough with cost estimates —
   extending the README this repo already ships for the monitoring
   deviation.
4. The job SHALL build the suites as named targets and run them directly
   (they are not CTest-registered), converting exit 77 into a loud
   failure in CI ("the bundle was enabled but skipped for want of
   configuration"), the run-real-cloud-suite.sh convention.
5. All toggles stay **off** and the job ships fail-closed naming missing
   variables until an operator provisions the account — mirroring how the
   azure/gcp/oci jobs shipped before their tenancies existed.

---

### Requirement 18: Documentation and Example Configuration

**User Story:** As an operator, I want worked configuration examples, so
adopting the provider does not require reading its implementation.

#### Acceptance Criteria

1. `docker/alibaba_quorum_manager/alibaba_quorum_manager.env.example`
   SHALL list one commented env var per config field (Requirements 3.1 and
   15.1's constructor inputs), mirroring
   `docker/oci_quorum_manager/oci_quorum_manager.env.example`.
2. `docker/alibaba_quorum_manager/README.md` SHALL document the
   prerequisite resources the code deliberately does not create (scaling
   group + scaling configuration + vSwitches per zone + security group;
   OSS bucket), the credential modes (AK pair, STS
   token, CI federation), the Requirement 7.3 zone-placement caveat, the
   Requirement 15.2 election-timeout sizing guidance, and a worked
   end-to-end example.
3. `doc/TODO.md`'s Alibaba Cloud entry SHALL flip to `[x]` with a summary
   naming what shipped and what remains blocked-on-account, and the cloud
   key-object persistence entry SHALL gain a note that the Alibaba OSS
   engine (its mandated first instance) ships here.
4. README.md's provider list and `DEPENDENCIES.md` (per 11.3) SHALL be
   updated.
