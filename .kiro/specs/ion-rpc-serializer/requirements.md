# Requirements Document

## Introduction

This document specifies the requirements for implementing an Amazon Ion-based RPC
serializer for the Raft consensus algorithm. The Ion serializer provides a second
concrete implementation of the `rpc_serializer` concept (`include/raft/types.hpp`) —
today satisfied only by `json_rpc_serializer` (`include/raft/json_serializer.hpp`) —
converting the same set of Raft RPC request/response structs to and from an opaque
`serialized_data` byte buffer, but using Amazon Ion instead of JSON as the wire format.
Because the HTTP and CoAP transports (and the in-process network simulator) are already
parameterized by `Types::serializer_type` and only ever move the resulting bytes, the
Ion serializer is a drop-in alternative wire format requiring no changes to transport
logic itself — only, where noted below, small format-detection additions so those
transports advertise the correct content type. Ion's binary encoding and native typed
values (in particular a native `blob` type for byte strings) let this serializer avoid
`json_rpc_serializer`'s base64 encoding of `command`/`data` payloads and its purely
textual, weakly-typed number/string representation, giving a smaller, faster-to-parse,
and more strictly-typed wire format for deployments that value that trade-off over JSON's
human readability.

## Glossary

- **ion_rpc_serializer**: The type being specified — a concrete implementation of the
  `rpc_serializer<S, Data>` concept that serializes/deserializes every Raft RPC message
  type using Amazon Ion.
- **Amazon Ion / Ion**: A richly-typed, self-describing, hierarchical data serialization
  format with interchangeable text and binary encodings; a superset of JSON at the text
  level. Reference implementation: `amazon-ion/ion-c` (C).
- **ion-c**: The C reference implementation of Amazon Ion (`ionc/ion.h`), providing
  `ion_reader_*`/`ion_writer_*` C APIs used to implement `ion_rpc_serializer`.
- **Binary_Ion**: Ion's compact binary encoding, self-identified by a 4-byte binary
  version marker (`0xE0 0x01 0x00 0xEA`) at the start of the stream.
- **Text_Ion**: Ion's human-readable text encoding, a syntactic superset of JSON.
- **Annotation**: An Ion value can carry one or more `symbol`-typed annotations
  (`annotation::value`) that tag its semantic type without adding a field to the value
  itself; used here to carry the RPC message type instead of a `"type"` string field.
- **blob**: Ion's native binary-data type (analogous to a byte string), used to encode
  `command`/`data` fields directly rather than base64-encoding them into a JSON string.
- **struct** (Ion): Ion's native associative-container type, analogous to a JSON object;
  used here as the top-level container for every serialized message.
- **serialized_data**: The concept (`include/raft/types.hpp`) any serializer's `Data`
  type parameter must satisfy — a `std::ranges::range` whose value type is `std::byte`.
- **rpc_serializer**: The concept (`include/raft/types.hpp`) `ion_rpc_serializer` must
  satisfy to be usable as `Types::serializer_type`.
- **serialization_exception**: The exception type (`include/raft/exceptions.hpp`) thrown
  by `json_rpc_serializer` on malformed input; `ion_rpc_serializer` must throw the same
  type for the same class of failures to remain a behaviorally compatible substitute.
- **Content-Format** (CoAP): The numeric option (RFC 7252 §12.3) identifying a CoAP
  message body's media type; `coap_utils::get_content_format_for_serializer`
  (`include/raft/coap_utils.hpp`) maps a serializer's `name()` to one of these values.
- **transport_types**: The concept (`include/raft/types.hpp`) requiring a
  `serializer_type` member satisfying `rpc_serializer`, used by the HTTP and CoAP
  transports and the network simulator.

## Requirements

### Requirement 1

**User Story:** As a Raft node operator, I want an Amazon Ion implementation of the
`rpc_serializer` concept, so that I can choose Ion instead of JSON as the wire format for
Raft RPCs without changing any transport or node code.

#### Acceptance Criteria

1. WHEN `ion_rpc_serializer<Data>` is instantiated with `Data = std::vector<std::byte>`
   THEN the system SHALL satisfy `kythira::rpc_serializer<ion_rpc_serializer<Data>, Data>`.
2. WHEN `ion_rpc_serializer<Data>` is used as `Types::serializer_type` for
   `default_raft_types`-style type bundles, `transport_types`, or `raft_types` THEN the
   system SHALL satisfy every concept requirement those bundles place on
   `serializer_type` with no other changes to the bundle.
3. WHEN `ion_rpc_serializer<Data>` is constructed THEN the system SHALL require only that
   `Data` satisfy `serialized_data` (mirroring `json_rpc_serializer`'s constraint),
   without introducing additional template requirements that would make it a non-drop-in
   replacement.
4. WHEN `ion_rpc_serializer<Data>::name()` is called THEN the system SHALL return a
   string identifying the serializer as Ion-based and distinguishing binary from text
   encoding (e.g. `"ion-binary"`/`"ion-text"`), mirroring
   `json_rpc_serializer::name()`'s use for content-format/content-type detection.

### Requirement 2

**User Story:** As a Raft node, I want every RPC message type serialized and
deserialized through Ion, so that Ion is a complete substitute for JSON rather than a
partial one.

#### Acceptance Criteria

1. WHEN `ion_rpc_serializer` is implemented THEN the system SHALL provide `serialize`
   overloads for `request_vote_request<>`, `request_vote_response<>`,
   `request_pre_vote_request<>`, `request_pre_vote_response<>`,
   `append_entries_request<>`, `append_entries_response<>`,
   `install_snapshot_request<>`, `install_snapshot_response<>`,
   `cluster_join_request<>`, `cluster_join_response<>`, `cluster_leave_request<>`,
   `cluster_leave_response<>`, `fetch_log_entries_request<>`, and
   `fetch_log_entries_response<>` — the same fourteen types `json_rpc_serializer`
   supports.
2. WHEN `ion_rpc_serializer` is implemented THEN the system SHALL provide a matching
   `deserialize_<message_name>` method for each type in Acceptance Criterion 2.1, with
   the same method names and template parameters (`NodeId`, `TermId`, `LogIndex`,
   `LogEntry`, `Address`) as `json_rpc_serializer`, so call sites can switch serializers
   by changing only the type.
3. WHEN `ion_rpc_serializer::deserialize<T>(data)` is called with any `T` from
   Acceptance Criterion 2.1's default-templated forms THEN the system SHALL dispatch to
   the corresponding `deserialize_<message_name>` method, mirroring
   `json_rpc_serializer::deserialize<T>`'s `if constexpr` dispatch table.
4. WHEN `append_entries_request<>`/`fetch_log_entries_response<>` are serialized THEN
   the system SHALL preserve every `log_entry<>` in `entries()`, including `term()`,
   `index()`, `command()`, and `type()` (`entry_type::normal`/`configuration`/`no_op`),
   with an empty `entries()` round-tripping to an empty (not omitted or null) Ion list.
5. WHEN `append_entries_response<>`/`install_snapshot_request<>` fields that are
   `std::optional` (`conflict_index`, `conflict_term`) are absent THEN the system SHALL
   omit the corresponding Ion struct field on serialize and SHALL deserialize its absence
   back to `std::nullopt`, never to a sentinel value.
6. WHEN `cluster_join_response<>`/`cluster_leave_response<>` are serialized with no
   `redirect` THEN the system SHALL omit the redirect field, and WHEN a `redirect` is
   present THEN the system SHALL serialize `peer_info<NodeId, Address>`'s `node_id` and
   `address` losslessly as a nested Ion struct.
7. WHEN `NodeId`/`Address` template parameters are `std::string` rather than the default
   `std::uint64_t` THEN the system SHALL serialize them as Ion `symbol` or `string`
   values (not `int`) and deserialize them back to the same `std::string`, mirroring
   `json_rpc_serializer`'s `if constexpr (std::same_as<NodeId, std::string>)` handling.

### Requirement 3

**User Story:** As a systems architect, I want the Ion wire format to use Ion's native
type system rather than re-deriving JSON's conventions inside Ion, so that the serializer
gets Ion's actual benefits (compactness, native binary data, strict typing) instead of
being JSON-with-different-punctuation.

#### Acceptance Criteria

1. WHEN a message is serialized THEN the system SHALL identify its RPC message type
   using a single Ion **annotation** on the top-level `struct` value (e.g.
   `request_vote_request::{term:1,...}`) rather than a `"type"` string field inside the
   struct, since Ion provides annotations as the idiomatic mechanism for tagging a
   value's semantic type without JSON's field-based workaround.
2. WHEN a `command` (`log_entry`) or `data` (`install_snapshot_request`) byte field is
   serialized THEN the system SHALL encode it as a native Ion `blob` value, and WHEN
   deserialized THEN the system SHALL decode it directly from that `blob` — the system
   SHALL NOT base64-encode binary payloads into an Ion `string`, since Ion natively
   represents binary data without a text-safe encoding detour.
3. WHEN integer fields (`term`, indices, node/leader IDs when numeric) are serialized
   THEN the system SHALL use Ion's native arbitrary-precision `int` type, and WHEN
   deserialized THEN the system SHALL reject a value that does not fit in the target
   integer type (e.g. `std::uint64_t`) rather than silently truncating it.
4. WHEN boolean fields (`vote_granted`, `success`, `done`, `available`, `accepted`) are
   serialized THEN the system SHALL use Ion's native `bool` type rather than encoding
   them as strings or integers.

### Requirement 4

**User Story:** As a system operator, I want to choose between Ion's binary and text
encodings, so that I can use the compact binary form for production traffic and the
human-readable text form for debugging and manual inspection.

#### Acceptance Criteria

1. WHEN `ion_rpc_serializer` is constructed THEN the system SHALL accept an encoding
   selection (binary or text) with binary as the default, since production RPC traffic
   should default to the more compact, faster-to-parse form.
2. WHEN binary encoding is selected THEN the system SHALL produce output beginning with
   Ion's binary version marker (`0xE0 0x01 0x00 0xEA`).
3. WHEN text encoding is selected THEN the system SHALL produce human-readable Ion text
   syntax that a developer can read without a decoder, suitable for logging or manual
   `ion-c`/`ion` CLI inspection.
4. WHEN deserializing THEN the system SHALL accept either encoding regardless of which
   encoding the instance was constructed to *write*, since Ion's binary version marker
   makes the two encodings unambiguously distinguishable on read and a receiver should
   not have to know the sender's write-side configuration to parse its input.
5. WHEN `name()` is called on a binary-configured vs. text-configured instance THEN the
   system SHALL return a distinguishable value for each, per Requirement 1.4.

### Requirement 5

**User Story:** As a reliability engineer, I want malformed or adversarial input to be
rejected safely, so that a corrupted or malicious peer cannot crash a node or trigger
undefined behavior via the Ion deserializer.

#### Acceptance Criteria

1. WHEN `deserialize_<message_name>` is called with a byte sequence that is not valid
   Ion (neither valid binary nor valid text) THEN the system SHALL throw
   `kythira::serialization_exception` rather than crash, hang, or return a
   partially-populated struct.
2. WHEN `deserialize_<message_name>` is called with well-formed Ion whose top-level
   annotation does not match the expected message type THEN the system SHALL throw
   `kythira::serialization_exception`, mirroring `json_rpc_serializer`'s `"type"`-field
   check.
3. WHEN `deserialize_<message_name>` is called with well-formed Ion that is missing a
   required field, has a field of the wrong Ion type (e.g. `string` where `int` is
   expected), or has a numeric value out of range for its target C++ type THEN the
   system SHALL throw `kythira::serialization_exception`.
4. WHEN `deserialize_<message_name>` is called with well-formed Ion whose top-level value
   is not a `struct` (e.g. a bare `int`, a `list`, `null`) THEN the system SHALL throw
   `kythira::serialization_exception`.
5. WHEN an `ion-c` C API call fails with a non-success `iERR` code during serialize or
   deserialize THEN the system SHALL translate that failure into
   `kythira::serialization_exception` carrying a human-readable message, and SHALL NOT
   propagate raw `ion-c` error codes, dangling readers/writers, or leaked resources to
   the caller.
6. WHEN a caller passes malformed input to any `deserialize_<message_name>` method THEN
   the system SHALL leave no partially-open `ion-c` reader handle or other resource leak,
   regardless of which validation step rejected the input.

### Requirement 6

**User Story:** As a distributed systems developer, I want `ion_rpc_serializer` to be
usable as `Types::serializer_type` for the existing HTTP and CoAP transports with correct
content-type/content-format advertisement, so that switching serializers is a one-line
type change rather than a transport-layer patch.

#### Acceptance Criteria

1. WHEN `ion_rpc_serializer<std::vector<std::byte>>` is substituted for
   `json_rpc_serializer<std::vector<std::byte>>` as a `transport_types::serializer_type`
   THEN the system SHALL require no changes to `cpp_httplib_client`/`cpp_httplib_server`,
   `boost_beast_http_client`/`boost_beast_http_server`, `coap_client`/`coap_server`, or
   `simulator_network_client`/`simulator_network_server` beyond the type substitution
   itself.
2. WHEN the CoAP transport sends a request/response serialized by `ion_rpc_serializer`
   THEN the system SHALL set the CoAP Content-Format option to a value identifying Ion,
   by extending `coap_utils::get_content_format_for_serializer` and the
   `coap_content_format` enum, since no Content-Format value is currently defined for
   Ion in that mapping.
3. WHEN the CoAP Content-Format value for Ion is chosen THEN the system SHALL use a
   number from CoAP's experimental/private-use range (RFC 7252 §12.3, 65000-65535)
   because no Ion Content-Format number is registered in IANA's CoAP Content-Formats
   registry, and SHALL use a single value for both binary and text Ion encodings, since
   Ion's binary version marker makes the two encodings self-describing to a receiver
   without a separate Content-Format number per encoding.
4. WHEN the HTTP transport (`cpp_httplib_client`/`server`,
   `boost_beast_http_client`/`server`) sends a request/response THEN the system SHALL
   set the `Content-Type` header from the active serializer's `name()` rather than the
   hardcoded `"application/json"` literal it uses today, so that Ion-serialized bodies
   are correctly labeled `application/ion` instead of falsely advertised as JSON; this
   change SHALL NOT alter the header value sent when `json_rpc_serializer` remains the
   configured serializer.

### Requirement 7

**User Story:** As a build engineer, I want the Ion serializer to be an optional
dependency that degrades gracefully when unavailable, consistent with every other
optional dependency in this project (CoAP/libcoap, gRPC, AWS SDK, stdexec, EDHOC/lakers).

#### Acceptance Criteria

1. WHEN `ion-c` is not found on the build machine THEN the system SHALL skip building
   any target depending on `ion_serializer.hpp`/`ion-c` and SHALL continue configuring
   and building the rest of the project successfully, mirroring `LIBCOAP_FOUND`'s
   graceful-degradation behavior.
2. WHEN `-DKYTHIRA_KCONFIG_STRICT=ON` is set and the `ION_SERIALIZER` Kconfig symbol is
   selected but `ion-c` is not found THEN the system SHALL fail configuration with a
   descriptive error, mirroring `kythira_kconfig_require`'s existing behavior for
   `COAP_TRANSPORT`.
3. WHEN `ion-c` is added as a project dependency THEN the system SHALL provide it via a
   `vcpkg-overlays/ion-c` overlay port (following the pattern already established by
   `vcpkg-overlays/lakers` and `vcpkg-overlays/stdexec`) rather than assuming an
   official vcpkg registry port exists, and SHALL gate it behind an opt-in vcpkg feature
   (mirroring the existing `edhoc` feature) so the default build does not fetch or build
   it.
4. WHEN the `ION_SERIALIZER` Kconfig symbol is added THEN the system SHALL follow the
   same `depends on`/gating pattern as `COAP_TRANSPORT` and `GRPC_TRANSPORT` in the root
   `Kconfig` file.
5. WHEN `ion-c` is found and `ION_SERIALIZER` is enabled THEN the system SHALL link
   `include/raft/ion_serializer.hpp`-consuming targets against the `ion-c` library
   without requiring consumers to add their own `find_package`/link directives beyond
   what other optional-dependency headers in this project already require.

### Requirement 8

**User Story:** As a testing engineer, I want the Ion serializer to be tested to the same
standard as the JSON serializer, so that I can trust it as a production-ready alternative.

#### Acceptance Criteria

1. WHEN unit tests are executed THEN the system SHALL verify via `static_assert` that
   `ion_rpc_serializer<std::vector<std::byte>>` satisfies `rpc_serializer`, mirroring
   `tests/rpc_serializer_concept_test.cpp`'s coverage of `json_rpc_serializer`.
2. WHEN property-based tests are executed THEN the system SHALL verify round-trip
   fidelity (serialize then deserialize reproduces the original value) for every message
   type in Requirement 2.1, for both binary and text encodings, mirroring
   `tests/rpc_serialization_property_test.cpp`'s coverage and property-numbering
   convention, run a minimum of 100 iterations with randomly generated inputs.
3. WHEN property-based tests are executed THEN the system SHALL verify that malformed
   input (random bytes, wrong annotation, missing fields, wrong Ion type per field,
   out-of-range numeric values, non-struct top-level value) is rejected via
   `kythira::serialization_exception` for every message type that
   `tests/rpc_malformed_message_property_test.cpp` exercises for JSON, achieving at
   least the same rejection rate for random-byte-sequence inputs (>= 95%).
4. WHEN property-based tests are executed THEN the system SHALL verify that for any
   valid message value, `json_rpc_serializer` and `ion_rpc_serializer` deserialize a
   fresh copy of that same original value to field-for-field-equal results — i.e. the
   two serializers agree on the semantic content of any message even though their wire
   bytes differ.
5. WHEN unit tests are executed THEN the system SHALL verify that binary-encoded output
   never round-trips successfully through the text-only path and vice versa without
   auto-detection (Requirement 4.4) — i.e. that deserialize correctly identifies and
   accepts either encoding from a single code path, not two separate ones the caller
   must choose between.
6. WHEN unit tests are executed THEN the system SHALL verify that a `command`/`data`
   payload containing bytes with no valid JSON-string/base64-safe interpretation (e.g.
   arbitrary high-bit bytes) round-trips correctly, demonstrating the native-`blob`
   advantage over base64-in-JSON.
