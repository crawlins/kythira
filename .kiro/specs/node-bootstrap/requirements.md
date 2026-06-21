# Requirements Document

## Introduction

This document specifies the requirements for bootstrapping a Kythira Raft node
that starts with empty persistence — either because it is being added to an
existing cluster for the first time, or because it is founding a new cluster.

Currently there is no in-band bootstrap mechanism. A node with empty persistence
starts, sets its configuration to `{self}`, and waits passively. It can never
join an existing cluster without an out-of-band call to `set_cluster_configuration()`,
which bypasses the Raft protocol and is only safe before the cluster starts. A
new node intended to join a running cluster has no way to do so at all.

The design introduces a `peer_discovery` concept as an abstract peer discovery
interface, a new `ClusterJoin` RPC pair, and a bootstrap flow integrated into
`node::start()`. All are optional — a node with no `peer_discovery` configured
(the default) behaves exactly as today.

## Glossary

- **Fresh node**: A node whose persistence engine contains no data — `current_term = 0`,
  `voted_for = nullopt`, no log entries, no snapshot. Distinguishable from a
  crashed-and-restarted node, which will have non-trivial persisted state.
- **Seed peer**: A cluster node whose network address is known ahead of time and
  used to bootstrap initial contact. Seed peers do not need to be the leader.
- **`peer_discovery`**: An abstraction that, given a timeout, returns a list of
  `peer_info` records (node ID + contact address) for seed peers.
- **`peer_info`**: A `(node_id, address)` pair returned by a `peer_discovery`.
- **Cluster join**: The full sequence by which a fresh node contacts a seed peer,
  locates the leader, and the leader executes `add_server()` on its behalf.
- **Cluster founding**: The degenerate case where a fresh node's `peer_discovery`
  returns no peers (or there is no `peer_discovery`). The node bootstraps as a
  single-node cluster and elects itself leader.
- **`no_op_peer_discovery`**: The default `peer_discovery` implementation that always
  returns an empty list, triggering cluster founding. Preserves existing
  single-node and test behaviour.

## Requirements

### Requirement 1: `peer_discovery` Concept

**User Story:** As a library user, I want a clean abstract interface for peer
discovery so I can plug in different discovery mechanisms (static config, DNS,
multicast, service registry) without changing the Raft node code.

#### Acceptance Criteria

1. A `peer_discovery<P, NodeId, Address>` concept SHALL require that `P` provides:
   - `typename P::node_id_type` equal to `NodeId`
   - `typename P::address_type` equal to `Address`
   - `find_peers(std::chrono::milliseconds timeout) -> kythira::Future<std::vector<peer_info<NodeId, Address>>>`
2. `peer_info<NodeId, Address>` SHALL be a struct with a `node_id` field and an
   `address` field, constructible from `(node_id, address)`.
3. The `peer_discovery` concept SHALL be defined in `include/raft/peer_discovery.hpp`
   alongside `peer_info` and `no_op_peer_discovery`.
4. `find_peers()` SHALL return a future that resolves with whatever peers have
   been discovered within `timeout`. Implementations that have no asynchronous
   work (e.g. `static_peer_discovery`) MAY resolve the future immediately.
   Implementations such as CoAP multicast that wait for responses over the
   network SHOULD resolve the future once the timeout elapses or all expected
   responses have arrived, whichever comes first.
5. Calling `find_peers()` multiple times SHALL be safe and MAY return different
   results on each call (discovery is stateless with respect to the caller).
6. The `peer_discovery<P, NodeId, Address>` concept SHALL require that `P`
   provides a method
   `register_node(NodeId self_id, Address self_address) -> kythira::Future<void>`
   that registers the owning node's identity with the discovery back-end.
   `register_node` MAY be called before the first `find_peers()` call and SHALL
   be idempotent (calling it again with the same arguments re-registers without
   error). Implementations that do not need this information MAY return an
   immediately-resolved future and otherwise ignore the arguments.

### Requirement 2: `no_op_peer_discovery` Default

**User Story:** As a developer using kythira today, I want the introduction of
`peer_discovery` to be entirely backwards-compatible so that existing code and tests
require no changes.

#### Acceptance Criteria

1. `no_op_peer_discovery<NodeId, Address>` SHALL satisfy `peer_discovery` and SHALL
   always return an empty vector from `find_peers()`.
2. `raft_types` SHALL gain a `peer_discovery_type` member that defaults to
   `no_op_peer_discovery<node_id_type, std::string>`.
3. WHEN `peer_discovery_type` is `no_op_peer_discovery` THEN bootstrap SHALL NOT
   attempt any network contact and the node SHALL found a single-node cluster
   exactly as it does today.
4. All 279 existing tests SHALL pass without modification after adding
   `peer_discovery_type` to `raft_types`.

### Requirement 3: Fresh Node Detection

**User Story:** As a Raft node starting up, I need to reliably determine whether
I am a fresh node (joining or founding) or a restarting node (recovering state)
so that I choose the correct startup path.

#### Acceptance Criteria

1. A node SHALL be considered fresh if and only if, after `initialize_from_storage()`,
   all of the following hold: `_current_term == 0`, `_voted_for` is `nullopt`,
   `get_last_log_index() == 0`, and no snapshot exists in the persistence engine.
2. A restarting node (any of the above conditions false) SHALL NOT enter the
   fresh-node bootstrap flow (Requirement 5). It MAY enter the reconnection
   flow (Requirement 9) if it cannot reach any known peer after starting.
3. The fresh-node check SHALL run inside `start()`, after `initialize_from_storage()`.

### Requirement 4: `ClusterJoin` RPC

**User Story:** As a fresh node, I need a protocol mechanism to request cluster
membership from an existing node so that I can join without out-of-band
coordination.

#### Acceptance Criteria

1. A `ClusterJoinRequest` message SHALL carry: the joining node's `node_id` and
   the joining node's contact `address` (so the leader can route `AppendEntries`
   back to it).
2. A `ClusterJoinResponse` message SHALL carry: `accepted` (bool), and — when
   `accepted` is false — a `redirect` field containing the leader's `peer_info`
   (node_id + address) if known, or `nullopt` if the contacted node does not
   know the current leader.
3. WHEN a node that IS the leader receives a `ClusterJoinRequest` THEN it SHALL
   call `add_server(req.node_id)` internally and return
   `ClusterJoinResponse{accepted: true}`.
4. WHEN a node that IS NOT the leader receives a `ClusterJoinRequest` THEN it
   SHALL return `ClusterJoinResponse{accepted: false, redirect: _known_leader}`.
5. The `network_client` concept SHALL gain a `send_cluster_join_request(address,
   request, timeout)` method. Note the routing key is `address_type`, not
   `node_id_type` — the new node does not yet know which node ID is at a given
   address.
6. The `network_server` concept SHALL gain a `register_cluster_join_handler`
   method accepting a handler with signature
   `ClusterJoinResponse(const ClusterJoinRequest&)`.
7. Existing `network_client` and `network_server` implementations
   (`simulator_network_client`, `simulator_network_server`) SHALL be extended
   to implement the new RPC methods.

### Requirement 5: Bootstrap Protocol

**User Story:** As a fresh node with a configured `peer_discovery`, I need an
automatic startup sequence that contacts seed peers, locates the leader, and
joins the cluster so that I begin participating without manual intervention.

#### Acceptance Criteria

1. WHEN a fresh node starts AND `peer_discovery.find_peers()` returns a non-empty
   list THEN the node SHALL attempt to join the cluster before entering its normal
   election-timer loop.
2. The join attempt SHALL:
   a. Try each `peer_info` in the returned list in order.
   b. Send a `ClusterJoinRequest` to the peer's address.
   c. If the response is `accepted: true`: wait for `AppendEntries` from the
      leader; the node is now a cluster member and proceeds normally.
   d. If the response is `accepted: false` with a redirect: retry once against
      the redirect target before moving to the next peer in the list.
   e. If a peer is unreachable or times out: move to the next peer.
3. WHEN all peers are exhausted without a successful join THEN the node SHALL
   wait for a configurable retry interval (`bootstrap_retry_interval`,
   default 5 s) and repeat the full `find_peers` + join sequence.
4. WHEN the join sequence succeeds AND the leader's `add_server()` replicates
   the configuration change THEN the node SHALL transition to normal follower
   operation without any further bootstrap-specific logic.
5. The bootstrap retry loop SHALL be cancellable: calling `stop()` while
   bootstrap is in progress SHALL terminate the loop cleanly.

### Requirement 6: Single-Node Cluster Founding

**User Story:** As the first node in a new cluster, I want to bootstrap as a
single-member cluster when no peers respond so that I become leader and am
ready to accept `add_server()` calls from subsequently joining nodes.

#### Acceptance Criteria

1. WHEN a fresh node starts AND `peer_discovery.find_peers()` returns an empty list
   THEN the node SHALL proceed as if `set_cluster_configuration({self})` had been
   called — i.e., it starts with its single-node configuration and participates
   in normal election logic (which will elect it leader immediately).
2. WHEN a fresh node starts with `no_op_peer_discovery` (the default) THEN it SHALL
   behave identically to the current behaviour (single-node cluster founding).
3. A founding node SHALL NOT require any new configuration beyond the existing
   constructor parameters.

### Requirement 7: `peer_discovery` Implementations

**User Story:** As a developer, I want concrete `peer_discovery` implementations
so I can use bootstrap in both test and real-network contexts.

#### Acceptance Criteria

1. `static_peer_discovery<NodeId, Address>` SHALL be provided in
   `include/raft/peer_discovery.hpp`. It is constructed with a fixed
   `std::vector<peer_info<NodeId, Address>>` and always returns that list from
   `find_peers()`. WHEN `register_node(self_id, self_address)` is called, it
   SHALL verify that a `peer_info` with `node_id == self_id` exists in the fixed
   list; if no such entry is found it SHALL throw `std::invalid_argument`.
2. `coap_multicast_peer_discovery` SHALL be provided in `include/raft/coap_transport.hpp`
   (alongside the existing CoAP client). It SHALL wrap the existing
   `coap_client::discover_raft_nodes()` multicast call and return the discovered
   nodes as `peer_info` records. The existing discovery implementation is reused
   unchanged; this class is a thin adaptor.
3. Both implementations SHALL satisfy the `peer_discovery` concept, verified by
   `static_assert`s in their respective headers.

> DNS-based implementations (`rfc1035_peer_discovery` and `rfc2136_ldns_discovery`)
> are specified in the [dns-peer-discovery spec](../dns-peer-discovery/requirements.md).

### Requirement 9: Reconnection via `peer_discovery` for Isolated Restarting Nodes

**User Story:** As a restarting node with valid persisted state that cannot reach
any of its previously known peers, I want to use the `peer_discovery` to discover
current peer addresses and re-establish contact with the cluster so that I resume
normal participation without operator intervention.

This covers the case where a node was a legitimate cluster member, restarts, and
finds that its peers are unreachable — because peer addresses changed, the node
was offline while the cluster topology shifted, or the cluster was simply moved.
Unlike a fresh node, this node is already in the committed cluster configuration
and does not need `add_server()` called on its behalf; it only needs to locate
the cluster so normal Raft replication can resume.

#### Acceptance Criteria

1. WHEN a restarting node starts AND a `peer_discovery` is configured AND the node
   has not received any `AppendEntries` or `RequestVote` message within a
   configurable isolation timeout (`peer_discovery_isolation_timeout`, default
   equal to `election_timeout * 2`) THEN the node SHALL call
   `peer_discovery.find_peers()` to discover current peer addresses.
2. WHEN `find_peers()` returns one or more `peer_info` records THEN the node
   SHALL update its internal peer address table so that subsequent RPCs route
   to the discovered addresses.
3. WHEN the peer address table is updated THEN the node SHALL continue normal
   follower/candidate operation — it SHALL NOT send a `ClusterJoinRequest`
   since it is already a cluster member.
4. WHEN the reconnection attempt does not restore contact (peers remain
   unreachable after address update) THEN the node SHALL repeat the
   `find_peers()` call after `bootstrap_retry_interval` until either contact
   is restored or `stop()` is called.
5. WHEN `find_peers()` returns an empty list THEN the node SHALL remain in
   its current state and retry after `bootstrap_retry_interval`. It SHALL NOT
   revert to single-node cluster behaviour — a restarting node must not
   silently form a split-brain cluster.
6. WHEN the node receives any valid `AppendEntries` or `RequestVote` from a
   known peer (before or after reconnection) THEN the reconnection loop SHALL
   stop immediately.
7. WHEN `peer_discovery_type` is `no_op_peer_discovery` THEN reconnection SHALL NOT
   be attempted (consistent with the backwards-compatibility requirement).

### Requirement 10: Node Registration at Startup

**User Story:** As a node starting up, I want my identity automatically registered
with the peer discovery back-end so that other nodes can discover me without any
additional call from application code.

#### Acceptance Criteria

1. A node SHALL call `peer_discovery.register_node(self_id, self_address)` inside
   `start()`, after `initialize_from_storage()` and before any call to
   `find_peers()` or the bootstrap / reconnection loops.
2. The `register_node` call applies to ALL nodes — both fresh nodes entering the
   bootstrap flow and restarting nodes entering the reconnection flow. It is not
   conditional on `is_fresh_node()`.
3. The future returned by `register_node` SHALL be awaited before proceeding;
   if it is rejected the node SHALL propagate the exception and abort startup.
4. WHEN `peer_discovery_type` is `no_op_peer_discovery` THEN `register_node`
   SHALL return an immediately-resolved future and impose no observable overhead,
   preserving the backwards-compatibility guarantee of Requirement 2.

### Requirement 8: Tests

**User Story:** As a developer, I need automated tests for the bootstrap flow so
that regressions are caught before they reach production.

#### Acceptance Criteria

1. A unit test SHALL verify that a fresh node with `no_op_peer_discovery` founds a
   single-node cluster (existing behaviour, confirmed to be unchanged).
2. A unit test SHALL verify that a fresh node with a `static_peer_discovery` pointing
   at a running single-node cluster joins successfully and reaches the same
   `last_applied` as the existing leader after a few commands.
3. A property test SHALL verify that when a fresh node's seed peers are all
   followers, the node follows redirects and eventually contacts the leader.
4. A property test SHALL verify that a fresh node retries if all seed peers are
   unreachable, and successfully joins once a peer becomes reachable.
5. A unit test SHALL verify that `stop()` called during the bootstrap retry loop
   terminates cleanly without hanging.
6. A property test SHALL verify that a restarting node that cannot reach its
   configured peers uses `peer_discovery` to discover updated addresses and
   resumes normal operation once contact is restored.
7. A property test SHALL verify that a restarting node that discovers updated
   peer addresses via `peer_discovery` does NOT send a `ClusterJoinRequest` and
   does NOT cause a second `add_server()` call on the leader.
8. A unit test SHALL verify that the reconnection loop stops immediately when
   a valid `AppendEntries` is received from any peer.
9. A unit test SHALL verify that `register_node` is called exactly once during
   `start()` for both a fresh node and a restarting node, and that startup is
   aborted if `register_node` returns a rejected future.
10. All 279 existing tests SHALL pass without modification.
