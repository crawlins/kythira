# Requirements Document

## Introduction

This document specifies the requirements for implementing gRPC transport support for the
Raft consensus algorithm. The gRPC transport provides a concrete implementation of the
`network_client`/`network_server` concept family (`include/raft/network.hpp`) — the base
RequestVote/AppendEntries/InstallSnapshot RPCs as well as the optional ClusterJoin/
ClusterLeave bootstrap extension, the RequestPreVote election extension, and the
peer-to-peer `fetch_log_entries` extension — using Protocol Buffers and the gRPC C++
library. Unlike the existing HTTP and CoAP transports, which serialize the generic
`serialized_data_type` byte buffer produced by a pluggable `rpc_serializer`, the gRPC
transport uses Protocol Buffers as both the wire format and the RPC framing, generated
from a single `.proto` IDL file that is the canonical description of every Raft RPC
message. This gives Raft clusters a modern, HTTP/2-based, strongly-typed, multiplexed
transport option suitable for production data-center deployments, alongside the existing
HTTP/HTTPS, CoAP/CoAPS, and TCP transports.

## Glossary

- **grpc_transport**: The system that implements network communication for Raft RPCs
  using gRPC and Protocol Buffers.
- **grpc_client**: A concrete implementation of the `network_client` concept (and, where
  the corresponding extension concept is satisfied, `network_client_with_cluster_join`,
  `network_client_with_cluster_leave`, `network_client_with_pre_vote`, and
  `network_client_with_log_fetch`) that sends Raft RPCs over gRPC.
- **grpc_server**: A concrete implementation of the `network_server` concept (and its
  corresponding optional extension concepts) that receives Raft RPCs over gRPC.
- **gRPC**: An open-source, HTTP/2-based RPC framework using Protocol Buffers as its
  interface definition language (IDL) and default wire format.
- **Protocol_Buffers (protobuf)**: Google's language-neutral, binary serialization format
  and IDL compiler (`protoc`) used to define RPC messages and services.
- **raft.proto**: The `.proto` file defining every Raft RPC message and gRPC service used
  by the gRPC transport; the single source of truth for the wire schema.
- **RaftService**: The gRPC service (defined in `raft.proto`) exposing the core
  RequestVote, AppendEntries, and InstallSnapshot RPCs.
- **Channel**: A gRPC abstraction representing a virtual connection to an endpoint,
  potentially backed by multiple HTTP/2 connections, that stubs are created from.
- **Stub**: The generated client-side proxy object used to invoke RPCs on a `Channel`.
- **ClientContext / ServerContext**: gRPC per-call objects carrying metadata, deadlines,
  and cancellation state for a single RPC invocation.
- **Callback_API**: gRPC's callback-based async API (`grpc::CallbackClientContext`,
  `grpc::ClientUnaryReactor`, `grpc::ServerUnaryReactor`, `CallbackService`), used instead
  of manually draining a `grpc::CompletionQueue`.
- **Deadline**: The per-call absolute time after which gRPC cancels an in-flight RPC with
  `DEADLINE_EXCEEDED`; the gRPC analogue of the `timeout` parameter already used by
  `network_client` methods.
- **Status_Code**: The `grpc::StatusCode` enum returned with every completed RPC (`OK`,
  `DEADLINE_EXCEEDED`, `UNAVAILABLE`, `INTERNAL`, `INVALID_ARGUMENT`, etc.).
- **Channel_Credentials / Server_Credentials**: gRPC's client-side and server-side
  transport-security configuration objects, used here to configure TLS/mTLS
  (`grpc::SslCredentials`, `grpc::SslServerCredentials`).
- **Keepalive**: gRPC's HTTP/2-level ping mechanism for detecting dead connections
  independent of application-level heartbeats.
- **grpc_transport_types**: The gRPC-specific types template-parameter bundle analogous
  to `http_transport_types`/`coap_transport_types`, providing `future_template`,
  `executor_type`, and `metrics_type`.
- **Node_Address_Book**: The mapping from Raft node IDs to gRPC target strings (e.g.
  `"node1.example.com:9443"`) used to construct/reuse `Channel`s.
- **Health_Checking_Protocol**: The standard gRPC health-check service
  (`grpc.health.v1.Health`) optionally exposed by `grpc_server` for load balancers and
  orchestrators.
- **Server_Reflection**: The standard gRPC reflection service, optionally exposed by
  `grpc_server` to let tools like `grpcurl` introspect `RaftService` without a copy of
  `raft.proto`.

## Requirements

### Requirement 1

**User Story:** As a distributed systems developer, I want to use gRPC transport for Raft
communication, so that I can deploy Raft clusters using a modern, HTTP/2-based,
strongly-typed RPC framework with broad tooling and multi-language interoperability.

#### Acceptance Criteria

1. WHEN the gRPC client is instantiated THEN the system SHALL conform to the
   `network_client` concept.
2. WHEN the gRPC server is instantiated THEN the system SHALL conform to the
   `network_server` concept.
3. WHEN a Raft node is instantiated with gRPC transport THEN the system SHALL use the
   gRPC client and gRPC server as the `network_client_type`/`network_server_type` members
   of its `Types` bundle.
4. WHEN gRPC transport is implemented THEN the system SHALL use the gRPC C++ library and
   Protocol Buffers for RPC framing and serialization.
5. WHEN gRPC transport is used THEN the system SHALL communicate over HTTP/2.
6. WHEN the gRPC transport is built THEN the system SHALL generate C++ message and stub
   code from a single `raft.proto` file using `protoc` and `grpc_cpp_plugin`.

### Requirement 2

**User Story:** As a systems architect, I want the Raft RPC wire schema to be defined in
one canonical `.proto` file, so that the message shapes are self-documenting, versionable,
and usable from other languages without hand-written serialization code.

#### Acceptance Criteria

1. WHEN `raft.proto` is defined THEN the system SHALL declare a message for each of
   `request_vote_request`, `request_vote_response`, `append_entries_request` (including
   a nested/repeated `LogEntry` message), `append_entries_response`,
   `install_snapshot_request`, `install_snapshot_response`, `request_pre_vote_request`,
   `request_pre_vote_response`, `cluster_join_request`, `cluster_join_response`,
   `cluster_leave_request`, `cluster_leave_response`, `fetch_log_entries_request`, and
   `fetch_log_entries_response`.
2. WHEN `raft.proto` is defined THEN the system SHALL declare `RaftService` with unary
   RPCs `RequestVote`, `AppendEntries`, and `InstallSnapshot`.
3. WHEN `raft.proto` is defined THEN the system SHALL declare separate service
   definitions (or additional RPCs) for the optional ClusterJoin/ClusterLeave,
   RequestPreVote, and FetchLogEntries extensions, so that a `grpc_server` may register
   only the handlers it supports without violating the base `network_server` concept.
4. WHEN a `LogEntry` message is defined THEN the system SHALL represent `term`, `index`,
   `command` (as `bytes`), and `type` (as an enum mirroring `kythira::entry_type`).
5. WHEN a redirect hint is represented (`cluster_join_response`/`cluster_leave_response`)
   THEN the system SHALL declare an optional `PeerInfo` message mirroring
   `kythira::peer_info<NodeId, Address>`.
6. WHEN the proto package is declared THEN the system SHALL use a versioned package name
   (`kythira.raft.v1`) to allow future breaking schema changes without colliding with
   this version.
7. WHEN generated code is produced THEN the system SHALL check `raft.proto` into the
   repository and generate `raft.pb.{h,cc}`/`raft.grpc.pb.{h,cc}` at build time rather
   than committing generated code.

### Requirement 3

**User Story:** As a Raft node, I want to send RequestVote RPCs over gRPC, so that I can
participate in leader elections using gRPC transport.

#### Acceptance Criteria

1. WHEN `send_request_vote` is called on the gRPC client THEN the system SHALL return
   `typename Types::template future_template<request_vote_response<>>`.
2. WHEN sending a RequestVote RPC THEN the system SHALL invoke `RaftService::RequestVote`
   on the target's `Stub`.
3. WHEN sending a RequestVote RPC THEN the system SHALL convert `request_vote_request<>`
   to the generated `RequestVoteRequest` protobuf message before invoking the RPC.
4. WHEN the RPC completes with `grpc::StatusCode::OK` THEN the system SHALL convert the
   returned `RequestVoteResponse` protobuf message to `request_vote_response<>` and
   satisfy the future.
5. WHEN the RPC fails or exceeds its deadline THEN the system SHALL set the future to an
   error state with an exception carrying the `grpc::Status` code and message.

### Requirement 4

**User Story:** As a Raft node, I want to send AppendEntries RPCs over gRPC, so that I can
replicate log entries using gRPC transport.

#### Acceptance Criteria

1. WHEN `send_append_entries` is called on the gRPC client THEN the system SHALL return
   `typename Types::template future_template<append_entries_response<>>`.
2. WHEN sending an AppendEntries RPC THEN the system SHALL invoke
   `RaftService::AppendEntries` on the target's `Stub`.
3. WHEN sending an AppendEntries RPC THEN the system SHALL convert
   `append_entries_request<>` — including every `LogEntry` in `entries()` — to the
   generated `AppendEntriesRequest` protobuf message before invoking the RPC.
4. WHEN the RPC completes with `grpc::StatusCode::OK` THEN the system SHALL convert the
   returned `AppendEntriesResponse` protobuf message (including optional
   `conflict_index`/`conflict_term`) to `append_entries_response<>` and satisfy the
   future.
5. WHEN the RPC fails or exceeds its deadline THEN the system SHALL set the future to an
   error state with an exception carrying the `grpc::Status` code and message.

### Requirement 5

**User Story:** As a Raft node, I want to send InstallSnapshot RPCs over gRPC, so that I
can transfer snapshots to lagging followers using gRPC transport.

#### Acceptance Criteria

1. WHEN `send_install_snapshot` is called on the gRPC client THEN the system SHALL return
   `typename Types::template future_template<install_snapshot_response<>>`.
2. WHEN sending an InstallSnapshot RPC THEN the system SHALL invoke
   `RaftService::InstallSnapshot` on the target's `Stub`.
3. WHEN sending an InstallSnapshot RPC THEN the system SHALL convert
   `install_snapshot_request<>` (including `offset`, `data`, and `done`) to the generated
   `InstallSnapshotRequest` protobuf message before invoking the RPC, preserving the
   existing offset-based chunking contract defined by `install_snapshot_request_type`
   rather than introducing gRPC client-streaming in the base implementation.
4. WHEN the RPC completes with `grpc::StatusCode::OK` THEN the system SHALL convert the
   returned `InstallSnapshotResponse` protobuf message to `install_snapshot_response<>`
   and satisfy the future.
5. WHEN the RPC fails or exceeds its deadline THEN the system SHALL set the future to an
   error state with an exception carrying the `grpc::Status` code and message.
6. WHEN a snapshot chunk's serialized `InstallSnapshotRequest` would exceed the
   configured gRPC max message size THEN the system SHALL document that callers must
   choose `snapshot_chunk_size` (`raft_configuration::_snapshot_chunk_size`) small enough
   to stay under that limit, since the base implementation does not fragment a single
   chunk across multiple RPCs.

### Requirement 6

**User Story:** As a Raft server, I want to receive and handle RequestVote, AppendEntries,
and InstallSnapshot RPCs over gRPC, so that I can participate in elections, accept log
replication, and accept snapshot transfers using gRPC transport.

#### Acceptance Criteria

1. WHEN `register_request_vote_handler`, `register_append_entries_handler`, or
   `register_install_snapshot_handler` is called THEN the system SHALL store the handler
   function for the corresponding RPC.
2. WHEN a `RequestVote`, `AppendEntries`, or `InstallSnapshot` call arrives THEN the
   system SHALL convert the incoming protobuf request message to the corresponding
   `kythira` request type before invoking the registered handler.
3. WHEN no handler has been registered for an incoming RPC THEN the system SHALL fail the
   call with `grpc::StatusCode::UNIMPLEMENTED` rather than crash or hang.
4. WHEN the registered handler returns a response THEN the system SHALL convert it to the
   corresponding generated protobuf response message and finish the call with
   `grpc::StatusCode::OK`.
5. WHEN the registered handler throws an exception THEN the system SHALL finish the call
   with `grpc::StatusCode::INTERNAL` and the exception's message as the status detail,
   without crashing the server process.

### Requirement 7

**User Story:** As a system operator, I want the gRPC server to have lifecycle management,
so that I can control when it accepts connections.

#### Acceptance Criteria

1. WHEN `start` is called on the gRPC server THEN the system SHALL bind to the configured
   address and port and begin accepting HTTP/2 connections.
2. WHEN `stop` is called on the gRPC server THEN the system SHALL stop accepting new
   calls and gracefully drain in-flight RPCs (`grpc::Server::Shutdown` with a deadline)
   before returning.
3. WHEN `is_running` is called THEN the system SHALL return `true` if the server is
   accepting connections and `false` otherwise.
4. WHEN the gRPC server object is destroyed while still running THEN the system SHALL
   stop the server rather than leak the listening port or background threads.

### Requirement 8

**User Story:** As a developer, I want the gRPC transport to use gRPC's async callback API
rather than blocking calls, so that Raft's futures-based concurrency model is preserved
and a single thread is never blocked waiting on network I/O.

#### Acceptance Criteria

1. WHEN the gRPC client sends an RPC THEN the system SHALL use gRPC's callback client API
   (`grpc::CallbackClientContext` / `grpc::ClientUnaryReactor`) rather than the blocking
   stub API, so that `send_*` never blocks the calling thread.
2. WHEN an RPC's callback/reactor fires THEN the system SHALL fulfill the corresponding
   `kythira::Promise` on `typename Types::executor_type` rather than on gRPC's internal
   completion thread, so that continuations chained onto the returned future run under
   the same executor discipline as other transports.
3. WHEN the gRPC server receives a call THEN the system SHALL implement the generated
   `RaftService::CallbackService` interface, returning a `grpc::ServerUnaryReactor*` per
   RPC method rather than blocking the RPC-handling thread.
4. WHEN a registered handler performs potentially blocking work (e.g. persistence I/O)
   THEN the system SHALL invoke it on `typename Types::executor_type` rather than
   directly on the thread gRPC delivered the call on, mirroring how the HTTP/CoAP
   transports avoid blocking their I/O threads on handler execution.

### Requirement 9

**User Story:** As a security-conscious operator, I want comprehensive TLS/mTLS support
for gRPC transport, so that I can protect Raft communication from eavesdropping and
tampering and reuse this project's existing certificate-issuance stack.

#### Acceptance Criteria

1. WHEN the gRPC client is configured with TLS enabled THEN the system SHALL construct
   its `Channel` with `grpc::SslCredentials` rather than `grpc::InsecureChannelCredentials`.
2. WHEN the gRPC server is configured with TLS certificates THEN the system SHALL
   construct its listener with `grpc::SslServerCredentials`.
3. WHEN TLS is enabled on the client THEN the system SHALL validate the server's
   certificate against a configured trusted root (`ca_cert_path` or an in-memory PEM
   bundle), and reject the channel/fail in-flight calls on validation failure.
4. WHEN mutual TLS is enabled THEN the system SHALL require the client to present a
   certificate and the server to verify it against a configured CA, consistent with the
   mutual-TLS pattern already used by `tls_tcp_rpc`/`ca_cluster_node`
   (`.kiro/specs/ca-cluster-rpc-mtls/`).
5. WHEN certificate material is sourced from this project's `certificate_authority`/
   `ca_service`/`ca_bootstrap_client` THEN the system SHALL accept PEM-encoded
   certificate, key, and CA chain strings or file paths without requiring a different
   certificate format.
6. WHEN SSL/TLS configuration is invalid (missing files, unreadable key, mismatched
   cert/key pair) THEN the system SHALL fail construction with a descriptive exception
   rather than silently falling back to an insecure channel/listener.
7. WHEN TLS is not configured THEN the system SHALL default to an insecure channel/server
   (matching `cpp_httplib_client_config::enable_ssl`'s default-off behavior), so gRPC
   transport remains usable for local development and the network simulator's already
   trusted environments without requiring certificates.

### Requirement 10

**User Story:** As a distributed systems developer, I want proper deadline handling for
gRPC calls, so that the system can detect and recover from network failures without
Raft's own timeout logic having to guess at gRPC's internal state.

#### Acceptance Criteria

1. WHEN `send_request_vote`, `send_append_entries`, or `send_install_snapshot` is called
   with a `timeout` parameter THEN the system SHALL set the `ClientContext`'s deadline to
   `now() + timeout` before invoking the RPC.
2. WHEN an RPC exceeds its deadline THEN the system SHALL set the future to an error
   state with an exception whose status code is `grpc::StatusCode::DEADLINE_EXCEEDED`.
3. WHEN a deadline-exceeded error is reported THEN the system SHALL make the configured
   timeout value available on the thrown exception for diagnostics.

### Requirement 11

**User Story:** As a reliability engineer, I want proper error handling and gRPC status
code interpretation, so that the system can distinguish between different failure modes
(peer unreachable, peer overloaded, malformed request, handler failure, deadline).

#### Acceptance Criteria

1. WHEN the gRPC client receives `grpc::StatusCode::UNAVAILABLE` THEN the system SHALL
   set the future to error state with a `grpc_connection_error` exception.
2. WHEN the gRPC client receives `grpc::StatusCode::DEADLINE_EXCEEDED` THEN the system
   SHALL set the future to error state with a `grpc_timeout_error` exception.
3. WHEN the gRPC client receives `grpc::StatusCode::INVALID_ARGUMENT` or
   `grpc::StatusCode::UNIMPLEMENTED` THEN the system SHALL set the future to error state
   with a `grpc_client_error` exception carrying the status code.
4. WHEN the gRPC client receives `grpc::StatusCode::INTERNAL` or
   `grpc::StatusCode::UNKNOWN` THEN the system SHALL set the future to error state with a
   `grpc_server_error` exception carrying the status code.
5. WHEN the gRPC client receives any other non-`OK` status THEN the system SHALL set the
   future to error state with a `grpc_transport_error` exception carrying the status code
   and message.
6. WHEN the gRPC server's message conversion from protobuf to a `kythira` request type
   fails (e.g. a value out of the target integer type's range) THEN the system SHALL
   finish the call with `grpc::StatusCode::INVALID_ARGUMENT` rather than throw an
   unhandled exception on the RPC thread.

### Requirement 12

**User Story:** As a system administrator, I want configurable gRPC client and server
settings, so that I can tune the transport for my deployment environment.

#### Acceptance Criteria

1. WHEN the gRPC client is constructed THEN the system SHALL accept a mapping from Raft
   node IDs to gRPC target strings (`host:port`).
2. WHEN the gRPC client is constructed THEN the system SHALL accept configuration for
   maximum send/receive message size, keepalive ping interval/timeout, and TLS settings.
3. WHEN the gRPC client sends RPCs to the same target repeatedly THEN the system SHALL
   reuse a single `Channel` (and its underlying HTTP/2 connection(s)) rather than
   creating a new one per call.
4. WHEN the gRPC server is constructed THEN the system SHALL accept configuration for
   bind address/port, maximum message size, maximum concurrent RPCs, keepalive settings,
   and TLS settings.
5. WHEN the gRPC server is constructed THEN the system SHALL accept a thread-pool/
   completion-queue-count configuration controlling how many gRPC-internal threads
   service incoming calls, independent of `Types::executor_type`.

### Requirement 13

**User Story:** As a system operator, I want the gRPC transport to emit metrics, so that I
can monitor performance and diagnose issues in production the same way I do for the HTTP
and CoAP transports.

#### Acceptance Criteria

1. WHEN the gRPC client or server is constructed THEN the system SHALL accept a metrics
   recorder instance conforming to the `metrics` concept.
2. WHEN the gRPC client sends a call THEN the system SHALL emit metrics for call count,
   latency, and request/response message size, dimensioned by `rpc_type` and
   `target_node_id`.
3. WHEN the gRPC server receives a call THEN the system SHALL emit metrics for call
   count, latency, and request/response message size, dimensioned by `rpc_type`.
4. WHEN a call completes with a non-`OK` status THEN the system SHALL emit an error
   metric dimensioned by the gRPC status code.
5. WHEN server lifecycle events occur (`start`/`stop`) THEN the system SHALL emit
   corresponding lifecycle metrics.

### Requirement 14

**User Story:** As a developer, I want the gRPC transport parameterized by a single types
template argument in the same style as the HTTP and CoAP transports, so that I can choose
the future implementation, executor, and metrics backend independently of the transport
protocol.

#### Acceptance Criteria

1. WHEN the gRPC client/server classes are defined THEN the system SHALL accept a single
   template parameter that conforms to a `grpc_transport_types` concept.
2. WHEN `grpc_transport_types` is defined THEN the system SHALL require `metrics_type`
   (satisfying `metrics`), `executor_type`, and a `future_template` template-template
   parameter, mirroring `transport_types` (`include/raft/types.hpp`) but omitting
   `serializer_type` since Protocol Buffers is the transport's fixed wire format.
3. WHEN `grpc_transport_types` validates `future_template` THEN the system SHALL verify
   it can be instantiated with `request_vote_response<>`, `append_entries_response<>`,
   and `install_snapshot_response<>`, and that each instantiation conforms to the
   `future` concept.
4. WHEN example `grpc_transport_types` implementations are created THEN the system SHALL
   demonstrate `kythira::Future` (the project's default backend) as `future_template`.

### Requirement 15

**User Story:** As a cluster operator, I want gRPC transport to optionally support the
ClusterJoin/ClusterLeave bootstrap RPCs, so that new nodes can join and leave a
gRPC-transported cluster the same way they already can over TCP transport.

#### Acceptance Criteria

1. WHEN the gRPC client provides `send_cluster_join_request`/`send_cluster_leave_request`
   THEN the system SHALL satisfy `network_client_with_cluster_join`/
   `network_client_with_cluster_leave` (`include/raft/network.hpp`).
2. WHEN the gRPC server provides `register_cluster_join_handler`/
   `register_cluster_leave_handler` THEN the system SHALL satisfy
   `network_server_with_cluster_join`/`network_server_with_cluster_leave`.
3. WHEN a ClusterJoin/ClusterLeave call is routed THEN the system SHALL address it by the
   joining/leaving node's contact address string (not yet a known node ID), consistent
   with the existing concept's `address_type`-keyed routing.
4. WHEN a `cluster_join_response`/`cluster_leave_response` carries a redirect hint THEN
   the system SHALL convert it to/from the `PeerInfo` protobuf message losslessly.

### Requirement 16

**User Story:** As a cluster operator, I want gRPC transport to optionally support the
RequestPreVote extension, so that a gRPC-transported cluster benefits from the disruptive-
server mitigation already available to other transports.

#### Acceptance Criteria

1. WHEN the gRPC client provides `send_request_pre_vote` THEN the system SHALL satisfy
   `network_client_with_pre_vote`.
2. WHEN the gRPC server provides `register_request_pre_vote_handler` THEN the system
   SHALL satisfy `network_server_with_pre_vote`.
3. WHEN a RequestPreVote call is made THEN the system SHALL use the same deadline,
   error-handling, and metrics behavior as the core RequestVote RPC.

### Requirement 17

**User Story:** As a cluster operator, I want gRPC transport to optionally support the
peer-to-peer `fetch_log_entries` extension, so that gossip-based catch-up
(`.kiro/specs/peer2peer-log-replication/`) can run over gRPC instead of requiring a
separate `tcp_gossip_peer2peer_replicator` channel.

#### Acceptance Criteria

1. WHEN the gRPC client provides `send_fetch_log_entries` THEN the system SHALL satisfy
   `network_client_with_log_fetch`.
2. WHEN the gRPC server provides `register_fetch_log_entries_handler` THEN the system
   SHALL satisfy `network_server_with_log_fetch`.
3. WHEN a `fetch_log_entries_response` is converted to/from protobuf THEN the system
   SHALL preserve every `LogEntry` in `entries()` without loss of `type()`.

### Requirement 18

**User Story:** As a build engineer, I want gRPC transport to be an optional dependency
that degrades gracefully when unavailable, consistent with every other optional
dependency in this project (CoAP/libcoap, AWS SDK, stdexec, etc.).

#### Acceptance Criteria

1. WHEN gRPC and Protobuf are not found on the build machine THEN the system SHALL skip
   building the gRPC transport target and continue configuring the rest of the project
   successfully, mirroring `LIBCOAP_FOUND`'s graceful-degradation behavior.
2. WHEN `-DKYTHIRA_KCONFIG_STRICT=ON` is set and the `GRPC_TRANSPORT` Kconfig symbol is
   selected but gRPC/Protobuf are not found THEN the system SHALL fail configuration with
   a descriptive error, mirroring `kythira_kconfig_require`'s existing behavior for
   `COAP_TRANSPORT`.
3. WHEN gRPC and Protobuf are found THEN the system SHALL generate `raft.pb.{h,cc}` and
   `raft.grpc.pb.{h,cc}` from `raft.proto` as part of the build, using `protoc` and
   `grpc_cpp_plugin` located via the detected gRPC/Protobuf installation.
4. WHEN the gRPC transport target is built THEN the system SHALL link against the gRPC
   C++ library, Protobuf runtime, and OpenSSL (already a required dependency for TLS
   elsewhere in the project).
5. WHEN gRPC is added as a project dependency THEN the system SHALL declare it in
   `vcpkg.json` and add a corresponding `GRPC_TRANSPORT` symbol to the root `Kconfig`
   file, following the same `depends on`/gating pattern as `COAP_TRANSPORT`.

### Requirement 19

**User Story:** As a testing engineer, I want the gRPC transport to be testable, so that I
can verify correctness through unit and property-based testing the same way the HTTP and
CoAP transports are tested.

#### Acceptance Criteria

1. WHEN property-based tests are executed THEN the system SHALL verify that every sent
   RPC either receives a response or the future resolves to a timeout/error within the
   configured deadline.
2. WHEN property-based tests are executed THEN the system SHALL verify that request/
   response protobuf conversion round-trips preserve message content for every RPC type,
   including edge cases (`entries()` empty, `conflict_index`/`conflict_term` absent,
   redirect hint absent).
3. WHEN property-based tests are executed THEN the system SHALL verify that concurrent
   calls from multiple client threads to the same server are handled correctly with no
   cross-talk between calls.
4. WHEN unit tests are executed THEN the system SHALL verify that `grpc_client`/
   `grpc_server` conform to `network_client`/`network_server` (and, separately, to each
   optional extension concept they implement) via `static_assert`.
5. WHEN unit tests are executed THEN the system SHALL verify server lifecycle transitions
   (`start`/`stop`/`is_running`) are safe to call repeatedly and from concurrent threads.
6. WHEN unit tests are executed THEN the system SHALL verify TLS/mTLS connection
   establishment and certificate-validation-failure rejection, using this project's
   existing `certificate_authority` test helpers to issue test certificates rather than
   checked-in fixed certificate files.
