# Implementation Plan — Node Bootstrap

## Status: Complete

**Last Updated**: June 18, 2026

## Overview

Introduce a `peer_discovery` concept and a `ClusterJoin` RPC so that a fresh node
can locate an existing cluster and request membership automatically. Eight phases:
concept and types, RPC messages and serialization, network layer extensions,
node logic, CoAP adaptor, RFC 2136 DNS adaptor, tests, and backwards-compatibility
regression.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 2],
      "description": "Foundation: peer_discovery concept/types and ClusterJoin message types"
    },
    {
      "wave": 2,
      "tasks": [3, 4],
      "description": "Serialization and raft_types expansion (depend on wave 1 types)"
    },
    {
      "wave": 3,
      "tasks": [5, 6],
      "description": "Network client/server concept extensions and simulator implementations"
    },
    {
      "wave": 4,
      "tasks": [7, 8, 9, 10],
      "description": "Node logic: is_fresh_node, handle_cluster_join, run_bootstrap, run_reconnect + start() wiring"
    },
    {
      "wave": 5,
      "tasks": [11],
      "description": "Discovery adaptors: CoAP multicast (DNS adaptors tracked in dns-peer-discovery spec)"
    },
    {
      "wave": 6,
      "tasks": [12, 13, 14, 15, 16, 17],
      "description": "Property and unit tests (bootstrap + reconnection)"
    },
    {
      "wave": 7,
      "tasks": [18],
      "description": "Full regression: all 279 existing tests must still pass"
    }
  ]
}
```

## Tasks

---

## Phase 1: `peer_discovery` Concept and Message Types (Tasks 1–2)

### Define the abstract interfaces

- [x] 1. Create `include/raft/peer_discovery.hpp`
  - Define `peer_info<NodeId, Address>` struct with `node_id` and `address` fields
  - Define `peer_discovery<P, NodeId, Address>` concept requiring `node_id_type`,
    `address_type`, `register_node(NodeId, Address) -> Future<void>`,
    and `find_peers(milliseconds) -> Future<vector<peer_info>>`
  - Implement `no_op_peer_discovery<NodeId, Address>` — `register_node()` returns
    an immediately-resolved future and ignores its arguments; `find_peers()` always
    returns empty vector
  - Implement `static_peer_discovery<NodeId, Address>` — ctor takes
    `vector<peer_info> peers`; `register_node(self_id, self_address)` searches
    the fixed list for an entry with `node_id == self_id` and throws
    `std::invalid_argument` if not found, otherwise returns immediately-resolved
    future; `find_peers()` returns the fixed list
  - Add `static_assert`s confirming both types satisfy `peer_discovery`
  - Verify: `cmake --build build` (header-only, no link step needed)
  - _Requirements: 1.1–1.6, 2.1, 7.1_

- [x] 2. Add `cluster_join_request` and `cluster_join_response` to `include/raft/types.hpp`
  - `cluster_join_request<NodeId, Address>` with `joining_node_id()` and
    `joining_address()` accessors
  - `cluster_join_response<NodeId, Address>` with `is_accepted()` and
    `redirect_peer()` accessors
  - Add `address_type` and `peer_discovery_type` to `default_raft_types` (defaulting
    to `std::string` and `no_op_peer_discovery<uint64_t, string>` respectively)
  - Add corresponding type aliases to `default_raft_types`:
    `cluster_join_request_type` and `cluster_join_response_type`
  - Verify: `cmake --build build` still succeeds — existing code unaffected
  - _Requirements: 2.2, 4.1, 4.2_

---

## Phase 2: Serialization (Tasks 3–4)

### Extend `json_rpc_serializer` for ClusterJoin messages

- [x] 3. Add `raft_types` concept requirements for bootstrap types
  - Extend the `raft_types` concept in `include/raft/types.hpp` to require
    `typename T::address_type`, `typename T::peer_discovery_type`, and
    `typename T::cluster_join_request_type`, `typename T::cluster_join_response_type`
  - Require `peer_discovery<T::peer_discovery_type, T::node_id_type, T::address_type>`
  - Verify: all existing `test_raft_types` usages still compile (they inherit
    the defaults from `default_raft_types`)
  - _Requirements: 2.2, 2.4_

- [x] 4. Add `cluster_join_request` / `cluster_join_response` serialization to
  `include/raft/json_serializer.hpp`
  - `serialize(const cluster_join_request<>&)` → JSON with `"type"`,
    `"node_id"`, `"contact_address"` fields
  - `deserialize_cluster_join_request<>(data)` — round-trip inverse
  - `serialize(const cluster_join_response<>&)` → JSON with `"accepted"` and
    optional `"redirect_node_id"`, `"redirect_address"`
  - `deserialize_cluster_join_response<>(data)` — round-trip inverse
  - Add unit tests in `tests/node_bootstrap_unit_test.cpp` (new file):
    serialization round-trips for both message types, including the
    null-redirect and non-null-redirect cases
  - _Requirements: 4.1, 4.2_

---

## Phase 3: Network Layer Extensions (Tasks 5–6)

### Extend concepts and simulator implementations

- [x] 5. Extend `network_client` and `network_server` concepts in
  `include/raft/network.hpp`
  - Add `send_cluster_join_request(address, request, timeout)` to `network_client`
    — note: routed by `address_type`, not `node_id_type`
  - Add `register_cluster_join_handler(handler)` to `network_server`
  - Verify: the concept change causes a compile error on the simulator types
    (expected — they haven't been updated yet); fix in the next task
  - _Requirements: 4.5, 4.6_

- [x] 6. Implement `send_cluster_join_request` and `register_cluster_join_handler`
  in `include/raft/simulator_network.hpp`
  - `simulator_network_client::send_cluster_join_request(address, request, timeout)`:
    serialize request, create `message_type` addressed to `address` directly
    (not via node_id→address conversion), send, receive, deserialize response
  - `simulator_network_server`: add `_cluster_join_handler` member and
    `register_cluster_join_handler()` setter; add a fourth dispatch branch in
    `handle_message()` for `cluster_join_request`
  - Verify: `static_assert`s at the bottom of `simulator_network.hpp` confirm
    both types still satisfy `network_client` and `network_server`
  - _Requirements: 4.5, 4.6, 4.7_

---

## Phase 4: Node Bootstrap Logic (Tasks 7–9)

### Wire bootstrap into `node<Types>`

- [x] 7. Add `is_fresh_node()`, `_self_address`, and `_peer_discovery` to `node<Types>`
  - Add `address_type _self_address` and `peer_discovery_type _peer_discovery` private members
  - Extend the constructor to accept both as optional trailing parameters
    (default: `address_type{}` and `peer_discovery_type{}` respectively)
  - Implement `is_fresh_node()`: returns true iff `_current_term == 0`,
    `_voted_for == nullopt`, `get_last_log_index() == 0`, and no snapshot exists
  - Unit test: a freshly constructed node with empty `memory_persistence_engine`
    reports `is_fresh_node() == true`; a node with term=1 reports false
  - _Requirements: 3.1, 3.2_

- [x] 8. Implement `handle_cluster_join()` and register it as an RPC handler
  - Private method `handle_cluster_join(const cluster_join_request_type&) -> cluster_join_response_type`
  - If `_state == leader`: call `add_server(req.joining_node_id())` (fire-and-forget;
    result future discarded since the join response signals acceptance, not
    completion) and return `{accepted: true}`
  - If not leader: return `{accepted: false, redirect: _known_leader info if available}`
  - Add `_known_leader_address` field (type `std::optional<address_type>`) populated
    when this node processes an `AppendEntries` from a leader — extract from a new
    optional `leader_address` field in `append_entries_request`, or derive from a
    side channel; the simplest approach is to add `leader_address` as an optional
    field to `append_entries_request` (populated by the leader when sending)
  - Register in `register_rpc_handlers()`
  - Unit tests:
    - `handle_cluster_join()` on a leader calls `add_server()` and returns accepted
    - `handle_cluster_join()` on a follower with known leader returns redirect
    - `handle_cluster_join()` on a follower without known leader returns
      `{accepted: false, redirect: nullopt}`
  - _Requirements: 4.3, 4.4_

- [x] 9. Implement `run_bootstrap()` and call it from `start()`
  - `run_bootstrap()` implements the retry loop described in the design
  - Uses `_peer_discovery.find_peers(_config.bootstrap_peer_find_timeout())` where
    `bootstrap_peer_find_timeout()` is a new `raft_configuration` field
    (default: 2 s)
  - Add `bootstrap_retry_interval()` to `raft_configuration` (default: 5 s)
  - Add `_config.bootstrap_peer_find_timeout()` and
    `_config.bootstrap_retry_interval()` setters for testing
  - Cooperative cancellation: check `_stop_requested` (a new `std::atomic<bool>`
    that `stop()` sets before the existing teardown) at the top of each retry
  - In `start()`: after `initialize_from_storage()`, call
    `_peer_discovery.register_node(_self_id, _self_address).get()` (applies to
    all nodes, fresh and restarting); then if `is_fresh_node()` call
    `run_bootstrap()`, then proceed with `register_rpc_handlers()` etc.
  - Integration test (in `node_bootstrap_unit_test.cpp`): node with
    `no_op_peer_discovery` passes through `run_bootstrap()` with no network calls
  - _Requirements: 5.1–5.5, 6.1–6.3, 10.1–10.4_

- [x] 10. Implement `run_reconnect()` and `update_peer_addresses()`, wire into `start()`
  - Add `update_peer_addresses(const std::vector<peer_info<...>>&)` private method:
    iterates the list and calls `_network_client.update_peer_address(id, addr)` for
    each entry
  - Add `update_peer_address(node_id, address)` to the `network_client` concept
    and implement it on `simulator_network_client` (updates the internal routing
    table) — mark with a concept constraint so it is only required when
    `peer_discovery_type` is not `no_op_peer_discovery` (use `if constexpr` or a
    separate refined concept to avoid breaking existing clients)
  - Add `_peer_contact_received` condition variable and `_peer_contact_mutex`;
    signal it from `handle_append_entries()` and `handle_request_vote()` on any
    valid incoming RPC
  - Add `peer_discovery_isolation_timeout()` to `raft_configuration` defaulting to
    `election_timeout * 2`
  - Implement `run_reconnect()`: wait on `_peer_contact_received` for
    `isolation_timeout`; on timeout call `_peer_discovery.find_peers().get()`,
    call `update_peer_addresses()`, sleep `bootstrap_retry_interval`, repeat;
    exit on `_stop_requested`
  - In `start()`: after `initialize_from_storage()`, if NOT `is_fresh_node()`
    AND peer finder is not `no_op`, launch `run_reconnect()` as a background
    thread stored in `_reconnect_thread`; join it in `stop()`
  - Unit test: a restarting node (term > 0) starts `run_reconnect()`; on first
    incoming `AppendEntries`, verify the reconnect thread exits
  - _Requirements: 9.1–9.7_

---

## Phase 5: CoAP Multicast Peer Discovery (Task 11)

### Wrap existing discovery in a `peer_discovery`-conforming class

- [x] 11. Implement `coap_multicast_peer_discovery` in `include/raft/coap_transport.hpp`
  - Ctor accepts `(string self_id, string self_address, coap_client_type& client,
    string multicast_address, uint16_t multicast_port)`; stores `_self_id` and
    `_self_address` for use in future multicast advertisement
  - `find_peers(timeout)` calls `_client.discover_raft_nodes(addr, port, timeout).get()`
    and converts each returned node ID string to
    `peer_info<string, string>{node_id, node_id}`
  - Add `static_assert(peer_discovery<coap_multicast_peer_discovery, std::string, std::string>)`
  - Verify: no changes to `discover_raft_nodes()` itself — this is a pure adaptor
  - _Requirements: 7.2, 7.3_

---

## Phase 5b: DNS Peer Discovery

DNS-based `peer_discovery` implementations (`rfc1035_peer_discovery` and
`rfc2136_ldns_discovery`) are tracked in the
[dns-peer-discovery spec](../dns-peer-discovery/tasks.md).

---

## Phase 6: Tests (Tasks 12–17)

### Verify the bootstrap and reconnection flows end-to-end

- [x] 12. Unit tests for concepts and types (`tests/node_bootstrap_unit_test.cpp`)
  - `static_assert` that `no_op_peer_discovery` and `static_peer_discovery` satisfy
    `peer_discovery`
  - `is_fresh_node()` true on empty persistence, false after term/log/snapshot
  - `handle_cluster_join()` leader path (accepted), follower-with-leader path
    (redirect), follower-without-leader path (no redirect)
  - Serialization round-trips for both join message types
  - `stop()` during bootstrap loop terminates within `retry_interval + epsilon`
  - Restarting node starts `run_reconnect()`; receiving `AppendEntries` stops it
  - _Requirements: 8.1, 8.5, 8.8_

- [x] 13. Join property test (`tests/node_bootstrap_join_property_test.cpp`)
  - Start a 1-node simulator cluster; wait for it to elect a leader
  - Create a second node with `static_peer_discovery` pointing at the first node
  - Call `start()` on the second node
  - Assert: second node eventually sees `_configuration.nodes().size() == 2`
  - Submit 5 commands to the leader; assert both nodes reach `last_applied == 6`
    (5 commands + 1 C_new config entry)
  - _Requirements: 8.2_

- [x] 14. Redirect property test (`tests/node_bootstrap_redirect_property_test.cpp`)
  - Start a 3-node simulator cluster; wait for a leader to be elected
  - Create a fourth node with `static_peer_discovery` listing only the two followers
  - Call `start()` on the fourth node
  - Assert: the node follows the redirect and joins successfully (4-node cluster)
  - _Requirements: 8.3_

- [x] 15. Retry property test (`tests/node_bootstrap_retry_property_test.cpp`)
  - Start a 1-node cluster; disable all simulator edges to it
  - Create a second node with `static_peer_discovery` pointing at the first
  - Start the second node; verify it enters the retry loop (check logs/metrics)
  - Re-enable the simulator edge after `retry_interval * 1.5`
  - Assert: join eventually succeeds
  - _Requirements: 8.4_

- [x] 16. Reconnection property test
  (`tests/node_bootstrap_reconnection_property_test.cpp`)
  - Start a 3-node cluster; wait for stability
  - Simulate an address change: update the simulator routing so node 3's address
    changes; node 1 and node 2 have stale address for node 3
  - Stop and restart node 3 with a `static_peer_discovery` returning nodes 1 and 2
  - Assert: node 3 reconnects (verifiable via `run_reconnect()` calling
    `update_peer_addresses()` and cluster converging)
  - Assert: no `ClusterJoinRequest` was sent by node 3 (check server handler
    call counts)
  - Assert: `add_server()` was NOT called on the leader for node 3
  - _Requirements: 8.6, 8.7_

- [x] 17. Reconnection no-split-brain test
  (`tests/node_bootstrap_no_split_brain_test.cpp`)
  - Start a 3-node cluster; stop node 3 and configure `no_op_peer_discovery`
  - Restart node 3 with all peers unreachable AND `no_op_peer_discovery`
  - Assert: node 3 does NOT elect itself leader of a 1-node cluster
  - Assert: node 3 remains a follower with `_configuration.nodes()` unchanged
  - _Requirements: 9.5_

---

## Phase 7: Regression (Task 18)

- [x] 18. Full regression: all 279 existing tests pass
  - Run `ctest --output-on-failure`; confirm 0 failures
  - Constructor changes are backward-compatible (new parameters are optional)
  - `raft_types` concept additions default to `no_op_peer_discovery` / `std::string`
  - No existing test should need modification
  - _Requirements: 2.4, 6.2, 8.9_

---

## Notes

- Task 8 introduces a `leader_address` field in `AppendEntries`. This is the
  only schema change that touches an existing RPC message. Keep it optional
  (defaulting to empty string) so existing serialized messages remain valid.
  Alternatively, the `_known_leader_address` can be populated from a separate
  side channel (e.g., the source address of the incoming `AppendEntries`
  network message). The simpler of the two approaches should be chosen during
  implementation.
- The `send_cluster_join_request` routing by `address_type` rather than
  `node_id_type` is intentional — the joining node does not yet know its peers'
  node IDs. The simulator network implementation will need to handle messages
  addressed directly to an address string, which it already supports internally
  (the `_node->address()` is a string in the simulator).
- Task 10 (CoAP adaptor) can be done at any time after task 1 since it only
  depends on `peer_info` and the CoAP client; it does not require the node
  bootstrap logic to be complete.
- Do not reuse `set_cluster_configuration()` inside `run_bootstrap()`. That
  method bypasses the Raft protocol and is only safe before the cluster starts.
  The correct path is always through `add_server()` on the leader.
