# Requirements Document

## Introduction

This document specifies the requirements for Oracle Cloud Infrastructure (OCI)
support in Kythira, closing the `doc/TODO.md` "Cloud Provider Support" entry:

> **Oracle Cloud Infrastructure (OCI)** — quorum manager backed by an Instance
> Pool and a `certificate_provider` backed by OCI Certificates Service

Two components ship together, matching how `ci-real-cloud-tests/design.md`
already scopes "a future spec adds that provider's quorum manager/certificate
provider AND a corresponding real-cloud test suite for it":

1. **`oci_instance_pool_quorum_manager`** — a `quorum_manager` implementation
   (`include/raft/quorum_management.hpp`) backed by an OCI Instance Pool, the
   OCI analogue of an AWS Auto Scaling Group. This is the primary artifact of
   this spec.
2. **`oci_certificates_provider`** — a `certificate_provider` implementation
   (`include/raft/certificate_provider.hpp`) backed by the OCI Certificates
   Management service.

**The single biggest architectural difference from the existing
`aws_ec2_quorum_manager`/`aws_asg_quorum_manager`/`aws_acm_pca_provider` work
(`.kiro/specs/aws-quorum-manager/`) is that Oracle does not publish an
official C++ SDK for OCI.** AWS's implementations wrap `aws-sdk-cpp`; Oracle's
supported SDKs are Java, Python, Go, .NET, Ruby, and TypeScript/JavaScript
only. This spec therefore does **not** follow the AWS SDK-wrapping pattern.
Instead it follows the pattern already established in this codebase by
`acme_certificate_provider` (`include/raft/acme_certificate_provider.hpp`),
which speaks RFC 8555 directly over `httplib::Client` with no ACME SDK
dependency: both new OCI components speak the OCI REST API directly over the
project's existing `httplib`/`boost::json`/OpenSSL dependencies (all three
already mandatory, unconditional project dependencies — see
`DEPENDENCIES.md` — used unconditionally by `acme_certificate_provider`
today), implementing OCI's request-signing scheme by hand.

This document originally marked two areas as **open questions requiring a
spike** (Requirement 1 and Requirement 12): (a) the exact request-signing
wire format and Instance Principal metadata-service contract, and (b)
whether the OCI Certificates Management service actually accepts a
caller-supplied CSR at all, since its self-service issuance flow was
suspected (before verification) to generate the key pair internally — a
materially different model from AWS ACM Private CA's `IssueCertificate(CSR)`.
Task 0 in `tasks.md` requires a real, dated `spike-notes.md` (mirroring
`.kiro/specs/boost-beast-http-transport/spike-notes.md`) recording actual
findings against real OCI API documentation and/or a live OCI tenancy before
either component's implementation proceeds past its signing/CSR-shaped
requirements.

**Update (see `spike-notes.md`, dated 2026-07-28):** a desk-research pass
against Oracle's own open-source `oci-go-sdk`/`oci-python-sdk`/
`terraform-provider-oci` repositories has since **resolved most of these
questions**. Most importantly: OCI Certificates Management **does** accept
a caller-supplied CSR (config type `MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA`,
confirmed directly from the `oci-go-sdk` struct source), so
`oci_certificates_provider::sign_csr` follows the same clean,
private-key-never-leaves-the-caller shape as every other
`certificate_provider` implementation in this codebase — the feared
Vault-export fallback is not needed. The exact OCI Request Signing Version 1
canonical-string construction is also now confirmed (with one correction to
this document's original header-order assumption, applied below), and
Instance Pool growth is confirmed to be pool-wide, not per-AD-targetable,
settling Requirement 6.2's fallback as the required design rather than a
contingency. `spike-notes.md` also corrects the certificate-revocation
operation name from this document's original guess. Two Task 0 sub-items —
the Instance Principal metadata-service contract, and CI OIDC federation —
remain genuinely open; see `spike-notes.md`'s Conclusions and `tasks.md`'s
updated Task 0 for exactly what's left.

## Glossary

- **OCID**: Oracle Cloud Identifier — OCI's globally unique resource
  identifier, e.g. `ocid1.instance.oc1.phx.anyhqljrgvpg...`. Unlike an AWS EC2
  instance ID (`i-0123456789abcdef0`, a fixed-width hex string), an OCID's
  trailing segment is a variable-length, non-hex-decodable opaque token.
  **This means the AWS `aws_ec2_quorum_manager` trick of deriving `NodeId`
  directly from the instance identifier (hex suffix → `uint64_t`) does not
  transfer to OCI.** `oci_instance_pool_quorum_manager` instead follows the
  `aws_asg_quorum_manager` pattern: `NodeId` is assigned by the manager and
  recorded in a `kythira-node-id` freeform tag, discovered by scanning tags
  (max-of-parsed + 1), exactly as `aws_asg_quorum_manager::next_node_id()`
  already does because ASG instance IDs are likewise not usable as a numeric
  seed.
- **Compartment**: An OCI logical container that scopes resource visibility
  and IAM policy, roughly analogous to an AWS account/sub-account boundary
  within one tenancy. Every OCI API call operates within a `compartmentId`.
- **Tenancy**: The root compartment; the OCI equivalent of an AWS account ID.
- **Region**: An OCI region (e.g. `us-phoenix-1`). OCI region identifiers are
  lowercase-hyphenated, distinct from AWS's `us-east-1` style but serve the
  same purpose.
- **Availability Domain (AD)**: An isolated OCI data center within a region,
  e.g. `"kIdk:PHX-AD-1"` (the leading token is tenancy-specific). This spec's
  `GroupId = std::string` maps one-to-one to an AD, the direct analogue of an
  AWS Availability Zone.
- **Fault Domain (FD)**: A finer-grained failure domain *within* one AD (1–3
  per AD), OCI's analogue of an EC2 Placement Group's `spread`/`partition`
  strategies. Out of scope for the initial implementation (see Non-Goals in
  `design.md`); `oci_instance_pool_placement_config` reserves a field for it.
- **Instance Configuration**: A template resource (`CreateInstanceConfiguration`)
  describing shape, image, VNIC, and metadata for instances an Instance Pool
  launches — the OCI analogue of an AWS EC2 Launch Template.
- **Instance Pool**: A `ComputeManagement` resource that launches and manages
  a set of instances from one Instance Configuration, spread across one or
  more Instance Pool Placement Configurations (AD + subnet + optional FD
  list). Size is driven by `UpdateInstancePool`'s `size` field — the OCI
  analogue of an AWS Auto Scaling Group's `DesiredCapacity`. This is the
  resource `oci_instance_pool_quorum_manager` drives.
- **Instance Pool Placement Configuration**: One `{availabilityDomain,
  primarySubnetId, faultDomains[]}` entry inside an Instance Pool's config —
  the OCI analogue of an ASG's per-AZ subnet mapping.
- **`DetachInstancePoolInstance`**: The OCI call that removes one instance
  from a pool, with `isDecrementSize` (don't replace it) and
  `isAutoTerminate` (terminate the detached instance rather than merely
  freeing it) flags — the OCI analogue of AWS's
  `TerminateInstanceInAutoScalingGroup(ShouldDecrementDesiredCapacity=true)`.
- **`lifecycleState`**: OCI Compute's per-resource status enum. For an
  Instance: `MOVING, PROVISIONING, RUNNING, STARTING, STOPPING, STOPPED,
  CREATING_IMAGE, TERMINATING, TERMINATED`. Live == `RUNNING`, the OCI
  analogue of an EC2 instance's `instance-state-name = running` check.
- **VCN (Virtual Cloud Network)**: OCI's analogue of an AWS VPC.
- **NSG (Network Security Group)**: OCI's analogue of an AWS EC2 Security
  Group — a set of ingress/egress rules attachable directly to a VNIC.
- **Service Gateway**: OCI's analogue of an AWS VPC Gateway Endpoint — lets
  private-subnet instances reach OCI public services (Object Storage, etc.)
  without a NAT Gateway or public IP.
- **NAT Gateway**: Same concept and name as AWS's; provides private-subnet
  egress to the public internet.
- **Object Storage**: OCI's analogue of AWS S3 — used by the real-OCI
  integration test fixture to host the `kythira-node` binary for instance
  bootstrap, mirroring `aws_quorum_manager_real_ec2_test.cpp`'s S3 usage.
- **Vault / Secrets**: OCI's analogue of AWS Secrets Manager / KMS; the
  Certificates Management service stores certificate private keys here when
  it generates them itself (see Requirement 12's open question).
- **Instance Principal**: An OCI authentication mechanism letting a compute
  instance call OCI APIs using an identity derived from the instance's own
  metadata and a short-lived certificate obtained from the local instance
  metadata service (`http://169.254.169.254/opc/v2/identity/cert.pem` and
  related endpoints) — analogous in *purpose* to an AWS IAM instance profile,
  but different in mechanism (certificate-based request signing, not a
  bearer session token).
- **Dynamic Group + Policy**: OCI's mechanism for granting Instance
  Principals permissions — a Dynamic Group matches instances by a rule
  (e.g. compartment membership or a freeform tag), and an IAM Policy grants
  that Dynamic Group specific verbs on specific resource types. Analogous to
  an AWS IAM role's trust policy + permission policy pair.
- **Request Signing Version 1**: OCI's HTTP request-signing scheme —
  conceptually similar to AWS SigV4 or an HTTP Signatures draft, but with its
  own canonical-string format and header set. Confirmed by Task 0's spike,
  not assumed; see Requirement 1.
- **Certificates Management service**: The OCI service exposing
  `CreateCertificateAuthority`, `CreateCertificate`, and related operations,
  reachable at a region-specific `certificatesmanagement.{region}.oraclecloud.com`
  endpoint. The OCI analogue of AWS Certificate Manager Private CA, with a
  materially different issuance model — see Requirement 12.
- **freeform tag**: An OCI resource tag: a flat `string → string` map,
  directly analogous to an AWS EC2 tag, set via each resource's `Update*`
  call (there is no separate `CreateTags` call as in AWS — freeform tags are
  a field on the resource's own update request and are **replaced**, not
  merged, unless the caller reads the existing map first and writes it back
  with additions).

---

## Requirements

### Requirement 1: OCI Request Signing and HTTP Client Foundation

**User Story:** As a library user who wants to run Kythira on OCI, I want a
signing/HTTP-client layer that authenticates every OCI API call correctly,
without a vendor SDK dependency, so that both the quorum manager and the
certificate provider can be built on the same tested foundation.

#### Acceptance Criteria

1. **Confirmed by Task 0's spike** (`spike-notes.md`, Finding 1, sourced
   directly from `oracle/oci-go-sdk`'s `common/http_signer.go`): the
   canonical string is built by lowercasing each signed header's name,
   formatting each as `"{name}: {value}"`, and joining the lines with `"\n"`;
   the `(request-target)` pseudo-header's value is `"{lowercased-method}
   {request-uri-path-and-query}"`. **Signed-header order for GET/DELETE**
   (no body): `date`, `(request-target)`, `host`. **Signed-header order for
   POST/PUT/PATCH** (with body): the same three, then `content-length`,
   `content-type`, `x-content-sha256`. The resulting `authorization` header
   is `Signature version="1",headers="{space-separated signed headers, in
   the order above}",keyId="{tenancy_ocid}/{user_ocid}/{fingerprint}",
   algorithm="rsa-sha256",signature="{base64 RSA-SHA256 signature}"`. This
   corrects an earlier draft of this requirement, which had listed
   `(request-target)` ahead of `date`. The Instance Principal
   metadata-service contract (Requirement 1.6) remains **unconfirmed** —
   `spike-notes.md`'s Conclusions list it as still open; Task 0 in
   `tasks.md` is updated to reflect that only that sub-item (plus CI OIDC
   federation, Requirement 14.2) still needs resolution before the
   corresponding code is written.
2. An `oci_client_config` struct SHALL be defined in
   `include/raft/oci_client_config.hpp` (compiled unconditionally — no
   feature-detection guard is needed since its only dependencies are
   `<chrono>`/`<string>`/`<optional>`, mirroring `aws_client_config.hpp`'s own
   unconditional compilation) with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `region` | `std::string` | *(required)* | OCI region identifier, e.g. `"us-phoenix-1"` |
   | `tenancy_id` | `std::string` | `""` | Tenancy OCID; required unless `use_instance_principal` is true |
   | `user_id` | `std::string` | `""` | User OCID; required unless `use_instance_principal` is true |
   | `fingerprint` | `std::string` | `""` | API signing key fingerprint; required unless `use_instance_principal` is true |
   | `private_key_pem` | `std::string` | `""` | RSA private key PEM for API-key signing; required unless `use_instance_principal` is true |
   | `private_key_passphrase` | `std::string` | `""` | Passphrase for `private_key_pem`, if encrypted |
   | `use_instance_principal` | `bool` | `false` | When true, ignore the four API-key fields above and authenticate via the instance metadata service (Requirement 1.6) |
   | `endpoint_override` | `std::string` | `""` | Override the derived `https://{service}.{region}.oraclecloud.com` base URL — for future local-mock testing (Requirement 13) |
   | `api_timeout` | `std::chrono::seconds` | `30s` | Per-call HTTP timeout |

3. `oci_client_config` SHALL be an aggregate (no user-declared constructors),
   matching `aws_client_config`.
4. An `oci_signing` component (`include/raft/oci_signing.hpp`) SHALL provide
   a pure function:
   ```cpp
   auto sign_request(const oci_client_config& cfg,
                      std::string_view method,
                      std::string_view request_target,   // e.g. "/20160918/instancePools/{id}"
                      std::string_view host,
                      const std::string& body)            // empty for GET/DELETE
       -> std::map<std::string, std::string>;             // headers to attach, incl. "authorization"
   ```
   that computes the `date`, `x-content-sha256` (for requests with a body),
   `content-length`/`content-type` (for requests with a body), and
   `authorization` headers per the OCI Request Signing Version 1 scheme
   confirmed in Requirement 1.1 above.
5. WHEN `cfg.use_instance_principal` is `false` AND any of `tenancy_id`,
   `user_id`, `fingerprint`, or `private_key_pem` is empty THEN
   `sign_request` SHALL throw `std::invalid_argument`.
6. WHEN `cfg.use_instance_principal` is `true` THEN signing SHALL instead use
   the short-lived certificate and private key obtained from the local
   instance metadata service (`http://169.254.169.254/opc/v2/identity/...`,
   confirmed by Task 0), refreshed automatically before its documented
   expiry. The exact refresh cadence and endpoint set SHALL be recorded in
   `spike-notes.md`.
7. An `oci_http_client` component (`include/raft/oci_http_client.hpp`) SHALL
   wrap `httplib::Client` (the same library `acme_certificate_provider`
   already depends on) and expose:
   ```cpp
   auto request(std::string_view method, std::string_view path,
                std::string body, std::string_view content_type)
       -> boost::json::value;     // parsed response body; throws on non-2xx
   ```
   applying `oci_signing::sign_request`'s headers to every call, and
   deriving `host`/base-URL from `{service}.{cfg.region}.oraclecloud.com`
   (or `cfg.endpoint_override` when non-empty) where `{service}` is supplied
   per-call (`"iaas"` for Compute/VCN, `"certificatesmanagement"`, etc.).
8. `oci_http_client` SHALL surface non-2xx responses as `std::runtime_error`
   carrying the OCI error response's `code` and `message` JSON fields
   (OCI error bodies are `{"code": "...", "message": "..."}`), mirroring how
   `aws_ec2_quorum_manager` wraps `AWSError::GetMessage()`.
9. `oci_http_client` SHALL retry exactly once on HTTP 429 (`TooManyRequests`)
   after honoring a `retry-after` response header if present, or a 1-second
   default delay otherwise; a second 429 SHALL propagate as an error. This
   matches the AWS implementations' choice to rely on their SDK's built-in
   retry policy for throttling (Requirement 15 of `aws-quorum-manager`) —
   since there is no SDK here, this one bounded retry is the closest
   equivalent without building a general backoff framework.

---

### Requirement 2: `oci_instance_pool_quorum_manager` Configuration

**User Story:** As a library user deploying a multi-AD Kythira cluster on
OCI, I want a configuration struct that captures the Instance Pool's
identity and my node-tagging preferences so I can construct the manager with
a single designated initializer.

#### Acceptance Criteria

1. An `oci_instance_pool_quorum_manager_config` struct SHALL be defined in
   `include/raft/oci_instance_pool_quorum_manager.hpp` with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `oci` | `oci_client_config` | `{}` | Credentials/region/signing mode |
   | `compartment_id` | `std::string` | *(required)* | OCID scoping all lookups |
   | `cluster_name` | `std::string` | *(required)* | Scope for freeform-tag filters |
   | `instance_pool_id` | `std::string` | *(required)* | OCID of a pre-existing Instance Pool |
   | `node_port` | `std::uint16_t` | `7000` | Port the kythira process listens on |
   | `heartbeat_timeout` | `std::chrono::seconds` | `30s` | 0 = lifecycleState-only liveness |
   | `heartbeat_grace_period` | `std::chrono::seconds` | `120s` | Grace window for freshly-launched instances with no heartbeat tag yet |
   | `topology` | `desired_topology<std::string>` | `{}` | Target counts per AD |
   | `provision_timeout` | `std::chrono::seconds` | `300s` | Max wait for a new instance to reach `RUNNING` |
   | `poll_interval` | `std::chrono::milliseconds` | `5000ms` | Interval between polls during provision |
   | `extra_tags` | `std::map<std::string, std::string>` | `{}` | Additional freeform tags merged onto every managed instance |

   Unlike `aws_ec2_quorum_manager_config`, there is no `image_id`,
   `instance_type`, `subnet_by_group`, or `user_data_template` field: those
   are captured once in the pre-existing OCI **Instance Configuration** that
   the Instance Pool already references, mirroring why
   `aws_asg_quorum_manager_config` likewise omits them in favor of the ASG's
   own launch template. `oci_instance_pool_quorum_manager` never calls
   `CreateInstanceConfiguration` or `CreateInstancePool` — provisioning the
   pool itself is an out-of-band operator action, exactly as
   `aws_acm_pca_provider`'s design note says of provisioning the ACM Private
   CA itself.
2. `oci_instance_pool_quorum_manager` SHALL validate at construction that
   `compartment_id`, `cluster_name`, and `instance_pool_id` are non-empty,
   `node_port` is non-zero, and (only when `oci.use_instance_principal` is
   `false`) `oci.tenancy_id`/`oci.user_id`/`oci.fingerprint`/
   `oci.private_key_pem` are non-empty. Violations SHALL throw
   `std::invalid_argument`.
3. At construction, the manager SHALL call `GetInstancePool(instance_pool_id)`
   once to confirm the pool exists and to read its current
   `placementConfigurations` (AD list), building the AD → placement-config
   map used by `provision_node`. WHEN the pool is not found or the call
   fails THEN the constructor SHALL throw `std::runtime_error`.

---

### Requirement 3: `oci_instance_pool_quorum_manager` Class Interface

**User Story:** As a library user, I want `oci_instance_pool_quorum_manager`
to satisfy `quorum_manager<Q, NodeId, Address, std::string>` so I can wire it
into a Raft node the same way as any AWS quorum manager, with no glue code.

#### Acceptance Criteria

1. `oci_instance_pool_quorum_manager<NodeId, Address>` SHALL satisfy
   `quorum_manager<oci_instance_pool_quorum_manager<NodeId, Address>, NodeId,
   Address, std::string>` with `placement_group_id_type = std::string`
   (Availability Domain), verified by a `static_assert` in its header.
2. The class SHALL be move-constructible and move-assignable; copy is
   deleted (it owns an `oci_http_client`, itself non-copyable because it
   wraps `httplib::Client`).
3. The class SHALL provide `assess_quorum`, `provision_node`,
   `decommission_node`, `topology`, and `maintain_quorum` with the same
   signatures as `aws_asg_quorum_manager` (substituting `std::string` OCID
   handling for EC2 instance IDs throughout).
4. The class SHALL be defined header-only, always compiled (no
   `#ifdef KYTHIRA_HAS_*_SDK` guard is needed because there is no vendor SDK
   to detect — see Requirement 11), but MAY be conditionally excluded from
   the default build via the `KYTHIRA_OCI_QUORUM_MANAGER` kconfig flag
   (Requirement 11.1) for consumers who want the smallest possible header
   surface.

---

### Requirement 4: Node Tagging Scheme

**User Story:** As an operator, I want every managed instance to carry
well-known freeform tags so I can audit cluster membership and so the
quorum manager can reconstruct its NodeId ↔ instance mapping after a
process restart, exactly as the AWS managers already do via EC2 tags.

#### Acceptance Criteria

1. Every instance discovered as newly launched by `provision_node` SHALL
   have these freeform tags applied via `UpdateInstance`:

   | Tag key | Value |
   |---|---|
   | `kythira-cluster` | `{cluster_name}` |
   | `kythira-node-id` | decimal string of the assigned `NodeId` |
   | `kythira-group` | `{availability_domain}` |
   | `kythira-managed-by` | `kythira-oci-instance-pool-quorum-manager` |

   (OCI freeform tag *keys* disallow the colon character `raft`/AWS tags use
   (`kythira:cluster`); this spec uses a hyphen (`kythira-cluster`) instead —
   confirm the exact allowed character set during Task 0's spike and adjust
   if the assumption above is wrong.)
2. Because `UpdateInstance` **replaces** the freeform tag map rather than
   merging into it, every tag-writing call SHALL first read the instance's
   current `freeformTags` via `GetInstance`, merge in the new/changed keys
   client-side, and submit the full merged map. This is the OCI-specific
   analogue of AWS `CreateTags`' native merge semantics — a correctness
   property this spec calls out explicitly (see `design.md` Property 1)
   because getting it wrong silently clobbers unrelated operator-set tags.
3. `config.extra_tags` SHALL be merged in at the same point; they SHALL NOT
   override the four tags above.
4. The `kythira-last-heartbeat` tag (Unix timestamp, decimal string) SHALL be
   written and updated exclusively by the kythira node process itself (via
   its own OCI API calls, using Instance Principal auth from inside the
   instance) — not by `oci_instance_pool_quorum_manager`, exactly mirroring
   Requirement 5/Property 5 of `aws-quorum-manager`'s design.

---

### Requirement 5: `assess_quorum`

**User Story:** As an orchestrator, I need `assess_quorum` to report which
nodes are live at the OCI infrastructure layer, combined with the
application-level heartbeat signal, so that stopped, terminating, or
crashed-but-still-running instances are all detected.

#### Acceptance Criteria

1. `assess_quorum` SHALL accept the caller-supplied `cluster` vector. When
   empty, it SHALL return a healthy result immediately without any OCI API
   call.
2. `assess_quorum` SHALL call `ListInstancePoolInstances(instance_pool_id)`
   to enumerate current pool membership, then `GetInstance` for each
   returned instance OCID to read `lifecycleState` and `freeformTags`
   (batched as tightly as the OCI API allows; if OCI offers no batch-get
   equivalent to `DescribeInstances`' multi-ID form — confirm in Task 0 —
   the calls SHALL be issued concurrently, bounded by a small worker pool,
   rather than serially, to keep latency comparable to AWS's single batched
   call).
3. A node is **live** when its `lifecycleState == RUNNING` AND the same
   heartbeat rule `aws_ec2_quorum_manager::assess_quorum` uses applies:
   `heartbeat_timeout == 0` → live unconditionally (legacy mode); else if
   `kythira-last-heartbeat` is present and fresh (`now - last_heartbeat <=
   heartbeat_timeout`) → live; else if no heartbeat tag yet and the instance
   is within `heartbeat_grace_period` of its `timeCreated` → live (starting
   up); else → unreachable.
4. A node is **unreachable** when its OCID is absent from the pool's current
   instance list, OR `lifecycleState` is any value other than `RUNNING`, OR
   the heartbeat rule above classifies it stale.
5. `quorum_status` and per-group (per-AD) health SHALL be computed using the
   same four-level mapping and per-group breakdown logic as
   `aws_ec2_quorum_manager::assess_quorum` (Requirements 6.5–6.6 of
   `aws-quorum-manager`), applied identically here.
6. WHEN any OCI API call in this sequence fails THEN `assess_quorum` SHALL
   return an exceptional Future.
7. `assess_quorum` SHALL NOT modify any OCI resource.
8. `assess_quorum` SHALL check the fault injection point
   `"raft/oci/instance_pool/list_instances"` before calling
   `ListInstancePoolInstances`.

---

### Requirement 6: `provision_node`

**User Story:** As an orchestrator that has detected a degraded AD, I need
`provision_node` to grow the Instance Pool by one, wait for the new instance
to become live, tag it, and return its address so it can join the cluster.

#### Acceptance Criteria

1. `provision_node(target_group, replacing)` SHALL validate that
   `target_group` is one of the Instance Pool's configured Availability
   Domains (read at construction per Requirement 2.3). WHEN it is not, the
   Future SHALL be rejected with `std::invalid_argument`.
2. `provision_node` SHALL call `GetInstancePool` to read the current `size`,
   then `UpdateInstancePool(instance_pool_id, size = current + 1)`.

   **Confirmed by Task 0's spike** (`spike-notes.md`, Finding 2, sourced
   from `oracle/terraform-provider-oci`'s `oci_core_instance_pool` resource
   documentation): unlike an AWS ASG's per-AZ subnet mapping, a single OCI
   Instance Pool's `size` is one pool-wide integer — OCI's own placement
   algorithm distributes new capacity across the pool's configured
   placement configurations, with no per-AD instance-count argument
   anywhere in the API. The implementation SHALL therefore require **one
   Instance Pool per AD** (the caller configures `topology` with one
   `oci_instance_pool_quorum_manager` instance per AD, analogous to how
   nothing stops an AWS deployment from using one ASG per AZ instead of one
   multi-AZ ASG) rather than guessing which AD a multi-AD pool's new
   instance landed in. This restriction SHALL be documented prominently in
   the class's header comment and in Requirement 15's example config.
3. `provision_node` SHALL then poll `ListInstancePoolInstances` at
   `config.poll_interval` intervals until an instance appears that lacks a
   `kythira-node-id` tag (the same "untagged instance = newly launched"
   heuristic `aws_asg_quorum_manager::provision_node` already uses), or
   `config.provision_timeout` elapses.
4. Once found, `provision_node` SHALL derive the new `NodeId` via the same
   max-of-parsed-tags-plus-one scan `aws_asg_quorum_manager::next_node_id()`
   uses (scanning all instances' `kythira-node-id` tags via
   `ListInstancePoolInstances`, not the pool being grown alone, so IDs stay
   unique across concurrent AD-scoped pools if Requirement 6.2's one-pool-
   per-AD restriction applies), and apply the tags from Requirement 4.1 via
   the read-merge-write pattern of Requirement 4.2.
5. `provision_node` SHALL read the new instance's private IP by calling
   `ListVnicAttachments(instance_id)` then `GetVnic(vnicId)`, reading
   `privateIp`, and return `peer_info{new_node_id, "{private_ip}:{node_port}"}`.
6. WHEN `UpdateInstancePool` fails THEN the Future SHALL be rejected with the
   OCI error; no cleanup is needed (no instance was created yet).
7. WHEN `provision_timeout` elapses before a new untagged instance appears
   THEN `provision_node` SHALL call `UpdateInstancePool(size = original)`
   (best-effort rollback, mirroring `aws_asg_quorum_manager`'s timeout
   handling since neither system supports targeting a single new instance
   for termination without first identifying it) and return an exceptional
   Future.
8. The `replacing` hint, when non-null, SHALL be logged for diagnostic
   purposes only, matching AWS's implementations.
9. `provision_node` SHALL check the fault injection point
   `"raft/oci/instance_pool/update_size"` before calling
   `UpdateInstancePool`.

---

### Requirement 7: `decommission_node`

**User Story:** As an orchestrator removing a broken node, I need
`decommission_node` to detach and terminate the instance from its pool so it
cannot rejoin and the account is not billed for a permanently broken node.

#### Acceptance Criteria

1. `decommission_node(node_id)` SHALL locate the instance's OCID by scanning
   `ListInstancePoolInstances` for a `kythira-node-id` tag matching
   `node_id` (there is no cheap reverse-computation the way EC2 instance
   IDs allow — see the Glossary's OCID entry — so, unlike
   `aws_ec2_quorum_manager::decommission_node`, this lookup always costs an
   API call; unlike `aws_asg_quorum_manager::decommission_node`, which has
   the identical cost, this is not a regression relative to the closer AWS
   analogue).
2. WHEN no instance with the matching tag is found THEN `decommission_node`
   SHALL return a successfully-resolved Future (idempotent — mirroring
   Requirement 8.2/14.2 of `aws-quorum-manager`).
3. WHEN found, `decommission_node` SHALL call
   `DetachInstancePoolInstance(instance_pool_id, instance_id,
   isDecrementSize=true, isAutoTerminate=true)`.
4. WHEN the found instance's `lifecycleState` is already `TERMINATING` or
   `TERMINATED` THEN `decommission_node` SHALL return a successfully-resolved
   Future without calling `DetachInstancePoolInstance` again.
5. WHEN `DetachInstancePoolInstance` fails for any reason other than
   "instance not found in this pool" THEN `decommission_node` SHALL return
   an exceptional Future with the OCI error message.
6. After a successful detach, `decommission_node` SHALL poll `GetInstance`
   until `lifecycleState` is no longer `RUNNING`, or 30 seconds elapse —
   mirroring the AWS managers' post-terminate consistency poll (Requirement
   8.4 of `aws-quorum-manager`) so a subsequent `assess_quorum` reliably
   observes the instance as unreachable.
7. `decommission_node` SHALL NOT remove the node from the Raft cluster
   configuration — that remains the `remove_server()`/`ClusterLeave` path's
   responsibility.
8. `decommission_node` SHALL check the fault injection point
   `"raft/oci/instance_pool/detach_instance"` before calling
   `DetachInstancePoolInstance`.

---

### Requirement 8: `topology`

**User Story:** As an orchestrator, I need `topology()` to return the
desired node count per AD synchronously, with no OCI API call, matching
every other `quorum_manager` implementation's contract.

#### Acceptance Criteria

1. `topology()` SHALL return `config.topology` unmodified.
2. `topology()` SHALL make no OCI API calls.

---

### Requirement 9: `maintain_quorum`

**User Story:** As a Raft leader, I need one call that assesses OCI quorum
health and automatically decommissions unhealthy nodes and provisions
replacements in the correct AD, mirroring `aws_ec2_quorum_manager`'s and
`aws_asg_quorum_manager`'s `maintain_quorum` behavior exactly (the concept
already requires it — `quorum_management.hpp`'s `quorum_manager` concept was
extended for this by `aws-quorum-manager`'s Requirement 19 and needs no
further concept change here).

#### Acceptance Criteria

1. `oci_instance_pool_quorum_manager::maintain_quorum(cluster)` SHALL:
   a. Check fault point `"raft/oci/instance_pool/maintain_quorum"`.
   b. Call `assess_quorum(cluster)` internally; propagate an exceptional
      Future immediately without any remediation.
   c. For each node in `quorum_health.unreachable_nodes`, call
      `decommission_node(node_id)`, awaited before proceeding; failures are
      logged (`std::cerr`) and do not abort remaining decommissions.
   d. Compute per-AD deficits from `config.topology` vs. live counts.
   e. For each AD with a positive deficit, call `provision_node(ad,
      replacing_hint)` the required number of times; failures are logged and
      do not abort remaining ADs.
   f. Return the pre-remediation `quorum_health` from step (b), exactly as
      the AWS managers do.
2. `maintain_quorum` SHALL honor `config.topology`'s per-AD targets and never
   provision a replacement into an AD other than the one with a deficit,
   matching AWS Requirement 19.3's topology-invariance property.
3. WHEN `assess_quorum` fails, `maintain_quorum` SHALL propagate the
   exception immediately with no decommission or provision calls attempted.

---

### Requirement 10: Fault Injection

**User Story:** As a developer writing chaos tests, I need fault injection
points in `oci_instance_pool_quorum_manager` so I can simulate OCI API
failures without talking to real OCI infrastructure.

#### Acceptance Criteria

1. `assess_quorum` SHALL check `"raft/oci/instance_pool/list_instances"`.
2. `provision_node` SHALL check `"raft/oci/instance_pool/update_size"`.
3. `decommission_node` SHALL check `"raft/oci/instance_pool/detach_instance"`.
4. `maintain_quorum` SHALL check `"raft/oci/instance_pool/maintain_quorum"`.
5. All fault points SHALL compile to no-ops when `FIU_ENABLE` is not
   defined, using the existing `fiu_do_on()` macro from
   `include/raft/fault_injection.hpp` — no new fault-injection mechanism is
   introduced.

---

### Requirement 11: Build Integration (No Vendor SDK to Detect)

**User Story:** As a developer building Kythira, I want OCI support available
by default without installing any new third-party SDK, since it depends only
on libraries the project already requires unconditionally.

#### Acceptance Criteria

1. Because `httplib`, `boost::json`, and OpenSSL are already mandatory,
   unconditional project dependencies (used today by
   `acme_certificate_provider` with no feature-detection gate), no
   `find_package`-based SDK detection is needed. Instead, both new headers
   SHALL be gated behind a single kconfig feature flag,
   `KYTHIRA_OCI_QUORUM_MANAGER` and `KYTHIRA_OCI_CERTIFICATES_PROVIDER`
   respectively (via the existing `kythira_kconfig_gate`/
   `kythira_kconfig_require` machinery documented in
   `.kiro/specs/kconfig-integration/`), each **defaulting to enabled**,
   consistent with `acme_certificate_provider` having no opt-out gate at
   all — the OCI components differ only in offering an explicit opt-out for
   consumers who want the smallest possible header surface, not in having a
   genuine missing-dependency reason to default off.
2. `CMakeLists.txt` SHALL NOT gain any new `find_package` call for this
   spec's components.
3. `DEPENDENCIES.md` SHALL gain a note under the existing
   `httplib`/OpenSSL/`boost::json` entries cross-referencing
   `oci_instance_pool_quorum_manager.hpp`/`oci_certificates_provider.hpp` as
   additional consumers, mirroring the existing note calling out
   `acme_certificate_provider.hpp` — not a new dependency section, since
   nothing new is required.

---

### Requirement 12: `oci_certificates_provider`

**User Story:** As a library user, I want a `certificate_provider`
implementation backed by OCI Certificates Management, matching the shape of
`aws_acm_pca_provider`, so I can issue TLS certificates for cluster nodes
running on OCI without operating a local `certificate_authority`.

#### Acceptance Criteria

1. **Confirmed by Task 0's spike** (`spike-notes.md`, Finding 3, sourced
   directly from `oracle/oci-go-sdk`'s
   `certificatesmanagement/create_certificate_managed_externally_issued_by_internal_ca_config_details.go`
   struct, cross-confirmed via the OCI CLI command reference for
   `create-certificate-managed-externally-issued-by-internal-ca`): OCI
   Certificates Management supports a third `certificateConfig.configType`,
   `MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA`, alongside the two this
   document originally knew about (`ISSUED_BY_INTERNAL_CA`, which does
   generate the key pair internally into Vault, and `IMPORTED`). This third
   config type takes `issuerCertificateAuthorityId` (the CA's OCID) and
   `csrPem` (the caller-supplied CSR, PEM-encoded) — both `mandatory:"true"`
   — and is the direct OCI analogue of AWS ACM Private CA's
   `IssueCertificate(CSR)`: the caller generates the key pair and CSR
   locally, submits only the CSR, and OCI's CA never sees or generates the
   private key. This settles the question an earlier draft of this
   requirement had left open (whether OCI's issuance model was compatible
   with `certificate_provider.hpp`'s "the CA never sees the key" invariant)
   in the affirmative — no deviation from that invariant, and no Vault-key-
   export fallback, is needed.
2. `sign_csr(csr_pem, options)` SHALL call `CreateCertificate` with
   `certificateConfig.configType = "MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA"`,
   `issuerCertificateAuthorityId = config.certificate_authority_id`, and
   `csrPem = csr_pem`, then poll `GetCertificate`/`GetCertificateBundle`
   until the certificate's `lifecycleState` is `ACTIVE` or
   `config.oci.api_timeout` elapses, and return `pem_material` with
   `private_key_pem` empty — matching `aws_acm_pca_provider::sign_csr`'s
   contract exactly, with no header-comment caveat needed (unlike the
   deviation an earlier draft of this requirement anticipated might be
   necessary). `options`' subject/SAN fields are informational for
   `CreateCertificate`'s `subject`/`certificateRules` request fields, not
   re-derived from the CSR itself, matching how
   `acme_certificate_provider::sign_csr` already treats `options`, not the
   CSR's own subject, as authoritative.
3. `oci_certificates_provider` SHALL NOT create or delete the OCI
   Certificate Authority resource itself — provisioning one (a billed,
   ongoing resource) remains an out-of-band operator action, exactly as
   `aws_acm_pca_provider` does not create or delete the ACM Private CA.
4. An `oci_certificates_provider_config` struct SHALL be defined with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `oci` | `oci_client_config` | `{}` | Credentials/region/signing mode |
   | `compartment_id` | `std::string` | *(required)* | OCID scoping the CA |
   | `certificate_authority_id` | `std::string` | *(required)* | OCID of a pre-existing OCI Certificate Authority resource |
   | `validity` | `std::chrono::seconds` | `30 days` | Certificate validity period |

5. `root_certificate_pem()` SHALL call
   `GetCertificateAuthorityBundle(certificate_authority_id)` and cache the
   result after the first successful call, matching
   `aws_acm_pca_provider::root_certificate_pem`'s caching behavior.
6. **Confirmed by Task 0's spike** (`spike-notes.md`, Finding 4):
   `revoke(certificate_serial)` SHALL call `RevokeCertificateVersion`
   (not `ScheduleCertificateDeletion`, this document's original guess —
   that operation schedules deletion of the certificate *resource*, a
   different operation from revoking one *version* of it) against the
   certificate version whose serial matches, mirroring
   `aws_acm_pca_provider::revoke`'s `RevokeCertificate` analogue.
7. Fault injection points `"raft/oci/certificates/create_certificate"`,
   `"raft/oci/certificates/get_bundle"`, and
   `"raft/oci/certificates/revoke"` SHALL be checked before the
   corresponding OCI calls.

---

### Requirement 13: Tests

**User Story:** As a developer, I need automated tests for both new OCI
components that run without a real OCI tenancy, plus an optional gated tier
against a real tenancy — mirroring the AWS managers' three-tier test
structure (unit / self-hosted-emulator / real-cloud), adapted for the fact
that **no official local OCI emulator equivalent to LocalStack exists**.

#### Acceptance Criteria

##### Concept satisfaction

1. `static_assert`s in both new headers SHALL verify concept satisfaction:
   `quorum_manager<oci_instance_pool_quorum_manager<std::uint64_t,
   std::string>, ...>` and `certificate_provider<oci_certificates_provider>`.

##### Unit tests (no network)

2. A unit test file `tests/oci_quorum_manager_unit_test.cpp` SHALL be
   registered as CTest target `oci-quorum-manager-unit-tests` with labels
   `unit;oci;quorum_manager`.
3. Unit tests SHALL cover: construction validation (empty
   `compartment_id`/`cluster_name`/`instance_pool_id` each throw
   `std::invalid_argument`; missing API-key fields with
   `use_instance_principal=false` throws), `oci_signing::sign_request`'s
   header set and canonical-string construction against fixed, hand-computed
   expected values (a golden-file style test, mirroring how JWS signing is
   tested in `acme_jws.hpp`'s own test suite), and each fault-injection point
   from Requirement 10.
4. A unit test SHALL verify the tag read-merge-write behavior of Requirement
   4.2: given a canned `GetInstance` response with a pre-existing unrelated
   freeform tag, the merged `UpdateInstance` request body SHALL retain that
   unrelated tag alongside the four kythira tags.

##### Local mock server ("no official OCI emulator" tier)

5. Because OCI has no publicly available, self-hostable emulator analogous
   to LocalStack, this spec introduces `tests/oci_mock_server.hpp`: a small
   `httplib::Server`-based mock implementing only the specific IaaS/
   ComputeManagement/Certificates Management REST endpoints this spec's two
   components call (`ListInstancePoolInstances`, `GetInstancePool`,
   `UpdateInstancePool`, `GetInstance`, `UpdateInstance`,
   `DetachInstancePoolInstance`, `ListVnicAttachments`, `GetVnic`,
   `CreateCertificate`, `GetCertificateBundle`,
   `GetCertificateAuthorityBundle`), with in-memory state transitions
   (`RUNNING` immediately, no simulated boot delay). This directly mirrors
   how `tests/acme_test_server.hpp` already fills the identical gap for
   ACME rather than depending on a vendor-run local CA, and satisfies this
   project's `doc/TODO.md` "Cloud Provider Support" convention of "a test
   that sends real data to a self-provisioned instance ... or a local
   emulator ... for a vendor API that has one" by substituting a
   purpose-built mock where the vendor genuinely has no self-hostable
   emulator, the same substitution `doc/TODO.md` already permits for the
   cloud-vendor *monitoring* entries with no local emulator.
6. Tests against `oci_mock_server` SHALL be registered as
   `oci-quorum-manager-mock-tests` (labels `integration;oci;mock`) and
   `oci-certificates-provider-mock-tests` (labels
   `integration;oci;mock;certificates`), both enabled by default (no
   opt-in macro needed — the mock server has no external dependency beyond
   `httplib`, which is already mandatory).
7. Mock-server test cases SHALL cover the equivalent scenarios AWS's
   LocalStack tier covers: provision N nodes and verify tags/sequential IDs,
   detect a stopped/terminated instance as degraded, decommission all nodes
   and verify idempotent double-decommission, and (for the certificates
   provider) issue a certificate and verify `root_certificate_pem` caching.

##### Real-OCI integration tests (gated)

8. Real-OCI integration tests SHALL be placed in
   `tests/oci_quorum_manager_real_test.cpp` and
   `tests/oci_certificates_provider_real_test.cpp`, guarded by
   `KYTHIRA_OCI_REAL_TESTS=1`, excluded from the default CTest run, and
   tagged `integration;oci;real`. This deliberately mirrors
   `KYTHIRA_AWS_REAL_EC2_TESTS`'s naming and default-off behavior.
9. The real-OCI fixture SHALL, as its first action, call a lightweight,
   read-only, always-permitted OCI call (e.g. `ListRegions` or the
   confirmed equivalent identity check — see Task 0) using the configured
   credentials; on failure for any reason (missing config, expired session,
   network error) the entire suite SHALL be skipped, not failed, mirroring
   the AWS fixture's `sts:GetCallerIdentity` pre-flight check.
10. Every resource the real-OCI fixture creates SHALL be tagged
    `kythira-test-run = {uuid}` and torn down unconditionally in the fixture
    destructor, with best-effort error collection printed to `std::cerr`,
    mirroring `RealEc2Fixture`'s teardown discipline exactly (Requirement
    16.10 of `aws-quorum-manager`). Costed resources (compute instances)
    are destroyed first.
11. The real-OCI fixture SHALL require pre-existing, env-var-supplied OCIDs
    for resources this spec's components explicitly do not create
    themselves (compartment, VCN/subnet, Instance Configuration, Instance
    Pool, Certificate Authority — see Requirement 2.1 and 12.4's "does not
    create" notes), analogous to how the AWS real-EC2 fixture accepts
    override env vars for pre-existing resources but differs in that these
    OCI resources have **no** fixture-side auto-creation fallback at all
    (unlike AWS's VPC/subnet auto-creation), since Requirement 2/12 scope
    both components to managing pre-existing pools/CAs only.

---

### Requirement 14: CI Wiring — Replace the `oci` Job Skeleton

**User Story:** As a maintainer, I want the existing no-op `oci` job in
`.github/workflows/real-cloud-tests.yml` (scaffolded by
`.kiro/specs/ci-real-cloud-tests/`, whose own design.md names this exact
future spec as the place to fill it in) replaced with real steps, following
the same toggle and credential-federation pattern already established for
the `aws` job.

#### Acceptance Criteria

1. The `oci` job SHALL gain the same resolution pattern as the `aws` job:
   `REAL_CLOUD_TESTS_OCI_ENABLED` (already scaffolded per
   `ci-real-cloud-tests/design.md`) gates the job; new
   `REAL_CLOUD_TESTS_OCI_INSTANCE_POOL_ENABLED` and
   `REAL_CLOUD_TESTS_OCI_CERTIFICATES_ENABLED` bundle-level variables gate
   each test binary independently, mirroring
   `REAL_CLOUD_TESTS_AWS_EC2_QUORUM_ENABLED`.
2. CI authentication SHALL use OCI's native workload-identity federation for
   GitHub Actions OIDC (OCI supports federating an external OIDC identity
   provider — including GitHub's — to a Dynamic Group without long-lived
   API keys stored as CI secrets), confirmed and documented precisely by
   Task 0's spike rather than assumed, mirroring the `aws` job's OIDC role
   assumption but adapted to OCI's Dynamic Group + Policy model (Glossary).
   If the spike finds no such mechanism currently available, the fallback
   SHALL be a long-lived API key stored as a CI secret with the same
   security caveat AWS's design explicitly avoided — a decision requiring
   explicit sign-off in `spike-notes.md`, not a silent regression from the
   AWS job's stronger posture.
3. A `scripts/ci-cloud-credentials/oci/` directory SHALL be added, mirroring
   `scripts/ci-cloud-credentials/aws/`'s shape: a provisioning script for
   CI's own federated identity, a `policies/` directory holding IAM Policy
   statement fragments per test bundle, and a `README.md` walkthrough.
4. Test cost estimation and reporting (mirroring Requirement 20 of
   `aws-quorum-manager`) SHALL be added to
   `tests/oci_quorum_manager_real_test.cpp`, using published OCI on-demand
   shape pricing for the fixture's chosen compute shape, with an equivalent
   `TestCostReport`/`CostAccumulator`/`CostSummaryFixture` structure.
5. Signal-driven test cleanup (mirroring Requirement 21 of
   `aws-quorum-manager`) SHALL be added identically: SIGTERM/SIGINT/SIGHUP/
   SIGQUIT/SIGPIPE handlers invoke the active fixture's `teardown()` before
   re-raising, with the same async-signal-safety caveat documented at the
   handler definition.

---

### Requirement 15: Documentation and Example Configuration

**User Story:** As an operator, I want a worked example configuration and
setup documentation for OCI support, since `doc/TODO.md`'s "Cloud Provider
Support" section requires this for every entry, including this one.

#### Acceptance Criteria

1. An example configuration file SHALL be added at
   `docker/oci_quorum_manager/oci_quorum_manager.env.example`, mirroring the
   existing `docker/ca_cluster_node/ca_cluster_node.env.example` /
   `docker/ca_service/ca_service.env.example` convention: one env var per
   `oci_instance_pool_quorum_manager_config`/`oci_client_config` field, with
   inline comments explaining each.
2. A `docker/oci_quorum_manager/README.md` SHALL document: how to create the
   prerequisite OCI resources this spec's components deliberately do not
   create themselves (compartment, VCN/subnet, Instance Configuration,
   Instance Pool, Certificate Authority), how to choose between API-key and
   Instance Principal auth, and a worked end-to-end example.
3. `doc/TODO.md`'s OCI bullet SHALL be updated from `[ ]` to `[x]` once
   `tasks.md`'s tasks are complete, following the same convention used for
   the AWS entry, with a one-line description of what shipped (mirroring
   the existing AWS bullet's parenthetical).
