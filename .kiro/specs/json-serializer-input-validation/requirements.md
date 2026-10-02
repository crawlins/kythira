# Requirements Document

## Introduction

`json_rpc_serializer` (`include/raft/json_serializer.hpp`) is the default
`rpc_serializer` and the reference the CBOR, Ion and Protobuf serializers were
modelled on, yet it is now the most permissive of the four on malformed input.
The sibling-implementation parity audit (finding Z1, high; Z2, medium) found,
and this spec re-verified on `main` at `9c738d2`, that the JSON decoder:

- reads every integer field as `static_cast<Target>(obj["…"].as_int64())` with
  no range check (`json_serializer.hpp:228-707`, about fifty sites). A negative
  number wraps into a huge unsigned value (`"term":-1` decodes to term
  2^64-1, which a receiving node would adopt as its current term), and
  `entry_type` (a `std::uint8_t` enum) silently truncates (`300` decodes as
  `44`). `cbor_rpc_serializer` rejects both via `narrow<Target>`
  (`cbor_serializer.hpp:1328-1333`) and `ion_rpc_serializer` via
  `read_int<Target>` (`ion_serializer.hpp:1144-1162`).
- cannot decode a `std::uint64_t` it encoded itself once the value is at
  least 2^63. Boost.JSON stores such a value as `kind::uint64`, and
  `as_int64()` throws on that kind (inferred from Boost.JSON's documented
  accessor semantics; not run, because the cloud sandbox has no Boost).
- stops base64 decoding at the first byte outside the alphabet
  (`json_serializer.hpp:834-837`) and returns what it had, so
  `"command":"AQID!!!!"` decodes to three bytes and `"AQ==AQID"` to one. Log
  entry commands and snapshot chunks are truncated with no error.
- lets Boost.JSON exceptions escape (`boost::system::system_error` from
  `parse`, `as_int64`, `as_string`, `as_object`, `as_array`) for syntax errors,
  missing fields and wrong JSON kinds, instead of `serialization_exception`.
  The test that should catch this (`tests/rpc_malformed_message_property_test.cpp:66-71`
  and five similar blocks) accepts any `std::exception`.
- accumulates base64 bits in a signed `int` that is shifted left without
  masking (`json_serializer.hpp:788-794, 831-839`), which is signed-overflow
  undefined behaviour on any payload longer than three bytes.

This spec brings the JSON decoder to the same validation contract as CBOR and
Ion: every malformed or out-of-range input is rejected with
`serialization_exception`, and nothing is silently truncated. It changes no
encoded bytes; every payload the current encoder produces still decodes to the
same value.

## Glossary

- **json_rpc_serializer**: The type being hardened
  (`include/raft/json_serializer.hpp`).
- **serialization_exception**: The exception type (`include/raft/exceptions.hpp`)
  the `rpc_serializer` family throws on malformed input.
- **Integer field**: Any field the decoder converts to an integral type:
  terms, log indices, node ids, integral group ids, `offset`, `responder_id`,
  and `entry_type`'s underlying type.
- **Target type**: The C++ type an integer field is stored into (for example
  `TermId`, `std::size_t`, `std::underlying_type_t<entry_type>`).
- **Canonical base64**: RFC 4648 §4 base64 with the standard alphabet, `=`
  padding to a multiple of four characters, and zero-valued pad bits; exactly
  the form `bytes_to_base64` emits.
- **Boost.JSON number kinds**: `kind::int64`, `kind::uint64` and
  `kind::double_`. The parser yields `int64` for integers that fit in it,
  `uint64` for larger non-negative integers, and `double_` for anything with a
  fraction or exponent.

## Requirements

### Requirement 1: Range-checked integer decoding

**User Story:** As a Raft node operator, I want the JSON decoder to reject
numbers that do not fit their field, so that a corrupted or hostile peer
cannot plant a wrapped-around term, index or id in my node.

#### Acceptance Criteria

1. WHEN an integer field holds a negative number and its target type is
   unsigned, THE json_rpc_serializer SHALL throw `serialization_exception`.
2. WHEN an integer field holds a value greater than
   `std::numeric_limits<Target>::max()` or less than
   `std::numeric_limits<Target>::min()`, THE json_rpc_serializer SHALL throw
   `serialization_exception`.
3. WHEN an integer field holds a JSON number with a fraction or exponent
   (Boost.JSON `kind::double_`), THE json_rpc_serializer SHALL throw
   `serialization_exception`, even if the value is integral (for example
   `1.0` or `1e3`).
4. THE json_rpc_serializer SHALL accept both `kind::int64` and
   `kind::uint64` for an integer field, so that every `std::uint64_t` value
   from 0 to 2^64-1 round-trips through `serialize` then `deserialize_*`.
5. WHEN `entry_type` decodes to a value outside
   `std::underlying_type_t<entry_type>`, THE json_rpc_serializer SHALL throw
   `serialization_exception`, matching CBOR's `narrow` check.
6. THE json_rpc_serializer SHALL apply criteria 1-5 to integral `group_id`
   values decoded by `decode_group_id`.

### Requirement 2: Strict base64 decoding

**User Story:** As a Raft node operator, I want a damaged `command` or
snapshot `data` field to be rejected rather than truncated, so that a log
entry or snapshot is never applied with missing bytes.

#### Acceptance Criteria

1. WHEN a base64 field contains a character outside the RFC 4648 standard
   alphabet and `=`, THE json_rpc_serializer SHALL throw
   `serialization_exception`.
2. WHEN a base64 field's length is not a multiple of four, THE
   json_rpc_serializer SHALL throw `serialization_exception`.
3. WHEN `=` appears anywhere other than the last one or two positions, or
   more than two `=` appear, THE json_rpc_serializer SHALL throw
   `serialization_exception`.
4. WHEN the pad bits of the final quantum are non-zero, THE
   json_rpc_serializer SHALL throw `serialization_exception`, so that each
   byte string has exactly one accepted encoding.
5. THE json_rpc_serializer SHALL decode the empty string to an empty byte
   vector.
6. THE json_rpc_serializer SHALL decode every string `bytes_to_base64` emits
   back to the original bytes.

### Requirement 3: One exception type for every decode failure

**User Story:** As a transport author, I want every JSON decode failure to
surface as `serialization_exception`, so that I can map it to a client error
the same way for all four serializers.

#### Acceptance Criteria

1. WHEN the input is not valid JSON, or its top-level value is not an object,
   THE json_rpc_serializer SHALL throw `serialization_exception`.
2. WHEN a required field is absent, THE json_rpc_serializer SHALL throw
   `serialization_exception` whose message names the field.
3. WHEN a field has the wrong JSON kind (for example a string where a number
   or array is expected), THE json_rpc_serializer SHALL throw
   `serialization_exception` whose message names the field.
4. THE json_rpc_serializer SHALL NOT let `boost::system::system_error`,
   `std::invalid_argument`, `std::out_of_range` or any other non-
   `serialization_exception` escape from any `deserialize_*` member or from
   `deserialize<T>`. `std::bad_alloc` is the one exception allowed through
   unchanged.
5. THE json_rpc_serializer SHALL keep its existing optional-field behaviour:
   an absent `group_id` decodes to `GroupId{}`, absent `conflict_index`,
   `conflict_term`, `redirect_*` decode to empty optionals, and an absent
   `entry_type` decodes to `entry_type::normal`.
6. THE json_rpc_serializer SHALL keep ignoring unknown keys, so that the
   additive-field compatibility story in `json_serializer.hpp:21-27` still
   holds.

### Requirement 4: No undefined behaviour in the base64 helpers

**User Story:** As a maintainer, I want the base64 helpers to be free of
signed overflow, so that sanitizer builds stay clean and the optimizer cannot
miscompile them.

#### Acceptance Criteria

1. THE `bytes_to_base64` and `base64_to_bytes` helpers SHALL keep their bit
   accumulator bounded (unsigned, masked to the bits still pending) so that no
   shift overflows for inputs of any length.
2. THE `bytes_to_base64` output SHALL be byte-for-byte identical to the
   current output for every input.

### Requirement 5: Wire compatibility

**User Story:** As an operator running a rolling upgrade, I want old and new
nodes to keep talking, so that this hardening needs no flag day.

#### Acceptance Criteria

1. THE json_rpc_serializer SHALL produce byte-for-byte the same encoding as
   before for every message.
2. THE json_rpc_serializer SHALL decode every payload the previous version
   encoded, for values below 2^63, to the same value as before.
3. THE json_rpc_serializer SHALL keep satisfying the `rpc_serializer` concept
   with no change to its public member signatures.

### Requirement 6: Tests at parity with CBOR and Ion

**User Story:** As a maintainer, I want the JSON malformed-input tests to pin
the exception type and cover the cases CBOR and Ion already cover, so that a
regression back to permissive decoding fails CI.

#### Acceptance Criteria

1. THE `rpc_malformed_message_property_test` SHALL require
   `serialization_exception` specifically, removing every
   `catch (const std::exception&)` fallback.
2. THE test suite SHALL include out-of-range cases for each integer target
   width in use: negative into unsigned, `entry_type` above 255, a
   `kind::double_` value, and a value above 2^64-1 (which Boost.JSON parses as
   a double).
3. THE test suite SHALL include bad-base64 cases for both `command` and
   snapshot `data`: an invalid character, a misplaced `=`, a length not a
   multiple of four, and non-zero pad bits.
4. THE test suite SHALL include a property test that round-trips random
   `std::uint64_t` values across the full range, including 2^63 and
   2^64-1, through every integer field of at least RequestVote,
   AppendEntries (entry term and index) and InstallSnapshot.
5. THE test suite SHALL include a property test that random byte strings
   round-trip through `command` and `data`, and that the encoded form is the
   same before and after this change (a recorded golden vector is enough).

## Out of Scope

- Ion's inability to carry `std::uint64_t` values at or above 2^63: it writes
  them as `static_cast<std::int64_t>` (`ion_serializer.hpp:119` and siblings),
  so they arrive negative and are rejected on decode. That fails loudly rather
  than silently and is a separate Ion change.
- Rejecting `entry_type` values that fit `std::uint8_t` but name no
  enumerator. No serializer checks this today; adding it to one would break
  parity.
- Rejecting unknown keys (CBOR does; JSON stays lenient, see 3.6).
- Protobuf's unchecked `uint64` casts (audit Z2) and CBOR-only length
  preflight (audit Z3).
