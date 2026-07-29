# Design Document

## Overview

`protobuf_rpc_serializer<Data>` is a second implementation of the `rpc_serializer<S,
Data>` concept (`include/raft/types.hpp`), alongside the existing
`json_rpc_serializer<Data>` (`include/raft/json_serializer.hpp`). It converts every
`kythira` Raft RPC request/response struct to and from a `Data` (typically
`std::vector<std::byte>`) payload, using generated Protocol Buffers message classes for
the actual encode/decode work. Because `cpp_httplib_client`/`cpp_httplib_server`,
`coap_client`/`coap_server`, and `tcp_rpc`/`tls_tcp_rpc` are all parameterized by
`Types::serializer_type` and only ever call `.serialize(msg)` / `.deserialize_*(data)` /
`.name()` on it, `protobuf_rpc_serializer` is a drop-in replacement: no transport code
changes, only the `serializer_type` alias in a `Types` bundle changes.

### Relationship to `grpc-transport`

`.kiro/specs/grpc-transport/` is a separate, not-yet-implemented spec that uses Protocol
Buffers as gRPC's own wire format and RPC framing, replacing `rpc_serializer` entirely
(its design doc's "Protobuf replaces `rpc_serializer`" section explains why:
`grpc_transport_types` has no `serializer_type` at all). This spec is the opposite move:
it keeps the existing transport-agnostic `rpc_serializer` abstraction and gives it a
Protocol-Buffers-backed implementation, so that projects that want binary encoding but
not a full gRPC/HTTP2 stack switch (new dependency: `grpc`, new I/O model, new service
generation) can get most of the wire-efficiency benefit with a one-line `serializer_type`
change to their existing HTTP/CoAP/TCP transport.

The two specs are deliberately kept independent and non-conflicting:

- Different `.proto` files (`raft_messages.proto` here vs. `raft.proto` for
  grpc-transport) and different proto packages (`kythira.raft.serializer.v1` vs.
  `kythira.raft.v1`), so both can be enabled in the same build without symbol collision.
- Different Kconfig symbols (`PROTOBUF_SERIALIZER` vs. `GRPC_TRANSPORT`) and different
  vcpkg dependencies (`protobuf` vs. `grpc`, which pulls in `protobuf` transitively) —
  enabling one does not require the other.
- If both are eventually implemented, unifying them into a single shared `.proto` file
  (grpc-transport's `raft.proto` `import`-ing this spec's message definitions, or vice
  versa) is a reasonable future consolidation, but is explicitly out of scope here so
  this spec does not depend on grpc-transport ever landing.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                  cpp_httplib_client / coap_client / tcp_rpc       │
│         (Types::serializer_type = protobuf_rpc_serializer<Data>) │
└───────────────────────────────┬───────────────────────────────────┘
                                 │ .serialize(msg) / .deserialize_*(data)
                                 ▼
┌─────────────────────────────────────────────────────────────────┐
│              protobuf_rpc_serializer<Data>                       │
│  (include/raft/protobuf_serializer.hpp — hand-written)            │
│                                                                    │
│  serialize(msg)         : [tag byte][msg.SerializeAsString()]      │
│  deserialize_*(data)    : check tag, ParseFromArray remainder      │
│  to_proto/from_proto    : kythira struct <-> generated proto type  │
└───────────────────────────────┬───────────────────────────────────┘
                                 │ uses
                                 ▼
┌─────────────────────────────────────────────────────────────────┐
│        raft_messages.pb.h / raft_messages.pb.cc (generated)       │
│   RequestVoteRequest, AppendEntriesRequest, LogEntry, ... (14)     │
└─────────────────────────────────────────────────────────────────┘
```

### Key design decision: a leading message-type tag byte

Protocol Buffers' binary wire format is not self-describing the way JSON is: a field is
identified on the wire only by a `(field_number, wire_type)` pair, and proto3 has no
required fields, so `ParseFromArray` on bytes belonging to a *different* message type can
easily "succeed" — field numbers that coincidentally share a compatible wire type get
silently (mis)interpreted, producing a plausible-looking but wrong value rather than a
parse error. `json_rpc_serializer` avoids the equivalent problem with a `"type"` string
field it checks before trusting the rest of the object
(`json_serializer.hpp`'s `if (obj["type"].as_string() != "...")` checks). This design
reproduces that safety property for the binary format with a one-byte discriminator
prepended to every payload, checked before any protobuf parsing is attempted:

```cpp
enum class message_tag : std::uint8_t {
    request_vote_request = 0,
    request_vote_response = 1,
    request_pre_vote_request = 2,
    request_pre_vote_response = 3,
    append_entries_request = 4,
    append_entries_response = 5,
    install_snapshot_request = 6,
    install_snapshot_response = 7,
    cluster_join_request = 8,
    cluster_join_response = 9,
    cluster_leave_request = 10,
    cluster_leave_response = 11,
    fetch_log_entries_request = 12,
    fetch_log_entries_response = 13,
};
```

`serialize(msg)` writes `[tag][protobuf bytes]`; every `deserialize_*` reads the tag
first and throws `serialization_exception` immediately on mismatch, exactly mirroring
`json_rpc_serializer`'s `"type"` check and satisfying the same malformed/wrong-type
rejection properties `tests/rpc_malformed_message_property_test.cpp` already exercises
for JSON (Requirement 5).

### Key design decision: `NodeIdValue` for generic `NodeId`

`kythira`'s RPC structs are templated over `NodeId` (default `std::uint64_t`, but
`tests/rpc_serialization_property_test.cpp` also exercises `std::string`). A protobuf
message field has one static type, so it cannot mirror a C++ template parameter directly
the way `json_rpc_serializer` mirrors it dynamically (`boost::json::value` can hold either
an int64 or a string under the same key at runtime). Instead, every field typed `NodeId`
in `raft_messages.proto` uses a small wrapper message:

```protobuf
message NodeIdValue {
  oneof value {
    uint64 numeric = 1;
    string text = 2;
  }
}
```

`to_proto`/`from_proto` select the populated case at compile time:

```cpp
template<typename NodeId>
auto to_node_id_value(NodeId id) -> NodeIdValue {
    NodeIdValue v;
    if constexpr (std::same_as<NodeId, std::string>) {
        v.set_text(id);
    } else {
        v.set_numeric(static_cast<std::uint64_t>(id));
    }
    return v;
}

template<typename NodeId>
auto from_node_id_value(const NodeIdValue& v) -> NodeId {
    if constexpr (std::same_as<NodeId, std::string>) {
        if (v.value_case() != NodeIdValue::kText) {
            throw serialization_exception("NodeIdValue: expected string, got numeric");
        }
        return v.text();
    } else {
        if (v.value_case() != NodeIdValue::kNumeric) {
            throw serialization_exception("NodeIdValue: expected numeric, got string");
        }
        return static_cast<NodeId>(v.numeric());
    }
}
```

This keeps every RPC struct's full template generality (Requirement 3) at the cost of one
extra nesting level versus a bare `uint64`/`string` field — an acceptable, explicit
trade-off given `NodeId = std::uint64_t` is this codebase's overwhelming default and the
`oneof` adds only a small, fixed per-field overhead.

### Data Flow — `serialize`

1. Caller calls `serializer.serialize(request_vote_request<> const&)` (overload
   resolution picks the right overload, exactly like `json_rpc_serializer::serialize`).
2. `to_proto(request)` builds a `RequestVoteRequest` protobuf message, converting
   `candidate_id` via `to_node_id_value`.
3. `proto_message.SerializeAsString()` produces the encoded bytes.
4. The one-byte tag for `request_vote_request` is prepended.
5. The combined bytes are copied into a `Data` instance and returned.

### Data Flow — `deserialize_request_vote_request`

1. Caller calls `serializer.deserialize_request_vote_request<NodeId, TermId,
   LogIndex>(data)`.
2. If `data.size() < 1`, throw `serialization_exception` ("payload too short").
3. Read the first byte; if it does not equal `message_tag::request_vote_request`, throw
   `serialization_exception` ("wrong message type").
4. `RequestVoteRequest proto_msg; if (!proto_msg.ParseFromArray(data.data() + 1,
   data.size() - 1)) throw serialization_exception("protobuf parse failed");`
5. `from_proto<NodeId, TermId, LogIndex>(proto_msg)` builds and returns the populated
   `request_vote_request<NodeId, TermId, LogIndex>`, converting `candidate_id` via
   `from_node_id_value<NodeId>`.

## Components and Interfaces

### `raft_messages.proto`

```protobuf
syntax = "proto3";

package kythira.raft.serializer.v1;

message NodeIdValue {
  oneof value {
    uint64 numeric = 1;
    string text = 2;
  }
}

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
  NodeIdValue candidate_id = 2;
  uint64 last_log_index = 3;
  uint64 last_log_term = 4;
}

message RequestVoteResponse {
  uint64 term = 1;
  bool vote_granted = 2;
}

message RequestPreVoteRequest {
  uint64 term = 1;
  NodeIdValue candidate_id = 2;
  uint64 last_log_index = 3;
  uint64 last_log_term = 4;
}

message RequestPreVoteResponse {
  uint64 term = 1;
  bool vote_granted = 2;
}

message AppendEntriesRequest {
  uint64 term = 1;
  NodeIdValue leader_id = 2;
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
  NodeIdValue leader_id = 2;
  uint64 last_included_index = 3;
  uint64 last_included_term = 4;
  uint64 offset = 5;
  bytes data = 6;
  bool done = 7;
}

message InstallSnapshotResponse {
  uint64 term = 1;
}

message PeerInfo {
  NodeIdValue node_id = 1;
  string address = 2;
}

message ClusterJoinRequest {
  NodeIdValue node_id = 1;
  string contact_address = 2;
}

message ClusterJoinResponse {
  bool accepted = 1;
  optional PeerInfo redirect = 2;
}

message ClusterLeaveRequest {
  NodeIdValue node_id = 1;
}

message ClusterLeaveResponse {
  bool accepted = 1;
  optional PeerInfo redirect = 2;
}

message FetchLogEntriesRequest {
  NodeIdValue requester_id = 1;
  uint64 from_index = 2;
  uint64 to_index = 3;
}

message FetchLogEntriesResponse {
  uint64 responder_id = 1;
  bool available = 2;
  uint64 prev_log_term = 3;
  repeated LogEntry entries = 4;
}
```

Note `FetchLogEntriesResponse::responder_id` stays a bare `uint64` (matching
`kythira::fetch_log_entries_response::_responder_id`, which is `std::uint64_t` regardless
of the struct's `NodeId` template parameter — there is no generic `NodeId` field to wrap
here, mirroring `json_rpc_serializer::deserialize_fetch_log_entries_response`'s identical
`static_cast<std::uint64_t>` treatment).

### `protobuf_rpc_serializer<Data>`

```cpp
namespace kythira {

template<typename Data>
requires std::ranges::range<Data> && std::same_as<std::ranges::range_value_t<Data>, std::byte>
class protobuf_rpc_serializer {
public:
    // One serialize() overload per message type, mirroring json_rpc_serializer:
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t>
    [[nodiscard]] auto serialize(const request_vote_request<NodeId, TermId, LogIndex>&) const -> Data;
    template<typename TermId = std::uint64_t>
    [[nodiscard]] auto serialize(const request_vote_response<TermId>&) const -> Data;
    // ... one overload per remaining message type (12 more), same pattern as
    // json_rpc_serializer.hpp lines 18-144 and 347-530.

    // One named deserialize_* per message type, mirroring json_rpc_serializer:
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t>
    [[nodiscard]] auto deserialize_request_vote_request(const Data&) const
        -> request_vote_request<NodeId, TermId, LogIndex>;
    // ... one method per remaining message type (13 more).

    // Generic dispatcher, mirroring json_rpc_serializer::deserialize<T>:
    template<typename T> [[nodiscard]] auto deserialize(const Data&) const -> T;

    [[nodiscard]] auto name() const -> std::string { return "protobuf"; }

private:
    enum class message_tag : std::uint8_t { /* 14 values, see Design Decision above */ };

    template<typename ProtoMessage>
    [[nodiscard]] auto encode(message_tag tag, const ProtoMessage& msg) const -> Data;

    template<typename ProtoMessage>
    [[nodiscard]] auto decode(message_tag expected_tag, const Data& data) const -> ProtoMessage;

    // to_proto/from_proto free-function-style private helpers, one pair per message type
    // plus to_node_id_value/from_node_id_value (see Design Decisions above).
};

static_assert(rpc_serializer<protobuf_rpc_serializer<std::vector<std::byte>>,
                              std::vector<std::byte>>,
              "protobuf_rpc_serializer must satisfy the rpc_serializer concept");

using protobuf_serializer = protobuf_rpc_serializer<std::vector<std::byte>>;

}  // namespace kythira
```

`encode`/`decode` are the private helpers every public `serialize`/`deserialize_*`
funnels through — analogous to `json_rpc_serializer`'s repeated `json_to_bytes`/
`bytes_to_string` pattern, but centralizing the tag-prepend/tag-check logic in one place
so it cannot be forgotten in a new message type added later.

## Data Models

### Header/Source File Structure

```
proto/
└── raft_messages.proto                  # canonical wire schema (Requirement 2)
include/raft/
└── protobuf_serializer.hpp              # protobuf_rpc_serializer<Data> (hand-written)
```

Unlike `json_rpc_serializer` (header-only, depends only on `boost::json` headers),
`protobuf_rpc_serializer` depends on generated `raft_messages.pb.cc`, which must be
compiled into a library target — see Build Integration below.

## Error Handling

`protobuf_rpc_serializer` reuses the existing `kythira::serialization_exception`
(`include/raft/exceptions.hpp`) rather than introducing a parallel exception hierarchy,
consistent with `json_rpc_serializer`'s error handling and with Requirement 5. Every
rejection path throws it with a descriptive message:

| Condition                                                        | Message (example)                         |
|--------------------------------------------------------------------|--------------------------------------------|
| Payload shorter than the 1-byte tag                                | `"protobuf payload too short for tag"`      |
| Tag byte does not match the expected message type                  | `"wrong message type for <expected>"`       |
| `ParseFromArray` returns `false`                                    | `"malformed protobuf payload for <type>"`   |
| `NodeIdValue` populated case does not match requested `NodeId` type | `"NodeIdValue: expected <X>, got <Y>"`      |
| `PeerInfo`/optional field access before checking presence          | *(not reachable — always presence-checked)* |

No new exception types are introduced (Requirement 5 only requires distinguishing
malformed input from success, which `serialization_exception` already does uniformly
across both serializers).

## Testing Strategy

### Unit Tests (`tests/protobuf_rpc_serializer_concept_test.cpp`)

Mirrors `tests/rpc_serializer_concept_test.cpp`:
1. `static_assert(kythira::rpc_serializer<protobuf_rpc_serializer<std::vector<std::byte>>,
   std::vector<std::byte>>)`.
2. Basic instantiate/serialize/deserialize smoke test for `request_vote_request<>`.
3. `name()` returns `"protobuf"`.

### Property-Based Tests (`tests/protobuf_rpc_serialization_property_test.cpp`)

Mirrors `tests/rpc_serialization_property_test.cpp`'s structure — one property per
message type plus the `std::string` `NodeId` variant — each tagged `**Feature:
protobuf-rpc-serializer, Property {number}: {property_text}**` and run a minimum of 100
iterations with randomly generated field values.

### Malformed-Message Tests (`tests/protobuf_rpc_malformed_message_property_test.cpp`)

Mirrors `tests/rpc_malformed_message_property_test.cpp`:
1. Random byte sequences fed to every `deserialize_*` method are rejected ≥95% of the
   time.
2. A validly-tagged-and-encoded message of type `X` fed to `deserialize_*` for type `Y ≠
   X` is rejected 100% of the time (this is the property the tag-byte design exists to
   guarantee, and is strictly stronger than what raw `ParseFromArray` alone would provide
   — see Design Decision above).
3. Truncated payloads (0 bytes, 1 byte, tag-only) are rejected without out-of-bounds
   access (run under ASan/UBSan in CI, consistent with this project's existing sanitizer
   configuration).

### Integration Test

An existing transport (`cpp_httplib_client`/`cpp_httplib_server` is the simplest,
following `tests/http_client_test.cpp`'s pattern) instantiated with
`Types::serializer_type = protobuf_rpc_serializer<std::vector<std::byte>>` completes a
full RequestVote/AppendEntries/InstallSnapshot exchange end-to-end, proving the drop-in
replacement claim (Requirement 1.2) rather than only unit-testing the serializer in
isolation.

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid
executions of a system — a formal statement about what the system should do.*

### Property 1: Round-trip fidelity for every message type

*For any* valid `kythira` request or response value of any of the fourteen message types
in scope, `deserialize_X(serialize(value))` produces a value equal to `value` across
every field, including empty `entries()`, absent `conflict_index`/`conflict_term`, and
absent redirect hints.
**Validates: Requirements 2.1, 2.2, 2.3, 2.4, 6.1, 6.2, 6.3, 6.4, 6.5, 6.6**

### Property 2: Generic `NodeId` round-trips for both supported instantiations

*For any* RPC struct with a `NodeId`-typed field, instantiating with either
`std::uint64_t` or `std::string` and round-tripping through `serialize`/`deserialize_*`
reproduces the original value exactly.
**Validates: Requirements 3.1, 3.2, 3.3, 3.4**

### Property 3: Wrong-tag payloads are always rejected

*For any* validly-serialized payload of message type `X` and any other message type `Y ≠
X` in scope, calling the `deserialize_*` method for `Y` on that payload always throws
`serialization_exception`, never returning a value.
**Validates: Requirements 5.1, 5.2, 5.6**

### Property 4: Malformed/random payloads are rejected at a high rate

*For any* randomly generated byte sequence, at least 95% are rejected by every
`deserialize_*` method with an exception rather than producing a value.
**Validates: Requirements 5.3, 5.4, 5.5**

### Property 5: `protobuf_rpc_serializer` is a transparent drop-in for `json_rpc_serializer`

*For any* transport (`cpp_httplib_client`/`server` at minimum) whose `Types::serializer_type`
is switched from `json_rpc_serializer<Data>` to `protobuf_rpc_serializer<Data>` with no
other code change, a full RequestVote/AppendEntries/InstallSnapshot exchange still
completes successfully end-to-end.
**Validates: Requirements 1.1, 1.2, 4.1, 4.2, 4.3, 4.4, 4.5, 10.4**

### Property-Based Testing Configuration

Each property-based test should:
- Run a minimum of 100 iterations with randomly generated inputs.
- Be tagged `**Feature: protobuf-rpc-serializer, Property {number}: {property_text}**`.
- Use the same Boost.Test-based property-testing approach as
  `tests/rpc_serialization_property_test.cpp` and
  `tests/rpc_malformed_message_property_test.cpp`.

## Implementation Notes

### Why not reuse `json_rpc_serializer`'s base64 helpers

`json_rpc_serializer` base64-encodes `std::vector<std::byte>` fields (`command`, snapshot
`data`) because JSON has no native binary type. Protocol Buffers' `bytes` field type
carries arbitrary binary data natively and more compactly, so `protobuf_rpc_serializer`
passes `command()`/`data()` straight through via `std::string(reinterpret_cast<const
char*>(bytes.data()), bytes.size())` (protobuf's `bytes` setters accept
`std::string`/`absl::string_view` over arbitrary octets, not just valid UTF-8) — no
base64 encode/decode step is needed or beneficial here (Requirement 6.6).

### `SerializeAsString`/`ParseFromArray` vs. `SerializeToArray`

`SerializeAsString()` is used for encoding for simplicity (one allocation, no need to
pre-compute `ByteSizeLong()`); `ParseFromArray(ptr, len)` is used for decoding to avoid an
extra copy of the tag-stripped payload into a `std::string` first. Both are part of
protobuf's stable public API and require no `arena` allocation setup for these
message sizes.

### Thread Safety

`protobuf_rpc_serializer` is stateless (no mutable members) — a single instance is safe
to share across threads and call concurrently, exactly like `json_rpc_serializer`. Each
`serialize`/`deserialize_*` call constructs its own local protobuf message object.

### Executor Discipline

`protobuf_rpc_serializer` performs no I/O and no async work — it is a pure, synchronous
encode/decode step invoked inline by the calling transport's own executor discipline
(unchanged from how `json_rpc_serializer` is invoked today).

## Dependencies

### External Libraries

- **Protocol Buffers** (`protobuf`): IDL compiler (`protoc`) and C++ runtime
  (`protobuf::libprotobuf`). vcpkg package `protobuf`; license BSD-3-Clause. No `grpc`
  package dependency.

### Internal Dependencies

- **raft/types.hpp**: `kythira` RPC message structs, `rpc_serializer`/`serialized_data`
  concepts.
- **raft/exceptions.hpp**: `serialization_exception`.

## Build Integration

### `vcpkg.json`

```jsonc
{
  "name": "protobuf",
  "version>=": "5.29.0"
}
```

Added to the top-level `dependencies` array alongside `libcoap`, following the same
always-declared-but-gracefully-optional pattern `COAP_TRANSPORT` uses.

### `Kconfig`

```kconfig
menu "Serialization"

config PROTOBUF_SERIALIZER
	bool "Protocol Buffers rpc_serializer implementation"
	default n
	help
	  find_package(Protobuf CONFIG). Backs PROTOBUF_FOUND. vcpkg package:
	  protobuf. Generates raft_messages.pb.{h,cc} from
	  proto/raft_messages.proto via protoc at build time. Independent of
	  GRPC_TRANSPORT (Transports menu) -- selecting this does not require
	  gRPC, and vice versa.

endmenu # Serialization
```

Placed in its own `menu "Serialization"` block (new) rather than the existing `menu
"Transports"`, since `protobuf_rpc_serializer` is a `serializer_type`, not a transport —
it is selected as a `Types::serializer_type` template argument alongside whichever
transport(s) are enabled, the same way `json_rpc_serializer` is available unconditionally
today.

### `CMakeLists.txt`

```cmake
kythira_kconfig_gate(PROTOBUF_SERIALIZER)
if(_KYTHIRA_GATE_PROTOBUF_SERIALIZER)
    find_package(Protobuf CONFIG QUIET)
endif()
kythira_kconfig_require(PROTOBUF_SERIALIZER "Protobuf_FOUND" "protobuf")

if(Protobuf_FOUND)
    set(RAFT_MESSAGES_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/proto/raft_messages.proto")
    set(PROTOBUF_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/raft_messages")
    file(MAKE_DIRECTORY "${PROTOBUF_GENERATED_DIR}")

    add_custom_command(
        OUTPUT "${PROTOBUF_GENERATED_DIR}/raft_messages.pb.cc"
               "${PROTOBUF_GENERATED_DIR}/raft_messages.pb.h"
        COMMAND protobuf::protoc
        ARGS --cpp_out="${PROTOBUF_GENERATED_DIR}"
             -I "${CMAKE_CURRENT_SOURCE_DIR}/proto" "${RAFT_MESSAGES_PROTO}"
        DEPENDS "${RAFT_MESSAGES_PROTO}" protobuf::protoc
    )

    add_library(raft_protobuf_serializer
        "${PROTOBUF_GENERATED_DIR}/raft_messages.pb.cc"
    )
    target_include_directories(raft_protobuf_serializer PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include "${PROTOBUF_GENERATED_DIR}"
    )
    target_link_libraries(raft_protobuf_serializer PUBLIC protobuf::libprotobuf)
    target_compile_features(raft_protobuf_serializer PUBLIC cxx_std_23)
else()
    message(WARNING "Protobuf not found. protobuf_rpc_serializer will not be available.")
endif()
```

Test targets that need `protobuf_rpc_serializer` link `raft_protobuf_serializer` and are
only added when `Protobuf_FOUND`, following the same conditional-`add_executable` pattern
CoAP-dependent tests already use for `LIBCOAP_FOUND`.

## Performance Considerations

- **Wire size**: protobuf's varint/tag-length-value binary encoding is expected to
  produce meaningfully smaller payloads than JSON for the same `kythira` struct,
  especially for `append_entries_request` with many `entries()` (no per-field string
  keys, no base64 expansion of `command`/`data` bytes — see Requirement 8 benchmark).
- **CPU cost**: protobuf's generated code typically serializes/parses faster than
  `boost::json`'s DOM-based approach, but this must be measured, not assumed (Requirement
  8) — the one-byte tag check and `NodeIdValue` `oneof` indirection add a small, bounded
  overhead per message that the benchmark should also make visible.
- Results belong in `doc/protobuf_serializer_performance_comparison.md`, following the
  existing `doc/future_backend_performance_comparison.md` convention (Requirement 8.2).

## Security Considerations

### Message-Type Confusion

The leading tag byte (Design Decision above) exists specifically to prevent message-type
confusion attacks where a peer sends bytes for one message type to an endpoint expecting
another, relying on protobuf's lenient wire-type-compatible parsing to produce a
plausible-but-wrong value rather than an error. This is a strictly stronger guarantee
than raw protobuf parsing provides on its own, and is the main non-obvious design
decision in this spec.

### Resource Exhaustion

`ParseFromArray` on attacker-controlled bytes can allocate memory proportional to
declared (but not yet verified) repeated-field/string/bytes lengths before failing; this
is an existing, unaddressed risk shared with `json_rpc_serializer` (unbounded JSON string/
array parsing) and with every other transport in this codebase, so it is not introduced
newly by this spec. Bounding maximum payload size remains the transport layer's
responsibility (e.g. `cpp_httplib_server_config`'s implicit body-size handling), not the
serializer's.

## Future Enhancements

### Schema unification with `grpc-transport`

If `.kiro/specs/grpc-transport/` is later implemented, consider whether
`raft_messages.proto` and `raft.proto` should be unified (one file, or one importing the
other's message definitions) rather than maintained as two independent schemas for
functionally identical message shapes. Deferred here since grpc-transport does not yet
exist and this spec must not depend on it (Requirement 1.3, 1.4).

### Arena Allocation

For very high-throughput deployments, protobuf's arena allocation API could reduce
per-message allocation overhead further; not adopted in the base implementation to keep
`protobuf_rpc_serializer` stateless and as simple as `json_rpc_serializer`.
