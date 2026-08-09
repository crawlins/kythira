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

**Implementation note — the split is not clean in two places.** libnyoci owns
Block2 on the *client* side only, so the adapter slices block-wise responses on
the server side itself; and libnyoci has no Block1 at all, so an over-large
request is refused rather than split. See "Block-wise transfer" below.

### Key design decision: security stays in kythira, not libnyoci

libnyoci provides DTLS only through an external ssl plugin and ships **no
OSCORE and no EDHOC**. kythira already owns OSCORE/EDHOC via
`coap_security.hpp` / `coap_edhoc.hpp`. Forking security per backend would be a
maintenance trap, so the intent was to keep kythira's `coap_security_provider`
*above* a plain-CoAP libnyoci core: decrypt inbound datagrams before handing
them to libnyoci-produced PDUs and encrypt outbound ones after.

**That is not achievable against the interface as it stands, and the
implementation does not pretend otherwise.** Every `coap_security_provider`
method is expressed in libcoap types (`coap_context_t*`, `coap_session_t*`,
`coap_pdu_t*`, `coap_address_t*`), and `protect`/`unprotect` are identity
passthroughs precisely *because* every mode is implemented below libcoap's PDU
API rather than as a byte-level transform an adapter could call. There is no
seam to sit above.

So the backend serves **plain CoAP only**. Configuration handling is shared
verbatim — `translate_legacy_fields()` moved into
`raft/coap_transport_config.hpp` so both backends reach the same verdict about
a given config — and any mode that would actually have to encrypt bytes is
refused at construction with a `coap_security_error` naming the reason.
Silently downgrading a node that asked for DTLS to plaintext Raft traffic is
strictly worse than not starting. `tasks.md` Task 5.1 records the two options
for closing the gap.

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

## Block-wise transfer

libnyoci is asymmetric here, and the adapter's shape follows from that:

- **Client, inbound (Block2): provided.** libnyoci's transaction layer requests
  the next block itself — but *only* when the transaction carries
  `NYOCI_TRANSACTION_ALWAYS_INVALIDATE`, which is why the adapter always sets
  that flag. The cost is that the response callback can fire more than once
  (once per block, plus a final invalidate), so every promise-settling path is
  guarded by a `settled` flag.
- **Server, outbound (Block2): not provided.** The adapter slices the response
  itself. It does so *statelessly*: the block number and szx come from the
  request's own Block2 option, so there is no per-peer transfer state to expire
  and a lost block is simply re-requested. Sizes come from
  `nyoci_outbound_get_space_remaining()` on the live packet rather than from a
  compile-time constant, so this holds for whatever `NYOCI_MAX_CONTENT_LENGTH`
  the library was built with.
- **Block1: absent entirely.** `COAP_OPTION_BLOCK1` appears only in libnyoci's
  option-name and option-type tables. A request that does not fit one datagram
  cannot be split, so `send_rpc` rejects its future with an error naming the
  missing support. This bounds InstallSnapshot; large-snapshot deployments
  should use the libcoap backend.

## Header incompatibility with libcoap

`coap_transport_libnyoci_impl.hpp` **cannot include
`raft/coap_transport.hpp`**, which is where the design originally expected to
find `pending_message`, `block_transfer_state` and the config structs. That
header includes `<coap3/coap.h>` whenever `LIBCOAP_AVAILABLE` is defined, and
the two C libraries cannot coexist in one translation unit: libcoap spells the
option numbers as object-like macros (`#define COAP_OPTION_IF_MATCH 1`) while
libnyoci spells them as enumerators (`COAP_OPTION_IF_MATCH = 1,`), so libcoap's
macros rewrite the middle of libnyoci's enum into `1 = 1,`. No include order
fixes it.

The library-neutral declarations therefore live in
`include/raft/coap_transport_config.hpp` — `coap_client_config`,
`coap_server_config`, `pending_message`, `received_message_info` and
`translate_legacy_fields()` — which `coap_transport.hpp` now includes, leaving
every existing call site unchanged. What stayed behind is genuinely
libcoap-shaped: `block_transfer_state` (holds a `coap_session_t*`),
`coap_error_info` (keyed by `coap_pdu_code_t`), and the `*_transport_types`
bundles.

The two libraries still do not conflict at *link* time (Requirement 2.4); a
translation unit selects a backend by which header it includes.

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

- `include/raft/network.hpp` (concepts), `coap_transport_config.hpp` (shared
  configs / `pending_message` / `translate_legacy_fields()` — *not*
  `coap_transport.hpp`, see "Header incompatibility with libcoap" above),
  `coap_security.hpp` (mode enum and config structs, libcoap-free),
  `coap_utils.hpp` + `serializer_registry.hpp` + `peer_capability_cache.hpp`
  (Content-Format negotiation, shared with the libcoap backend so the two are
  wire-interoperable), `Types::serializer_type` (encoding).

## Build Integration

### `vcpkg-overlays/libnyoci/`

Autotools port: `vcpkg_from_github` (pinned `REF`/`SHA512`) →
`vcpkg_configure_make AUTOCONFIG` → `vcpkg_install_make` →
`vcpkg_fixup_pkgconfig`. No `vcpkg_cmake_config_fixup` (there is no CMake
config package).

The real configure flags are `--disable-examples --disable-nyocictl
--disable-plugtest --disable-dependency-tracking`; the sketch's
`--disable-tests`/`--without-examples` do not exist in `configure.ac`, and
`--disable-dependency-tracking` is mandatory rather than optional (automake's
depfile bootstrap fails otherwise — upstream's own CI passes it). The port also
carries one patch, for an autoconf-archive incompatibility; see
`vcpkg-overlays/libnyoci/README.md`. Host build tools include
**`autoconf-archive`**, because `configure.ac` calls `AX_PTHREAD`.

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
