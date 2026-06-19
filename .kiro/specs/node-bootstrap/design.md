# Design Document

## Overview

This document describes the design for bootstrapping a fresh Kythira Raft node.
The centerpiece is a `peer_discovery` concept that abstracts peer discovery, a new
`ClusterJoin` RPC pair for the initial membership request, and a bootstrap flow
wired into `node::start()`. All additions are opt-in: the default `no_op_peer_discovery`
preserves existing behaviour exactly.

## Architecture

```
node::start()
  │
  ├── initialize_from_storage()
  │
  ├── is_fresh_node?
  │     ├── yes ──► run_bootstrap()       (join or found a new cluster)
  │     └── no  ──► run_reconnect()       (background; exits on first peer contact)
  │
  └── register_rpc_handlers()  (including ClusterJoin handler)

run_bootstrap():
  peer_discovery.find_peers(timeout).get()
  │
  ├── empty ──► found single-node cluster (existing behaviour)
  │
  └── [peer_info, ...]
            │
            ▼
      bootstrap_loop():
        for each peer_info p:
          send_cluster_join_request(p.address, {self_node_id, self_address})
          ├── accepted=true  ──► return  (AppendEntries from leader will follow)
          └── accepted=false
                ├── redirect known ──► retry against redirect (once)
                └── no redirect    ──► try next peer
        all peers exhausted ──► sleep(retry_interval) → repeat
  (cancellable via _stop_requested)

run_reconnect():   [restarting node only; runs as background thread]
  wait isolation_timeout for any incoming AppendEntries / RequestVote
  │
  ├── message received ──► exit (normal replication will proceed)
  │
  └── timeout elapsed
        peer_discovery.find_peers(timeout).get()
        │
        ├── empty     ──► sleep(retry_interval) → repeat
        └── [peer_info, ...]
              │
              update_peer_addresses(peer_infos)   ← updates network client table
              sleep(retry_interval) → repeat until peer contact or stop()

Any existing node (receiving ClusterJoinRequest):
  ├── if leader:  add_server(req.node_id) → respond accepted=true
  └── if follower/candidate: respond accepted=false, redirect=_known_leader
```

## Components and Interfaces

### 1. `include/raft/peer_discovery.hpp` — new file

#### `peer_info<NodeId, Address>`

```cpp
template<typename NodeId, typename Address>
struct peer_info {
    NodeId  node_id;
    Address address;
};
```

#### `peer_discovery` concept

```cpp
template<typename P, typename NodeId, typename Address>
concept peer_discovery =
    requires(P& finder, std::chrono::milliseconds timeout,
             NodeId self_id, Address self_address) {
        typename P::node_id_type;
        typename P::address_type;
        requires std::same_as<typename P::node_id_type, NodeId>;
        requires std::same_as<typename P::address_type, Address>;
        { finder.register_node(self_id, self_address) }
            -> std::same_as<kythira::Future<void>>;
        { finder.find_peers(timeout) }
            -> std::same_as<kythira::Future<std::vector<peer_info<NodeId, Address>>>>;
    };
```

#### `no_op_peer_discovery<NodeId, Address>`

```cpp
template<typename NodeId, typename Address>
class no_op_peer_discovery {
public:
    using node_id_type  = NodeId;
    using address_type  = Address;

    auto register_node(NodeId, Address) -> kythira::Future<void> {
        return kythira::FutureFactory::makeImmediateFuture<void>();
    }

    auto find_peers(std::chrono::milliseconds) const
        -> kythira::Future<std::vector<peer_info<NodeId, Address>>> {
        return kythira::FutureFactory::makeImmediateFuture<
            std::vector<peer_info<NodeId, Address>>>({});
    }
};
```

#### `static_peer_discovery<NodeId, Address>`

```cpp
template<typename NodeId, typename Address>
class static_peer_discovery {
public:
    using node_id_type  = NodeId;
    using address_type  = Address;

    explicit static_peer_discovery(std::vector<peer_info<NodeId, Address>> peers)
        : _peers(std::move(peers)) {}

    auto register_node(NodeId self_id, Address) -> kythira::Future<void> {
        auto it = std::ranges::find(_peers, self_id, &peer_info<NodeId, Address>::node_id);
        if (it == _peers.end()) {
            throw std::invalid_argument(
                "static_peer_discovery: self_id not found in fixed peers list");
        }
        return kythira::FutureFactory::makeImmediateFuture<void>();
    }

    auto find_peers(std::chrono::milliseconds) const
        -> kythira::Future<std::vector<peer_info<NodeId, Address>>> {
        return kythira::FutureFactory::makeImmediateFuture<
            std::vector<peer_info<NodeId, Address>>>(_peers);
    }

private:
    std::vector<peer_info<NodeId, Address>> _peers;
};
```

`static_assert`s for both types appear at the bottom of the header.

### 2. `include/raft/types.hpp` — additions

#### `cluster_join_request` / `cluster_join_response` message types

```cpp
template<typename NodeId = std::uint64_t, typename Address = std::string>
struct cluster_join_request {
    NodeId  node_id;
    Address contact_address;

    [[nodiscard]] auto joining_node_id() const -> NodeId { return node_id; }
    [[nodiscard]] auto joining_address() const -> const Address& { return contact_address; }
};

template<typename NodeId = std::uint64_t, typename Address = std::string>
struct cluster_join_response {
    bool accepted;
    std::optional<peer_info<NodeId, Address>> redirect;

    [[nodiscard]] auto is_accepted() const -> bool { return accepted; }
    [[nodiscard]] auto redirect_peer() const
        -> const std::optional<peer_info<NodeId, Address>>& { return redirect; }
};
```

#### `address_type` in `raft_types` and `default_raft_types`

Add to the `raft_types` concept:

```cpp
typename T::address_type;
typename T::peer_discovery_type;
requires peer_discovery<typename T::peer_discovery_type,
                     typename T::node_id_type,
                     typename T::address_type>;
```

Add to `default_raft_types`:

```cpp
using address_type      = std::string;
using peer_discovery_type  = no_op_peer_discovery<node_id_type, address_type>;
```

Add to `default_raft_types` the new RPC message type aliases:

```cpp
using cluster_join_request_type  =
    cluster_join_request<node_id_type, address_type>;
using cluster_join_response_type =
    cluster_join_response<node_id_type, address_type>;
```

### 3. `include/raft/network.hpp` — extend concepts

Add to `network_client`:

```cpp
// send_cluster_join_request routes by address (not node_id) because
// the caller does not yet know the target's node_id
{
    client.send_cluster_join_request(
        address,                          // address_type
        cluster_join_request<>{},         // request
        timeout)
} -> std::same_as<kythira::Future<kythira::cluster_join_response<>>>;
```

Add to `network_server`:

```cpp
{
    server.register_cluster_join_handler(
        std::function<kythira::cluster_join_response<>(
            const kythira::cluster_join_request<>&)>{})
} -> std::same_as<void>;
```

### 4. `include/raft/simulator_network.hpp` — extend implementations

`simulator_network_client` gains:

```cpp
auto send_cluster_join_request(const address_type& target,
                                const kythira::cluster_join_request<>& req,
                                std::chrono::milliseconds timeout)
    -> kythira::Future<kythira::cluster_join_response<>>;
```

This follows the same send/receive pattern as the existing three RPC methods.

`simulator_network_server` gains:

```cpp
auto register_cluster_join_handler(
    std::function<kythira::cluster_join_response<>(
        const kythira::cluster_join_request<>&)> handler) -> void;
```

The `handle_message()` type-dispatch logic gains a fourth branch for
`ClusterJoinRequest`.

### 5. `include/raft/raft.hpp` — bootstrap integration

#### `node<Types>` new type aliases

```cpp
using address_type              = typename Types::address_type;
using peer_discovery_type          = typename Types::peer_discovery_type;
using cluster_join_request_type = typename Types::cluster_join_request_type;
using cluster_join_response_type= typename Types::cluster_join_response_type;
```

#### Constructor

The constructor gains an optional `address_type self_address` parameter (needed
so the node knows what contact address to advertise in `ClusterJoinRequest`):

```cpp
node(node_id_type node_id,
     network_client_type network_client,
     network_server_type network_server,
     persistence_engine_type persistence,
     state_machine_type state_machine,
     logger_type logger,
     metrics_type metrics,
     membership_manager_type membership,
     configuration_type config,
     address_type self_address = address_type{},   // new, optional
     peer_discovery_type peer_discovery = peer_discovery_type{})  // new, optional
```

#### `is_fresh_node()` private helper

```cpp
auto is_fresh_node() const -> bool {
    return _current_term == 0
        && !_voted_for.has_value()
        && _persistence.get_last_log_index() == 0
        && !_persistence.load_snapshot().has_value();
}
```

Called inside `start()` after `initialize_from_storage()`.

#### `update_peer_addresses()` private method

The `network_client` concept gains an optional `update_peer_address` method:

```cpp
// In network_client concept (optional — only required when peer_discovery is used)
{ client.update_peer_address(node_id, address) } -> std::same_as<void>;
```

`simulator_network_client` implements this by updating its internal
`node_id → address` routing table. HTTP and CoAP clients would update their
connection pools. The node calls this method for each `peer_info` returned by
`find_peers()` during reconnection.

#### `run_reconnect()` private method

```cpp
auto run_reconnect() -> void;
```

Runs as a detached background thread started from `start()` when the node is
not fresh. The thread:

1. Waits up to `_config.peer_discovery_isolation_timeout()` for a notification
   that a valid RPC has been received. The notification comes via a
   `_peer_contact_received` condition variable that the RPC handlers signal
   on any valid `AppendEntries` or `RequestVote`.
2. If notified: thread exits (peer contact established, normal operation).
3. If timeout elapses: calls `_peer_discovery.find_peers(...).get()`, passes each
   result to `update_peer_addresses()`, then sleeps `bootstrap_retry_interval`
   and repeats from step 1.
4. Cooperative stop: checks `_stop_requested` before each sleep.

The thread is joined in `stop()` before the network server is stopped.

A new `raft_configuration` field `peer_discovery_isolation_timeout` defaults to
`election_timeout * 2` — long enough that a slow-starting but reachable cluster
doesn't spuriously trigger reconnection, but short enough to react quickly to a
genuine address change.

#### `run_bootstrap()` private method

```cpp
auto run_bootstrap() -> void;
```

Contains the retry loop. Runs synchronously inside `start()` (before
`_running = true`), with a cooperative cancellation check on `_running` so
that `stop()` can interrupt it. The loop:

1. Calls `_peer_discovery.find_peers(_config.bootstrap_peer_find_timeout())`
2. If empty: returns immediately (single-node founding path)
3. For each `peer_info`: sends `ClusterJoinRequest`, processes response
4. On `accepted=true`: returns (node is now joining via AppendEntries)
5. On redirect: retries against the redirect target once
6. On all-fail: sleeps `_config.bootstrap_retry_interval()` then loops

#### `start()` changes

```cpp
auto node<Types>::start() -> void {
    initialize_from_storage();

    _peer_discovery.register_node(_self_id, _self_address).get();

    if (is_fresh_node()) {
        run_bootstrap();       // no-op if peer_discovery returns empty
    }

    register_rpc_handlers();   // now includes ClusterJoin handler
    _network_server.start();
    _last_heartbeat = std::chrono::steady_clock::now();
    _running.store(true, std::memory_order_release);
}
```

#### ClusterJoin handler registration

In `register_rpc_handlers()`:

```cpp
_network_server.register_cluster_join_handler(
    [this](const cluster_join_request_type& req) -> cluster_join_response_type {
        return handle_cluster_join(req);
    });
```

#### `handle_cluster_join()` private method

```cpp
auto handle_cluster_join(const cluster_join_request_type& req)
    -> cluster_join_response_type {
    std::lock_guard<std::mutex> lock(_mutex);

    if (_state == server_state::leader) {
        // Fire-and-forget: add_server() runs asynchronously; the joining
        // node learns it was accepted and waits for AppendEntries to flow.
        add_server_locked(req.joining_node_id());   // internal, already holds lock
        return cluster_join_response_type{true, std::nullopt};
    }

    // Not leader: redirect if we know who the leader is
    std::optional<peer_info<node_id_type, address_type>> redirect;
    if (_known_leader.has_value()) {
        redirect = peer_info<node_id_type, address_type>{
            *_known_leader, _known_leader_address};
    }
    return cluster_join_response_type{false, redirect};
}
```

A new `_known_leader_address` field tracks the last known leader's contact
address (populated when a node receives an `AppendEntries` from the leader and
the leader's address is embedded in the message, or via a new optional field in
`AppendEntries`). This is a minor addition to `AppendEntries` or a separate
side-channel; the exact mechanism is an implementation detail.

### 6. `include/raft/coap_transport.hpp` — `coap_multicast_peer_discovery`

A thin adaptor class in `include/raft/coap_transport.hpp`:

```cpp
class coap_multicast_peer_discovery {
public:
    using node_id_type  = std::string;
    using address_type  = std::string;

    explicit coap_multicast_peer_discovery(
        coap_client_type& client,
        std::string multicast_address = "224.0.1.187",
        std::uint16_t multicast_port  = 5683)
        : _client(client),
          _multicast_address(std::move(multicast_address)),
          _multicast_port(multicast_port) {}

    auto register_node(std::string self_id, std::string self_address)
        -> kythira::Future<void> {
        _self_id      = std::move(self_id);
        _self_address = std::move(self_address);
        return kythira::FutureFactory::makeImmediateFuture<void>();
    }

    auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<peer_info<std::string, std::string>>>;

private:
    std::string       _self_id;
    std::string       _self_address;
    coap_client_type& _client;
    std::string       _multicast_address;
    std::uint16_t     _multicast_port;
};
```

`find_peers()` calls `_client.discover_raft_nodes(_multicast_address, _multicast_port, timeout).get()`
and converts each returned string to a `peer_info{node_id, node_id}` (using the
node ID as both the identifier and the address, matching the existing CoAP
transport convention).

### 7. `include/raft/rfc1035_peer_discovery.hpp` — RFC 1035 DNS query

Provides only the `find_peers` half of the `peer_discovery` concept. Does not
implement `register_node` and therefore does NOT satisfy the full concept. It is
a building block consumed by `rfc2136_ldns_discovery` and can also be composed
into other implementations that handle registration via a different mechanism.

#### Record convention

The shared DNS name carries one A record (IPv4) or AAAA record (IPv6) per live
cluster node, forming a multi-entry RRSET:

```
raft.cluster.example.com.  30  IN  A     10.0.0.1
raft.cluster.example.com.  30  IN  A     10.0.0.2
raft.cluster.example.com.  30  IN  AAAA  fd00::3
```

`find_peers()` queries both record types and merges the results, returning all
IP addresses without self-filtering (callers apply their own filter if needed).

#### `rfc1035_peer_discovery::config`

```cpp
struct config {
    std::string   server;       // DNS server IP or hostname
    std::uint16_t port{53};
    std::string   shared_name;  // e.g. "raft.cluster.example.com."
};
```

#### Class sketch

```cpp
class rfc1035_peer_discovery {
public:
    explicit rfc1035_peer_discovery(config cfg);

    // Issues A and AAAA queries for shared_name; returns all records as
    // peer_info{ip, ip}. Does not filter out any particular address.
    auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<peer_info<std::string, std::string>>>;

private:
    // Builds an ldns_resolver* aimed at _cfg.server:_cfg.port.
    // Caller owns the returned pointer (ldns_resolver_deep_free).
    ldns_resolver* make_resolver() const;

    config _cfg;
};
```

#### `find_peers()` sequence

1. Create an `ldns_resolver` via `make_resolver()` pointing at `_cfg.server`.
2. Send two queries for `_cfg.shared_name` — one for `LDNS_RR_TYPE_A` and one
   for `LDNS_RR_TYPE_AAAA` — using `ldns_resolver_send()`. Set the resolver
   read timeout from the `timeout` argument on each query.
3. Merge the answer sections from both responses using
   `ldns_pkt_rr_list_by_type(..., LDNS_SECTION_ANSWER)`.
4. For each A or AAAA RR, stringify the rdata with `ldns_rdf2str()` to obtain
   the IP address string. Construct `peer_info{ip, ip}` (the IP address serves
   as both node identifier and contact address at this layer).
5. Return the collected `peer_info` entries as an immediately-resolved future.

### 8. `include/raft/rfc2136_ldns_discovery.hpp` — RFC 2136 dynamic DNS

Handles node registration (RFC 2136 UPDATE) and delegates `find_peers` to an
embedded `rfc1035_peer_discovery` instance, filtering out the owning node's own
address from the results. libldns provides packet construction, TSIG signing,
and resolver I/O.

#### `rfc2136_ldns_discovery::config`

```cpp
struct config {
    rfc1035_peer_discovery::config query;  // DNS server, port, and shared_name
    std::string   zone;                    // e.g. "cluster.example.com."
    std::uint32_t ttl{30};                 // A/AAAA record TTL in seconds
    // RFC 2845 TSIG — leave key_name empty to disable authentication
    std::string   tsig_key_name;
    std::string   tsig_algorithm{"hmac-sha256."};
    std::string   tsig_key_base64;
};
```

#### Class sketch

```cpp
class rfc2136_ldns_discovery {
public:
    using node_id_type  = std::string;
    using address_type  = std::string;

    explicit rfc2136_ldns_discovery(config cfg);

    // Best-effort deregistration — removes own A/AAAA record via RFC 2136 DELETE.
    ~rfc2136_ldns_discovery();

    // Sends RFC 2136 UPDATE adding an A record (IPv4) or AAAA record (IPv6)
    // for self_address; resolves the future once the server returns NOERROR.
    // Stores self_id and self_address for find_peers() filtering and the dtor.
    auto register_node(std::string self_id, std::string self_address)
        -> kythira::Future<void>;

    // Delegates to _rfc1035.find_peers(timeout), then removes any entry whose
    // address matches _self_address before returning.
    auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<peer_info<std::string, std::string>>>;

private:
    // Sends RFC 2136 UPDATE deleting own A/AAAA record; swallows all exceptions.
    void deregister_self() noexcept;

    // Builds a DNS UPDATE packet for the given operation.
    // rr_type is LDNS_RR_TYPE_A or LDNS_RR_TYPE_AAAA; add=true → ADD, false → DELETE.
    ldns_pkt* build_update(ldns_rr_type rr_type, const std::string& ip, bool add) const;

    // If _cfg.tsig_key_name is non-empty, signs pkt in place with TSIG.
    void maybe_sign(ldns_pkt* pkt) const;

    rfc1035_peer_discovery _rfc1035;
    std::string            _self_id;
    std::string            _self_address;
    config                 _cfg;
};
```

#### `find_peers()` delegation

```cpp
auto rfc2136_ldns_discovery::find_peers(std::chrono::milliseconds timeout)
    -> kythira::Future<std::vector<peer_info<std::string, std::string>>> {
    return _rfc1035.find_peers(timeout).then([this](auto peers) {
        std::erase_if(peers, [&](const auto& p) { return p.address == _self_address; });
        return peers;
    });
}
```

#### `register_node()` / `deregister_self()` sequence

Both call `build_update()` to construct an `ldns_pkt` of type `LDNS_PACKET_UPDATE`.
The record type is `LDNS_RR_TYPE_A` for an IPv4 `self_address` and
`LDNS_RR_TYPE_AAAA` for an IPv6 address (detected via `ldns_rdf_new_frm_str`
succeeding for the appropriate type):
- **register_node**: stores `self_id`/`self_address`; Zone = `_cfg.zone`; Update
  section = one ADD record: `shared_name TTL IN A <ip>` or `... IN AAAA <ip>`.
- **deregister_self**: Zone = `_cfg.zone`; Update section = one DELETE-specific-RR
  for the same A/AAAA content (RFC 2136 §2.5.4).

`maybe_sign()` calls `ldns_pkt_tsig_sign_next()` when `_cfg.tsig_key_name` is
non-empty. UPDATE packets are sent over TCP (`ldns_tcp_send()` preferred per
RFC 2136 §6.1).

#### DEPENDENCIES.md entry

```
libldns   ≥ 1.7   DNS packet construction, RFC 1035/3596 queries, RFC 2136 UPDATE, RFC 2845 TSIG
```

`static_assert(peer_discovery<rfc2136_ldns_discovery, std::string, std::string>)` at
the bottom of `rfc2136_ldns_discovery.hpp`.

## Data Models

### `cluster_join_request` wire format (JSON)

```json
{ "type": "cluster_join_request", "node_id": 4, "contact_address": "node4" }
```

### `cluster_join_response` wire format (JSON)

```json
{ "accepted": true }
{ "accepted": false, "redirect_node_id": 1, "redirect_address": "node1" }
{ "accepted": false }
```

The `json_rpc_serializer` gains `serialize` / `deserialize_cluster_join_*`
overloads following the same pattern as the existing RPC message pairs.

## Correctness Properties

### Property 1: Bootstrap does not violate safety
**Validates: Requirements 4.3, 5.4**

A `ClusterJoinRequest` that reaches the leader results in a call to
`add_server()`, which goes through the full joint consensus protocol. The new
node only becomes a voting member once C_new is committed. No safety invariant
(single leader per term, log matching) is weakened.

### Property 2: Bootstrap is idempotent
**Validates: Requirements 5.2**

If a fresh node sends `ClusterJoinRequest` to the same leader twice (e.g.,
due to a retry after a dropped response), the second `add_server()` call returns
immediately with "node already in configuration" — the same error path that
exists today. The node ends up in the cluster exactly once.

### Property 3: No bootstrap on restart
**Validates: Requirements 3.2**

A restarting node (non-zero persisted term, log, or snapshot) never enters
`run_bootstrap()`. It recovers state and rejoins the cluster via the normal
follower catch-up path (AppendEntries from the leader).

### Property 4: Backwards compatibility
**Validates: Requirements 2.3, 2.4**

With `no_op_peer_discovery` (the default), `run_bootstrap()` calls `find_peers()`
once, receives an empty vector, and returns immediately. `run_reconnect()` is
started but exits instantly when `_peer_contact_received` fires on the first
incoming RPC. The code path from `start()` to normal operation is unchanged
relative to today.

### Property 5: Reconnection does not cause split-brain
**Validates: Requirements 9.3, 9.5**

A restarting node that cannot reach peers only updates its address table — it
never calls `add_server()` on itself and never adjusts `_current_term` or
`_voted_for`. It cannot form a quorum alone because `no_op_peer_discovery` produces
an empty list and a restarting node never enters the single-node-founding path.
When the cluster is eventually located, the restarting node rejoins as a
follower and is subject to normal log-consistency checks.

## Error Handling

- **Peer unreachable**: `send_cluster_join_request()` throws / future rejects →
  caught inside `run_bootstrap()`, node moves to next peer.
- **Leader busy (config change in progress)**: leader returns
  `ClusterJoinResponse{accepted: false}` without a redirect (it is the leader
  but cannot currently `add_server()`). The joining node treats this like
  an unreachable peer and retries after `bootstrap_retry_interval`.
- **`stop()` during bootstrap**: `run_bootstrap()` checks `!_running.load()` at
  the top of each retry iteration. `stop()` sets `_running = false` and the loop
  exits on the next check.
- **Network partition**: bootstrap retries indefinitely until either a peer
  responds or `stop()` is called. No maximum retry count — the operator controls
  shutdown.

## Testing Strategy

Tests live in `tests/node_bootstrap_*.cpp` using `test_raft_types` and the
simulator. The `peer_discovery` used in tests is `static_peer_discovery`.

- `node_bootstrap_unit_test.cpp` — unit tests for `peer_discovery` concept
  satisfaction, `no_op_peer_discovery`, `static_peer_discovery`, `is_fresh_node()`,
  `handle_cluster_join()` routing (leader vs. follower), and `ClusterJoin` RPC
  serialization round-trips.
- `node_bootstrap_join_property_test.cpp` — property test: single-node cluster
  running; fresh node with `static_peer_discovery` pointing at it starts; after
  bootstrap, 2-node cluster converges and both nodes apply the same commands.
- `node_bootstrap_redirect_property_test.cpp` — property test: fresh node's
  seed peers are all followers; verify the redirect path leads to the leader
  and the join completes.
- `node_bootstrap_retry_property_test.cpp` — property test: seed peers initially
  unreachable (simulator edges disabled); after enabling the edge, the retry
  loop succeeds and the node joins.
- `node_bootstrap_stop_during_bootstrap_test.cpp` — unit test: bootstrap loop
  starts with unreachable peers; `stop()` called; verify loop terminates within
  `bootstrap_retry_interval + epsilon`.
