# CoAP Transport (cantcoap backend) Requirements Document

## Introduction

This document specifies the requirements for a **second, alternate backend**
for the existing CoAP transport (`.kiro/specs/coap-transport/`), built on
**cantcoap** ([staropram/cantcoap][repo]) instead of libcoap. cantcoap is a
minimal, BSD-2-licensed CoAP **message codec**: it constructs and parses RFC
7252 PDUs and nothing else — no sockets, no retransmission, no duplicate
detection, no block-wise state machine, no DTLS. The goal is a transport that
satisfies the *same* `network_client`/`network_server` concepts
(`include/raft/network.hpp`) as the libcoap-backed transport, while kythira
**owns the entire transport stack** around the codec, reusing its existing
retransmission, block-wise and security scaffolding.

This is the companion spec to `.kiro/specs/coap-transport-libnyoci/`; the two
sit at opposite ends of the port-vs-adapter cost curve, and
`doc/coap_library_alternatives.md` carries the side-by-side comparison.

## Glossary

- **Cantcoap_Backend**: the cantcoap-backed implementation of the CoAP transport
- **CoapPDU**: cantcoap's single-message build/parse type — the *only* thing cantcoap provides
- **Adapter**: the C++ transport built around `CoapPDU` (socket, timers, reliability, block-wise, security)
- **CANTCOAP_AVAILABLE**: the build macro gating the backend, mirroring `LIBCOAP_AVAILABLE`
- **Reliability_Layer**: retransmission + duplicate detection, implemented here using the existing `pending_message` / `received_message_info` types
- **Security_Layer**: kythira's existing `coap_security_provider` / `coap_edhoc`, wrapping the socket

## Requirements

### Requirement 1 — Concept-compatible transport

**User Story:** As a Raft integrator, I want the cantcoap backend to satisfy the
same network concepts as the libcoap transport, so I can select it without
changing consensus-layer code.

#### Acceptance Criteria

1. WHEN the client type is instantiated THEN the Cantcoap_Backend SHALL satisfy `kythira::network_client`.
2. WHEN the server type is instantiated THEN the Cantcoap_Backend SHALL satisfy `kythira::network_server`.
3. WHEN the client and server types are declared THEN they SHALL be templated on `Types` and constrained by `kythira::transport_types<Types>`.
4. WHEN a `static_assert(network_client<...>)` / `static_assert(network_server<...>)` is compiled THEN it SHALL pass.
5. WHEN the backend is used with any existing `Types::serializer_type` THEN it SHALL require no serializer changes.

### Requirement 2 — Optional, gracefully-degrading dependency

**User Story:** As a build maintainer, I want cantcoap opt-in and absent by
default, so a normal build is unaffected.

#### Acceptance Criteria

1. WHEN the `coap-cantcoap` vcpkg feature is not selected THEN the build SHALL NOT fetch, build, or link cantcoap.
2. WHEN cantcoap is absent THEN the adapter SHALL compile to a stub and the build SHALL warn, never hard-error (except under `KYTHIRA_KCONFIG_STRICT`).
3. WHEN cantcoap is present THEN the build SHALL define `CANTCOAP_AVAILABLE` and link `cantcoap::cantcoap`.
4. WHEN both libcoap and cantcoap are available THEN backend selection SHALL be a configuration choice with no link conflict.

### Requirement 3 — Vendored-build overlay port

**User Story:** As a build maintainer, I want cantcoap packaged through a vcpkg
overlay despite it shipping no build system.

#### Acceptance Criteria

1. WHEN the overlay port is invoked THEN it SHALL vendor a `CMakeLists.txt` into the source tree (cantcoap ships only source files and a test-only Makefile) and drive it with `vcpkg_cmake_configure`/`vcpkg_cmake_install`.
2. WHEN the port completes THEN it SHALL export a `cantcoap::cantcoap` imported target discoverable via `find_package(cantcoap CONFIG)`.
3. WHEN the vendored build compiles THEN its source/header list SHALL be reconfirmed against the pinned commit (upstream is unversioned).
4. WHEN the port is pinned THEN it SHALL use a specific commit SHA (not a moving branch) with a verified `SHA512`.
5. WHEN the port installs THEN it SHALL install the `LICENSE` via `vcpkg_install_copyright`.

### Requirement 4 — Reliability layer (built here, not by cantcoap)

**User Story:** As a Raft node, I want confirmable-message reliability even
though cantcoap provides none, so consensus messages are delivered.

#### Acceptance Criteria

1. WHEN a confirmable RPC is sent THEN the Cantcoap_Backend SHALL track it in a `pending_message` and retransmit on timeout using CoAP's exponential backoff (ACK_TIMEOUT × ACK_RANDOM_FACTOR, doubling), reusing the existing `pending_message` type.
2. WHEN `MAX_RETRANSMIT` is exceeded THEN the backend SHALL reject the correlated future with a descriptive `coap_*` exception.
3. WHEN a response arrives THEN the backend SHALL correlate it by token to exactly one pending future and resolve it.
4. WHEN a duplicate message arrives THEN the backend SHALL detect it via `Message_ID` (using `received_message_info`) and discard it.
5. WHEN a non-confirmable message is sent THEN the backend SHALL transmit without tracking an acknowledgment.

### Requirement 5 — Block-wise transfer (built here)

**User Story:** As an integrator, I want large payloads to transfer, even though
cantcoap only encodes single messages.

#### Acceptance Criteria

1. WHEN a payload exceeds the block size THEN the backend SHALL split it and drive a `block_transfer_state` reassembly using the Block1/Block2 helpers in `coap_block_option.hpp`.
2. WHEN block options are read/written THEN the backend SHALL use `CoapPDU`'s option API for the encoding and its own state machine for sequencing.
3. WHEN a block transfer stalls THEN the backend SHALL time it out and reject the associated future.

### Requirement 6 — Security layer (wrapping the socket)

**User Story:** As a security-conscious operator, I want DTLS/OSCORE/EDHOC to
behave identically to the libcoap path, since cantcoap is cleartext-only.

#### Acceptance Criteria

1. WHEN DTLS is enabled THEN the backend SHALL wrap its UDP socket with the existing `coap_security_provider` — decrypt inbound datagrams before `CoapPDU` parsing and encrypt outbound `CoapPDU` bytes before `sendto`.
2. WHEN OSCORE or EDHOC is configured THEN the backend SHALL reuse `coap_edhoc`/`coap_security` exactly as the libcoap path (cantcoap ships neither).
3. WHEN security is disabled THEN the backend SHALL operate over plain UDP.
4. WHEN a datagram fails decryption or `CoapPDU::validate()` THEN the backend SHALL drop it without invoking a handler and without crashing.

### Requirement 7 — Sockets, threading, lifecycle (built here)

**User Story:** As an integrator, I want the backend to own its socket and
threads cleanly, since cantcoap owns none of this.

#### Acceptance Criteria

1. WHEN a client or server is constructed THEN it SHALL open and bind a UDP socket and run a combined RX + retransmission-timer loop on a `std::jthread`.
2. WHEN a datagram is received THEN the loop SHALL parse it with `CoapPDU`, correlate by token, and dispatch to the pending future or the registered handler.
3. WHEN `stop()` is called THEN the backend SHALL end the loop, close the socket, join the thread, and reject any in-flight futures.
4. WHEN shared state (pending map, seen map, handlers) is accessed from the loop and the caller THEN access SHALL be synchronized.

### Requirement 8 — Testing parity

**User Story:** As a maintainer, I want the cantcoap backend held to the same
test bar as libcoap, given it carries more hand-written logic.

#### Acceptance Criteria

1. WHEN the backend is tested THEN there SHALL be a concept-conformance test asserting both concepts.
2. WHEN the backend is tested THEN there SHALL be an end-to-end round trip over a real loopback socket for all three RPCs.
3. WHEN reliability is tested THEN retransmission, duplicate suppression, and max-retransmit failure SHALL each have property-based coverage, mirroring the existing `coap_*_property_test.cpp` suite.
4. WHEN block-wise transfer is tested THEN a payload larger than one block SHALL round-trip.
5. WHEN a malformed or undecryptable datagram is injected THEN the backend SHALL drop it and stay responsive (fuzz/property test).
6. WHEN any container harness is added THEN it SHALL follow the Docker/rootless-Podman rules in `CLAUDE.md`.
7. WHEN cantcoap is absent THEN the backend's tests SHALL be individually skipped, not fail the build.

[repo]: https://github.com/staropram/cantcoap
