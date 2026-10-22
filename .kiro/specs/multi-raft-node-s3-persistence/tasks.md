# Implementation Plan

## Implementation Status

Spec only. No task has started.

**Blocked on `.kiro/specs/object-backup-s3-endpoint/` (PR #599):** do not
start task 1 until that spec's task 2 (`include/raft/aws_endpoint_override.hpp`)
has merged. Task 3.2's help text also needs its task 1 findings, and task 9
needs its task 7.1 decision (LocalStack or MinIO).

## Overview

Add `--persistence s3` to `multi_raft_node`: an SDK-free parser for the
`--s3-*` flags, an S3 alternative in `switchable_persistence` holding either
fencing instantiation, a process-wide AWS SDK scope, build gating, and unit,
process and CI tests against a stateful S3 mock and a real S3-compatible
service. The engine and `aws_s3_client` do not change.

## Tasks

- [ ] 1. Build gating
  - [ ] 1.1 In `cmd/multi_raft_node/CMakeLists.txt`, find AWSSDK with one
    component list built from what is enabled (`ec2` for discovery, `s3`
    when `AWSSDK_FOUND AND _KYTHIRA_GATE_AWS_S3_PERSISTENCE`), define
    `KYTHIRA_NODE_HAS_S3_PERSISTENCE` in the second case, and confirm
    `AWSSDK_LINK_LIBRARIES` then carries both components
  - [ ] 1.2 Confirm the `no_cloud` defconfig build still configures and links
    `multi_raft_node` with no SDK
  - _Requirements: 7.1, 7.3_

- [ ] 2. Options and parser
  - [ ] 2.1 Add `persistence_mode::s3`, `s3_fencing` and
    `s3_persistence_options` to `config.hpp`, with `hard_state` defaulting
    to `combined`
  - [ ] 2.2 Add the `--s3-*` branches to the shared `apply` lambda in
    `config.cpp`, recording `flags_seen`
  - [ ] 2.3 Add the validation rows in the design: required flags,
    `--data-dir` refused, `--s3-*` refused under other modes,
    `validate_s3_endpoint()`, `--s3-timeout 0`, the
    `ignored_endpoint_variable_warnings()` and election-timeout warnings
    with `k_s3_min_election_timeout`
  - [ ] 2.4 Write `tests/multi_raft_node_s3_options_test.cpp` and register it
    unconditionally
  - _Requirements: 1.1, 1.5, 2.1, 2.2, 3.1, 3.2, 3.3, 3.4, 3.5, 5.1, 5.2, 8.1, 8.2, 8.3, 10.1_

- [ ] 3. Help text
  - [ ] 3.1 Document `s3` under `--persistence` in `node_usage()`, marked
    unavailable when the arm is not compiled in
  - [ ] 3.2 Add an "S3 persistence" section listing every `--s3-*` flag with
    its default, and the region variables from object-backup-s3-endpoint
    task 1
  - _Requirements: 7.2, 9.1_

- [ ] 4. Store
  - [ ] 4.1 Add the two S3 engine aliases and alternatives to
    `switchable_state`, the S3 constructor (owner id from the node id under
    `cas`), and the new `dispatch` branches
  - [ ] 4.2 Forward `durability()` and `durable_through()` to the S3
    engines
  - [ ] 4.3 Build the host and group prefix in `make_store()`
  - [ ] 4.4 Measure peak compile memory of a `run_*.cpp` before and after.
    If it rises materially, move the S3 engine construction behind an
    out-of-line factory in its own translation unit, and record the numbers
    in design.md
  - _Requirements: 1.2, 1.3, 2.3, 4.1, 4.2, 4.3_

- [ ] 5. `main` and status surface
  - [ ] 5.1 Add `aws_sdk_scope`, construct it for `s3` or `ec2-tag` before
    discovery so it outlives every store, and remove discovery's own
    init/shutdown
  - [ ] 5.2 Refuse `--persistence s3` in a build without the arm, naming
    `CONFIG_AWS_S3_PERSISTENCE`
  - [ ] 5.3 Preflight the engine options against a throwaway empty store
    before the transport starts
  - [ ] 5.4 Print the host prefix and effective options at startup; catch
    `persistence_fenced_error` from group construction with the
    Requirement 4.4 message
  - [ ] 5.5 Extend `control_server.hpp`: `persistence_name()` and `durable()`
    for `s3`, the `"s3"` status object, and `"fenced_groups"`
  - _Requirements: 2.4, 4.4, 5.3, 5.4, 6.1, 6.2, 6.3, 7.2_

- [ ] 6. Stateful S3 mock and store unit test
  - [ ] 6.1 Add `tests/s3_stateful_mock.hpp` (PUT with conditional headers and
    `Content-MD5`, GET, DELETE, ListObjectsV2 with continuation) bound to
    `localhost`, with a PUT delay knob, a fail-next-N rule by key prefix,
    per-prefix PUT counters and a concurrent-request high-water mark
  - [ ] 6.2 Add `tests/multi_raft_node_s3_store_test.cpp` running the
    switchable store's S3 arm through it under both fencing modes, including
    a refused second owner
  - [ ] 6.3 Add the write-behind wrapper cases: the `durability()` matrix,
    staging with no PUT then one flush at `barrier_through`,
    `durable_through()` tracking, and the `per-entry` request-round bound
  - _Requirements: 10.2, 11.1, 11.2_

- [ ] 7. Process test
  - [ ] 7.1 Add `tests/multi_raft_node_s3_process_test.cpp`: three hosts,
    write, stop, check key layout, restart, read back, then a duplicate
    node id refused under `cas`; run with write-behind off and on
  - [ ] 7.2 Add the unavailable-arm case, registered only in a build
    without the arm
  - [ ] 7.3 Register both with the gating in Requirement 10.6
  - _Requirements: 1.4, 4.4, 10.3, 10.4, 10.6_

- [ ] 8. Write-behind process tests
  - [ ] 8.1 Batching: 16 writers, 50 ms PUT delay; assert B > 2 with
    write-behind on and `segmented`, B = 1 with it off and `per-entry`;
    print both; cross-check the status surface's `flushes` against the
    mock's leader log PUT count
  - [ ] 8.2 Crash: SIGKILL the leader under write-behind load, restart it,
    and read back every acknowledged key
  - [ ] 8.3 Failed flush: fail one leader log PUT; assert no acknowledged
    entry is missing from the mock, later writes succeed, and every
    acknowledged key survives a full restart
  - [ ] 8.4 Run the duplicate-host case with write-behind on and assert the
    refused host wrote nothing under the first host's prefix
  - _Requirements: 11.3, 11.4, 11.5, 11.6, 11.7_

- [ ] 9. CI against a real S3-compatible service
  - [ ] 9.1 Add a step after the backup tool's S3 step from
    object-backup-s3-endpoint task 7, reusing its service and bucket setup,
    running the three-host write, restart and read cycle with write-behind
    off and on
  - [ ] 9.2 Run it locally under rootless Podman as well as Docker
  - _Requirements: 10.5_

- [ ] 10. Documentation
  - [ ] 10.1 Add "Running a cluster on S3" to
    `doc/cloud_object_persistence.md`: a three-host LocalStack or MinIO
    example, a real-S3 example, the election-timeout note, and that the host
    is a measurement tool
  - [ ] 10.2 Add a `doc/CHANGELOG.md` entry
  - _Requirements: 9.2, 9.3_

## Notes

- A Tier D row on real S3 is the natural follow-up. It needs Clark's go
  under the real-cloud rules, so it is not a task here.
- Other providers would be further arms of the same shape, with
  `--azure-blob-*`, `--gcs-*` and similar flag groups.
- Validating `--discovery-endpoint` with the same header is a one-line
  follow-up once task 2 lands.
