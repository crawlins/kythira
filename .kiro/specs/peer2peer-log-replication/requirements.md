# Peer-to-Peer Log Replication (Gossip Catch-Up) Requirements Document

## Introduction

Today, log replication in `node<Types>` is a strict star topology: the leader
alone sends every `AppendEntries`/`InstallSnapshot` RPC to every other cluster
member. `replicate_to_followers()` (`include/raft/raft.hpp`) iterates
`_next_index` — one entry per follower/learner — and for each one calls either
`send_install_snapshot_to()` or `send_append_entries_to()` individually. In
steady state (small per-heartbeat batches, all followers within
`_max_entries_per_append` of the leader) this is cheap and its low latency is
valuable. It becomes a bottleneck precisely when many members are
simultaneously *behind*: a rolling restart, a cluster-wide partition healing,
a burst of `add_server()` joins (`.kiro/specs/membership-change/`) or learner
attach operations (`.kiro/specs/non-voting-nodes/`) all leave several members
needing a large range of missing entries at once — and the leader is the only
node in the cluster permitted to supply them, so its own CPU and outbound
bandwidth become the hard ceiling on how fast the whole cluster catches up,
independent of how much spare capacity the other, already-caught-up followers
have.

This document specifies a peer-to-peer catch-up path that lets a lagging
member pull missing log entries directly from another cluster member that
already has them, instead of exclusively from the leader. Three design
decisions scope everything that follows:

1. **The leader remains the sole commit authority.** This spec does not
   change how `_commit_index` advances (`advance_commit_index()`,
   `calculate_new_commit_index()`) or how elections work. Peer-to-peer
   catch-up only populates a lagging node's *log*; that node still learns
   what is committed exclusively from the leader, exactly as today. See
   Requirement 6 for why this means no new safety argument is needed beyond
   what Raft's Log Matching Property already guarantees.
2. **It is opt-in and pluggable**, matching every other extension this
   project has added to `node<Types>` (`peer_discovery_type`,
   `quorum_manager_type`, `membership_manager_type`): a new
   `peer2peer_replicator_type` concept, defaulting to a no-op implementation
   that preserves today's leader-only behavior byte-for-byte for any
   `Types` bundle that doesn't declare one.
3. **It is a catch-up mechanism only.** The leader continues to push
   `AppendEntries` directly to every follower in the steady state
   (`replicate_to_followers()` is unchanged); peer-to-peer fetching only
   activates when a node detects it has fallen more than a configurable gap
   behind the newest progress it knows about.

## Glossary

- **Progress digest**: a small `(node_id, term, last_log_index)` tuple a node
  periodically advertises about itself and learns about its peers, via its
  `peer2peer_replicator_type`. Never carries log entries or command payloads —
  only enough to answer "who might have what I'm missing?".
- **Catch-up gap**: the difference between a node's own `get_last_log_index()`
  and the highest `last_log_index` it has seen in any progress digest,
  including the leader's own heartbeats.
- **Source peer**: a peer a lagging node's `peer2peer_replicator_type`
  identifies, from gossiped progress digests, as likely to hold the entries
  the lagging node needs.
- **Provisional entries**: log entries appended via a peer-to-peer
  `fetch_log_entries` exchange rather than the leader's own `AppendEntries`.
  They are ordinary log entries in every respect once appended — same
  conflict/truncation rules, same eventual commit path — the term
  "provisional" only describes that they arrived from a non-authoritative
  source and are never used, by themselves, to advance `_commit_index`.
- **Gossip round**: one periodic maintenance-thread tick on which a node
  advertises its own progress digest and/or refreshes its view of peers'
  digests through its `peer2peer_replicator_type`.

## Requirements

### Requirement 1: `peer2peer_replicator` concept and no-op default

**User Story:** As a maintainer of a `Types` bundle that predates this spec, I
want my existing cluster to keep behaving exactly as it does today, so that
adopting a newer `raft.hpp` never silently changes replication behavior
underneath me.

#### Acceptance Criteria

1. A new header `include/raft/peer2peer_replication.hpp` SHALL define a
   `peer2peer_replicator` concept, following the shape already established by
   `peer_discovery` (`include/raft/peer_discovery.hpp`): asynchronous methods
   returning `kythira::Future<...>`, parameterized on `NodeId`, `Address`,
   and `LogIndex`.
2. The concept SHALL require:
   - `advertise_progress(self_id, self_address, term, last_log_index) -> Future<void>`
     — publish this node's own progress digest.
   - `find_catch_up_source(from_index, to_index, timeout) -> Future<std::optional<peer_info<NodeId, Address>>>`
     — return a peer believed (from gossiped digests) to hold entries
     covering `[from_index, to_index]`, or `std::nullopt` if none is known.
3. A `no_op_peer2peer_replicator<NodeId, Address, LogIndex>` SHALL be
   provided: `advertise_progress` always succeeds immediately;
   `find_catch_up_source` always resolves to `std::nullopt`. Both
   `peer2peer_replicator` and `no_op_peer2peer_replicator` SHALL be verified
   with a `static_assert`, matching `peer_discovery.hpp`'s existing pattern.
4. `no_op_peer2peer_replicator` always returning `std::nullopt` SHALL be
   sufficient, by itself (see Requirement 4.4), to guarantee zero behavioral
   change for any `Types` bundle that does not opt in — no additional
   feature flag is needed.

### Requirement 2: `node_config<Types>` / `node<Types>` wiring

**User Story:** As the author of a new `Types` bundle, I want to opt into
peer-to-peer catch-up by declaring one new type alias, without touching any
existing bundle that doesn't, so adoption is additive.

#### Acceptance Criteria

1. `types.hpp` SHALL gain a `_has_peer2peer_replicator_type<T>` concept
   (`requires { typename T::peer2peer_replicator_type; }`) and a
   `_peer2peer_replicator_type_traits<T, NodeId, Address, LogIndex, bool>`
   trait pair, mirroring `_quorum_manager_type_traits`/
   `_has_quorum_manager_type` in `raft.hpp` exactly (primary template falls
   back to `no_op_peer2peer_replicator<NodeId, Address, LogIndex>`;
   specialization for `true` uses `typename T::peer2peer_replicator_type`).
2. `raft_types` SHALL NOT require `peer2peer_replicator_type` — it remains an
   optional extension, exactly like `peer_discovery_type` is today (absent
   from the `raft_types` concept's `requires` block, resolved instead via a
   fallback trait).
3. `node_config<Types>` SHALL gain an optional
   `peer2peer_replicator_type peer2peer_replicator{}` field (default-
   constructed, so existing call sites using designated initializers continue
   to compile unchanged).
4. `node<Types>` SHALL gain a `peer2peer_replicator_type` alias resolved via
   the trait in 2.1, an `_peer2peer_replicator` member, and SHALL accept it
   through both the preferred `node_config<Types>` constructor and (as a
   trailing defaulted parameter, matching how `peer_discovery` was added to
   the legacy positional constructor) the legacy positional constructor.
5. Existing test types that construct `node<Types>` without knowledge of this
   feature SHALL continue to compile and run unchanged — verified by the
   full existing regression suite passing with zero modifications to
   pre-existing `Types` bundles.

### Requirement 3: Progress-digest gossip

**User Story:** As a cluster member, I want a lightweight, continuously
refreshed view of roughly how far every other member's log extends, so I can
tell whether I'm behind and who might already have what I'm missing, without
the leader having to tell me directly.

#### Acceptance Criteria

1. On every maintenance-thread tick (the same cadence already driving
   `check_heartbeat_timeout()`/`check_election_timeout()`), a node SHALL call
   `_peer2peer_replicator.advertise_progress(_node_id, self_address,
   _current_term, get_last_log_index())`, gated by a new
   `raft_configuration::_progress_gossip_interval` (default 500ms, coarser
   than `_heartbeat_interval`'s 50ms default since progress digests are
   advisory, not correctness-critical) so it does not fire on every tick.
2. A node's own progress digest SHALL be advertised regardless of
   `server_state` (leader, candidate, or follower) — a lagging leader that
   just lost an election still has entries other nodes may need.
3. Failure to advertise (peer2peer_replicator transport error) SHALL be
   logged at debug level and SHALL NOT be retried more aggressively than the
   next scheduled gossip round — this is best-effort dissemination, not a
   correctness-critical RPC, and SHALL NOT block or delay any Raft-critical
   operation (election, heartbeat, commit advancement).
4. This requirement's gossip transport is intentionally abstract — concrete
   dissemination mechanics (random peer subset exchange, SWIM-style
   piggybacking, etc.) are the concern of a specific
   `peer2peer_replicator_type` implementation and are explicitly out of
   scope for this spec (see Requirement 9).

### Requirement 4: Catch-up gap detection and trigger

**User Story:** As a lagging follower, I want to notice on my own that I'm
falling behind and proactively seek out a peer who can help, rather than
waiting for the leader to notice and personally stream everything to me.

#### Acceptance Criteria

1. On every maintenance-thread tick, a node SHALL compute its catch-up gap:
   `highest_known_last_log_index` (the maximum `last_log_index` across every
   progress digest currently known, including the leader's) minus
   `get_last_log_index()`.
2. WHEN the catch-up gap exceeds a new
   `raft_configuration::_catch_up_gap_threshold` (default 50 entries — larger
   than `_max_entries_per_append`'s default of 100 would single-handedly
   close in one normal `AppendEntries` batch, so the trigger only fires for
   gaps that would otherwise take several leader-driven round-trips), the
   node SHALL call `_peer2peer_replicator.find_catch_up_source(from_index,
   to_index, timeout)`, where `from_index` is `get_last_log_index() + 1` and
   `to_index` is capped at `from_index + _catch_up_fetch_max_entries - 1`
   (new config field, default 500) so a single catch-up fetch cannot request
   an unbounded amount of data.
3. A leader SHALL NOT trigger catch-up fetches for itself — this mechanism
   exists for followers (and candidates, transiently) falling behind, not for
   a leader (which is, by definition of `_commit_index`'s quorum requirement,
   never the node furthest behind).
4. WHEN `find_catch_up_source` resolves to `std::nullopt` (including always,
   for `no_op_peer2peer_replicator`), the node SHALL take no further action
   this tick — replication continues exactly as it does today, via the
   leader's own `replicate_to_followers()`/`send_append_entries_to()`/
   `send_install_snapshot_to()` path. This is the mechanism by which
   Requirement 1.4's "zero behavioral change when opted out" holds.
5. A node already mid-flight on a catch-up fetch to a given source SHALL NOT
   start a second concurrent fetch until the first resolves (success,
   failure, or timeout) — at most one outstanding peer-to-peer fetch per
   node at a time.

### Requirement 5: `fetch_log_entries` RPC

**User Story:** As a lagging node that has identified a source peer, I want a
narrow, purpose-built RPC to pull exactly the entries I'm missing from that
peer, using the same transport infrastructure every other Raft RPC already
uses.

#### Acceptance Criteria

1. `types.hpp` SHALL define `fetch_log_entries_request<NodeId, TermId,
   LogIndex>` (`requester_id`, `from_index`, `to_index`) and
   `fetch_log_entries_response<TermId, LogIndex, LogEntry>` (`responder_id`,
   `available` (bool — false if the responder has already compacted past
   `from_index` and cannot serve it), `prev_log_term` (the responder's term
   at `from_index - 1`, for the requester's consistency check), `entries`),
   following the existing private-member-plus-getter shape used by
   `append_entries_request`/`response` (`include/raft/types.hpp`).
2. `network.hpp` SHALL gain optional concept extensions
   `network_client_with_log_fetch` / `network_server_with_log_fetch`,
   mirroring `network_client_with_cluster_join`/
   `network_server_with_cluster_join` exactly: NOT required by the base
   `network_client`/`network_server` concepts, so every existing transport
   implementation (`simulator_network_client/server`, `tcp_rpc_client/
   server`, `tls_tcp_rpc_client/server`) continues to satisfy `network_client`/
   `network_server` unmodified.
3. `node<Types>` SHALL dispatch on
   `network_client_with_log_fetch<network_client_type>` via `if constexpr`
   (matching the existing `network_client_with_cluster_join` dispatch sites)
   — WHEN the configured transport does not support it, peer-to-peer catch-up
   SHALL be unreachable regardless of `peer2peer_replicator_type`
   configuration, falling back to Requirement 4.4's behavior.
4. A responder receiving a `fetch_log_entries_request` SHALL reply with
   `available = false` (no entries) if it does not itself have an entry at
   `from_index` (already compacted into a snapshot, or simply doesn't have it
   yet) — it SHALL NOT block waiting to acquire entries it doesn't have, and
   SHALL NOT consult its own peer2peer_replicator recursively to try to
   obtain them on the requester's behalf.
5. A responder SHALL serve at most `min(to_index, its own
   get_last_log_index())` entries — it never fabricates or pads a response
   past what it actually has persisted.

### Requirement 6: Safety — provisional entries and shared consistency logic

**User Story:** As an operator, I want peer-to-peer catch-up to be exactly as
safe as today's leader-only replication — a lagging node pulling from the
"wrong" or a stale peer must never be able to corrupt the cluster's log or
cause a committed entry to be lost or altered — so this feature is safe to
enable without re-deriving Raft's safety proof from scratch.

#### Acceptance Criteria

1. `handle_append_entries()`'s Rules 3–4 consistency-check-and-truncate logic
   (`include/raft/raft.hpp`, prevLogIndex/prevLogTerm verification, conflict
   detection, and truncation of conflicting suffixes) SHALL be extracted into
   a private helper — e.g. `append_entries_with_consistency_check(prev_log_index,
   prev_log_term, entries) -> append_entries_response_type` — called by both
   `handle_append_entries()` (unchanged behavior) and the new peer-to-peer
   entry-apply path, so the two paths cannot drift and peer-to-peer catch-up
   gets the exact same conflict/truncation guarantees as leader-driven
   replication for free.
2. A response to `fetch_log_entries` SHALL be applied via that shared helper
   exactly as if it were an `AppendEntries` payload with `prev_log_index =
   from_index - 1`, `prev_log_term = response.prev_log_term()`, `entries =
   response.entries()` — if the consistency check fails (the requester's log
   doesn't actually chain onto what the source peer sent), the fetched batch
   SHALL be discarded entirely and treated as a failed catch-up attempt
   (Requirement 7), not partially applied.
3. Applying a peer-sourced batch SHALL NOT modify `_commit_index`, SHALL NOT
   modify `_known_leader`, and SHALL NOT reset the election timer — those
   remain exclusively driven by the actual leader's own `AppendEntries`/
   heartbeats, upholding this spec's Design Decision 1 (leader remains sole
   commit authority).
4. No new safety machinery beyond 6.1–6.3 is required, because: entries
   appended via peer-to-peer fetch are — like any log entry accepted from any
   source under Raft's Log Matching Property — always still subject to being
   overwritten by the *actual* leader's next `AppendEntries` if they turn out
   to be from an abandoned term (the existing conflict-detection in the
   shared helper handles this identically whether the conflicting entry
   arrived from the leader or a peer). Worst case for a bad or stale source
   peer is wasted local work (entries later truncated), never a committed
   entry being lost, altered, or a follower diverging from the leader's
   eventual log undetected.
5. A test SHALL verify this directly: a follower catches up via a peer whose
   log is later shown to diverge from the real leader's (e.g. the peer was
   itself on an abandoned term) — assert the follower's log ends up matching
   the leader's after subsequent normal heartbeats, exactly as it would if no
   peer-to-peer fetch had occurred.

### Requirement 7: Fallback behavior

**User Story:** As an operator, I want peer-to-peer catch-up to be a pure
optimization — if it doesn't work for any reason, the cluster must still
converge exactly as it does today, just potentially more slowly.

#### Acceptance Criteria

1. WHEN a `fetch_log_entries` RPC fails (timeout, transport error,
   `available = false`, or the consistency check in Requirement 6.2 fails),
   the requesting node SHALL take no corrective action itself beyond logging
   — it SHALL simply wait for the next maintenance-thread tick, at which
   point Requirement 4 re-evaluates the gap and either retries via
   `find_catch_up_source` again or, if the node is still suficiently behind
   that the leader's own `replicate_to_followers()` has already switched it
   to `send_install_snapshot_to()` (unchanged existing threshold: `next_idx <
   first_log_index`), receives a snapshot from the leader as it would today.
2. Peer-to-peer catch-up and the leader's existing direct replication SHALL
   be able to proceed concurrently without conflicting — if the leader's
   regular `AppendEntries` arrives while a peer-to-peer fetch is outstanding,
   both are applied through the same shared consistency-check helper
   (Requirement 6.1), so whichever arrives second simply succeeds or is
   rejected by the ordinary conflict rules; no explicit coordination between
   the two paths is required.
3. This spec SHALL NOT modify `replicate_to_followers()`,
   `send_append_entries_to()`, or `send_install_snapshot_to()` — the leader's
   existing direct-push behavior is completely unchanged; peer-to-peer catch-
   up is purely additive.

### Requirement 8: Configuration

**User Story:** As an operator tuning a cluster for its network and hardware
characteristics, I want the same kind of explicit, documented configuration
knobs this project already provides for every other timing-sensitive
behavior.

#### Acceptance Criteria

1. `raft_configuration` (`include/raft/types.hpp`) SHALL gain:
   `_progress_gossip_interval` (default 500ms), `_catch_up_gap_threshold`
   (default 50, a `log_index_type`-compatible count), `_catch_up_fetch_max_entries`
   (default 500), and `_catch_up_fetch_timeout` (default 5000ms, matching
   `_append_entries_timeout`'s order of magnitude).
2. Each new field SHALL follow the existing private-member-plus-accessor
   convention (`_field_name` with a `field_name()` getter) and SHALL be
   documented with a `///<` comment, matching every existing
   `raft_configuration` field.

### Requirement 9: Reference implementation and testability

**User Story:** As a developer testing this feature, I want a deterministic,
in-memory `peer2peer_replicator_type` I can wire into the existing
simulator-based test infrastructure, without needing a real network-based
gossip protocol implementation to exist yet.

#### Acceptance Criteria

1. A `static_peer2peer_replicator<NodeId, Address, LogIndex>` SHALL be
   provided (`include/raft/peer2peer_replication.hpp`), mirroring
   `static_peer_discovery`'s shape: backed by a shared, injectable in-memory
   table of advertised progress digests (e.g. a
   `std::shared_ptr<synchronized_map<...>>`, reused across every node
   instance in a simulator-based test cluster) so `advertise_progress`/
   `find_catch_up_source` behave deterministically and observably in tests
   without any real network I/O.
2. A production-grade gossip transport (e.g. genuine epidemic/anti-entropy
   dissemination over a real network, SWIM-style piggybacked failure
   detection, etc.) is explicitly OUT OF SCOPE for this spec — it is left for
   a follow-on spec, mirroring how `node-bootstrap` shipped the abstract
   `peer_discovery` concept plus a `static_peer_discovery` reference/test
   implementation first, with `rfc1035_peer_discovery`/
   `rfc2136_ldns_discovery`/`poco_peer_discovery`/`rfc2136_dns_sd_discovery`
   arriving as separate, later specs under `.kiro/specs/dns-peer-discovery/`.
3. Property tests using the existing network simulator
   (`kythira::simulator_network_client`/`simulator_network_server`,
   `include/raft/simulator_network.hpp`) SHALL cover: a node joining a
   cluster with a large existing log catches up via a peer rather than the
   leader; a partitioned-then-reconnected follower catches up via a peer; a
   catch-up fetch from a source that turns out to be stale/wrong still
   converges correctly (Requirement 6.5); and that with the no-op default,
   catch-up behavior is bit-for-bit identical to today's leader-only path
   (Requirement 1.4/4.4).

### Requirement 10: Metrics and logging parity

**User Story:** As an operator running this cluster in production, I want
peer-to-peer catch-up activity to be as observable as every other replication
path already is, so I can tell whether it's helping and diagnose it if not.

#### Acceptance Criteria

1. A peer-to-peer catch-up attempt SHALL emit metrics analogous to the
   existing `raft_replication_round`/`raft_append_entries_received` metrics
   (e.g. `raft_peer_catch_up_attempt`, `raft_peer_catch_up_success`,
   dimensioned by `node_id` and `source_peer`), following the existing
   `_metrics.set_metric_name(...)`/`add_dimension(...)`/`add_one()`/`emit()`
   call pattern used throughout `raft.hpp`.
2. Gap detection, source discovery, fetch attempts, and outcomes (success,
   `available = false`, consistency-check failure, timeout) SHALL be logged
   at `debug` level via `_logger`, matching the verbosity and structured-
   field style of existing `AppendEntries`/`InstallSnapshot` logging.

## Backwards Compatibility

No existing `Types` bundle, `network_client`/`network_server` implementation,
or test is modified by this spec's default behavior: `peer2peer_replicator_type`
is an optional extension (Requirement 2.2) resolved to a no-op that always
returns `std::nullopt` (Requirement 1.3) when absent, `fetch_log_entries`
support is an optional transport extension (Requirement 5.2) that existing
transports simply don't satisfy, and `replicate_to_followers()`'s leader-push
path is untouched (Requirement 7.3). A cluster built entirely from pre-this-
spec `Types` bundles compiles and behaves identically before and after this
spec lands.
