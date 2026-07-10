# Peer-to-Peer Gossip Transport Requirements Document

## Introduction

`.kiro/specs/peer2peer-log-replication/` defines the `peer2peer_replicator`
concept and ships exactly two implementations: `no_op_peer2peer_replicator`
(preserves today's leader-only behavior) and `static_peer2peer_replicator`
(an in-memory, shared-table stand-in for tests). Its own Requirement 9.2 and
design.md Non-Goals explicitly defer "a production-grade gossip transport...
to a follow-on spec, mirroring how `node-bootstrap` shipped the abstract
`peer_discovery` concept plus a `static_peer_discovery` reference/test
implementation first, with `rfc1035_peer_discovery`/`rfc2136_ldns_discovery`/
`poco_peer_discovery`/`rfc2136_dns_sd_discovery` arriving as separate, later
specs under `.kiro/specs/dns-peer-discovery/`."

This document is that follow-on: `tcp_gossip_peer2peer_replicator`, a real
network-based implementation of `peer2peer_replicator` using a standard
anti-entropy gossip protocol (periodic randomized push-pull digest exchange —
the style used by Cassandra's and the original Amazon Dynamo's gossip
layers), not a full membership/failure-detection protocol like SWIM. That
distinction is deliberate: Raft already detects liveness through its own
election timeouts and RPC failures — this transport's only job is best-effort
dissemination of small `(node_id, term, last_log_index)` progress digests
among peers whose *addresses* are already known. Discovering unknown peers is
a separately solved problem (`peer_discovery_type`,
`.kiro/specs/dns-peer-discovery/`); this spec depends only on being told the
cluster's existing member list, exactly like `static_peer_discovery` already
is.

Two decisions carry over unchanged from the depended-on spec: the leader
remains the sole commit authority (this spec transports progress digests
only, never log entries — those still travel over `fetch_log_entries`,
already specified), and gossip failures are best-effort and never affect
Raft correctness (a `tcp_gossip_peer2peer_replicator` that can't reach any
peer this round behaves exactly like `no_op_peer2peer_replicator` for that
round — `find_catch_up_source` returns what it currently has, possibly
nothing).

A third decision is new to this spec, informed directly by this project's own
CI history: `ca_cluster_node_test`
(`.kiro/specs/certificate-authority/`) — a real 3-process Raft cluster test —
was, as of this writing, the single largest source of CI flakiness in this
repository, because running several real OS processes with real election
timers under `ctest -j$(nproc)` CPU contention made their timing
unpredictable (see the CI-reliability fix landed alongside
`doc/TODO.md`'s Build Tooling section). This spec's own test strategy
(Requirement 10) deliberately avoids reproducing that pattern: it validates
the real gossip transport over real TCP sockets and a real background thread,
but keeps every test in a single process with multiple
`tcp_gossip_peer2peer_replicator` instances rather than spawning subprocesses
— real enough to catch real transport/serialization/scheduling bugs, without
reintroducing the specific flakiness shape this project has already paid to
fix once.

## Glossary

- **Known peer list**: the static, out-of-band-configured
  `(node_id, address)` mapping this implementation is constructed with —
  the same kind of list `static_peer_discovery` takes and the same
  `--peers` information every existing Raft binary (`ca_cluster_node`,
  `chaos_node`, `dns_discovery_node`) already requires.
- **Gossip round**: one iteration of the background gossip thread: select a
  small random subset of known peers (the *fanout*) and perform a push-pull
  digest exchange with each.
- **Fanout**: the number of peers contacted in a single gossip round
  (default 3 — informed by this project's own documented cluster sizes,
  3–7 nodes, where contacting most or all peers each round is itself
  cheap; a larger derived fanout is not needed at this scale).
- **Push-pull exchange**: one `gossip_exchange` round-trip with a single
  peer — the requester sends its entire local digest table, the responder
  replies with its entire local digest table, and both sides merge what
  they received into their own table.
- **Digest**: `(node_id, address, term, last_log_index, fresh_until)` — a
  peer's self-reported progress plus a freshness deadline, never log
  entries or command payloads.
- **`fresh_until`**: an epoch-seconds deadline after which a digest is
  pruned from a node's local table if it has not been refreshed — the same
  pattern already used by `rfc2136_dns_sd_discovery`
  (`include/raft/rfc2136_dns_sd_discovery.hpp`) for exactly this purpose
  (letting stale entries from crashed nodes, which never explicitly
  deregister, eventually disappear).

## Requirements

### Requirement 1: `tcp_gossip_peer2peer_replicator` satisfies `peer2peer_replicator`

**User Story:** As a `node<Types>` user who already adopted the
`peer2peer_replicator_type` extension against `static_peer2peer_replicator`
for testing, I want a real implementation with the identical public
interface, so switching from the test stand-in to production is a one-line
type change.

#### Acceptance Criteria

1. `tcp_gossip_peer2peer_replicator<NodeId, Address, LogIndex>`
   (`include/raft/tcp_gossip_transport.hpp`, new file) SHALL satisfy the
   `peer2peer_replicator` concept defined in
   `include/raft/peer2peer_replication.hpp` (verified by `static_assert`,
   matching that header's existing pattern for its own two implementations).
2. `advertise_progress(self_id, self_address, term, last_log_index)` SHALL
   update this node's own entry in its local digest table (setting a fresh
   `fresh_until` deadline) and SHALL resolve immediately — the actual
   network dissemination happens asynchronously on the background gossip
   thread's own schedule (Requirement 4), not synchronously within this
   call, matching Requirement 3.3 of the depended-on spec ("never blocks or
   delays any Raft-critical operation").
3. `find_catch_up_source(from_index, to_index, timeout)` SHALL return the
   first peer in the current local digest table (after pruning expired
   entries, Requirement 6.3) whose `last_log_index >= from_index` AND whose
   `node_id` is currently in this instance's active-membership set
   (Requirement 2.3), excluding this node's own entry — `timeout` is
   accepted for interface compatibility but this lookup is a synchronous
   local-table read, never itself performing network I/O (the network I/O
   already happened, or is already scheduled to happen, via the background
   gossip thread).
4. `update_membership(member_ids)` SHALL replace this instance's active-
   membership set with `member_ids` and SHALL resolve immediately — it is a
   synchronous local-table write, exactly like `advertise_progress`
   (Requirement 1.2). This is `node<Types>`'s only mechanism for telling
   this transport who is currently a cluster member (Requirement 2).

### Requirement 2: Membership from the replicated log, addresses from a static book

**User Story:** As an operator, I want this transport's gossip peer set to
automatically track `add_server()`/`remove_server()` the same way every
other part of `node<Types>` already does, rather than maintaining a second,
independently-drifting peer list — while still being told, once, how to
reach any node ID that might ever be a member, since the replicated log
itself carries no network addresses.

#### Acceptance Criteria

1. `cluster_configuration<NodeId>` (`include/raft/types.hpp`) carries only
   node IDs — `nodes()`, `old_nodes()`, `learners()` — never addresses, so
   there is no way for this transport to derive `(node_id, address)` pairs
   from the log alone. `tcp_gossip_peer2peer_replicator` SHALL therefore
   still be constructed with a static `tcp_gossip_config::address_book`
   (`std::vector<peer_info<NodeId, Address>>`, `Address` resolving to a
   `host:port` pair) — but this is address-resolution data only, NOT a
   statement of current membership.
2. **Current membership** SHALL instead come exclusively from
   `update_membership()` calls (Requirement 1.4), driven by
   `node<Types>::cluster_members()` (`.kiro/specs/peer2peer-log-replication/`
   Requirement 11) — the replicated log's own core cluster membership.
   `tcp_gossip_config` SHALL NOT contain a separate "current members" field;
   an implementation that has never received an `update_membership` call
   (e.g. immediately after construction, before `node<Types>` has started)
   SHALL behave as if the active-membership set is empty — gossip rounds
   select no peers, `find_catch_up_source` never returns a result — until
   the first call arrives.
3. Each gossip round's fanout selection (Requirement 4.2) SHALL draw from
   the **intersection** of the current active-membership set (Requirement
   1.4) and `address_book`'s keys — a member ID absent from `address_book`
   (no known address) SHALL be logged at debug level and skipped for that
   round, not treated as an error; a stale `address_book` entry for a
   node ID no longer in the active-membership set SHALL simply never be
   selected (Requirement 2.2 already ensures this).
4. Provisioning `address_book` with an address for a brand-new member
   before that member's own `add_server()` configuration entry is even
   proposed (so a gossip-capable address exists the moment the entry
   commits) is an operational/deployment concern, not something this
   transport solves — see design.md's Non-Goals for the natural integration
   point (`ClusterJoin`'s address-carrying request,
   `.kiro/specs/node-bootstrap/`) that a later spec could use to
   auto-populate `address_book`, deliberately left out of scope here.

### Requirement 3: Self-contained TCP gossip listener

**User Story:** As a maintainer, I want the gossip transport to be its own
independent channel, not entangled with the Raft-critical RPC transport, so
a bug or overload in gossip can never affect consensus traffic and vice
versa.

#### Acceptance Criteria

1. `tcp_gossip_peer2peer_replicator` SHALL open its own TCP listener on
   `listen_port`, entirely independent of whatever `network_client_type`/
   `network_server_type` the owning `node<Types>` is configured with — it
   SHALL NOT extend `network.hpp`'s `network_client`/`network_server`
   concepts or reuse `fetch_log_entries`'s transport in any way, matching
   how every existing `peer_discovery_type` implementation
   (`rfc1035_peer_discovery`, `poco_peer_discovery`, etc.) is already
   independent of the Raft RPC transport.
2. The listener's accept loop and connection handling SHALL reuse
   `tcp_rpc.hpp`'s existing `tcp_detail::connect_to`/`frame_send`/
   `frame_recv`/`bytes_to_str`/`str_to_bytes` free functions
   (already `inline`, header-only, and reused once already by
   `tls_tcp_rpc.hpp` per `.kiro/specs/ca-cluster-rpc-mtls/`) rather than
   duplicating TCP framing logic.
3. `tcp_rpc.hpp`, `tcp_rpc_client`, `tcp_rpc_server`, and `network.hpp`
   SHALL remain byte-for-byte unmodified by this spec.

### Requirement 4: Background gossip round thread

**User Story:** As a cluster member, I want my progress digest table to stay
continuously refreshed against my peers' without any explicit action from
`node<Types>` beyond the `advertise_progress` calls it already makes.

#### Acceptance Criteria

1. On construction, `tcp_gossip_peer2peer_replicator` SHALL start a
   dedicated background thread running a gossip loop, following the exact
   `start_fresher()`/`stop_fresher()`/`fresher_loop()` shape already
   established by `rfc2136_dns_sd_discovery`
   (`include/raft/rfc2136_dns_sd_discovery.hpp:456-493`); on destruction it
   SHALL be signaled to stop and joined before the object is destroyed.
2. Each gossip round (cadence: `gossip_round_interval`, Requirement 9), the
   thread SHALL compute the eligible peer set as the intersection of the
   current active-membership set (Requirement 1.4/2.2) and
   `address_book`'s keys (Requirement 2.3), excluding self, then select
   `min(fanout, eligible.size())` of them uniformly at random and perform a
   push-pull exchange (Requirement 5) with each, sequentially or
   concurrently at the implementation's discretion. WHEN the eligible set is
   empty (no `update_membership` call received yet, or no member has a
   known address), the round is a no-op — logged at debug level, not an
   error.
3. A gossip round SHALL run even if `advertise_progress` was not called
   since the previous round — it always sends whatever is currently in the
   local table, including this node's own last-advertised entry.
4. Thread lifecycle errors (e.g. the listener socket failing to bind) SHALL
   be surfaced from the constructor as a thrown exception — fail loudly at
   construction time rather than silently running with no working gossip,
   matching this project's existing fail-closed posture for other
   correctness-adjacent startup checks (e.g. `--auth-token`/
   `--unseal-key-file` in `ca_cluster_node`).

### Requirement 5: `gossip_exchange` wire protocol

**User Story:** As a developer extending this transport later, I want the
wire format to be simple and self-describing, not require touching the core
Raft RPC serializer.

#### Acceptance Criteria

1. A `gossip_exchange_request`/`gossip_exchange_response` pair SHALL be
   defined (implementation file, not `types.hpp` — this is not a Raft
   consensus RPC and SHALL NOT be added to `raft_types`, `network_client`,
   or `network_server`), each carrying: `sender_node_id` and a
   `std::vector<gossip_digest<NodeId, Address, LogIndex>>` (the sender's
   complete current local table).
2. Encoding SHALL use `boost::json` directly (already a project dependency,
   already used for exactly this kind of small self-contained wire format
   by `ca_state_machine`'s command encoding,
   `include/raft/ca_state_machine.hpp`) rather than extending
   `json_rpc_serializer`, which is scoped to the three core Raft RPCs.
3. Framing over the wire SHALL reuse `tcp_detail::frame_send`/`frame_recv`
   (Requirement 3.2) — `[4-byte big-endian length][JSON payload]`,
   identical to every other TCP-framed protocol in this project.
4. A responder receiving a `gossip_exchange_request` it cannot parse (schema
   mismatch, corrupt payload) SHALL close the connection without a response
   rather than crash — the requester treats this identically to a
   connection failure (Requirement 8).

### Requirement 6: Digest merge and freshness pruning

**User Story:** As a node forming a view of cluster-wide progress from
gossip, I want the merge to prefer newer information without needing perfect
clock synchronization or a separate coordination mechanism.

#### Acceptance Criteria

1. WHEN merging an incoming digest for a `node_id` already present in the
   local table, the incoming digest SHALL replace the local one if and only
   if `(incoming.term, incoming.last_log_index)` is lexicographically
   greater than `(local.term, local.last_log_index)` — higher term always
   wins; equal term compares `last_log_index`. A digest for a `node_id` not
   yet present SHALL always be added.
2. This merge rule does not need to be perfectly monotonic in the face of a
   node that loses persisted state and restarts from scratch (dropping back
   to a lower term/index) — Property 3 of
   `.kiro/specs/peer2peer-log-replication/design.md` already establishes
   that a stale or "wrong" catch-up source can only cause wasted local
   work on the consumer side, never a safety violation, so this spec does
   not need additional machinery to guarantee strict monotonicity beyond
   6.1's simple comparison.
3. On every gossip round, before selecting fanout peers, the local table
   SHALL be pruned of any digest whose `fresh_until` has passed —
   mirroring `rfc2136_dns_sd_discovery`'s exact freshness-expiry pattern —
   so a crashed peer's last-known progress eventually stops being offered
   as a catch-up source.
4. `advertise_progress` (Requirement 1.2) SHALL set this node's own entry's
   `fresh_until` to `now + freshness_interval` (Requirement 9); a
   `gossip_exchange` push-pull (Requirement 5) SHALL propagate each
   digest's already-set `fresh_until` unchanged — freshness deadlines are
   set once, at the originating node, and travel with the digest, not
   reset by every hop.

### Requirement 7: Interaction with `fetch_log_entries`

**User Story:** As a lagging node whose `find_catch_up_source` call returned
a peer, I want that peer to actually be reachable via the existing
`fetch_log_entries` RPC path from the depended-on spec, not a separate
gossip-specific fetch mechanism.

#### Acceptance Criteria

1. `find_catch_up_source`'s returned `peer_info<NodeId, Address>`
   `Address` field SHALL be the peer's Raft RPC address (the same address
   `network_client_type`/`network_server_type` uses to reach it for
   `fetch_log_entries`) — NOT this transport's own gossip-listener address.
   `tcp_gossip_config::address_book` (Requirement 2.1) therefore carries
   Raft RPC addresses, matching `find_catch_up_source`'s documented
   contract in the depended-on spec (Requirement 1.2 there).
2. This spec introduces no new entry-fetching mechanism — the actual pull
   of log entries from the returned peer continues to go through
   `node<Types>::maybe_catch_up_from_peer()` and `fetch_log_entries`
   exactly as specified in `.kiro/specs/peer2peer-log-replication/`.

### Requirement 8: Fault tolerance

**User Story:** As an operator, I want a network blip, an unreachable peer,
or a gossip-listener crash to degrade gracefully to "peer-to-peer catch-up
temporarily unavailable," never to a Raft-affecting failure.

#### Acceptance Criteria

1. A failed push-pull exchange with one peer (connection refused, timeout,
   malformed response) SHALL be logged and SHALL NOT abort the current
   gossip round for the remaining selected peers, nor prevent future
   rounds.
2. If every peer in a given round is unreachable, the node's local table
   simply does not gain new information that round — it is not treated as
   an error condition distinct from a normal partial-information round; the
   next round tries again.
3. None of this transport's failure modes SHALL propagate as an exception
   out of `advertise_progress`/`find_catch_up_source` (Requirement 1) —
   both remain best-effort and always resolve, matching
   `no_op_peer2peer_replicator`'s own contract of "always succeeds/always
   resolves," just with actual gossiped data when available instead of
   none.

### Requirement 9: Configuration

**User Story:** As an operator tuning gossip overhead against catch-up
responsiveness, I want explicit, documented knobs, matching this project's
existing configuration conventions.

#### Acceptance Criteria

1. `tcp_gossip_config` SHALL include: `fanout` (default 3),
   `gossip_round_interval` (default 500ms — matching the depended-on
   spec's `raft_configuration::progress_gossip_interval` default, so a
   node's self-advertisement and this transport's dissemination cadence
   are aligned by default), and `freshness_interval` (default 5s — 10x the
   round interval, tolerating several consecutive missed rounds before a
   peer's digest is pruned, mirroring `rfc2136_dns_sd_discovery`'s
   `freshness_interval` being refreshed at half its own value well before
   expiry).
2. Every field SHALL be documented with a `///<` comment, matching this
   project's existing configuration-struct convention.

### Requirement 10: Testing strategy (and the explicit anti-flakiness constraint)

**User Story:** As a maintainer who has already spent effort diagnosing and
fixing this project's worst source of CI flakiness (a real multi-process
Raft cluster test), I want this spec's own tests to validate the real
transport without reintroducing that same flakiness shape.

#### Acceptance Criteria

1. Unit tests SHALL cover the merge rule (Requirement 6.1), freshness
   pruning (Requirement 6.3), and fanout peer selection, without any
   network I/O — pure logic against an in-memory table.
2. Integration tests SHALL use real TCP sockets and the real background
   gossip thread (Requirement 4), with multiple
   `tcp_gossip_peer2peer_replicator` instances constructed **in a single
   test process** on distinct loopback ports — real enough to catch actual
   framing/serialization/threading bugs, but explicitly NOT spawning
   separate OS processes (unlike `ca_cluster_node_test.cpp`'s
   `posix_spawn`-based pattern), so CPU-scheduling contention between
   independent processes under `ctest -j$(nproc)` — the specific mechanism
   diagnosed as this project's dominant CI flake source — cannot recur
   here.
3. An end-to-end property test SHALL wire real
   `tcp_gossip_peer2peer_replicator` instances as the
   `peer2peer_replicator_type` for a multi-node `node<Types>` cluster whose
   Raft RPC transport (`network_client_type`/`network_server_type`)
   remains the existing in-process network simulator
   (`simulator_network_client`/`server`) — deliberately keeping Raft
   consensus itself on its already-deterministic simulated transport while
   only the gossip layer runs over real sockets, so this test exercises
   the new transport under real conditions without compounding two
   independent sources of network/scheduling nondeterminism at once.
4. The property test in 10.3 SHALL cover: a lagging node's digest table
   converges to reflect a peer's advertised progress within a small,
   bounded number of gossip rounds; a node that stops advertising (goes
   silent, simulating a crash) has its digest pruned from other nodes'
   tables after `freshness_interval` elapses; `find_catch_up_source`
   returns a usable peer once the table reflects sufficient progress.

## Non-Goals

- **SWIM-style failure detection, indirect probing, or suspicion states.**
  Raft's own election timeout and RPC failure handling already detect
  unreachable peers for consensus purposes; this transport only
  disseminates progress digests and explicitly does not attempt to be a
  general-purpose membership/failure-detection protocol.
- **Delta/digest-compressed gossip (Merkle-tree summaries, version
  vectors beyond the simple `(term, last_log_index)` comparison in
  Requirement 6.1).** This project's documented cluster sizes (3–7 nodes)
  make sending each node's complete small table every round cheap; that
  technique exists to bound gossip bandwidth on much larger clusters and is
  unneeded here.
- **Live address discovery for brand-new members.** *Membership* itself is
  now dynamic (Requirement 2.2, driven by the replicated log via
  `update_membership`) — that was this spec's whole correction from an
  earlier static-`known_peers` design. What remains static is `address_book`,
  the node-ID-to-address resolution data (Requirement 2.1), since addresses
  aren't log data. Auto-provisioning `address_book` for a node the moment it
  joins (e.g. by observing `ClusterJoin` requests, `.kiro/specs/node-bootstrap/`,
  which do carry an address) is a reasonable follow-on but is explicitly out
  of scope here (Requirement 2.4).
- **Wiring this transport into any specific production binary's CLI**
  (`ca_cluster_node`, `chaos_node`, etc.). This spec ships a usable,
  independently-testable library component; adopting it in a specific
  binary (new `--gossip-port`/`--gossip-fanout` flags, deployment docs,
  etc.) is a natural follow-on but is out of scope here, matching how the
  depended-on spec itself never modified any existing binary either.
