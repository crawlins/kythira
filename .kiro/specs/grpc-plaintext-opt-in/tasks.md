# Implementation Plan

## Implementation Status

Implemented. Where the code differs from the design text:

- `grpc_detail::target_is_local()` lives in its own header,
  `include/raft/grpc_target.hpp`, which needs nothing from gRPC. Its unit
  test is registered outside the `raft_grpc_transport` gate, so it runs on
  every build leg. It takes a `hosts_path` for that test, and
  `net_bind::is_loopback_bind_address()` gained the same defaulted parameter.
- The IPv4-mapped `::ffff:127.x.y.z` counts as loopback for client targets
  only. Bind addresses keep the existing rule (not loopback), which
  `tcp_rpc_unit_test` pins and an `IPV6_V6ONLY` listener needs.
- `grpc.client.plaintext.enabled` carries a `loopback_only` dimension, to
  match the server's.
- Testing Strategy case 1 checks that the refusal happens in the
  constructor rather than that the port is free afterwards: `start()` is the
  only place a listener is opened, and a port probe would reintroduce the
  port-reuse race `make_server()` documents.
- The sweep (6.2) found two non-loopback plaintext binds: the `"*"` case of
  `localhost_and_ipv6_binds_end_to_end`, and `unlisted_bind_name_is_refused`,
  whose unlisted name the gate would otherwise refuse in the constructor
  before `start()` reaches the name check. Every example and benchmark
  binds and dials `127.0.0.1`.

## Overview

Add the `allow_plaintext` gate to the gRPC transport's server and client,
using the project's existing loopback rule, plus tests and docs.

## Tasks

- [x] 1. Error type
  - [x] 1.1 Add a protected `(StatusCode, message)` constructor to
    `grpc_tls_configuration_error`
  - [x] 1.2 Add `grpc_plaintext_refused_error` with `address()`
  - _Requirements: 4.1, 4.2_

- [x] 2. Configuration
  - [x] 2.1 Add `allow_plaintext{false}` to `grpc_server_config` and
    `grpc_client_config`, with doc comments
  - _Requirements: 1.1, 1.2, 1.4_

- [x] 3. Target classification
  - [x] 3.1 Implement `grpc_detail::target_is_local()` per the design table,
    built on `net_bind::is_loopback_bind_address()`
  - [x] 3.2 Write `tests/grpc_target_classification_unit_test.cpp` and
    register it in `tests/CMakeLists.txt`
  - _Requirements: 3.3, 3.4_

- [x] 4. Server gate
  - [x] 4.1 Refuse plaintext off loopback in `build_server_credentials()`
  - [x] 4.2 Emit `grpc.server.plaintext.enabled` from `start()` when the
    opt-in admitted the listener
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 1.3_

- [x] 5. Client gate
  - [x] 5.1 Check every configured target in the constructor
  - [x] 5.2 Check the address-keyed `get_or_create_channel()` before
    creating a channel
  - [x] 5.3 Emit `grpc.client.plaintext.enabled` when opted in
  - _Requirements: 3.1, 3.2, 3.5_

- [x] 6. Integration tests
  - [x] 6.1 Add the eight cases from the design's Testing Strategy to
    `tests/grpc_transport_integration_test.cpp`
  - [x] 6.2 Sweep existing tests, examples and benchmarks for non-loopback
    plaintext gRPC binds and add `allow_plaintext = true` with a comment
  - _Requirements: 5.1_

- [x] 7. Documentation
  - [x] 7.1 Document the rule in `doc/grpc_transport_README.md`
  - [x] 7.2 Add a pointer from grpc-transport Requirement 9.7 to this spec
  - _Requirements: 5.2, 5.3_

## Notes

- The HTTP transports and CoAP keep their plaintext defaults. Each can
  adopt this pattern in its own spec.
