# Design Document — CoAP Transport (libnyoci backend)

## Overview

`coap_libnyoci_client<Types>` / `coap_libnyoci_server<Types>`
(`include/raft/coap_transport_libnyoci_impl.hpp`) are a second implementation of
the `kythira::network_client` / `kythira::network_server` concepts
(`include/raft/network.hpp`), sitting alongside the libcoap-backed
`coap_client`/`coap_server` (`include/raft/coap_transport.hpp`). They speak the
same CoAP protocol over the wire but delegate the CoAP mechanics to
**libnyoci** rather than libcoap.

Because every consumer of the transport (the Raft layer, the network concepts)
only calls `send_*` / `register_*_handler` / `start` / `stop` / `is_running`,
the libnyoci backend is a **drop-in alternative**: no Raft-layer code changes
shape. The work is confined to (a) an autotools overlay port and (b) the
adapter that bridges libnyoci's C, callback-driven API to kythira's
`future_template<...>` promise model.

### Key design decision: bridge, don't build

libnyoci is a *full* CoAP stack. The adapter therefore does **not** implement
sockets, retransmission, duplicate detection or block-wise transfer — libnyoci
owns all of it. The adapter's entire job is translation:

- **outbound:** serialize an RPC → start a `nyoci_transaction` (POST to the
  target's resource) → return a future whose promise is fulfilled from
  libnyoci's response callback.
- **inbound:** register a libnyoci request handler per RPC resource →
  deserialize → invoke the stored `std::function` handler → emit the response.

This is the defining contrast with the cantcoap backend
(`.kiro/specs/coap-transport-cantcoap/`), where the adapter must build all of
the above by hand. Here the adapter is thin; the *port* is the expensive part.

### Key design decision: security stays in kythira, not libnyoci

libnyoci provides DTLS only through an external ssl plugin and ships **no
OSCORE and no EDHOC**. kythira already owns OSCORE/EDHOC via
`coap_security.hpp` / `coap_edhoc.hpp`. Forking security per backend would be a
maintenance trap, so the default design keeps kythira's `coap_security_provider`
*above* a plain-CoAP libnyoci core: decrypt inbound datagrams before handing
them to libnyoci-produced PDUs and encrypt outbound ones after. libnyoci's own
ssl plugin is used only if a concrete reason emerges, and that reason is
documented.

### Non-goal: replacing libcoap

This backend is additive. libcoap remains the default; `coap-libnyoci` is an
opt-in feature. The value is proving the transport is library-agnostic and
giving operators a second, differently-licensed (MIT) stack.

## Architecture

### Component diagram

```
        Raft layer  (unchanged; depends only on network_client/server concepts)
             │
   ┌─────────┴──────────────────────────────────────┐
   │  coap_libnyoci_client<Types>  /  ..._server     │  include/raft/
   │  (adapter: RPC <-> nyoci transaction/handler)   │  coap_transport_libnyoci_impl.hpp
   └─────────┬───────────────────────┬───────────────┘
             │ serialize/deserialize │ security (reused)
      Types::serializer_type    coap_security_provider / coap_edhoc
             │                       │
   ┌─────────┴───────────────────────┴───────────────┐
   │              libnyoci  (nyoci_t)                 │  vcpkg-overlays/libnyoci/
   │  socket · token/retransmit · block-wise · async  │  (autotools port)
   └──────────────────────────────────────────────────┘
```

### Send flow (example: `send_append_entries`)

1. `send_rpc<append_entries_request, append_entries_response>(target, "/raft/ae", req, timeout)`.
2. Look up the target endpoint; `Types::serializer_type::serialize(req)` → bytes.
3. (If secure) encrypt via `coap_security_provider`.
4. Build a libnyoci outbound POST to the resource, attach a token, and
   `nyoci_transaction_begin` with `on_response` and a heap-allocated context
   holding the promise's resolve/reject callbacks (a `pending_message`, reused
   from `coap_transport.hpp`).
5. Return `future_template<append_entries_response>`; the process-loop thread
   drives libnyoci until the response callback fulfils (or rejects) the promise.

### Receive flow

1. libnyoci invokes the registered inbound handler for the resource on the
   process-loop thread.
2. Read the payload; (if secure) decrypt; `deserialize<append_entries_request>()`.
3. Invoke the stored `ae_handler_` (guarded by `mutex_`).
4. Serialize the response and emit it via libnyoci's outbound-response API,
   echoing the request token.

## Components and Interfaces

### `coap_libnyoci_client<Types>` (`requires transport_types<Types>`)

- `future_template<T>` / `metrics_type` aliases taken from `Types`, identical to `coap_client`.
- Public: `send_request_vote` / `send_append_entries` / `send_install_snapshot`.
- Private: `send_rpc<Request,Response>`, a single `nyoci_t` on a `std::jthread`,
  the C `on_response` trampoline, and the reused `pending_message` context type.

### `coap_libnyoci_server<Types>` (`requires transport_types<Types>`)

- Public: `register_*_handler`, `start`, `stop`, `is_running`.
- Private: `nyoci_t` + resource registrations, stored handler `std::function`s,
  `std::jthread` process loop, `std::atomic<bool> running_`, `std::mutex`.

## Error Handling

Reuses the existing `coap_exceptions.hpp` hierarchy. libnyoci status codes are
translated at the callback boundary into the same exception types the libcoap
path throws, so callers observe identical failure semantics. A transaction that
libnyoci reports as timed-out/exhausted rejects its future; a malformed inbound
PDU is dropped without invoking a handler.

## Testing Strategy

- **Concept conformance** (`tests/coap_libnyoci_concept_conformance_test.cpp`):
  `static_assert` the two concepts, mirroring `coap_concept_conformance_test.cpp`.
- **End-to-end** (`tests/coap_libnyoci_integration_test.cpp`): loopback
  client↔server round trip for all three RPCs, plus a block-wise-sized payload.
- **Security** reuses the existing `coap_security_*` tests parametrized over the backend.
- All libnyoci-labeled tests are individually skipped when `LIBNYOCI_AVAILABLE`
  is undefined, exactly as the Folly/libcoap-specific tests are.
- Any container harness obeys the Docker/rootless-Podman rules in `CLAUDE.md`.

## Dependencies

### External

- **libnyoci** (MIT), built from source via `vcpkg-overlays/libnyoci/`. Host
  build tools required: `autoconf`, `automake`, `libtool`, `pkg-config`.

### Internal

- `include/raft/network.hpp` (concepts), `coap_transport.hpp` (shared configs /
  `pending_message` / `block_transfer_state`), `coap_security.hpp` /
  `coap_edhoc.hpp` (security), `Types::serializer_type` (encoding).

## Build Integration

### `vcpkg-overlays/libnyoci/`

Autotools port: `vcpkg_from_github` (pinned `REF`/`SHA512`) →
`vcpkg_configure_make AUTOCONFIG` (with `--disable-tests --without-examples
--enable-static`) → `vcpkg_install_make` → `vcpkg_fixup_pkgconfig`. No
`vcpkg_cmake_config_fixup` (there is no CMake config package).

### Root `vcpkg.json`

Opt-in feature `coap-libnyoci` depending on `libnyoci`, mirroring how `edhoc`/`ion`
gate `lakers`/`ion-c`.

### Root `CMakeLists.txt`

Next to the `COAP_TRANSPORT` block: `pkg_check_modules(LIBNYOCI QUIET libnyoci)`;
when found, link `${LIBNYOCI_LIBRARIES}`, add `${LIBNYOCI_INCLUDE_DIRS}`, and
define `LIBNYOCI_AVAILABLE` — structurally identical to the `LIBCOAP_FOUND` →
`LIBCOAP_AVAILABLE` wiring. Discovery is pkg-config (the system-libcoap fallback
path), *not* `find_package(... CONFIG)`.

See `doc/coap_library_alternatives.md` for the exact snippets and the
libnyoci-vs-cantcoap trade-off.
