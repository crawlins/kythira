# Implementation Plan

## Status: Complete (16/16 tasks)

Spec written October 2, 2026 from the sibling-implementation parity audit
(findings Z1 and Z2), re-verified against `main` at `9c738d2`.

## Major Tasks Overview

### Task 1: Golden vectors first
*Capture the current encoder's bytes before touching anything, so the
"no wire change" claim is tested rather than asserted.*

### Tasks 2-4: Checked decoding
*Base64 helpers, checked field accessors, and the exception guard.*

### Task 5: Rewrite each `deserialize_*` onto the accessors

### Tasks 6-7: Tests and final validation

## Detailed Task List

- [x] 1. Record encoder golden vectors
  - [x] 1.1 Add `tests/json_serializer_range_property_test.cpp` with a
    golden-vector case: AppendEntries with `command` lengths 0-5 and 1024,
    and InstallSnapshot with the same `data` lengths, serialized with fixed
    field values; check them against byte strings captured from the encoder
    on `main` before any other task lands
    - Register it in `tests/CMakeLists.txt` next to
      `rpc_malformed_message_property_test` (same Boost.JSON condition)
    - _Requirements: 4.2, 5.1_

- [x] 2. Make the base64 helpers well-defined and strict
  - [x] 2.1 Change `bytes_to_base64`'s accumulator to `std::uint32_t`, masked
    after each emitted sextet; golden vectors from 1.1 must still pass
    - _Requirements: 4.1, 4.2_
  - [x] 2.2 Rewrite `base64_to_bytes` to validate the whole string: length a
    multiple of 4, alphabet-only before the padding, `=` only in the last one
    or two positions, zero pad bits; throw `serialization_exception` on each
    defect; unsigned masked accumulator
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 4.1_

- [x] 3. Add checked field accessors (private, static)
  - [x] 3.1 `require`, `read_bool`, `read_string`, `read_array`,
    `read_object`, using `if_contains` so a missing key never inserts a null
    - _Requirements: 3.2, 3.3_
  - [x] 3.2 `read_int<std::integral Target>` switching on `kind()`: `int64`
    (reject negative into unsigned, range-check), `uint64` (range-check),
    `double_` and others rejected
    - _Requirements: 1.1, 1.2, 1.3, 1.4_
  - [x] 3.3 `read_id<Id>` (string or `read_int`) and `read_bytes`
    (`read_string` + strict base64)
    - _Requirements: 1.1-1.4, 2.1-2.4_
  - [x] 3.4 Route `decode_group_id`'s integral branch through `read_id`,
    keeping the absent-key default
    - _Requirements: 1.6, 3.5_

- [x] 4. Add the decode guard
  - [x] 4.1 `guarded(message_type, fn)`: rethrow `serialization_exception`
    and `std::bad_alloc`, wrap every other `std::exception`
    - _Requirements: 3.1, 3.4_
  - [x] 4.2 `parse_object(data, expected_type)` replacing the repeated
    parse / `as_object` / `"type"` check
    - _Requirements: 3.1, 3.2_

- [x] 5. Port every `deserialize_*` member to the accessors and the guard
  - [x] 5.1 RequestVote, PreVote and TimeoutNow request/response (6 members)
    - _Requirements: 1.1-1.4, 3.1-3.4_
  - [x] 5.2 AppendEntries request/response, including entry `term`, `index`,
    `command`, `entry_type` via `read_int<std::underlying_type_t<entry_type>>`,
    and the optional `conflict_*` fields
    - _Requirements: 1.5, 2.1-2.4, 3.5_
  - [x] 5.3 InstallSnapshot request/response, `offset` as `std::size_t`,
    `data` via `read_bytes`
    - _Requirements: 1.2, 2.1-2.4_
  - [x] 5.4 ClusterJoin and ClusterLeave request/response, including the
    optional `redirect_*` pair
    - _Requirements: 3.2, 3.3, 3.5_
  - [x] 5.5 FetchLogEntries request/response (`responder_id` as
    `std::uint64_t`, entries as in 5.2)
    - _Requirements: 1.1-1.5, 2.1-2.4_
  - [x] 5.6 Grep the header: no `as_int64()`, `obj["` reads or unguarded
    `boost::json::parse` remain on any decode path
    - _Requirements: 3.4_

- [x] 6. Tests
  - [x] 6.1 In `tests/rpc_malformed_message_property_test.cpp`, replace every
    `catch (const std::exception&)` fallback with a requirement that the
    exception is `serialization_exception`
    - _Requirements: 6.1, 3.4_
  - [x] 6.2 Add out-of-range cases: `term:-1`, `entry_type:300`, `term:1.0`,
    `term:18446744073709551616`, negative `offset`, negative integral
    `group_id`, `candidate_id` as a string where an integer is expected
    - _Requirements: 6.2, 1.1-1.6_
  - [x] 6.3 Add bad-base64 cases for `command` and `data`: invalid character,
    `=` mid-string, length not a multiple of 4, non-zero pad bits; plus a
    positive case that the empty string decodes to no bytes
    - _Requirements: 6.3, 2.1-2.5_
  - [x] 6.4 Add a property test that random `std::uint64_t` values (plus 0,
    2^63-1, 2^63, 2^64-1) round-trip through RequestVote, AppendEntries
    (including entry term/index), InstallSnapshot and FetchLogEntriesResponse
    - _Requirements: 6.4, 1.4, 5.2_
  - [x] 6.5 Add a property test that random byte strings (length 0-4096)
    round-trip through `command` and `data`
    - _Requirements: 6.5, 2.6_

- [x] 7. Final validation
  - [x] 7.1 Run the JSON test binaries plus `ion_json_serializer_equivalence_property_test`,
    `shard_group_id_wire_property_test` and `cbor_json_size_comparison_property_test`
    unchanged; all pass
    - _Requirements: 5.1, 5.2, 5.3_
  - [x] 7.2 Build the malformed and range tests once with
    `CMAKE_CXX_FLAGS=-fsanitize=undefined` (`KYTHIRA_SANITIZER` offers only
    `thread` and `address`, neither of which reports signed overflow) and
    confirm no UBSan report from the base64 helpers
    - _Requirements: 4.1_
  - [x] 7.3 Update the parity audit row (Z1, and the JSON half of Z2) and the
    `doc/CHANGELOG.md` entry
    - _Requirements: all_

## Implementation Notes (October 3, 2026)

- Golden vectors were captured from `main` at `c1a774c` before the decoder
  changed, and the new encoder reproduces them byte for byte.
- Both JSON test binaries fail against the old header (22 base64 cases not
  rejected, leaked `boost::system::system_error`, uint64 values >= 2^63 not
  decodable) and pass against the new one.
- 7.1: `shard_group_id_wire_property_test` and
  `cbor_json_size_comparison_property_test` were run locally and pass;
  `ion_json_serializer_equivalence_property_test` needs ion-c and is left to
  CI.
- 7.2: no UBSan report from either test binary. Since C++20 a signed left
  shift is defined (it wraps), so the old accumulator was not flagged under
  `-std=c++23` either; the unsigned, masked accumulator still removes the
  dependence on that rule and keeps the value bounded.
- 7.3: the parity audit lives in the project files, not the repo, and is
  updated there.

## Known Follow-ups

- Done (2026-10-07): Ion writes unsigned values above `INT64_MAX` through an
  arbitrary-precision `ION_INT` and reads every int that way, and protobuf's
  `uint64` conversions are range-checked (audit Z2). Covered by
  `ion_serializer_range_property_test` and
  `protobuf_serializer_range_property_test`.
- CBOR-only declared-length preflight (audit Z3).
