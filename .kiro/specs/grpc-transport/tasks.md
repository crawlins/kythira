# Implementation Plan

## Implementation Status

Tasks 1–12 are implemented in the tree:

- `proto/raft.proto`, `vcpkg.json` (`grpc`), `Kconfig` (`GRPC_TRANSPORT`), and
  the `CMakeLists.txt` codegen + `raft_grpc_transport` target (Tasks 1–2).
- `include/raft/grpc_exceptions.hpp`, `grpc_message_conversion.hpp`,
  `grpc_transport.hpp` (types concept + config), and `grpc_transport_impl.hpp`
  (`grpc_client`/`grpc_server` with the core RPCs, TLS/mTLS, the callback API,
  metrics, and every optional extension), plus `src/grpc_transport_impl.cpp`
  (Tasks 2–9).
- Tests `tests/grpc_transport_conversion_property_test.cpp` (Property 2/3,
  concept `static_assert`s) and `tests/grpc_transport_integration_test.cpp`
  (end-to-end, unregistered→UNIMPLEMENTED, lifecycle, concurrency, mTLS),
  `examples/grpc_transport_example.cpp`, and README + `doc/grpc_transport_README.md`
  (Tasks 10–12).

Tasks 1.5 and 13 (verify generated code compiles; run the full suite; strict-mode
and graceful-degradation configure checks; performance sanity pass) require a
build machine with gRPC/Protobuf present and are validated in CI where the
`grpc` vcpkg port is installed — they are not checkable in a gRPC-absent
environment, where the target is (correctly) skipped.

## Major Tasks Overview

### Tasks 1-2: Schema and Build Integration
*Define `raft.proto` and wire up optional-dependency detection/codegen before any C++
transport code is written, so every later task can assume generated headers exist.*

### Tasks 3-6: Core Transport (`RaftService`)
*RequestVote/AppendEntries/InstallSnapshot client, server, TLS, and error handling —
parity with the HTTP and CoAP transports.*

### Tasks 7-9: Optional Extension Services
*ClusterJoin/ClusterLeave, RequestPreVote, and `fetch_log_entries` — parity with
`tcp_rpc`/`tls_tcp_rpc`'s full concept-family coverage.*

### Tasks 10-13: Testing, Examples, Documentation, Final Validation

## Detailed Task List

- [ ] 1. Define `raft.proto` and generated-code build integration
  - [ ] 1.1 Write `proto/raft.proto`
    - Declare `kythira.raft.v1` package, `EntryType` enum, `LogEntry` message
    - Declare RequestVote/AppendEntries/InstallSnapshot request/response messages and
      `RaftService`
    - Declare `RequestPreVoteRequest`/`Response` and `RaftElectionExtensionService`
    - Declare `PeerInfo`, ClusterJoin/ClusterLeave request/response messages and
      `RaftBootstrapService`
    - Declare `FetchLogEntriesRequest`/`Response` and `RaftPeerReplicationService`
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6_

  - [ ] 1.2 Add `grpc` to `vcpkg.json`
    - Add the `grpc` dependency with a minimum version providing the stable (non-
      experimental) callback API
    - _Requirements: 18.4, 18.5_

  - [ ] 1.3 Add `GRPC_TRANSPORT` Kconfig symbol
    - Add symbol and help text to the root `Kconfig`, following the `COAP_TRANSPORT`
      pattern
    - _Requirements: 18.4, 18.5_

  - [ ] 1.4 Wire `find_package(gRPC)`/`find_package(Protobuf)` and codegen into
        `CMakeLists.txt`
    - `kythira_kconfig_gate(GRPC_TRANSPORT)` / `kythira_kconfig_require(...)` following
      the `COAP_TRANSPORT` gating pattern
    - Graceful skip with a warning when gRPC/Protobuf are not found and
      `KYTHIRA_KCONFIG_STRICT` is not set
    - `add_custom_command` invoking `protobuf::protoc` with `gRPC::grpc_cpp_plugin` to
      generate `raft.pb.{h,cc}`/`raft.grpc.pb.{h,cc}` into the build directory (not
      checked in)
    - Define the `raft_grpc_transport` target linking `gRPC::grpc++`,
      `protobuf::libprotobuf`, `OpenSSL::SSL`, `OpenSSL::Crypto`
    - _Requirements: 1.6, 2.7, 18.1, 18.2, 18.3_

  - [ ] 1.5 Verify generated code compiles standalone
    - Confirm `raft.pb.h`/`raft.grpc.pb.h` compile cleanly under this project's
      `.clang-format`-adjacent build flags before any hand-written code depends on them
    - _Requirements: 1.6_

- [ ] 2. Implement exception types and message conversion
  - [ ] 2.1 Create `include/raft/grpc_exceptions.hpp`
    - `grpc_transport_error` base, `grpc_connection_error`, `grpc_timeout_error`,
      `grpc_client_error`, `grpc_server_error`, `grpc_tls_configuration_error`
    - _Requirements: 11.1, 11.2, 11.3, 11.4, 11.5, 9.6_

  - [ ] 2.2 Create `include/raft/grpc_message_conversion.hpp`
    - `to_proto`/`from_proto` for `LogEntry`/`EntryType`, RequestVote, AppendEntries,
      InstallSnapshot request/response pairs
    - `to_proto`/`from_proto` for RequestPreVote, ClusterJoin/ClusterLeave (incl.
      `PeerInfo` redirect hint), `fetch_log_entries`
    - Fail closed on out-of-range/invalid enum conversion (surfaced later as
      `INVALID_ARGUMENT` per Requirement 11.6, not undefined behavior)
    - _Requirements: 2.4, 2.5, 3.3, 3.4, 4.3, 4.4, 5.3, 5.4, 11.6, 15.4, 16.3, 17.3_

- [ ] 3. Implement `grpc_transport_types` concept and configuration structures
  - [ ] 3.1 Define `grpc_transport_types` concept in `include/raft/grpc_transport.hpp`
    - `metrics_type` (satisfying `metrics`), `executor_type`, `future_template`
      template-template parameter validated against the three core response types
    - Deliberately no `serializer_type` member (see design doc's "Protobuf replaces
      `rpc_serializer`" decision)
    - _Requirements: 14.1, 14.2, 14.3_

  - [ ] 3.2 Provide an example `grpc_transport_types` implementation
    - `kythira::Future`-backed example analogous to `http_transport_types`
    - _Requirements: 14.4_

  - [ ] 3.3 Define `grpc_client_config`/`grpc_server_config`
    - Message-size limits, keepalive settings, TLS fields (PEM strings + file-path
      convenience constructors), `user_agent`, `enable_health_check_service`,
      `enable_reflection`
    - _Requirements: 9.5, 9.6, 12.2, 12.4, 12.5_

- [ ] 4. Implement `grpc_client<Types>` — core RPCs
  - [ ] 4.1 Class skeleton
    - Template with `Types` parameter, `requires grpc_transport_types<Types>`
    - Constructor accepting `node_id_to_target_map`, `config`, `metrics`
    - `_channels`/`_stubs` maps, `_channel_credentials`, `_mutex`
    - _Requirements: 1.1, 12.1, 12.3, 14.1_

  - [ ] 4.2 Implement `get_or_create_channel` (node-ID-keyed and address-keyed
        overloads)
    - Build `grpc::ChannelArguments` from `config` (message size, keepalive)
    - Construct `Channel` with `_channel_credentials`; cache in `_channels`
    - _Requirements: 12.2, 12.3_

  - [ ] 4.3 Implement `build_channel_credentials` (TLS)
    - Insecure by default; `grpc::SslCredentials` when `config.enable_tls`
    - Mutual TLS (client cert/key) when configured
    - Fail construction with `grpc_tls_configuration_error` on invalid material rather
      than falling back to insecure
    - _Requirements: 9.1, 9.3, 9.4, 9.5, 9.6, 9.7_

  - [ ] 4.4 Implement generic `call_unary` helper
    - Deadline from `timeout`; `grpc::CallbackClientContext`/`ClientUnaryReactor`
    - Promise fulfillment posted onto `Types::executor_type`
    - `status_to_exception` per the Error Handling mapping table
    - Metrics emission (count, latency, size, errors)
    - _Requirements: 8.1, 8.2, 10.1, 10.2, 10.3, 11.1, 11.2, 11.3, 11.4, 11.5, 13.2, 13.4_

  - [ ] 4.5 Implement `send_request_vote`/`send_append_entries`/`send_install_snapshot`
    - Each a thin `to_proto` + `call_unary` wrapper against `RaftService::Stub`
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 4.1, 4.2, 4.3, 4.4, 4.5, 5.1, 5.2, 5.3,
      5.4, 5.5_

  - [ ] 4.6 `static_assert(network_client<grpc_client<...>>)`
    - _Requirements: 1.1_

- [ ] 5. Implement `grpc_server<Types>` — core RPCs
  - [ ] 5.1 Class skeleton
    - Template with `Types` parameter, `requires grpc_transport_types<Types>`
    - Constructor accepting `bind_address`, `bind_port`, `config`, `metrics`
    - `_handlers`, `_server`, `_running`, `_mutex`
    - _Requirements: 1.2, 12.4, 14.1_

  - [ ] 5.2 Implement `register_request_vote_handler`/`register_append_entries_handler`/
        `register_install_snapshot_handler`
    - _Requirements: 6.1_

  - [ ] 5.3 Implement `RaftService::CallbackService` override
    - Generic `handle_unary` helper: `from_proto`, invoke handler on
      `Types::executor_type`, `to_proto` the result, `reactor->Finish(...)`
    - Exception-from-handler → `INTERNAL` status; conversion failure → `INVALID_ARGUMENT`
    - _Requirements: 6.2, 6.3, 6.4, 6.5, 8.3, 8.4, 11.6_

  - [ ] 5.4 Implement `build_server_credentials` (TLS)
    - Insecure by default; `grpc::SslServerCredentials` when `config.enable_tls`
    - `require_client_cert` mutual-TLS enforcement
    - _Requirements: 9.2, 9.3, 9.4, 9.5, 9.6, 9.7_

  - [ ] 5.5 Implement `start()`/`stop()`/`is_running()`
    - `grpc::ServerBuilder`: bind, register only services with at least one handler
      configured, apply credentials/keepalive/message-size options, `BuildAndStart()`
    - `stop()`: `Shutdown()` with a grace deadline, join server thread
    - Safe to call repeatedly; destructor stops the server if still running
    - _Requirements: 7.1, 7.2, 7.3, 7.4_

  - [ ] 5.6 Metrics emission (server)
    - Count/latency/size/error metrics per call; lifecycle metrics on start/stop
    - _Requirements: 13.1, 13.3, 13.4, 13.5_

  - [ ] 5.7 `static_assert(network_server<grpc_server<...>>)`
    - _Requirements: 1.2_

- [ ] 6. TLS/mTLS integration testing groundwork
  - [ ] 6.1 Add a test helper issuing short-lived client/server certificates via
        `certificate_authority` for use in gRPC transport tests
    - Mirrors how `tls_tcp_rpc_test.cpp`/`ca_bootstrap_client_test.cpp` source test
      certificates today, rather than checking in fixed PEM fixtures
    - _Requirements: 19.6_

- [ ] 7. Implement optional extension: RequestPreVote
  - [ ] 7.1 Client: `send_request_pre_vote` against `RaftElectionExtensionService::Stub`
    - _Requirements: 16.1, 16.3_
  - [ ] 7.2 Server: `register_request_pre_vote_handler` +
        `RaftElectionExtensionService::CallbackService` override
    - Service registered with `ServerBuilder` only if a handler was configured
    - _Requirements: 16.2, 6.3_
  - [ ] 7.3 `static_assert`s for `network_client_with_pre_vote`/
        `network_server_with_pre_vote`
    - _Requirements: 16.1, 16.2_

- [ ] 8. Implement optional extension: ClusterJoin/ClusterLeave bootstrap
  - [ ] 8.1 Client: `send_cluster_join_request`/`send_cluster_leave_request` against
        `RaftBootstrapService::Stub`, addressed by contact address (not node ID) via
        `get_or_create_channel(const std::string&)`
    - _Requirements: 15.1, 15.3_
  - [ ] 8.2 Server: `register_cluster_join_handler`/`register_cluster_leave_handler` +
        `RaftBootstrapService::CallbackService` override
    - _Requirements: 15.2_
  - [ ] 8.3 `PeerInfo` redirect-hint round-trip
    - _Requirements: 15.4_
  - [ ] 8.4 `static_assert`s for `network_client_with_cluster_join/_leave`/
        `network_server_with_cluster_join/_leave`
    - _Requirements: 15.1, 15.2_

- [ ] 9. Implement optional extension: peer-to-peer `fetch_log_entries`
  - [ ] 9.1 Client: `send_fetch_log_entries` against `RaftPeerReplicationService::Stub`
    - _Requirements: 17.1_
  - [ ] 9.2 Server: `register_fetch_log_entries_handler` +
        `RaftPeerReplicationService::CallbackService` override
    - _Requirements: 17.2_
  - [ ] 9.3 `LogEntry`/`entry_type` round-trip fidelity in `fetch_log_entries_response`
    - _Requirements: 17.3_
  - [ ] 9.4 `static_assert`s for `network_client_with_log_fetch`/
        `network_server_with_log_fetch`
    - _Requirements: 17.1, 17.2_

- [ ] 10. Property-based testing
  - [ ] 10.1 **Property 1: Every call resolves within its deadline**
    - **Validates: Requirements 10.1, 10.2, 19.1**
  - [ ] 10.2 **Property 2: Protobuf round-trip preserves content**
    - **Validates: Requirements 2.1, 2.4, 2.5, 15.4, 17.3, 19.2**
  - [ ] 10.3 **Property 3: Status code maps to the correct exception type**
    - **Validates: Requirements 11.1, 11.2, 11.3, 11.4, 11.5**
  - [ ] 10.4 **Property 4: Handler invocation for every registered RPC**
    - **Validates: Requirements 6.2, 6.4**
  - [ ] 10.5 **Property 5: Unregistered optional RPCs never crash the server**
    - **Validates: Requirement 6.3**
  - [ ] 10.6 **Property 6: Channel reuse for repeated calls to the same target**
    - **Validates: Requirement 12.3**
  - [ ] 10.7 **Property 7: TLS is never silently downgraded**
    - **Validates: Requirements 9.1, 9.2, 9.6**

- [ ] 11. Unit and integration testing
  - [ ] 11.1 Unit tests for `grpc_client`
    - Concept conformance, construction, per-RPC success/error/timeout paths, channel
      reuse, TLS config validation, metrics emission
    - _Requirements: 1.1, 3.4, 3.5, 4.4, 4.5, 5.4, 5.5, 9.1, 9.3, 9.6, 10.1, 10.2, 12.1,
      12.2, 12.3, 13.2_
  - [ ] 11.2 Unit tests for `grpc_server`
    - Concept conformance, handler registration, lifecycle safety, error handling
      (handler exception, malformed request, unregistered service), TLS config
      validation, metrics emission
    - _Requirements: 1.2, 6.1, 6.3, 6.5, 7.1, 7.2, 7.3, 7.4, 9.2, 9.4, 9.6, 11.6, 13.1,
      13.3_
  - [ ] 11.3 Integration test: client-server end-to-end for all core and optional RPCs
    - Insecure and mutual-TLS variants
    - _Requirements: 3.2, 3.4, 4.2, 4.4, 5.2, 5.4, 9.3, 9.4, 15.1, 15.2, 16.1, 16.2,
      17.1, 17.2_
  - [ ] 11.4 Integration test: concurrent calls
    - Multiple client threads calling the same server concurrently; verify no cross-talk
      and correct connection reuse under load
    - _Requirements: 12.3, 19.3_
  - [ ] 11.5 Integration test: graceful shutdown
    - `stop()` while calls are in flight completes them before returning
    - _Requirements: 7.2_

- [ ] 12. Documentation and examples
  - [ ] 12.1 Example program demonstrating gRPC transport usage
    - Client/server setup with `grpc_transport_types`, all core and optional RPCs, TLS
      configuration, error handling, metrics collection
    - Follow this project's example-program guidelines (comprehensive scenario
      coverage, exit codes)
    - _Requirements: All_
  - [ ] 12.2 README updates
    - Add gRPC transport to the features list and Transport Layer Comparison table
    - Add a usage example and TLS/mTLS setup instructions, following the existing HTTP/
      CoAP transport sections' structure
    - Link to this spec directory the way other feature sections link to their
      `.kiro/specs/<feature>/` design docs
    - _Requirements: All (documentation only; not required for this spec itself)_
  - [ ] 12.3 Troubleshooting notes
    - Common gRPC/protoc/vcpkg build issues, TLS/certificate issues, deadline tuning
    - _Requirements: All (documentation only; not required for this spec itself)_

- [ ] 13. Final validation
  - [ ] 13.1 Run the full test suite with `GRPC_TRANSPORT` enabled
    - All unit, property, and integration tests compile and pass under CTest
    - _Requirements: 19.1-19.6_
  - [ ] 13.2 Confirm graceful degradation with `GRPC_TRANSPORT` unset/gRPC absent
    - Rest of the project configures and builds unaffected
    - _Requirements: 18.1_
  - [ ] 13.3 Confirm `KYTHIRA_KCONFIG_STRICT` failure path
    - Configuration fails loudly when `GRPC_TRANSPORT` is selected but gRPC/Protobuf are
      missing
    - _Requirements: 18.2_
  - [ ] 13.4 Performance sanity pass
    - Basic throughput/latency numbers recorded (not necessarily a full comparison
      report) before declaring the transport production-ready
    - _Requirements: (Performance Considerations, design.md)_
