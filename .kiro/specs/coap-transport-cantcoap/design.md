# Design Document — CoAP Transport (cantcoap backend)

## Overview

`coap_cantcoap_client<Types>` / `coap_cantcoap_server<Types>`
(`include/raft/coap_transport_cantcoap_impl.hpp`) are a second implementation of
the `kythira::network_client` / `kythira::network_server` concepts
(`include/raft/network.hpp`), sitting alongside the libcoap-backed transport.
They speak CoAP on the wire, but where the libcoap and libnyoci backends
delegate the protocol to a full library, this backend delegates only the
**message encoding** to cantcoap (`CoapPDU`) and implements everything else —
socket, reliability, block-wise, security — itself.

To the Raft layer it is a drop-in alternative: the consensus code only calls
`send_*` / `register_*_handler` / `start` / `stop` / `is_running`, so nothing
above the transport changes shape. The cost lands entirely inside the adapter.

### Key design decision: own the stack, reuse kythira's scaffolding

cantcoap is a codec, so the adapter must supply the transport. Crucially, most
of that machinery **already exists in kythira** and is reused rather than
rewritten:

| Concern | cantcoap provides | Reused kythira scaffolding |
|---|---|---|
| PDU encode/parse | `CoapPDU` | — |
| UDP socket + loop | nothing | new (thin: `socket`/`recvfrom`/`sendto` + timer) |
| Retransmit / dedup | nothing | `pending_message`, `received_message_info` (`coap_transport.hpp`) |
| Block-wise | nothing | `block_transfer_state` + `coap_block_option.hpp` |
| DTLS / OSCORE / EDHOC | nothing | `coap_security_provider`, `coap_edhoc` |

So "own the stack" is really "own the socket + timer loop and wire up the
existing pieces" — not "reimplement CoAP from scratch." This is the defining
contrast with the libnyoci backend, where the library owns all of the above and
the adapter is thin.

### Key design decision: security wraps the socket, not the codec

cantcoap is cleartext-only. The `coap_security_provider` is inserted between the
socket and `CoapPDU`: inbound datagrams are decrypted before parsing, outbound
`CoapPDU::getPDUPointer()` bytes are encrypted before `sendto`. OSCORE/EDHOC
stay in `coap_edhoc.hpp` exactly as on the libcoap path, so the security story
does not fork per backend.

### Non-goal: reimplementing CoAP semantics cantcoap can't express

cantcoap covers RFC 7252 message format and options. Anything genuinely beyond
message encoding that kythira does not already have (e.g. Observe) is out of
scope for the first cut; the backend targets the same request/response +
block-wise surface the Raft transport actually uses.

## Architecture

### Component diagram

```
        Raft layer  (unchanged; depends only on network_client/server concepts)
             │
   ┌─────────┴───────────────────────────────────────────────┐
   │  coap_cantcoap_client<Types> / ..._server                │  include/raft/
   │  ┌───────────────────────────────────────────────────┐  │  coap_transport_cantcoap_impl.hpp
   │  │ reliability: pending_message / received_message_info│  │
   │  │ block-wise : block_transfer_state / coap_block_opt  │  │  (reused)
   │  │ security   : coap_security_provider / coap_edhoc    │  │  (reused)
   │  │ socket+loop: UDP fd + RX/timer std::jthread         │  │  (new, thin)
   │  └───────────────────────┬───────────────────────────┘  │
   │        encode/parse       │                              │
   │                       CoapPDU (cantcoap)                 │  vcpkg-overlays/cantcoap/
   └──────────────────────────────────────────────────────────┘  (vendored CMakeLists)
```

### Send flow (example: `send_append_entries`, confirmable)

1. `serialize(req)` → bytes; assign a token and a 16-bit Message_ID.
2. Build the PDU: `CoapPDU` with version 1, type CONFIRMABLE, code POST, token,
   Message_ID, URI = `/raft/ae`, payload = bytes (split into Block1 blocks if oversized).
3. Record a `pending_message` (token, timeout, resolve/reject callbacks) in `pending_`.
4. (If secure) encrypt `pdu.getPDUPointer()`; `sendto` on `fd_`.
5. Return `future_template<append_entries_response>`. The loop retransmits on
   timeout (exponential backoff) until an ACK/response arrives or MAX_RETRANSMIT.

### Receive flow (RX + timer loop, on `std::jthread`)

1. `recvfrom` → (if secure) decrypt → `CoapPDU in(buf, n); if (in.validate()!=1) drop`.
2. Duplicate check on `in.getMessageID()` via `received_message_info`; drop if seen.
3. If it correlates by token to a `pending_message` → resolve that future
   (reassembling Block2 into `block_transfer_state` if blocked).
4. Else route by `in.getURI()` to the registered handler → deserialize → invoke
   `std::function` (guarded) → build a response PDU echoing token/Message_ID →
   encrypt → `sendto` to the peer address from `recvfrom`.
5. On each tick, retransmit due `pending_` entries; expire stalled block transfers.

## Components and Interfaces

### `coap_cantcoap_client<Types>` (`requires transport_types<Types>`)

- `future_template<T>` / `metrics_type` from `Types`, as in `coap_client`.
- Public: the three `send_*` methods. Private: `send_rpc<Request,Response>`,
  `fd_`, `pending_`, `seen_`, `block_transfers_`, `security_`, `next_message_id()`,
  and the RX/timer `std::jthread`.

### `coap_cantcoap_server<Types>` (`requires transport_types<Types>`)

- Public: `register_*_handler`, `start`, `stop`, `is_running`. Private: `fd_`,
  `seen_`, `security_`, stored handler `std::function`s, RX loop `std::jthread`,
  `running_`, `mutex_`.

## Error Handling

Reuses `coap_exceptions.hpp`. Retransmission exhaustion, block-transfer timeout,
and send failures reject the correlated future with a descriptive exception.
Malformed PDUs (`validate() != 1`) and decryption failures are dropped silently
(logged at debug), never crashing the loop — covered by a fuzz/property test.

## Testing Strategy

- **Concept conformance** (`tests/coap_cantcoap_concept_conformance_test.cpp`).
- **End-to-end** (`tests/coap_cantcoap_integration_test.cpp`): loopback round
  trip for all three RPCs + an oversized (block-wise) payload.
- **Reliability property tests** mirroring the existing suite:
  retransmission/backoff, duplicate suppression, MAX_RETRANSMIT failure.
- **Robustness**: malformed/undecryptable datagram injection keeps the loop live.
- Reuses the existing `coap_security_*` tests parametrized over the backend.
- All cantcoap-labeled tests skip when `CANTCOAP_AVAILABLE` is undefined.
- Container harnesses follow the Docker/rootless-Podman rules in `CLAUDE.md`.

## Dependencies

### External

- **cantcoap** (BSD-2-Clause), built from source via `vcpkg-overlays/cantcoap/`
  with a vendored `CMakeLists.txt` (upstream ships no build system).

### Internal

- `network.hpp` (concepts); `coap_transport.hpp` (`pending_message`,
  `received_message_info`, `block_transfer_state`, configs); `coap_block_option.hpp`
  (Block1/Block2); `coap_security.hpp` / `coap_edhoc.hpp` (security);
  `Types::serializer_type` (encoding).

## Build Integration

### `vcpkg-overlays/cantcoap/`

`vcpkg_from_github` (commit-pinned `REF`/`SHA512`) → `file(COPY CMakeLists.txt …)`
into the source tree → `vcpkg_cmake_configure`/`vcpkg_cmake_install` →
`vcpkg_cmake_config_fixup(PACKAGE_NAME cantcoap)`. The vendored `CMakeLists.txt`
builds a static lib from `cantcoap.cpp`/`nendian.c`, installs the public headers,
and exports `cantcoap::cantcoap`.

### Root `vcpkg.json`

Opt-in feature `coap-cantcoap` depending on `cantcoap`.

### Root `CMakeLists.txt`

Next to the `COAP_TRANSPORT` block: `find_package(cantcoap QUIET CONFIG)`; when
found, link `cantcoap::cantcoap` and define `CANTCOAP_AVAILABLE` — mirroring the
`LIBCOAP_FOUND` → `LIBCOAP_AVAILABLE` wiring, but via `find_package(... CONFIG)`
(the vendored config package), not pkg-config.

See `doc/coap_library_alternatives.md` for the exact snippets and the
cantcoap-vs-libnyoci trade-off.
