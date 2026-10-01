# Implementation Plan

## Implementation Status

Not started. This spec depends on PR #368 (plaintext Raft RPC opt-in) and
PR #369 (shared `net_bind` helpers) being merged.

## Overview

Add the `allow_plaintext` gate to the gRPC transport's server and client,
using the project's existing loopback rule, plus tests and docs.

## Tasks

- [ ] 1. Error type
  - [ ] 1.1 Add a protected `(StatusCode, message)` constructor to
    `grpc_tls_configuration_error`
  - [ ] 1.2 Add `grpc_plaintext_refused_error` with `address()`
  - _Requirements: 4.1, 4.2_

- [ ] 2. Configuration
  - [ ] 2.1 Add `allow_plaintext{false}` to `grpc_server_config` and
    `grpc_client_config`, with doc comments
  - _Requirements: 1.1, 1.2, 1.4_

- [ ] 3. Target classification
  - [ ] 3.1 Implement `grpc_detail::target_is_local()` per the design table,
    built on `net_bind::is_loopback_bind_address()`
  - [ ] 3.2 Write `tests/grpc_target_classification_unit_test.cpp` and
    register it in `tests/CMakeLists.txt`
  - _Requirements: 3.3, 3.4_

- [ ] 4. Server gate
  - [ ] 4.1 Refuse plaintext off loopback in `build_server_credentials()`
  - [ ] 4.2 Emit `grpc.server.plaintext.enabled` from `start()` when the
    opt-in admitted the listener
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 1.3_

- [ ] 5. Client gate
  - [ ] 5.1 Check every configured target in the constructor
  - [ ] 5.2 Check the address-keyed `get_or_create_channel()` before
    creating a channel
  - [ ] 5.3 Emit `grpc.client.plaintext.enabled` when opted in
  - _Requirements: 3.1, 3.2, 3.5_

- [ ] 6. Integration tests
  - [ ] 6.1 Add the eight cases from the design's Testing Strategy to
    `tests/grpc_transport_integration_test.cpp`
  - [ ] 6.2 Sweep existing tests, examples and benchmarks for non-loopback
    plaintext gRPC binds and add `allow_plaintext = true` with a comment
  - _Requirements: 5.1_

- [ ] 7. Documentation
  - [ ] 7.1 Document the rule in `doc/grpc_transport_README.md`
  - [ ] 7.2 Add a pointer from grpc-transport Requirement 9.7 to this spec
  - _Requirements: 5.2, 5.3_

## Notes

- The HTTP transports and CoAP keep their plaintext defaults. Each can
  adopt this pattern in its own spec.
