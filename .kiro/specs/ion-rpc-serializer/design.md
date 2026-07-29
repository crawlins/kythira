# Design Document

## Overview

`ion_rpc_serializer<Data>` is a second implementation of the `rpc_serializer<S, Data>`
concept (`include/raft/types.hpp`), sitting alongside `json_rpc_serializer<Data>`
(`include/raft/json_serializer.hpp`) as an alternative `Types::serializer_type`. It
converts the same fourteen Raft RPC request/response struct types to and from an opaque
`serialized_data` byte buffer, using Amazon Ion (via the `ion-c` C library) instead of
JSON as the wire encoding.

Because every consumer of `serializer_type` — `cpp_httplib_client`/`server`,
`boost_beast_http_client`/`server`, `coap_client`/`coap_server`,
`simulator_network_client`/`server` — only ever calls `serialize`/`deserialize_*`/
`deserialize<T>`/`name()` on it and moves the resulting bytes, `ion_rpc_serializer` is a
**pure substitution**: no transport class changes shape. The only non-substitution work
is teaching the two transports' format-detection helpers (CoAP's Content-Format option,
HTTP's `Content-Type` header) about a new serializer name, covered in Requirement 6.

### Key design decision: annotation-based message typing, not a `"type"` field

`json_rpc_serializer` identifies a message's RPC type with a `"type"` JSON string field
inside the object (`obj["type"] = "request_vote_request"`) because JSON has no native
mechanism for tagging a value's semantic type. Ion does: an **annotation** is a `symbol`
(or list of symbols) attached to a value without being a field of it. `ion_rpc_serializer`
uses a single annotation on the top-level `struct` for this purpose:

```
request_vote_request::{term:1,candidate_id:2,last_log_index:3,last_log_term:4}
```

This is both more idiomatic Ion and marginally cheaper on the wire (an annotation is one
symbol reference; a `"type"` field costs a field-name symbol *and* a string value). The
deserializer reads the annotation before touching the struct's fields, exactly mirroring
`json_rpc_serializer`'s existing "check `obj["type"]` first, then parse fields" structure
— *only* the mechanism changes, not the validation order (Requirement 5.2).

### Key design decision: native `blob`, not base64-in-string

`json_rpc_serializer::bytes_to_base64`/`base64_to_bytes` exist solely because JSON has no
binary type — every `command`/`data` byte payload is base64-encoded into a JSON string,
costing ~33% wire-size inflation and a full encode/decode pass per field. Ion has a
native `blob` type built for exactly this. `ion_rpc_serializer` writes `command`/`data`
directly as `ion_writer_write_blob`/reads them via `ion_reader_read_lob_bytes` with no
intermediate text encoding (Requirement 3.2). This is the serializer's most concrete,
measurable advantage over the JSON implementation and is called out explicitly rather
than left implicit, since it is also the reason `ion_rpc_serializer` has no
`bytes_to_base64`/`base64_to_bytes`-equivalent helpers at all.

### Key design decision: binary-first, with text as a debugging aid

Ion's binary encoding is self-describing (a 4-byte version marker distinguishes it
unambiguously from text Ion) and is the format that realizes Ion's size/parse-speed
advantages over JSON; text Ion is a superset-of-JSON syntax useful mainly for humans
reading logs or `ion` CLI output. `ion_rpc_serializer` therefore defaults to writing
binary Ion, but accepts a constructor-time choice to write text Ion instead, for
deployments that want to `tcpdump`/log-inspect raw RPC bodies during development
(Requirement 4). Because the binary marker makes the two encodings self-describing,
**deserialize is always encoding-agnostic** — a single code path accepts either, so a
binary-writing node and a text-writing node (e.g. one built with a debug flag) can still
interoperate without negotiating format out of band (Requirement 4.4). This mirrors how
CoAP's block-wise transfer and the transport's own wire framing already tolerate
heterogeneous peers without a version-negotiation handshake.

### Non-goal: gzip-wrapped Ion

The Ion specification permits either encoding to be wrapped in Gzip. `ion_rpc_serializer`
does not support this. RPC payloads here are already small, latency-sensitive, and
transport-layer-compressed where that matters (e.g. HTTP/2 for a future gRPC transport);
adding a decompression step to every RPC deserialize would be a needless attack surface
(decompression-bomb-shaped inputs) for a feature this project does not need. If a future
use case wants compressed snapshots, that belongs at the transport or snapshot layer, not
folded into the RPC serializer that also carries small per-heartbeat messages.

## Architecture

### Component Diagram

```
┌─────────────────────────────────────────────────────────────┐
│              cpp_httplib_client/server, coap_client/server,   │
│              simulator_network_client/server                  │
│         (parameterized by Types::serializer_type)             │
└───────────────────────────┬───────────────────────────────────┘
                             │ serialize / deserialize_* / deserialize<T> / name()
                             ▼
              ┌───────────────────────────────┐
              │   ion_rpc_serializer<Data>     │   (drop-in alternative to
              │                                 │    json_rpc_serializer<Data>)
              │  - serialize(msg) -> Data       │
              │  - deserialize_<name>(Data)     │
              │  - deserialize<T>(Data) -> T    │
              │  - name() -> "ion-binary"/      │
              │              "ion-text"         │
              └───────────────┬─────────────────┘
                              │ ion_writer_*/ion_reader_* (ionc/ion.h)
                              ▼
              ┌───────────────────────────────┐
              │        ion-c (C library)       │
              │  binary & text Ion reader/     │
              │  writer, symbol tables         │
              └─────────────────────────────────┘
```

### Serialize Flow (example: `append_entries_request<>`)

1. `ion_writer_open_buffer` (or `_stream`) opens a writer over an in-memory buffer sized
   from a small initial estimate, growing as needed — mirroring
   `json_rpc_serializer::json_to_bytes`'s role of producing the final `Data` buffer.
2. `ion_writer_add_annotation_symbol(writer, "append_entries_request")` tags the
   upcoming value.
3. `ion_writer_start_container(writer, tid_STRUCT)` opens the top-level struct.
4. For each scalar field (`term`, `leader_id`, `prev_log_index`, `prev_log_term`,
   `leader_commit`): `ion_writer_write_field_name` + `ion_writer_write_int64`/
   `write_bool` as appropriate.
5. For `entries`: `ion_writer_write_field_name(writer, "entries")` then
   `ion_writer_start_container(writer, tid_LIST)`; for each `log_entry<>`, a nested
   struct with `term`/`index` (`int`), `command` (native `blob`, Requirement 3.2), and
   `type` (`int`, the `entry_type` enum's underlying value); `ion_writer_finish_container`
   closes the list.
6. `ion_writer_finish_container` closes the struct; `ion_writer_close` flushes and
   finalizes the writer, producing the final byte buffer copied into `Data`.

### Deserialize Flow (example: `append_entries_request<>`)

1. `ion_reader_open_buffer` opens a reader over the input bytes — this call alone
   determines whether the input is well-formed Ion at all (Requirement 5.1); a failure
   here is translated immediately to `serialization_exception`.
2. `ion_reader_next` advances to the top-level value; `ion_reader_get_type` must report
   `tid_STRUCT` (Requirement 5.4) and `ion_reader_get_annotation_count`/
   `get_annotation` must report exactly the expected annotation symbol
   (`"append_entries_request"`) (Requirement 5.2).
3. `ion_reader_step_in` enters the struct; the reader loops over fields via
   `ion_reader_next`/`ion_reader_get_field_name`, dispatching on field name to the
   matching `ion_reader_read_int64`/`read_bool`/(nested struct/list) call, tracking which
   required fields were seen.
4. Any field with an unexpected `ion_reader_get_type` (e.g. `string` where `int` is
   expected), an `int` that does not fit `ion_reader_read_int64`'s target range check, or
   a required field never seen by the time `ion_reader_step_out` returns raises
   `serialization_exception` (Requirement 5.3).
5. `ion_reader_step_out`/`ion_reader_close` release the reader; these run even on the
   exception paths above (Requirement 5.6), via RAII wrapping described in
   "Implementation Notes".

## Components and Interfaces

### `ion_rpc_serializer<Data>`

```cpp
namespace kythira {

enum class ion_encoding : std::uint8_t { binary, text };

template<typename Data>
requires std::ranges::range<Data> && std::same_as<std::ranges::range_value_t<Data>, std::byte>
class ion_rpc_serializer {
public:
    explicit ion_rpc_serializer(ion_encoding encoding = ion_encoding::binary);

    // One overload per request/response type — identical surface to json_rpc_serializer
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t>
    [[nodiscard]] auto serialize(const request_vote_request<NodeId, TermId, LogIndex>&) const -> Data;
    template<typename TermId = std::uint64_t>
    [[nodiscard]] auto serialize(const request_vote_response<TermId>&) const -> Data;
    // ... request_pre_vote_{request,response}, append_entries_{request,response},
    //     install_snapshot_{request,response}, cluster_join_{request,response},
    //     cluster_leave_{request,response}, fetch_log_entries_{request,response}

    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t>
    [[nodiscard]] auto deserialize_request_vote_request(const Data&) const
        -> request_vote_request<NodeId, TermId, LogIndex>;
    // ... one deserialize_<message_name> per type, matching json_rpc_serializer's names

    template<typename T> [[nodiscard]] auto deserialize(const Data& data) const -> T;

    [[nodiscard]] auto name() const -> std::string;  // "ion-binary" | "ion-text"

private:
    ion_encoding _encoding;

    // ion-c writer/reader RAII wrappers (see Implementation Notes)
    [[nodiscard]] auto open_writer() const -> detail::ion_writer_handle;
    [[nodiscard]] auto finish_writer(detail::ion_writer_handle&) const -> Data;
    [[nodiscard]] auto open_reader(const Data&) const -> detail::ion_reader_handle;

    // Shared field-level helpers used by every serialize/deserialize overload
    static auto write_field_int(ion_writer* writer, const char* field, std::int64_t value) -> void;
    static auto write_field_bool(ion_writer* writer, const char* field, bool value) -> void;
    static auto write_field_blob(ion_writer* writer, const char* field,
                                  const std::vector<std::byte>& value) -> void;
    static auto write_log_entry(ion_writer* writer, const log_entry<>& entry) -> void;

    static auto read_required_int64(ion_reader* reader, const char* field) -> std::int64_t;
    static auto read_required_bool(ion_reader* reader, const char* field) -> bool;
    static auto read_blob(ion_reader* reader, const char* field) -> std::vector<std::byte>;
    static auto read_log_entry(ion_reader* reader) -> log_entry<>;

    static auto expect_annotation(ion_reader* reader, const char* expected_type) -> void;
    static auto translate_ion_error(iERR code, const char* context) -> void;  // throws
};

static_assert(rpc_serializer<ion_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>,
              "ion_rpc_serializer must satisfy the rpc_serializer concept");

using ion_serializer = ion_rpc_serializer<std::vector<std::byte>>;

}  // namespace kythira
```

Every public method's name, template-parameter list, and default template arguments are
identical to `json_rpc_serializer`'s (Requirement 2.2) — the only public-surface addition
is the constructor's `ion_encoding` parameter and the `ion_encoding` enum itself. This is
deliberate: a call site can go from `json_rpc_serializer<Data>` to
`ion_rpc_serializer<Data>` as a type-only change (Requirement 1.3), or add
`ion_encoding::text` at construction if it wants readable logs, without touching any
`serialize`/`deserialize_*` call.

`cluster_join_request<>`/`cluster_join_response<>`/`cluster_leave_request<>`/
`cluster_leave_response<>` expose their `node_id`/`contact_address`/`accepted`/`redirect`
members as public fields rather than accessor methods (see `include/raft/types.hpp`);
`ion_rpc_serializer` reads/writes them the same way `json_rpc_serializer` does (direct
member access), it is not introducing accessor methods that do not exist on those types.

## Data Models

### Wire Struct Field Tables

Each row is `field name` → `Ion type` → C++ source. `int`/`bool`/`blob`/`string`/
`symbol`/`struct`/`list` are Ion's native types.

**`request_vote_request` / `request_pre_vote_request`** (annotation:
`request_vote_request` / `request_pre_vote_request`)

| field | Ion type | source |
|---|---|---|
| `term` | int | `req.term()` |
| `candidate_id` | int (or symbol/string if `NodeId = std::string`) | `req.candidate_id()` |
| `last_log_index` | int | `req.last_log_index()` |
| `last_log_term` | int | `req.last_log_term()` |

**`request_vote_response` / `request_pre_vote_response`**

| field | Ion type | source |
|---|---|---|
| `term` | int | `resp.term()` |
| `vote_granted` | bool | `resp.vote_granted()` |

**`append_entries_request`** (annotation: `append_entries_request`)

| field | Ion type | source |
|---|---|---|
| `term` | int | `req.term()` |
| `leader_id` | int/symbol | `req.leader_id()` |
| `prev_log_index` | int | `req.prev_log_index()` |
| `prev_log_term` | int | `req.prev_log_term()` |
| `leader_commit` | int | `req.leader_commit()` |
| `entries` | list\<struct\> | `req.entries()`, each `{term:int, index:int, command:blob, entry_type:int}` |

**`append_entries_response`**

| field | Ion type | source | present when |
|---|---|---|---|
| `term` | int | `resp.term()` | always |
| `success` | bool | `resp.success()` | always |
| `conflict_index` | int | `*resp.conflict_index()` | `has_value()` |
| `conflict_term` | int | `*resp.conflict_term()` | `has_value()` |

**`install_snapshot_request`** (annotation: `install_snapshot_request`)

| field | Ion type | source |
|---|---|---|
| `term` | int | `req.term()` |
| `leader_id` | int/symbol | `req.leader_id()` |
| `last_included_index` | int | `req.last_included_index()` |
| `last_included_term` | int | `req.last_included_term()` |
| `offset` | int | `req.offset()` |
| `data` | blob | `req.data()` — native blob, no base64 (Requirement 3.2) |
| `done` | bool | `req.done()` |

**`install_snapshot_response`**

| field | Ion type | source |
|---|---|---|
| `term` | int | `resp.term()` |

**`cluster_join_request` / `cluster_leave_request`**

| field | Ion type | source |
|---|---|---|
| `node_id` | int/symbol | `req.node_id` |
| `contact_address` (join only) | string | `req.contact_address` |

**`cluster_join_response` / `cluster_leave_response`**

| field | Ion type | source | present when |
|---|---|---|---|
| `accepted` | bool | `resp.accepted` | always |
| `redirect` | struct `{node_id:int/symbol, address:string}` | `*resp.redirect` | `has_value()` |

**`fetch_log_entries_request`** (annotation: `fetch_log_entries_request`)

| field | Ion type | source |
|---|---|---|
| `requester_id` | int/symbol | `req.requester_id()` |
| `from_index` | int | `req.from_index()` |
| `to_index` | int | `req.to_index()` |

**`fetch_log_entries_response`**

| field | Ion type | source |
|---|---|---|
| `responder_id` | int | `resp.responder_id()` |
| `available` | bool | `resp.available()` |
| `prev_log_term` | int | `resp.prev_log_term()` |
| `entries` | list\<struct\> | same shape as `append_entries_request.entries` |

## Error Handling

### Exception Type

`ion_rpc_serializer` throws `kythira::serialization_exception`
(`include/raft/exceptions.hpp`) for every input-validation and `ion-c` failure — the same
type `json_rpc_serializer` throws — rather than introducing a new Ion-specific exception
hierarchy. This is a deliberate compatibility choice: nothing downstream should need to
know or care which serializer produced the exception, since neither serializer's
exception is caught specifically by transport code today (both propagate as generic
`std::exception` failures to callers), and a caller that swaps
`json_rpc_serializer`→`ion_rpc_serializer` should not have its `catch` clauses stop
compiling or stop matching.

### `ion-c` Error Translation

`ion-c` C API calls return an `iERR` status code (`IERR_OK` on success). Every
`ion_rpc_serializer` method that calls into `ion-c` checks the return value and, on
failure, calls `translate_ion_error(code, context)`, which throws
`serialization_exception` with a message combining the failing operation's context
(e.g. `"reading field 'term'"`) and `ion-c`'s own error description
(`ion_error_to_str(code)`). Representative cases:

| `iERR` (`ion-c`) | Cause | `ion_rpc_serializer` behavior |
|---|---|---|
| `IERR_INVALID_ARG` | Bad argument to a `ion-c` call (programming error, should not occur) | `serialization_exception`, treated as a bug if seen |
| `IERR_UNEXPECTED_EOF` | Truncated input | `serialization_exception("unexpected end of Ion input")` |
| `IERR_PARSER_INVALID_TOKEN` / `IERR_INVALID_UTF8` | Malformed text Ion | `serialization_exception` |
| `IERR_NUMERIC_OVERFLOW` | `int` value out of range for target field | `serialization_exception` |
| `IERR_INVALID_STATE` | Reader/writer API misuse (e.g. reading a field before `step_in`) | `serialization_exception`, treated as a bug if seen |

Application-level checks not covered by an `iERR` code (missing annotation, missing
required field, wrong top-level container type — Requirements 5.2-5.4) throw
`serialization_exception` directly from `ion_rpc_serializer` code, with a message naming
the offending field/expectation, mirroring `json_rpc_serializer`'s
`"Invalid message type for request_vote_request"`-style messages.

### CoAP Content-Format Integration

`include/raft/coap_utils.hpp`'s `coap_content_format` enum and
`get_content_format_for_serializer` gain one new entry:

```cpp
enum class coap_content_format : std::uint16_t {
    text_plain = 0,
    application_link_format = 40,
    application_xml = 41,
    application_octet_stream = 42,
    application_exi = 47,
    application_json = 50,
    application_cbor = 60,
    application_ion = 65000,  // NEW: experimental/private-use range (RFC 7252 §12.3);
                               // no IANA-registered Content-Format exists for Ion.
                               // Covers both binary and text Ion — see Requirement 6.3.
};

inline auto get_content_format_for_serializer(const std::string& serializer_name)
    -> coap_content_format {
    if (serializer_name.find("ion") != std::string::npos) {   // NEW, checked
        return coap_content_format::application_ion;           // before the "json"
    }                                                            // check below, or after —
    if (serializer_name.find("json") != std::string::npos || ... // "json"/"ion" never
    // ... existing cases unchanged                                collide as substrings
}
```

65000 is chosen as an arbitrary but documented value from RFC 7252's reserved
experimental-use range; if IANA ever registers an official CoAP Content-Format for Ion,
this project should migrate to it (tracked as a Future Enhancement below) rather than
treat 65000 as a permanent public commitment.

### HTTP Content-Type Integration

`cpp_httplib_client`/`server` and `boost_beast_http_client`/`server` currently hardcode
`content_type_json = "application/json"` (`include/raft/http_transport_impl.hpp:34`,
mirrored in the Beast transport) regardless of which `serializer_type` is actually
configured — a pre-existing gap this spec closes as part of making `ion_rpc_serializer`
genuinely substitutable end-to-end (Requirement 6.4). A small `http_utils`-style helper,
analogous to `coap_utils::get_content_format_for_serializer`, maps a serializer's
`name()` to a media-type string:

```cpp
inline auto get_content_type_for_serializer(const std::string& serializer_name)
    -> std::string {
    if (serializer_name.find("ion") != std::string::npos) {
        return "application/ion";
    }
    return "application/json";  // preserves today's behavior for json_rpc_serializer
                                  // and as the fallback default
}
```

`application/ion` (per the Ion project's own media-type convention) is used for **both**
binary and text Ion, matching the CoAP Content-Format decision above and for the same
reason: the binary version marker makes the two self-describing to any Ion-aware
receiver, so the media type only needs to say "this is Ion," not which Ion.

## Metrics Collection

`ion_rpc_serializer` does not itself hold a `metrics_type`/collect metrics — mirroring
`json_rpc_serializer`, which is a stateless value type with no metrics dependency. Any
serialize/deserialize latency or payload-size metrics remain the transport layer's
responsibility (`cpp_httplib_client`/`coap_client`, etc. already record request/response
sizes independent of which serializer produced them), consistent with how those
transports do not currently dimension metrics by serializer choice either.

## Testing Strategy

### Unit Tests

New `tests/ion_serializer_concept_test.cpp`, mirroring
`tests/rpc_serializer_concept_test.cpp`:

1. `static_assert(kythira::rpc_serializer<kythira::ion_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>)`.
2. Basic instantiate/serialize/deserialize smoke test for one message type, both
   `ion_encoding::binary` and `ion_encoding::text`.
3. `name()` returns a distinguishable string per encoding.

### Property-Based Tests

New `tests/ion_serialization_property_test.cpp`, mirroring
`tests/rpc_serialization_property_test.cpp`'s structure and random-generator helpers
one-for-one (reusing the same `generate_random_*` helper shapes), parameterized over both
encodings — one round-trip property per message type from Requirement 2.1, plus the
`std::string`-`NodeId` variant.

New `tests/ion_malformed_message_property_test.cpp`, mirroring
`tests/rpc_malformed_message_property_test.cpp`: random-byte rejection, wrong-annotation
rejection, missing-field rejection, wrong-Ion-type-per-field rejection, non-struct
top-level-value rejection.

New `tests/ion_json_serializer_equivalence_property_test.cpp` (Requirement 8.4): for each
message type, generate a random value, serialize+deserialize it through both
`json_rpc_serializer` and `ion_rpc_serializer`, and assert the two deserialized results
are field-for-field equal to each other (and to the original).

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid
executions of a system — a formal statement about what the system should do.*

### Property 1: Round-trip fidelity (per message type, per encoding)

*For any* valid value of any of the fourteen message types, for either
`ion_encoding::binary` or `ion_encoding::text`, `deserialize_<message_name>(serialize(v))`
produces a value equal to `v` across every field, including empty `entries()`, absent
`std::optional` fields, and an absent redirect hint.
**Validates: Requirements 2.4, 2.5, 2.6, 2.7, 4.2, 4.3, 8.2**

### Property 2: Binary-vs-text is transparent to the reader

*For any* valid message value, serializing with `ion_encoding::binary` and with
`ion_encoding::text` and deserializing each through the same `deserialize_<message_name>`
call (which does not know which encoding was used to write) both succeed and produce
equal results.
**Validates: Requirements 4.4, 8.5**

### Property 3: Native blob round-trips arbitrary bytes

*For any* `command`/`data` byte sequence, including sequences with no valid
JSON-string/base64-safe interpretation, `ion_rpc_serializer` round-trips it exactly, with
no base64 encode/decode step in the implementation.
**Validates: Requirements 3.2, 8.6**

### Property 4: Malformed input is rejected, never crashes

*For any* input in the categories "random bytes," "wrong annotation," "missing required
field," "wrong Ion type for a field," "numeric value out of target-type range," or
"non-struct top-level value," every `deserialize_<message_name>` call throws
`kythira::serialization_exception` (or, for random bytes, some `std::exception`) rather
than crashing, hanging, or returning a partially-populated struct, and leaks no `ion-c`
reader handle.
**Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 8.3**

### Property 5: Ion and JSON serializers agree semantically

*For any* valid message value, deserializing it after a round trip through
`json_rpc_serializer` and through `ion_rpc_serializer` produces field-for-field-equal
results, even though the two serializers' wire bytes are entirely different.
**Validates: Requirement 8.4**

### Property-Based Testing Configuration

Each property-based test should:
- Run a minimum of 100 iterations with randomly generated inputs (except fixed
  malformed-input test-vector cases, which are exhaustive over their vector list).
- Be tagged `**Feature: ion-rpc-serializer, Property {number}: {property_text}**`.
- Reuse the existing `tests/rpc_serialization_property_test.cpp`/
  `tests/rpc_malformed_message_property_test.cpp` random-value generator helpers where
  the same struct types are involved, rather than re-deriving equivalent generators.

## Implementation Notes

### Thread Safety

`ion_rpc_serializer` is a stateless value type (its only member is the `ion_encoding`
selection, read-only after construction) — every `serialize`/`deserialize_*` call opens
its own `ion-c` reader/writer over a local buffer and closes it before returning, holding
no shared mutable state. This mirrors `json_rpc_serializer`'s statelessness and makes
`ion_rpc_serializer` safe to share (e.g. one instance per transport, called concurrently
from multiple threads) without external synchronization, exactly as
`json_rpc_serializer` already is.

### RAII Wrapping of `ion-c` Handles

`ion-c`'s `ion_reader_open_*`/`ion_reader_close` and `ion_writer_open_*`/
`ion_writer_close` are paired C-style open/close calls, not RAII by default. `ion_serializer.hpp`
defines small internal RAII wrappers (`detail::ion_reader_handle`, `detail::ion_writer_handle`)
whose destructors call the corresponding `_close` function, so that a `serialization_exception`
thrown mid-parse (Requirement 5.6) still releases the handle via stack unwinding rather than
requiring every call site to hand-write a `try`/`catch`-and-close block. This is the same
role RAII plays elsewhere in this codebase for C-API resource ownership (e.g. `libcoap`'s
`coap_context_t*`/`coap_session_t*` wrapping in the CoAP transport).

### Header-Only, Like `json_serializer.hpp`

`include/raft/ion_serializer.hpp` is header-only, mirroring `json_serializer.hpp`'s
structure — the class is a template, so there is no meaningful non-template translation
unit to place implementation code in. `ion-c` itself is a compiled C library the target
links against; only the small RAII wrapper and shared field helpers live in the header.

### Message-Type Dispatch Table

`deserialize<T>(data)` mirrors `json_rpc_serializer::deserialize<T>`'s `if constexpr`
chain over the same fourteen default-templated types, dispatching to the matching
`deserialize_<message_name>` (Requirement 2.3) — this table is duplicated rather than
shared with `json_rpc_serializer` because the two classes have no common base and sharing
it would require introducing one purely to deduplicate a dispatch `if constexpr` chain,
which is not otherwise needed.

## Dependencies

### External Libraries

- **ion-c** (`amazon-ion/ion-c`): Amazon Ion's C reference implementation — binary/text
  reader and writer, symbol table management. Not present in the default vcpkg registry;
  provided via a project-authored overlay port (`vcpkg-overlays/ion-c`), following the
  pattern already established by `vcpkg-overlays/lakers` and `vcpkg-overlays/stdexec`.
  License: Apache 2.0.
- `ion-c` itself has no further mandatory external dependencies beyond a C toolchain and
  CMake (both already required by this project); it optionally bundles a decimal-number
  library (`decNumber`) for Ion's `decimal` type, which `ion_rpc_serializer` does not use
  (all numeric fields here are `int`/`bool`).

### Internal Dependencies

- **raft/types.hpp**: `rpc_serializer`/`serialized_data` concepts; every Raft RPC
  message struct (`request_vote_request<>`, etc.).
- **raft/exceptions.hpp**: `serialization_exception`.
- **raft/coap_utils.hpp**: `coap_content_format` enum and
  `get_content_format_for_serializer`, extended per Requirement 6.2-6.3.
- **raft/http_transport_impl.hpp** / **raft/beast_http_transport_impl.hpp**: the
  `Content-Type` header logic, changed per Requirement 6.4.

## Build Integration

### `vcpkg-overlays/ion-c/vcpkg.json`

```jsonc
{
  "name": "ion-c",
  "version-date": "<pinned ion-c release date>",
  "description": "C implementation of Amazon Ion (amazon-ion/ion-c), built from source via an overlay port since no official vcpkg registry port exists yet.",
  "homepage": "https://github.com/amazon-ion/ion-c",
  "license": "Apache-2.0",
  "dependencies": [
    { "name": "vcpkg-cmake", "host": true },
    { "name": "vcpkg-cmake-config", "host": true }
  ]
}
```

`vcpkg-overlays/ion-c/portfile.cmake` follows `vcpkg-overlays/stdexec/portfile.cmake`'s
shape (`vcpkg_from_github` + `vcpkg_cmake_configure`/`vcpkg_cmake_install` +
`vcpkg_cmake_config_fixup`), not `vcpkg-overlays/lakers/portfile.cmake`'s
`cargo build`-from-source shape, since `ion-c` is a plain CMake C project with no Rust
toolchain requirement — a materially simpler port than `lakers`.

### `vcpkg-configuration.json`

```jsonc
{
  "overlay-ports": [
    "./vcpkg-overlays/lakers",
    "./vcpkg-overlays/stdexec",
    "./vcpkg-overlays/ion-c"
  ]
}
```

### Root `vcpkg.json`

Add an opt-in `"ion"` feature (mirroring the existing `"edhoc"` feature), rather than an
unconditional dependency, so the default build does not fetch/build `ion-c`:

```jsonc
"features": {
  "edhoc": { "...": "..." },
  "ion": {
    "description": "Amazon Ion RPC serializer (ion_rpc_serializer, include/raft/ion_serializer.hpp). Builds ion-c from source via the vcpkg-overlays/ion-c overlay port. Opt-in: not part of the default install; omitting it leaves ION_C_AVAILABLE undefined and ion_rpc_serializer unavailable.",
    "dependencies": ["ion-c"]
  }
}
```

### `Kconfig`

```kconfig
config ION_SERIALIZER
	bool "Amazon Ion RPC serializer (ion-c)"
	help
	  find_package(ionc CONFIG). Backs ION_C_AVAILABLE. vcpkg feature: ion
	  (vcpkg-overlays/ion-c overlay port; not built by default).
	  Provides ion_rpc_serializer (include/raft/ion_serializer.hpp) as an
	  alternative Types::serializer_type to json_rpc_serializer.
```

### `CMakeLists.txt`

```cmake
kythira_kconfig_gate(ION_SERIALIZER)
if(_KYTHIRA_GATE_ION_SERIALIZER)
    find_package(ionc CONFIG QUIET)
endif()
kythira_kconfig_require(ION_SERIALIZER "ionc_FOUND" "ion-c")

if(ionc_FOUND)
    # ion_serializer.hpp is header-only; expose ion-c linkage to consumers
    add_library(raft_ion_serializer INTERFACE)
    target_link_libraries(raft_ion_serializer INTERFACE ionc::ionc)
    target_compile_definitions(raft_ion_serializer INTERFACE KYTHIRA_ION_SERIALIZER_AVAILABLE)
else()
    message(WARNING "ion-c not found. Ion RPC serializer will not be available.")
endif()
```

Test targets consuming `ion_serializer.hpp` (`ion_serializer_concept_test`,
`ion_serialization_property_test`, `ion_malformed_message_property_test`,
`ion_json_serializer_equivalence_property_test`) are only added to
`tests/CMakeLists.txt`'s build when `ionc_FOUND`, mirroring how `COAP_TRANSPORT`-gated
tests are conditionally registered today.

### Header/Source File Structure

```
vcpkg-overlays/ion-c/
├── vcpkg.json
└── portfile.cmake
include/raft/
└── ion_serializer.hpp        # ion_rpc_serializer<Data>, ion_encoding, detail:: RAII wrappers
tests/
├── ion_serializer_concept_test.cpp
├── ion_serialization_property_test.cpp
├── ion_malformed_message_property_test.cpp
└── ion_json_serializer_equivalence_property_test.cpp
```

## Performance Considerations

- **No base64 tax**: `command`/`data` blobs avoid the ~33% size inflation and extra
  encode/decode pass base64-in-JSON requires (Requirement 3.2) — the single largest,
  most measurable difference from `json_rpc_serializer` for `AppendEntries`/
  `InstallSnapshot` traffic, which carry the bulk of the payload bytes in a running
  cluster.
- **Binary Ion vs. JSON text**: Ion's binary encoding represents integers and booleans
  more compactly than JSON's decimal-text digits, and its (optional, `ion-c`-managed)
  symbol tables can amortize repeated field-name costs across many `AppendEntries`
  calls carrying the same struct shape — though this project sends short-lived,
  independently-opened writers per call rather than a persisted shared symbol table
  across calls (see Future Enhancements), so this benefit is partial in the base
  implementation.
- **Parse cost**: `ion-c`'s binary reader does not need to re-tokenize decimal digits or
  unescape JSON string content the way `boost::json::parse` does for every field, though
  this project has not benchmarked the magnitude of that difference and this design does
  not assert a specific speedup without measurement (consistent with this project's
  existing convention of benchmarking rather than asserting — see
  `doc/future_backend_performance_comparison.md` as precedent).

## Security Considerations

### Untrusted-Input Parsing

RPC payloads arrive from network peers and must be treated as untrusted. `ion-c`'s reader
validates structure incrementally (Requirement 5) rather than materializing an
unconstrained in-memory tree before validation, bounding worst-case parse cost to input
size. `ion_rpc_serializer` adds no further trust in a peer's claimed field types or
counts — every read validates the actual Ion type against what the target C++ field
expects (Requirement 5.3) before conversion.

### No Compression

As covered in Overview's "Non-goal" section, Gzip-wrapped Ion is explicitly unsupported,
removing decompression-bomb-shaped inputs as an attack surface for this serializer.

### No Code Execution / Reflection

Ion (unlike, say, a serializer built on native language reflection or `pickle`-style
object graphs) has no mechanism for a payload to name a C++ type or invoke a callback —
`ion_rpc_serializer`'s field-by-field, statically-typed read/write functions are the only
path from bytes to a `kythira` struct, matching `json_rpc_serializer`'s equivalent
static-typing discipline.

## Future Enhancements

### Official IANA CoAP Content-Format

If IANA ever registers a CoAP Content-Format number for Ion, `coap_utils::coap_content_format::application_ion`
should migrate from the private-use value (65000) chosen here to the registered one, with
a transition period recognizing both.

### Shared/Persisted Symbol Table

`ion-c` supports a shared symbol table catalog that would let repeated field names (e.g.
`"term"`, `"entries"`) be encoded once across many messages rather than once per message.
The base implementation opens an independent writer per `serialize` call and does not
persist a symbol table across calls; wiring a per-`ion_rpc_serializer`-instance shared
catalog is a natural follow-on once the base implementation's simpler per-call model is
proven correct, but adds cross-call state (a departure from today's fully stateless
design) that is out of scope here.

### `ion-cpp` Adoption

Should Amazon publish and stabilize a native C++ Ion implementation (the amazon-ion
GitHub organization currently offers `ion-c`, `ion-java`, `ion-go`, `ion-js`,
`ion-python`, and `ion-rust`, but no dedicated `ion-cpp`), migrating from `ion-c`'s C API
to it could remove this design's RAII-wrapper layer entirely. Not pursued now since no
such library exists to adopt.
