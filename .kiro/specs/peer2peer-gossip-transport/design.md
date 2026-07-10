# Peer-to-Peer Gossip Transport Design Document

## Overview

`.kiro/specs/peer2peer-log-replication/` shipped the abstract
`peer2peer_replicator` concept, a no-op default, and an in-memory
`static_peer2peer_replicator` for tests — but no implementation a real
cluster could actually run. This design fills that gap with
`tcp_gossip_peer2peer_replicator`: a self-contained, independent TCP channel
that runs a classic anti-entropy gossip protocol (randomized push-pull digest
exchange, à la Cassandra/Dynamo) among a static, operator-configured peer
list, entirely decoupled from the Raft-critical `network_client_type`/
`network_server_type` transport.

Two things this design deliberately does *not* do, both to keep scope tight
and risk low:

1. **It is not a failure-detection protocol.** SWIM and similar protocols
   solve "is this peer alive," with indirect probing and suspicion states.
   Raft already answers that question for consensus purposes via election
   timeouts. This transport only answers "roughly how far has each peer's
   log gotten," on a best-effort, eventually-consistent basis — closer to
   Cassandra's original gossip layer (which also assumes membership is
   otherwise known) than to a general membership protocol.
2. **It does not reuse the Raft RPC transport.** Every existing
   `peer_discovery_type` implementation in this codebase (`rfc1035_peer_discovery`,
   `poco_peer_discovery`, `rfc2136_dns_sd_discovery`) is its own independent
   channel, not layered onto `network_client_type`/`network_server_type`.
   This design follows that same separation: a gossip-layer bug or overload
   can never touch consensus traffic, and vice versa.

## Architecture

```
Per-node state:
  local digest table: node_id -> { address, term, last_log_index, fresh_until }
  (populated by: this node's own advertise_progress() calls, and by every
   gossip_exchange this node has participated in, as either side)

Every gossip_round_interval (default 500ms), on a dedicated background thread:

  Node A                                         Node B (randomly selected,
                                                          one of `fanout` peers)
    │  gossip_exchange_request                          │
    │  { sender: A, digests: A's full local table }     │
    │ ──────────────────────────────────────────────►   │
    │                                                    │  merge A's digests
    │                                                    │  into B's table
    │                                                    │  (Requirement 6.1:
    │                                                    │   higher (term,
    │                                                    │   last_log_index)
    │                                                    │   wins per node_id)
    │   gossip_exchange_response                         │
    │   { sender: B, digests: B's full local table }     │
    │ ◄──────────────────────────────────────────────    │
    │  merge B's digests                                 │
    │  into A's table                                    │
    │                                                     │

After a handful of rounds (in an N-node cluster with fanout f, information
propagates in O(log N / log f) rounds — the standard epidemic-gossip
convergence bound), every node's table reflects every other node's progress,
without any node needing to talk to all N-1 others directly.

find_catch_up_source(from_index, ...) is a pure local read of this table —
no network I/O happens synchronously inside a node<Types> call.
```

```
include/raft/tcp_gossip_transport.hpp              (new)
  │
  ├── gossip_digest<NodeId, Address, LogIndex>  ──► node_id, address, term,
  │                                                  last_log_index, fresh_until
  │
  ├── gossip_exchange_request/response          ──► sender_node_id,
  │     (implementation detail, not in types.hpp)   vector<gossip_digest<...>>
  │                                                  — boost::json encoded,
  │                                                  tcp_detail-framed
  │
  ├── tcp_gossip_config                         ──► address_book (address
  │                                                  resolution only, NOT
  │                                                  membership), listen_port,
  │                                                  fanout, gossip_round_interval,
  │                                                  freshness_interval
  │
  └── tcp_gossip_peer2peer_replicator<NodeId, Address, LogIndex>
        - satisfies peer2peer_replicator (static_assert)
        - advertise_progress() / find_catch_up_source()  — Requirement 1
        - update_membership()                            — Requirement 1.4/2,
          the sole source of truth for current membership (_active_members)
        - eligible_peers()                                — Requirement 2.3,
          _active_members ∩ address_book
        - start_gossip_thread()/stop_gossip_thread()/gossip_loop()
          (shape mirrors rfc2136_dns_sd_discovery's
           start_fresher()/stop_fresher()/fresher_loop())
        - listener accept loop, reusing tcp_detail::connect_to/frame_send/
          frame_recv/bytes_to_str/str_to_bytes from tcp_rpc.hpp
        - merge()/prune_expired() — Requirement 6
```

## Components and Interfaces

### 1. `gossip_digest` and the wire types

```cpp
namespace kythira {

// Requirement 5.1 — never carries log entries, only enough to answer
// "who might have what I'm missing."
template<typename NodeId, typename Address, typename LogIndex>
struct gossip_digest {
    NodeId node_id;
    Address address;           // this peer's Raft RPC address, NOT its
                                // gossip-listener address (Requirement 7.1)
    std::uint64_t term;
    LogIndex last_log_index;
    std::int64_t fresh_until;  // epoch seconds; Requirement 6.4
};

// Implementation-file-local, not added to types.hpp/raft_types
// (Requirement 5.1) — this is not a Raft consensus RPC.
template<typename NodeId, typename Address, typename LogIndex>
struct gossip_exchange_message {
    NodeId sender_node_id;
    std::vector<gossip_digest<NodeId, Address, LogIndex>> digests;
};
// gossip_exchange_request and gossip_exchange_response are both this same
// shape — the protocol is symmetric push-pull, so request and response
// carry identical information in opposite directions.

}  // namespace kythira
```

### 2. `tcp_gossip_config`

```cpp
namespace kythira {

template<typename NodeId, typename Address>
struct tcp_gossip_config {
    // Requirement 2.1: address resolution only — NOT a statement of current
    // membership. cluster_configuration<NodeId> carries no addresses, so
    // this is still supplied out-of-band, but current membership itself
    // comes exclusively from update_membership() calls (Requirement 2.2),
    // not from this list.
    std::vector<peer_info<NodeId, Address>> address_book;
    std::uint16_t listen_port;                             ///< This node's gossip listener port.
    std::size_t fanout{3};                                 ///< Peers contacted per round.
    std::chrono::milliseconds gossip_round_interval{500};  ///< Cadence between rounds.
    std::chrono::seconds freshness_interval{5};             ///< TTL for a digest since last refresh.
};

}  // namespace kythira
```

### 3. `tcp_gossip_peer2peer_replicator`

```cpp
namespace kythira {

template<typename NodeId, typename Address, typename LogIndex>
class tcp_gossip_peer2peer_replicator {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using log_index_type = LogIndex;

    explicit tcp_gossip_peer2peer_replicator(tcp_gossip_config<NodeId, Address> cfg);
    ~tcp_gossip_peer2peer_replicator();  // stop_gossip_thread() + stop_listener()

    tcp_gossip_peer2peer_replicator(const tcp_gossip_peer2peer_replicator&) = delete;
    auto operator=(const tcp_gossip_peer2peer_replicator&) -> tcp_gossip_peer2peer_replicator& = delete;

    // Requirement 1.2 — updates the local table only; resolves immediately.
    auto advertise_progress(NodeId self_id, Address self_address, std::uint64_t term,
                            LogIndex last_log_index) -> kythira::Future<void>;

    // Requirement 1.3 — pure local-table read, filtered by _active_members.
    auto find_catch_up_source(LogIndex from_index, LogIndex to_index,
                              std::chrono::milliseconds timeout) const
        -> kythira::Future<std::optional<peer_info<NodeId, Address>>>;

    // Requirement 1.4/2.2 — replaces _active_members; resolves immediately.
    // This, not address_book, is this instance's source of truth for
    // "who is currently a cluster member" (Requirement 2).
    auto update_membership(std::vector<NodeId> member_ids) -> kythira::Future<void>;

private:
    auto start_gossip_thread() -> void;
    auto stop_gossip_thread() -> void;
    auto gossip_loop() -> void;                                    // Requirement 4
    auto run_one_round() -> void;                                   // select fanout, exchange_with each
    auto exchange_with(const peer_info<NodeId, Address>& peer) -> void;  // one push-pull RPC
    [[nodiscard]] auto eligible_peers() const
        -> std::vector<peer_info<NodeId, Address>>;  // Requirement 2.3: _active_members ∩ address_book

    auto start_listener() -> void;
    auto stop_listener() -> void;
    auto accept_loop() -> void;                                     // reuses tcp_detail helpers
    auto handle_incoming_exchange(int connection_fd) -> void;

    auto merge(const std::vector<gossip_digest<NodeId, Address, LogIndex>>& incoming) -> void;  // Requirement 6.1
    auto prune_expired() -> void;                                   // Requirement 6.3
    [[nodiscard]] auto snapshot_table() const
        -> std::vector<gossip_digest<NodeId, Address, LogIndex>>;

    tcp_gossip_config<NodeId, Address> _cfg;
    folly::Synchronized<std::unordered_map<NodeId, gossip_digest<NodeId, Address, LogIndex>>>
        _table;
    // Requirement 2.2: empty until the first update_membership() call —
    // deliberately NOT initialized from _cfg.address_book's keys, since
    // address_book is address-resolution data, not a membership statement.
    folly::Synchronized<std::unordered_set<NodeId>> _active_members;
    std::atomic<bool> _running{false};
    std::thread _gossip_thread;
    std::thread _listener_thread;
    int _listen_fd{-1};
};

static_assert(peer2peer_replicator<
    tcp_gossip_peer2peer_replicator<std::uint64_t, std::string, std::uint64_t>,
    std::uint64_t, std::string, std::uint64_t>);

}  // namespace kythira
```

`merge()` is the entire safety-relevant logic in this class:

```cpp
// Requirement 6.1: higher (term, last_log_index) wins per node_id; a
// node_id not yet present is always added. No cross-node coordination,
// no clock synchronization required.
auto tcp_gossip_peer2peer_replicator<NodeId, Address, LogIndex>::merge(
    const std::vector<gossip_digest<NodeId, Address, LogIndex>>& incoming) -> void {
    auto locked = _table.wlock();
    for (const auto& d : incoming) {
        auto it = locked->find(d.node_id);
        if (it == locked->end() ||
            std::tie(d.term, d.last_log_index) >
                std::tie(it->second.term, it->second.last_log_index)) {
            (*locked)[d.node_id] = d;
        }
    }
}
```

`update_membership()` and `eligible_peers()` are how Requirement 2's
"membership from the log, addresses from a static book" split is enforced:

```cpp
// Requirement 1.4/2.2: this, not address_book, is the source of truth for
// current membership.
auto tcp_gossip_peer2peer_replicator<NodeId, Address, LogIndex>::update_membership(
    std::vector<NodeId> member_ids) -> kythira::Future<void> {
    *_active_members.wlock() =
        std::unordered_set<NodeId>(member_ids.begin(), member_ids.end());
    return kythira::FutureFactory::makeFuture();
}

// Requirement 2.3: intersection, not union — a member with no known address
// is unreachable regardless of membership status; a stale address_book
// entry for a former member is never selected regardless of address_book
// contents, because it's absent from _active_members.
auto tcp_gossip_peer2peer_replicator<NodeId, Address, LogIndex>::eligible_peers() const
    -> std::vector<peer_info<NodeId, Address>> {
    auto members = _active_members.rlock();
    std::vector<peer_info<NodeId, Address>> result;
    for (const auto& peer : _cfg.address_book) {
        if (members->contains(peer.node_id)) result.push_back(peer);
    }
    return result;
}
```

### 4. `exchange_with()` — one push-pull round

```cpp
// Requirement 5: framing/encoding, Requirement 8: failure isolation.
auto tcp_gossip_peer2peer_replicator<NodeId, Address, LogIndex>::exchange_with(
    const peer_info<NodeId, Address>& peer) -> void {
    auto [host, port] = split_host_port(peer.address);  // Address = "host:port"
    int fd = tcp_detail::connect_to(host, port, _cfg.gossip_round_interval);
    if (fd < 0) {
        // Requirement 8.1 — logged, this round's exchange with this peer
        // simply doesn't happen; other peers in this round are unaffected.
        return;
    }
    struct Guard { int fd; ~Guard() { if (fd >= 0) ::close(fd); } } g{fd};

    auto request = gossip_exchange_message<NodeId, Address, LogIndex>{
        .sender_node_id = _self_id, .digests = snapshot_table()};
    if (!tcp_detail::frame_send(fd, encode_gossip_message(request))) return;

    auto raw = tcp_detail::frame_recv(fd);
    if (!raw.has_value()) return;  // Requirement 8.1 — malformed/closed, no-op

    auto response = decode_gossip_message<NodeId, Address, LogIndex>(*raw);
    merge(response.digests);
}
```

### 5. `node<Types>` usage (no changes to `raft.hpp` required)

Because `tcp_gossip_peer2peer_replicator` fully satisfies the
`peer2peer_replicator` concept already consumed by `node<Types>` (per the
depended-on spec), adopting it is purely a `Types` bundle change:

```cpp
struct my_raft_types {
    // ... existing fields ...
    using peer2peer_replicator_type =
        kythira::tcp_gossip_peer2peer_replicator<node_id_type, address_type, log_index_type>;
};

// construction: address_book still needs every node ID that might ever be
// a member (Requirement 2.4) — but which of them are CURRENTLY members is
// never passed here. That comes later, automatically, via node<Types>'s own
// cluster_members()-driven update_membership() calls (depended-on spec,
// Requirement 11) — not shown in this snippet because the caller never
// invokes it directly.
node_config<my_raft_types> cfg{
    // ... existing fields ...
    .peer2peer_replicator = tcp_gossip_peer2peer_replicator<...>(tcp_gossip_config<...>{
        .address_book = {{1, "10.0.0.1:9000"}, {2, "10.0.0.2:9000"}, {3, "10.0.0.3:9000"}},
        .listen_port = 9100,
    }),
};
```

No changes to `include/raft/raft.hpp`, `types.hpp`, or `network.hpp` are
needed by *this* spec specifically — the `update_membership()` call sites
inside `raft.hpp` already exist by the time this spec is implemented,
because they're the depended-on spec's own Requirement 11/Task 10. This
spec's own footprint is entirely `tcp_gossip_transport.hpp` plus its test
files, which is the payoff of the depended-on spec having already done the
harder work of defining a clean, membership-aware abstraction boundary.

## Data Models

### `gossip_exchange` wire shape

Symmetric push-pull — request and response share one shape:

```json
{
  "sender_node_id": 2,
  "digests": [
    { "node_id": 1, "address": "10.0.0.1:9000", "term": 3, "last_log_index": 250, "fresh_until": 1799999500 },
    { "node_id": 2, "address": "10.0.0.2:9000", "term": 3, "last_log_index": 251, "fresh_until": 1799999620 }
  ]
}
```

### `tcp_gossip_config` — the two independent inputs (Requirement 2)

```
address_book: [(1, "10.0.0.1:9000"), (2, "10.0.0.2:9000"), (3, "10.0.0.3:9000")]
                  ↑ static, out-of-band, address resolution only

_active_members (via update_membership(), not part of tcp_gossip_config):
                  {1, 2, 3}  before a membership change
                  {1, 2, 3, 4}  after add_server(4) commits and
                                cluster_members() reflects it
                  {1, 2, 3}     after remove_server(4) commits, even though
                                node 4's address may still linger in
                                address_book and its digest may still
                                linger in _table until fresh_until expires

eligible_peers() = address_book ∩ _active_members (by node_id), always —
this is the only set gossip rounds and find_catch_up_source ever draw from.
```

## Correctness Properties

### Property 1: Gossip convergence does not depend on any single node
**Validates: Requirements 4.2**

Because every gossip round is a random peer selection (Requirement 4.2), no
node — including whichever node happens to be Raft leader — is a required
relay for information to reach any other node. A digest originating at node
X reaches node Y through *some* chain of intermediate exchanges, and because
selection is random and repeated every round, the expected number of rounds
before Y has seen X's digest is bounded (the standard epidemic/rumor-
spreading result: O(log N) rounds for N nodes with any constant fanout ≥ 2).
This is what makes the transport suitable for exactly the scenario the
depended-on spec targets: catch-up load spreading across whichever peers
happen to have capacity, not funneling through one node.

### Property 2: A gossip-layer failure cannot affect Raft correctness
**Validates: Requirements 1.2, 1.3, 8.1, 8.2, 8.3**

`advertise_progress`/`find_catch_up_source` never throw and never block on
network I/O (Requirement 1.2/1.3, Requirement 8.3) — the former is a local
write, the latter a local read. The background gossip thread's own failures
(Requirement 8.1/8.2) are entirely self-contained: a round with zero
reachable peers simply leaves the local table unchanged, which in turn means
`find_catch_up_source` returns what it already had (possibly `nullopt`),
which `node<Types>::maybe_catch_up_from_peer()` already treats as "no
source available, fall back to the leader" (depended-on spec, Requirement
4.4/7.1). No new failure mode is introduced at the `node<Types>` boundary —
the worst case for a completely gossip-isolated node (all peers unreachable
via this transport) is that it behaves exactly as if
`peer2peer_replicator_type` were `no_op_peer2peer_replicator`, which is
already a fully supported, safe configuration.

### Property 3: Merge staleness cannot cause a safety violation
**Validates: Requirements 6.1, 6.2**

This follows directly from Property 3 of
`.kiro/specs/peer2peer-log-replication/design.md`: any digest this transport
returns as a catch-up source is only ever used to select which peer to send
a `fetch_log_entries` RPC to; the entries actually received are still run
through `append_entries_with_consistency_check()` before being accepted.
A merge that keeps a slightly stale digest (Requirement 6.2 explicitly
accepts this possibility) can, at worst, cause `find_catch_up_source` to
suggest a peer that turns out to not actually have the requested range
(handled by `fetch_log_entries`'s `available = false` response) or whose
entries get rejected/truncated later (Property 3, depended-on spec) — never
a log entry silently accepted that shouldn't have been.

### Property 4: A removed member is never offered as a catch-up source, regardless of table staleness
**Validates: Requirements 1.4, 2.2, 2.3**

`find_catch_up_source` and `eligible_peers()` both filter through
`_active_members` (Requirement 1.3/2.3), which reflects only the most recent
`update_membership()` call. Because `_table` (gossiped digests) and
`_active_members` (current membership) are deliberately separate data
structures — the former can lag behind the latter by up to one
`fresh_until` window (Requirement 6.3), but the latter updates synchronously
the instant `update_membership()` is called (Requirement 1.4) — a node's
removal takes effect for catch-up-eligibility purposes immediately, even
though its stale digest may take up to `freshness_interval` longer to
actually disappear from `_table`. This is the direct implementation of
`.kiro/specs/peer2peer-log-replication/design.md`'s Property 6 for this
concrete transport.

## Error Handling

- **Peer unreachable during a gossip round** (Requirement 8.1): logged at
  debug level, that peer skipped for this round, remaining fanout peers
  still attempted.
- **Malformed/truncated `gossip_exchange` payload** (Requirement 5.4):
  connection closed without a response by the responder; the requester's
  `frame_recv` returns `std::nullopt` or a payload that fails to decode,
  treated identically to Requirement 8.1's connection failure.
- **Listener fails to bind at construction** (Requirement 4.4): thrown from
  the constructor — fail loudly and immediately, not silently degraded.
- **All peers unreachable across many consecutive rounds**: no distinct
  error state — the local table simply doesn't grow beyond what
  `advertise_progress` itself has set for this node's own entry; this is
  observationally identical to running with `no_op_peer2peer_replicator`
  (Property 2).

## Testing Strategy

- **Unit** (no network I/O): `merge()`'s `(term, last_log_index)` comparison
  rule, including the "not yet present" and "equal term, higher index"
  cases; `prune_expired()` removing only digests past their `fresh_until`;
  `eligible_peers()`'s intersection logic (a member with no address, an
  address with no membership, both excluded correctly); fanout selection
  over `eligible_peers()` excluding self and respecting
  `min(fanout, eligible.size())`.
- **Integration, real TCP, single process** (Requirement 10.2 — the explicit
  anti-flakiness constraint): construct several
  `tcp_gossip_peer2peer_replicator` instances in one test binary, each on
  its own loopback port, with each other in their `address_book`s and each
  instance's `update_membership()` called with the full instance set;
  confirm a `gossip_exchange` round trip actually happens over real sockets
  and that `advertise_progress` on one instance is visible via
  `find_catch_up_source` on another within a bounded number of rounds — no
  `posix_spawn`, no separate OS processes, avoiding the exact
  CPU-contention-under-`ctest -j$(nproc)` mechanism identified as this
  project's dominant flake source.
- **End-to-end property test, mixed transport** (Requirement 10.3): a
  multi-node `node<Types>` cluster using the in-process network simulator
  (`simulator_network_client`/`server`) for Raft RPCs but real
  `tcp_gossip_peer2peer_replicator` instances for
  `peer2peer_replicator_type` — validates the real transport's integration
  with actual catch-up behavior (reusing the depended-on spec's catch-up
  property-test scenarios: new node joining, partition-reconnect) without
  making Raft consensus itself depend on real-socket timing.
- **Freshness expiry** (Requirement 10.4): one instance stops calling
  `advertise_progress` (simulating a crash) while others continue gossiping;
  confirm its digest disappears from peers' tables after
  `freshness_interval` elapses, and that `find_catch_up_source` no longer
  offers it.

## Non-Goals

- **SWIM-style failure detection / indirect probing / suspicion states.**
  See Overview — Raft's own liveness detection already covers this
  transport's actual correctness needs.
- **Delta-compressed or Merkle-tree-summarized gossip payloads.** Sending
  the full small table every round is appropriate at this project's
  documented cluster scale (3–7 nodes); see Requirements' Non-Goals for the
  full rationale.
- **Live address discovery for brand-new members.** Membership itself is
  dynamic (`update_membership()`, Requirement 2.2); `address_book` — the
  node-ID-to-address resolution data, unavoidably static since addresses
  aren't log data — is not. See requirements.md's Non-Goals for the natural
  integration point (`ClusterJoin`) this spec deliberately leaves for a
  later follow-on.
- **CLI/deployment wiring into any specific binary.** This spec ships a
  tested, usable library component only.
