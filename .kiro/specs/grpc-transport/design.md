# Design Document

## Overview

The gRPC transport implements the `network_client`/`network_server` concept family
(`include/raft/network.hpp`) — the base RequestVote/AppendEntries/InstallSnapshot RPCs
plus the optional ClusterJoin/ClusterLeave bootstrap extension, RequestPreVote election
extension, and peer-to-peer `fetch_log_entries` extension — on top of gRPC and Protocol
Buffers. It sits alongside the existing HTTP (`cpp_httplib_client`/`cpp_httplib_server`)
and CoAP (`coap_client`/`coap_server`) transports as a third concrete transport, and
alongside the extension-implementing `tcp_rpc`/`tls_tcp_rpc` transport as a second
transport that implements the full concept family rather than only the three core RPCs.

The implementation consists of two main components:

- **grpc_client**: Implements `network_client` and, where the corresponding extension
  concept requires it, `network_client_with_cluster_join`, `network_client_with_cluster_leave`,
  `network_client_with_pre_vote`, and `network_client_with_log_fetch`.
- **grpc_server**: Implements `network_server` and the mirror-image server-side extension
  concepts.

Both are parameterized by a single `grpc_transport_types` template argument (mirroring
`transport_types`), and both are generated-code consumers of a single `raft.proto` file
that is the canonical schema for every Raft RPC message this project defines.

### Key design decision: Protobuf replaces `rpc_serializer`

The HTTP and CoAP transports are parameterized by `Types::serializer_type`, a pluggable
component satisfying `rpc_serializer<S, Data>` that converts between `kythira` request/
response structs and an opaque `std::vector<std::byte>` wire payload (today, always
`json_rpc_serializer`). This indirection exists because HTTP and CoAP are themselves
serialization-agnostic — the transport only moves bytes.

gRPC is different: Protocol Buffers *is* both the IDL and the wire format, and the
generated stub/service code is typed in terms of generated message classes, not raw
bytes. Introducing a second layer of `rpc_serializer` underneath gRPC would mean
serializing a `kythira` struct to (say) JSON bytes, then stuffing those bytes into a
protobuf `bytes` field — discarding gRPC's schema, cross-language tooling, and wire
efficiency for no benefit. The gRPC transport therefore does **not** use
`Types::serializer_type` at all; `grpc_transport_types` omits `serializer_type` entirely
(Requirement 14.2). Conversion instead happens via a fixed set of free functions
(`to_proto`/`from_proto` overloads, one pair per message type) that convert directly
between `kythira` structs and the generated protobuf message classes. This mirrors how
CoAP's transport-specific block-wise/DTLS concerns are handled outside the generic
serializer layer, and keeps `raft.proto` — not a `serializer_type` template parameter —
as the single source of truth for the gRPC wire schema.

### Key design decision: full concept-family coverage, not base-only

`cpp_httplib_client`/`cpp_httplib_server` and the CoAP transport implement only the base
`network_client`/`network_server` concepts; the optional bootstrap/pre-vote/log-fetch
extensions today are implemented only by `tcp_rpc`/`tls_tcp_rpc`. Because gRPC services
compose cheaply (a `grpc::Server` can host any number of independent `Service`
implementations on one listener, and a client can hold any number of stubs over one
`Channel`), there is little marginal cost to `grpc_client`/`grpc_server` implementing
every concept in `network.hpp`. This design implements all of them (Requirements 15–17),
positioning gRPC transport as the second full-coverage transport alongside TCP, and the
first full-coverage transport with production-grade TLS, connection pooling, and
observability built in from day one (`tls_tcp_rpc` has mTLS but not the HTTP/CoAP
transports' metrics/config richness).

Each optional extension lives in its own gRPC service (`RaftBootstrapService`,
`RaftElectionExtensionService`, `RaftPeerReplicationService`) rather than additional RPCs
bolted onto `RaftService`, so that:
- the base `RaftService` stays a stable, minimal surface matching every other transport's
  core scope,
- a future non-C++ client that only wants to observe RequestVote/AppendEntries/
  InstallSnapshot never needs to know the extension messages exist, and
- `grpc_server::start()` can register only the services corresponding to handlers that
  were actually provided, without changing `RaftService`'s shape.

An unregistered optional service still needs to not crash a peer that (incorrectly)
calls it; see Requirement 6.3 — gRPC returns `UNIMPLEMENTED` automatically for a method
on a service that was never registered with the `grpc::ServerBuilder`, which is the
desired behavior with no extra code.

## Architecture

### Component Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                        Raft Node                             │
│  (Template parameters: NetworkClient, NetworkServer, ...)    │
└────────────┬──────────────────────────────┬─────────────────┘
             │                              │
             │ uses                         │ uses
             ▼                              ▼
┌────────────────────────┐      ┌────────────────────────────┐
│  grpc_client<Types>    │      │   grpc_server<Types>       │
│                        │      │                            │
│  Implements:           │      │   Implements:              │
│  - network_client      │      │   - network_server         │
│  - network_client_with_│      │   - network_server_with_   │
│    cluster_join/leave  │      │     cluster_join/leave     │
│  - network_client_with_│      │   - network_server_with_   │
│    pre_vote            │      │     pre_vote               │
│  - network_client_with_│      │   - network_server_with_   │
│    log_fetch           │      │     log_fetch              │
│                        │      │                            │
│  Uses Types::          │      │   Uses Types::             │
│  - future_template     │      │   - future_template        │
│  - executor_type       │      │   - executor_type          │
│  - metrics_type        │      │   - metrics_type           │
└────────┬───────────────┘      └────────┬───────────────────┘
         │                               │
         │ to_proto/from_proto           │ to_proto/from_proto
         ▼                               ▼
┌────────────────────────────────────────────────────────────┐
│         raft.pb.h / raft.grpc.pb.h  (generated)             │
│  RaftService, RaftBootstrapService,                          │
│  RaftElectionExtensionService, RaftPeerReplicationService     │
└────────┬───────────────────────────────┬─────────────────────┘
         │                               │
         ▼                               ▼
┌────────────────────────┐      ┌────────────────────────────┐
│   gRPC C++ Channel/Stub │      │   gRPC C++ Server           │
│   (Callback API)        │      │   (CallbackService API)     │
└────────┬───────────────┘      └────────┬───────────────────┘
         │                               │
         ▼                               ▼
┌────────────────────────────────────────────────────────────┐
│                    HTTP/2 (gRPC C++ core)                    │
└────────────────────────────────────────────────────────────┘
```

### Request Flow (Client Side, RequestVote)

1. Raft node calls `send_request_vote(target, request, timeout)` on `grpc_client`.
2. `grpc_client` looks up (or lazily creates and caches) the `Channel` for `target` from
   its node-ID-to-address book, and obtains/reuses a `RaftService::Stub`.
3. `to_proto(request)` converts `request_vote_request<>` to `RequestVoteRequest`.
4. A `kythira::Promise<request_vote_response<>>` is created; its future is returned
   immediately.
5. A `grpc::CallbackClientContext` is configured with `deadline = now() + timeout` and,
   if TLS is enabled, per-call auth metadata as needed.
6. `stub->async()->RequestVote(&context, &proto_request, &proto_response, reactor)` is
   invoked with a `grpc::ClientUnaryReactor` whose `OnDone(grpc::Status)` callback:
   - on `status.ok()`: converts `proto_response` via `from_proto` and posts
     `promise.setValue(...)` onto `Types::executor_type`;
   - on failure: maps `status.error_code()` to the appropriate exception type
     (Requirement 11) and posts `promise.setException(...)` onto `Types::executor_type`.
7. Metrics for request count, latency, and size are recorded around step 6.

### Request Flow (Server Side, RequestVote)

1. gRPC delivers an incoming `RequestVote` call to `grpc_server`'s
   `RaftService::CallbackService` override, which returns a
   `grpc::ServerUnaryReactor*` immediately (never blocking the delivering thread).
2. `from_proto(*proto_request)` converts to `request_vote_request<>`.
3. The registered handler `std::function` is invoked on `Types::executor_type`.
4. When the handler's result is ready, `to_proto(response)` converts it to
   `RequestVoteResponse`, the reactor's response message is populated, and
   `reactor->Finish(grpc::Status::OK)` is called.
5. If the handler throws, `reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL,
   exception.what()))` is called instead.
6. Metrics for request count, latency, and size are recorded around steps 3–5.

InstallSnapshot follows the same flow with the existing offset/data/done chunking
contract unchanged — the leader calls `send_install_snapshot` once per chunk, exactly as
it already does over HTTP/CoAP/TCP (Requirement 5.3).

## Components and Interfaces

### `raft.proto`

```protobuf
syntax = "proto3";

package kythira.raft.v1;

enum EntryType {
  ENTRY_TYPE_NORMAL = 0;
  ENTRY_TYPE_CONFIGURATION = 1;
  ENTRY_TYPE_NO_OP = 2;
}

message LogEntry {
  uint64 term = 1;
  uint64 index = 2;
  bytes command = 3;
  EntryType type = 4;
}

message RequestVoteRequest {
  uint64 term = 1;
  uint64 candidate_id = 2;
  uint64 last_log_index = 3;
  uint64 last_log_term = 4;
}

message RequestVoteResponse {
  uint64 term = 1;
  bool vote_granted = 2;
}

message AppendEntriesRequest {
  uint64 term = 1;
  uint64 leader_id = 2;
  uint64 prev_log_index = 3;
  uint64 prev_log_term = 4;
  repeated LogEntry entries = 5;
  uint64 leader_commit = 6;
}

message AppendEntriesResponse {
  uint64 term = 1;
  bool success = 2;
  optional uint64 conflict_index = 3;
  optional uint64 conflict_term = 4;
}

message InstallSnapshotRequest {
  uint64 term = 1;
  uint64 leader_id = 2;
  uint64 last_included_index = 3;
  uint64 last_included_term = 4;
  uint64 offset = 5;
  bytes data = 6;
  bool done = 7;
}

message InstallSnapshotResponse {
  uint64 term = 1;
}

service RaftService {
  rpc RequestVote(RequestVoteRequest) returns (RequestVoteResponse);
  rpc AppendEntries(AppendEntriesRequest) returns (AppendEntriesResponse);
  rpc InstallSnapshot(InstallSnapshotRequest) returns (InstallSnapshotResponse);
}

// ── Optional extensions (Requirements 15-17) ────────────────────────────────

message RequestPreVoteRequest {
  uint64 term = 1;
  uint64 candidate_id = 2;
  uint64 last_log_index = 3;
  uint64 last_log_term = 4;
}

message RequestPreVoteResponse {
  uint64 term = 1;
  bool vote_granted = 2;
}

service RaftElectionExtensionService {
  rpc RequestPreVote(RequestPreVoteRequest) returns (RequestPreVoteResponse);
}

message PeerInfo {
  uint64 node_id = 1;
  string address = 2;
}

message ClusterJoinRequest {
  uint64 node_id = 1;
  string contact_address = 2;
}

message ClusterJoinResponse {
  bool accepted = 1;
  optional PeerInfo redirect = 2;
}

message ClusterLeaveRequest {
  uint64 node_id = 1;
}

message ClusterLeaveResponse {
  bool accepted = 1;
  optional PeerInfo redirect = 2;
}

service RaftBootstrapService {
  rpc ClusterJoin(ClusterJoinRequest) returns (ClusterJoinResponse);
  rpc ClusterLeave(ClusterLeaveRequest) returns (ClusterLeaveResponse);
}

message FetchLogEntriesRequest {
  uint64 requester_id = 1;
  uint64 from_index = 2;
  uint64 to_index = 3;
}

message FetchLogEntriesResponse {
  uint64 responder_id = 1;
  bool available = 2;
  uint64 prev_log_term = 3;
  repeated LogEntry entries = 4;
}

service RaftPeerReplicationService {
  rpc FetchLogEntries(FetchLogEntriesRequest) returns (FetchLogEntriesResponse);
}
```

`ClusterJoinRequest`/`ClusterLeaveRequest` are addressed by contact address, not node ID
(mirroring `network_client_with_cluster_join`'s `const std::string& addr` parameter); the
`grpc_client`'s bootstrap stub is therefore constructed against an ad hoc `Channel` for
the given address rather than looked up in the node-ID address book (Requirement 15.3).

### `grpc_transport_types` Concept

```cpp
template<typename T>
concept grpc_transport_types =
    requires {
        typename T::metrics_type;
        typename T::executor_type;
    } && kythira::metrics<typename T::metrics_type> &&
    requires {
        typename T::template future_template<kythira::request_vote_response<>>;
        typename T::template future_template<kythira::append_entries_response<>>;
        typename T::template future_template<kythira::install_snapshot_response<>>;
    } &&
    future<typename T::template future_template<kythira::request_vote_response<>>,
           kythira::request_vote_response<>> &&
    future<typename T::template future_template<kythira::append_entries_response<>>,
           kythira::append_entries_response<>> &&
    future<typename T::template future_template<kythira::install_snapshot_response<>>,
           kythira::install_snapshot_response<>>;
```

**Example Implementation:**

```cpp
struct grpc_kythira_transport_types {
    using metrics_type = kythira::noop_metrics;
    using executor_type = folly::CPUThreadPoolExecutor;
    template<typename T> using future_template = kythira::Future<T>;
};
```

### `grpc_client<Types>`

**Template Parameters:**
- `Types`: A single template parameter conforming to `grpc_transport_types`.

**Constructor Parameters:**
- `node_id_to_target_map`: `std::unordered_map<std::uint64_t, std::string>` mapping node
  IDs to gRPC target strings (e.g. `"10.0.0.2:9443"`).
- `config`: `grpc_client_config` (below).
- `metrics`: `typename Types::metrics_type` instance.

**Public Methods:**

```cpp
// network_client
auto send_request_vote(std::uint64_t target, const request_vote_request<>& req,
                        std::chrono::milliseconds timeout)
    -> typename Types::template future_template<request_vote_response<>>;
auto send_append_entries(std::uint64_t target, const append_entries_request<>& req,
                          std::chrono::milliseconds timeout)
    -> typename Types::template future_template<append_entries_response<>>;
auto send_install_snapshot(std::uint64_t target, const install_snapshot_request<>& req,
                            std::chrono::milliseconds timeout)
    -> typename Types::template future_template<install_snapshot_response<>>;

// network_client_with_pre_vote
auto send_request_pre_vote(std::uint64_t target, const request_pre_vote_request<>& req,
                            std::chrono::milliseconds timeout)
    -> typename Types::template future_template<request_pre_vote_response<>>;

// network_client_with_cluster_join / _with_cluster_leave
auto send_cluster_join_request(const std::string& addr, const cluster_join_request<>& req,
                                std::chrono::milliseconds timeout)
    -> typename Types::template future_template<cluster_join_response<>>;
auto send_cluster_leave_request(const std::string& addr, const cluster_leave_request<>& req,
                                 std::chrono::milliseconds timeout)
    -> typename Types::template future_template<cluster_leave_response<>>;

// network_client_with_log_fetch
auto send_fetch_log_entries(std::uint64_t target, const fetch_log_entries_request<>& req,
                             std::chrono::milliseconds timeout)
    -> typename Types::template future_template<fetch_log_entries_response<>>;
```

**Private Members:**
- `_node_id_to_target`: Address book.
- `_channels`: `std::unordered_map<std::uint64_t, std::shared_ptr<grpc::Channel>>` — one
  reused `Channel` per target node.
- `_stubs`: Per-channel cached `RaftService::Stub`/`RaftElectionExtensionService::Stub`/
  `RaftPeerReplicationService::Stub` instances.
- `_channel_credentials`: `std::shared_ptr<grpc::ChannelCredentials>` derived once from
  `config` at construction (`grpc::SslCredentials(...)` or
  `grpc::InsecureChannelCredentials()`).
- `_config`, `_metrics`, `_executor` (`typename Types::executor_type`), `_mutex`
  (protects `_channels`/`_stubs` lazy-creation).

**Private Methods:**

```cpp
auto get_or_create_channel(std::uint64_t target) -> std::shared_ptr<grpc::Channel>;
auto get_or_create_channel(const std::string& target_address)
    -> std::shared_ptr<grpc::Channel>;  // bootstrap path, keyed by address not node id

template<typename ProtoRequest, typename ProtoResponse, typename Response, typename Stub,
         typename RpcMethod>
auto call_unary(std::shared_ptr<grpc::Channel> channel, RpcMethod method,
                 const ProtoRequest& proto_req, std::chrono::milliseconds timeout)
    -> typename Types::template future_template<Response>;

auto status_to_exception(const grpc::Status& status) -> std::exception_ptr;
```

`call_unary` is the single generic implementation every `send_*` method funnels through,
analogous to `cpp_httplib_client::send_rpc` — it owns Promise creation, deadline setting,
the callback-API invocation, status-to-exception mapping, and metrics emission, so each
public `send_*` method is a thin `to_proto`/stub-selection wrapper around it.

### `grpc_server<Types>`

**Template Parameters:**
- `Types`: A single template parameter conforming to `grpc_transport_types`.

**Constructor Parameters:**
- `bind_address`, `bind_port`.
- `config`: `grpc_server_config` (below).
- `metrics`: `typename Types::metrics_type` instance.

**Public Methods:**

```cpp
// network_server
auto register_request_vote_handler(
    std::function<request_vote_response<>(const request_vote_request<>&)> handler) -> void;
auto register_append_entries_handler(
    std::function<append_entries_response<>(const append_entries_request<>&)> handler) -> void;
auto register_install_snapshot_handler(
    std::function<install_snapshot_response<>(const install_snapshot_request<>&)> handler) -> void;

// network_server_with_pre_vote
auto register_request_pre_vote_handler(
    std::function<request_pre_vote_response<>(const request_pre_vote_request<>&)> handler) -> void;

// network_server_with_cluster_join / _with_cluster_leave
auto register_cluster_join_handler(
    std::function<cluster_join_response<>(const cluster_join_request<>&)> handler) -> void;
auto register_cluster_leave_handler(
    std::function<cluster_leave_response<>(const cluster_leave_request<>&)> handler) -> void;

// network_server_with_log_fetch
auto register_fetch_log_entries_handler(
    std::function<fetch_log_entries_response<>(const fetch_log_entries_request<>&)> handler) -> void;

auto start() -> void;
auto stop() -> void;
auto is_running() const -> bool;
```

**Private Members:**
- One `std::optional<...>` `CallbackService` implementation per gRPC service
  (`_raft_service`, `_bootstrap_service`, `_election_extension_service`,
  `_peer_replication_service`) — only the services whose handlers were registered before
  `start()` are added to the `grpc::ServerBuilder` (Requirement 6.3's `UNIMPLEMENTED`
  behavior falls out of simply never registering a service nobody configured).
- `_handlers`: one `std::function` member per RPC, guarded by `_mutex`.
- `_server`: `std::unique_ptr<grpc::Server>`.
- `_bind_address`, `_bind_port`, `_config`, `_metrics`, `_executor`, `_running`
  (`std::atomic<bool>`), `_mutex`.

**Private Methods:**

```cpp
auto build_server_credentials() const -> std::shared_ptr<grpc::ServerCredentials>;

template<typename ProtoRequest, typename Request, typename Response, typename ProtoResponse>
auto handle_unary(std::string_view rpc_type, const ProtoRequest* proto_req,
                   ProtoResponse* proto_resp,
                   const std::function<Response(const Request&)>& handler)
    -> grpc::ServerUnaryReactor*;
```

`handle_unary` is `grpc_server`'s equivalent of `cpp_httplib_server::handle_rpc_endpoint`
— the one generic implementation every `CallbackService` override funnels through.

## Data Models

### `grpc_client_config`

```cpp
struct grpc_client_config {
    std::size_t max_send_message_size{16 * 1024 * 1024};     // 16 MB
    std::size_t max_receive_message_size{16 * 1024 * 1024};  // 16 MB
    std::chrono::seconds keepalive_time{30};
    std::chrono::seconds keepalive_timeout{10};
    bool keepalive_permit_without_calls{true};

    bool enable_tls{false};
    bool enable_ssl_verification{true};
    std::string ca_cert_pem{};       // trusted root, PEM
    std::string client_cert_pem{};   // mutual TLS
    std::string client_key_pem{};    // mutual TLS
    std::string target_name_override{};  // testing: bypass SAN check

    std::string user_agent{"kythira-grpc-transport/1.0"};
};
```

### `grpc_server_config`

```cpp
struct grpc_server_config {
    std::size_t max_send_message_size{16 * 1024 * 1024};
    std::size_t max_receive_message_size{16 * 1024 * 1024};
    std::size_t max_concurrent_rpcs{200};
    std::chrono::seconds keepalive_time{30};
    std::chrono::seconds keepalive_timeout{10};

    bool enable_tls{false};
    std::string server_cert_pem{};
    std::string server_key_pem{};
    std::string ca_cert_pem{};          // for require_client_cert
    bool require_client_cert{false};

    bool enable_health_check_service{true};
    bool enable_reflection{false};  // opt-in: exposes raft.proto shape to grpcurl etc.
};
```

Certificate/key material is accepted as in-memory PEM strings first (matching
`certificate_authority::issue()`'s in-memory output and `ca_bootstrap_client`'s
fingerprint-pinned bootstrap flow) with file-path convenience constructors layered on
top, rather than requiring every caller to round-trip through the filesystem the way
`cpp_httplib_client_config`/`cpp_httplib_server_config` do.

### Address Book

```cpp
std::unordered_map<std::uint64_t, std::string> node_id_to_target;
// Example: {1: "10.0.0.10:9443", 2: "10.0.0.11:9443"}
```

## Error Handling

### Exception Types

```cpp
class grpc_transport_error : public std::runtime_error {
public:
    grpc_transport_error(grpc::StatusCode code, const std::string& message);
    [[nodiscard]] auto status_code() const -> grpc::StatusCode;
private:
    grpc::StatusCode _status_code;
};

class grpc_connection_error : public grpc_transport_error {  // UNAVAILABLE
public:
    explicit grpc_connection_error(const std::string& message);
};

class grpc_timeout_error : public grpc_transport_error {  // DEADLINE_EXCEEDED
public:
    grpc_timeout_error(const std::string& message, std::chrono::milliseconds configured_timeout);
    [[nodiscard]] auto configured_timeout() const -> std::chrono::milliseconds;
private:
    std::chrono::milliseconds _configured_timeout;
};

class grpc_client_error : public grpc_transport_error {  // INVALID_ARGUMENT, UNIMPLEMENTED, ...
public:
    grpc_client_error(grpc::StatusCode code, const std::string& message);
};

class grpc_server_error : public grpc_transport_error {  // INTERNAL, UNKNOWN
public:
    grpc_server_error(grpc::StatusCode code, const std::string& message);
};

class grpc_tls_configuration_error : public grpc_transport_error {
public:
    explicit grpc_tls_configuration_error(const std::string& message);
};
```

### Status Code → Exception Mapping (client side)

| `grpc::StatusCode`                          | Exception                |
|----------------------------------------------|---------------------------|
| `OK`                                          | *(future resolved normally)* |
| `DEADLINE_EXCEEDED`                           | `grpc_timeout_error`      |
| `UNAVAILABLE`                                 | `grpc_connection_error`   |
| `INVALID_ARGUMENT`, `UNIMPLEMENTED`, `NOT_FOUND`, `FAILED_PRECONDITION` | `grpc_client_error` |
| `INTERNAL`, `UNKNOWN`, `DATA_LOSS`            | `grpc_server_error`       |
| any other non-`OK`                            | `grpc_transport_error`    |

### Server-Side Error Handling

- Handler throws → `reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()))`.
- `from_proto` conversion failure (e.g. impossible enum value) →
  `grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, ...)`.
- No handler registered for a service that also was never added to the
  `grpc::ServerBuilder` → gRPC itself returns `UNIMPLEMENTED`; no application code needed.
- Oversized incoming message → gRPC itself rejects at the transport layer per
  `max_receive_message_size`, surfaced to the caller as `RESOURCE_EXHAUSTED`.

## Metrics Collection

Mirrors the HTTP transport's metric shape (Requirement 13), with `grpc.` prefixes:

**Client**: `grpc.client.call.sent`, `grpc.client.call.latency`, `grpc.client.call.request_size`,
`grpc.client.call.response_size` (dimensions: `rpc_type`, `target_node_id`, `status`);
`grpc.client.error` (dimensions: `rpc_type`, `target_node_id`, `status_code`);
`grpc.client.channel.created`/`grpc.client.channel.reused` (dimension: `target_node_id`).

**Server**: `grpc.server.call.received`, `grpc.server.call.latency`, `grpc.server.call.request_size`,
`grpc.server.call.response_size` (dimensions: `rpc_type`, `status`);
`grpc.server.error` (dimensions: `rpc_type`, `status_code`);
`grpc.server.started`/`grpc.server.stopped`, `grpc.server.active_calls` (gauge).

## Testing Strategy

### Unit Tests

1. Concept-conformance `static_assert`s: `grpc_client<Types>` satisfies `network_client`
   and (separately) each optional extension concept it implements;
   `grpc_server<Types>` likewise for `network_server` and its extensions.
2. Construction tests: valid/invalid `grpc_client_config`/`grpc_server_config`; address
   book initialization.
3. `to_proto`/`from_proto` round-trip tests for every message type, including edge cases
   (empty `entries()`, absent `conflict_index`/`conflict_term`, absent redirect hint,
   every `EntryType` value).
4. TLS: certificate/key loading success and failure paths, mutual-TLS handshake success
   and client-certificate-rejection, using `certificate_authority` to mint short-lived
   test certificates rather than checked-in fixtures.
5. Error handling: verify each `grpc::StatusCode` row in the mapping table above produces
   the documented exception type.
6. Server lifecycle: repeated `start()`/`stop()`, `is_running()` before/after, and
   destruction while running.
7. `UNIMPLEMENTED` behavior when a client calls an extension service the server never
   registered a handler for.

### Property-Based Tests

Using the project's existing property-testing conventions (Boost.Test-based, see
`http-transport`'s Property 1–16 catalogue and
`tests/rpc_serialization_property_test.cpp`), each tagged
`**Feature: grpc-transport, Property {number}: {property_text}**` and run a minimum of
100 iterations with randomly generated inputs.

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid
executions of a system — a formal statement about what the system should do.*

### Property 1: Every call resolves within its deadline

*For any* RPC sent with timeout `t`, the returned future resolves (success or error)
within `t` plus a small, bounded scheduling overhead — it never hangs indefinitely.
**Validates: Requirements 10.1, 10.2, 19.1**

### Property 2: Protobuf round-trip preserves content

*For any* valid `kythira` request or response value for any of the RPC types in scope,
`from_proto(to_proto(value))` produces a value equal to the original across every field,
including `std::optional` fields that are absent and `entries()` that are empty.
**Validates: Requirements 2.1, 2.4, 2.5, 15.4, 17.3, 19.2**

### Property 3: Status code maps to the correct exception type

*For any* `grpc::StatusCode` a server can return, the client-side future is set to an
error whose exception type matches the mapping table in the Error Handling section.
**Validates: Requirements 11.1, 11.2, 11.3, 11.4, 11.5**

### Property 4: Handler invocation for every registered RPC

*For any* valid RPC request received by `grpc_server` for which a handler is registered,
that handler is invoked exactly once with the `from_proto`-converted request, and its
result is what gets `to_proto`-converted into the response.
**Validates: Requirements 6.2, 6.4**

### Property 5: Unregistered optional RPCs never crash the server

*For any* extension RPC (`RequestPreVote`, `ClusterJoin`, `ClusterLeave`,
`FetchLogEntries`) for which `grpc_server` was never given a handler, a client call to
that RPC completes with `grpc::StatusCode::UNIMPLEMENTED` rather than hanging, crashing
the server process, or being silently dropped.
**Validates: Requirement 6.3**

### Property 6: Channel reuse for repeated calls to the same target

*For any* sequence of calls from `grpc_client` to the same target node, at most one
`grpc::Channel` is created for that target across the sequence.
**Validates: Requirement 12.3**

### Property 7: TLS is never silently downgraded

*For any* `grpc_client_config`/`grpc_server_config` with `enable_tls = true`, either the
resulting channel/server uses `grpc::SslCredentials`/`grpc::SslServerCredentials`, or
construction fails with `grpc_tls_configuration_error` — it never silently falls back to
an insecure channel/listener.
**Validates: Requirements 9.1, 9.2, 9.6**

### Property-Based Testing Configuration

Each property-based test should:
- Run a minimum of 100 iterations with randomly generated inputs.
- Be tagged `**Feature: grpc-transport, Property {number}: {property_text}**`.
- Use the same Boost.Test-based property-testing approach as the rest of the project's
  RPC-layer tests (`tests/rpc_serialization_property_test.cpp`,
  `tests/rpc_malformed_message_property_test.cpp`).

## Implementation Notes

### Thread Safety

`grpc_client`/`grpc_server` must be thread-safe: `_channels`/`_stubs`/`_handlers` are
protected by a mutex on lazy-creation/registration paths; gRPC's own `Channel`, `Stub`,
and `Server` objects are documented thread-safe for concurrent use once constructed.

### Callback API vs. Completion Queue

The design uses gRPC C++'s callback API
(`grpc::CallbackClientContext`/`grpc::ClientUnaryReactor` on the client,
`CallbackService`/`grpc::ServerUnaryReactor` on the server) rather than manually managing
a `grpc::CompletionQueue` and polling thread. This removes an entire category of
completion-queue-lifecycle bugs and matches the callback-driven shape `kythira::Promise`
already expects. The callback API has been GA (non-`experimental`) since gRPC 1.42+;
`vcpkg.json`'s pinned `grpc` version must be new enough to provide it as a stable API.

### Executor Discipline

Every promise fulfillment (`setValue`/`setException`) happens by posting a task onto
`typename Types::executor_type`, never directly from gRPC's internal callback thread —
this matches how the HTTP transport hands off from cpp-httplib's I/O thread and keeps
whatever continuation the Raft core chains onto the returned future running under a
predictable, project-controlled executor rather than gRPC's own thread pool.

### Certificate Integration

`grpc_client_config`/`grpc_server_config` accept PEM strings directly so that material
produced by `certificate_authority::issue()`, fetched via `ca_service`'s `--serve` HTTP
API, or obtained through `ca_bootstrap_client::fetch_trusted_root()`'s fingerprint-pinned
first-contact flow can be handed to the gRPC transport without an intermediate
filesystem round-trip, consistent with how `ca_cluster_node`'s Raft-internal RPC channel
already bootstraps `tls_tcp_rpc` mTLS from the same material
(`.kiro/specs/ca-cluster-rpc-mtls/`).

### Serializer Layer Bypass

As covered in Overview, `grpc_transport_types` intentionally has no `serializer_type`
member and does not participate in `rpc_serializer`. This is a deliberate asymmetry with
`transport_types` (used by HTTP/CoAP) documented here so it is not mistaken for an
oversight during review.

### Error Recovery

Consistent with every other transport in this project, the gRPC transport implements no
automatic retry logic — Raft's own retry policies (`retry_policy_config`,
`raft_configuration::_request_vote_retry_policy` etc.) own that decision, and the
transport layer's only job is to report failure accurately and promptly via the returned
future's error state.

## Dependencies

### External Libraries

- **gRPC C++** (`grpc`): RPC framework, HTTP/2 transport, TLS integration. vcpkg package
  `grpc`; license Apache 2.0.
- **Protocol Buffers** (`protobuf`): IDL compiler and runtime, pulled in transitively by
  the `grpc` vcpkg port. License BSD-3-Clause.
- **OpenSSL**: Already a required project dependency; reused for gRPC's TLS credentials.

### Internal Dependencies

- **raft/types.hpp**: Raft RPC message types and concepts (`request_vote_request<>`
  etc.), `metrics` concept.
- **raft/network.hpp**: `network_client`/`network_server` and every optional extension
  concept this transport implements.
- **raft/certificate_authority.hpp** / **raft/ca_bootstrap_client.hpp** (test-only):
  issuing short-lived certificates for TLS unit/integration tests.

## Build Integration

### `vcpkg.json`

```jsonc
{
  "name": "grpc",
  "version>=": "1.71.0"
}
```

Added to the top-level `dependencies` array alongside `libcoap`, following the same
always-declared-but-gracefully-optional pattern `COAP_TRANSPORT` already uses — vcpkg
installs it, but `CMakeLists.txt` only requires it be found when `GRPC_TRANSPORT` is
selected (or unconditionally probed and silently skipped, mirroring `LIBCOAP_FOUND`).

### `Kconfig`

```kconfig
config GRPC_TRANSPORT
	bool "gRPC transport (grpc, Protocol Buffers)"
	help
	  find_package(gRPC CONFIG) and find_package(Protobuf CONFIG).
	  Backs GRPC_FOUND / PROTOBUF_FOUND. vcpkg package: grpc.
	  Generates raft.pb.{h,cc}/raft.grpc.pb.{h,cc} from proto/raft.proto
	  via protoc + grpc_cpp_plugin at build time.
```

### `CMakeLists.txt`

```cmake
kythira_kconfig_gate(GRPC_TRANSPORT)
if(_KYTHIRA_GATE_GRPC_TRANSPORT)
    find_package(gRPC CONFIG QUIET)
    find_package(Protobuf CONFIG QUIET)
endif()
kythira_kconfig_require(GRPC_TRANSPORT "gRPC_FOUND AND Protobuf_FOUND" "grpc")

if(gRPC_FOUND AND Protobuf_FOUND)
    set(RAFT_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/proto/raft.proto")
    set(GRPC_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/raft")
    file(MAKE_DIRECTORY "${GRPC_GENERATED_DIR}")

    add_custom_command(
        OUTPUT "${GRPC_GENERATED_DIR}/raft.pb.cc" "${GRPC_GENERATED_DIR}/raft.pb.h"
               "${GRPC_GENERATED_DIR}/raft.grpc.pb.cc" "${GRPC_GENERATED_DIR}/raft.grpc.pb.h"
        COMMAND protobuf::protoc
        ARGS --grpc_out="${GRPC_GENERATED_DIR}" --cpp_out="${GRPC_GENERATED_DIR}"
             --plugin=protoc-gen-grpc="$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
             -I "${CMAKE_CURRENT_SOURCE_DIR}/proto" "${RAFT_PROTO}"
        DEPENDS "${RAFT_PROTO}" protobuf::protoc gRPC::grpc_cpp_plugin
    )

    add_library(raft_grpc_transport
        src/grpc_transport_impl.cpp
        "${GRPC_GENERATED_DIR}/raft.pb.cc"
        "${GRPC_GENERATED_DIR}/raft.grpc.pb.cc"
    )
    target_include_directories(raft_grpc_transport PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include "${GRPC_GENERATED_DIR}"
    )
    target_link_libraries(raft_grpc_transport PUBLIC
        gRPC::grpc++ protobuf::libprotobuf OpenSSL::SSL OpenSSL::Crypto
    )
    target_compile_features(raft_grpc_transport PUBLIC cxx_std_23)
else()
    message(WARNING "gRPC/Protobuf not found. gRPC transport will not be available.")
endif()
```

### Header/Source File Structure

```
proto/
└── raft.proto                        # canonical wire schema (Requirement 2)
include/raft/
├── grpc_transport.hpp                # grpc_transport_types concept, config structs
├── grpc_transport_impl.hpp           # grpc_client<Types>, grpc_server<Types>
├── grpc_message_conversion.hpp       # to_proto/from_proto overloads
└── grpc_exceptions.hpp               # exception types
src/
└── grpc_transport_impl.cpp           # explicit template instantiations, if any
```

## Performance Considerations

- **HTTP/2 multiplexing**: a single `Channel` can carry many concurrent RPCs without the
  connection-pool sizing HTTP/1.1 needs, simplifying `grpc_client_config` relative to
  `cpp_httplib_client_config::connection_pool_size`.
- **Binary framing**: protobuf's binary wire format is typically smaller and faster to
  (de)serialize than the JSON payloads HTTP/CoAP transports send today.
- **Keepalive**: HTTP/2-level pings detect a dead peer independent of and typically
  faster than a TCP-level timeout, letting `send_*` fail over to
  `grpc_connection_error` promptly rather than waiting out the full RPC deadline.
- Expected throughput/latency characteristics should be benchmarked the same way the
  Folly-vs-stdexec future backend comparison is (`doc/future_backend_performance_comparison.md`),
  producing a `doc/grpc_transport_performance_comparison.md` companion once implemented,
  rather than being asserted here without measurement.

## Security Considerations

### TLS/mTLS

- TLS is off by default (matching every other transport's default), and must be
  explicitly enabled via `enable_tls = true`.
- When mutual TLS is enabled, both the client certificate and server certificate are
  validated against configured trusted roots — never trust-on-first-use in the base
  implementation (fingerprint-pinned bootstrap, if wanted, follows
  `ca_bootstrap_client`'s existing pattern rather than reinventing one).
- `target_name_override` exists solely for test convenience (bypassing SAN hostname
  checks against a test certificate) and must never be set outside test builds.

### Request Size Limits

`max_receive_message_size` bounds both `AppendEntries` (many log entries) and
`InstallSnapshot` (large `data` chunks); operators must size it consistently with
`raft_configuration::_max_entries_per_append`/`_snapshot_chunk_size` to avoid a
configuration that Raft itself would attempt but gRPC would reject.

### Rate Limiting

As with the HTTP transport, the gRPC transport does not implement rate limiting;
`max_concurrent_rpcs` bounds concurrency but real admission control belongs at the
infrastructure layer (load balancer, service mesh sidecar) or a future dedicated
enhancement.

## Future Enhancements

### Client-streaming InstallSnapshot

Replace the unary, offset-chunked `InstallSnapshot` RPC with a client-streaming RPC that
sends the whole snapshot as a sequence of messages over one call, removing the
per-chunk round-trip and `offset` bookkeeping. Out of scope for the base implementation
(Requirement 5.3) to keep the wire contract uniform with the other transports.

### Health Checking & Reflection

`grpc_server_config::enable_health_check_service`/`enable_reflection` are present in the
data model as configuration hooks; wiring the standard
`grpc::health::v1::HealthCheckServiceInterface` and
`grpc::reflection::v1alpha::ServerReflection` implementations in is deferred to
implementation time rather than specified in detail here, since neither changes any
`network_server` concept behavior.

### Load Balancing / Service Discovery

gRPC's built-in client-side load-balancing (`round_robin`, `pick_first`) and
name-resolver plugins are a natural fit for this project's existing peer-discovery
backends (`rfc1035_peer_discovery`, `poco_peer_discovery`, etc.) but integrating a custom
gRPC resolver is a separate, larger effort out of scope here.

### Interceptors for Tracing

gRPC's client/server interceptor API is a natural home for propagating the OTLP tracing
context this project already emits via `otlp_exporter.hpp`, but is left as a follow-on
rather than specified here, since it changes no `network_client`/`network_server`
behavior.
