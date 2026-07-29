# Implementation Plan

## Major Tasks Overview

### Tasks 1-2: Dependency and Build Integration
*Stand up the `ion-c` overlay port and Kconfig/CMake gating before any C++ serializer
code is written, so later tasks can assume `ionc::ionc` is linkable.*

### Tasks 3-5: Core Serializer (`ion_rpc_serializer`)
*RAII wrappers, shared field helpers, and one serialize/deserialize pair per RPC message
type — parity with `json_rpc_serializer`'s coverage.*

### Task 6: Transport Format-Detection Integration
*CoAP Content-Format and HTTP Content-Type updates so `ion_rpc_serializer` is genuinely
substitutable end-to-end, not just at the type level.*

### Tasks 7-9: Testing, Documentation, Final Validation

## Detailed Task List

- [ ] 1. Add the `ion-c` overlay port
  - [ ] 1.1 Create `vcpkg-overlays/ion-c/vcpkg.json`
    - Pin an `amazon-ion/ion-c` release; declare `vcpkg-cmake`/`vcpkg-cmake-config` host
      dependencies
    - _Requirements: 7.3_
  - [ ] 1.2 Create `vcpkg-overlays/ion-c/portfile.cmake`
    - `vcpkg_from_github` fetching the pinned `ion-c` release; `vcpkg_cmake_configure`/
      `vcpkg_cmake_install`/`vcpkg_cmake_config_fixup`, following
      `vcpkg-overlays/stdexec/portfile.cmake`'s CMake-port shape (not
      `vcpkg-overlays/lakers`'s cargo-build shape)
    - `vcpkg_install_copyright` from `ion-c`'s license file
    - _Requirements: 7.3_
  - [ ] 1.3 Register the overlay port in `vcpkg-configuration.json`
    - Add `"./vcpkg-overlays/ion-c"` to `overlay-ports`
    - _Requirements: 7.3_
  - [ ] 1.4 Add the opt-in `"ion"` feature to root `vcpkg.json`
    - Mirrors the existing `"edhoc"` feature's opt-in shape; depends on `ion-c`
    - _Requirements: 7.3_

- [ ] 2. Wire Kconfig/CMake gating
  - [ ] 2.1 Add `ION_SERIALIZER` Kconfig symbol to the root `Kconfig`
    - Follow the `COAP_TRANSPORT`/`GRPC_TRANSPORT` `depends on`/help-text pattern
    - _Requirements: 7.2, 7.4_
  - [ ] 2.2 Wire `find_package(ionc CONFIG)` and graceful degradation into
        `CMakeLists.txt`
    - `kythira_kconfig_gate(ION_SERIALIZER)` / `kythira_kconfig_require(...)` mirroring
      `COAP_TRANSPORT`'s gating pattern
    - Define the `raft_ion_serializer` interface target linking `ionc::ionc`
    - Warn-and-skip when `ion-c` is not found and `KYTHIRA_KCONFIG_STRICT` is unset
    - _Requirements: 7.1, 7.2, 7.5_

- [ ] 3. Implement `ion-c` RAII wrappers and shared field helpers
  - [ ] 3.1 Create `include/raft/ion_serializer.hpp` skeleton
    - `ion_encoding` enum; `ion_rpc_serializer<Data>` class template with the
      `requires serialized_data<Data>`-equivalent constraint
    - _Requirements: 1.3_
  - [ ] 3.2 Implement `detail::ion_reader_handle`/`detail::ion_writer_handle` RAII
        wrappers
    - Constructors open via `ion_reader_open_buffer`/`ion_writer_open_buffer` (binary or
      text per `ion_encoding`); destructors call `ion_reader_close`/`ion_writer_close`
      unconditionally
    - _Requirements: 4.1, 4.2, 4.3, 5.6_
  - [ ] 3.3 Implement `translate_ion_error(iERR, context)`
    - Maps every non-`IERR_OK` code to `kythira::serialization_exception` with a
      context-carrying message
    - _Requirements: 5.5_
  - [ ] 3.4 Implement shared write helpers
    - `write_field_int`, `write_field_bool`, `write_field_blob` (native Ion `blob`, no
      base64), `write_log_entry`
    - _Requirements: 3.2, 3.3, 3.4_
  - [ ] 3.5 Implement shared read helpers
    - `read_required_int64` (range-checked against target type), `read_required_bool`,
      `read_blob`, `read_log_entry`, `expect_annotation` (validates the single top-level
      annotation matches the expected message-type symbol)
    - _Requirements: 3.2, 3.3, 5.2, 5.3, 5.4_

- [ ] 4. Implement `serialize`/`deserialize_*` for the core three RPCs
  - [ ] 4.1 `request_vote_request`/`response`, `request_pre_vote_request`/`response`
    - Annotation-tagged struct per the Data Models field table; `NodeId = std::string`
      branch serialized as Ion `symbol`/`string`
    - _Requirements: 2.1, 2.2, 2.7, 3.1_
  - [ ] 4.2 `append_entries_request`/`response`
    - `entries` as `list<struct>` with native-`blob` `command`; empty list serializes to
      an empty (not omitted) Ion list; `conflict_index`/`conflict_term` omitted when
      `std::nullopt`
    - _Requirements: 2.1, 2.2, 2.4, 2.5, 3.1, 3.2_
  - [ ] 4.3 `install_snapshot_request`/`response`
    - `data` as native `blob`
    - _Requirements: 2.1, 2.2, 3.1, 3.2_

- [ ] 5. Implement `serialize`/`deserialize_*` for the bootstrap and peer-replication
      extension RPCs
  - [ ] 5.1 `cluster_join_request`/`response`, `cluster_leave_request`/`response`
    - Public-field access (not accessor methods) matching those types' actual shape;
      `redirect`/`PeerInfo`-equivalent struct omitted when absent
    - _Requirements: 2.1, 2.2, 2.6, 3.1_
  - [ ] 5.2 `fetch_log_entries_request`/`response`
    - `entries` reuses the same nested-struct shape as `append_entries_request`
    - _Requirements: 2.1, 2.2, 2.4, 3.1_
  - [ ] 5.3 Implement `deserialize<T>(data)` dispatch table
    - `if constexpr` chain over all fourteen default-templated types, mirroring
      `json_rpc_serializer::deserialize<T>`
    - _Requirements: 2.3_
  - [ ] 5.4 Implement `name()`
    - Returns `"ion-binary"`/`"ion-text"` per the instance's `ion_encoding`
    - _Requirements: 1.4, 4.5_
  - [ ] 5.5 `static_assert(rpc_serializer<ion_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>)`
    - _Requirements: 1.1_

- [ ] 6. Transport format-detection integration
  - [ ] 6.1 Extend `coap_utils::coap_content_format`/`get_content_format_for_serializer`
    - Add `application_ion = 65000`; match on `serializer_name.find("ion")` before the
      existing checks, single value for both encodings
    - _Requirements: 6.2, 6.3_
  - [ ] 6.2 Add a `get_content_type_for_serializer` helper and use it in
        `cpp_httplib_client`/`server`
    - Replace the hardcoded `content_type_json` literal at send sites with the helper's
      result derived from `_serializer.name()`; verify `json_rpc_serializer` still
      produces `"application/json"` unchanged
    - _Requirements: 6.4_
  - [ ] 6.3 Apply the same `Content-Type` change to
        `boost_beast_http_client`/`boost_beast_http_server`
    - _Requirements: 6.4_
  - [ ] 6.4 Verify `simulator_network_client`/`server` require no changes
    - Confirm (does not transmit a Content-Type/Content-Format at all) they already work
      unmodified with `ion_rpc_serializer` substituted for `serializer_type`
    - _Requirements: 6.1_

- [ ] 7. Unit and property-based testing
  - [ ] 7.1 `tests/ion_serializer_concept_test.cpp`
    - Concept-conformance `static_assert`, basic serialize/deserialize smoke test for
      both encodings, `name()` distinguishability
    - _Requirements: 8.1_
  - [ ] 7.2 `tests/ion_serialization_property_test.cpp`
    - **Property 1: Round-trip fidelity (per message type, per encoding)** — one test
      per message type from Requirement 2.1, both encodings, plus the `std::string`
      `NodeId` variant
    - **Validates: Requirements 2.4, 2.5, 2.6, 2.7, 4.2, 4.3, 8.2**
  - [ ] 7.3 **Property 2: Binary-vs-text is transparent to the reader**
    - **Validates: Requirements 4.4, 8.5**
  - [ ] 7.4 **Property 3: Native blob round-trips arbitrary bytes**
    - Include byte sequences with no valid JSON-string/base64-safe interpretation
    - **Validates: Requirements 3.2, 8.6**
  - [ ] 7.5 `tests/ion_malformed_message_property_test.cpp`
    - **Property 4: Malformed input is rejected, never crashes** — random bytes, wrong
      annotation, missing required field, wrong Ion type per field, out-of-range
      numeric value, non-struct top-level value; reuse
      `tests/rpc_malformed_message_property_test.cpp`'s test-vector categories
    - **Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 8.3**
  - [ ] 7.6 `tests/ion_json_serializer_equivalence_property_test.cpp`
    - **Property 5: Ion and JSON serializers agree semantically**
    - **Validates: Requirement 8.4**
  - [ ] 7.7 Register all new test targets in `tests/CMakeLists.txt`, gated on
        `ionc_FOUND`
    - _Requirements: 7.1_

- [ ] 8. Documentation
  - [ ] 8.1 README updates
    - Add `ion_rpc_serializer` to the pluggable-serializer discussion alongside
      `json_rpc_serializer`; document the opt-in `ion` vcpkg feature and
      `ION_SERIALIZER` Kconfig symbol; note the `application/ion` Content-Type and the
      private-use CoAP Content-Format 65000
    - _Requirements: All (documentation only; not required for this spec itself)_
  - [ ] 8.2 Example program demonstrating `ion_rpc_serializer` usage
    - Construct a `transport_types`/`raft_types` bundle with
      `serializer_type = ion_rpc_serializer<std::vector<std::byte>>`, both encodings,
      following this project's example-program guidelines
    - _Requirements: All_

- [ ] 9. Final validation
  - [ ] 9.1 Run the full test suite with `ION_SERIALIZER` enabled
    - All new unit and property tests compile and pass under CTest
    - _Requirements: 8.1-8.6_
  - [ ] 9.2 Confirm graceful degradation with `ION_SERIALIZER` unset/`ion-c` absent
    - Rest of the project configures and builds unaffected; `json_rpc_serializer`-based
      targets are unaffected by the missing optional dependency
    - _Requirements: 7.1_
  - [ ] 9.3 Confirm `KYTHIRA_KCONFIG_STRICT` failure path
    - Configuration fails loudly when `ION_SERIALIZER` is selected but `ion-c` is missing
    - _Requirements: 7.2_
  - [ ] 9.4 End-to-end sanity check over HTTP and CoAP transports
    - A node pair configured with `ion_rpc_serializer` (both `cpp_httplib`/Beast HTTP and
      CoAP transports) completes a full RequestVote/AppendEntries/InstallSnapshot cycle,
      with the correct `Content-Type`/Content-Format observed on the wire
    - _Requirements: 6.1, 6.2, 6.3, 6.4_
