# Requirements Document

## Introduction

Raft has two optional election RPCs in this codebase, each detected at compile
time by a concept in `include/raft/network.hpp`:

- **RequestPreVote** (`network_client_with_pre_vote`,
  `network_server_with_pre_vote`). Ongaro's dissertation §9.6. A node whose
  election timer fires first asks its peers whether they *would* vote for it,
  and only bumps its term if a majority say yes. This stops a node that was
  partitioned, or just restarted, from forcing a healthy leader to step down
  with a term it climbed while cut off.
- **TimeoutNow** (`network_client_with_timeout_now`,
  `network_server_with_timeout_now`). Dissertation §3.10. The leader tells a
  caught-up follower to campaign immediately. `node<Types>::transfer_leadership()`,
  the placement driver's `transfer_leader` operator, load-split `scatter` and
  the elastic-capacity move off a leader all depend on it.

`node<Types>` uses `if constexpr` on these concepts. A transport without
PreVote goes straight to a real election (`raft.hpp:4088`). A transport
without TimeoutNow fails `transfer_leadership()` with
`leader_transfer_unsupported_exception` (`raft.hpp:2310`), and the multi-Raft
host skips `transfer_leader` and `scatter` as `unsupported`
(`multi_raft_impl.hpp:3216,3241`).

On `main` at 9c738d2 the extensions are implemented as follows:

| Transport | PreVote | TimeoutNow |
|---|---|---|
| gRPC | yes | yes |
| `tcp_rpc` | yes | yes |
| `tls_tcp_rpc` | yes | yes |
| simulator | yes | yes |
| cpp-httplib | **no** | **no** |
| Boost.Beast | **no** | **no** |
| Proxygen | **no** | **no** |
| CoAP libcoap / libnyoci / cantcoap | **no** | **no** on `main`; added by PR #385 |

The HTTP transports define exactly three endpoints
(`http_transport_impl.hpp:37-39`, `beast_http_transport_impl.hpp:28-30`,
`proxygen_http_transport.hpp:176-178`). No CoAP backend mentions `pre_vote`.

Two consequences follow, and neither is written down as a decision anywhere:

1. **Every HTTP and CoAP cluster is exposed to the disruptive-server problem
   PreVote exists to fix.** A node that rejoins after a partition forces an
   election on every one of these transports. The coap-transport-multi-raft
   design records the CoAP gap in a capability table (§2) and asserts it with
   a negative `static_assert`, but gives no reason. The HTTP gap is recorded
   nowhere.
2. **`cmd/multi_raft_node` cannot move leadership.** It speaks only
   cpp-httplib, Beast or Proxygen (`cmd/multi_raft_node/config.cpp:68`). So
   PD `transfer_leader`, `scatter`, and the elastic-capacity move off a
   leader always report `unsupported`
   (`.kiro/specs/elastic-shard-capacity/design.md:637`,
   `.kiro/specs/multi-raft-performance/requirements.md:107-116`).

This spec adds both RPCs to all three HTTP transports and PreVote to all three
CoAP backends, so every wire transport supports the same election RPCs. It
also makes PreVote safe to roll out into a cluster whose peers do not have it
yet, which none of the earlier PreVote work had to consider because it only
reached transports that were never deployed in mixed versions.

### Sources

- `/mnt/project-files/audits/parallel-implementation-parity-audit.md`, gap T5
  and "Highest-value unowned gaps" item 10.
- `/mnt/project-files/audits/spec-gap-analysis-2026-10-02.md`, multi-raft
  item 1 (T29), the elastic-capacity move-off-leader finding, and the transport-concept
  finding that no spec covers the optional RPCs on HTTP.

### Prerequisites

- **PR #385** (coap-transport-multi-raft) adds TimeoutNow to all three CoAP
  backends, the `/raft/timeout_now` resource, per-RPC CoAP reliability,
  `tests/coap_capability_table.hpp`, and the positive and negative concept
  `static_assert`s at the end of each backend header. Requirements 5 and 6
  build on those and start once #385 has merged. The HTTP requirements do not
  depend on it.

### Non-goals

- **ClusterJoin/ClusterLeave and FetchLogEntries on HTTP or CoAP** (parity
  audit T6). They are bootstrap and catch-up features with their own design
  questions (address-keyed rather than node-keyed dispatch). They can follow
  this pattern in their own spec.
- **cpp-httplib's ignored per-call timeout** (spec gap analysis, HTTP item 1).
  PreVote and TimeoutNow inherit whatever timeout behaviour the existing three
  RPCs have; fixing it is separate work.
- **Changing PreVote or TimeoutNow semantics in `node<Types>`.** The handlers,
  the leader-stickiness check and the "deferred campaign never pre-votes" rule
  (`raft.hpp:2617`) are unchanged. The only core change is how a pre-vote
  round counts a peer that does not implement the RPC (Requirement 4).
- **A PreVote on/off switch.** PreVote follows the transport's concepts, as
  it already does on gRPC and TCP.

## Glossary

- **Extension RPC**: RequestPreVote or TimeoutNow, the two RPCs this spec
  adds.
- **HTTP transports**: `cpp_httplib_client`/`cpp_httplib_server`,
  `boost_beast_client`/`boost_beast_server`, `proxygen_client`/
  `proxygen_server`.
- **CoAP backends**: the libcoap, libnyoci and cantcoap implementations of
  `coap_client`/`coap_server`.
- **Older peer**: a node running a build whose transport lacks an extension
  RPC. It answers that RPC's path with HTTP 404 or CoAP 4.04 Not Found.
- **Unregistered handler**: a server that has the endpoint but no handler
  registered for it yet, for example before `node<Types>::start()` runs.
- **Not-implemented outcome**: the client-side result of calling an extension
  RPC on an older peer or an unregistered handler. It is reported as
  `kythira::rpc_not_implemented_exception` (Requirement 4.1).

## Requirements

### Requirement 1: HTTP endpoints

**User Story:** As an operator running Raft over HTTP, I want PreVote and
leadership transfer, so that a rejoining node does not depose a healthy leader
and the placement driver can move leaders.

#### Acceptance Criteria

1. THE HTTP transports SHALL serve `POST /v1/raft/request_pre_vote` and
   `POST /v1/raft/timeout_now`, alongside the existing three endpoints and
   with the same path prefix.
2. THE HTTP clients SHALL provide `send_request_pre_vote(target, request,
   timeout)` and `send_timeout_now(target, request, timeout)`, and THE HTTP
   servers SHALL provide `register_request_pre_vote_handler(handler)` and
   `register_timeout_now_handler(handler)`, with the signatures the concepts
   in `network.hpp` require.
3. THE new endpoints SHALL use the same send path, content negotiation (415 /
   406 handling and the per-peer capability cache), body-size limit, TLS and
   mTLS, metrics, and error mapping as `request_vote`. There SHALL be no
   per-endpoint special case beyond the path and the message types.
4. THE `rpc_type` metric dimension SHALL be `request_pre_vote` and
   `timeout_now` for the new endpoints; it is empty for them today.
5. THE serializers SHALL NOT change: every serializer already encodes and
   decodes `request_pre_vote_*` and `timeout_now_*` (JSON, CBOR, Ion,
   protobuf).

### Requirement 2: HTTP concept conformance is asserted both ways

**User Story:** As a maintainer, I want the compiler to tell me which
extensions each transport has, so that a lost extension fails the build
instead of surfacing as an `unsupported` skip in production.

#### Acceptance Criteria

1. EACH HTTP transport header SHALL end with `static_assert`s that its client
   satisfies `network_client_with_pre_vote` and
   `network_client_with_timeout_now`, and its server the matching server
   concepts.
2. EACH SHALL also assert, negatively, that it does NOT satisfy
   `network_client_with_log_fetch`, `network_client_with_cluster_join` or
   `network_client_with_cluster_leave`, with a message pointing at this spec's
   design, matching the pattern #385 uses for CoAP.
3. `multi_group_network_server` over each HTTP transport SHALL register the
   pre-vote and timeout-now handlers. This follows from the concepts
   (`group_transport.hpp:257,272`) and SHALL be checked by a test, not assumed.

### Requirement 3: Unregistered handlers answer "not implemented"

**User Story:** As an operator, I want a peer that cannot serve an extension
RPC to say so distinctly, so that the caller can tell "not supported" from
"failed".

#### Acceptance Criteria

1. WHEN an HTTP server receives a request for an extension RPC whose handler
   is not registered, THEN it SHALL answer **501 Not Implemented**. The three
   mandatory RPCs keep today's 500.
2. WHEN a CoAP server receives a request for an extension RPC whose handler
   is not registered, THEN it SHALL answer **5.01 Not Implemented**, which is
   what #385 already does for `/raft/timeout_now`.
3. THE HTTP clients SHALL map 404 and 501 on an extension RPC, and THE CoAP
   clients SHALL map 4.04 and 5.01 on an extension RPC, to
   `rpc_not_implemented_exception`. Any other status keeps today's mapping.
4. A 404 or 4.04 on a mandatory RPC SHALL keep today's mapping. A peer missing
   `request_vote` is misconfigured, not older.

### Requirement 4: PreVote is safe in a mixed-version cluster

**User Story:** As an operator doing a rolling upgrade, I want upgraded nodes
to keep electing leaders while some peers still run the old build, so that
the upgrade cannot cost the cluster its liveness.

Today a failed pre-vote reply counts as a refusal (`start_pre_vote()` counts
only `vote_granted()` values). Without this requirement, an upgraded HTTP or
CoAP node would get "no" from every older peer and could only ever campaign
if a majority of the cluster were already upgraded.

#### Acceptance Criteria

1. THE library SHALL define `kythira::rpc_not_implemented_exception` in
   `include/raft/exceptions.hpp`, carrying the RPC name and the target node.
2. `error_handler::classify_error()` SHALL classify it as non-retryable, so a
   pre-vote to an older peer costs one round trip, not three.
3. WHEN a pre-vote round collects `rpc_not_implemented_exception` from a peer,
   THEN `start_pre_vote()` SHALL count that peer as having granted the
   pre-vote. This is exactly the pre-PreVote behaviour for that peer, and it
   is safe because a pre-vote grant changes no state: the real election that
   follows still needs real RequestVote grants.
4. THE node SHALL log a warning naming the peer the first time it sees the
   not-implemented outcome from it, and SHALL emit a counter
   (`raft.pre_vote.peer_not_implemented`) on every occurrence, so a peer stuck
   on an old build is visible. It SHALL NOT log on every round.
5. Timeouts, refused connections and every other error SHALL keep counting
   as a refusal, as today.
6. WHEN `transfer_leadership()`'s TimeoutNow gets the not-implemented outcome,
   THEN the transfer SHALL fail with `leader_transfer_unsupported_exception`,
   so the multi-Raft host records the skip as `unsupported`, the same reason
   it records for a transport without the concept.
7. THE fallback SHALL apply to every transport that reports the outcome, not
   only HTTP and CoAP. Whether gRPC (UNIMPLEMENTED) and `tcp_rpc` report it is
   recorded in the design; changing them is not required.

### Requirement 5: CoAP PreVote

**User Story:** As an operator running Raft over CoAP, I want PreVote on
whichever CoAP backend I build, so that the choice of backend does not decide
whether my cluster tolerates a rejoining node.

Starts once PR #385 has merged.

#### Acceptance Criteria

1. ALL three CoAP backends SHALL serve `/raft/request_pre_vote` and provide
   `send_request_pre_vote` / `register_request_pre_vote_handler`.
2. THE resource SHALL be registered with the same flags as
   `/raft/request_vote`, including `COAP_RESOURCE_FLAGS_OSCORE_ONLY` under
   OSCORE on libcoap, and with the same DTLS, OSCORE, ACE and duplicate
   detection treatment.
3. Pre-vote requests SHALL use the same reliability as RequestVote (the
   configured `use_confirmable_messages`), NOT TimeoutNow's forced CON. A lost
   pre-vote is retried by `start_pre_vote()` and costs at most one election
   timeout, the same as a lost RequestVote.
4. EACH backend header's `static_assert` for pre-vote SHALL flip from negative
   to positive, and `tests/coap_capability_table.hpp` and the
   coap-transport-multi-raft design §2 table SHALL say "yes" for pre-vote.

### Requirement 6: Tests

**User Story:** As a maintainer, I want the new RPCs exercised end to end, so
that concept conformance is not the only evidence they work.

#### Acceptance Criteria

1. `tests/three_way_http_transport_equivalence_test.cpp` SHALL round-trip a
   pre-vote and a timeout-now request through all three HTTP transports and
   compare the responses, including `group_id`.
2. A test SHALL run a three-node `node<Types>` cluster over cpp-httplib (the
   `cmd/multi_raft_node` default) and show: the log line `Starting pre-vote
   round` on an election; a node isolated for several election timeouts and
   then reconnected does NOT raise the leader's term; and
   `transfer_leadership()` moves leadership to the named target.
3. A test SHALL show the mixed-version fallback: a node whose peers answer 404
   (HTTP) and 4.04 (CoAP) for the pre-vote path still wins an election, counts
   them as granted, and logs one warning per peer.
4. A test SHALL show that a 501 from an unregistered handler maps to
   `rpc_not_implemented_exception` on each HTTP transport and on libcoap.
5. A multi-Raft host test over cpp-httplib SHALL apply a `transfer_leader`
   operator and a `scatter`, and both SHALL end `accepted` and move
   leadership, not `unsupported`. The elastic-capacity scenario's
   move-off-leader path (`tests/docker_chaos/elastic_capacity_docker_test.cpp`)
   SHALL no longer leave the shard one voter over.
6. THE CoAP integration tests for each backend SHALL gain a pre-vote case
   next to #385's timeout-now case. Where a backend cannot run in CI (the
   parity audit's C1: cantcoap and libnyoci are not built there), the tasks
   SHALL say so rather than mark the case verified.

### Requirement 7: Documentation

#### Acceptance Criteria

1. THE HTTP transport documentation SHALL list five endpoints and say which
   extensions each transport implements.
2. `.kiro/specs/multi-raft-performance/requirements.md` (the "only the three
   mandatory RPCs" note), `.kiro/specs/elastic-shard-capacity/design.md`
   (§15 amendment 10, move-off-leader on cpp-httplib) and the coap-transport-multi-raft
   design table SHALL point at this spec instead of describing the gap as
   current.
3. `doc/CHANGELOG.md` SHALL record the rolling-upgrade behaviour of
   Requirement 4 under the release that ships it.
