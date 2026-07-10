# Implementation Plan — Peer-to-Peer Log Replication (Gossip Catch-Up)

## Status: Not Started

**Last Updated**: July 10, 2026

## Overview

Add an opt-in `peer2peer_replicator_type` extension to `node<Types>` that
lets a lagging cluster member pull missing log entries from another member
that already has them, instead of exclusively from the leader — addressing
the single-leader replication fan-out bottleneck for catch-up scenarios
(rolling restarts, healed partitions, bursty joins) without changing
steady-state replication, commit-index advancement, or election safety.
Defaults to a no-op that preserves today's leader-only behavior exactly. The
replicator's own notion of "who's in the cluster" tracks `_configuration` —
the replicated log's own core cluster membership — rather than separately
maintained configuration (Requirement 11).

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 3, 5, 6, 9],
      "description": "Independent foundations: peer2peer_replicator concept + no-op default, fetch_log_entries request/response types, raft_configuration new fields, network_client/server_with_log_fetch concepts, and the append_entries_with_consistency_check() extraction (a pure refactor of existing handle_append_entries() logic, no new types needed)"
    },
    {
      "wave": 2,
      "tasks": [2, 4, 7],
      "description": "static_peer2peer_replicator reference implementation (needs task 1's concept + peer_info), _peer2peer_replicator_type_traits fallback (pairs with task 1), simulator_network_client/server log-fetch support (needs task 6's concepts)"
    },
    {
      "wave": 3,
      "tasks": [8],
      "description": "node_config<Types>/node<Types> wiring — needs tasks 1, 4, 5 (types + fallback trait + config fields) all in place"
    },
    {
      "wave": 4,
      "tasks": [10],
      "description": "cluster_members() and its wiring into every _configuration assignment site — needs task 8's _peer2peer_replicator member to exist"
    },
    {
      "wave": 5,
      "tasks": [11, 12, 13],
      "description": "Core catch-up logic — handle_fetch_log_entries() responder, maybe_gossip_progress(), maybe_catch_up_from_peer(); each needs task 8's wiring and task 9's extracted consistency-check helper; independent of each other"
    },
    {
      "wave": 6,
      "tasks": [14, 15],
      "description": "Maintenance-thread wiring (needs 11-13) and metrics/logging (needs 11-13; independent of 14)"
    },
    {
      "wave": 7,
      "tasks": [16, 17, 18, 19, 20, 21],
      "description": "Test suite — depends on all implementation waves; the six test tasks are independent of each other"
    }
  ]
}
```

## Tasks

---

## Phase 1: Concept, Types, and Configuration (Tasks 1–7)

- [ ] 1. Add `peer2peer_replicator` concept and `no_op_peer2peer_replicator`
  - New file `include/raft/peer2peer_replication.hpp`, structured like
    `include/raft/peer_discovery.hpp`
  - `peer2peer_replicator<P, NodeId, Address, LogIndex>` concept requiring
    `advertise_progress(self_id, self_address, term, last_log_index) ->
    Future<void>`, `find_catch_up_source(from_index, to_index, timeout) ->
    Future<std::optional<peer_info<NodeId, Address>>>`, and
    `update_membership(member_ids) -> Future<void>` (Requirement 11 — see
    Task 10 for where `node<Types>` calls this)
  - `no_op_peer2peer_replicator<NodeId, Address, LogIndex>`:
    `advertise_progress`/`update_membership` resolve immediately;
    `find_catch_up_source` always resolves `std::nullopt`
  - `static_assert` both satisfy the concept, matching `peer_discovery.hpp`'s
    pattern
  - Verify: `cmake --build build` compiles the new header standalone
  - _Requirements: 1.1, 1.2, 1.3_

- [ ] 2. Add `static_peer2peer_replicator` reference implementation
  - Same file as Task 1
  - Backed by a `std::shared_ptr<folly::Synchronized<std::unordered_map<NodeId,
    progress_digest>>>` shared across every node instance in a test cluster,
    plus a per-instance `folly::Synchronized<std::unordered_set<NodeId>>`
    tracking this instance's own last-`update_membership`-supplied set
  - `advertise_progress` writes/overwrites this node's entry;
    `find_catch_up_source` returns the first peer, present in this
    instance's current membership set, whose gossiped
    `last_log_index >= from_index` (Requirement 11 — a peer removed from
    membership is never offered even if its stale digest lingers in the
    shared table); `update_membership` replaces the per-instance set
  - Verify: unit test constructs a shared table, two instances pointing at
    it, confirms `find_catch_up_source` sees the other instance's advertised
    progress; a third test confirms a node dropped via `update_membership`
    stops being offered even though its digest is still in the shared table
  - _Requirements: 9.1, 11.4_

- [ ] 3. Add `fetch_log_entries_request`/`fetch_log_entries_response` to
      `types.hpp`
  - `fetch_log_entries_request<NodeId, TermId, LogIndex>`:
    `requester_id`, `from_index`, `to_index`
  - `fetch_log_entries_response<TermId, LogIndex, LogEntry>`:
    `responder_id`, `available`, `prev_log_term`, `entries`
  - Private-member-plus-getter shape, matching
    `append_entries_request`/`response` exactly
  - Verify: `cmake --build build` compiles; a round-trip
    construct-then-read-all-accessors unit test
  - _Requirements: 5.1_

- [ ] 4. Add `_has_peer2peer_replicator_type<T>` /
      `_peer2peer_replicator_type_traits<T, NodeId, Address, LogIndex, bool>`
  - `types.hpp`, immediately alongside where `_has_bootstrap_types`/
    `_bootstrap_type_traits` live
  - Mirrors `_has_quorum_manager_type`/`_quorum_manager_type_traits`
    (`raft.hpp`) exactly: primary template (false case) resolves
    `peer2peer_replicator_type` to `no_op_peer2peer_replicator<NodeId,
    Address, LogIndex>`; `true` specialization resolves to `typename
    T::peer2peer_replicator_type`
  - Verify: a unit test instantiates the trait against a `Types` bundle
    without `peer2peer_replicator_type` and confirms it resolves to the
    no-op type; another bundle that does declare one resolves to that type
  - _Requirements: 2.1, 2.2_

- [ ] 5. Add new `raft_configuration` fields
  - `_progress_gossip_interval` (default 500ms), `_catch_up_gap_threshold`
    (default 50), `_catch_up_fetch_max_entries` (default 500),
    `_catch_up_fetch_timeout` (default 5000ms) — private member plus
    `field_name()` getter, `///<` doc comment, matching every existing
    `raft_configuration` field
  - Verify: `cmake --build build` compiles; defaults match the values
    specified here
  - _Requirements: 8.1, 8.2_

- [ ] 6. Add `network_client_with_log_fetch`/`network_server_with_log_fetch`
      to `network.hpp`
  - Mirrors `network_client_with_cluster_join`/
    `network_server_with_cluster_join` exactly — optional, NOT required by
    the base `network_client`/`network_server` concepts
  - `network_client_with_log_fetch`: `client.send_fetch_log_entries(target,
    req, timeout) -> Future<fetch_log_entries_response<>>`
  - `network_server_with_log_fetch`: `server.register_fetch_log_entries_handler(handler)
    -> void`
  - Verify: existing `simulator_network_client`/`server`,
    `tcp_rpc_client`/`server` still satisfy `network_client`/`network_server`
    unmodified (no regression in existing `static_assert`s)
  - _Requirements: 5.2_

- [ ] 7. Add log-fetch support to `simulator_network_client`/`server`
  - `include/raft/simulator_network.hpp` — needed so property tests
    (Phase 3) can exercise the full catch-up path without a real transport
  - `send_fetch_log_entries` routes to the target's registered handler
    through the same in-process registry `send_append_entries` already uses
  - `register_fetch_log_entries_handler` stores the handler alongside the
    existing three
  - Verify: `static_assert(network_client_with_log_fetch<simulator_network_client<...>>)`
    and the server equivalent
  - _Requirements: 5.2, 9.3_

---

## Phase 2: `node<Types>` Wiring, Consistency-Check Extraction, and Membership Sync (Tasks 8–10)

- [ ] 8. Wire `peer2peer_replicator_type` into `node_config<Types>`/`node<Types>`
  - `node_config<Types>`: add `peer2peer_replicator_type peer2peer_replicator{}`
    optional field (resolved via Task 4's trait), after the existing
    `quorum_manager` field
  - `node<Types>`: add `peer2peer_replicator_type` alias (via the trait),
    `_peer2peer_replicator` member, accept it via `node_config<Types>` and
    as a trailing defaulted parameter on the legacy positional constructor —
    mirror exactly how `peer_discovery` was added to both constructors
  - Verify: every existing test file constructing `node<Types>`/
    `node_config<Types>` compiles unmodified (Requirement 2.5's regression
    gate — full check happens in Task 20, but this task's own build must not
    break anything already compiling)
  - _Requirements: 2.3, 2.4_

- [ ] 9. Extract `append_entries_with_consistency_check()` from
      `handle_append_entries()`
  - `raft.hpp` — move Rules 3–4 (prevLogIndex/prevLogTerm consistency check,
    conflict detection, truncation, append) out of `handle_append_entries()`
    into a private method taking `(prev_log_index, prev_log_term, entries)`
    and returning `append_entries_response_type`; `handle_append_entries()`
    calls it after its own Rules 1–2 (term check, become-follower,
    `_known_leader`/election-timer bookkeeping) and before its own
    post-check `leader_commit` handling
  - Pure refactor — no behavioral change
  - Verify: the entire existing `handle_append_entries`-exercising test
    suite (unit + property, e.g. `raft_state_machine_safety_property_test`,
    `raft_election_safety_property_test`, and every membership-change test)
    passes unmodified after the extraction, byte-for-byte same outcomes
  - _Requirements: 6.1_

- [ ] 10. Implement `cluster_members()` and wire membership synchronization
  - `raft.hpp` — `cluster_members() const -> std::vector<node_id_type>`:
    `_configuration.nodes()` unioned with `_configuration.old_nodes().value()`
    when `_configuration.is_joint_consensus()` is true; never includes
    `_configuration.learners()` (Requirement 11.2)
  - Add a small private helper, e.g. `sync_peer2peer_membership()`, calling
    `_peer2peer_replicator.update_membership(cluster_members())`
    fire-and-forget (result `Future` not waited on)
  - Call `sync_peer2peer_membership()` immediately after every existing
    `_configuration = ...` assignment in `raft.hpp`: `add_server()`,
    `remove_server()`, both `apply_committed_entries()` config-entry sites,
    `handle_append_entries()`'s follower-adopts-config path,
    `install_snapshot()`, `initialize_from_storage()`, and the
    truncation-revert path — eight call sites total as of this writing
    (design.md, Component 6)
  - Verify: a unit test using `static_peer2peer_replicator` drives
    `add_server()`/`remove_server()` against a `node<Types>` instance and
    confirms `update_membership` is called with the expected core-membership
    set (including the `is_joint_consensus()` union with `old_nodes()`) at
    each transition; a second test confirms a learner never appears in any
    call's argument
  - _Requirements: 11.1, 11.2, 11.3, 11.5_

---

## Phase 3: Catch-Up Logic (Tasks 11–15)

- [ ] 11. Implement `handle_fetch_log_entries()` (responder side)
  - `raft.hpp` — WHEN this node has an entry at `request.from_index()`,
    respond `available = true` with `prev_log_term` (the term at
    `from_index - 1`, or `0` if `from_index == 1`) and entries up to
    `min(request.to_index(), get_last_log_index())`; otherwise respond
    `available = false` with an empty `entries`
  - Registered via `register_fetch_log_entries_handler` only WHEN
    `network_server_with_log_fetch<network_server_type>` (`if constexpr`,
    matching the existing `network_server_with_cluster_join` dispatch)
  - Verify: unit test against a node with a populated log — request a
    sub-range, confirm exact entries and `prev_log_term` returned; request a
    range starting before the log's first entry (simulating post-compaction),
    confirm `available = false`
  - _Requirements: 5.4, 5.5_

- [ ] 12. Implement `maybe_gossip_progress()`
  - `raft.hpp` — calls `_peer2peer_replicator.advertise_progress(_node_id,
    self_address, _current_term, get_last_log_index())`, gated by
    `_config.progress_gossip_interval()` so it doesn't fire every
    maintenance tick; runs regardless of `server_state`
  - Failure logged at debug level, never retried outside the normal gossip
    cadence, never blocks any other operation
  - Verify: unit test with `static_peer2peer_replicator` — confirm a node's
    progress becomes visible to another instance sharing the same table
    after one gossip call, and that calling it below the configured
    interval a second time is a no-op (no duplicate advertisement burst)
  - _Requirements: 3.1, 3.2, 3.3_

- [ ] 13. Implement `maybe_catch_up_from_peer()`
  - `raft.hpp` — computes the catch-up gap (highest known
    `last_log_index` from any progress digest minus `get_last_log_index()`);
    WHEN it exceeds `_config.catch_up_gap_threshold()` AND no fetch is
    already outstanding (single-flight guard) AND this node is not the
    leader: calls `find_catch_up_source(from_index, to_index, timeout)`
    (`to_index` capped by `catch_up_fetch_max_entries()`)
  - WHEN a source is found and `network_client_with_log_fetch<network_client_type>`:
    sends `fetch_log_entries`, applies the response via Task 9's
    `append_entries_with_consistency_check()` using `prev_log_index =
    from_index - 1`, `prev_log_term = response.prev_log_term()`
  - WHEN no source is found, or the transport doesn't support log-fetch, or
    the RPC fails, or the consistency check rejects the batch: no-op this
    tick, clear the single-flight guard, re-evaluated next tick
  - Verify: unit test with `static_peer2peer_replicator` and
    `simulator_network_*` — a node behind another instance in the shared
    table successfully catches up its log via the fetch path; a second unit
    test confirms a leader never triggers this for itself
  - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 6.2, 6.3, 7.1, 7.2_

- [ ] 14. Wire `maybe_gossip_progress()`/`maybe_catch_up_from_peer()` into the
      maintenance-thread tick
  - `raft.hpp` — same tick loop already calling
    `check_heartbeat_timeout()`/`check_election_timeout()`
  - Verify: a running simulator-based node calls both on schedule
    (observable via the debug logging added in Task 15) without disrupting
    existing heartbeat/election timing
  - _Requirements: 3.1, 4.1_

- [ ] 15. Add metrics and logging
  - `raft_peer_catch_up_attempt`/`raft_peer_catch_up_success` metrics
    (dimensioned by `node_id`, `source_peer`), following the existing
    `_metrics.set_metric_name(...)`/`add_dimension(...)`/`add_one()`/`emit()`
    pattern
  - Debug-level structured logging for gap detection, source discovery,
    fetch attempts, and every outcome (success, `available = false`,
    consistency-check failure, timeout), matching existing
    `AppendEntries`/`InstallSnapshot` logging verbosity and field style
  - Verify: log output inspected manually during Task 13's unit tests shows
    the expected sequence of debug lines for both a successful and a failed
    catch-up attempt
  - _Requirements: 10.1, 10.2_

---

## Phase 4: Tests (Tasks 16–21)

- [ ] 16. Unit tests for concept, no-op/static implementations, and the
      consistency-check extraction
  - `tests/peer2peer_replication_unit_test.cpp` (new file): concept
    satisfaction (`static_assert`), `no_op_peer2peer_replicator` always
    resolving as specified, `static_peer2peer_replicator` table and
    membership-filtering behavior
  - Confirms Task 9's extraction changed no behavior: existing
    `handle_append_entries`-related test files require zero modification
  - Verify: `ctest -R peer2peer_replication_unit_test` passes
  - _Requirements: 1.3, 9.1_

- [ ] 17. Property test: new node catches up via a peer, not the leader
  - `tests/peer2peer_catch_up_new_node_property_test.cpp` (new file) — using
    the network simulator with `static_peer2peer_replicator` shared across
    all nodes
  - Start a 3-node cluster with substantial committed history, add a 4th
    node with an empty log and `peer2peer_replicator_type` configured
  - Assert: the new node's log converges to match the cluster; assert the
    *leader's* `raft_peer_catch_up_attempt`/replication-round metrics for
    that node stay low relative to a control run with the no-op default
    (demonstrating the leader did comparatively little of the catch-up work)
  - _Requirements: 9.3_

- [ ] 18. Property test: partition-reconnect catch-up via a peer
  - `tests/peer2peer_catch_up_partition_reconnect_property_test.cpp` (new
    file)
  - Start a 3-node cluster, partition one follower away long enough for the
    other two to advance well past the configured gap threshold, heal the
    partition
  - Assert: the reconnected follower's log converges, exercising the
    catch-up path rather than waiting on however many `AppendEntries` round
    trips the leader alone would need
  - _Requirements: 9.3_

- [ ] 19. Safety property test: stale/incorrect source peer still converges
      correctly
  - `tests/peer2peer_catch_up_stale_source_safety_property_test.cpp` (new
    file) — the direct test of design.md's Property 3
  - Construct a scenario where a lagging node fetches from a source peer
    whose log is later shown to diverge from the real leader's (simulate the
    source having briefly been on an abandoned term whose tail gets
    truncated)
  - Assert: the fetching node's log ends up matching the real leader's log
    after subsequent normal heartbeats — identical outcome to if no
    peer-to-peer fetch had occurred; no crash, no stuck state, no operator
    intervention required
  - _Requirements: 6.4, 6.5_

- [ ] 20. Full regression and no-op-default equivalence
  - Run `ctest --output-on-failure` — confirm 0 failures across the entire
    existing suite with zero modifications to any pre-existing `Types`
    bundle or test file (Requirement 2.5)
  - Add one property test explicitly diffing a cluster run with
    `peer2peer_replicator_type` present-but-configured-as-no-op against a
    `Types` bundle that never declares the field at all — assert identical
    committed log contents and identical leader-driven replication metrics
    (Requirement 1.4/4.4/Property 1 in design.md)
  - _Requirements: 2.5, 7.3_

- [ ] 21. Property test: membership synchronization (Property 6)
  - `tests/peer2peer_catch_up_membership_sync_property_test.cpp` (new file)
  - Using the network simulator and `static_peer2peer_replicator`: drive an
    `add_server()` on a running cluster and confirm the new node becomes
    catch-up-eligible at the same point its configuration entry takes effect
    for every other purpose; drive a `remove_server()` and confirm the
    removed node stops being offered as a catch-up source immediately, even
    though its last-advertised digest may still be present in the shared
    table
  - Assert: a learner attached to the cluster (`.kiro/specs/non-voting-nodes/`)
    is never offered as a catch-up source, regardless of how far its log has
    advanced (Requirement 11.2)
  - Assert: during a joint-consensus window (`is_joint_consensus() == true`),
    both a C_old member being removed and a C_new member being added are
    catch-up-eligible simultaneously (Requirement 11.1's union)
  - _Requirements: 11.1, 11.2, 11.3, 11.5_

---

## Notes

- Tasks 1, 3, 5, 6, and 9 have no dependencies on each other and are good
  candidates for parallel implementation.
- Task 9 (the `append_entries_with_consistency_check()` extraction) is
  low-risk and high-value to land early and in isolation — it changes no
  behavior by itself, and every later task in Phase 3 depends on it existing
  cleanly. Consider landing it as its own pull request ahead of the rest of
  this spec, verified purely by "the existing test suite still passes."
- `static_peer2peer_replicator` (Task 2) is intentionally the *only*
  `peer2peer_replicator_type` implementation this spec ships. Do not scope a
  real network-transport gossip implementation into this spec's tasks — see
  design.md's Non-Goals and Requirement 9.2; that is deliberately a
  follow-on spec (`.kiro/specs/peer2peer-gossip-transport/`), mirroring
  `node-bootstrap` → `dns-peer-discovery`.
- Task 13's single-flight guard and Task 8's constructor wiring touch the
  same class (`node<Types>`) but different concerns (state vs. construction)
  — implement Task 8 first so Task 13 has a member to guard.
- Task 10's eight call sites are a snapshot as of this writing — if a future
  change adds a ninth place `_configuration` is assigned, it should also
  gain a `sync_peer2peer_membership()` call. There is deliberately no single
  choke point (e.g. a private setter) enforcing this mechanically in this
  spec's scope; consider whether introducing one is worthwhile once a third
  or fourth consumer of "notify me when `_configuration` changes" exists
  (this spec is the second, after `_config_synchronizer`).
- When adding the new RPC's JSON wire format (used implicitly by
  `tcp_rpc_client`/`server` if a later spec extends those transports beyond
  the simulator), follow `json_serializer.hpp`'s existing
  `append_entries_request`/`response` (de)serialization as the template —
  not part of this spec's own tasks (Task 7 only wires the simulator
  transport), but worth noting for whoever picks up real-transport support.
