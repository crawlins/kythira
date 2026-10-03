# Implementation Plan

## Implementation Status

Implemented on 2026-10-03, except task 6.3 (see below). Where each test ran:

| Test | Where it ran |
|---|---|
| `leader_transfer_test` (tasks 1.5, 5.3 on the fabric) | locally, boost future backend |
| `http_pre_vote_timeout_now_test` (5.4, 5.5, 6.1, 6.2) | locally on the Folly backend against Folly/Proxygen v2025.05.19.00, three runs, all transports |
| `three_way_http_transport_equivalence_test` (5.1-5.3) | locally on the Folly backend, same Proxygen build |
| `coap_timeout_now_test` (libcoap 4.3.5; 8.1, 8.4, 9.2) | locally |
| `coap_cantcoap_integration_test` (8.3, 9.1) | locally |
| `coap_libnyoci_integration_test` (8.2, 9.1) | **compiled only**: the sandbox has no IPv6, which libnyoci needs. Not run. |
| Raft election and membership property tests | locally, as a regression check after the leader-refuses-pre-vote fix |

Two departures from the design:

- **A leader with a live quorum refuses pre-votes**
  (`handle_request_pre_vote()`). The isolated-follower test (5.4) failed
  without it: in a three-node cluster the leader itself granted a rejoining
  follower's pre-vote, which with the follower's own vote is a majority, so
  the follower raised its term anyway. The leader refuses only while a
  majority answered it within an election timeout, etcd's CheckQuorum lease;
  refusing unconditionally broke `membership_change_leave_rpc_property_test`,
  where a leader that can hear but not reach its peers must be replaced.
- **`coap_conformance_types.hpp` is now `transport_conformance_types.hpp`**,
  because the HTTP headers' concept assertions (2.5) use the same types
  bundle. `coap_detail::conformance_types` stays as an alias.

Design §5.4's "repeat with the pre-vote handler unregistered to show the test
can fail" was not added as a permanent case. The test is known to detect a
raised term: it failed that way before the fix above.

## Overview

Add RequestPreVote and TimeoutNow to the three HTTP transports and
RequestPreVote to the three CoAP backends, with a core fallback that keeps
elections working while a cluster is part-way through an upgrade.

## Tasks

- [x] 1. Not-implemented outcome in the core
  - [x] 1.1 Add `rpc_not_implemented_exception` (derived from
    `network_exception`, with `rpc()` and `target()`) to
    `include/raft/exceptions.hpp`
  - [x] 1.2 Classify it as non-retryable in `error_handler::classify_error()`
  - [x] 1.3 In `start_pre_vote()`, count it as a granted pre-vote, warn once
    per peer, and emit `raft.pre_vote.peer_not_implemented`
  - [x] 1.4 In `transfer_leadership()`'s TimeoutNow send path, turn it into
    `leader_transfer_unsupported_exception`
  - [x] 1.5 Unit-test 1.2-1.4 over the simulator with a client wrapper that
    throws the exception for chosen peers
  - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 4.6_

- [x] 2. cpp-httplib
  - [x] 2.1 Add the two endpoint constants, client `send_*` members, server
    handlers, `register_*_handler` methods and `setup_endpoints()` routes
  - [x] 2.2 Add the two names to the `rpc_type` chains in `send_rpc` and
    `handle_rpc_endpoint`
  - [x] 2.3 Answer 501 for an unregistered extension handler
  - [x] 2.4 Map 404 and 501 on an extension RPC to
    `rpc_not_implemented_exception` in the client wrappers
  - [x] 2.5 Add the positive and negative concept `static_assert`s
  - _Requirements: 1.1-1.5, 2.1, 2.2, 3.1, 3.3, 3.4_

- [x] 3. Boost.Beast: same as task 2 for `boost_beast_client` /
  `boost_beast_server`
  - _Requirements: 1.1-1.5, 2.1, 2.2, 3.1, 3.3, 3.4_

- [x] 4. Proxygen: same as task 2 for `proxygen_client` / `proxygen_server`
  - _Requirements: 1.1-1.5, 2.1, 2.2, 3.1, 3.3, 3.4_

- [x] 5. HTTP tests
  - [x] 5.1 Extension round trips with `group_id` in
    `tests/three_way_http_transport_equivalence_test.cpp`
  - [x] 5.2 Unregistered-handler case on each HTTP transport, checking one
    attempt only
  - [x] 5.3 Older-peer case: cluster with peers serving only the three
    mandatory routes still elects, one warning per peer
  - [x] 5.4 Three-node cluster over cpp-httplib: pre-vote round logged;
    isolated-then-reconnected follower leaves the leader's term unchanged;
    `transfer_leadership()` reaches the named node
  - [x] 5.5 `transfer_leadership()` on Beast and Proxygen
  - [x] 5.6 Sweep HTTP tests for the `make_dormant_config()` pattern (design
    §7) and fix any that now fail (none: only the simulator-based peer2peer
    tests use it)
  - _Requirements: 6.1, 6.2, 6.3, 6.4_

- [ ] 6. Multi-Raft over HTTP
  - [x] 6.1 Test that `multi_group_network_server` over cpp-httplib registers
    the pre-vote and timeout-now handlers
  - [x] 6.2 Host test: `transfer_leader` and `scatter` end `accepted` over
    cpp-httplib
  - [ ] 6.3 Re-run the elastic-capacity Docker scenario and confirm the
    move-off-leader path no longer leaves a shard one voter over
    (not run: needs the Docker elastic scenario, which the implementing
    sandbox cannot host; 6.2 covers the same transfer on the host API)
  - _Requirements: 2.3, 6.5_

- [x] 7. Documentation
  - [x] 7.1 HTTP transport docs: five endpoints and the capability table
    (`doc/http_transport_endpoints.md`)
  - [x] 7.2 Point multi-raft-performance requirements (the "three mandatory
    RPCs" note) and elastic-shard-capacity design §15 amendment 10 at this
    spec
  - [x] 7.3 `doc/CHANGELOG.md`: new endpoints and the rolling-upgrade
    behaviour
  - _Requirements: 7.1, 7.2, 7.3_

- [x] 8. CoAP PreVote (after #385)
  - [x] 8.1 libcoap: `send_request_pre_vote` at default reliability, the
    `raft/request_pre_vote` resource with request_vote's flags, 5.01 when
    unregistered
  - [x] 8.2 libnyoci: the same
  - [x] 8.3 cantcoap: the same
  - [x] 8.4 Map 4.04 and 5.01 on an extension RPC to
    `rpc_not_implemented_exception` on all three
  - [x] 8.5 Flip each backend's pre-vote `static_assert` to positive and
    update `tests/coap_capability_table.hpp`
  - _Requirements: 3.2, 3.3, 5.1, 5.2, 5.3, 5.4_

- [x] 9. CoAP tests and docs (after #385)
  - [x] 9.1 Pre-vote case in each backend's integration test, beside the
    TimeoutNow case
  - [x] 9.2 Unregistered-handler and older-peer cases on libcoap
  - [x] 9.3 Update the coap-transport-multi-raft design §2 table
  - _Requirements: 5.4, 6.3, 6.4, 6.6, 7.2_

## Notes

- cantcoap and libnyoci are not built in CI (parity audit C1), so 9.1 for
  those two backends can only be run locally. Record where it ran; do not
  tick it on a stub build that skipped.
- Re-run the Raft timing sweep (#390) and the multi-Raft benchmark HTTP and
  CoAP rows after this lands (design §7).
