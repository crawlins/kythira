# Implementation Plan — Peer-to-Peer Gossip Transport

## Status: Not Started

**Last Updated**: July 10, 2026

## Overview

Implement `tcp_gossip_peer2peer_replicator`, a real anti-entropy gossip
transport satisfying the `peer2peer_replicator` concept from
`.kiro/specs/peer2peer-log-replication/` (a dependency of this spec — that
spec's Requirement 1 concept, Requirement 11 membership-sync mechanism, and
Requirement 9.2 deferred "production-grade gossip transport" scope are what
this spec fulfills). Self-contained: its own TCP listener, its own
background gossip thread, its own small `boost::json` wire protocol — no
changes to `raft.hpp`, `types.hpp`, or `network.hpp`. Current membership
comes exclusively from `update_membership()` calls driven by the replicated
log (via the depended-on spec's `cluster_members()`); this transport's own
static configuration supplies address resolution only.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 2],
      "description": "Independent foundational types: gossip_digest/gossip_exchange_message wire types + boost::json encode/decode, and tcp_gossip_config (address_book + tuning knobs, no membership field)"
    },
    {
      "wave": 2,
      "tasks": [3, 4],
      "description": "TCP listener/accept loop (needs task 1's wire encode/decode) and merge()/prune_expired() local-table logic (needs only task 1's gossip_digest) — independent of each other"
    },
    {
      "wave": 3,
      "tasks": [5],
      "description": "exchange_with() client-side push-pull round — needs task 1 (wire format) and task 4 (merge, to apply the response)"
    },
    {
      "wave": 4,
      "tasks": [6],
      "description": "update_membership()/eligible_peers() — needs task 2's tcp_gossip_config (address_book) only; independent of the gossip thread itself"
    },
    {
      "wave": 5,
      "tasks": [7],
      "description": "gossip_loop()/start_gossip_thread()/stop_gossip_thread() — needs task 5 (exchange_with), task 4 (prune_expired), and task 6 (eligible_peers, to select fanout from)"
    },
    {
      "wave": 6,
      "tasks": [8],
      "description": "Public API (advertise_progress/find_catch_up_source/update_membership) and static_assert against peer2peer_replicator — ties together tasks 3, 4, 6, 7"
    },
    {
      "wave": 7,
      "tasks": [9, 10],
      "description": "Unit tests for merge/prune/eligible-peers/fanout-selection logic — depend on tasks 4, 6, 7; independent of each other"
    },
    {
      "wave": 8,
      "tasks": [11, 12, 13, 14],
      "description": "Integration, end-to-end, freshness-expiry, and membership-removal tests — depend on task 8's complete public API; independent of each other"
    }
  ]
}
```

## Tasks

---

## Phase 1: Wire Types and Configuration (Tasks 1–2)

- [ ] 1. Add `gossip_digest` and `gossip_exchange_message` wire types
  - New file `include/raft/tcp_gossip_transport.hpp`
  - `gossip_digest<NodeId, Address, LogIndex>`: `node_id`, `address`
    (this peer's Raft RPC address, per Requirement 7.1 — not the gossip
    listener's own address), `term`, `last_log_index`, `fresh_until`
    (epoch seconds)
  - `gossip_exchange_message<NodeId, Address, LogIndex>`: `sender_node_id`,
    `std::vector<gossip_digest<...>>` — used as both the request and
    response shape (symmetric push-pull)
  - `encode_gossip_message(...)`/`decode_gossip_message<...>(...)` using
    `boost::json` directly, following `ca_state_machine`'s existing
    command-encoding convention rather than extending
    `json_rpc_serializer`
  - Verify: unit test round-trips a `gossip_exchange_message` with several
    digests through encode then decode, confirms exact equality
  - _Requirements: 5.1, 5.2_

- [ ] 2. Add `tcp_gossip_config`
  - Same file as Task 1
  - `address_book` (`std::vector<peer_info<NodeId, Address>>` — address
    resolution only, deliberately NOT a membership statement, Requirement
    2.1), `listen_port`, `fanout` (default 3), `gossip_round_interval`
    (default 500ms), `freshness_interval` (default 5s) — each field
    documented with a `///<` comment
  - Verify: `cmake --build build` compiles; defaults match the values
    specified here; confirm by inspection that `tcp_gossip_config` has no
    "current members" field of any kind (Requirement 2.2)
  - _Requirements: 2.1, 9.1, 9.2_

---

## Phase 2: Transport Internals (Tasks 3–4)

- [ ] 3. Implement the TCP listener and accept loop
  - Same file — `start_listener()`/`stop_listener()`/`accept_loop()`,
    reusing `tcp_detail::connect_to`/`frame_send`/`frame_recv`/
    `bytes_to_str`/`str_to_bytes` from `tcp_rpc.hpp` for framing, exactly
    as `tls_tcp_rpc.hpp` already reuses them (per
    `.kiro/specs/ca-cluster-rpc-mtls/`)
  - `handle_incoming_exchange(fd)`: decode the incoming
    `gossip_exchange_message`, merge it (Task 4), reply with this node's
    own current table encoded the same way
  - A malformed/undecodable incoming payload closes the connection without
    a response (Requirement 5.4)
  - Verify: unit test opens a raw socket to a running listener, sends a
    hand-encoded valid `gossip_exchange_message`, confirms a well-formed
    response is received and the sent digests are reflected in the
    listener's table afterward; a second test sends garbage bytes and
    confirms the connection is closed without hanging or crashing
  - _Requirements: 3.1, 3.2, 5.3, 5.4_

- [ ] 4. Implement `merge()` and `prune_expired()`
  - Same file — `merge(incoming)`: for each digest, replace the local
    entry for that `node_id` iff `(incoming.term, incoming.last_log_index)`
    is lexicographically greater than the existing entry's, or add it if
    absent (`std::tie` comparison, under `folly::Synchronized`'s write
    lock)
  - `prune_expired()`: removes every entry whose `fresh_until` has passed
    the current epoch time
  - Verify: covered by Task 9's unit tests (this task's own build/compile
    verification is `cmake --build build`)
  - _Requirements: 6.1, 6.2, 6.3, 6.4_

---

## Phase 3: Membership, Gossip Rounds, and Public API (Tasks 5–8)

- [ ] 5. Implement `exchange_with()` (client-side push-pull)
  - Same file — connects to the given peer's Raft-RPC-derived `host:port`
    (split from `Address`), sends this node's current table snapshot as a
    `gossip_exchange_request`, receives the peer's table as a
    `gossip_exchange_response`, merges it (Task 4)
  - A connection failure, send failure, or receive failure is logged and
    the method simply returns — no exception propagates
    (Requirement 8.1/8.3)
  - Verify: unit test against two real, already-running
    `tcp_gossip_peer2peer_replicator`-style listeners (or a minimal test
    double presenting the same wire protocol) confirms both sides' tables
    reflect the other's post-exchange digests; a test against an
    unreachable port confirms `exchange_with` returns cleanly without
    throwing
  - _Requirements: 5.1, 5.2, 5.3, 8.1, 8.3_

- [ ] 6. Implement `update_membership()` and `eligible_peers()`
  - Same file — `update_membership(member_ids)`: replaces
    `_active_members` (a `folly::Synchronized<std::unordered_set<NodeId>>`,
    empty until the first call, Requirement 2.2) and resolves immediately
  - `eligible_peers()`: returns the subset of `_cfg.address_book` whose
    `node_id` is currently in `_active_members` — the intersection, not
    union (Requirement 2.3); a member with no address, or an address for a
    non-member, is excluded either way
  - Verify: unit test confirms `eligible_peers()` is empty before the first
    `update_membership()` call; confirms it reflects exactly the
    intersection across several `update_membership()`/`address_book`
    combinations, including a member absent from `address_book` and an
    `address_book` entry for a non-member
  - _Requirements: 1.4, 2.2, 2.3_

- [ ] 7. Implement the background gossip thread
  - Same file — `start_gossip_thread()`/`stop_gossip_thread()`/
    `gossip_loop()`, mirroring `rfc2136_dns_sd_discovery`'s
    `start_fresher()`/`stop_fresher()`/`fresher_loop()` shape exactly
    (`include/raft/rfc2136_dns_sd_discovery.hpp:456-493`)
  - Each round: `prune_expired()` (Task 4), compute `eligible_peers()`
    (Task 6), select `min(fanout, eligible.size())` random peers from that
    set excluding self, call `exchange_with()` (Task 5) for each, sleep
    until the next `gossip_round_interval`
  - WHEN `eligible_peers()` is empty, the round is a no-op — logged at
    debug level, not an error (Requirement 4.2)
  - Verify: unit test with a short `gossip_round_interval` confirms
    multiple rounds execute within a bounded wall-clock window (loose
    timing assertion, not exact — matching how existing timer-driven tests
    in this project already tolerate scheduling jitter) and that
    `stop_gossip_thread()` joins cleanly with no leaked thread; a second
    test confirms rounds are silent no-ops before any `update_membership()`
    call
  - _Requirements: 4.1, 4.2, 4.3_

- [ ] 8. Implement `tcp_gossip_peer2peer_replicator`'s public API
  - Same file — constructor (starts listener then gossip thread; throws if
    the listener fails to bind, Requirement 4.4), destructor (stops gossip
    thread then listener), `advertise_progress()` (updates own table entry
    with a fresh `fresh_until`, resolves immediately), `find_catch_up_source()`
    (local-table read filtered by `_active_members`, excludes self, returns
    the first entry with `last_log_index >= from_index`), `update_membership()`
    (Task 6, exposed as part of the public API)
  - `static_assert(peer2peer_replicator<tcp_gossip_peer2peer_replicator<...>, ...>)`
  - Verify: the `static_assert` compiles; a unit test constructs one
    instance, calls `update_membership` then `advertise_progress` for a
    synthetic peer digest (bypassing the network path directly via a
    test-only accessor, or via a real second instance), confirms
    `find_catch_up_source` returns it correctly; a second test confirms
    `find_catch_up_source` returns `nullopt` for a digest present in the
    table but absent from the most recent `update_membership` call
  - _Requirements: 1.1, 1.2, 1.3, 1.4_

---

## Phase 4: Tests (Tasks 9–14)

- [ ] 9. Unit tests: merge and prune logic
  - `tests/tcp_gossip_transport_merge_unit_test.cpp` (new file) — pure
    logic, no network I/O: higher-term wins, equal-term-higher-index wins,
    lower-term/index loses, not-yet-present always added; pruning removes
    only past-`fresh_until` entries and leaves others untouched
  - Verify: `ctest -R tcp_gossip_transport_merge_unit_test` passes
  - _Requirements: 6.1, 6.2, 6.3_

- [ ] 10. Unit tests: eligible-peers intersection, fanout selection, and
       wire encode/decode
  - Same file or a sibling — `eligible_peers()`'s intersection correctness
    (Task 6's verify step promoted to a permanent regression test); fanout
    selection excludes self and respects `min(fanout, eligible.size())`
    across repeated draws (including the edge case
    `eligible.size() <= fanout`, where every eligible peer is selected
    every round); `encode_gossip_message`/`decode_gossip_message`
    round-trip (Task 1's verify step promoted to a permanent regression
    test here)
  - Verify: `ctest` passes
  - _Requirements: 2.3, 4.2, 5.1, 5.2_

- [ ] 11. Integration test: real TCP, single process, multiple instances
  - `tests/tcp_gossip_transport_integration_test.cpp` (new file) —
    the explicit anti-flakiness test from Requirement 10.2: several
    `tcp_gossip_peer2peer_replicator` instances constructed **in this one
    test process** on distinct loopback ports, each other in their
    `address_book`s, each instance's `update_membership()` called with the
    full instance set — NO `posix_spawn`, no subprocesses
  - Confirm a digest advertised on one instance becomes visible via
    `find_catch_up_source` on another within a small, bounded number of
    `gossip_round_interval`s
  - _Requirements: 10.2_

- [ ] 12. End-to-end property test: mixed transport
  - `tests/tcp_gossip_transport_catch_up_property_test.cpp` (new file) —
    a multi-node `node<Types>` cluster using
    `simulator_network_client`/`server` for Raft RPCs but real
    `tcp_gossip_peer2peer_replicator` instances for
    `peer2peer_replicator_type`
  - Reuse the depended-on spec's catch-up scenarios (new node with an
    empty log joining a cluster with existing history; a node that fell
    behind due to a simulated disconnect) — assert the same convergence
    properties hold with the real gossip transport as already verified
    with `static_peer2peer_replicator` in
    `.kiro/specs/peer2peer-log-replication/`. `update_membership()` is
    called automatically here by `node<Types>` itself (that spec's
    Requirement 11) — this test does not call it directly.
  - _Requirements: 10.3_

- [ ] 13. Freshness expiry test
  - Same file as Task 12, or a sibling — one instance stops calling
    `advertise_progress` (simulating a crash) while others continue
    gossiping normally
  - Assert: the silent instance's digest disappears from the other
    instances' tables after `freshness_interval` elapses, and
    `find_catch_up_source` no longer offers it as a source afterward
  - _Requirements: 10.4_

- [ ] 14. Membership-removal test (design.md Property 4)
  - Same file as Task 12, or a sibling — using the mixed-transport setup
    from Task 12, drive a `remove_server()` on the cluster
  - Assert: the removed node stops being offered by `find_catch_up_source`
    on every other instance immediately upon the next `update_membership()`
    call reflecting the removal (i.e. as soon as `node<Types>`'s own
    `cluster_members()`-driven sync fires — depended-on spec, Requirement
    11), even though the removed node's last-advertised digest may still
    be present in `_table` until `freshness_interval` separately elapses
  - This is the direct test of design.md's Property 4 — the guarantee that
    membership-eligibility and gossip-table staleness are independent axes
  - _Requirements: 1.4, 2.2, 2.3_

---

## Notes

- Tasks 1 and 2 have no dependencies on each other and are good candidates
  for parallel implementation.
- Task 6 (`update_membership()`/`eligible_peers()`) is deliberately
  independent of the gossip thread (Task 7) and listener (Task 3) — it's
  pure local-state logic over `tcp_gossip_config::address_book` plus
  whatever `update_membership()` has most recently supplied. Landing it
  early and testing it in isolation (Task 10) is low-risk, matching this
  project's general preference for extracting and testing pure logic ahead
  of the I/O-bound code that consumes it.
- Do not scope SWIM-style failure detection, delta/Merkle-compressed
  gossip payloads, live *address* discovery for brand-new members, or any
  production-binary CLI wiring into this spec's tasks — see design.md's
  Non-Goals. Each is a legitimate potential follow-on but would
  meaningfully expand this spec's safety and testing surface. Live
  *membership* tracking itself is already in scope (Task 6) — do not
  confuse the two when reviewing scope.
- Task 11's constraint (single-process, real-TCP, no subprocesses) is not
  optional stylistic preference — it is this spec's direct response to
  `ca_cluster_node_test.cpp` having been this project's dominant CI-flake
  source (real multi-process Raft clusters flaking under
  `ctest -j$(nproc)` CPU contention, fixed via `--repeat until-pass:3` and
  `PROCESSORS 4` rather than avoided at the design level). Do not restructure
  Task 11 or Task 12 into a subprocess-spawning test without first
  re-reading that history in `doc/TODO.md`'s Build Tooling section.
- Task 12's choice to keep Raft RPCs on the network simulator while gossip
  runs over real TCP is deliberate (design.md, Testing Strategy) — resist
  the temptation to "make it more realistic" by also switching
  `network_client_type`/`network_server_type` to a real transport in the
  same test; that reintroduces exactly the kind of compounded
  nondeterminism this spec's test design was written to avoid.
- Task 14 depends on the depended-on spec's Task 10
  (`.kiro/specs/peer2peer-log-replication/tasks.md`) already being
  implemented — `remove_server()` must actually be driving
  `update_membership()` calls for this test to exercise anything real.
