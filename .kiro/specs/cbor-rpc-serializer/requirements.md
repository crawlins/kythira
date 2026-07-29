# Requirements Document

## Introduction

This document specifies the requirements for `cbor_rpc_serializer`, a concrete
implementation of the `kythira::rpc_serializer` concept (`include/raft/types.hpp`) that
encodes every Raft RPC message using CBOR (Concise Binary Object Representation, RFC
8949) instead of JSON. `rpc_serializer` is the compile-time seam already used by
`Types::serializer_type` on `raft_types`/`tcp_raft_types` and by every transport built on
`transport_types` (`tcp_rpc`/`tls_tcp_rpc`, the cpp-httplib HTTP transport, and the CoAP
transport); today it is satisfied only by `json_rpc_serializer`
(`include/raft/json_serializer.hpp`). `cbor_rpc_serializer` is a drop-in alternative: it
converts the same `kythira` request/response structs to and from an opaque
`std::vector<std::byte>` payload, so no transport code changes to accommodate it — a node
adopts CBOR simply by naming `cbor_rpc_serializer<...>` as its `Types::serializer_type`.
Per `doc/TODO.md`'s "RPC Serializer Implementations" section, CBOR is expected to be the
lowest-effort binary alternative to implement first, since its structure maps directly
onto `json_rpc_serializer`'s existing object-per-message field layout, while dropping
JSON's text-parsing overhead and (for byte-carrying fields such as log-entry commands and
snapshot chunks) its base64 size inflation.

## Glossary

- **CBOR**: Concise Binary Object Representation (RFC 8949), a binary data-serialization
  format designed for small code size and small message size, structurally a superset of
  JSON's data model (maps, arrays, integers, strings, byte strings, booleans, null).
- **Major_Type**: The 3-bit type tag in a CBOR item's initial byte (RFC 8949 §3):
  0 = unsigned integer, 2 = byte string, 3 = text string, 4 = array, 5 = map, 7 = simple
  values/floats (including booleans).
- **Additional_Info**: The 5-bit value following a CBOR item's major type, encoding either
  a small immediate value or how many following bytes hold the item's length/value.
- **Definite-length encoding**: A CBOR array/map/string whose item count/byte length is
  declared up front in its header, as opposed to indefinite-length (streamed,
  break-terminated) encoding; this implementation uses only definite-length items.
- **cbor_rpc_serializer**: The type specified by this document; a template class
  parameterized on `Data` (the serialized-byte-container type) satisfying
  `kythira::rpc_serializer<cbor_rpc_serializer<Data>, Data>`.
- **rpc_serializer concept**: `kythira::rpc_serializer<S, Data>` (`include/raft/types.hpp`)
  — the minimal compile-time contract a serializer type must satisfy so it can be named
  as `Types::serializer_type`.
- **serialized_data concept**: `kythira::serialized_data<T>` — constrains `Data` to a
  range whose value type is `std::byte`.
- **json_rpc_serializer**: The existing default `rpc_serializer` implementation
  (`include/raft/json_serializer.hpp`), used as the reference for message shape, method
  surface, and error-handling behavior this implementation must mirror.
- **Message discriminant**: The `"type"` field `json_rpc_serializer` writes into every
  encoded message (e.g. `"request_vote_request"`) and validates on decode, letting a
  `deserialize_*` method reject a byte sequence that decodes cleanly but represents the
  wrong message type.
- **coap_utils::get_content_format_for_serializer**: An existing helper
  (`include/raft/coap_utils.hpp`) that inspects a serializer's `name()` string and already
  recognizes the substring `"cbor"` (case-insensitively) as CoAP Content-Format
  `application/cbor` — a seam that already anticipates this implementation.

## Requirements

### Requirement 1

**User Story:** As a Raft node operator, I want a CBOR implementation of the RPC
serializer, so that I can reduce wire size and remove text-parsing overhead relative to
JSON without changing which transport (`tcp_rpc`/`tls_tcp_rpc`, HTTP, CoAP) my cluster
uses.

#### Acceptance Criteria

1. WHEN `cbor_rpc_serializer<Data>` is instantiated with any `Data` satisfying
   `kythira::serialized_data<Data>` THEN the system SHALL satisfy
   `kythira::rpc_serializer<cbor_rpc_serializer<Data>, Data>`, verified by a
   `static_assert` mirroring `json_serializer.hpp`'s existing one.
2. WHEN a Raft node's `Types` bundle names `cbor_rpc_serializer<Data>` as
   `serializer_type` THEN the system SHALL require no changes to
   `tcp_rpc`/`tls_tcp_rpc`, the cpp-httplib HTTP transport, or the CoAP transport, since
   all three are already parameterized generically on `Types::serializer_type`.
3. WHEN `cbor_rpc_serializer<Data>` is provided as a convenience alias THEN the system
   SHALL define `using cbor_serializer = cbor_rpc_serializer<std::vector<std::byte>>;`
   mirroring `json_serializer`'s existing alias.

### Requirement 2

**User Story:** As a Raft node, I want to serialize and deserialize every core RPC
message using CBOR, so that leader election and log replication work identically to the
JSON path.

#### Acceptance Criteria

1. WHEN `serialize` is called with a `request_vote_request<>` or
   `request_vote_response<>` THEN the system SHALL produce a CBOR-encoded `Data` value
   carrying `term`, `candidate_id`, `last_log_index`, `last_log_term` (request) or `term`,
   `vote_granted` (response).
2. WHEN `serialize` is called with a `request_pre_vote_request<>` or
   `request_pre_vote_response<>` THEN the system SHALL produce a CBOR-encoded `Data` value
   with the same field shape as Requirement 2.1, tagged with its own distinct message
   discriminant.
3. WHEN `serialize` is called with an `append_entries_request<>` THEN the system SHALL
   encode `term`, `leader_id`, `prev_log_index`, `prev_log_term`, `leader_commit`, and
   `entries()` as a CBOR array of maps, each carrying `term`, `index`, `command` (as a
   CBOR byte string, not base64 text), and `entry_type` (as an unsigned integer mirroring
   `kythira::entry_type`'s underlying value).
4. WHEN `serialize` is called with an `append_entries_response<>` THEN the system SHALL
   encode `term`, `success`, and, only when present, `conflict_index`/`conflict_term` —
   an absent `std::optional` SHALL be represented by omitting the corresponding map key,
   never by a CBOR null.
5. WHEN `serialize` is called with an `install_snapshot_request<>` THEN the system SHALL
   encode `term`, `leader_id`, `last_included_index`, `last_included_term`, `offset`,
   `data` (as a CBOR byte string, not base64 text), and `done`.
6. WHEN `serialize` is called with an `install_snapshot_response<>` THEN the system SHALL
   encode `term`.
7. WHEN `deserialize_request_vote_request`, `deserialize_request_vote_response`,
   `deserialize_request_pre_vote_request`, `deserialize_request_pre_vote_response`,
   `deserialize_append_entries_request`, `deserialize_append_entries_response`,
   `deserialize_install_snapshot_request`, or `deserialize_install_snapshot_response` is
   called on `Data` produced by the corresponding `serialize` overload THEN the system
   SHALL reconstruct a value equal in every field to the original.
8. WHEN the generic `template<typename T> deserialize<T>(const Data&)` method is called
   THEN the system SHALL dispatch to the correct `deserialize_*` method for every `T` that
   `json_rpc_serializer::deserialize` already dispatches for, mirroring its
   `if constexpr` chain.

### Requirement 3

**User Story:** As a cluster operator using the bootstrap and peer-to-peer catch-up
extensions, I want CBOR serialization of ClusterJoin, ClusterLeave, and FetchLogEntries
messages, so that these optional RPCs work over any transport using CBOR the same way
they do over JSON.

#### Acceptance Criteria

1. WHEN `serialize` is called with a `cluster_join_request<>` or `cluster_leave_request<>`
   THEN the system SHALL encode `node_id` and, for `cluster_join_request<>`,
   `contact_address`.
2. WHEN `serialize` is called with a `cluster_join_response<>` or
   `cluster_leave_response<>` THEN the system SHALL encode `accepted` and, only when
   `redirect` holds a value, a nested map carrying the redirecting `peer_info`'s
   `node_id`/`address` — an absent redirect SHALL be represented by omitting the
   corresponding map key, never by a CBOR null.
3. WHEN `serialize` is called with a `fetch_log_entries_request<>` THEN the system SHALL
   encode `requester_id`, `from_index`, `to_index`.
4. WHEN `serialize` is called with a `fetch_log_entries_response<>` THEN the system SHALL
   encode `responder_id`, `available`, `prev_log_term`, and `entries()` using the same
   per-entry map shape as Requirement 2.3.
5. WHEN the corresponding `deserialize_*` method is called on `Data` produced by any
   `serialize` overload in this requirement THEN the system SHALL reconstruct a value
   equal in every field to the original, including the empty-`entries()` and
   absent-`redirect` edge cases.

### Requirement 4

**User Story:** As a developer choosing `NodeId`/`Address` template parameters, I want the
CBOR serializer to support both integral and string identifiers, so that CBOR encoding
works for every `NodeId`/`Address` configuration `json_rpc_serializer` already supports.

#### Acceptance Criteria

1. WHEN `NodeId` is an unsigned integral type THEN the system SHALL encode
   `candidate_id`/`leader_id`/`requester_id`/`node_id` fields as CBOR unsigned integers
   (major type 0).
2. WHEN `NodeId` is `std::string` THEN the system SHALL encode the corresponding field as
   a CBOR text string (major type 3), selected via `if constexpr` exactly as
   `json_rpc_serializer` already branches on `std::same_as<NodeId, std::string>`.
3. WHEN `Address` is `std::string` THEN the system SHALL encode `contact_address` and any
   `peer_info::address` field as a CBOR text string.

### Requirement 5

**User Story:** As a developer relying on the message discriminant for defense-in-depth,
I want CBOR-encoded messages to carry the same kind of type tag `json_rpc_serializer`
writes, so that calling the wrong `deserialize_*` method on a validly-encoded message of a
different type fails loudly instead of silently misreading fields.

#### Acceptance Criteria

1. WHEN any `serialize` overload produces a `Data` value THEN the system SHALL include a
   message-discriminant map entry (e.g. a `"type"` key mapped to a CBOR text string, such
   as `"request_vote_request"`) using the same discriminant strings
   `json_rpc_serializer` already uses for the same message types.
2. WHEN a `deserialize_*` method decodes a `Data` value whose discriminant does not match
   the message type that method expects THEN the system SHALL throw
   `kythira::serialization_exception` rather than return a partially- or
   incorrectly-populated struct.

### Requirement 6

**User Story:** As a system operator, I want the CBOR serializer to reject malformed or
truncated input safely, so that a corrupted or adversarial payload cannot crash a Raft
node or read out-of-bounds memory.

#### Acceptance Criteria

1. WHEN any `deserialize_*` method is given a `Data` value that is not well-formed CBOR
   (truncated header, declared length exceeding the remaining buffer, unsupported major
   type/additional-info combination) THEN the system SHALL throw
   `kythira::serialization_exception` rather than read past the end of the buffer or
   invoke undefined behavior.
2. WHEN any `deserialize_*` method is given well-formed CBOR whose top-level item is not a
   map, or whose map is missing a required key, THEN the system SHALL throw
   `kythira::serialization_exception`.
3. WHEN decoding an integer field into a narrower target type (e.g. a CBOR unsigned
   integer into `TermId`/`LogIndex`) would lose information THEN the system SHALL either
   reject the value with `kythira::serialization_exception` or define the narrowing
   behavior explicitly — it SHALL NOT silently truncate without a documented rule.
4. WHEN the decoder encounters a CBOR construct it deliberately does not support
   (indefinite-length items, tags, floating-point numbers, negative integers, `null`/
   `undefined` outside a documented optional-field convention) THEN the system SHALL
   reject it with `kythira::serialization_exception` rather than attempt a best-effort
   partial interpretation.

### Requirement 7

**User Story:** As a developer integrating the CBOR serializer with the CoAP transport, I
want its `name()` method to be recognized by the existing Content-Format detection logic,
so that CoAP messages are correctly tagged `application/cbor` with no CoAP transport code
changes.

#### Acceptance Criteria

1. WHEN `name()` is called on `cbor_rpc_serializer<Data>` THEN the system SHALL return a
   string containing the substring `"cbor"` (e.g. `"cbor"`).
2. WHEN `coap_utils::get_content_format_for_serializer` (`include/raft/coap_utils.hpp`) is
   called with that string THEN the system SHALL resolve to
   `coap_content_format::application_cbor`, exercising the existing case-insensitive
   `"cbor"`/`"CBOR"` substring match already present in that function without requiring
   any change to it.

### Requirement 8

**User Story:** As a build engineer, I want the CBOR serializer to introduce no new
external dependency, so that adopting it carries none of the build-system risk a full
schema/codegen-based format (e.g. Protocol Buffers) would.

#### Acceptance Criteria

1. WHEN `cbor_rpc_serializer` is implemented THEN the system SHALL encode and decode CBOR
   using a self-contained implementation covering only the CBOR major types this
   project's messages need (unsigned integer, byte string, text string, array, map,
   boolean), requiring no addition to `vcpkg.json` or the root `Kconfig`.
2. WHEN `cbor_rpc_serializer` is added to the build THEN the system SHALL require only a
   new header (mirroring `include/raft/json_serializer.hpp`) and, if any, a corresponding
   `.cpp`/test target — no new `find_package` call or Kconfig gating symbol.

### Requirement 9

**User Story:** As a testing engineer, I want the CBOR serializer to be tested at the same
rigor as the JSON serializer, so that its correctness is established before it is chosen
in production.

#### Acceptance Criteria

1. WHEN unit tests are executed THEN the system SHALL verify, via `static_assert`, that
   `cbor_rpc_serializer<std::vector<std::byte>>` satisfies `kythira::rpc_serializer` and
   that `std::vector<std::byte>` satisfies `kythira::serialized_data`, mirroring
   `tests/rpc_serializer_concept_test.cpp`.
2. WHEN property-based tests are executed THEN the system SHALL verify round-trip
   correctness (`deserialize(serialize(x)) == x`, field-by-field) for every RPC message
   type in Requirements 2 and 3, across randomly generated inputs, including edge cases
   (empty `entries()`, absent `conflict_index`/`conflict_term`, absent `redirect`, both
   integral and string `NodeId`).
3. WHEN property-based tests are executed THEN the system SHALL verify that random and
   deliberately-truncated byte sequences are rejected with `kythira::serialization_exception`
   rather than causing a crash, mirroring `tests/rpc_malformed_message_property_test.cpp`.
4. WHEN property-based tests are executed THEN the system SHALL verify that calling a
   `deserialize_*` method on a validly-encoded message of a different type throws
   `kythira::serialization_exception` (Requirement 5.2).
5. Each property-based test SHALL run a minimum of 100 iterations with randomly generated
   inputs and be tagged `**Feature: cbor-rpc-serializer, Property {number}: {property_text}**`,
   consistent with this project's existing property-testing convention.

### Requirement 10

**User Story:** As a Raft node operator evaluating whether to switch from JSON to CBOR, I
want CBOR's wire-size advantage to be measurable, so that the choice is based on evidence
rather than assumed from the format alone.

#### Acceptance Criteria

1. WHEN a message containing at least one non-empty byte field (a log entry `command` or
   an `install_snapshot_request` `data` chunk) is encoded with both serializers THEN the
   system SHALL verify (in a test) that `cbor_rpc_serializer`'s output is no larger than
   `json_rpc_serializer`'s output for the same logical value, since CBOR byte strings
   avoid `json_rpc_serializer`'s base64 encoding of the same bytes.
2. WHEN the CBOR serializer is documented THEN the system SHALL note that this size
   comparison is a property test, not a specification of exact byte counts, since CBOR
   integer encoding width and JSON's decimal digit count both vary with field value.
