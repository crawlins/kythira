# Design Document

## Overview

`cbor_rpc_serializer<Data>` is a second concrete implementation of
`kythira::rpc_serializer<S, Data>` (`include/raft/types.hpp`), sitting alongside
`json_rpc_serializer<Data>` (`include/raft/json_serializer.hpp`) as an interchangeable
`Types::serializer_type`. It converts every `kythira` RPC request/response struct to and
from an opaque `Data` (`serialized_data`-constrained) byte buffer using CBOR (RFC 8949)
instead of JSON text. Because every transport built on `transport_types` — `tcp_rpc`/
`tls_tcp_rpc`, the cpp-httplib HTTP transport, and the CoAP transport — already treats
`Types::serializer_type` as an opaque byte-in/byte-out component, this is a pure addition:
no transport class changes. A node adopts CBOR by naming
`cbor_rpc_serializer<std::vector<std::byte>>` (or its `cbor_serializer` alias) as
`serializer_type` in its `Types` bundle, exactly as it would name `json_serializer` today.

This mirrors `json_rpc_serializer`'s public shape closely by design: one `serialize`
overload per message type, one `deserialize_<message>` method per message type, a generic
dispatching `deserialize<T>`, and a `name()` method — so the two implementations are
interchangeable at the call site and reviewable side-by-side.

### Key design decision: hand-rolled minimal codec, not a CBOR library

`json_rpc_serializer` already favors small, in-house helpers over general-purpose
libraries where the project's actual needs are narrow — it hand-rolls its own
base64 encode/decode rather than reaching for a base64 library. This implementation
follows the same philosophy: rather than adding a general-purpose CBOR library dependency
(e.g. `libcbor`, `tinycbor`, or a header-only C++ library with CBOR support), it
implements exactly the subset of RFC 8949 the 14 fixed Raft message shapes need —
definite-length unsigned integers, byte strings, text strings, arrays, maps, and
booleans. This is the concrete reason CBOR is `doc/TODO.md`'s "likely lowest-effort
binary alternative": unlike Protocol Buffers or gRPC, it needs no schema compiler, no
generated code, and (per this decision) no new external dependency at all — `vcpkg.json`
and the root `Kconfig` are untouched (Requirement 8).

The tradeoff is that this codec is deliberately not a general-purpose CBOR
implementation: it does not support indefinite-length items, tags, floating-point
numbers, negative integers, or arbitrary nesting beyond what these 14 message shapes
require. Anything outside that subset is a decode error (Requirement 6.4), not a silently
degraded best-effort parse. If a future need for general CBOR interop arises (e.g.
consuming CBOR produced by a non-`kythira` peer), that is a reason to revisit this
decision, not a gap in the current one.

### Key design decision: byte strings replace base64 text

`json_rpc_serializer` base64-encodes every raw-byte field (`log_entry::command()`,
`install_snapshot_request::data()`) because JSON has no native byte-string type — this
costs roughly 33% wire-size inflation on exactly the fields most likely to be large.
CBOR's major type 2 (byte string) carries raw bytes directly with only a short
length-prefixed header, so `cbor_rpc_serializer` writes `command`/`data` fields as CBOR
byte strings with no text transcoding step at all. This is the single largest and most
mechanical win CBOR offers over JSON for this project's message shapes, and is the basis
for the size-comparison property in Requirement 10.

### Key design decision: text-string map keys, not integer keys

CBOR maps may use any CBOR value as a key, including small unsigned integers, which would
shave a few more bytes per field than text-string keys matching `json_rpc_serializer`'s
field names (`"term"`, `"candidate_id"`, ...). This design keeps text-string keys
identical to the JSON field names for the first implementation: it keeps the encoder/
decoder a direct structural mirror of `json_rpc_serializer` (easing review and making a
hex-dumped CBOR payload human-legible without a key/number legend), at the cost of a few
bytes per field versus an integer-keyed scheme. An integer-keyed `cbor_rpc_serializer`
variant is noted as a Future Enhancement rather than built here, since it is a pure
size/readability tradeoff orthogonal to correctness.

### Key design decision: absent optional fields are omitted, never CBOR null

`append_entries_response::conflict_index()`/`conflict_term()` and
`cluster_join_response::redirect`/`cluster_leave_response::redirect` are
`std::optional`. `json_rpc_serializer` represents "absent" by omitting the JSON object
key entirely (`obj.contains("conflict_index")` gates both write and read). This design
keeps that convention for CBOR: an absent optional is an omitted map key, not a `null`/
CBOR-`undefined` entry, so the decoder never needs to special-case a present-but-null
key, and the encoder never emits the `null` simple value this codec otherwise does not
support (Requirement 6.4 rejects `null`/`undefined` precisely because this convention
means the implementation never needs to produce or accept it).

## Architecture

`cbor_rpc_serializer<Data>` has no client/server/lifecycle component of its own — it is a
stateless (or trivially-stateful) conversion layer invoked by whichever transport's
client/server classes hold a `_serializer` member:

```
┌───────────────────────────────────────────────────────────┐
│                        Raft Node                            │
│         (Types::serializer_type = cbor_rpc_serializer)       │
└──────────────────────────────┬──────────────────────────────┘
                                │
                                ▼
         ┌──────────────────────────────────────────┐
         │   tcp_rpc / tls_tcp_rpc / cpp_httplib_*    │
         │   / coap_client / coap_server              │
         │   — hold `Types::serializer_type _serializer` │
         └───────────────────┬──────────────────────┘
                              │ serialize(request) -> Data
                              │ deserialize_<msg>(Data) -> response
                              ▼
              ┌────────────────────────────────┐
              │   cbor_rpc_serializer<Data>      │
              │   (this document)                │
              └────────────────────────────────┘
```

This is structurally identical to the existing JSON path — the only difference is which
concrete type transports resolve `Types::serializer_type` to.

## Components and Interfaces

### `cbor_rpc_serializer<Data>`

```cpp
template<typename Data>
requires std::ranges::range<Data> && std::same_as<std::ranges::range_value_t<Data>, std::byte>
class cbor_rpc_serializer {
public:
    // One serialize overload per message type (same set as json_rpc_serializer):
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t>
    [[nodiscard]] auto serialize(const request_vote_request<NodeId, TermId, LogIndex>&) const -> Data;
    template<typename TermId = std::uint64_t>
    [[nodiscard]] auto serialize(const request_vote_response<TermId>&) const -> Data;
    // ... request_pre_vote_request/response, append_entries_request/response,
    //     install_snapshot_request/response, cluster_join_request/response,
    //     cluster_leave_request/response, fetch_log_entries_request/response

    // One deserialize_<message> per message type, mirroring json_rpc_serializer's names:
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t>
    [[nodiscard]] auto deserialize_request_vote_request(const Data&) const
        -> request_vote_request<NodeId, TermId, LogIndex>;
    // ... one per message type, same signatures as json_rpc_serializer

    // Generic dispatcher, same if constexpr chain as json_rpc_serializer::deserialize
    template<typename T> [[nodiscard]] auto deserialize(const Data& data) const -> T;

    [[nodiscard]] auto name() const -> std::string { return "cbor"; }

private:
    // --- Encoding primitives (RFC 8949 §3) ---
    auto write_uint(std::vector<std::byte>& out, std::uint64_t value) const -> void;
    auto write_byte_string(std::vector<std::byte>& out, const std::vector<std::byte>& bytes) const -> void;
    auto write_text_string(std::vector<std::byte>& out, std::string_view text) const -> void;
    auto write_bool(std::vector<std::byte>& out, bool value) const -> void;
    auto write_array_header(std::vector<std::byte>& out, std::size_t count) const -> void;
    auto write_map_header(std::vector<std::byte>& out, std::size_t count) const -> void;

    // --- Decoding cursor: sequential position tracking over Data ---
    struct decode_cursor {
        const std::byte* pos;
        const std::byte* end;
    };
    auto read_major_type_and_info(decode_cursor& cur) const -> std::pair<std::uint8_t, std::uint8_t>;
    auto read_uint(decode_cursor& cur) const -> std::uint64_t;
    auto read_byte_string(decode_cursor& cur) const -> std::vector<std::byte>;
    auto read_text_string(decode_cursor& cur) const -> std::string;
    auto read_bool(decode_cursor& cur) const -> bool;
    auto read_array_header(decode_cursor& cur) const -> std::size_t;
    auto read_map_header(decode_cursor& cur) const -> std::size_t;

    // --- Message-level helpers ---
    auto to_data(const std::vector<std::byte>& bytes) const -> Data;
    auto require_discriminant(decode_cursor& cur, std::string_view expected) const -> void;
};

static_assert(rpc_serializer<cbor_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>,
              "cbor_rpc_serializer must satisfy the rpc_serializer concept");

using cbor_serializer = cbor_rpc_serializer<std::vector<std::byte>>;
```

Unlike `boost::json::parse`, which builds a navigable DOM up front so field access is
random-access-by-key, CBOR's definite-length map/array encoding is a flat, sequential
byte stream: a map's header declares only *how many* key/value pairs follow, not their
byte offsets. Decoding therefore walks the buffer once, left to right, via
`decode_cursor` — reading each declared key, matching it against the known field set for
that message type, and reading the corresponding value inline. This is the one
structural difference from `json_rpc_serializer`'s `obj["field"]`-style access, and the
reason `read_*` helpers take `decode_cursor&` by reference rather than an index.

### CBOR Item Encoding (RFC 8949 §3, the accepted subset)

| Kythira value            | CBOR major type | Additional info                         |
|---------------------------|-----------------|-------------------------------------------|
| unsigned integer field     | 0 (unsigned int)| immediate (0-23) or 1/2/4/8-byte length-prefixed, per RFC 8949 §3.1 |
| `entry_type` enum          | 0 (unsigned int)| `static_cast<std::underlying_type_t<entry_type>>(value)` |
| byte field (`command`, `data`) | 2 (byte string) | length-prefixed, same width rule as above |
| string field (`NodeId`/`Address` as `std::string`, discriminant) | 3 (text string) | length-prefixed, same width rule as above |
| `entries()` / message list | 4 (array)       | length-prefixed (item count) |
| message / `LogEntry` object | 5 (map)        | length-prefixed (key/value pair count) |
| `bool` field                | 7 (simple value)| `0xf4` (false) / `0xf5` (true) |

Every length/count uses the shortest RFC 8949 §3.1 encoding that fits the value
(immediate 0-23 in the initial byte; otherwise 1/2/4/8 following bytes for 8/16/32/64-bit
values) — this is what keeps small messages small without requiring "canonical CBOR"
determinism guarantees beyond that.

### Message discriminant

Every encoded message's map includes a `"type"` text-string key (map's first key, by
convention) whose value is the same discriminant string `json_rpc_serializer` already
uses (`"request_vote_request"`, `"append_entries_response"`, etc. — see Requirement 5.1).
`require_discriminant` reads and checks this key before any other field is decoded,
mirroring `json_rpc_serializer`'s `if (obj["type"].as_string() != "...")` guard and
throwing `serialization_exception` on mismatch (Requirement 5.2).

### Per-message map layout

Field order and names are identical to `json_rpc_serializer`'s object layout
(`json_serializer.hpp` lines 21-586) with two representational changes carried through
every message: byte fields become CBOR byte strings instead of base64 text, and absent
`std::optional` fields are omitted map keys (never CBOR null) — both already covered
above. `LogEntry` (used inside `entries()` for both `append_entries_request` and
`fetch_log_entries_response`) is encoded as its own 4-key map: `term`, `index`,
`command` (byte string), `entry_type` (unsigned integer). `PeerInfo`-shaped redirects
(`cluster_join_response`/`cluster_leave_response`) are encoded as a nested 2-key map:
`redirect_node_id`, `redirect_address` — matching `json_rpc_serializer`'s flattened
`redirect_node_id`/`redirect_address` keys rather than introducing a nested
`peer_info`-named map, for exact structural parity with the JSON version.

## Error Handling

No new exception type is introduced. `cbor_rpc_serializer` throws
`kythira::serialization_exception` (`include/raft/exceptions.hpp`) — the same exception
`json_rpc_serializer` already throws for its own message-discriminant mismatches — for
every failure mode:

- Truncated input (a declared length/header extends past the buffer's remaining bytes).
- An unsupported major type/additional-info combination (indefinite-length items, tags,
  floats, negative integers, `null`/`undefined`; see Requirement 6.4).
- A top-level item that is not a map, or a map missing a required key.
- A message-discriminant mismatch (Requirement 5.2).
- An integer value that cannot be narrowed into its target type without loss — checked
  explicitly before the `static_cast`, rather than left to undefined/implementation-defined
  narrowing behavior.

Every `read_*` helper checks `cur.pos` against `cur.end` before advancing or dereferencing
— this is the CBOR decoder's equivalent of `json_rpc_serializer`'s reliance on
`boost::json::parse` to reject malformed text; because this codec is hand-rolled rather
than delegated to a library, these bounds checks are this implementation's responsibility
and are exercised directly by the malformed-input property tests (Requirement 9.3).

## Interactions with Existing Transports

- **`tcp_rpc`/`tls_tcp_rpc`**: No change. These transports frame `Data` with their own
  length-prefix at the socket level and are already serializer-agnostic.
- **CoAP transport**: No change, and a positive side effect —
  `coap_utils::get_content_format_for_serializer` (`include/raft/coap_utils.hpp:149`)
  already matches the substring `"cbor"`/`"CBOR"` in a serializer's `name()` and maps it
  to `coap_content_format::application_cbor` (`coap_utils.hpp:156-160`). Because
  `cbor_rpc_serializer::name()` returns `"cbor"` (Requirement 7.1), CoAP messages are
  automatically tagged with the correct Content-Format option with zero CoAP-transport
  code changes.
- **HTTP transport (cpp-httplib)**: A known, pre-existing limitation, not something this
  spec fixes. `cpp_httplib_client`/`cpp_httplib_server` currently hardcode the
  `Content-Type` header to the literal `"application/json"`
  (`include/raft/http_transport_impl.hpp:34-35`, used at lines 1050 and 1081) regardless
  of `_serializer.name()` — unlike the CoAP transport, HTTP never consults the
  serializer's name when choosing a content type. Naming `cbor_rpc_serializer` as the
  HTTP transport's `serializer_type` still round-trips correctly (both client and server
  use the same serializer, so the byte payload is decoded correctly on receipt), but the
  `Content-Type` header sent on the wire would incorrectly read `application/json`. This
  is called out as a Future Enhancement (generalize
  `cpp_httplib_client`/`cpp_httplib_server` to derive their `Content-Type` from
  `_serializer.name()`, the same information CoAP already uses) rather than included in
  this serializer's requirements, since it is a pre-existing HTTP transport gap that
  would need the same fix regardless of which non-JSON serializer exposed it.

## Testing Strategy

### Unit Tests

1. Concept-conformance `static_assert`s mirroring
   `tests/rpc_serializer_concept_test.cpp`: `cbor_rpc_serializer<std::vector<std::byte>>`
   satisfies `rpc_serializer`; `std::vector<std::byte>` satisfies `serialized_data`.
2. Round-trip unit tests for every message type: construct a value, `serialize`, the
   matching `deserialize_<message>`, assert field-by-field equality — one test per
   message type, mirroring `json_rpc_serializer`'s existing
   `test_json_serializer_instantiation` shape.
3. Message-discriminant mismatch tests: serialize message A, attempt
   `deserialize_<message-B>` on the result, assert `serialization_exception` is thrown.
4. `name()` returns a string containing `"cbor"`, and
   `coap_utils::get_content_format_for_serializer(serializer.name())` resolves to
   `coap_content_format::application_cbor`.

### Property-Based Tests

Following this project's existing convention
(`tests/rpc_serialization_property_test.cpp`, `tests/rpc_malformed_message_property_test.cpp`),
each tagged `**Feature: cbor-rpc-serializer, Property {number}: {property_text}**` and run
a minimum of 100 iterations with randomly generated inputs.

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid
executions of a system — a formal statement about what the system should do.*

### Property 1: Round-trip preserves content

*For any* valid `kythira` request or response value for any RPC message type in scope
(Requirements 2, 3), `deserialize_<message>(serialize(value))` produces a value equal to
`value` across every field, including `entries()` empty, `conflict_index`/`conflict_term`
absent, `redirect` absent, and both integral and `std::string` `NodeId`/`Address`.
**Validates: Requirements 2.7, 2.8, 3.5, 4.1, 4.2, 4.3**

### Property 2: Malformed and truncated input is rejected, never crashes

*For any* random or deliberately-truncated byte sequence that is not a valid encoding for
the message type a `deserialize_*` method expects, that method throws
`kythira::serialization_exception` — it never reads out of bounds, never invokes
undefined behavior, and never returns a partially-populated value.
**Validates: Requirements 6.1, 6.2, 6.4, 9.3**

### Property 3: Message-discriminant mismatch is detected

*For any* two distinct message types A and B in scope, calling B's `deserialize_*` method
on the output of A's `serialize` throws `kythira::serialization_exception`.
**Validates: Requirements 5.2, 9.4**

### Property 4: CBOR output is never larger than JSON output for byte-carrying messages

*For any* `append_entries_request` or `install_snapshot_request` value with a non-empty
`command`/`data` byte field, `cbor_rpc_serializer`'s serialized size is no larger than
`json_rpc_serializer`'s serialized size for the same logical value.
**Validates: Requirement 10.1**

### Property 5: `name()` resolves to the CoAP CBOR content format

*For any* instantiation of `cbor_rpc_serializer<Data>`, `name()` contains the substring
`"cbor"`, and passing it to `coap_utils::get_content_format_for_serializer` resolves to
`coap_content_format::application_cbor`.
**Validates: Requirement 7.1, 7.2**

### Property-Based Testing Configuration

Each property-based test should:
- Run a minimum of 100 iterations with randomly generated inputs.
- Be tagged `**Feature: cbor-rpc-serializer, Property {number}: {property_text}**`.
- Reuse the input-generation helpers already present in
  `tests/rpc_serialization_property_test.cpp` (random terms, indices, node IDs, commands,
  log entries) rather than duplicating them, parameterizing the existing generators over
  serializer type where practical.

## Implementation Notes

### No new exception type

`serialization_exception` (`include/raft/exceptions.hpp`) is reused as-is; a
CBOR-specific exception subtype was considered and rejected as an unnecessary
abstraction — nothing in this project's error-handling paths (HTTP/CoAP/TCP transports'
`catch` blocks) discriminates on serializer-specific exception subtypes today, and
`json_rpc_serializer` itself does not define one either.

### Decoder scope is deliberately narrow

The decoder accepts exactly the major-type/additional-info combinations the 14 message
shapes require and rejects everything else outright (Requirement 6.4), rather than
implementing a general-purpose, spec-complete CBOR parser. This keeps the implementation
small enough to audit by hand and avoids a class of parser bugs that come from accepting
constructs (indefinite-length streaming, tags, floats) with no corresponding kythira
field to decode them into.

### Thread safety

`cbor_rpc_serializer` holds no mutable state across calls (its `write_*`/`read_*` helpers
operate on caller-provided buffers/cursors), so — like `json_rpc_serializer` — a single
instance may be safely shared and called concurrently from multiple threads without
external synchronization.

## Dependencies

### External Libraries

None (Requirement 8) — this is the deliberate contrast with a schema/codegen-driven
alternative such as Protocol Buffers.

### Internal Dependencies

- **raft/types.hpp**: RPC message types and concepts (`request_vote_request<>` etc.),
  `rpc_serializer`/`serialized_data` concepts.
- **raft/exceptions.hpp**: `serialization_exception`.
- **raft/coap_utils.hpp** (indirect, via `name()`): existing Content-Format detection this
  implementation plugs into without modification.

## Build Integration

No `vcpkg.json` or `Kconfig` changes (Requirement 8). New files only:

```
include/raft/
└── cbor_serializer.hpp             # cbor_rpc_serializer<Data>, cbor_serializer alias
tests/
├── cbor_serializer_concept_test.cpp        # mirrors rpc_serializer_concept_test.cpp
├── cbor_serialization_property_test.cpp    # mirrors rpc_serialization_property_test.cpp
└── cbor_malformed_message_property_test.cpp # mirrors rpc_malformed_message_property_test.cpp
```

Each new test target is added to `tests/CMakeLists.txt` following the existing
`json`-serializer test targets' pattern (a plain Boost.Test executable with no additional
`find_package` dependency).

## Performance Considerations

- **No text-parsing overhead**: unlike `boost::json::parse`, decoding is a single
  sequential pass over the byte buffer with no intermediate string-to-number conversion.
- **No base64 inflation**: byte fields (`command`, `data`) are copied directly as CBOR
  byte strings, avoiding `json_rpc_serializer`'s ~33% size increase on exactly the fields
  most likely to dominate message size (`AppendEntries` with large commands,
  `InstallSnapshot` chunks).
- **Compact integers**: RFC 8949's variable-width unsigned-integer encoding means small
  values (most `term`/`index` values in a healthy cluster) cost 1-3 bytes versus JSON's
  decimal-digit-per-byte cost, though this varies with value magnitude and is not assumed
  without the Property 4 measurement.
- Expected throughput/size characteristics should be benchmarked the same way
  `doc/future_backend_performance_comparison.md` benchmarks future backends, rather than
  asserted here without measurement, should the project want a
  `doc/cbor_serializer_performance_comparison.md` companion once implemented.

## Security Considerations

### Bounds-checked decoding

Every `read_*` helper validates a declared length/count against the decode cursor's
remaining bytes *before* using it to size a `resize`/reserve or advance the cursor — CBOR
length-prefixed items are a classic parser attack surface (a crafted header claiming a
length far larger than the actual remaining buffer), and because this codec is hand-rolled
rather than delegated to an already-hardened library, this bounds-checking discipline is
this implementation's own responsibility rather than inherited for free.

### Restricted accepted subset

The decoder accepts only the major-type/additional-info combinations documented above and
rejects everything else (Requirement 6.4) rather than attempting to interpret or skip over
unrecognized constructs. This closes off an entire class of "the decoder silently accepts
a malformed-but-plausible variant" bugs by refusing anything outside the validated,
narrow subset — there is no best-effort fallback path for an attacker to discover and
exploit.

### No trust-on-first-use or format negotiation

`cbor_rpc_serializer` does not negotiate format with a peer; both sides of a connection
must be configured with the same `Types::serializer_type` out of band (the same
requirement `json_rpc_serializer` already has). A future mixed-format deployment would
need an explicit content-type-driven dispatch layer, which is out of scope here.

## Future Enhancements

### Integer-keyed compact encoding

A `cbor_rpc_serializer_compact` (or a mode flag) using small unsigned-integer map keys
instead of text-string field names would shave several bytes per field at the cost of
losing the current implementation's "hex dump reads like the JSON field names" property.
Left as a follow-on since it is a pure size/readability tradeoff, not a correctness gap.

### HTTP transport Content-Type generalization — implemented

As described in Interactions with Existing Transports, `cpp_httplib_client`/
`cpp_httplib_server` should derive their `Content-Type` header from `_serializer.name()`
(as CoAP already does via `coap_utils::get_content_format_for_serializer`) instead of the
hardcoded `"application/json"` literal, so that CBOR (or any future non-JSON serializer)
is correctly labeled on the wire over HTTP too.

This has since been implemented: `include/raft/http_transport_impl.hpp` adds a
`content_type_for_serializer` helper that composes
`coap_utils::get_content_format_for_serializer(name())` with
`coap_utils::content_format_to_string`, and both the client request and server response
paths use it in place of the removed `content_type_json` literal. It is advisory labeling
only — both peers still decode with their own configured `_serializer`, so no
wire-protocol behavior changes.

### Canonical CBOR (RFC 8949 §4.2) conformance

The current design already uses the shortest-length encoding for every integer/length
(RFC 8949 §4.2 rule 1) and definite-length-only items (rule 3), but does not yet enforce
map-key sort order (rule 2) — this implementation's map keys are written in each
message's fixed, declared field order rather than sorted, which is deterministic per
message type but not "canonical" in the RFC's byte-for-byte, cross-implementation sense.
Full canonical-CBOR conformance is left as a follow-on, to be taken up only if a concrete
need for it emerges (e.g. content-addressing serialized messages by hash).
