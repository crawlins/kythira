# CoAP Transport (libnyoci backend) Requirements Document

## Introduction

This document specifies the requirements for a **second, alternate backend**
for the existing CoAP transport (`.kiro/specs/coap-transport/`), built on
**libnyoci** ([darconeous/libnyoci][repo], formerly SMCP) instead of libcoap.
libnyoci is a full-stack RFC 7252 client/server library in C: it owns the UDP
socket, the message/token layer, retransmission of confirmable messages,
observe, async responses and block-wise transfer. The goal is a transport that
satisfies the *same* `network_client`/`network_server` concepts
(`include/raft/network.hpp`) as the libcoap-backed `coap_client`/`coap_server`,
so the Raft layer consumes it interchangeably, while validating that the CoAP
transport is not permanently coupled to a single library.

This is the companion spec to `.kiro/specs/coap-transport-cantcoap/`; the two
sit at opposite ends of the port-vs-adapter cost curve, and
`doc/coap_library_alternatives.md` carries the side-by-side comparison.

## Glossary

- **Libnyoci_Backend**: the libnyoci-backed implementation of the CoAP transport
- **Nyoci_Interface**: a libnyoci `nyoci_t` instance — one CoAP endpoint owning a socket and its transactions
- **Nyoci_Transaction**: a libnyoci outbound request whose response is delivered through a C callback
- **Adapter**: the C++ layer translating kythira RPCs to/from libnyoci and bridging its callbacks into `future_template<...>` promises
- **LIBNYOCI_AVAILABLE**: the build macro gating the backend, mirroring `LIBCOAP_AVAILABLE`
- **Security_Layer**: kythira's existing `coap_security_provider` / `coap_edhoc` (DTLS, OSCORE, EDHOC), reused rather than delegated to libnyoci

## Requirements

### Requirement 1 — Concept-compatible transport

**User Story:** As a Raft integrator, I want the libnyoci backend to satisfy the
same network concepts as the libcoap transport, so I can select it without
changing any consensus-layer code.

#### Acceptance Criteria

1. WHEN the client type is instantiated THEN the Libnyoci_Backend SHALL satisfy `kythira::network_client` (`send_request_vote`/`send_append_entries`/`send_install_snapshot` returning the correct `future_template<...>`).
2. WHEN the server type is instantiated THEN the Libnyoci_Backend SHALL satisfy `kythira::network_server` (`register_*_handler`, `start`, `stop`, `is_running`).
3. WHEN the client and server types are declared THEN they SHALL be templated on `Types` and constrained by `kythira::transport_types<Types>`, exactly as `coap_client`/`coap_server` are.
4. WHEN a `static_assert(network_client<...>)` / `static_assert(network_server<...>)` is compiled against the backend THEN it SHALL pass.
5. WHEN the backend is used with any existing `Types::serializer_type` (JSON, CBOR, Ion) THEN it SHALL require no serializer changes — the serializer is a pure substitution.

### Requirement 2 — Optional, gracefully-degrading dependency

**User Story:** As a build maintainer, I want libnyoci to be opt-in and absent
by default, so a normal build is unaffected and never fetches it.

#### Acceptance Criteria

1. WHEN the `coap-libnyoci` vcpkg feature is not selected THEN the build SHALL NOT fetch, build, or link libnyoci.
2. WHEN libnyoci is absent THEN the adapter SHALL compile to a stub and the build SHALL emit a warning, never a hard error (except under `KYTHIRA_KCONFIG_STRICT`), matching the `LIBCOAP_FOUND` degradation pattern.
3. WHEN libnyoci is present THEN the build SHALL define `LIBNYOCI_AVAILABLE` project-wide and link the discovered library.
4. WHEN both libcoap and libnyoci are available THEN selecting the backend SHALL be a configuration choice; the two SHALL NOT conflict at link time.
5. WHEN the overlay port is built THEN it SHALL pin an exact `REF` and verified `SHA512`, regenerated per the port README.

### Requirement 3 — Autotools overlay port

**User Story:** As a build maintainer, I want libnyoci packaged through a vcpkg
overlay, so it is fetched and built reproducibly like ion-c and lakers.

#### Acceptance Criteria

1. WHEN the overlay port is invoked THEN it SHALL build libnyoci from source with `vcpkg_configure_make` + `vcpkg_install_make` (libnyoci is autotools, not CMake).
2. WHEN building from a GitHub archive tarball THEN the port SHALL re-run `autoreconf` (`AUTOCONFIG`), since the archive omits the generated `./configure`.
3. WHEN the port completes THEN it SHALL install a `libnyoci.pc` fixed up via `vcpkg_fixup_pkgconfig`, so downstream discovery uses `pkg_check_modules`.
4. WHEN the port builds THEN it SHALL disable libnyoci's own tests, CLI (`nyocictl`) and example daemons.
5. WHEN the port installs THEN it SHALL install the project `LICENSE` via `vcpkg_install_copyright`.

### Requirement 4 — Reliable delivery via libnyoci's own machinery

**User Story:** As a Raft node, I want confirmable-message reliability, so
consensus messages reach their destination.

#### Acceptance Criteria

1. WHEN a confirmable RPC is sent THEN the Libnyoci_Backend SHALL rely on libnyoci's transaction layer for retransmission and acknowledgment, not reimplement it.
2. WHEN a response arrives THEN the adapter SHALL resolve exactly the one pending `future_template<Response>` correlated by token, and no other.
3. WHEN a transaction exhausts retransmissions or times out THEN the adapter SHALL reject the corresponding future with a descriptive `coap_*` exception.
4. WHEN duplicate responses arrive THEN libnyoci's duplicate handling SHALL suppress them and the future SHALL resolve at most once.
5. WHEN a large payload is sent or received THEN the backend SHALL use libnyoci's block-wise transfer transparently to the caller.

### Requirement 5 — Security via the existing layer

**User Story:** As a security-conscious operator, I want DTLS/OSCORE/EDHOC to
behave identically regardless of CoAP backend, so security is not forked.

#### Acceptance Criteria

1. WHEN DTLS is enabled THEN the backend SHALL use kythira's `coap_security_provider` rather than diverging into a libnyoci-specific configuration path, unless a documented reason requires libnyoci's ssl plugin.
2. WHEN OSCORE or EDHOC is configured THEN the backend SHALL reuse `coap_edhoc`/`coap_security` exactly as the libcoap path does (libnyoci ships neither).
3. WHEN security is disabled THEN the backend SHALL operate over plain CoAP.
4. WHEN certificate or key material is invalid THEN the backend SHALL surface the same validation errors as the libcoap path.

### Requirement 6 — Threading and lifecycle

**User Story:** As an integrator, I want deterministic startup/shutdown, so the
transport releases sockets and threads cleanly.

#### Acceptance Criteria

1. WHEN the client or server is constructed THEN it SHALL create one `Nyoci_Interface` and drive it on a dedicated background thread (`std::jthread`) running libnyoci's process loop until stop.
2. WHEN `stop()` is called THEN the server SHALL halt the process loop, close the socket, and join the thread without leaking resources.
3. WHEN handlers are registered or invoked concurrently with the process loop THEN access to shared state SHALL be synchronized.
4. WHEN the object is destroyed THEN any in-flight transactions SHALL be cancelled and their futures rejected, never left unresolved.

### Requirement 7 — Testing parity

**User Story:** As a maintainer, I want the libnyoci backend held to the same
test bar as libcoap, so it is trustworthy.

#### Acceptance Criteria

1. WHEN the backend is tested THEN there SHALL be a concept-conformance test asserting `network_client`/`network_server`.
2. WHEN the backend is tested THEN there SHALL be an end-to-end RequestVote/AppendEntries/InstallSnapshot round trip over a real loopback socket.
3. WHEN large messages are tested THEN block-wise transfer SHALL be exercised end to end.
4. WHEN any container-based harness is added THEN it SHALL follow the project's Docker/rootless-Podman compatibility rules (no static IPs, `container_runtime()`, no `--privileged`).
5. WHEN libnyoci is absent THEN the backend's tests SHALL be individually skipped, not fail the build.

[repo]: https://github.com/darconeous/libnyoci
