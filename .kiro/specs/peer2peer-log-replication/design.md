# Peer-to-Peer Log Replication (Gossip Catch-Up) Design Document

## Overview

`node<Types>` replicates its log in a strict star: `replicate_to_followers()`
iterates `_next_index` and personally sends each follower/learner either an
`AppendEntries` or an `InstallSnapshot` RPC. That is correct and low-latency
in steady state, but it means the leader is the *only* source of catch-up
data — so when several members are behind at once (rolling restart, healed
partition, a burst of `add_server()`/learner joins), the leader's own
CPU/bandwidth caps how fast the whole cluster converges, even though the
already-caught-up followers sitting right next to those lagging members have
spare capacity to help.

This design adds a second, opt-in path: a lagging node periodically learns
(via lightweight gossiped progress digests, never log entries) roughly how
far ahead its peers are, and — only when it has fallen more than a
configurable gap behind — pulls the missing range directly from one of them
via a new narrow `fetch_log_entries` RPC. The leader's existing direct-push
path is untouched; this is additive capacity, not a replacement.

The design leans on one observation to avoid inventing new safety machinery:
Raft's Log Matching Property already means an entry received from *any*
source, at a given `(index, term)`, is either identical to what a correct
leader would have sent, or gets detected and overwritten as a conflict the
next time a real `AppendEntries` arrives. So a lagging node can safely accept
entries from a peer using **exactly the same consistency-check-and-truncate
logic `handle_append_entries()` already has** — it just needs to *not* treat
the peer as if it were the leader for anything else (commit index, known
leader, election timer). Section "Correctness Properties" below makes this
precise.

## Architecture

```
Steady state (unchanged) — leader pushes directly:
  Leader ──AppendEntries──► Follower A (caught up)
  Leader ──AppendEntries──► Follower B (caught up)
  Leader ──AppendEntries──► Follower C (caught up)

Catch-up scenario (new) — C falls behind (e.g. just rejoined after partition):
  Leader ──AppendEntries (small batches, same as always)──► A, B
  Leader ──AppendEntries (small batches, same as always)──► C  (would take
                                                                many round-
                                                                trips alone)
             gossip: A/B/C periodically advertise (node_id, term,
             last_log_index) via peer2peer_replicator — no log data
  C, seeing A is far ahead of its own last_log_index (gap > threshold):
    C ──fetch_log_entries(from, to)──► A
    A ──fetch_log_entries_response(prev_log_term, entries)──► C
  C applies the response through the SAME consistency-check-and-truncate
  helper handle_append_entries() uses — provisional, never advances
  C's _commit_index, never touches C's _known_leader or election timer.
  C's log is now caught up; ordinary leader AppendEntries continues driving
  actual commit progress exactly as before.

If no source is known, or the fetch fails: nothing changes from today — the
leader's own replicate_to_followers() keeps pushing C directly (including
falling back to InstallSnapshot if C is behind the leader's compacted log
horizon, exactly as it does now).
```

```
include/raft/peer2peer_replication.hpp          (new)
  │
  ├── peer2peer_replicator (concept) ─────────► advertise_progress(...),
  │                                              find_catch_up_source(...)
  │                                              — mirrors peer_discovery's
  │                                                shape exactly.
  │
  ├── no_op_peer2peer_replicator<NodeId, Address, LogIndex>
  │     - advertise_progress: succeeds immediately
  │     - find_catch_up_source: always resolves nullopt
  │     ⇒ this alone guarantees zero behavioral change when not opted in
  │
  └── static_peer2peer_replicator<NodeId, Address, LogIndex>
        - shared in-memory progress table (synchronized_map), for
          simulator-based tests; not a real gossip transport

include/raft/types.hpp                          (extended)
  - fetch_log_entries_request<NodeId, TermId, LogIndex>
  - fetch_log_entries_response<TermId, LogIndex, LogEntry>
  - _has_peer2peer_replicator_type<T> / _peer2peer_replicator_type_traits<T, ...>
    (mirrors _has_quorum_manager_type / _quorum_manager_type_traits in raft.hpp)
  - raft_configuration: + _progress_gossip_interval, _catch_up_gap_threshold,
    _catch_up_fetch_max_entries, _catch_up_fetch_timeout

include/raft/network.hpp                        (extended)
  - network_client_with_log_fetch / network_server_with_log_fetch
    (optional extensions, mirrors network_client_with_cluster_join exactly)

include/raft/raft.hpp                           (extended)
  - node_config<Types>::peer2peer_replicator field
  - node<Types>::_peer2peer_replicator member
  - node<Types>::append_entries_with_consistency_check(...)  — extracted
    from handle_append_entries()'s Rules 3-4, shared by both paths
  - node<Types>::maybe_gossip_progress()       — Requirement 3
  - node<Types>::maybe_catch_up_from_peer()    — Requirement 4-7
  - node<Types>::handle_fetch_log_entries(...) — responder side, Requirement 5
```

## Components and Interfaces

### 1. `peer2peer_replicator` concept and `no_op_peer2peer_replicator`

```cpp
namespace kythira {

// Requirement 1.1/1.2: mirrors peer_discovery's shape — async, timeout-bounded,
// parameterized on NodeId/Address/LogIndex.
template<typename P, typename NodeId, typename Address, typename LogIndex>
concept peer2peer_replicator =
    requires(P& replicator, NodeId self_id, Address self_address,
             std::uint64_t term, LogIndex last_log_index, LogIndex from_index,
             LogIndex to_index, std::chrono::milliseconds timeout) {
        { replicator.advertise_progress(self_id, self_address, term, last_log_index) }
            -> std::same_as<kythira::Future<void>>;
        { replicator.find_catch_up_source(from_index, to_index, timeout) }
            -> std::same_as<kythira::Future<std::optional<peer_info<NodeId, Address>>>>;
    };

// Requirement 1.3: zero behavioral change when a Types bundle doesn't opt in.
template<typename NodeId, typename Address, typename LogIndex>
class no_op_peer2peer_replicator {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using log_index_type = LogIndex;

    auto advertise_progress(NodeId, Address, std::uint64_t, LogIndex) -> kythira::Future<void> {
        return kythira::FutureFactory::makeFuture();
    }
    auto find_catch_up_source(LogIndex, LogIndex, std::chrono::milliseconds) const
        -> kythira::Future<std::optional<peer_info<NodeId, Address>>> {
        return kythira::FutureFactory::makeFuture(std::optional<peer_info<NodeId, Address>>{});
    }
};

}  // namespace kythira
```

Reusing `peer_info<NodeId, Address>` (already defined in `peer_discovery.hpp`)
for `find_catch_up_source`'s result avoids introducing a parallel "peer
handle" type — a catch-up source and a discovered peer are the same kind of
fact (a node id plus how to reach it).

### 2. `static_peer2peer_replicator` (test/reference implementation)

```cpp
namespace kythira {

// Requirement 9.1: deterministic, in-memory stand-in for a real gossip
// transport. Construct one shared table and pass copies of this object
// (holding the same shared_ptr) to every node in a simulator-based test
// cluster — mirrors how simulator_network's shared registry works.
template<typename NodeId, typename Address, typename LogIndex>
class static_peer2peer_replicator {
public:
    struct progress_digest {
        Address address;
        std::uint64_t term;
        LogIndex last_log_index;
    };
    using table_type = folly::Synchronized<std::unordered_map<NodeId, progress_digest>>;

    explicit static_peer2peer_replicator(std::shared_ptr<table_type> shared_table)
        : _table(std::move(shared_table)) {}

    auto advertise_progress(NodeId self_id, Address self_address, std::uint64_t term,
                            LogIndex last_log_index) -> kythira::Future<void> {
        _table->wlock()->insert_or_assign(self_id, progress_digest{self_address, term, last_log_index});
        return kythira::FutureFactory::makeFuture();
    }

    auto find_catch_up_source(LogIndex from_index, LogIndex /*to_index*/, std::chrono::milliseconds) const
        -> kythira::Future<std::optional<peer_info<NodeId, Address>>> {
        auto locked = _table->rlock();
        for (const auto& [id, digest] : *locked) {
            // Requirement 4: any peer gossiped as being at or past from_index
            // is a candidate — node<Types> itself decides via the RPC
            // response whether that peer still actually has the range.
            if (digest.last_log_index >= from_index) {
                return kythira::FutureFactory::makeFuture(
                    std::optional<peer_info<NodeId, Address>>{peer_info<NodeId, Address>{id, digest.address}});
            }
        }
        return kythira::FutureFactory::makeFuture(std::optional<peer_info<NodeId, Address>>{});
    }

private:
    std::shared_ptr<table_type> _table;
};

}  // namespace kythira
```

A real gossip transport (epidemic dissemination, SWIM-piggybacked, etc.) is
explicitly deferred to a follow-on spec (Requirement 9.2) — this class exists
so `node<Types>`'s catch-up logic can be tested end-to-end today.

### 3. New RPC types (`types.hpp`)

```cpp
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
struct fetch_log_entries_request {
    NodeId _requester_id;
    LogIndex _from_index;
    LogIndex _to_index;

    [[nodiscard]] auto requester_id() const -> NodeId { return _requester_id; }
    [[nodiscard]] auto from_index() const -> LogIndex { return _from_index; }
    [[nodiscard]] auto to_index() const -> LogIndex { return _to_index; }
};

template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
         typename LogEntry = log_entry<TermId, LogIndex>>
requires term_id<TermId> && log_index<LogIndex> && log_entry_type<LogEntry, TermId, LogIndex>
struct fetch_log_entries_response {
    std::uint64_t _responder_id;
    bool _available;
    TermId _prev_log_term;
    std::vector<LogEntry> _entries;

    [[nodiscard]] auto responder_id() const -> std::uint64_t { return _responder_id; }
    [[nodiscard]] auto available() const -> bool { return _available; }
    [[nodiscard]] auto prev_log_term() const -> TermId { return _prev_log_term; }
    [[nodiscard]] auto entries() const -> const std::vector<LogEntry>& { return _entries; }
};
```

Shaped identically to `append_entries_request`/`response` on purpose
(Requirement 6.2 applies the response through the exact same consistency-
check helper `AppendEntries` uses) — `fetch_log_entries_response` is
essentially `append_entries_request` without a `leader_id`/`leader_commit`,
because the responder is never treated as a leader.

### 4. `network_client_with_log_fetch` / `network_server_with_log_fetch`

```cpp
// Requirement 5.2 — optional, mirrors network_client_with_cluster_join.
// Existing transports (simulator_network_*, tcp_rpc_*, tls_tcp_rpc_*) are
// unaffected until each one opts in with its own send_fetch_log_entries/
// register_fetch_log_entries_handler implementation.
template<typename C>
concept network_client_with_log_fetch =
    requires(C client, node_id_type target, const fetch_log_entries_request<>& req,
             std::chrono::milliseconds timeout) {
        { client.send_fetch_log_entries(target, req, timeout) }
            -> std::same_as<kythira::Future<fetch_log_entries_response<>>>;
    };

template<typename S>
concept network_server_with_log_fetch =
    requires(S server,
             std::function<fetch_log_entries_response<>(const fetch_log_entries_request<>&)> handler) {
        { server.register_fetch_log_entries_handler(handler) } -> std::same_as<void>;
    };
```

### 5. `node<Types>` integration

```cpp
// Requirement 6.1: extracted from handle_append_entries()'s Rules 3-4 so
// both the leader-driven and peer-to-peer paths share one implementation.
// Pure log-consistency logic — no term bookkeeping, no _known_leader, no
// election-timer reset, no _commit_index change. Those stay in
// handle_append_entries() itself, wrapped around a call to this.
auto node<Types>::append_entries_with_consistency_check(
    log_index_type prev_log_index, term_id_type prev_log_term,
    const std::vector<log_entry_type>& entries)
    -> append_entries_response_type;   // unchanged Rules 3-4 body, moved verbatim

// Requirement 3: gossip this node's own progress every
// _progress_gossip_interval, regardless of server_state.
auto node<Types>::maybe_gossip_progress() -> void;

// Requirement 4: on every maintenance tick, compute the catch-up gap against
// the highest last_log_index seen via progress digests; if it exceeds
// _catch_up_gap_threshold and no fetch is already outstanding, ask
// _peer2peer_replicator for a source and, if one is found, call
// send_fetch_log_entries (if network_client_with_log_fetch<network_client_type>).
// A no-op replicator (find_catch_up_source always nullopt) makes this a
// silent no-op every tick — Requirement 4.4.
auto node<Types>::maybe_catch_up_from_peer() -> void;

// Requirement 5.4/5.5: responder side. Only reachable if
// network_server_with_log_fetch<network_server_type> — registered
// alongside the existing RequestVote/AppendEntries/InstallSnapshot handlers.
auto node<Types>::handle_fetch_log_entries(const fetch_log_entries_request_type& request)
    -> fetch_log_entries_response_type;
```

`maybe_catch_up_from_peer()`'s dispatch on transport support:

```cpp
auto node<Types>::maybe_catch_up_from_peer() -> void {
    if constexpr (!network_client_with_log_fetch<network_client_type>) {
        return;  // Requirement 5.3 — transport doesn't support it, nothing to do
    } else {
        // ... gap computation, find_catch_up_source, single-outstanding-fetch
        // guard (Requirement 4.5), send_fetch_log_entries, apply response via
        // append_entries_with_consistency_check (Requirement 6.2) ...
    }
}
```

This mirrors the existing `if constexpr (network_client_with_cluster_join<...>)`
dispatch already used for `ClusterJoin` (`raft.hpp:5278`) — no new dispatch
idiom introduced.

## Correctness Properties

### Property 1: Zero behavioral change when not opted in

`no_op_peer2peer_replicator::find_catch_up_source` always resolves
`std::nullopt` (Requirement 1.3). `maybe_catch_up_from_peer()` takes no
further action when that happens (Requirement 4.4). So for any `Types`
bundle that doesn't declare `peer2peer_replicator_type`, every tick's call to
`maybe_catch_up_from_peer()` is a gap computation followed by an immediate
no-op — no RPC is ever sent, `replicate_to_followers()`'s leader-push path is
never touched (Requirement 7.3), and `maybe_gossip_progress()`'s calls
similarly resolve immediately without I/O. Verified directly by Requirement
9.3's "bit-for-bit identical to today" property test.

### Property 2: Peer-to-peer catch-up cannot violate the Log Matching Property

`append_entries_with_consistency_check()` is the *same code* Raft already
relies on for safety when the leader replicates — it rejects any batch whose
`prev_log_index`/`prev_log_term` doesn't match what the receiver already has,
and truncates on conflict. Feeding it a peer-sourced batch (Requirement 6.2)
means a receiver can only ever end up with a log entry at `(index, term)`
that was either: (a) already consistent with everything the receiver had
before, or (b) rejected outright. Nothing about *where* the batch came from
enters this check — the property that holds for leader-sourced batches holds
identically for peer-sourced ones.

### Property 3: A bad or stale source peer cannot cause divergence

Suppose a lagging node `L` fetches from source peer `P`, and `P` turns out to
be on an abandoned term (its entries were never actually committed by a real
leader and get superseded). `L` appends `P`'s entries via
`append_entries_with_consistency_check()`, same as it would append leader
entries. The next time `L` receives a genuine `AppendEntries` from the actual
current leader, that leader's `prev_log_index`/`prev_log_term` either matches
what `L` has (in which case `P`'s entries were, transitively, correct — `P`
wasn't actually wrong) or it doesn't, in which case Rule 4's conflict
detection truncates `L`'s log from the conflict point exactly as it would for
*any* conflicting suffix, regardless of origin. `L` converges to the leader's
log either way. This is the same guarantee that already protects a follower
from a leader that itself crashes mid-replication and is replaced by a
different leader with a diverging tail (a scenario Raft already handles) —
peer-to-peer catch-up introduces no new failure shape, only a new *source*
for entries that were always going to be subject to this check.

### Property 4: Peer-to-peer catch-up never advances commit index

`append_entries_with_consistency_check()` (Requirement 6.1) does not touch
`_commit_index` — that remains the exclusive responsibility of
`handle_append_entries()`'s own post-check logic (`leader_commit` handling)
and `advance_commit_index()`/`calculate_new_commit_index()`, both entirely
leader/quorum driven and untouched by this spec. A node can have a fully
caught-up *log* via peer-to-peer fetch while its `_commit_index` still lags
until the real leader's next heartbeat confirms it — matching this spec's
Design Decision 1 exactly.

### Property 5: At most one outstanding fetch per node

Requirement 4.5's single-in-flight guard means `maybe_catch_up_from_peer()`
cannot itself create unbounded concurrent load on a source peer or on the
requester's own bookkeeping — a straightforward reentrancy guard (a
`std::optional<pending_fetch>` member checked/set under `_mutex`, cleared
when the outstanding future resolves), the same shape already used for
single-flight guards elsewhere in `raft.hpp` (e.g. the maintenance-thread's
own one-tick-at-a-time execution).

## Error Handling

- **`find_catch_up_source` resolves `nullopt`**: no-op this tick, re-evaluated
  next tick (Requirement 4.4/7.1).
- **`fetch_log_entries` RPC times out or errors**: logged at debug level,
  in-flight guard cleared, no-op this tick, re-evaluated next tick
  (Requirement 7.1) — exactly as a failed `AppendEntries` retry already
  degrades to "try again next tick" rather than a hard failure.
- **Responder has already compacted past `from_index`**: responds
  `available = false` (Requirement 5.4); requester treats this identically to
  an RPC failure — no-op this tick. If the node is behind the *leader's* own
  compaction horizon too, the leader's existing `send_install_snapshot_to()`
  path (unchanged) eventually resolves it exactly as it does today.
- **Consistency check on the fetched batch fails** (Requirement 6.2): the
  batch is discarded in full — never partially applied — and treated as a
  failed catch-up attempt (Requirement 7.1).
- **Gossip advertisement fails** (transport error in
  `advertise_progress`): logged at debug level, never retried more
  aggressively than the next scheduled gossip round, never blocks any
  Raft-critical path (Requirement 3.3).

## Testing Strategy

- **Unit**: `no_op_peer2peer_replicator` satisfies the concept and always
  resolves as specified; `static_peer2peer_replicator` returns the correct
  candidate (or none) for a range of table contents; `append_entries_with_
  consistency_check()` extraction is a pure refactor — the entire existing
  `handle_append_entries()` unit/property test suite must continue to pass
  unmodified, proving no behavioral change from the extraction itself.
- **Property** (network simulator, `static_peer2peer_replicator` wired in
  across all simulated nodes sharing one table): a node with an empty log
  joining a cluster with substantial existing history catches up primarily
  via a peer rather than the leader (assert: leader's
  `raft_replication_round`/append-entries-sent metrics for that node stay
  low while the node's log still converges); a follower that reconnects
  after a simulated partition catches up via a peer; Property 1's "identical
  to today with the no-op default" check, diffing behavior/metrics with
  `peer2peer_replicator_type` present-but-no-op against a `Types` bundle
  that never declares it at all.
- **Safety** (Property 3 above, directly tested): construct a scenario where
  a follower fetches from a peer whose log later needs truncation by the
  real leader (simulate the peer having briefly been on an abandoned term);
  assert the fetching node's log still ends up matching the eventual
  leader's log after subsequent normal heartbeats — no divergence, no
  crash, no need for operator intervention.
- **Backwards compatibility**: every existing test file that constructs
  `node<Types>`/`node_config<Types>` without a `peer2peer_replicator_type`
  continues to compile and pass completely unmodified — the regression gate
  for Requirement 2.5.

## Non-Goals

- **A production-grade gossip transport.** `static_peer2peer_replicator` is
  an in-memory test/reference implementation, not something intended for a
  real deployment. A real epidemic/anti-entropy or SWIM-style transport is
  explicitly deferred to a follow-on spec (Requirement 9.2), the same way
  `node-bootstrap` shipped `peer_discovery` + `static_peer_discovery` first
  and `rfc1035_peer_discovery`/DNS-SD adaptors arrived later under
  `.kiro/specs/dns-peer-discovery/`.
- **Peer-to-peer distribution of snapshots.** `send_install_snapshot_to()` is
  unchanged and remains leader-only; a node behind the leader's compaction
  horizon still gets its snapshot from the leader. Extending peer-to-peer
  transfer to snapshots (which are larger and would benefit even more from
  spreading load) is a natural follow-on but is out of scope here to keep
  this spec's safety argument centered on ordinary log entries, where the
  Log Matching Property applies directly.
- **Changing commit-index advancement or quorum calculation.** Explicitly
  ruled out by this spec's first design decision; any change there is a
  different, much larger spec with its own safety re-derivation.
- **Load-aware or topology-aware source selection** (e.g. preferring a
  geographically closer peer, or one with spare bandwidth). Source selection
  is entirely `peer2peer_replicator_type`-implementation-defined;
  `static_peer2peer_replicator`'s "first peer in the table with enough log"
  policy is deliberately simplistic. Smarter selection policies are a
  follow-on concern for whichever concrete implementation wants them.
