# Implementation Plan

## Major Tasks Overview

### Tasks 1-2: Schema and Build Integration
*Define `raft_messages.proto` and wire up optional-dependency detection/codegen before
any C++ serializer code is written.*

### Tasks 3-5: Core Serializer Implementation
*`protobuf_rpc_serializer<Data>` — message-type tag, `NodeIdValue` wrapper, `to_proto`/
`from_proto` conversions, and the public `serialize`/`deserialize_*`/`name()` surface.*

### Tasks 6-9: Testing, Benchmarking, Documentation, Final Validation

## Detailed Task List

- [ ] 1. Define `raft_messages.proto` and generated-code build integration
  - [ ] 1.1 Write `proto/raft_messages.proto`
    - Declare `kythira.raft.serializer.v1` package, `NodeIdValue` oneof wrapper,
      `EntryType` enum, `LogEntry` message
    - Declare RequestVote/RequestPreVote/AppendEntries/InstallSnapshot request/response
      messages
    - Declare `PeerInfo`, ClusterJoin/ClusterLeave request/response messages
    - Declare `FetchLogEntriesRequest`/`Response`
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 3.1_

  - [ ] 1.2 Add `protobuf` to `vcpkg.json`
    - _Requirements: 9.4, 9.5_

  - [ ] 1.3 Add `PROTOBUF_SERIALIZER` Kconfig symbol under a new `menu "Serialization"`
    - Add symbol and help text to the root `Kconfig`, following the `COAP_TRANSPORT`
      pattern but noting independence from `GRPC_TRANSPORT`
    - _Requirements: 9.5_

  - [ ] 1.4 Wire `find_package(Protobuf)` and codegen into `CMakeLists.txt`
    - `kythira_kconfig_gate(PROTOBUF_SERIALIZER)` / `kythira_kconfig_require(...)`
      following the `COAP_TRANSPORT` gating pattern
    - Graceful skip with a warning when Protobuf is not found and
      `KYTHIRA_KCONFIG_STRICT` is not set
    - `add_custom_command` invoking `protobuf::protoc` to generate
      `raft_messages.pb.{h,cc}` into the build directory (not checked in)
    - Define the `raft_protobuf_serializer` target linking `protobuf::libprotobuf`
    - _Requirements: 2.6, 9.1, 9.2, 9.3, 9.4_

  - [ ] 1.5 Verify generated code compiles standalone
    - Confirm `raft_messages.pb.h` compiles cleanly before any hand-written code depends
      on it
    - _Requirements: 2.6_

- [ ] 2. Implement message-type tag and `NodeIdValue` helpers
  - [ ] 2.1 Define `message_tag` enum (14 values) in `include/raft/protobuf_serializer.hpp`
    - _Requirements: 5.1_

  - [ ] 2.2 Implement `encode`/`decode` private helpers
    - `encode`: prepend tag byte, `SerializeAsString()`
    - `decode`: length check, tag check (throw `serialization_exception` on mismatch),
      `ParseFromArray` on the remainder (throw `serialization_exception` on parse
      failure)
    - _Requirements: 5.1, 5.2, 5.3, 5.4_

  - [ ] 2.3 Implement `to_node_id_value<NodeId>`/`from_node_id_value<NodeId>`
    - `if constexpr (std::same_as<NodeId, std::string>)` branch selecting the `oneof`
      case; throw `serialization_exception` on populated-case/requested-type mismatch on
      the decode side
    - _Requirements: 3.1, 3.2_

- [ ] 3. Implement `protobuf_rpc_serializer<Data>` — core RPC messages
  - [ ] 3.1 Class skeleton with `requires serialized_data<Data>` constraint
    - _Requirements: 1.1, 7.1_

  - [ ] 3.2 `to_proto`/`from_proto` for `LogEntry`/`EntryType`
    - _Requirements: 2.2, 6.5_

  - [ ] 3.3 `serialize`/`deserialize_request_vote_request`/`deserialize_request_vote_response`
    - Using `NodeIdValue` for `candidate_id`
    - _Requirements: 3.3, 4.1, 4.2, 6.1_

  - [ ] 3.4 `serialize`/`deserialize_request_pre_vote_request`/
        `deserialize_request_pre_vote_response`
    - _Requirements: 4.1, 4.2, 6.1_

  - [ ] 3.5 `serialize`/`deserialize_append_entries_request`/
        `deserialize_append_entries_response`
    - `entries()` as `repeated LogEntry`, including the empty-list case
    - `conflict_index`/`conflict_term` as proto3 `optional`, including the absent case
    - _Requirements: 4.1, 4.2, 6.1, 6.2, 6.3_

  - [ ] 3.6 `serialize`/`deserialize_install_snapshot_request`/
        `deserialize_install_snapshot_response`
    - `data` passed through as raw `bytes` (no base64)
    - _Requirements: 4.1, 4.2, 6.1, 6.6_

  - [ ] 3.7 `static_assert(rpc_serializer<protobuf_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>>)`
    - _Requirements: 1.1, 4.5_

- [ ] 4. Implement `protobuf_rpc_serializer<Data>` — bootstrap and peer2peer extension
      messages
  - [ ] 4.1 `PeerInfo` `to_proto`/`from_proto`, including absent-redirect handling
    - _Requirements: 2.3, 6.4_

  - [ ] 4.2 `serialize`/`deserialize_cluster_join_request`/
        `deserialize_cluster_join_response`
    - _Requirements: 4.1, 4.2, 6.1, 6.4_

  - [ ] 4.3 `serialize`/`deserialize_cluster_leave_request`/
        `deserialize_cluster_leave_response`
    - _Requirements: 4.1, 4.2, 6.1, 6.4_

  - [ ] 4.4 `serialize`/`deserialize_fetch_log_entries_request`/
        `deserialize_fetch_log_entries_response`
    - `responder_id` stays a bare `uint64` (not `NodeIdValue`), matching
      `fetch_log_entries_response::_responder_id`'s fixed `std::uint64_t` type
    - _Requirements: 4.1, 4.2, 6.1_

- [ ] 5. Implement generic dispatcher and `name()`
  - [ ] 5.1 `template<typename T> deserialize<T>(const Data&) const -> T` dispatch table
    - One `if constexpr` branch per message type, mirroring
      `json_rpc_serializer::deserialize<T>`
    - _Requirements: 4.3_

  - [ ] 5.2 `name()` returns `"protobuf"`
    - _Requirements: 4.4_

- [ ] 6. Unit and concept-conformance testing
  - [ ] 6.1 `tests/protobuf_rpc_serializer_concept_test.cpp`
    - `static_assert` against `rpc_serializer`; smoke-test serialize/deserialize for
      `request_vote_request<>`; `name()` check
    - _Requirements: 10.1_

  - [ ] 6.2 Register test target in `tests/CMakeLists.txt`, gated on `Protobuf_FOUND`
    - _Requirements: 9.1_

- [ ] 7. Property-based testing
  - [ ] 7.1 **Property 1: Round-trip fidelity for every message type**
    - One test case per message type plus edge cases (empty `entries()`, absent
      `conflict_index`/`conflict_term`, absent redirect)
    - **Validates: Requirements 2.1, 2.2, 2.3, 2.4, 6.1, 6.2, 6.3, 6.4, 6.5, 6.6**
  - [ ] 7.2 **Property 2: Generic `NodeId` round-trips for both supported instantiations**
    - `std::uint64_t` and `std::string` `NodeId`, mirroring
      `tests/rpc_serialization_property_test.cpp`'s existing `std::string` coverage
    - **Validates: Requirements 3.1, 3.2, 3.3, 3.4**
  - [ ] 7.3 **Property 3: Wrong-tag payloads are always rejected**
    - Serialize as type `X`, attempt `deserialize_*` for every other type `Y`
    - **Validates: Requirements 5.1, 5.2, 5.6**
  - [ ] 7.4 **Property 4: Malformed/random payloads are rejected at a high rate**
    - Random bytes, including 0-byte and 1-byte (tag-only) payloads
    - **Validates: Requirements 5.3, 5.4, 5.5**
  - [ ] 7.5 **Property 5: Transparent drop-in for `json_rpc_serializer`**
    - End-to-end exchange over `cpp_httplib_client`/`server` with
      `serializer_type = protobuf_rpc_serializer<...>`
    - **Validates: Requirements 1.1, 1.2, 4.1, 4.2, 4.3, 4.4, 4.5, 10.4**

- [ ] 8. Benchmarking and documentation
  - [ ] 8.1 Write a benchmark comparing `protobuf_rpc_serializer` vs. `json_rpc_serializer`
    - Payload size and serialize/deserialize latency across varying `entries()` counts
      and command sizes
    - _Requirements: 8.1_
  - [ ] 8.2 Record results in `doc/protobuf_serializer_performance_comparison.md`
    - _Requirements: 8.2_
  - [ ] 8.3 README updates
    - Mention `protobuf_rpc_serializer` as an alternative `Types::serializer_type`,
      following the existing HTTP/CoAP transport sections' structure
    - Link to this spec directory
    - _Requirements: All (documentation only; not required for this spec itself)_

- [ ] 9. Final validation
  - [ ] 9.1 Run the full test suite with `PROTOBUF_SERIALIZER` enabled
    - All unit, property, and integration tests compile and pass under CTest
    - _Requirements: 10.1, 10.2, 10.3, 10.4_
  - [ ] 9.2 Confirm graceful degradation with `PROTOBUF_SERIALIZER` unset/Protobuf absent
    - Rest of the project configures and builds unaffected
    - _Requirements: 9.1_
  - [ ] 9.3 Confirm `KYTHIRA_KCONFIG_STRICT` failure path
    - Configuration fails loudly when `PROTOBUF_SERIALIZER` is selected but Protobuf is
      missing
    - _Requirements: 9.2_
  - [ ] 9.4 Confirm coexistence with `GRPC_TRANSPORT`
    - Both symbols selectable independently (if/when `grpc-transport` is implemented)
      without duplicate-symbol/proto-package collisions
    - _Requirements: 1.4_
