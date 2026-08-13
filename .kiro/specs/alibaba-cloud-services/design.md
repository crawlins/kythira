# Design Document

## Overview

| Component | Concept satisfied | Mechanism | Closest sibling |
|---|---|---|---|
| `alibaba_signing.hpp` / `alibaba_http_client.hpp` | — (foundation) | Hand-rolled ACS3-HMAC-SHA256 over httplib + boost::json | `oci_signing.hpp` / `oci_http_client.hpp` |
| `alibaba_oss_client.hpp` | — (foundation) | Hand-rolled OSS V4 header signing, 4 object operations | none (first data-plane client) |
| `alibaba_ess_quorum_manager` | `kythira::quorum_manager` | ESS scaling-group capacity + ECS tags/status | `aws_asg_quorum_manager` (semantics), `oci_instance_pool_quorum_manager` (transport) |
| `alibaba_oss_persistence_engine` | `kythira::persistence_engine` | One object per state item in an OSS bucket | `file_persistence_engine` (layout/mirror), none (cloud) |

**The load-bearing architectural decision of this spec: no vendor SDK.**
The general `aliyun-openapi-cpp-sdk` is not in vcpkg; the OSS-only
`aliyun-oss-cpp-sdk` is, but adopting it for one component would (a) add a
second HTTP stack (libcurl) to a process that otherwise speaks httplib,
(b) bury the persistence engine's durability-critical write/retry behavior
inside a vendor client, and (c) leave the mock tier fighting the SDK's
virtual-host bucket addressing. The OCI provider already proved the
hand-rolled shape end to end in this repo — signing, transport, mock
server that verifies signatures, real tier, CI federation — and Alibaba's
signing is *simpler* than OCI's (HMAC-SHA256 throughout; no RSA key
handling, no x509 federation exchange). Task 0 records this decision as
refutable: if the spike finds V3/V4 signing materially harder than
documented, the vcpkg OSS SDK is the fallback for the data plane only.

The second structural decision: **two signers, one config**. The control
plane (ESS/ECS/STS) and OSS share `alibaba_client_config` and the
HMAC/SHA helpers, but canonical forms differ enough (RPC-style
query-signing headers vs S3-like resource paths) that pretending they are
one scheme would produce a worse abstraction than two small, separately
golden-vectored signers.

## Non-Goals

- **No certificate provider.** Descoped by operator decision (August 13,
  2026) **on cost grounds**: no Alibaba CA — private or public — will be
  purchased. CAS Private CA is a paid resource whose minimum usable
  footprint is a root + sub-CA purchase, and Raft mTLS material is
  deployment-agnostic — the existing local/AWS/GCP/Azure providers cover
  issuance. **Revivable under the right conditions** (an Alibaba-standard
  deployment needing in-cloud issuance, an already-owned CA, or vendor
  pricing changes): the shared foundation this spec builds is exactly
  what the provider would sit on, and Requirement 12 records where the
  drafted requirements live. `cmd/ca_service` is untouched.
- **No heartbeat-tag liveness.** OCI grew a node-side heartbeat writer
  because its spec demanded application-level liveness. This provider
  mirrors `aws_asg_quorum_manager` instead: ESS `InService` + ECS
  `Running` is the liveness bar. An Alibaba heartbeat analogue (ECS tags
  written under an instance RAM role) is possible and deliberately
  deferred until a real deployment wants it.
- **No stockout-classification ladder.** OCI's out-of-capacity classifier
  exists because live runs taught us its exact error shapes. We have no
  Alibaba account and refuse to guess; scale-out failures surface raw
  (Requirement 7.6) until real failures justify classification.
- **No snapshot retention / backup catalog / restore-into-fresh-cluster /
  fencing.** Those are cross-provider decisions belonging to the
  forthcoming cloud key-object persistence spec (doc/TODO.md); this
  engine is its mandated first instance and keeps single-slot parity with
  `file_persistence_engine` so that spec can make those calls uniformly
  (Requirement 15.7, 15.8).
- **No scaling-group/CA/bucket lifecycle management.** Prerequisite
  resources are operator-owned, as in every sibling provider.
- **No chaos_node integration.** `chaos_node` uses
  `docker_quorum_manager` and `file_persistence_engine` by design; cloud
  managers/engines are consumed by tests and (for certificates)
  `ca_service`. Wiring the OSS engine into a product binary is future
  work once the cross-provider persistence spec exists.
- **No CloudMonitor changes.** The monitoring config shipped separately
  (doc/cloud_vendor_monitoring.md); this spec touches neither it nor its
  stored-AK ingestion deviation.

## Architecture

```
quorum_management layer
        │  (concept calls)
        ▼
alibaba_ess_quorum_manager ──────┐
        │                        │ tags/status/IPs
        │ capacity               ▼
        │  ESS: DescribeScalingGroups / DescribeScalingInstances /
        │       ModifyScalingGroup / RemoveInstances
        │  ECS: DescribeInstances / DescribeInstanceStatus / TagResources
        │                        │
        ▼                        ▼
   alibaba_http_client ── alibaba_signing (ACS3-HMAC-SHA256)
        │ httplib POST, fresh client per request
        ▼
   <service>.aliyuncs.com   (or endpoint_override → mock server)

raft node ──► alibaba_oss_persistence_engine ──► alibaba_oss_client
                    (in-memory mirror,               (OSS V4 signing,
                     synchronous writes)              PUT/GET/DELETE/List)
```

## Data Models

### `alibaba_client_config`

Per Requirement 1.1 — a plain aggregate, no constructor, validation in the
signer. `security_token` non-empty selects STS-credential mode (the token
rides as `x-acs-security-token` for the control plane and OSS's
`x-oss-security-token` for the data plane, both included in the signed
header set). `endpoint_override` is the single mock hook for all services
at once, including OSS (which switches to path-style addressing under it,
Requirement 2.3).

### ECS tag schema

| Tag key | Value | Writer |
|---|---|---|
| `kythira-cluster` | `config.cluster_name` | manager, at adoption |
| `kythira-node-id` | decimal NodeId | manager, at adoption |
| *(operator keys)* | `config.extra_tags` | manager, at adoption |

ECS `TagResources` is additive per-key (create-or-overwrite of exactly the
keys passed) — no read-merge-write cycle, deliberately contrasted in the
header with OCI's whole-map `freeformTags` replacement (that spec's
Property 1). The mock models additive semantics faithfully so a regression
toward map-replacement assumptions is testable.

### V3 canonical form (to be confirmed by Task 0)

Per vendor documentation: canonical request =
`HTTPMethod\nCanonicalURI\nCanonicalQueryString\nCanonicalHeaders\nSignedHeaders\nHashedRequestPayload`;
string-to-sign = `ACS3-HMAC-SHA256\n` + SHA-256(canonical request);
signature = HMAC-SHA256(AccessKeySecret, string-to-sign) hex; header
`Authorization: ACS3-HMAC-SHA256 Credential=<ak>,SignedHeaders=<h;h;h>,Signature=<hex>`.
Signed headers: `host`, `x-acs-action`, `x-acs-content-sha256`,
`x-acs-date`, `x-acs-signature-nonce`, `x-acs-version` (+
`x-acs-security-token` when present). **Task 0 confirms this against a
captured known-good exchange and the golden vectors come from that
capture** — the OCI spike corrected two guessed details of this kind
(host-header port, federation header set), and this spec assumes its
paraphrase is similarly fallible.

### OSS object layout

Per Requirement 14; keys under `<prefix>/`:

```
term                    "42"
voted_for               "7" | "none"
log/00000000000000000042    {"term":3,"index":42,"command_b64":"...","type":0}
snapshot                {file-engine snapshot JSON}
```

20-digit zero-padded log keys make lexicographic List order equal numeric
order, so recovery is one prefixed List (paginated) + ordered GETs, and
`get_last_log_index` at load is the last page's last key.

## Components and Interfaces

### 1. `include/raft/alibaba_signing.hpp`

```cpp
#pragma once
// V3 (ACS3-HMAC-SHA256) request signing for Alibaba Cloud RPC-style
// OpenAPI. Pure functions; time and nonce injectable for golden tests.
namespace kythira::alibaba_signing {

namespace detail {
struct openssl_deleter { /* EVP/HMAC RAII, as oci_signing */ };
auto sha256_hex(std::string_view) -> std::string;
auto hmac_sha256(std::string_view key, std::string_view msg) -> std::string;
auto percent_encode(std::string_view) -> std::string;  // vendor rules
}

auto canonical_request(method, path, query, headers) -> std::string;
auto string_to_sign(const std::string& canonical) -> std::string;

// Returns every header to attach, Authorization included.
auto sign_request(const alibaba_client_config& cfg,
                  std::string_view method, std::string_view host,
                  std::string_view path,
                  const std::map<std::string, std::string>& query,
                  std::string_view action, std::string_view version,
                  std::string_view body,
                  std::chrono::system_clock::time_point when,
                  std::string_view nonce)
    -> std::map<std::string, std::string>;

}  // namespace kythira::alibaba_signing
```

Throws `std::invalid_argument` on empty AK/secret (Requirement 1.4). No
stateful mode exists (unlike OCI's Instance Principal signer): STS
credentials arrive pre-obtained in the config, and CI's federation step
performs the AssumeRoleWithOIDC exchange outside the library (a deliberate
simplification — the exchange is one unauthenticated STS call in the
workflow, not a renewal loop the library must own).

### 2. `include/raft/alibaba_http_client.hpp`

```cpp
class alibaba_http_client {
  public:
    explicit alibaba_http_client(alibaba_client_config cfg);

    // One RPC-style call: derives host, signs, POSTs, parses JSON,
    // unwraps vendor errors, retries throttling once.
    [[nodiscard]] auto rpc(std::string_view service, std::string_view action,
                           std::string_view version,
                           const std::map<std::string, std::string>& params,
                           std::string_view body = "") const
        -> boost::json::value;
  private:
    [[nodiscard]] auto host_for(std::string_view service) const -> std::string;
    [[nodiscard]] auto make_client(const std::string& host) const
        -> httplib::Client;  // fresh per request, oci_http_client idiom
    alibaba_client_config _cfg;
};
```

Endpoint table (Task 0 confirms the region-qualified cases): `ess` →
`ess.aliyuncs.com`, `ecs` → `ecs.<region>.aliyuncs.com` (regional
endpoints documented for ECS), `sts` → `sts.aliyuncs.com`.
`endpoint_override` wins for all.

### 3. `include/raft/alibaba_oss_client.hpp`

```cpp
class alibaba_oss_client {
  public:
    explicit alibaba_oss_client(alibaba_client_config cfg);
    auto put_object(bucket, key, std::string_view bytes) const -> void;
    [[nodiscard]] auto get_object(bucket, key) const
        -> std::optional<std::string>;                  // nullopt on 404
    auto delete_object(bucket, key) const -> void;
    [[nodiscard]] auto list_keys(bucket, prefix) const
        -> std::vector<std::string>;                    // full pagination
  private:
    // OSS V4 signing (OSS4-HMAC-SHA256); virtual-host addressing, or
    // path-style under endpoint_override (Requirement 2.3).
};
```

Writes never auto-retry (Requirement 2.5); the engine owns write retries
because it knows its PUTs are idempotent full-object overwrites. The XML
listing helper extracts `<Key>`/`<IsTruncated>`/`<NextContinuationToken>`
by delimited scanning and throws on structural surprise (Requirement 2.4).

### 4. `include/raft/alibaba_ess_quorum_manager.hpp`

Config per Requirement 3.1; class per Requirement 4. Sequences:

#### `assess_quorum`
1. ESS `DescribeScalingInstances` (paginate) → instance IDs + lifecycle
   states.
2. ECS `DescribeInstances` batched by the API's ID-list limit → status,
   zone, private IP, tags in one call per batch (no per-instance fan-out —
   the batching ECS offers is why this manager needs no
   `parallel_for` helper like OCI's).
3. Filter to `kythira-cluster == cluster_name`; live = `InService` +
   `Running` (Requirement 6.2); aggregate per zone against
   `config.topology` with `aws_asg_quorum_manager`'s thresholds.

#### `provision_node`
1. Snapshot member IDs; `ModifyScalingGroup` DesiredCapacity+1.
2. Poll `DescribeScalingInstances` until a non-snapshot instance is
   `InService` and `Running`, or timeout.
3. Tag it (`TagResources`: cluster, node-id = tag-scan max+1, extras);
   resolve private IP; return `{node_id, ip:port}`. Zone mismatch with
   `target_group` proceeds with logging (Requirement 7.3).

#### `decommission_node`
1. Resolve instance by node-id tag (absent → resolved future, 8.3).
2. ESS `RemoveInstances` (capacity-decrementing form, Task 0-confirmed).
3. Poll until it leaves the group listing.

#### `maintain_quorum`
The six-step sibling-identical sequence (Requirement 9).

### 5. `include/raft/alibaba_oss_persistence.hpp`

```cpp
template <typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
          typename LogIndex = std::uint64_t>
class alibaba_oss_persistence_engine {
  public:
    alibaba_oss_persistence_engine(alibaba_client_config cfg,
                                   std::string bucket, std::string prefix);
    // ... the 12 persistence_engine methods ...
  private:
    mutable std::mutex _mu;
    // In-memory mirror: term, voted_for, ordered log map, snapshot —
    // loaded once at construction; reads never touch the network.
    alibaba_oss_client _oss;
};
```

Write path per method: serialize → fault point → `put_object`/
`delete_object` (one retry, PUTs only) → update mirror → return. The
mirror is updated **after** the OSS call succeeds, so a throwing write
leaves memory consistent with the store (the caller treats the operation
as failed; a retry re-executes the full PUT). `truncate_log(i)` deletes
keys ≥ pad(i) (batch of DELETEs, each fault-wrapped);
`delete_log_entries_before(i)` likewise below the bound. Load path:
`list_keys(prefix)` → GET each → parse-or-throw naming the key (15.6).

## Correctness Properties

### Property 1: A returned write is a durable write
**Validates: Requirement 15.2, 15.3**

The Raft safety argument needs `save_current_term`/`save_voted_for`
durable before the node acts on them. The engine returns only after OSS
acknowledges 2xx, and OSS's documented contract is that a PUT is
acknowledged only once durably stored, with strong read-after-write
consistency. There is no buffered/async path to get this wrong by
construction — the property could only fail if a non-2xx were treated as
success, so the unit tier includes a mock case where PUT returns 500 and
asserts the method throws and the mirror is unchanged.

### Property 2: Idempotency of decommission
Identical to Property 2 of `aws-quorum-manager`'s design, substituting
"no instance carries the node-id tag" for its absent-instance condition
(Requirement 8.3). **Validates: Requirement 8.3.**

### Property 3: NodeId assignment is tag-based, not identifier-derived
Identical to the sibling property in `aws_asg_quorum_manager`'s design:
ESS instance IDs give nothing stable to derive from, so identity lives in
tags and survives replacement. **Validates: Requirement 5.3.**

### Property 4: Foreign instances are invisible
An operator may share a scaling group with non-kythira instances or a
second cluster. Filtering on `kythira-cluster` before any counting,
tagging, or removal means the blast radius of every mutating call is
bounded to this cluster's members; the mock tier seeds foreign instances
to pin it. **Validates: Requirements 6.4, 8.1.**

### Property 5: Log-key ordering equals log-index ordering
20-digit zero-padding makes lexicographic OSS listing order the numeric
index order for all indices below 10^20; recovery correctness rests on
it, so a unit case pins the padding at the boundary (9→10 digits) and the
listing helper's pagination. **Validates: Requirements 14.1, 15.4.**

## Error Handling

- **Vendor API errors** (control plane): `std::runtime_error` carrying
  `Code`/`Message`/`RequestId` (Requirement 1.7); OSS errors likewise from
  XML (2.6).
- **Throttling**: one retry honoring `Retry-After` (control plane);
  persistence writes retry once at the engine layer, PUT/DELETE only
  (15.3).
- **Polling timeouts** (provision/decommission): exceptional future
  naming resource and elapsed time.
- **Constructor validation**: `std::invalid_argument` naming the field —
  every component.
- **Persistence load corruption**: constructor throw naming the object
  key (15.6) — the one place this provider is deliberately *stricter*
  than its file-engine sibling.

## Testing Strategy

### Unit tests (no network)
Golden-vector signing (V3 + OSS V4, vectors captured by Task 0),
endpoint/override/error/retry behavior against local `httplib::Server`s
via `endpoint_override`, constructor validation, tag logic, fault points,
persistence round-trip + reload-survival + durability-ordering
(Requirement 16.2–16.5). No issuance tier exists — the certificate
provider is descoped (Non-Goals).

### Mock-server tests (`tests/alibaba_mock_server.hpp`)
One server, all services (ESS/ECS + OSS path-style), in-memory state,
**signature verification from the received bytes** (16.6) — the
oci_mock_server rule, kept because it caught what golden vectors cannot:
divergence between what was sent and what was signed. Full
provision/decommission/maintain and persistence flows; CTest-registered,
on by default (16.7).

### Real-Alibaba integration tests (opt-in, `KYTHIRA_ALIBABA_REAL_TESTS`)
Compiled always (under the gates), never CTest-registered, exit-77 skip
with named missing values, read-only pre-flight, cost reporting and
signal-driven teardown from the OCI harness patterns (16.8–16.10).
**Blocked on an account existing; every task depending on a live run says
so** (16.11).

## Dependencies

```
httplib      (already required — HTTP transport, unconditional)
OpenSSL      (already required — HMAC/SHA/EVP for both signers)
boost::json  (already required — control-plane response parsing)
libfiu       (already optional — fault points, FIU_FOUND-gated)
```

No vcpkg.json change, no find_package, no new DEPENDENCIES.md section — a
cross-reference note under the existing entries (Requirement 11.3),
exactly the OCI treatment.
