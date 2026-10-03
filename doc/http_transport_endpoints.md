# HTTP Transport Endpoints

The three HTTP transports (cpp-httplib, Boost.Beast and Proxygen) serve the
same six Raft endpoints. Every request is a `POST` whose body is encoded with
the configured serializer; content negotiation (415 / 406), TLS, body-size
limits and per-call timeouts work the same way on all six.

| Path | Request | Response | Kind |
|---|---|---|---|
| `/v1/raft/request_vote` | `request_vote_request<>` | `request_vote_response<>` | mandatory |
| `/v1/raft/append_entries` | `append_entries_request<>` | `append_entries_response<>` | mandatory |
| `/v1/raft/install_snapshot` | `install_snapshot_request<>` | `install_snapshot_response<>` | mandatory |
| `/v1/raft/request_pre_vote` | `request_pre_vote_request<>` | `request_pre_vote_response<>` | extension |
| `/v1/raft/timeout_now` | `timeout_now_request<>` | `timeout_now_response<>` | extension |
| `/v1/raft/fetch_log_entries` | `fetch_log_entries_request<>` | `fetch_log_entries_response<>` | peer-to-peer catch-up |

The names match the gRPC methods and the `tcp_rpc` type tags, so a capture or
a metric reads the same way on every transport.

## Extensions per transport

| Transport | PreVote | TimeoutNow | Log fetch | Cluster join/leave |
|---|---|---|---|---|
| gRPC | yes | yes | yes | yes |
| `tcp_rpc` | yes | yes | yes | yes |
| `tls_tcp_rpc` | yes | yes | yes | no |
| cpp-httplib, Beast, Proxygen | yes | yes | yes | no |
| CoAP (libcoap, cantcoap, libnyoci) | yes | yes | yes | no |
| simulator | yes | yes | yes | yes |

Each HTTP and CoAP implementation header ends with `static_assert`s that prove
this table: positive for the three extensions it carries, negative for cluster
join/leave. Adding those later turns the negative assertion into a build
failure that points at the table. Log fetch (`.kiro/specs/peer2peer-log-replication/`)
arrived on HTTP and CoAP after PreVote and TimeoutNow; the rest of this page
is about the latter two.

## Status codes on the extension endpoints

A server that has the route but no registered handler answers
**501 Not Implemented** for PreVote and TimeoutNow. The three mandatory
endpoints and `fetch_log_entries` keep answering 500 in that case.

On the client side, PreVote and TimeoutNow map two statuses to
`kythira::rpc_not_implemented_exception`:

| Status on an extension RPC | Client sees |
|---|---|
| 404 Not Found (an older peer with no route) | `rpc_not_implemented_exception` |
| 501 Not Implemented (no handler registered) | `rpc_not_implemented_exception` |
| anything else | unchanged (`http_client_error` / `http_server_error`) |

A 404 on a mandatory endpoint still raises `http_client_error`, because it
means a misconfigured peer, not an older one.

## Rolling upgrades

`rpc_not_implemented_exception` is never retried. The Raft core treats it as
follows:

- **PreVote**: a peer that answers "not implemented" counts as having granted
  the pre-vote. A pre-vote grant changes no state on either node; the
  candidate still needs a majority of real RequestVote grants. So a cluster
  part-way through an upgrade keeps electing leaders, and the disruptive-server
  protection PreVote adds holds among the upgraded peers. Each node logs one
  warning per such peer and counts every occurrence in the
  `raft.pre_vote.peer_not_implemented` metric (dimensions `node_id`,
  `peer_id`).
- **TimeoutNow**: `transfer_leadership()` to a peer that does not implement it
  ends with `leader_transfer_unsupported_exception`, which the multi-Raft host
  reports as `unsupported`.

The full design is in `.kiro/specs/http-coap-pre-vote-timeout-now/`.
