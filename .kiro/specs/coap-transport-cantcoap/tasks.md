# Implementation Plan — CoAP Transport (cantcoap backend)

## Status: Implemented (Tasks 1-5, 7-9 complete; Task 6 partial by design)

The overlay port is pinned and builds; the adapter owns a UDP socket, an RX and
retransmission loop, duplicate suppression, block-wise sequencing and OSCORE.
`coap_cantcoap_client`/`coap_cantcoap_server` satisfy
`network_client`/`network_server` and speak the same wire protocol as the
libcoap and libnyoci backends. DTLS is refused, with a reason — see Task 6.

**Last Updated**: August 8, 2026

## What this backend cost, versus the other two

The spec predicted an easy port and a hard adapter, and that held — but the
adapter was smaller than expected, for a reason worth recording: **almost
everything above the socket already existed.**

| Concern | cantcoap gives | Where it came from |
|---|---|---|
| PDU encode/parse | `CoapPDU` | cantcoap |
| UDP socket + loop | nothing | new here, ~150 lines |
| Retransmit / dedup | nothing | `pending_message`, `received_message_info` |
| Block-wise | nothing | `block_option` + our own sequencing |
| OSCORE | nothing | `oscore::security_context` — **inherited free** |
| DTLS | nothing | refused (Task 6) |

The OSCORE row is the interesting one. `raft/oscore.hpp` was written for the
libnyoci backend and made transport-neutral on principle; this backend picked it
up with no changes at all, because it already owns the bytes on both sides of
the socket. That principle paid for itself the first time it was tested.

## Detailed Task List

- [x] 1. Finalize the `vcpkg-overlays/cantcoap` port
  - [x] 1.1 Pin a real commit SHA and regenerate `SHA512`
    - `99e9ed517d50d36bdaa195f3034af435c23fb210` (master, 2026-05-09). cantcoap
      has no tags at all, so a commit pin is the only option — but unlike
      libnyoci this project is still maintained, so re-pinning periodically is
      reasonable rather than pointless.
  - [x] 1.2 Reconfirm the vendored `CMakeLists.txt` file list against the pin
    - The skeleton listed `nendian.c`/`nendian.h`, which **do not exist**. The
      real tree is `cantcoap.cpp` + `cantcoap.h` + `dbg.h` + `sysdep.h`, plus
      `nethelper.c`/`.h` which are getaddrinfo boilerplate for cantcoap's own
      examples. The library is one translation unit; `nethelper` is deliberately
      not built, since this backend supplies its own socket anyway.
    - `sysdep.h` is included only by `cantcoap.cpp`, so it is not installed;
      `dbg.h` is included by `cantcoap.h` and is.
  - [x] 1.3 Export a working `cantcoap::cantcoap` config package
    - The vendored CMakeLists installs a real `cantcoap-config.cmake` that
      includes a separate targets file, rather than installing the targets file
      *as* the config. The latter appears to work until something calls
      `find_package(cantcoap CONFIG REQUIRED)` twice in one project, at which
      point the second include re-defines the imported target and hard-errors.
    - Verified by extracting the pinned tarball, copying the vendored
      CMakeLists in, and running configure/build/install exactly as the port
      does.

- [x] 2. Build gating (opt-in, gracefully degrading)
  - [x] 2.1 `coap-cantcoap` feature in the root `vcpkg.json`
  - [x] 2.2 `find_package(cantcoap CONFIG)` probe + `CANTCOAP_AVAILABLE`, plus a
        `COAP_TRANSPORT_CANTCOAP` Kconfig symbol (default `n`)
    - Discovery is CONFIG-mode, not pkg-config: cantcoap ships no build system,
      so the port supplies both the build and the config package. This is the
      mirror image of the libnyoci port, which is autotools and therefore
      pkg-config only.
  - [x] 2.3 Default build unaffected; both new test targets compile and pass
        without cantcoap present

- [x] 3. Concept surface
  - Templated on `Types`, constrained by `transport_types<Types>`, satisfying
    `network_client`/`network_server` — asserted in the conformance test, which
    compiles with or without cantcoap.

- [x] 4. Reliability layer
  - [x] 4.1 Confirmable retransmission with RFC 7252 backoff
    - `pending_message` is reused as the spec asked, wrapped in a
      `pending_exchange` that adds what a codec cannot supply: the exact
      datagram to resend, the peer address, the deadline and the block cursor.
      The first retransmission waits `ACK_TIMEOUT` plus a random factor and each
      subsequent one doubles.
  - [x] 4.2 `MAX_RETRANSMIT` exhaustion rejects with `coap_timeout_error`
  - [x] 4.3 Token correlation to exactly one pending future
  - [x] 4.4 Duplicate suppression by Message ID via `received_message_info`, on
        both client and server, with a 60-second window
  - [x] 4.5 Non-confirmable messages are sent without retransmission tracking

- [x] 5. Block-wise transfer
  - Block1 out and Block2 back, driven by our own state machine over
    `block_option` from `coap_block_option.hpp`. The server's Block2 slicing is
    stateless — the block number comes from the request — and its Block1
    reassembly is bounded by `max_request_size`, answering 4.13 rather than
    growing without limit.
  - Covered by a 12 KiB snapshot against a 256-byte block size, roughly 48
    round trips.

- [~] 6. Security layer
  - [x] 6.1 OSCORE, via `raft/oscore.hpp`
    - Inherited unchanged from the libnyoci work. The adapter protects the
      cantcoap-built PDU by re-reading it through the neutral codec and
      protecting the result, and verifies inbound datagrams before anything
      looks at them. Covered end to end, with two negative controls: a wrong
      Master Secret and a plaintext client are both refused *and the RPC handler
      never runs*.
  - [ ] 6.2 DTLS — **refused, not implemented**
    - cantcoap is cleartext-only, and unlike libnyoci there is no DTLS plugin to
      drive. Providing it would mean running an OpenSSL DTLS BIO over this
      backend's own socket — handshake, retransmission, cookie exchange and all
      — which is a transport in its own right rather than an adapter detail.
    - `plan_security()` refuses every DTLS mode at construction with a message
      that points at OSCORE (which this backend does provide) or at the other
      two backends. A half-implementation that merely looked encrypted would be
      worse than refusing.
    - Note this is a *different* reason from the libnyoci backend's RPK refusal:
      there the surface existed and OpenSSL lacked the feature; here the surface
      does not exist at all.
  - [ ] 6.3 EDHOC bootstrap — not implemented here
    - The libnyoci backend serves `/.well-known/edhoc`; this one does not yet.
      Static OSCORE credentials work. Refused explicitly rather than treated as
      static provisioning.
  - [x] 6.4 Malformed or undecryptable datagrams are dropped without invoking a
        handler and without crashing

- [x] 7. Sockets, threading, lifecycle
  - One AF_INET6 socket per client and per server, `IPV6_V6ONLY` off so v4 peers
    arrive v4-mapped. One `std::jthread` each, polling with a bounded timeout so
    the *same* loop that receives datagrams also drives retransmission and
    expiry — no second timer thread, and no lock ordering between them.
  - `stop()` ends the loop, closes the socket, joins, and rejects every
    in-flight future; the destructor sweeps anything the loop missed.

- [x] 8. Tests
  - `tests/coap_cantcoap_concept_conformance_test.cpp` (5 cases) and
    `tests/coap_cantcoap_integration_test.cpp` (17 cases).
  - Beyond the three round trips: block-wise over 12 KiB, retransmission
    exhaustion *with timing assertions that the schedule actually ran and then
    terminated*, duplicate suppression, in-flight cancellation, server restart
    on the same port, unknown target, unhandled RPC, and a robustness case that
    fires empty/truncated/version-0/oversized-token/random datagrams at the
    server and then proves it still answers a real RPC.
  - OSCORE has a positive case and two negative controls, because a
    security test that only checks the happy path cannot tell protection from
    its absence.

- [x] 9. Documentation
  - `doc/coap_library_alternatives.md` becomes a three-way comparison with the
    predictions scored; `DEPENDENCIES.md` gains cantcoap.

## Follow-ups

- **DTLS** (Task 6.2), if a deployment needs channel security on this backend
  specifically. OSCORE covers object security today.
- **EDHOC bootstrap** (Task 6.3): the machinery exists in
  `coap_edhoc_bootstrap.hpp`; what is missing is serving `/.well-known/edhoc`
  from this server, which is a small piece of work.
- **arm64**, unverified here as for the other backends — no cross toolchain.
- **Cross-backend interop tests** still need two processes, since no two CoAP
  backends can share a translation unit.
