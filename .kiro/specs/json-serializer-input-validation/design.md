# Design Document

## Overview

The fix is local to `include/raft/json_serializer.hpp`. Every `deserialize_*`
member keeps its signature and its field order; what changes is how it reads
fields. Today each read is an inline `static_cast<T>(obj["k"].as_int64())`
or `base64_to_bytes(std::string(obj["k"].as_string()))`. After this change
each read goes through a small set of private, checked accessors that throw
`serialization_exception` with the field name, and each public decode entry
point runs inside one guard that converts any stray library exception.

Encoding is untouched apart from making `bytes_to_base64`'s accumulator
well-defined; its output bytes do not change.

## Key design decision: accept both Boost.JSON integer kinds

Boost.JSON keeps integers in one of two kinds. The encoder assigns
`std::uint64_t` values, which Boost.JSON stores as `kind::uint64`, and
serializes as plain digits. The parser then reads those digits back as
`kind::int64` when they fit and `kind::uint64` when they do not. The current
decoder calls `as_int64()`, which only accepts `kind::int64`, so it works for
values below 2^63 by accident and throws above it.

The checked reader switches on `value.kind()`:

| kind | handling |
|------|----------|
| `int64` | if negative and `Target` is unsigned, reject; otherwise range-check against `Target` |
| `uint64` | range-check against `Target` |
| `double_` | reject, including integral doubles like `1.0`; the encoder never emits them, and accepting them would admit rounding (2^53 + 1 is not representable) |
| anything else | reject as wrong kind |

This mirrors Ion's `read_int<Target>` (`ion_serializer.hpp:1144-1162`) and
CBOR's `narrow<Target>` (`cbor_serializer.hpp:1328-1333`); the difference is
that JSON, unlike Ion, keeps the full `std::uint64_t` range.

## Key design decision: strict, canonical base64

The current decoder treats the first non-alphabet byte (including `=`) as the
end of input. That made padding work, but it also made every other defect
invisible. The new decoder validates the whole string before emitting bytes:

1. Length must be a multiple of four. The encoder always pads.
2. Every character in positions `[0, n-2)` must be in the alphabet.
3. The last two positions may be `=` (`xx==` or `xxx=`), and `=` may not be
   followed by a non-`=` character.
4. The unused low bits of the final quantum must be zero (RFC 4648 §3.5
   permits a decoder to reject non-zero pad bits). This makes the encoding
   canonical, so `encode(decode(s)) == s` for every accepted `s`.

Rule 4 is the only one that could reject something a non-Kythira encoder
might produce. No such encoder exists today: every JSON payload on the wire
comes from `bytes_to_base64`, which always zeroes pad bits. The strictness is
cheap now and hard to add once third-party clients exist.

The decoder reserves `(n / 4) * 3` bytes up front; `n` is already bounded by
the size of the JSON text, so this is not an attacker-chosen allocation.

The accumulator becomes `std::uint32_t` and is masked after each emitted
byte (`val &= (1u << valb) - 1`), which removes the signed-overflow UB in
both helpers. `bytes_to_base64` gets the same treatment, and a golden-vector
test proves its output is unchanged.

## Key design decision: one guard, not fifty try blocks

The checked accessors give good messages for the expected failures. A guard
catches whatever is left (a `parse` syntax error, `as_object()` on a top-level
array, a container method throwing `std::out_of_range`):

```cpp
template<typename F>
static auto guarded(const char* message_type, F&& decode) -> decltype(decode()) {
    try {
        return std::forward<F>(decode)();
    } catch (const serialization_exception&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        throw serialization_exception(std::string("JSON decode (") + message_type +
                                      "): " + e.what());
    }
}
```

Each public `deserialize_*` body becomes `return guarded("append_entries_request",
[&] { ... });`. `deserialize<T>` already dispatches to those members, so it
needs no separate guard. `std::bad_alloc` passes through so memory pressure is
not reported as a protocol error.

## Checked accessors

All are private, `static`, and take the object as `const boost::json::object&`.
Switching from `obj["k"]` (which inserts a null into a non-const object when
`k` is absent) to `obj.if_contains("k")` is what turns a missing field into a
named error instead of a `system_error` about a null value.

```cpp
static auto require(const boost::json::object& obj, std::string_view key)
    -> const boost::json::value&;               // throws "missing field 'key'"

template<std::integral Target>
static auto read_int(const boost::json::object& obj, std::string_view key) -> Target;

static auto read_bool(const boost::json::object& obj, std::string_view key) -> bool;
static auto read_string(const boost::json::object& obj, std::string_view key) -> std::string;
static auto read_array(const boost::json::object& obj, std::string_view key)
    -> const boost::json::array&;
static auto read_object(const boost::json::value& v, std::string_view what)
    -> const boost::json::object&;
static auto read_bytes(const boost::json::object& obj, std::string_view key)
    -> std::vector<std::byte>;                  // read_string + strict base64

template<typename Id>
static auto read_id(const boost::json::object& obj, std::string_view key) -> Id;
    // std::string -> read_string, integral -> read_int<Id>
```

`read_int` is constrained to `std::integral`; `entry_type` is read as
`read_int<std::underlying_type_t<entry_type>>` and then cast, which is what
gives Requirement 1.5. `offset` is read as `read_int<std::size_t>`.
`decode_group_id` keeps its "absent means `GroupId{}`" branch and otherwise
calls `read_id<GroupId>`.

Optional fields keep `obj.contains(k)` (or `if_contains`) followed by the same
checked reader, so an optional field that is present but malformed is still
rejected.

A small `parse_object(const Data&)` replaces the repeated
`boost::json::parse(bytes_to_string(data)).as_object()` and the
`obj["type"].as_string() != "..."` check, so the message-type test also reads
through `read_string`.

## Error messages

Messages follow `JSON decode: field '<key>': <reason>`, with reasons such as
`missing`, `expected integer, got string`, `negative value for unsigned
field`, `value out of range`, `invalid base64 character`, `misplaced base64
padding`, `base64 length not a multiple of 4`, `non-zero base64 pad bits`.
Tests check the exception type only, not the text, so the wording can evolve.

## Testing strategy

All new cases go in the existing `tests/rpc_malformed_message_property_test.cpp`
and a new `tests/json_serializer_range_property_test.cpp`, both built under the
same Boost.JSON condition as today (`tests/CMakeLists.txt:2224`).

- Tighten every `catch (const std::exception&)` in the existing malformed test
  to `serialization_exception` only. The random-bytes and invalid-syntax
  cases depend on the guard; they are the regression check for Requirement 3.
- Add explicit cases mirroring `ion_malformed_message_property_test.cpp`'s
  `property_out_of_range_numeric_rejected`: `term:-1`, `entry_type:300`,
  `term:1.0`, `term:18446744073709551616`, negative `offset`, negative integral
  `group_id`.
- Add bad-base64 cases for `command` and `data`.
- New range property test: for random `std::uint64_t` (plus fixed 0, 2^63-1,
  2^63, 2^64-1), round-trip RequestVote, AppendEntries with entries,
  InstallSnapshot and FetchLogEntriesResponse and compare field by field.
- Golden vectors: serialize a fixed AppendEntries and InstallSnapshot with
  command/data lengths 0 through 5 and a 1 KiB payload, and compare against
  byte strings captured from the current encoder before the change. This is
  the evidence for Requirements 4.2 and 5.1.
- `ion_json_serializer_equivalence_property_test` and
  `shard_group_id_wire_property_test` already exercise JSON round trips and
  must stay green unchanged.

## Risks

- **A peer currently sending something malformed now gets an error.** That is
  the point, and nothing in-tree sends malformed JSON. The transports already
  catch `std::exception` around decode (for example
  `http_transport_impl.hpp:1311` on the client response path), and `serialization_exception` derives from
  it, so no transport needs a change to keep handling the failure.
- **Performance.** The checked readers add a kind switch and two comparisons
  per integer, and base64 validation is one extra pass over the string. Both
  are small next to Boost.JSON parsing itself; no benchmark gate is proposed.
