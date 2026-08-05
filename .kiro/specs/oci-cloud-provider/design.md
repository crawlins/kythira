# Design Document

## Overview

This document designs two OCI-backed components:

| Component | Concept satisfied | Mechanism | AWS analogue |
|---|---|---|---|
| `oci_instance_pool_quorum_manager` | `quorum_manager` | `UpdateInstancePool` size + `DetachInstancePoolInstance` | `aws_asg_quorum_manager` |
| `oci_certificates_provider` | `certificate_provider` | OCI Certificates Management REST API | `aws_acm_pca_provider` |

Both are header-only and always compiled (gated only by kconfig opt-out
flags, not by SDK detection — see Requirement 11). Both speak the OCI REST
API directly over `httplib::Client`, `boost::json`, and OpenSSL — all three
already mandatory project dependencies — implementing OCI's request-signing
scheme by hand, in the same spirit as `acme_certificate_provider` speaking
RFC 8555 directly with no ACME SDK.

**This is the load-bearing architectural decision of this spec**: Oracle does
not publish an official C++ SDK, so there is no `aws-sdk-cpp`-shaped
dependency to wrap. Everything AWS gets from `aws-sdk-cpp` for free — request
signing, retry policy, response (de)serialization into typed structs — this
spec builds by hand on top of `httplib`/`boost::json`, scoped to only the
handful of OCI operations these two components actually call.

## Non-Goals

- **OCI Fault Domains** (the finer-grained placement dimension inside one
  Availability Domain): not modeled in the initial `GroupId = std::string`
  (AD-only) design. `oci_instance_pool_placement_config` (design-internal,
  not part of the public config surface — placement configs live on the
  pre-existing Instance Pool, not in `oci_instance_pool_quorum_manager_config`)
  reserves room for a future FD-aware extension analogous to
  `ec2_placement_group_config`, but nothing in this spec requires it.
- **Creating or deleting the Instance Pool, its Instance Configuration, or
  the OCI Certificate Authority resource.** Both are pre-existing,
  operator-provisioned inputs, exactly as `aws_acm_pca_provider` treats the
  ACM Private CA and as `aws_asg_quorum_manager` treats the ASG's launch
  template.
- **OCI preemptible instances, in the *quorum manager*** (OCI's analogue of
  AWS spot capacity): no equivalent to `ec2_spot_options` is added to
  `oci_instance_pool_quorum_manager`. Preemptible-vs-on-demand is configured
  on the pre-existing Instance Configuration, exactly as
  `aws_asg_quorum_manager` leaves the spot/on-demand mix entirely to the
  ASG's launch template — the quorum manager needs no changes, matching
  `aws_asg_quorum_manager`'s existing spot-agnosticism (Requirement 10.1's
  note in `aws-quorum-manager`).

  **This exclusion is scoped to the manager and does not extend to the
  real-OCI test harness** (Requirement 13.12–13.14). AWS's own
  `spot_first_launch_options`/`is_insufficient_capacity` escalation lives in
  `tests/aws_quorum_manager_real_ec2_test.cpp` and nowhere else — it is a
  property of the *test harness*, which launches instances directly, not of
  `aws_asg_quorum_manager`. The OCI harness needs the same capability for
  the same two reasons, one of which is not about cost at all:

  1. **Capacity.** "Out of host capacity" is a routine OCI failure for a
     single shape in a single Availability Domain, and a harness that picks
     one shape in one AD fails the whole suite when that AD is short. The
     GCP suite demonstrated this failure mode for real on 2026-08-03:
     `gcp_quorum_manager_real_gce_test` failed outright with
     `[ZONE_RESOURCE_POOL_EXHAUSTED] The zone 'us-central1-c' does not have
     enough resources available`. OCI is at least as prone to this.
  2. **Cost.** Preemptible instances are substantially cheaper, and the
     harness creates instances purely to observe lifecycle state.

  Note that OCI's stockouts are scoped by **(shape, Availability Domain)**,
  not by shape alone as on AWS, so the escalation ladder has an extra
  dimension: see Requirement 13.13.
- **Per-AD-targeted Instance Pool growth.** Confirmed absent by Task 0's
  spike (`spike-notes.md` Finding 2): `UpdateInstancePool` cannot target a
  specific AD within a multi-AD pool (Requirement 6.2). The one-Instance-
  Pool-per-AD requirement is therefore a confirmed constraint of this
  design, not a contingency — treated as an acceptable operational
  constraint, not a defect to work around inside the manager.
- **A general HTTP retry/backoff framework.** Requirement 1.9's single
  bounded 429 retry is intentionally minimal; building a configurable retry
  policy comparable to `aws-sdk-cpp`'s is out of scope for the set of calls
  this spec makes.

## Architecture

```
quorum_manager concept (quorum_management.hpp)
  │
  └── oci_instance_pool_quorum_manager        (include/raft/oci_instance_pool_quorum_manager.hpp)
        ├── oci_http_client                   (shared)
        ├── assess_quorum     → ListInstancePoolInstances + GetInstance (per-instance)
        ├── provision_node    → UpdateInstancePool(size+1) + poll + UpdateInstance (tag)
        │                       + ListVnicAttachments + GetVnic
        ├── decommission_node → ListInstancePoolInstances (locate) + DetachInstancePoolInstance
        └── maintain_quorum   → assess_quorum + decommission_node* + provision_node*
                                (identical algorithm to aws_ec2/asg_quorum_manager::maintain_quorum)

certificate_provider concept (certificate_provider.hpp)
  │
  └── oci_certificates_provider                (include/raft/oci_certificates_provider.hpp)
        ├── oci_http_client                     (shared)
        ├── root_certificate_pem → GetCertificateAuthorityBundle (cached)
        ├── sign_csr             → CreateCertificate(configType=
        │                          MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA)
        │                          + GetCertificate/GetCertificateBundle
        └── revoke                → RevokeCertificateVersion

Shared foundation (include/raft/):
  oci_client_config.hpp    — credentials / region / signing-mode config (unconditional)
  oci_signing.hpp          — Request Signing Version 1 canonical-string + header construction
  oci_http_client.hpp      — httplib::Client wrapper: signs, sends, parses JSON, retries 429 once

Test-only:
  tests/oci_mock_server.hpp                 — httplib::Server-based fake OCI REST API
  tests/oci_quorum_manager_unit_test.cpp
  tests/oci_quorum_manager_real_test.cpp             (KYTHIRA_OCI_REAL_TESTS)
  tests/oci_certificates_provider_real_test.cpp       (KYTHIRA_OCI_REAL_TESTS)
```

## Data Models

### `oci_client_config`

```cpp
#pragma once
#include <chrono>
#include <string>

namespace kythira {

struct oci_client_config {
    std::string region;                      // required, e.g. "us-phoenix-1"
    std::string tenancy_id;                   // required unless use_instance_principal
    std::string user_id;                      // required unless use_instance_principal
    std::string fingerprint;                  // required unless use_instance_principal
    std::string private_key_pem;              // required unless use_instance_principal
    std::string private_key_passphrase;       // optional
    bool use_instance_principal{false};
    std::string endpoint_override;            // empty = derive from region
    std::chrono::seconds api_timeout{30};
};

}  // namespace kythira
```

An aggregate, matching `aws_client_config`'s shape exactly — deliberately
symmetric so a reader already familiar with the AWS config immediately
recognizes the equivalent OCI fields.

### Instance freeform tag schema

```
kythira-cluster       = {cluster_name}
kythira-node-id       = {node_id}              ; decimal string
kythira-group         = {availability_domain}  ; e.g. "kIdk:PHX-AD-1"
kythira-managed-by    = kythira-oci-instance-pool-quorum-manager
kythira-last-heartbeat = {unix_timestamp}      ; written by the kythira process itself
```

Four tags are written by `provision_node` via the read-merge-write pattern
(Requirement 4.2); `kythira-last-heartbeat` is written by the kythira node
process using Instance Principal auth from inside the instance, mirroring
the AWS design's `kythira:last-heartbeat` tag exactly, substituting hyphens
for colons per Requirement 4.1's tag-key-charset caveat.

### OCI Request Signing Version 1 — canonical form (confirmed — `spike-notes.md` Finding 1)

Sourced directly from `oracle/oci-go-sdk`'s `common/http_signer.go`. The
signing string is built from a fixed, ordered subset of headers, each
formatted as `"{lowercased-name}: {value}"` and joined with `"\n"`, e.g.
for a `POST` with a body:

```
date: {rfc1123 date}
(request-target): post /20160918/instancePools/{id}
host: iaas.{region}.oraclecloud.com
content-length: {body byte length}
content-type: application/json
x-content-sha256: {base64(sha256(body))}
```

**`date` is first, then `(request-target)`, then `host`** — an earlier
draft of this section had `(request-target)` first; that was wrong and is
corrected here. The `(request-target)` pseudo-header's value is
`"{lowercased-method} {request-uri-path-and-query}"`.

Signed with the configured RSA private key (SHA-256 digest), producing a
base64 signature placed in an `authorization` header of the form:

```
Signature version="1",headers="date (request-target) host content-length
  content-type x-content-sha256",keyId="{tenancy_ocid}/{user_ocid}/{fingerprint}",
  algorithm="rsa-sha256",signature="{base64 signature}"
```

`GET`/`DELETE` requests (no body) omit `content-length`, `content-type`, and
`x-content-sha256` from both the signed-headers list and the canonical
string, leaving just `date`, `(request-target)`, `host` in that order.

Instance Principal signing replaces the API-key `keyId`/private-key pair
with a short-lived X.509 certificate + private key fetched from the local
instance metadata service, refreshed before expiry; the canonical-string
construction and `authorization` header shape are otherwise identical.
**The exact metadata-service paths and refresh cadence remain unconfirmed**
(`spike-notes.md`'s Conclusions, Task 0(b)) — `instance_principal_signer`'s
implementation is blocked on a follow-up desk-research or live-tenancy pass
before Requirement 1.6 can be implemented as more than a stub.

## Components and Interfaces

### 1. `include/raft/oci_signing.hpp`

```cpp
#pragma once
#include <raft/oci_client_config.hpp>
#include <map>
#include <string>
#include <string_view>

namespace kythira::oci_signing {

// Pure function: builds the canonical string per the scheme above and
// returns every header sign_request adds (including "authorization",
// "date", and — for non-empty bodies — "content-length"/"content-type"/
// "x-content-sha256"). Throws std::invalid_argument if cfg lacks the
// fields Requirement 1.5 requires for the selected auth mode.
[[nodiscard]] auto sign_request(const oci_client_config& cfg,
                                std::string_view method,
                                std::string_view request_target,
                                std::string_view host,
                                const std::string& body)
    -> std::map<std::string, std::string>;

// Instance Principal support: fetches/caches/refreshes the short-lived
// signing certificate from the local instance metadata service. Only
// exercised when cfg.use_instance_principal is true.
class instance_principal_signer {
public:
    [[nodiscard]] auto current_key_and_cert() -> std::pair<std::string, std::string>;
private:
    std::string _cached_key_pem;
    std::string _cached_cert_pem;
    std::chrono::system_clock::time_point _expiry;
};

}  // namespace kythira::oci_signing
```

`sign_request` is pure and side-effect-free for the API-key path, making it
directly unit-testable against golden fixed inputs (Requirement 13.3) the
same way `acme_jws.hpp`'s JWS construction is unit-tested against known
vectors. `instance_principal_signer` is the one stateful piece (it caches a
short-lived credential), isolated into its own small class so the pure
signing logic stays trivially testable independent of it.

### 2. `include/raft/oci_http_client.hpp`

```cpp
#pragma once
#include <raft/oci_client_config.hpp>
#include <raft/oci_signing.hpp>
#include <boost/json.hpp>
#include <httplib.h>
#include <string>
#include <string_view>

namespace kythira {

class oci_http_client {
public:
    explicit oci_http_client(oci_client_config cfg);

    // service: "iaas" | "certificatesmanagement" | "certificates" | "vaults"
    // (each OCI service has its own regional hostname).
    [[nodiscard]] auto request(std::string_view service, std::string_view method,
                               std::string_view path, std::string body = {},
                               std::string_view content_type = "application/json")
        -> boost::json::value;

private:
    oci_client_config _cfg;
    oci_signing::instance_principal_signer _ip_signer;  // unused unless use_instance_principal

    [[nodiscard]] auto host_for(std::string_view service) const -> std::string;
    [[nodiscard]] auto make_client(const std::string& origin) const
        -> std::unique_ptr<httplib::Client>;
};

}  // namespace kythira
```

This mirrors `acme_certificate_provider_impl.hpp`'s `acme_detail::make_client`
helper (connection/read timeouts, TLS enabled by scheme) almost exactly —
the only OCI-specific addition is that every request's headers are computed
by `oci_signing::sign_request` first, and every response is parsed as JSON
via `boost::json::parse` (already a project dependency, used by the ACME
provider for its own JSON handling) before being returned or thrown as an
error.

### 3. `include/raft/oci_instance_pool_quorum_manager.hpp`

#### Configuration struct

```cpp
struct oci_instance_pool_quorum_manager_config {
    oci_client_config oci{};
    std::string compartment_id;                     // required
    std::string cluster_name;                       // required
    std::string instance_pool_id;                    // required
    std::uint16_t node_port{7000};
    std::chrono::seconds heartbeat_timeout{30};
    std::chrono::seconds heartbeat_grace_period{120};
    desired_topology<std::string> topology{};
    std::chrono::seconds provision_timeout{300};
    std::chrono::milliseconds poll_interval{5000};
    std::map<std::string, std::string> extra_tags{};
};
```

#### Class sketch

```cpp
template<typename NodeId = std::uint64_t, typename Address = std::string>
requires kythira::node_id<NodeId>
class oci_instance_pool_quorum_manager {
public:
    using node_id_type            = NodeId;
    using address_type            = Address;
    using placement_group_id_type = std::string;  // Availability Domain

    explicit oci_instance_pool_quorum_manager(oci_instance_pool_quorum_manager_config cfg);

    oci_instance_pool_quorum_manager(const oci_instance_pool_quorum_manager&)            = delete;
    oci_instance_pool_quorum_manager& operator=(const oci_instance_pool_quorum_manager&) = delete;
    oci_instance_pool_quorum_manager(oci_instance_pool_quorum_manager&&)                 = default;
    oci_instance_pool_quorum_manager& operator=(oci_instance_pool_quorum_manager&&)      = default;

    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::Future<quorum_health<NodeId, std::string>>;

    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::Future<quorum_health<NodeId, std::string>>;

    auto provision_node(std::string target_group, std::optional<NodeId> replacing)
        -> kythira::Future<peer_info<NodeId, Address>>;

    auto decommission_node(const NodeId& node) -> kythira::Future<void>;

    [[nodiscard]] auto topology() const -> kythira::desired_topology<std::string>;

private:
    oci_instance_pool_quorum_manager_config _cfg;
    oci_http_client _http;
    std::vector<std::string> _availability_domains;  // read at construction

    auto node_id_str(const NodeId&) const -> std::string;
    auto next_node_id() const -> NodeId;
    auto find_instance_id(const NodeId&) const -> std::optional<std::string>;
    auto merged_tags(const std::string& instance_id,
                     const std::map<std::string, std::string>& new_tags) const
        -> std::map<std::string, std::string>;
    auto apply_tags(const std::string& instance_id,
                    const std::map<std::string, std::string>& tags) const -> void;
    auto private_ip_of(const std::string& instance_id) const -> std::string;
    static auto compute_quorum_status(std::size_t live, std::size_t total) -> quorum_status;
};

static_assert(quorum_manager<oci_instance_pool_quorum_manager<std::uint64_t, std::string>,
                             std::uint64_t, std::string, std::string>);
```

#### `assess_quorum` sequence

```
1. Check fault point "raft/oci/instance_pool/list_instances".
2. instances = _http.request("iaas", "GET",
       "/20160918/instancePools/{instance_pool_id}/instances?compartmentId={compartment_id}")
3. For each instance summary, GetInstance(instanceId) to read lifecycleState
   and freeformTags (issued concurrently, bounded worker pool — Req 5.2).
4. Classify each as live/unreachable using the same rule as
   aws_ec2_quorum_manager::assess_quorum (running-equivalent state +
   heartbeat/grace-period check), building live_map and group_live keyed by
   kythira-group.
5. Iterate the supplied cluster vector; classify; build placement_group_health
   entries with target_count from _cfg.topology.
6. Compute quorum_status via compute_quorum_status(live, total).
7. Return quorum_health wrapped in an immediately-resolved Future; any
   failed OCI call along the way instead returns an exceptional Future.
```

#### `provision_node` sequence

```
1. Check fault point "raft/oci/instance_pool/update_size".
2. Validate target_group is one of _availability_domains → exceptional
   Future (std::invalid_argument) if not.
3. pool = GetInstancePool(instance_pool_id); orig_size = pool.size
4. UpdateInstancePool(instance_pool_id, size = orig_size + 1)
   → on failure: exceptional Future, no cleanup needed.
5. Poll ListInstancePoolInstances every poll_interval:
     find an instance whose freeformTags lacks "kythira-node-id"
   until found or provision_timeout elapses.
6. If timeout: UpdateInstancePool(size = orig_size) best-effort rollback;
   return exceptional Future.
7. new_id = next_node_id()   // max-of-parsed "kythira-node-id" tags + 1,
                              // scanned across ListInstancePoolInstances
8. apply_tags(new_instance_id, {kythira-cluster, kythira-node-id,
   kythira-group=target_group, kythira-managed-by} ∪ extra_tags)
   via merged_tags() (read-merge-write, Requirement 4.2)
9. vnics = ListVnicAttachments(new_instance_id)
   vnic  = GetVnic(vnics[0].vnicId)
   private_ip = vnic.privateIp
10. Return peer_info{new_id, "{private_ip}:{node_port}"}
```

#### `decommission_node` sequence

```
1. Check fault point "raft/oci/instance_pool/detach_instance".
2. instance_id = find_instance_id(node_id)   // scan ListInstancePoolInstances
   → if not found: return resolved Future (idempotent)
3. state = GetInstance(instance_id).lifecycleState
   → if TERMINATING or TERMINATED: return resolved Future
4. DetachInstancePoolInstance(instance_pool_id, instance_id,
                              isDecrementSize=true, isAutoTerminate=true)
   → on failure: exceptional Future
5. Poll GetInstance(instance_id) until lifecycleState != RUNNING or 30s elapse.
6. Return resolved Future.
```

#### `maintain_quorum` sequence

Identical in shape to `aws_ec2_quorum_manager::maintain_quorum` /
`aws_asg_quorum_manager::maintain_quorum` (design.md's six-step sequence in
`aws-quorum-manager`), substituting this class's own `assess_quorum`/
`decommission_node`/`provision_node` and fault point
`"raft/oci/instance_pool/maintain_quorum"`. Not reproduced verbatim here to
avoid drift between two copies of the same six steps — see that document's
`maintain_quorum sequence` section, which this implementation follows
exactly.

#### `next_node_id` / tag helpers

```cpp
auto oci_instance_pool_quorum_manager::next_node_id() const -> NodeId {
    // ListInstancePoolInstances (all lifecycle states); scan
    // "kythira-node-id" freeform tags; parse to NodeId; return max + 1,
    // or NodeId{1} if none found. Identical in spirit to
    // aws_asg_quorum_manager::next_node_id(), substituting freeform tags
    // for EC2 tags.
}

auto oci_instance_pool_quorum_manager::merged_tags(
    const std::string& instance_id,
    const std::map<std::string, std::string>& new_tags) const
    -> std::map<std::string, std::string> {
    auto current = GetInstance(instance_id).freeformTags;  // may be empty
    for (const auto& [k, v] : new_tags) current[k] = v;
    return current;
}
```

The read-merge-write pattern in `merged_tags` is the direct consequence of
Property 1 below — `UpdateInstance`'s freeform-tag field is a full
replacement, not a merge, unlike AWS `CreateTags`.

### 4. `include/raft/oci_certificates_provider.hpp`

```cpp
struct oci_certificates_provider_config {
    oci_client_config oci{};
    std::string compartment_id;            // required
    std::string certificate_authority_id;  // required
    std::chrono::seconds validity{std::chrono::hours(24 * 30)};
};

class oci_certificates_provider {
public:
    explicit oci_certificates_provider(oci_certificates_provider_config cfg);

    [[nodiscard]] auto root_certificate_pem() -> kythira::future_default<std::string>;
    [[nodiscard]] auto sign_csr(std::string csr_pem, csr_signing_options options)
        -> kythira::future_default<pem_material>;
    [[nodiscard]] auto revoke(const std::string& certificate_serial)
        -> kythira::future_default<void>;

private:
    oci_http_client _http;
    oci_certificates_provider_config _config;
    std::mutex _mutex;
    std::optional<std::string> _cached_root_pem;
};

static_assert(certificate_provider<oci_certificates_provider>);
```

`sign_csr`'s body (confirmed by Task 0's spike, `spike-notes.md` Finding 3)
calls `CreateCertificate` with `certificateConfig.configType =
"MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA"`, `issuerCertificateAuthorityId
= _config.certificate_authority_id`, and `csrPem = csr_pem`, then polls
`GetCertificate`/`GetCertificateBundle` until `lifecycleState == "ACTIVE"`
or `_config.oci.api_timeout` elapses, returning `pem_material` with
`private_key_pem` left empty. No Vault/`secrets` service call is needed —
an earlier draft of this header anticipated a Vault-key-export fallback
for the case where OCI generates the key internally, which Finding 3 ruled
out as unnecessary: OCI does accept the caller's own CSR.

## Correctness Properties

### Property 1: Freeform tags require read-merge-write, not blind write

**Validates: Requirement 4.2, 4.3**

Unlike AWS `CreateTags` (additive — existing tags untouched unless the same
key is reused), OCI `UpdateInstance`'s `freeformTags` field is a full
replacement of the resource's tag map. Every tag-writing call in
`oci_instance_pool_quorum_manager` therefore reads the instance's current
tags via `GetInstance` first and writes back the union. Skipping this step
would silently erase any tag an operator or another system had set on the
instance directly — a correctness bug with no error signal, since
`UpdateInstance` itself succeeds regardless of what it overwrites. A unit
test (Requirement 13.4) exists specifically because this failure mode is
silent.

### Property 2: Idempotency of decommission

**Validates: Requirement 7.2, 7.4**

`decommission_node` returns a resolved Future when no matching instance is
found, or when the found instance is already `TERMINATING`/`TERMINATED` —
identical in spirit to Property 2 of `aws-quorum-manager`'s design, applied
to OCI's lifecycle states instead of EC2's.

### Property 3: NodeId assignment is tag-based, not identifier-derived

**Validates: Requirement 6.4 (Glossary: OCID)**

Unlike `aws_ec2_quorum_manager` (which derives `NodeId` from the EC2
instance ID's hex suffix with zero extra API calls), OCIDs are not
hex-decodable, so `oci_instance_pool_quorum_manager` always pays the same
"scan tags, take max + 1" cost that `aws_asg_quorum_manager` already pays
for the identical structural reason (ASG-launched instance IDs are equally
unusable as a numeric seed). This is not a regression introduced by this
spec — it is the same cost profile as the closer AWS analogue.

### Property 4: Topology-invariant replacement

**Validates: Requirement 9.2**

Identical to Property 7 of `aws-quorum-manager`'s design:
`maintain_quorum` computes per-AD deficits from `config.topology` and never
provisions a replacement into an AD other than the one with a deficit.

### Property 5: Application-level health detection via heartbeat timeout

**Validates: Requirement 5.3**

Identical to Property 5 of `aws-quorum-manager`'s design, substituting
`lifecycleState == RUNNING` for EC2's `running` state check and the
`kythira-last-heartbeat` freeform tag for `kythira:last-heartbeat`.

## Error Handling

- **OCI error responses**: `oci_http_client::request` parses a non-2xx
  response body's `{"code": ..., "message": ...}` shape and throws
  `std::runtime_error` with both fields concatenated, the closest analogue
  to how the AWS managers unwrap `AWSError::GetMessage()`.
- **Transient throttling (429)**: one bounded retry per Requirement 1.9;
  a second 429 propagates as an error. No general backoff framework (see
  Non-Goals).
- **Polling timeouts**: `provision_node`'s timeout path attempts a
  best-effort `UpdateInstancePool(size = original)` rollback before
  rejecting the Future, mirroring `aws_asg_quorum_manager`'s equivalent
  rollback (it cannot terminate a *specific* not-yet-identified instance,
  same limitation AWS's ASG manager has).
- **Constructor validation**: missing required config fields throw
  `std::invalid_argument` synchronously, before any OCI API call, matching
  every existing quorum manager and certificate provider in this codebase.
- **Instance Principal credential refresh failure**: if the metadata
  service is unreachable (e.g. the calling process is not actually running
  on an OCI compute instance despite `use_instance_principal=true`),
  `sign_request` propagates the underlying HTTP error from
  `instance_principal_signer::current_key_and_cert()` rather than silently
  falling back to unsigned requests.

## Testing Strategy

### Unit tests (no network)

Cover construction validation, `oci_signing::sign_request`'s canonical
string against golden fixed vectors derived from the confirmed construction
in `design.md`'s "OCI Request Signing Version 1" section
(`spike-notes.md` Finding 1 is the source of truth for these vectors), the
tag read-merge-write behavior (Property 1), and every fault-injection point
via `fiu_enable`/`fiu_disable`, following the exact pattern
`aws_quorum_manager_unit_test.cpp` already uses.

### Mock-server tests (`tests/oci_mock_server.hpp`)

An `httplib::Server` subclass exposing just the handful of routes this
spec's components call, with in-memory `InstancePool`/`Instance`/
`Certificate` state and synchronous (no simulated boot delay) lifecycle
transitions — directly analogous to `tests/acme_test_server.hpp`'s role for
the ACME provider, filling the gap left by OCI having no LocalStack
equivalent (see Requirement 13.5's rationale, and `doc/TODO.md`'s explicit
allowance for a config/mock substitute where no vendor emulator exists).
Enabled by default; no opt-in build flag, since it introduces no new
dependency beyond `httplib`.

### Real-OCI integration tests (opt-in, `KYTHIRA_OCI_REAL_TESTS`)

Structured like `aws_quorum_manager_real_ec2_test.cpp`'s fixture: a
pre-flight identity check that skips (not fails) the whole suite on
failure, per-test cost estimation and reporting, and signal-driven cleanup.
The fixture differs from the AWS one in requiring more pre-existing,
env-var-supplied resources up front (Requirement 13.11) since this spec's
components never create an Instance Pool, Instance Configuration, VCN, or
Certificate Authority themselves — those must already exist in the test
tenancy.

## Dependencies

```
No new third-party dependency. Both components build on:
  httplib      (already required — used unconditionally by acme_certificate_provider)
  boost::json  (already required — used unconditionally elsewhere in this project)
  OpenSSL      (already required — RSA-SHA256 signing for oci_signing.hpp)
```

`DEPENDENCIES.md` gains a cross-reference note only (Requirement 11.3); no
new install instructions, no new `vcpkg.json`/`CMakeLists.txt` package
entry.
