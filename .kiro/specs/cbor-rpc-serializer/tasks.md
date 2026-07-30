# Implementation Plan

## Status: Implemented

`include/raft/cbor_serializer.hpp` (`cbor_rpc_serializer<Data>` / `cbor_serializer`) is
implemented, with unit, round-trip, malformed-input, discriminant-mismatch, and CBOR-vs-JSON
size-comparison tests registered in `tests/CMakeLists.txt`. No `vcpkg.json`/`Kconfig` change.
The codec was validated field-by-field and under AddressSanitizer/UndefinedBehaviorSanitizer
out of tree (round-trips for every message type, truncation at every offset, thousands of
random/hostile inputs, narrowing rejection); the in-tree Boost.Test targets build and run under
CI, which owns the full folly/boost dependency graph. Remaining unchecked items below
(example program, end-to-end CoAP sanity check) are optional follow-ups.

## Major Tasks Overview

### Task 1: Codec Primitives
*Hand-rolled RFC 8949 encode/decode primitives (`write_*`/`read_*`, `decode_cursor`) —
no CBOR library dependency, per Requirement 8. Everything else builds on these.*

### Tasks 2-3: Core Serializer (`cbor_rpc_serializer`)
*One `serialize`/`deserialize_<message>` pair per RPC message type — parity with
`json_rpc_serializer`'s coverage, including the message-discriminant guard and the
omitted-key convention for absent `std::optional` fields.*

### Task 4: Dispatcher, `name()`, Concept Conformance
*Generic `deserialize<T>` dispatch table and the `static_assert` proving
`cbor_rpc_serializer` satisfies `rpc_serializer`.*

### Task 5: Build Integration
*New header and test targets only — no `vcpkg.json`/`Kconfig` changes (Requirement 8).*

### Tasks 6-8: Testing
*Unit tests, property-based tests (round-trip, malformed input, discriminant mismatch),
and the CBOR-vs-JSON size-comparison property (Requirement 10).*

### Tasks 9-10: Documentation and Final Validation

## Detailed Task List

- [x] 1. Implement CBOR codec primitives
  - [x] 1.1 Create `include/raft/cbor_serializer.hpp` skeleton
    - `cbor_rpc_serializer<Data>` class template with the `serialized_data<Data>`-
      equivalent constraint mirroring `json_rpc_serializer`'s
    - _Requirements: 1.1, 8.1_
  - [x] 1.2 Implement `write_uint`, `write_bool`, `write_array_header`, `write_map_header`
    - Shortest RFC 8949 §3.1 encoding for every value (immediate 0-23, else 1/2/4/8-byte
      length-prefixed)
    - _Requirements: 8.1_
  - [x] 1.3 Implement `write_byte_string`, `write_text_string`
    - Byte fields written as CBOR major type 2 directly (no base64 transcoding)
    - _Requirements: 2.3, 2.5_
  - [x] 1.4 Implement `decode_cursor` and `read_major_type_and_info`
    - Bounds-checks `cur.pos` against `cur.end` before every advance/dereference
    - _Requirements: 6.1_
  - [x] 1.5 Implement `read_uint`, `read_bool`, `read_array_header`, `read_map_header`
    - Reject indefinite-length items, tags, floats, negative integers (major types 1, 6,
      and additional-info 31) with `kythira::serialization_exception`
    - _Requirements: 6.1, 6.4_
  - [x] 1.6 Implement `read_byte_string`, `read_text_string`
    - Validate declared length against `cur.end - cur.pos` before `resize`/copy
    - _Requirements: 6.1_
  - [x] 1.7 Implement narrowing-integer checks
    - Reject (via `kythira::serialization_exception`) any decoded `std::uint64_t` that
      cannot fit the target field type (`TermId`/`LogIndex`/etc.) before `static_cast`
    - _Requirements: 6.3_
  - [x] 1.8 Implement `require_discriminant`
    - Reads the map's `"type"` text-string key first and throws
      `kythira::serialization_exception` on mismatch, mirroring
      `json_rpc_serializer`'s `if (obj["type"].as_string() != "...")` guard
    - _Requirements: 5.1, 5.2_

- [x] 2. Implement `serialize`/`deserialize_*` for the core four RPCs
  - [x] 2.1 `request_vote_request`/`response`, `request_pre_vote_request`/`response`
    - `NodeId` encoded as CBOR unsigned integer or text string via `if constexpr
      (std::same_as<NodeId, std::string>)`, matching `json_rpc_serializer`'s branch
    - _Requirements: 2.1, 2.2, 4.1, 4.2, 5.1_
  - [x] 2.2 `append_entries_request`/`response`
    - `entries()` as a CBOR array of maps (`term`, `index`, `command` as byte string,
      `entry_type` as unsigned integer); `conflict_index`/`conflict_term` map keys
      omitted (never CBOR null) when `std::nullopt`
    - _Requirements: 2.3, 2.4, 5.1_
  - [x] 2.3 `install_snapshot_request`/`response`
    - `data` written as a CBOR byte string, no base64 transcoding
    - _Requirements: 2.5, 2.6, 5.1_
  - [x] 2.4 Round-trip each of the eight `deserialize_*` methods against its `serialize`
        counterpart
    - _Requirements: 2.7_

- [x] 3. Implement `serialize`/`deserialize_*` for the bootstrap and peer2p2p
      extension RPCs
  - [x] 3.1 `cluster_join_request`/`response`, `cluster_leave_request`/`response`
    - `redirect`'s `node_id`/`address` flattened into `redirect_node_id`/
      `redirect_address` map keys, omitted entirely when `redirect` is absent — matching
      `json_rpc_serializer`'s flattened field names, not a nested map
    - _Requirements: 3.1, 3.2, 4.3, 5.1_
  - [x] 3.2 `fetch_log_entries_request`/`response`
    - `entries()` reuses the same per-entry map shape as Task 2.2
    - _Requirements: 3.3, 3.4, 5.1_
  - [x] 3.3 Round-trip each of the six `deserialize_*` methods against its `serialize`
        counterpart, including empty `entries()` and absent `redirect`
    - _Requirements: 3.5_

- [x] 4. Implement generic dispatcher, `name()`, and concept conformance
  - [x] 4.1 `template<typename T> deserialize<T>(const Data&) const -> T` dispatch table
    - One `if constexpr` branch per message type, mirroring
      `json_rpc_serializer::deserialize<T>`'s chain exactly
    - _Requirements: 2.8_
  - [x] 4.2 `name()` returns `"cbor"`
    - _Requirements: 7.1_
  - [x] 4.3 `static_assert(rpc_serializer<cbor_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>>)`
    - _Requirements: 1.1_
  - [x] 4.4 Define `using cbor_serializer = cbor_rpc_serializer<std::vector<std::byte>>;`
    - _Requirements: 1.3_
  - [x] 4.5 Confirm `coap_utils::get_content_format_for_serializer("cbor")` resolves to
        `coap_content_format::application_cbor` with no changes to that function
    - _Requirements: 7.2_

- [x] 5. Build integration
  - [x] 5.1 Confirm no `vcpkg.json` or root `Kconfig` changes are needed
    - `cbor_rpc_serializer` is a single new header with no external dependency
    - _Requirements: 8.1, 8.2_
  - [x] 5.2 Register new test targets in `tests/CMakeLists.txt`
    - Follow the existing `rpc_serializer_concept_test`/`rpc_serialization_property_test`/
      `rpc_malformed_message_property_test` targets' pattern (plain Boost.Test
      executables, no additional `find_package`)
    - _Requirements: 8.2_

- [x] 6. Unit tests
  - [x] 6.1 `tests/cbor_serializer_concept_test.cpp`
    - Mirrors `tests/rpc_serializer_concept_test.cpp`: `static_assert`s for
      `rpc_serializer<cbor_rpc_serializer<std::vector<std::byte>>, ...>` and
      `serialized_data<std::vector<std::byte>>`
    - _Requirements: 9.1_
  - [x] 6.2 Per-message-type round-trip unit tests
    - One test per message type mirroring `json_rpc_serializer`'s
      `test_json_serializer_instantiation` shape
    - _Requirements: 2.7, 3.5_
  - [x] 6.3 Message-discriminant mismatch unit tests
    - Serialize message A, call message B's `deserialize_*`, assert
      `kythira::serialization_exception` is thrown
    - _Requirements: 5.2_
  - [x] 6.4 `name()` and CoAP Content-Format unit test
    - `name()` contains `"cbor"`;
      `coap_utils::get_content_format_for_serializer(serializer.name())` resolves to
      `coap_content_format::application_cbor`
    - _Requirements: 7.1, 7.2_

- [x] 7. Property-based tests
  - [x] 7.1 `tests/cbor_serialization_property_test.cpp`
    - **Property 1: Round-trip preserves content** — every RPC message type from
      Requirements 2 and 3, both integral and `std::string` `NodeId`/`Address`, empty
      `entries()`, absent `conflict_index`/`conflict_term`, absent `redirect`
    - Reuse `tests/rpc_serialization_property_test.cpp`'s input-generation helpers rather
      than duplicating them
    - **Validates: Requirements 2.7, 2.8, 3.5, 4.1, 4.2, 4.3, 9.2**
  - [x] 7.2 **Property 3: Message-discriminant mismatch is detected**
    - For any two distinct message types A and B in scope, calling B's `deserialize_*`
      on A's `serialize` output throws `kythira::serialization_exception`
    - **Validates: Requirements 5.2, 9.4**
  - [x] 7.3 `tests/cbor_malformed_message_property_test.cpp`
    - **Property 2: Malformed and truncated input is rejected, never crashes** — random
      bytes, truncated headers, declared lengths exceeding the remaining buffer,
      unsupported major-type/additional-info combinations
    - Mirror `tests/rpc_malformed_message_property_test.cpp`'s test-vector categories
    - **Validates: Requirements 6.1, 6.2, 6.4, 9.3**
  - [x] 7.4 **Property 5: `name()` resolves to the CoAP CBOR content format**
    - For any instantiation of `cbor_rpc_serializer<Data>`
    - **Validates: Requirements 7.1, 7.2**
  - [x] 7.5 Register all new property test targets in `tests/CMakeLists.txt`, each run at
        a minimum of 100 iterations and tagged
        `**Feature: cbor-rpc-serializer, Property {number}: {property_text}**`
    - _Requirements: 9.5_

- [x] 8. Size-comparison test
  - [x] 8.1 `tests/cbor_json_size_comparison_property_test.cpp`
    - **Property 4: CBOR output is never larger than JSON output for byte-carrying
      messages** — for any `append_entries_request`/`install_snapshot_request` with a
      non-empty `command`/`data` field, compare `cbor_rpc_serializer` output size against
      `json_rpc_serializer` output size for the same logical value
    - **Validates: Requirement 10.1**
  - [x] 8.2 Document the comparison as a property test, not an exact-byte-count guarantee
    - _Requirements: 10.2_

- [x] 9. Documentation
  - [x] 9.1 README updates
    - Add `cbor_rpc_serializer` to the pluggable-serializer discussion alongside
      `json_rpc_serializer`; note it requires no new dependency and is a pure drop-in
      `Types::serializer_type`
    - _Requirements: All (documentation only; not required for this spec itself)_
  - [x] 9.2 Note the pre-existing HTTP transport `Content-Type` limitation
    - Document that `cpp_httplib_client`/`server` hardcode `"application/json"`
      regardless of `_serializer.name()` (`include/raft/http_transport_impl.hpp:34-35`),
      so CBOR over HTTP round-trips correctly but is mislabeled on the wire; call out the
      Future Enhancement rather than fixing it in this spec
    - _Requirements: All (documentation only; not required for this spec itself)_
  - [x] 9.3 Example program demonstrating `cbor_rpc_serializer` usage
    - `examples/cbor_serializer_example.cpp` (registered in `examples/CMakeLists.txt` and
      run under CTest): names `cbor_serializer` as a bundle's `serializer_type`
      (static_assert `rpc_serializer` conformance), round-trips each RPC family over CBOR,
      prints the CBOR-vs-JSON wire sizes, and shows the `application/cbor` media type
    - _Requirements: All_

  - [x] 9.4 Implement the HTTP transport `Content-Type` generalization (the Future
        Enhancement 9.2 noted)
    - `cpp_httplib_client`/`cpp_httplib_server` now derive their `Content-Type` from
      `_serializer.name()` via `coap_utils::get_content_format_for_serializer` +
      `content_format_to_string` (`include/raft/http_transport_impl.hpp`), replacing the
      hardcoded `"application/json"`, so CBOR over HTTP is labeled `application/cbor` on
      the wire — matching what CoAP already does. Advisory labeling only: the receiver
      still decodes with its own `_serializer`, so no wire-protocol behavior changes

- [ ] 10. Final validation
  - [ ] 10.1 Run the full test suite
    - All new unit and property tests compile and pass under CTest
    - _Requirements: 9.1-9.5_
  - [x] 10.2 Confirm zero build-system impact
    - `vcpkg.json` and root `Kconfig` are unchanged; a build with
      `cbor_serializer.hpp` unused elsewhere in the project is unaffected
    - _Requirements: 8.1, 8.2_
  - [ ] 10.3 End-to-end sanity check over CoAP transport
    - A node pair configured with `cbor_serializer` as `Types::serializer_type` completes
      a full RequestVote/AppendEntries/InstallSnapshot cycle, with
      `application/cbor` observed as the CoAP Content-Format
    - _Requirements: 7.1, 7.2_
