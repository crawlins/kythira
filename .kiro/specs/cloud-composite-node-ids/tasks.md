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

- [ ] 3. AWS managers
  - [x] 3.1 Replace `ec2_id_to_node_id`/`node_id_to_ec2_id` with
    `to_instance_id`/`from_instance`; composite and string mode use
    `aws_ec2_node_id` (landed as `instance_id_of`/`node_id_of_instance`
    on both managers, over the static `instance_id_for_node`/
    `node_id_for_instance` and numeric tag-lookup helpers)
  - [x] 3.2 Numeric mode: tag-based allocation and lookup, identity tags
    and `{NODE_ID}` in the launch request, and the R11.4 fallback
  - [x] 3.3 ASG manager: route through the new helpers in both modes
  - [ ] 3.4 Change both defaults to `aws_ec2_node_id` (breaking)
  - [x] 3.5 Unit tests with `i-f0123456789abcdef`, `i-1234abcd` and
    `i-0123456789abcdef0` in all three modes
    (`tests/aws_ec2_node_id_mock_test.cpp`, a local EC2/Auto Scaling fake)
  - _Requirements: 4.1-4.6, 11.1-11.5, 13.1_

- [ ] 4. Other managers in composite mode
  - [ ] 4.1 Alibaba ESS and OCI pool: id from the adopted instance
  - [ ] 4.2 Azure VMSS and GCP MIG: id from the member name and scope
  - [ ] 4.3 Azure VM, GCP Compute and Docker: generated names within
    provider limits; `{NODE_ID}` substitution
  - [ ] 4.4 Change each default `NodeId` (breaking)
  - [ ] 4.5 Composite-mode case per manager in its mock or unit suite
  - _Requirements: 5.1-5.5, 6.4, 14.3_

- [ ] 5. Raft core and serializers
  - [ ] 5.1 Remove `node_id_to_u64`; type `_responder_id` as `NodeId`;
    fix `group_transport.hpp:325`
  - [ ] 5.2 Add `NodeIdValue responder = 2` to `raft_messages.proto` with
    fallback decoding
  - [ ] 5.3 Serializers: textual ids through `node_id_traits`; JSON,
    object-store snapshot and backup numeric paths without `int64` casts
  - [ ] 5.4 Serializer round-trip property test for each composite type
  - _Requirements: 8.1-8.6_

- [ ] 6. Transports
  - [ ] 6.1 Parameterise the three `network_client` concepts on the
    target type
  - [ ] 6.2 Add a `NodeId` parameter to the simulator, TCP, TLS-TCP,
    httplib, Proxygen, Beast, gRPC and CoAP transports
  - [ ] 6.3 Fix `tcp_rpc` address dispatch and `tls_tcp_rpc` peer names
  - [ ] 6.4 Add dual node id fields to `proto/raft.proto` and the gRPC
    conversion
  - [ ] 6.5 `std::string`-target smoke test per transport; three-node
    composite-id cluster test over the simulator with forced catch-up
  - _Requirements: 9.1-9.6, 14.4, 14.5_

- [ ] 7. CA identity and serials
  - [ ] 7.1 Generalise peer identity, enrollment MAC and `rpc_tls_ready`
    to `NodeId`; URI SAN for textual ids; version byte
  - [ ] 7.2 Add `certificate_serial`; use it in the CA, the three cloud
    CA providers and both revoke endpoints
  - [ ] 7.3 Tests: numeric enrollment unchanged, textual enrollment and
    verification, reserved-URI refusal, 20-octet serial revoke
  - _Requirements: 12.1-12.5_

- [ ] 8. Self id and binaries
  - [ ] 8.1 Add `include/raft/self_node_id.hpp` for the five metadata
    services, opt-in, bounded timeout; unit tests against a local fake
    endpoint
  - [ ] 8.2 Add `KYTHIRA_NODE_ID_KIND` to Kconfig and `host_node_id`;
    convert the `cmd/` aliases and parsers to `parse_host_node_id`
  - [ ] 8.3 Print textual ids as JSON strings in `multi_raft_node`'s
    control server
  - [ ] 8.4 Build every binary in both Kconfig modes in CI
  - _Requirements: 6.1-6.3, 10.1-10.5_

- [x] 9. LocalStack without the hook
  - [x] 9.1 Run `ca_cluster_node_localstack_test` and the EC2 cases of
    `aws_quorum_manager_localstack_test` against stock LocalStack 4.x
  - [x] 9.2 Delete `aws-shaped-instance-ids.py` and its compose mount
    from whichever of PRs #433/#442 or main carries them; update the
    project memory note
  - _Requirements: 13.2, 13.3_

- [ ] 10. Documentation and default flip
  - [ ] 10.1 Provider READMEs and the quorum-management doc: composite
    ids, `--node-id=metadata:<provider>`, numeric mode for existing
    clusters
  - [ ] 10.2 Point the provider specs' node-id criteria to this spec
  - [ ] 10.3 Separately, once tasks 5-7 are on main, flip
    `KYTHIRA_NODE_ID_KIND` to `text` (breaking)
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
