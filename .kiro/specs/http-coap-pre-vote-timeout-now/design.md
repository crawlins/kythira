# Design Document

## Overview

Three pieces, in dependency order:

1. **HTTP**: two more endpoints on each of cpp-httplib, Beast and Proxygen,
   reusing the generic send and dispatch paths that already carry the three
   mandatory RPCs.
2. **Core**: a `rpc_not_implemented_exception`, and one rule in
   `start_pre_vote()` that counts it as a grant, so PreVote can be rolled out
   one node at a time.
3. **CoAP**: `/raft/request_pre_vote` on all three backends, transcribed from
   #385's `/raft/timeout_now`.

No message type, serializer or Raft rule changes. The work is plumbing plus
one deliberate liveness decision (§3).

## 1. Why the gap exists, and why it is not deliberate

PreVote reached transports one at a time, each in the commit that needed it:
`tcp_rpc` first, then `tls_tcp_rpc` and the simulator together
(`doc/TODO.md`, "PreVote extended to `tls_tcp_rpc_*` and the in-memory
simulator"), then gRPC. TimeoutNow arrived in one commit, "carry TimeoutNow on
every wire transport", which covered gRPC, `tcp_rpc` and `tls_tcp_rpc` and left
out HTTP and CoAP. #385 then added it to CoAP.

None of those commits, and no spec, says HTTP or CoAP should lack either RPC.
The coap-transport-multi-raft design §2 asserts CoAP's lack of pre-vote with a
`static_assert` and writes "absent extensions are not a defect … only a defect
when undocumented", then does not document why. The multi-raft-performance
requirements describe the HTTP gap as a fact of the bench matrix, not a
choice. So the gap is an accident of history, and the fix is to close it, not
to write down a rationale.

## 2. HTTP transports

### 2.1 Endpoints

| Path | Request | Response |
|---|---|---|
| `/v1/raft/request_vote` | existing | |
| `/v1/raft/append_entries` | existing | |
| `/v1/raft/install_snapshot` | existing | |
| `/v1/raft/request_pre_vote` | `request_pre_vote_request<>` | `request_pre_vote_response<>` |
| `/v1/raft/timeout_now` | `timeout_now_request<>` | `timeout_now_response<>` |

The constants go next to the existing three in each transport
(`endpoint_*` in `http_transport_impl.hpp`, `beast_endpoint_*`,
`proxygen_detail::proxygen_endpoint_*`). The names match the gRPC methods and
the `tcp_rpc` type tags, so a capture or a metric reads the same way on every
transport.

### 2.2 Client

Each client gains two members that are one-line calls into its existing
generic `send_rpc<Request, Response>(target, endpoint, request, timeout)`:

```cpp
// Satisfies kythira::network_client_with_pre_vote.
auto send_request_pre_vote(std::uint64_t target, const request_pre_vote_request<>& request,
                           std::chrono::milliseconds timeout)
    -> typename Types::template future_template<request_pre_vote_response<>> {
    return send_rpc<request_pre_vote_request<>, request_pre_vote_response<>>(
        target, endpoint_request_pre_vote, request, timeout);
}
```

`send_timeout_now` is the same shape. Because they go through `send_rpc`, they
get content negotiation, the 415 retry walk, TLS and the capability cache with
no extra code. The `rpc_type` if-chain at the top of `send_rpc` gains the two
names (Requirement 1.4). Replacing that chain with a lookup table is tempting
but out of scope.

### 2.3 Server

Each server gains two `std::function` members, two `register_*_handler`
methods and two route registrations, mirroring `request_vote`. cpp-httplib's
`setup_endpoints()` gets two more `server.Post(...)` calls. Beast and Proxygen
dispatch on the path in one function, which gains two branches.

The only behavioural difference from the mandatory RPCs is the unregistered
handler (Requirement 3.1): `handle_rpc_endpoint` takes a flag saying whether
the RPC is an extension, and answers 501 rather than 500 when it is and the
handler is empty. A server that cannot serve an RPC is the textbook meaning of
501, and keeping 500 for the mandatory three avoids changing any existing
test's expectations.

### 2.4 Client-side status mapping

Today `send_rpc` maps every 4xx to an `http_client_error` and every 5xx to an
`http_server_error` carrying the status. The extension wrappers catch exactly
two cases and rethrow:

| Status on an extension RPC | Result |
|---|---|
| 404 | `rpc_not_implemented_exception` (an older peer: no route) |
| 501 | `rpc_not_implemented_exception` (no handler registered) |
| anything else | unchanged |

The mapping lives in the extension wrappers, not in `send_rpc`, so a 404 on
`/v1/raft/request_vote` still means "misconfigured peer" (Requirement 3.4).

A 404 is ambiguous in principle: a reverse proxy in front of the node could
answer 404 for a path it does not forward. That is still "this peer cannot
serve the RPC", which is the condition the fallback in §3 is for, so treating
it the same way is correct.

### 2.5 Concept assertions

Each HTTP header ends with the block #385 introduced for CoAP:

```cpp
static_assert(kythira::network_client_with_pre_vote<cpp_httplib_client<default_http_types>>);
static_assert(kythira::network_client_with_timeout_now<cpp_httplib_client<default_http_types>>);
static_assert(kythira::network_server_with_pre_vote<cpp_httplib_server<default_http_types>>);
static_assert(kythira::network_server_with_timeout_now<cpp_httplib_server<default_http_types>>);
static_assert(!kythira::network_client_with_log_fetch<cpp_httplib_client<default_http_types>>,
              "HTTP does not implement log fetch; see "
              ".kiro/specs/http-coap-pre-vote-timeout-now/design.md §6.");
```

The types bundle named here is whichever one the header already instantiates
in its tests; the task picks it. The negative assertions are the point: they
make the next missing extension a build failure, not an `unsupported` skip.

## 3. Rolling upgrades: the not-implemented outcome

### 3.1 The problem

`start_pre_vote()` counts a peer only when it returns a granted response.
An error is a refusal. That was harmless on `tcp_rpc`, `tls_tcp_rpc` and the
simulator, where clusters are built from one binary. HTTP and CoAP are the
transports `cmd/multi_raft_node` and the CoAP deployments run, so they will be
upgraded node by node.

Take three nodes with one upgraded. The upgraded node's pre-vote gets two
errors and never escalates, so it can never become leader. That is survivable
while the two older nodes can still elect each other. With five nodes, two
upgraded, and two of the three older nodes down, only an upgraded node with
an up-to-date log may be able to win, and it never tries. The cluster has a
quorum of live nodes and no leader. A feature meant to make elections less
disruptive would instead stop them.

### 3.2 The rule

A peer that answers "not implemented" is treated as having granted the
pre-vote.

This is safe. A pre-vote grant changes nothing on either node: no term, no
`voted_for`, no persistence. Its only effect is to let the candidate go on to
a real election, which still needs a majority of real RequestVote grants, and
those older peers answer RequestVote under the same rules as before. So for
that one peer the outcome is exactly what it was before PreVote existed. The
guarantee PreVote adds, that an isolated node cannot depose a healthy leader,
holds among the upgraded peers and is absent for the older ones, which is the
best a partial rollout can offer.

It is also the only option that needs no configuration. The alternatives were
worse:

- **A per-node "PreVote enabled" flag** that operators flip after the upgrade
  finishes. It adds a second rollout step and a way to forget it, and the
  requirements rule it out.
- **Remembering per peer that it lacks PreVote and sending RequestVote
  instead.** Same safety, but it is state that must expire when the peer
  upgrades, and it changes `start_election()`'s inputs. The grant rule needs
  no state because each round asks again.

### 3.3 Mechanics

```cpp
// include/raft/exceptions.hpp
class rpc_not_implemented_exception : public network_exception {
public:
    rpc_not_implemented_exception(std::string rpc, std::uint64_t target);
    auto rpc() const -> const std::string&;
    auto target() const -> std::uint64_t;
};
```

- **Base class.** `network_exception`, the core's transport-failure type.
  The HTTP and CoAP transports' own error types (`http_transport_error`,
  `coap_transport_error`) derive from `std::runtime_error` directly, so no
  existing transport `catch` changes meaning.
- **Retry classification.** `error_handler::classify_error()` matches on
  message text. The exception's `what()` contains "not implemented", and a new
  branch above the default classifies that as `permanent_failure`,
  `should_retry = false`. A `dynamic_cast` check ahead of the text matching
  would be sturdier and is preferred if `classify_error` can take it without
  restructuring; the task decides.
- **The pre-vote counter.** In `start_pre_vote()`'s `thenTry`, an exception
  of this type becomes a synthesised `{peer_id, request_pre_vote_response{
  ._term = prospective_term, ._vote_granted = true}}` and is reported to a
  per-node `std::unordered_set<node_id_type>` of peers already warned about,
  under `_mutex`. The quorum lambdas do not change. The set is never cleared:
  a warning per peer per process lifetime is enough, and the counter carries
  the rate.
- **TimeoutNow.** Where `transfer_leadership()`'s path sends TimeoutNow (`raft.hpp:2489`), the
  exception becomes `leader_transfer_unsupported_exception`, the same type
  the `if constexpr` branch throws today. The multi-Raft host's existing
  handling then records `unsupported` with no new code.

### 3.4 Other transports

Requirement 4.7 asks only that this be recorded.

| Transport | What an older peer does with an unknown extension RPC | Reports not-implemented? |
|---|---|---|
| HTTP (all three) | 404 (no route) | yes, after this spec |
| CoAP (all three) | 4.04 (no resource) | yes, after this spec |
| gRPC | `UNIMPLEMENTED`, mapped to the caller-fault exception in `grpc_exceptions.hpp` | no; a follow-up can map `UNIMPLEMENTED` on an extension RPC |
| `tcp_rpc`, `tls_tcp_rpc` | closes the connection without replying (`tcp_rpc.hpp:467`) | no; indistinguishable from a crash |

gRPC and TCP already had PreVote before any deployment mixed versions, so
neither needs the fallback today.

## 4. CoAP PreVote

A transcription of #385's TimeoutNow, with one difference.

- **Client**: `send_request_pre_vote` calls `send_rpc<…>(target,
  "/raft/request_pre_vote", request, timeout)` with the *default*
  reliability, not `force_confirmable`. #385 forces CON for TimeoutNow because
  it is one rare message whose loss costs a whole election timeout with
  nothing retrying it. A pre-vote is neither: `start_pre_vote()` retries it,
  and it is sent at the same rate as RequestVote, so it takes RequestVote's
  reliability.
- **Server**: a `raft/request_pre_vote` resource registered exactly like
  `raft/request_vote`, including the OSCORE-only flag and the shared exchange
  table for duplicate detection. With no handler registered it answers 5.01,
  as #385's TimeoutNow resource does.
- **libnyoci and cantcoap**: the same two additions in their own dispatch.
- **Status mapping**: 4.04 and 5.01 on an extension RPC map to
  `rpc_not_implemented_exception`. Today these surface as
  `coap_client_error` / `coap_server_error`; the mapping goes in the
  extension wrappers, as on HTTP.
- **Assertions and table**: each backend's `!network_client_with_pre_vote`
  assertion becomes positive, and `tests/coap_capability_table.hpp` changes
  with it, so the build proves the table.

## 5. Testing strategy

1. **Equivalence** (`three_way_http_transport_equivalence_test.cpp`): one case
   per extension RPC, round-tripped through each HTTP transport with a
   non-zero `group_id`, responses compared.
2. **Unregistered handler**: each HTTP transport and libcoap, server started
   with no extension handlers; the client gets `rpc_not_implemented_exception`
   and exactly one attempt reaches the server (the non-retry rule).
3. **Older peer**: a cpp-httplib server whose route table has only the three
   mandatory endpoints (built by registering a raw `httplib::Server` on the
   peer's port, so no transport code is forked). A three-node cluster with two
   such peers elects the upgraded node, which logs one warning per peer over
   several rounds. The CoAP equivalent uses a libcoap context without the
   pre-vote resource.
4. **Disruptive server**: three nodes over cpp-httplib. Isolate a follower for
   several election timeouts by stopping its server and client, reconnect it,
   and check the leader's term did not change. Repeat with the pre-vote
   handler unregistered on that follower's peers to show the test can fail.
5. **Leadership transfer**: `transfer_leadership()` over each HTTP transport
   moves leadership to the named node within the transfer timeout.
6. **Multi-Raft**: a `multi_group_network_server` over cpp-httplib registers
   five handlers per group, and a host applies `transfer_leader` and `scatter`
   to completion.
7. **CoAP**: a pre-vote case in each backend's integration test next to
   #385's TimeoutNow case. cantcoap and libnyoci are not built in CI (parity
   audit C1), so those cases are run locally and the tasks say so.

## 6. Capability table after this spec

| Transport | PreVote | TimeoutNow | Log fetch | Cluster join/leave |
|---|---|---|---|---|
| gRPC | yes | yes | yes | yes |
| `tcp_rpc` | yes | yes | no | yes (#389) |
| `tls_tcp_rpc` | yes | yes | no | no |
| cpp-httplib, Beast, Proxygen | **yes** | **yes** | no | no |
| CoAP (all three) | **yes** | yes (#385) | no | no |
| simulator | yes | yes | yes | yes |

Log fetch and cluster join/leave on HTTP and CoAP remain open (parity audit
T6); they are address-keyed rather than node-keyed and need their own design.

## 7. Risks

- **Election timing changes on every HTTP and CoAP cluster.** A node whose
  timer fires now spends one round trip on a pre-vote before campaigning, and
  a rejoining node no longer forces an election. Both are PreVote's intended
  effect, but they move the numbers the Raft timing sweep (#390) and the
  multi-Raft benchmark rows measure, so those are re-run once this lands.
- **Leader stickiness and long election timeouts.** `handle_request_pre_vote()`
  refuses a pre-vote from a node while it has heard from a leader within its
  own election timeout. The simulator rollout found a test whose "dormant"
  nodes had a ten-minute timeout and could never grant
  (`doc/TODO.md`, the `make_dormant_config()` note). Any HTTP or CoAP test
  using the same trick will fail the same way once those transports gain
  PreVote; the task that adds PreVote sweeps for it.
