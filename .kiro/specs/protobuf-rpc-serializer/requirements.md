# Requirements Document

## Introduction

This document specifies the requirements for a Protocol Buffers implementation of the
`rpc_serializer` concept (`include/raft/types.hpp`), analogous to the existing
`json_rpc_serializer` (`include/raft/json_serializer.hpp`). `protobuf_rpc_serializer`
converts every `kythira` Raft RPC request/response struct to and from an opaque
`std::vector<std::byte>` payload backed by generated Protocol Buffers message classes,
so that the HTTP, CoAP, and TCP transports — all of which are parameterized by
`Types::serializer_type` and only move opaque byte buffers — can be switched from JSON to
a smaller, faster binary wire format without any transport-layer code change. This is
distinct from the (separately specified, not-yet-implemented) `grpc-transport`
(`.kiro/specs/grpc-transport/`), which uses Protocol Buffers as gRPC's fixed RPC framing
and does not use `rpc_serializer` at all; this spec instead plugs Protocol Buffers into
the existing generic serializer slot so that today's HTTP/CoAP/TCP transports benefit
without adopting gRPC.

## Glossary

- **protobuf_rpc_serializer**: The class implementing the `rpc_serializer` concept using
  Protocol Buffers, parameterized by `Data` the same way `json_rpc_serializer<Data>` is.
- **rpc_serializer**: The concept (`include/raft/types.hpp`) that `Types::serializer_type`
  must satisfy; the pluggable component the HTTP/CoAP/TCP transports use to convert
  between `kythira` request/response structs and a `serialized_data`-satisfying byte
  buffer.
- **serialized_data**: The concept constraining `Data` to a `std::ranges::range` whose
  value type is `std::byte` (e.g. `std::vector<std::byte>`).
- **Protocol Buffers (protobuf)**: Google's language-neutral binary serialization format
  and IDL compiler (`protoc`), used here purely as a wire-format library — no gRPC
  service/channel/stub machinery is involved.
- **raft_messages.proto**: The `.proto` file declaring one message per `kythira` RPC
  request/response struct; the canonical schema for this serializer's wire format.
- **message-type tag**: A single discriminator byte prepended to every serialized payload
  identifying which `raft_messages.proto` message follows, analogous to
  `json_rpc_serializer`'s `"type"` JSON field, needed because raw Protocol Buffers bytes
  are not self-describing the way JSON is.
- **NodeIdValue**: A small `oneof`-based wrapper message used wherever a `kythira` struct
  field is generic over `NodeId` (which may be instantiated as `std::uint64_t` or
  `std::string`), since a single protobuf field cannot change static type per template
  instantiation the way a C++ template parameter can.
- **generic deserialize dispatcher**: The `template<typename T> auto deserialize(const
  Data&) const -> T` method that dispatches to the type-specific `deserialize_*` method
  based on `T`, mirroring `json_rpc_serializer::deserialize<T>`.
- **serialization_exception**: The existing exception type (`include/raft/exceptions.hpp`)
  thrown by `json_rpc_serializer` on malformed or mismatched-type input; reused here for
  consistency rather than introducing a parallel protobuf-specific exception hierarchy.

## Requirements

### Requirement 1

**User Story:** As a distributed systems developer, I want a Protocol Buffers
implementation of `rpc_serializer`, so that I can use a compact binary wire format with
the existing HTTP, CoAP, and TCP transports without writing custom serialization code or
switching to gRPC.

#### Acceptance Criteria

1. WHEN `protobuf_rpc_serializer<Data>` is instantiated with `Data` satisfying
   `serialized_data` THEN the system SHALL satisfy the `rpc_serializer<S, Data>` concept.
2. WHEN `protobuf_rpc_serializer<Data>` is used as `Types::serializer_type` for
   `cpp_httplib_client`/`cpp_httplib_server`, `coap_client`/`coap_server`, or
   `tcp_rpc`/`tls_tcp_rpc` THEN the system SHALL require no changes to those transports'
   implementation, matching how `json_rpc_serializer` plugs in today.
3. WHEN `protobuf_rpc_serializer` is defined THEN the system SHALL NOT depend on the gRPC
   C++ library or any `grpc-transport` (`.kiro/specs/grpc-transport/`) type — Protobuf is
   used purely as an encoding library here.
4. WHEN both `protobuf_rpc_serializer` and (if separately implemented) `grpc-transport`
   are enabled in the same build THEN the system SHALL NOT produce duplicate-symbol or
   duplicate-proto-package errors, since each declares its own independent `.proto`
   package (Requirement 9).

### Requirement 2

**User Story:** As a systems architect, I want every Raft RPC message shape declared in
one canonical `.proto` file, so that the wire schema is self-documenting, versionable, and
usable from other languages if this serializer's output is ever consumed outside this
codebase.

#### Acceptance Criteria

1. WHEN `raft_messages.proto` is defined THEN the system SHALL declare a message for each
   of `request_vote_request`, `request_vote_response`, `request_pre_vote_request`,
   `request_pre_vote_response`, `append_entries_request` (including a nested/repeated
   `LogEntry` message), `append_entries_response`, `install_snapshot_request`,
   `install_snapshot_response`, `cluster_join_request`, `cluster_join_response`,
   `cluster_leave_request`, `cluster_leave_response`, `fetch_log_entries_request`, and
   `fetch_log_entries_response` — the same fourteen shapes `json_rpc_serializer` already
   supports.
2. WHEN a `LogEntry` message is defined THEN the system SHALL represent `term`, `index`,
   `command` (as `bytes`), and `type` (as an enum mirroring `kythira::entry_type`:
   `normal`, `configuration`, `no_op`).
3. WHEN a redirect hint is represented (`cluster_join_response`/`cluster_leave_response`)
   THEN the system SHALL declare an optional `PeerInfo` message mirroring
   `kythira::peer_info<NodeId, Address>`, absent when no redirect applies.
4. WHEN an `AppendEntriesResponse` message is defined THEN the system SHALL represent
   `conflict_index`/`conflict_term` as proto3 `optional` fields, present only when the
   corresponding `std::optional` in `append_entries_response<>` holds a value.
5. WHEN the proto package is declared THEN the system SHALL use a versioned package name
   distinct from `grpc-transport`'s `kythira.raft.v1` (e.g. `kythira.raft.serializer.v1`),
   so the two specs' generated code can coexist without collision (Requirement 1.4).
6. WHEN generated code is produced THEN the system SHALL check `raft_messages.proto` into
   the repository and generate `raft_messages.pb.{h,cc}` at build time via `protoc` rather
   than committing generated code.

### Requirement 3

**User Story:** As a Raft node operator, I want fields that are generic over `NodeId` (a
template parameter that may be `std::uint64_t` or `std::string` in this codebase) to
round-trip correctly regardless of which concrete type is used, so that
`protobuf_rpc_serializer` is a drop-in replacement for `json_rpc_serializer` for every
`NodeId` instantiation the test suite already exercises.

#### Acceptance Criteria

1. WHEN a `kythira` field typed as `NodeId` (e.g. `candidate_id`, `leader_id`, `node_id`,
   `requester_id`) is serialized THEN the system SHALL encode it via a `NodeIdValue`
   `oneof` wrapper message carrying either a `uint64` or a `string` variant, selected at
   compile time via `if constexpr (std::same_as<NodeId, std::string>)`.
2. WHEN a `NodeIdValue` is deserialized against a target `NodeId` type THEN the system
   SHALL read the matching `oneof` case and SHALL throw `serialization_exception` if the
   wire data's populated case does not match the requested `NodeId` type.
3. WHEN `request_vote_request<std::string, TermId, LogIndex>` is serialized and
   deserialized THEN the system SHALL round-trip `candidate_id()` losslessly, matching the
   existing `std::string` `NodeId` coverage in
   `tests/rpc_serialization_property_test.cpp`.
4. WHEN `Address` fields (e.g. `contact_address`, `peer_info::address`) are serialized
   THEN the system SHALL encode them as protobuf `string`, matching this codebase's
   universal default of `Address = std::string`.

### Requirement 4

**User Story:** As a Raft node, I want `protobuf_rpc_serializer` to expose the same public
method surface as `json_rpc_serializer`, so that swapping `Types::serializer_type` is the
only change required to switch a transport from JSON to Protocol Buffers.

#### Acceptance Criteria

1. WHEN `protobuf_rpc_serializer<Data>::serialize(msg)` is called for any of the fourteen
   message types in scope THEN the system SHALL return a `Data` payload via overload
   resolution on `msg`'s concrete type, mirroring `json_rpc_serializer::serialize`'s
   overload set.
2. WHEN a type-specific deserialize method (e.g. `deserialize_request_vote_request`,
   `deserialize_append_entries_response`) is called with a `Data` payload THEN the system
   SHALL return the corresponding populated `kythira` struct.
3. WHEN the generic `template<typename T> deserialize<T>(const Data&)` dispatcher is
   called THEN the system SHALL dispatch to the correct type-specific method for every
   `T` in scope, mirroring `json_rpc_serializer::deserialize<T>`.
4. WHEN `name()` is called on `protobuf_rpc_serializer` THEN the system SHALL return
   `"protobuf"`, distinguishing it from `json_rpc_serializer::name()`'s `"json"`.
5. WHEN `protobuf_rpc_serializer<std::vector<std::byte>>` is checked against
   `kythira::rpc_serializer` THEN the system SHALL pass a `static_assert`, mirroring
   `json_serializer.hpp`'s existing `static_assert` for `json_rpc_serializer`.

### Requirement 5

**User Story:** As a reliability engineer, I want malformed or mistyped input rejected
with a clear exception rather than silently producing incorrect data, so that a corrupted
or adversarial peer cannot cause `protobuf_rpc_serializer` to hand the Raft core a
plausible-looking but wrong value the way an unchecked Protocol Buffers parse could.

#### Acceptance Criteria

1. WHEN any `serialize()` call produces a payload THEN the system SHALL prepend a
   one-byte message-type tag before the Protocol-Buffers-encoded message bytes.
2. WHEN a type-specific `deserialize_*` method reads a payload whose leading tag byte does
   not match the expected message type THEN the system SHALL throw
   `serialization_exception` without attempting to parse the remaining bytes as that
   message type.
3. WHEN a type-specific `deserialize_*` method reads a payload whose tag matches but whose
   remaining bytes fail `ParseFromArray`/equivalent protobuf parsing THEN the system SHALL
   throw `serialization_exception`.
4. WHEN a `deserialize_*` method is given an empty payload or a payload shorter than the
   one-byte tag THEN the system SHALL throw `serialization_exception` rather than reading
   out of bounds.
5. WHEN property-based testing feeds random byte sequences to any `deserialize_*` method
   THEN the system SHALL reject at least 95% of them, mirroring the existing malformed-
   message rejection bar in `tests/rpc_malformed_message_property_test.cpp`.
6. WHEN property-based testing feeds a validly-tagged-and-encoded message of one type to
   the `deserialize_*` method for a different type THEN the system SHALL reject it via the
   tag check in Acceptance Criterion 2, closing the gap where lenient Protocol Buffers
   parsing might otherwise accept bytes from the wrong message shape.

### Requirement 6

**User Story:** As a distributed systems developer, I want round-trip fidelity for every
field of every RPC message, including edge cases like empty log-entry lists and absent
optional fields, so that switching to Protocol Buffers never silently drops or corrupts
data that JSON serialization preserved.

#### Acceptance Criteria

1. WHEN any of the fourteen message types is serialized and then deserialized THEN the
   system SHALL reproduce every field's value exactly, including `std::vector<LogEntry>`
   entries in `append_entries_request`/`fetch_log_entries_response`.
2. WHEN `append_entries_request::entries()` is empty THEN the system SHALL round-trip it
   as an empty (not null/absent) repeated field.
3. WHEN `append_entries_response::conflict_index()`/`conflict_term()` are absent
   (`std::nullopt`) THEN the system SHALL round-trip them as absent, and WHEN present
   THEN the system SHALL round-trip the held value.
4. WHEN `cluster_join_response`/`cluster_leave_response::redirect_peer()` is absent THEN
   the system SHALL round-trip it as absent, and WHEN present THEN the system SHALL
   round-trip the held `peer_info<NodeId, Address>` losslessly.
5. WHEN a `log_entry::type()` is any `kythira::entry_type` value (`normal`,
   `configuration`, `no_op`) THEN the system SHALL round-trip it without loss.
6. WHEN `install_snapshot_request::data()` contains arbitrary binary bytes (including
   `0x00` bytes and non-UTF-8 sequences) THEN the system SHALL round-trip it exactly via
   a protobuf `bytes` field, unlike `json_rpc_serializer`'s base64-in-JSON-string
   workaround.

### Requirement 7

**User Story:** As a system operator, I want `protobuf_rpc_serializer` to fail fast and
clearly at compile time if used with template parameter combinations it does not support,
so that a misconfiguration is caught during development rather than producing confusing
runtime errors.

#### Acceptance Criteria

1. WHEN `protobuf_rpc_serializer<Data>` is instantiated with a `Data` that does not
   satisfy `serialized_data` THEN the system SHALL fail to compile via the class's
   `requires` clause, mirroring `json_rpc_serializer`'s existing constraint.
2. WHEN a message struct is serialized/deserialized with `TermId`/`LogIndex` types other
   than an integral type satisfying `term_id`/`log_index` (`include/raft/types.hpp`) THEN
   the system SHALL fail to compile rather than silently truncate or misencode, matching
   how the underlying `kythira` struct templates already constrain these parameters.

### Requirement 8

**User Story:** As a performance-conscious operator, I want to measure whether Protocol
Buffers serialization is actually faster/smaller than the existing JSON serializer for
Raft's RPC message shapes, so that adopting it is justified by data rather than assumed.

#### Acceptance Criteria

1. WHEN `protobuf_rpc_serializer` is implemented THEN the system SHALL include a benchmark
   comparing serialized payload size and serialize/deserialize latency against
   `json_rpc_serializer` for representative `append_entries_request` payloads (varying
   `entries()` count and average command size).
2. WHEN the benchmark is run THEN the system SHALL record results in a
   `doc/protobuf_serializer_performance_comparison.md` companion document, following the
   existing convention of `doc/future_backend_performance_comparison.md`.

### Requirement 9

**User Story:** As a build engineer, I want Protocol Buffers to be an optional dependency
that degrades gracefully when unavailable, consistent with every other optional
dependency in this project (CoAP/libcoap, AWS SDK, stdexec, etc.), and independent of
whether `grpc-transport` is ever implemented.

#### Acceptance Criteria

1. WHEN Protobuf is not found on the build machine THEN the system SHALL skip building
   the `protobuf_rpc_serializer` target and continue configuring the rest of the project
   successfully, mirroring `LIBCOAP_FOUND`'s graceful-degradation behavior.
2. WHEN `-DKYTHIRA_KCONFIG_STRICT=ON` is set and the `PROTOBUF_SERIALIZER` Kconfig symbol
   is selected but Protobuf is not found THEN the system SHALL fail configuration with a
   descriptive error, mirroring `kythira_kconfig_require`'s existing behavior for
   `COAP_TRANSPORT`.
3. WHEN Protobuf is found THEN the system SHALL generate `raft_messages.pb.{h,cc}` from
   `raft_messages.proto` as part of the build, using `protoc` located via the detected
   Protobuf installation, independent of whether `gRPC`/`GRPC_TRANSPORT` is present.
4. WHEN the `protobuf_rpc_serializer` target is built THEN the system SHALL link only
   against the Protobuf runtime (`protobuf::libprotobuf`) — no gRPC library dependency.
5. WHEN Protobuf is added as a project dependency THEN the system SHALL declare it in
   `vcpkg.json` and add a corresponding `PROTOBUF_SERIALIZER` Kconfig symbol, following
   the same `depends on`/gating pattern as `COAP_TRANSPORT`.

### Requirement 10

**User Story:** As a testing engineer, I want `protobuf_rpc_serializer` tested to the same
rigor as `json_rpc_serializer`, so that I can trust it in production the same way the
existing JSON path is trusted.

#### Acceptance Criteria

1. WHEN unit tests are executed THEN the system SHALL verify
   `protobuf_rpc_serializer<std::vector<std::byte>>` satisfies `rpc_serializer` via
   `static_assert`, mirroring `tests/rpc_serializer_concept_test.cpp`.
2. WHEN property-based tests are executed THEN the system SHALL verify round-trip fidelity
   for every message type and every edge case in Requirement 6, mirroring
   `tests/rpc_serialization_property_test.cpp`'s per-message-type property structure,
   with a minimum of 100 iterations per property.
3. WHEN property-based tests are executed THEN the system SHALL verify malformed/wrong-
   type rejection per Requirement 5, mirroring
   `tests/rpc_malformed_message_property_test.cpp`.
4. WHEN integration tests are executed THEN the system SHALL verify that
   `cpp_httplib_client`/`cpp_httplib_server` (or another existing transport) configured
   with `protobuf_rpc_serializer` as `Types::serializer_type` successfully completes a
   full RequestVote/AppendEntries/InstallSnapshot exchange end-to-end.
