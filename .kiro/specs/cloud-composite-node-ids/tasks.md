# Implementation Plan

## Overview

Introduce composite node ids, make the cloud quorum managers use their
provider's composite id as the node identity, and carry textual ids
through the core, transports, CA and binaries. See `requirements.md` for
the audit and `design.md` for the types and per-component changes.

## Tasks

- [x] 1. Composite id types and helpers
  - [x] 1.1 Add `include/raft/composite_node_id.hpp`: the
    `composite_node_id`, `textual_node_id` and `node_id` concepts, and
    `basic_composite_node_id` with percent-encoding and case folding
  - [x] 1.2 Add the six provider rule sets and type aliases
  - [x] 1.3 Add `node_id_traits` and `next_numeric_node_id`, moving the
    Alibaba strict parser into it
  - [x] 1.4 Add `tests/composite_node_id_test.cpp` and
    `tests/node_id_traits_test.cpp`; register outside the cloud gates
  - _Requirements: 1.1-1.7, 2.1-2.5, 3.1-3.3, 14.1, 14.2_

- [x] 2. Numeric hardening (independent of composite mode)
  - [x] 2.1 Replace the numeric parsers and allocators in the OCI, Azure
    VM, Azure VMSS and Docker managers and `aws_ec2_peer_discovery` with
    the task 1.3 helpers; replace every `node_id_str` copy (each manager's
    `node_id_str` is now a one-line call to `node_id_traits::to_text`;
    parsing and allocation go through `parse_numeric_node_id`,
    `numeric_node_id_as` and `numeric_node_id_ceiling`, added next to
    `node_id_traits`, which Alibaba ESS and GCP now use too)
  - [x] 2.2 Skip unparseable tags with a log line; de-duplicate discovery
    by parsed id
  - [x] 2.3 Add cluster filters to the OCI pool scan, the VMSS scan and
    `assess_quorum`, and the MIG lookup and collision check (the MIG
    collision check stays deliberately unscoped: an id another cluster in
    the zone holds only costs a redraw)
  - [x] 2.4 Refuse narrowing in GCP id generation; make the OCI manager
    compile with `std::string` (GCP now draws inside the narrower type's
    range rather than failing; `numeric_node_id_as` refuses every other
    narrowing with `std::overflow_error`)
  - [x] 2.5 Unit and mock tests for each fix
  - _Requirements: 3.4, 7.1-7.7_

- [x] 3. AWS managers
  - [x] 3.1 Replace `ec2_id_to_node_id`/`node_id_to_ec2_id` with
    `to_instance_id`/`from_instance`; composite and string mode use
    `aws_ec2_node_id` (landed as `instance_id_of`/`node_id_of_instance`
    on both managers, over the static `instance_id_for_node`/
    `node_id_for_instance` and numeric tag-lookup helpers)
  - [x] 3.2 Numeric mode: tag-based allocation and lookup, identity tags
    and `{NODE_ID}` in the launch request, and the R11.4 fallback
  - [x] 3.3 ASG manager: route through the new helpers in both modes
  - [x] 3.4 Change both defaults to `aws_ec2_node_id` (breaking) (done
    October 16, 2026; every in-tree `<>` that relied on numeric ids now
    names `<std::uint64_t>`, and the mock suite's composite cases run
    through the default)
  - [x] 3.5 Unit tests with `i-f0123456789abcdef`, `i-1234abcd` and
    `i-0123456789abcdef0` in all three modes
    (`tests/aws_ec2_node_id_mock_test.cpp`, a local EC2/Auto Scaling fake)
  - _Requirements: 4.1-4.6, 11.1-11.5, 13.1_

- [x] 4. Other managers in composite mode
  - [x] 4.1 Alibaba ESS and OCI pool: id from the adopted instance
  - [x] 4.2 Azure VMSS and GCP MIG: id from the member name and scope
  - [x] 4.3 Azure VM, GCP Compute and Docker: generated names within
    provider limits; `{NODE_ID}` substitution
  - [x] 4.4 Change each default `NodeId` (breaking)
  - [x] 4.5 Composite-mode case per manager in its mock or unit suite
  - Done October 16, 2026. As built: the mode switch is
    `node_id_traits<NodeId>::is_textual`, and numeric mode is unchanged.
    Every manager still checks the cluster tag or label before counting or
    removing a member, so an id cannot reach another cluster's instance in a
    shared group. Out-of-scope or unparseable ids read unreachable in
    `assess_quorum` and are refused by `decommission_node`
    (`std::invalid_argument`). Generated names come from
    `include/raft/generated_node_name.hpp`: the cluster part is cut to fit,
    never the suffix; collisions get a fresh suffix. Deviations: the OCI
    pool's scope is the region key in the pool's OCID, not `oci.region`
    (the two spell the region differently); GCP labels cannot hold `:` or
    `/`, so both GCP managers' `kythira-node-id` label holds the instance
    name, and Compute also writes the full text to the instance metadata
    item `kythira-node-id`; Docker gains `{NODE_ID}` substitution in
    `extra_env`/`extra_args`, textual modes only. `std::string` now holds
    canonical text in every manager (R5.1), so R7.7's decimal-text string
    mode is gone: the OCI and Docker tests for it were replaced. In-tree
    code that relied on the defaults now names `<std::uint64_t>`.
  - _Requirements: 5.1-5.5, 6.4, 14.3_

- [x] 5. Raft core and serializers
  - [x] 5.1 Remove `node_id_to_u64`; type `_responder_id` as `NodeId`;
    fix `group_transport.hpp:325`
  - [x] 5.2 Add `NodeIdValue responder` to `raft_messages.proto` with
    fallback decoding
  - [x] 5.3 Serializers: textual ids through `node_id_traits`; JSON,
    object-store snapshot and backup numeric paths without `int64` casts
  - [x] 5.4 Serializer round-trip property test for each composite type
  - Done. As built: the proto field is `responder = 6`, not 2 (tag 2 is
    `available`). `fetch_log_entries_response` takes `NodeId` as its first
    parameter, matching the request types. The multi-raft demultiplexer
    learns the host's id through `set_local_node_id()` for the "unknown
    group" answer. The JSON numeric branch was already range-checked by the
    json-serializer-input-validation work; the remaining `as_int64()` reads
    were the snapshot member lists in both persistence engines.
    `composite_node_id_wire_property_test` covers all four serializers.
    Still open: persistence engines only handle numeric and `std::string`
    ids (composite ids there belong with task 6.5's cluster test). (The ION
    gap for numeric ids at or above 2^63 was closed separately.)
  - _Requirements: 8.1-8.6_

- [x] 6. Transports
  - [x] 6.1 Parameterise the three `network_client` concepts on the
    target type
  - [x] 6.2 Add a `NodeId` parameter to the simulator, TCP, TLS-TCP,
    httplib, Proxygen, Beast, gRPC and CoAP transports
  - [x] 6.3 Fix `tcp_rpc` address dispatch and `tls_tcp_rpc` peer names
  - [x] 6.4 Add dual node id fields to `proto/raft.proto` and the gRPC
    conversion
  - [x] 6.5 `std::string`-target smoke test per transport; three-node
    composite-id cluster test over the simulator with forced catch-up
  - First part done: 6.1 in full; 6.2 for the simulator and TCP; 6.3's
    `tcp_rpc` half; 6.5's TCP smoke test and the simulator cluster test
    (`composite_node_id_transport_test`). As built: every client and
    server concept in `network.hpp` (the optional extensions and
    ClusterJoin/Leave too) takes `NodeId` defaulting to `std::uint64_t`,
    and `node<Types>` checks its extensions against `node_id_type`, so a
    textual node keeps PreVote, TimeoutNow and catch-up.
    `node<Types>` static-asserts the base client concept only for
    non-numeric ids, since numeric test bundles use partial mocks. The TCP
    transport is `basic_tcp_rpc_client<NodeId>`/`basic_tcp_rpc_server<NodeId>`
    with the old names as numeric aliases. The cluster test also needed the
    configuration log entry codec (`config_entry.hpp`) to encode ids
    through `node_id_traits` and read numeric ids across the full `uint64`
    range. Second part done: TLS-TCP for 6.2, 6.3 and 6.5. As built:
    `basic_tls_tcp_rpc_client<NodeId>`/`basic_tls_tcp_rpc_server<NodeId>`
    and `basic_tls_rpc_trust_policy<NodeId>`, numeric aliases keeping the
    old names; the policy matches a textual id's URI SAN and a numeric
    id's DNS SAN, never the other (`cert_has_peer_name_in<NodeId>`);
    `tls_tcp_rpc_textual_node_id_test` covers `std::string` and
    `aws_ec2_node_id` peers. Third part done: httplib, Beast, Proxygen,
    gRPC and all three CoAP backends. As built: each client and server
    takes `NodeId` defaulting to `std::uint64_t` (old names stay the
    numeric instantiation) and renders ids with `node_id_traits::to_text`.
    No transport puts an id in a URL path, header or CoAP option, so
    nothing needed percent-encoding. The HTTP transports still bind TLS to
    the dialled host's DNS/IP SAN, not to the node id; libcoap, cantcoap
    and libnyoci gain `coap_peer_identity_validator` (URI SAN for textual
    ids), and libcoap now runs its name check on the leaf only. gRPC adds
    `NodeIdValue` beside every uint64 id field at a new tag: numeric
    senders fill both, textual senders only the new one, receivers prefer
    it, so a rolling upgrade works both ways. Each transport has a
    `*_textual_node_id_test` over `std::string` and `aws_ec2_node_id`.
  - _Requirements: 9.1-9.6, 14.4, 14.5_

- [x] 7. CA identity and serials
  - [x] 7.1 Generalise peer identity, enrollment MAC and `rpc_tls_ready`
    to `NodeId`; URI SAN for textual ids; version byte (library half done:
    `peer_identity_name`, `is_reserved_peer_uri`, `peer_enrollment_mac`
    and `classify_peer_enrollment` over `NodeId`, the CA issuing
    `uri_names`, and `record_rpc_tls_ready` version 2, a `"version": 2`
    field with the id as a JSON string. Binary half done with task 8.2:
    `ca_cluster_node` built with `KYTHIRA_NODE_ID_KIND=text` takes a
    textual `--node-id` and peer ids, certifies its peer identity as the URI
    SAN under the fixed CN `kythira-ca-cluster-peer`, and carries the id as
    a string in the readiness command and its HTTP body; numeric builds are
    unchanged)
  - [x] 7.2 Add `certificate_serial`; use it in the CA, the three cloud
    CA providers and both revoke endpoints
    (done; as built: up to 20 octets, ledger entries keep the numeric
    encoding when a serial fits 64 bits and use `"version": 2` with hex
    otherwise; revoke bodies keep `"serial"` as a decimal number or
    string, hex on the ACM path where it always was, and accept the new
    `"serial_hex"` for any width; issue/renew responses carry
    `"serial_hex"`. The in-house CA still issues 64-bit serials, and the
    peer-trust revocation set stays 64-bit)
  - [x] 7.3 Tests: numeric enrollment unchanged, textual enrollment and
    verification, reserved-URI refusal, 20-octet serial revoke
  - 7.1 and 7.3 done October 16, 2026; 7.2 (`certificate_serial`) is
    ticked with its own change. 7.3's enrollment and URI cases are in
    `ca_issuance_hardening_unit_test`, `ca_state_machine_unit_test` and
    `tls_tcp_rpc_textual_node_id_test`; its 20-octet serial case is 7.2's
    `certificate_serial_unit_test`.
    `ca_cluster_node_test`, `ca_cluster_node_rpc_tls_test` and
    `ca_cluster_node_rpc_tls_restart_test` pass against a text-mode
    `ca_cluster_node` (decimal ids, so URI-SAN peers).
  - _Requirements: 12.1-12.5_

- [x] 8. Self id and binaries
  - [x] 8.1 Add `include/raft/self_node_id.hpp` for the five metadata
    services, opt-in, bounded timeout; unit tests against a local fake
    endpoint
  - [x] 8.2 Add `KYTHIRA_NODE_ID_KIND` to Kconfig and `host_node_id`;
    convert the `cmd/` aliases and parsers to `parse_host_node_id`
  - [x] 8.3 Print textual ids as JSON strings in `multi_raft_node`'s
    control server
  - [x] 8.4 Build every binary in both Kconfig modes in CI
  - Done October 16, 2026. As built: `self_node_id.hpp` covers aws-ec2,
    alibaba-ecs, azure-vm, gcp and oci (`aws`, `alibaba` and `azure` also
    accepted); the bound is one deadline over the whole lookup (default
    2 s) rather than one call, since AWS, Alibaba and GCP need two or three
    requests; there is no IMDSv1 fallback, Alibaba uses the token flow too,
    GCP is addressed by IP so an off-cloud lookup never waits on DNS, and
    the result type is `node_id_result<T>` (std::expected's member names)
    because clang 18 over libstdc++ 13 has no `std::expected`.
    `host_node_id.hpp` holds the alias and the parsers: text mode takes
    canonical text of any provider or a decimal integer (normalised), the
    local id also `metadata:<provider>`, which a numeric build refuses;
    peer ids never read metadata. Deviations: `KYTHIRA_NODE_ID_TEXT`
    reaches C++ through the `kythira_host_node_id` CMake target, not an
    `#include` of `autoconf.hpp` (which would define every feature macro);
    `multi_raft_node` still reads its id only from `--node-id`, not
    `KYTHIRA_NODE_ID`. Behaviour in text mode: `chaos_node`'s default peer
    address becomes `{native}:<port>` (a new placeholder); `ca_cluster_node`
    uses the URI SAN (7.1); every JSON node id, including
    `multi_raft_node`'s control server and capacity plane, is a JSON
    string, through `node_id_json` (8.3). Library touch points, each
    defaulted: `basic_tcp_raft_types<NodeId>`,
    `threshold_split_merge_policy<NodeId>`, the multi-Raft harness handles,
    and by-reference loops in `file_persistence.hpp`. 8.4 builds the
    `host_binaries` aggregate and `host_node_id_test` in the Config
    Variants job from a second defconfig, not a full build-and-test leg:
    the library and tests are already instantiated over textual ids, so
    only the binaries' own instantiations were missing.
  - _Requirements: 6.1-6.3, 10.1-10.5_

- [x] 9. LocalStack without the hook
  - [x] 9.1 Run `ca_cluster_node_localstack_test` and the EC2 cases of
    `aws_quorum_manager_localstack_test` against stock LocalStack 4.x
  - [x] 9.2 Delete `aws-shaped-instance-ids.py` and its compose mount
    from whichever of PRs #433/#442 or main carries them; update the
    project memory note
  - _Requirements: 13.2, 13.3_

- [x] 10. Documentation and default flip
  - [x] 10.1 Provider READMEs and the quorum-management doc: composite
    ids, `--node-id=metadata:<provider>`, numeric mode for existing
    clusters
  - [x] 10.2 Point the provider specs' node-id criteria to this spec
  - [x] 10.3 Separately, once tasks 5-7 are on main, flip
    `KYTHIRA_NODE_ID_KIND` to `text` (breaking)
  - Done October 16, 2026, after tasks 6 and 7.2 reached `main` (#562),
    in the same change as task 8, whose binaries it flips. As built: the
    Kconfig default is `text`, so `ci_full_defconfig` and every defconfig
    derived from it build text binaries, and the build-and-test legs run
    the `ca_cluster_node` suites against them (with decimal ids, which a
    text build normalises; all three suites pass locally that way).
    `configs/ci_numeric_node_id_defconfig` replaces
    `ci_text_node_id_defconfig`, so Config Variants still builds both
    kinds. Deviation: the CMake cache variable `KYTHIRA_NODE_ID_KIND`
    (`-DKYTHIRA_NODE_ID_KIND=numeric|text`) picks the kind without
    kconfiglib, and a configure without kconfiglib otherwise follows the
    text default, so "default" means the same on every machine. The
    order is Kconfig (a config that sets the choice), then the override
    (a configure error if it contradicts that config), then text. The
    builds that stay numeric pin the override: the `ca_cluster_node`
    Dockerfile (and the ECS task definitions and AMI built from it), and
    the container-test and arm64 smoke-test workflows, because
    `quorum-healing-tests` and `docker-elastic-capacity-tests` model
    numeric clusters (container names `kythira-<cluster>-<n>`, the
    admitted node read as a JSON number). Those two refuse to build in a
    text build instead of failing obscurely. The other real-cloud jobs
    run no build-tree host binary (the AWS job deploys the pinned AMI), so
    they are not pinned.
  - _Requirements: 10.1_

## Notes

- Task 2 fixes real bugs on the numeric paths and can ship first, on its
  own, without any composite-mode work.
- Task 3 alone, in numeric mode with tag lookup, is enough to retire the
  LocalStack hook (task 9) for the existing binaries, which stay numeric
  until task 10.3.
- Tasks 5 and 6 are what make a composite `NodeId` usable in a running
  `kythira::node`; until they land, composite ids are usable in the
  managers and in `std::string` form only.
- Tasks 4.1 and 4.2 touch the same adoption code as the
  group-scale-up-rollback spec; whichever lands second rebases.
