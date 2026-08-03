# Implementation Plan

## Status: Complete (45/45 tasks) — implementation verified working against
the real `ion-c` library (not just `-fsyntax-only`-validated against a hand-
written API stub, the state this was left in when `ion_rpc_serializer` first
merged): all 6 `ion_*`-labeled CTest binaries pass, including a real
RequestVote/AppendEntries/InstallSnapshot round trip over both HTTP
(cpp-httplib) and CoAP with `ion_rpc_serializer` as `Types::serializer_type`
(Task 9.4's own end-to-end sanity check, `tests/ion_http_coap_end_to_end_test.cpp`,
added this round). Building against the real library (rather than the stub)
surfaced four genuine bugs, three of them in `ion-c` itself — see
`## Known Follow-ups` for the full accounting. This file's checkboxes were
last updated to reflect what actually shipped in the original PR that added
`ion_rpc_serializer` (all unchecked, despite ~40/45 already being done per
`doc/TODO.md`'s own, more accurate "Pending Specifications" tracking) — now
reconciled to match reality.

**Last Updated**: August 3, 2026 (real-`ion-c` build verification: task 9's
final validation actually run for the first time, four real bugs found and
fixed; see `## Known Follow-ups`)

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

- [x] 1. Add the `ion-c` overlay port
  - [x] 1.1 Create `vcpkg-overlays/ion-c/vcpkg.json`
    - Pin an `amazon-ion/ion-c` release; declare `vcpkg-cmake`/`vcpkg-cmake-config` host
      dependencies
    - _Requirements: 7.3_
  - [x] 1.2 Create `vcpkg-overlays/ion-c/portfile.cmake`
    - `vcpkg_from_github` fetching the pinned `ion-c` release; `vcpkg_cmake_configure`/
      `vcpkg_cmake_install`/`vcpkg_cmake_config_fixup`, following
      `vcpkg-overlays/stdexec/portfile.cmake`'s CMake-port shape (not
      `vcpkg-overlays/lakers`'s cargo-build shape)
    - `vcpkg_install_copyright` from `ion-c`'s license file
    - _Requirements: 7.3_
  - [x] 1.3 Register the overlay port in `vcpkg-configuration.json`
    - Add `"./vcpkg-overlays/ion-c"` to `overlay-ports`
    - _Requirements: 7.3_
  - [x] 1.4 Add the opt-in `"ion"` feature to root `vcpkg.json`
    - Mirrors the existing `"edhoc"` feature's opt-in shape; depends on `ion-c`
    - _Requirements: 7.3_

- [x] 2. Wire Kconfig/CMake gating
  - [x] 2.1 Add `ION_SERIALIZER` Kconfig symbol to the root `Kconfig`
    - Follow the `COAP_TRANSPORT`/`GRPC_TRANSPORT` `depends on`/help-text pattern
    - _Requirements: 7.2, 7.4_
  - [x] 2.2 Wire `find_package(ionc CONFIG)` and graceful degradation into
        `CMakeLists.txt`
    - `kythira_kconfig_gate(ION_SERIALIZER)` / `kythira_kconfig_require(...)` mirroring
      `COAP_TRANSPORT`'s gating pattern
    - Define the `raft_ion_serializer` interface target linking `ionc::ionc`
    - Warn-and-skip when `ion-c` is not found and `KYTHIRA_KCONFIG_STRICT` is unset
    - _Requirements: 7.1, 7.2, 7.5_

- [x] 3. Implement `ion-c` RAII wrappers and shared field helpers
  - [x] 3.1 Create `include/raft/ion_serializer.hpp` skeleton
    - `ion_encoding` enum; `ion_rpc_serializer<Data>` class template with the
      `requires serialized_data<Data>`-equivalent constraint
    - _Requirements: 1.3_
  - [x] 3.2 Implement `detail::ion_reader_handle`/`detail::ion_writer_handle` RAII
        wrappers
    - Constructors open via `ion_reader_open_buffer`/`ion_writer_open_buffer` (binary or
      text per `ion_encoding`); destructors call `ion_reader_close`/`ion_writer_close`
      unconditionally
    - _Requirements: 4.1, 4.2, 4.3, 5.6_
  - [x] 3.3 Implement `translate_ion_error(iERR, context)`
    - Maps every non-`IERR_OK` code to `kythira::serialization_exception` with a
      context-carrying message
    - _Requirements: 5.5_
  - [x] 3.4 Implement shared write helpers
    - `write_field_int`, `write_field_bool`, `write_field_blob` (native Ion `blob`, no
      base64), `write_log_entry`
    - _Requirements: 3.2, 3.3, 3.4_
  - [x] 3.5 Implement shared read helpers
    - `read_required_int64` (range-checked against target type), `read_required_bool`,
      `read_blob`, `read_log_entry`, `expect_annotation` (validates the single top-level
      annotation matches the expected message-type symbol)
    - _Requirements: 3.2, 3.3, 5.2, 5.3, 5.4_

- [x] 4. Implement `serialize`/`deserialize_*` for the core three RPCs
  - [x] 4.1 `request_vote_request`/`response`, `request_pre_vote_request`/`response`
    - Annotation-tagged struct per the Data Models field table; `NodeId = std::string`
      branch serialized as Ion `symbol`/`string`
    - _Requirements: 2.1, 2.2, 2.7, 3.1_
  - [x] 4.2 `append_entries_request`/`response`
    - `entries` as `list<struct>` with native-`blob` `command`; empty list serializes to
      an empty (not omitted) Ion list; `conflict_index`/`conflict_term` omitted when
      `std::nullopt`
    - _Requirements: 2.1, 2.2, 2.4, 2.5, 3.1, 3.2_
  - [x] 4.3 `install_snapshot_request`/`response`
    - `data` as native `blob`
    - _Requirements: 2.1, 2.2, 3.1, 3.2_

- [x] 5. Implement `serialize`/`deserialize_*` for the bootstrap and peer-replication
      extension RPCs
  - [x] 5.1 `cluster_join_request`/`response`, `cluster_leave_request`/`response`
    - Public-field access (not accessor methods) matching those types' actual shape;
      `redirect`/`PeerInfo`-equivalent struct omitted when absent
    - _Requirements: 2.1, 2.2, 2.6, 3.1_
  - [x] 5.2 `fetch_log_entries_request`/`response`
    - `entries` reuses the same nested-struct shape as `append_entries_request`
    - _Requirements: 2.1, 2.2, 2.4, 3.1_
  - [x] 5.3 Implement `deserialize<T>(data)` dispatch table
    - `if constexpr` chain over all fourteen default-templated types, mirroring
      `json_rpc_serializer::deserialize<T>`
    - _Requirements: 2.3_
  - [x] 5.4 Implement `name()`
    - Returns `"ion-binary"`/`"ion-text"` per the instance's `ion_encoding`
    - _Requirements: 1.4, 4.5_
  - [x] 5.5 `static_assert(rpc_serializer<ion_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>)`
    - _Requirements: 1.1_

- [x] 6. Transport format-detection integration
  - [x] 6.1 Extend `coap_utils::coap_content_format`/`get_content_format_for_serializer`
    - Add `application_ion = 65000`; match on `serializer_name.find("ion")` before the
      existing checks, single value for both encodings
    - _Requirements: 6.2, 6.3_
  - [x] 6.2 Add a `get_content_type_for_serializer` helper and use it in
        `cpp_httplib_client`/`server`
    - Replace the hardcoded `content_type_json` literal at send sites with the helper's
      result derived from `_serializer.name()`; verify `json_rpc_serializer` still
      produces `"application/json"` unchanged
    - _Requirements: 6.4_
  - [x] 6.3 Apply the same `Content-Type` change to
        `boost_beast_http_client`/`boost_beast_http_server`
    - _Requirements: 6.4_
  - [x] 6.4 Verify `simulator_network_client`/`server` require no changes
    - Confirm (does not transmit a Content-Type/Content-Format at all) they already work
      unmodified with `ion_rpc_serializer` substituted for `serializer_type`
    - _Requirements: 6.1_

- [x] 7. Unit and property-based testing
  - [x] 7.1 `tests/ion_serializer_concept_test.cpp`
    - Concept-conformance `static_assert`, basic serialize/deserialize smoke test for
      both encodings, `name()` distinguishability
    - _Requirements: 8.1_
  - [x] 7.2 `tests/ion_serialization_property_test.cpp`
    - **Property 1: Round-trip fidelity (per message type, per encoding)** — one test
      per message type from Requirement 2.1, both encodings, plus the `std::string`
      `NodeId` variant
    - **Validates: Requirements 2.4, 2.5, 2.6, 2.7, 4.2, 4.3, 8.2**
  - [x] 7.3 **Property 2: Binary-vs-text is transparent to the reader**
    - **Validates: Requirements 4.4, 8.5**
  - [x] 7.4 **Property 3: Native blob round-trips arbitrary bytes**
    - Include byte sequences with no valid JSON-string/base64-safe interpretation
    - **Validates: Requirements 3.2, 8.6**
  - [x] 7.5 `tests/ion_malformed_message_property_test.cpp`
    - **Property 4: Malformed input is rejected, never crashes** — random bytes, wrong
      annotation, missing required field, wrong Ion type per field, out-of-range
      numeric value, non-struct top-level value; reuse
      `tests/rpc_malformed_message_property_test.cpp`'s test-vector categories
    - **Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 8.3**
  - [x] 7.6 `tests/ion_json_serializer_equivalence_property_test.cpp`
    - **Property 5: Ion and JSON serializers agree semantically**
    - **Validates: Requirement 8.4**
  - [x] 7.7 Register all new test targets in `tests/CMakeLists.txt`, gated on
        `ionc_FOUND`
    - _Requirements: 7.1_

- [x] 8. Documentation
  - [x] 8.1 README updates
    - Add `ion_rpc_serializer` to the pluggable-serializer discussion alongside
      `json_rpc_serializer`; document the opt-in `ion` vcpkg feature and
      `ION_SERIALIZER` Kconfig symbol; note the `application/ion` Content-Type and the
      private-use CoAP Content-Format 65000
    - _Requirements: All (documentation only; not required for this spec itself)_
  - [x] 8.2 Example program demonstrating `ion_rpc_serializer` usage
    - Construct a `transport_types`/`raft_types` bundle with
      `serializer_type = ion_rpc_serializer<std::vector<std::byte>>`, both encodings,
      following this project's example-program guidelines
    - _Requirements: All_

- [x] 9. Final validation
  - [x] 9.1 Run the full test suite with `ION_SERIALIZER` enabled
    - All new unit and property tests compile and pass under CTest
    - _Requirements: 8.1-8.6_
  - [x] 9.2 Confirm graceful degradation with `ION_SERIALIZER` unset/`ion-c` absent
    - Rest of the project configures and builds unaffected; `json_rpc_serializer`-based
      targets are unaffected by the missing optional dependency
    - _Requirements: 7.1_
  - [x] 9.3 Confirm `KYTHIRA_KCONFIG_STRICT` failure path
    - Configuration fails loudly when `ION_SERIALIZER` is selected but `ion-c` is missing
    - _Requirements: 7.2_
  - [x] 9.4 End-to-end sanity check over HTTP and CoAP transports
    - A node pair configured with `ion_rpc_serializer` (both `cpp_httplib`/Beast HTTP and
      CoAP transports) completes a full RequestVote/AppendEntries/InstallSnapshot cycle,
      with the correct `Content-Type`/Content-Format observed on the wire
    - _Requirements: 6.1, 6.2, 6.3, 6.4_
    - New: `tests/ion_http_coap_end_to_end_test.cpp` — real client/server round
      trips over both `cpp_httplib` HTTP and CoAP (mirroring
      `coap_cbor_end_to_end_test.cpp`'s own cbor-rpc-serializer precedent, task
      10.3 there), plus a unit-level check that `ion_rpc_serializer::name()`
      (`"ion-binary"`/`"ion-text"`) maps to `application/ion` through both
      transports' shared `coap_utils`-based detection. Boost.Beast coverage
      not added here: this spec's own Requirement 6 scope predates
      `boost-beast-http-transport` existing as a second HTTP transport, and
      the `Content-Type` mapping Beast uses
      (`beast_content_type_for_serializer`) is the exact same
      `coap_utils::get_content_format_for_serializer` call `cpp_httplib`'s
      `content_type_for_serializer` already routes through — so the mapping
      itself is already covered by the same evidence, not a
      transport-specific gap. Adding a Beast-specific ion round-trip test
      would be a reasonable, but separate, follow-up if this spec's own
      Requirement 6 is ever revised to name Beast explicitly.

## Known Follow-ups

All 45 tasks are done. Nothing is open — the items below are a record of
what building against the *real* `ion-c` library (rather than the
`-fsyntax-only` API-stub validation the original PR shipped with, per
`doc/TODO.md`'s own "Pending Specifications" entry) actually found, kept for
context rather than as a to-do list.

### Round 1: bringing up a real `ion-c` build surfaced four genuine bugs, three of them upstream

The `ion` vcpkg feature had never actually been built before this round —
`vcpkg-overlays/ion-c/portfile.cmake`'s `SHA512` was a `0` placeholder (per
its own `README.md`, "the authoring environment had no outbound access to
GitHub archive downloads to compute it"), so `ion_rpc_serializer` had only
ever been validated against a hand-written `ion-c` API stub. Actually
installing `ion-c` (`vcpkg install --x-feature=ion`) and building the real
`ion_*` test targets against it surfaced four real, previously-latent bugs:

1. **Fixed: the `SHA512` placeholder itself.** Computed directly
   (`curl -sSL <url> | sha512sum`) rather than left for the next person to
   regenerate.
2. **Fixed, real upstream bug in `ion-c` itself: `cmake/VersionHeader.cmake`
   generates `build_version.h`'s version macros by parsing `git describe`,
   which has no fallback when there is no `.git` directory.**
   `vcpkg_from_github` extracts a plain source tarball, so `git describe`
   (run from inside the extracted tree) always fails there, the version
   regex never matches, and `IONC_VERSION_MAJOR`/`MINOR`/`PATCH` substitute
   as empty — which fails `ion_version.c`'s own build with "expected
   expression before ';' token" (`*major = ;`). Not a vcpkg-specific
   problem: any consumer building `ion-c` from a source archive rather than
   a git clone hits this. Fixed with
   `vcpkg-overlays/ion-c/0001-fix-version-header-without-git-describe.patch`
   (falls back to the version `project(IonC VERSION 1.1.3 ...)` already
   declares, passed through a new `-D IONC_FALLBACK_VERSION=...` argument
   since `VersionHeader.cmake` runs as its own separate `cmake -P` process
   with no `project()` call of its own).
3. **Fixed: `vcpkg_cmake_config_fixup(PACKAGE_NAME ionc ...)` only controls
   which `share/<name>` directory the config files land in, not the
   filenames themselves.** `ion-c`'s own build still names them
   `IonCConfig.cmake`/`IonCConfigVersion.cmake` (from `project(IonC ...)`);
   CMake's `find_package(ionc CONFIG)` looks for an exactly-cased
   `ioncConfig.cmake`, which a case-sensitive filesystem does not match
   against `IonCConfig.cmake` even in the right directory. Fixed with an
   explicit `file(RENAME ...)` in `portfile.cmake` after
   `vcpkg_cmake_config_fixup()`.
4. **Fixed: `raft_ion_serializer` (root `CMakeLists.txt`) didn't propagate
   `DECNUMDIGITS`, which `ion-c`'s own `ion_decimal.h` `#error`s without.**
   `ion-c`'s own `CMakeLists.txt` sets this directory-scoped via
   `add_definitions()`, which is never exported on `IonC::ionc`'s usage
   requirements, so any consumer (including this project's own tests)
   failed to build against the real headers with "DECNUMDIGITS must be
   defined to be >= DECQUAD_Pmax" until this project's own
   `raft_ion_serializer` INTERFACE target also defined it
   (`DECNUMDIGITS=34`, matching the value `vcpkg-overlays/ion-c` builds
   with, since its portfile doesn't override `IONC_DECIMAL_NUM_DIGITS`).

### Round 2: a genuine, serious upstream bug — `ion-c`'s own `ASSERT()` macro hangs forever under `NDEBUG`, discovered via this spec's own "never crashes" property test

With the real library finally building and linking, running
`tests/ion_malformed_message_property_test.cpp`'s
`property_truncated_message_rejected` case (Property 4: "malformed input is
rejected, never crashes" — Requirement 5) hung indefinitely at 100% CPU. A
gdb backtrace (the hung process started fresh under `gdb --args ...` and
interrupted, since this sandbox's `ptrace_scope` blocks attaching to an
already-running process) pinned it exactly: `ion-c`'s own
`ionc/ion_internal.h` defines
`ASSERT(x)` as `while (!(x)) { ion_helper_breakpoint(), assert(x); }`,
**unconditionally**. Under `NDEBUG` (this port's Release build, the
default), `assert(x)` compiles away entirely, so the `while` condition
never becomes false and a failed internal invariant — here,
`ion_reader_step_out()` on a container whose declared length was never
fully consumed because the caller's buffer was truncated mid-container,
exactly the case this test constructs — spins forever instead of either
aborting (a debug build) or being silently skipped (a release build,
matching plain `assert()`'s own `NDEBUG` semantics). This is a real hazard
for *any* consumer building `ion-c` in Release mode and feeding it
malformed/truncated input, not specific to this codebase's own reader code.
Fixed with
`vcpkg-overlays/ion-c/0002-fix-assert-infinite-loop-under-ndebug.patch`,
making `ASSERT(x)` a true no-op under `NDEBUG`. Two prior attempts at the
replacement macro each failed to even compile, for instructive reasons
recorded in the patch's own comment: a bare `((void)0)` broke on call sites
that invoke `ASSERT(x)` with no trailing semicolon (two adjacent
expansions parse as one calling the other), and `do {} while (0)` — the
usual "safe macro" idiom — has its *own* trailing semicolon as part of its
grammar, which those same call sites also don't provide. `while (0) {}`
(the same bare-`while` statement shape as the original macro, just with an
always-false condition) was the form that actually compiled everywhere.

### What this means for `KYTHIRA_KCONFIG_STRICT` builds and CI

None of the four fixes required touching `include/raft/ion_serializer.hpp`
itself — every one was in the `ion-c` overlay port or this project's own
CMake wiring around it. No CI job currently builds with the `ion` vcpkg
feature enabled (it remains fully opt-in, per Requirement 7), so none of
this was previously exercised by CI; it was only found by actually
installing `ion-c` and building against it directly in this round.
