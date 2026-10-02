# Requirements Document

## Introduction

Kythira identifies Raft nodes with a `NodeId` that the `node_id` concept
(`include/raft/types.hpp:34`) restricts to an unsigned integer or
`std::string`. Every shipped binary, transport and quorum manager defaults
to `std::uint64_t`. Cloud identifiers do not fit that shape:

| Identifier | Shape | Fits `uint64_t`? |
|---|---|---|
| AWS EC2 instance id | `i-` + 8 or 17 hex digits (68 bits) | Only when the first of 17 digits is `0` |
| Alibaba ECS instance id | `i-` + ~20 base-36 characters | No |
| Azure VM | resource group + name, `vmId` GUID (128 bits) | No |
| Azure VMSS (Flexible) member | scale set + VM name; instance id is unique only per scale set | No |
| GCP Compute instance | project + zone + name; numeric `id` | Name no; numeric id yes, but only per project |
| OCI instance | OCID (`ocid1.instance.oc1.<region>.<unique>`) | No |
| Docker container | name, or 64-hex id | No |
| X.509 serial from a cloud CA | up to 20 octets | No |

Today the code bridges the gap in three ways, and each has failed or can
fail:

1. **AWS derives the node id from the instance id.**
   `aws_ec2_quorum_manager::ec2_id_to_node_id`
   (`aws_ec2_quorum_manager.hpp:496-503`) runs `std::stoull(id.substr(2),
   nullptr, 16)` over 17 hex digits. Any id whose first digit is not `0`
   throws `std::out_of_range`, which fails `assess_quorum` for the whole
   cluster and `provision_node` after the instance is already running.
   LocalStack 4.x issues such ids for 15 of every 16 launches, which is why
   the LocalStack tier never ran green; PRs #433 and #442 patch LocalStack
   (`docker/aws-localstack/init/ready.d/aws-shaped-instance-ids.py`) rather
   than the manager. Real AWS ids have started with `0` so far, but AWS does
   not promise that. Legacy 8-digit ids parse but format back as
   `i-000000001234abcd`, so the round trip names the wrong instance. With a
   `std::string` NodeId the manager still goes through `uint64_t` (it
   stores the decimal form of the hex value), so choosing strings does not
   avoid the overflow. The ASG manager reuses both helpers
   (`aws_asg_quorum_manager.hpp:181,195,392,416`).
2. **Every other manager allocates a number and stores it in a tag.**
   Alibaba, OCI, Azure VM, Azure VMSS and Docker take the highest
   `kythira-node-id` tag plus one; GCP Compute and MIG draw a random 63-bit
   number. That works, but the parsing and allocation are re-implemented
   per manager with different bugs (see Requirement 7).
3. **The rest of the stack assumes `uint64_t`.** The `network_client`
   concepts (`network.hpp:16-17` and siblings), every transport's
   peer map, `proto/raft.proto`, the CA's peer identity, and every `cmd/`
   binary's `--node-id` parser are integer-only, and `raft.hpp:622-627`
   `node_id_to_u64` returns `0` for every string id, so log catch-up
   (`raft.hpp:6956`) silently targets node 0 when ids are strings.

This spec introduces a **composite node id**: a typed value made of the
provider, the scope in which the cloud's own id is unique, and that native
id, with one canonical text form. A cloud quorum manager uses its
provider's composite id as the node identity, so the identity is the cloud
resource itself and nothing has to be parsed into an integer. The spec
also carries text ids through the transports, serializers, CA and binaries
so a composite id can run end to end, and keeps integer ids working for
existing deployments.

### Verified against main

Checked on `origin/main` at `9bdabb0` (2026-10-02). The LocalStack
instance-id hook is not on main; it is in open PRs #433 and #442.

### Non-goals

- Converting an existing cluster's persisted integer ids to composite ids
  in place. `NodeId` is a compile-time type, so a cluster changes id kind
  by being rebuilt (restore from `raft_object_backup` into a new cluster).
  Requirement 11 keeps integer clusters working instead.
- Multi-Raft `GroupId` and shard ids. They are allocated by Kythira, never
  read from a cloud, and stay integers.
- Values that are legitimately numeric: GCS object generations, Retry-After
  seconds, token expiry, log indexes, terms, fencing tokens, the in-house
  CA's own serial counter.
- Cross-account or cross-provider clusters. The composite form leaves room
  for them (Requirement 1.6) but no manager is required to support them.

## Glossary

- **Native id**: the identifier the cloud itself assigns or accepts for one
  instance (`i-0abc...`, an OCID, a VM name).
- **Scope**: the namespace in which a native id is unique and addressable
  (AWS region, Azure resource group, GCP project and zone).
- **Composite node id**: a value of a type satisfying the
  `composite_node_id` concept: provider, scope and native id, with a
  canonical text form.
- **Canonical text**: `<provider>:<scope>:<native>`, the one string form
  of a composite id used on the wire, in tags, in certificates and on the
  command line.
- **Textual node id**: `std::string` or any composite node id. Everything
  that serializes, logs or tags a textual id uses its canonical text.
- **Numeric node id**: an unsigned integer `NodeId`, as today.
- **Allocated numeric id**: a numeric id a manager chooses and stores in
  the `kythira-node-id` / `kythira:node-id` tag or label.

## Requirements

### Requirement 1: The composite node id concept

**User Story:** As a library user, I want a node id type that carries the
cloud's own identifier without squeezing it into an integer, so that the
node's identity is the cloud resource itself.

#### Acceptance Criteria

1. The library SHALL define a concept `composite_node_id<T>` in a new
   header `include/raft/composite_node_id.hpp` requiring: `std::regular`,
   `std::totally_ordered`, a `std::hash` specialisation, a static
   `provider` string, `scope()` and `native()` accessors returning
   `std::string_view`, `to_string()` returning the canonical text, and a
   static `parse(std::string_view) -> std::optional<T>`.
2. The `node_id` concept SHALL accept `composite_node_id` types in addition
   to unsigned integers and `std::string`.
3. FOR every composite id `x`, `T::parse(x.to_string())` SHALL equal `x`,
   and FOR every string `s` for which `T::parse(s)` succeeds,
   `T::parse(s)->to_string()` SHALL equal `s` (one canonical spelling).
4. `parse` SHALL reject text whose provider does not match, whose scope or
   native id fails the provider's validation, or that has extra or missing
   segments. It SHALL NOT throw.
5. Ordering and equality SHALL compare provider, scope and native id
   lexicographically on canonical text, so the order is the same in every
   process and in sorted on-disk structures.
6. A segment SHALL percent-encode `:` and `%`, so a GCP domain-scoped
   project (`example.com:proj`) or any future native id containing `:`
   round-trips.
7. Where a provider treats a component case-insensitively (Azure resource
   group and VM names), the type SHALL store and print it in lower case so
   two spellings of the same resource compare equal.

### Requirement 2: Provider composite id types

**User Story:** As an operator running on a given cloud, I want the node id
to be that cloud's instance identifier, validated for that cloud, so that a
malformed or foreign id is refused rather than misread.

#### Acceptance Criteria

1. The library SHALL provide these types, each satisfying
   `composite_node_id`:

   | Type | Provider | Scope | Native id and validation |
   |---|---|---|---|
   | `aws_ec2_node_id` | `aws-ec2` | region | `i-` + exactly 8 or 17 lower-case hex digits |
   | `alibaba_ecs_node_id` | `alibaba-ecs` | region id | `i-` + 1-64 of `[a-z0-9]` |
   | `azure_vm_node_id` | `azure-vm` | resource group | VM name, 1-64 of `[a-z0-9._-]` |
   | `gcp_instance_node_id` | `gcp` | `<project>/<zone>` | instance name, RFC 1035 label, 1-63 |
   | `oci_instance_node_id` | `oci` | region parsed from the OCID | `ocid1.instance.` OCID |
   | `docker_container_node_id` | `docker` | compose project or cluster name | container name |

2. Azure VMSS Flexible members SHALL use `azure_vm_node_id`: a Flexible
   member is an ordinary VM in the scale set's resource group, and the VM
   name is unique there, unlike the scale-set instance id.
3. GCP MIG members SHALL use `gcp_instance_node_id`; the MIG chooses the
   name, and project and zone make it unique.
4. Each type SHALL offer a constructor from its parts that validates them
   and throws `std::invalid_argument` on failure, for code that builds an
   id from a cloud API response.
5. The AWS type SHALL accept a legacy 8-digit id and SHALL print it back
   exactly as given, never zero-padded.

### Requirement 3: One set of node id helpers

**User Story:** As a maintainer, I want one place that knows how to print,
parse and compare every kind of node id, so that each manager and
serializer stops carrying its own `if constexpr (std::is_same_v<NodeId,
std::string>)` branch.

#### Acceptance Criteria

1. The library SHALL provide `node_id_traits<NodeId>` with `to_text`,
   `from_text` (returning `std::optional`), and `is_textual`, defined for
   numeric, `std::string` and composite ids.
2. `from_text` for a numeric id SHALL accept only a non-empty run of ASCII
   digits whose value fits `NodeId`, using `std::from_chars`. It SHALL
   reject signs, whitespace, `0x`, trailing characters and out-of-range
   values without throwing.
3. The library SHALL provide `next_numeric_node_id(max_seen)` that returns
   `max_seen + 1` or fails with `std::overflow_error` at the type's
   maximum, never wrapping to zero.
4. Every `node_id_str` copy in the quorum managers, `aws_ec2_peer_discovery
   ::from_string`, and the serializers' string-vs-integer branches SHALL
   be replaced by these helpers.

### Requirement 4: AWS managers use the instance id as the node id

**User Story:** As an operator on AWS, I want a node's id to be its EC2
instance, so that no instance id, from AWS or LocalStack, can fail to fit.

#### Acceptance Criteria

1. WHEN `aws_ec2_quorum_manager` or `aws_asg_quorum_manager` is
   instantiated with `NodeId = aws_ec2_node_id` THEN the node id SHALL be
   `{configured region, instance id}` and SHALL be mapped back to the
   instance by reading those fields, with no arithmetic.
2. WHEN either is instantiated with `NodeId = std::string` THEN the node id
   SHALL be the canonical text of the `aws_ec2_node_id`. This changes the
   string mode, which today holds the decimal value of the hex digits.
3. The managers SHALL never call `std::stoull` (or any integer parse) on an
   instance id. `ec2_id_to_node_id` and `node_id_to_ec2_id` SHALL be
   removed or replaced by the composite type's constructor and accessors.
4. IN composite and string mode the managers SHALL write the canonical
   text to `kythira:node-id` and SHALL reject, in `assess_quorum`, an
   instance whose region does not match the configured region, reporting
   that node unreachable rather than failing the whole assessment.
5. The default `NodeId` of both managers SHALL become `aws_ec2_node_id`.
   This is a breaking change to the template default and SHALL be called
   out as one in the commit and changelog.
6. Numeric mode SHALL remain available under Requirement 11.

### Requirement 5: Other cloud managers use their composite ids

**User Story:** As an operator on Alibaba, Azure, GCP, OCI or Docker, I
want the same guarantee as on AWS, so that no manager allocates, parses or
races over a number it then has to map back to an instance.

#### Acceptance Criteria

1. Each of `alibaba_ess_quorum_manager`, `azure_vm_quorum_manager`,
   `azure_vmss_quorum_manager`, `gcp_compute_quorum_manager`,
   `gcp_mig_quorum_manager`, `oci_instance_pool_quorum_manager` and
   `docker_quorum_manager` SHALL accept its provider's composite id
   (Requirement 2.1) as `NodeId`, and `std::string` holding that id's
   canonical text.
2. IN composite or string mode the group managers (ESS, VMSS, MIG, OCI
   pool) SHALL take the node id from the adopted instance's own native id
   and SHALL NOT allocate one. The `next_node_id` scans, their races
   (`alibaba_ess_quorum_manager.hpp:796-798`,
   `azure_vm_quorum_manager.hpp:683-686`) and id reuse after decommission
   no longer apply in this mode.
3. IN composite or string mode the name-chosen managers (Azure VM, GCP
   Compute, Docker) SHALL generate the resource name before creating it,
   with a random suffix that keeps the name within the provider's limit
   (GCP 63 characters, which `gcp_compute_quorum_manager` does not check
   today), and the node id SHALL be that name in its scope.
4. Mapping a node id back to an instance SHALL use the native id directly
   (OCID, VM name, instance name, `i-` id), not a tag search. The tag or
   label SHALL still be written, holding canonical text, for discovery and
   the leak audit.
5. Each manager's default `NodeId` SHALL become its composite type, with
   the same breaking-change note as Requirement 4.5.

### Requirement 6: Nodes can learn their own id

**User Story:** As an operator, I want a node launched by a quorum manager
to know its own id without the manager passing it in, so that
`{NODE_ID}` in user-data stops being "a known limitation"
(`aws_ec2_quorum_manager.hpp:308-318`).

#### Acceptance Criteria

1. The library SHALL provide `self_node_id<NodeId>(source)` that builds the
   composite id from the instance metadata service: AWS IMDSv2
   (`instance-id`, `placement/region`), Alibaba metadata
   (`instance-id`, `region-id`), Azure IMDS (`compute.name`,
   `compute.resourceGroupName`), GCP metadata (`instance/name`,
   `instance/zone`, `project/project-id`), OCI IMDS v2 (`instance/id`).
2. Every binary that takes `--node-id` (Requirement 10) SHALL accept
   `--node-id=metadata:<provider>` and resolve it through this function at
   start-up, failing fast with the metadata error if it cannot.
3. A metadata lookup SHALL use a bounded timeout and SHALL NOT be attempted
   unless asked for, so a binary off-cloud never stalls on a link-local
   address.
4. Managers that know the name before launch (Azure VM, GCP Compute,
   Docker) SHALL substitute `{NODE_ID}` with canonical text. Managers that
   do not (EC2, ASG, ESS, VMSS, MIG, OCI pool) SHALL document
   `--node-id=metadata:<provider>` as the replacement for `{NODE_ID}`.

### Requirement 7: Numeric ids are parsed and allocated safely

**User Story:** As an operator of an existing integer-id cluster, I want
the numeric paths fixed too, so that a stray tag or a large id cannot
crash a scan, wrap to zero or collide with another cluster.

#### Acceptance Criteria

1. Every numeric tag, label, environment variable and command-line parse
   of a node id SHALL use `node_id_traits::from_text` (Requirement 3.2).
   This covers `oci_instance_pool_quorum_manager.hpp:859-876`,
   `azure_vm_quorum_manager.hpp:624-649,727-733`,
   `azure_vmss_quorum_manager.hpp:691-714`,
   `docker_quorum_manager.hpp:450-456,496-532`,
   `aws_ec2_peer_discovery.hpp:370-376`, and the `cmd/` parsers listed in
   Requirement 10.
2. Every numeric allocation SHALL use `next_numeric_node_id` (Requirement
   3.3). Today OCI, Azure VM, Azure VMSS and Docker wrap silently at the
   maximum.
3. An unparseable tag or label on one instance SHALL be skipped with a log
   line, never fail the whole scan. `aws_ec2_peer_discovery` and
   `docker_quorum_manager::find_by_idempotency_key` fail today.
4. Allocation scans and reverse lookups SHALL filter on the cluster tag or
   label. The OCI pool scan (`:859-876`), the VMSS scan and `assess_quorum`
   (`:254-268`, `:691-714`), and the MIG lookup and collision check
   (`:539-576`) do not today.
5. `aws_ec2_peer_discovery` SHALL de-duplicate by parsed id, not raw tag
   text, so `01` and `1` cannot both be returned.
6. Narrowing SHALL be refused: a GCP random id or AWS legacy value that
   does not fit a narrower `NodeId` SHALL fail allocation, not truncate.
7. `oci_instance_pool_quorum_manager` SHALL compile with a `std::string`
   `NodeId`; today `static_cast<NodeId>(highest + 1)` cannot.

### Requirement 8: Raft core and messages carry any node id

**User Story:** As a library user, I want a `kythira::node` with textual
ids to replicate correctly, so that catch-up and responses are addressed
to the right peer.

#### Acceptance Criteria

1. `node_id_to_u64` (`raft.hpp:622-627`) SHALL be removed. Every RPC SHALL
   be addressed by `node_id_type`.
2. `fetch_log_entries_response::_responder_id` (`types.hpp:425,432`) SHALL
   have type `NodeId`.
3. `proto/raft_messages.proto` SHALL add `NodeIdValue responder = 2` to the
   fetch response; encoders SHALL write it, and decoders SHALL prefer it
   and fall back to `responder_id = 1` for numeric ids from older peers.
4. The JSON serializer's numeric branch SHALL decode with
   `to_number<NodeId>()` instead of `as_int64()`, so ids at or above 2^63
   decode. The same applies to snapshot node lists in
   `object_store_persistence.hpp:1583-1586` and to
   `object_store_backup.hpp:693-707`, which today casts to `int64_t`.
5. The CBOR, ION, JSON and protobuf serializers SHALL encode a composite id
   as its canonical text and decode it with `parse`, failing the decode on
   a `nullopt`.
6. A property test SHALL round-trip every message type through every
   serializer with each composite type as `NodeId`.

### Requirement 9: Transports route by any node id

**User Story:** As a library user, I want every shipped transport to accept
the node's own id type, so that a composite-id node can be built at all.

#### Acceptance Criteria

1. The `network_client`, `network_client_with_pre_vote` and
   `network_client_with_log_fetch` concepts SHALL take the target's type
   as a parameter defaulting to `std::uint64_t`, and `raft.hpp` SHALL check
   its client against `node_id_type`.
2. The TCP, TLS-TCP, cpp-httplib, Proxygen, Beast, gRPC, CoAP and simulator
   transports SHALL key their peer maps and `send_*` targets by a `NodeId`
   template parameter defaulting to `std::uint64_t`.
3. `tcp_rpc`'s address dispatch (`tcp_rpc.hpp:177-180,403-404`) SHALL look
   the address up in the peer registry first and SHALL NOT treat an
   all-digit string as a node id when the registry's `NodeId` is textual.
   A digit string longer than `uint64_t` SHALL be an error result, not an
   uncaught `std::out_of_range`.
4. `tls_tcp_rpc`'s `peer_node_ids` (`tls_tcp_rpc.hpp:122`) SHALL map
   certificate names to `NodeId`.
5. `proto/raft.proto` SHALL add a `NodeIdValue`-equivalent field next to
   each `uint64` node id field (`candidate_id`, `leader_id`, `node_id`,
   `requester_id`, `responder_id`). Senders SHALL fill the new field;
   receivers SHALL prefer it and fall back to the `uint64` field, so mixed
   numeric clusters keep interoperating during a rolling upgrade.
6. `coap_multicast_peer_discovery` (string ids, `coap_transport.hpp:747`)
   SHALL be usable with a `coap_client` instantiated for `std::string`.

### Requirement 10: Binaries accept textual ids

**User Story:** As an operator, I want `--node-id` and peer lists to accept
the cloud id, so that I can run the shipped binaries on a composite-id
cluster.

#### Acceptance Criteria

1. A Kconfig choice `KYTHIRA_NODE_ID_KIND` (`numeric` or `text`) SHALL pick
   the host binaries' `node_id_type`: `std::uint64_t` or `std::string`
   holding canonical text. `numeric` SHALL stay the default until
   Requirements 8, 9 and 12 have landed; flipping it is a separate,
   breaking change.
2. IN `text` mode `--node-id`, `KYTHIRA_NODE_ID`, `NODE_ID` and every peer
   list (`--peers`, `--peer id=url`, `KYTHIRA_PEERS`, `PEERS`) SHALL accept
   canonical text, a plain numeric string, or `metadata:<provider>`
   (Requirement 6) for the local id.
3. This SHALL cover `cmd/chaos_node/config.hpp:94-133`,
   `cmd/ca_cluster_node/config.hpp:133,196`,
   `cmd/multi_raft_node/config.cpp:16-35,313-314`,
   `cmd/redis_gateway_node/config.cpp:46-57,125,141`,
   `cmd/multi_raft_bench/main.cpp:144,166`, and the `host_stacks.hpp`,
   `run_host.hpp` and `capacity_plane.hpp` type aliases.
4. `multi_raft_node`'s control server (`control_server.hpp:93-210`) SHALL
   print a textual id as a JSON string.
5. `multi_raft_node`'s `aws_ec2_peer_discovery` instantiation
   (`main.cpp:159`) SHALL follow `KYTHIRA_NODE_ID_KIND`.

### Requirement 11: Integer-id clusters keep working

**User Story:** As an operator of an existing cluster, I want my numeric
ids to keep working on AWS after this change, so that I am not forced to
rebuild the cluster.

#### Acceptance Criteria

1. The AWS managers SHALL keep a numeric mode, selected by an unsigned
   integral `NodeId`.
2. IN numeric mode the AWS managers SHALL allocate the id with
   `next_numeric_node_id` over the cluster's `kythira:node-id` tags, and
   map it back with `DescribeInstances` filtered on `tag:kythira:cluster`
   and `tag:kythira:node-id`, the reverse lookup every other manager
   already uses.
3. IN numeric mode the id SHALL be known before `RunInstances`, so
   `kythira:node-id`, `Name` and `{NODE_ID}` SHALL go in the launch request
   (`launch_tag_specification`), closing the window
   `aws_ec2_quorum_manager.hpp:362-372` documents.
4. An instance launched before this change carries a `kythira:node-id` tag
   holding the decimal value its old id was derived from (written by
   `apply_identity_tags`), so it SHALL be found by the same tag lookup.
   WHERE that best-effort tag is missing and the instance id's 17 hex
   digits fit `uint64_t`, the manager SHALL fall back to the old
   derivation for that instance and log that it did; otherwise it SHALL
   report the instance as foreign and skip it.
5. Numeric-mode ids SHALL still be unique per cluster when two managers
   provision at once to the same extent as the other max-plus-one
   managers; the race SHALL be documented, not fixed, here.

### Requirement 12: Certificates and the CA carry textual ids and wide serials

**User Story:** As an operator using the CA cluster, I want peer
certificates and revocation to work for composite ids and cloud-issued
serials, so that mTLS does not force ids or serials back into integers.

#### Acceptance Criteria

1. `peer_identity_dns_name`, `peer_enrollment_mac` and the
   `rpc_tls_ready` command (`ca_http_helpers.hpp:147,219,272`,
   `ca_state_machine.hpp:436-440`, `cmd/ca_cluster_node/main.cpp:1210-1221`)
   SHALL take the node id as `NodeId`.
2. A numeric id SHALL keep its DNS SAN `ca-cluster-node-<n>`. A textual id
   SHALL be carried in a URI SAN `urn:kythira:node:<canonical text>`,
   percent-encoded per RFC 3986, because canonical text is not a valid DNS
   name. Verification SHALL accept exactly one of the two for a given
   peer.
3. The enrollment MAC SHALL be computed over the canonical text bytes for
   textual ids and over the existing encoding for numeric ids, so existing
   enrollments stay valid.
4. The `rpc_tls_ready` command SHALL gain a version byte so a textual id is
   never decoded as a number by an older replica; an older replica SHALL
   reject the new version rather than misread it.
5. Certificate serials SHALL be represented by a `certificate_serial` value
   type holding up to 20 octets, printed as lower-case hex.
   `certificate_authority.hpp:73,133`, `oci_certificates_provider.hpp:500`
   (which returns 0 for wide serials today), the ACM PCA and GCP CAS
   providers (which leave the serial at 0), and the revoke endpoints in
   `cmd/ca_cluster_node/main.cpp:1416-1418` and `cmd/ca_service/main.cpp:757`
   SHALL use it. The in-house CA's counter SHALL still issue serials that
   fit 64 bits.

### Requirement 13: LocalStack runs the AWS managers unpatched

**User Story:** As a maintainer, I want the LocalStack tier to pass against
stock LocalStack, so that the instance-id hook from PRs #433 and #442 can
be deleted.

#### Acceptance Criteria

1. A unit test SHALL drive both AWS managers, in composite, string and
   numeric mode, with instance ids `i-f0123456789abcdef` (non-zero first
   digit), `i-1234abcd` (legacy) and `i-0123456789abcdef0`.
2. `ca_cluster_node_localstack_test` and the EC2 cases of
   `aws_quorum_manager_localstack_test` SHALL pass against stock
   `localstack/localstack:4.x` with no init hook.
3. WHEN criteria 1 and 2 pass THEN
   `docker/aws-localstack/init/ready.d/aws-shaped-instance-ids.py` and its
   compose mount SHALL be deleted from whichever branch carries them, and
   the project memory note on the overflow SHALL be updated.

### Requirement 14: Tests

**User Story:** As a maintainer, I want the id handling covered offline, so
that a regression shows up without a cloud account.

#### Acceptance Criteria

1. A unit test SHALL cover each composite type: valid and invalid native
   ids, round-trip, case folding, percent-encoding, ordering and hashing.
2. A unit test SHALL cover `node_id_traits` and `next_numeric_node_id`,
   including signs, whitespace, `0x`, trailing garbage, the type's maximum
   and a narrower `NodeId`.
3. Each manager's existing mock or unit suite SHALL gain a composite-mode
   case for provision, assess and decommission.
4. A multi-node `kythira::node` test SHALL run with a composite `NodeId`
   over the simulator network, including a log catch-up through
   `fetch_log_entries`, which would have caught `node_id_to_u64`.
5. A transport test per shipped transport SHALL send each RPC to a
   `std::string` target.
6. All new tests SHALL be registered outside every cloud gate in
   `tests/CMakeLists.txt` unless they need a cloud SDK.
