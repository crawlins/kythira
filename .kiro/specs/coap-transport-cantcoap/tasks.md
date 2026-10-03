# Implementation Plan — CoAP Transport (cantcoap backend)

## Status: Complete (Tasks 1-9)

The overlay port is pinned and builds; the adapter owns a UDP socket, an RX and
retransmission loop, duplicate suppression, block-wise sequencing, OSCORE with
an optional EDHOC bootstrap, and DTLS 1.2 (PSK, PKI and RPK) driven over that
same socket. `coap_cantcoap_client`/`coap_cantcoap_server` satisfy
`network_client`/`network_server` and speak the same wire protocol as the
libcoap and libnyoci backends.

**Last Updated**: October 2, 2026

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
| EDHOC bootstrap | nothing | `coap_edhoc_bootstrap.hpp` — **inherited**, plus the resource |
| DTLS | nothing | new here: OpenSSL over our own socket (`coap_cantcoap_dtls.hpp`) |

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

- [x] 6. Security layer
  - [x] 6.1 OSCORE, via `raft/oscore.hpp`
    - Inherited unchanged from the libnyoci work. The adapter protects the
      cantcoap-built PDU by re-reading it through the neutral codec and
      protecting the result, and verifies inbound datagrams before anything
      looks at them. Covered end to end, with two negative controls: a wrong
      Master Secret and a plaintext client are both refused *and the RPC handler
      never runs*.
  - [x] 6.2 DTLS, over this backend's own socket
    - `coap_security_provider` still cannot be reused — its interface is libcoap
      types — so `include/raft/coap_cantcoap_dtls.hpp` supplies the layer the
      requirement describes: between `recvfrom`/`sendto` and `CoapPDU`,
      decrypting inbound and encrypting outbound, with CoAP's own reliability
      running over it unchanged (RFC 7252 Section 9.1).
    - One `SSL` per peer over a small custom BIO whose write is one `sendto()`
      and whose read is one queued datagram. Not a socket BIO (there is one
      socket shared by every peer) and not `BIO_s_mem` (a byte stream, so a
      multi-datagram handshake flight would come out concatenated;
      `BIO_s_dgram_mem` only arrived in OpenSSL 3.2). This is how libcoap drives
      OpenSSL too. Handshake retransmission is OpenSSL's, ticked from the
      existing poll loop, so no thread is added.
    - `dtls_psk` (the server answers only its configured identity), `dtls_pki`
      (chain, key, CA, `verify_peer_cert` as mutual authentication on a server,
      `cipher_suites`, and `cn_validator` run once the handshake completes) and
      `dtls_rpk` (RFC 7250, pinned against `trusted_peer_keys`). RPK needs
      OpenSSL 3.2+; on an older one it is refused at construction naming the
      version, which is a version gate rather than a missing surface.
    - The server answers every ClientHello with a HelloVerifyRequest cookie
      keyed on the peer address, so it cannot be used as an amplifier; the
      session table is capped (LRU eviction) because a first ClientHello still
      allocates.
    - Session recovery both ways: a stopping server sends close_notify, and a
      client whose exchange goes unanswered drops its session, so either side
      restarting is followed by a fresh handshake rather than records nobody
      can read. A failed handshake rejects the waiting requests at once with
      the reason, not at their timeout.
    - Key material loads where the socket opens — client construction, server
      `start()` — so bad credentials never leave a listening socket behind.
  - [x] 6.3 EDHOC bootstrap on `/.well-known/edhoc`
    - The libnyoci shape, carried by this backend's own exchanges: the client
      runs the initiator lazily on the first RPC through a raw (no
      Content-Format, no OSCORE) POST, and the server rendezvouses with a
      responder thread through `edhoc_responder_channel`. The wire shape
      matches the libnyoci backend's (no Content-Format, 2.04 replies, an empty
      2.04 for message_3), though no test crosses the two yet.
    - Two things the libnyoci version does not do, both found by tests here:
      - A server whose initiator vanished after message_2 is not wedged. A
        message_1 always starts a fresh responder; message_1 and message_3 are
        told apart by their first CBOR item (an integer METHOD versus the
        CIPHERTEXT_3 byte string), since this carriage has no C_R prefix.
      - A client whose server restarted recovers. An EDHOC server with no
        matching context answers with an unprotected 4.01; the client fails
        that request promptly and drops its context, so the next request
        bootstraps again.
    - Each exchange pins the context it was protected under, so a context
      replaced mid-flight cannot be used to verify the wrong response.
  - [x] 6.4 Malformed or undecryptable datagrams are dropped without invoking a
        handler and without crashing

- [x] 7. Sockets, threading, lifecycle
  - One AF_INET6 socket per client and per server, `IPV6_V6ONLY` off so v4 peers
    arrive v4-mapped, falling back to AF_INET on a kernel with no IPv6 at all
    (found by running the suite on one: every test failed to open a socket).
    One `std::jthread` each, polling with a bounded timeout so
    the *same* loop that receives datagrams also drives retransmission and
    expiry — no second timer thread, and no lock ordering between them.
  - `stop()` ends the loop, closes the socket, joins, and rejects every
    in-flight future; the destructor sweeps anything the loop missed.

- [x] 8. Tests
  - `tests/coap_cantcoap_concept_conformance_test.cpp` (5 cases),
    `tests/coap_cantcoap_integration_test.cpp` (22 cases with lakers) and
    `tests/coap_cantcoap_dtls_test.cpp` (15 cases on OpenSSL 3.2+, 14 below).
  - Beyond the three round trips: block-wise over 12 KiB, retransmission
    exhaustion *with timing assertions that the schedule actually ran and then
    terminated*, duplicate suppression, in-flight cancellation, server restart
    on the same port, unknown target, unhandled RPC, and a robustness case that
    fires empty/truncated/version-0/oversized-token/random datagrams at the
    server and then proves it still answers a real RPC.
  - EDHOC: bootstrap then RPC, bootstrap-once across five RPCs and a block-wise
    snapshot, a mismatched credential that fails without the handler running
    *and* leaves the server able to bootstrap the next client, plaintext
    refused with 4.01 before any handshake, and recovery after a server
    restart.
  - DTLS: PSK, PKI and RPK round trips, each with negative controls (unknown
    identity, wrong key, untrusted CA, missing client certificate, a refusing
    `cn_validator`, unpinned raw keys on either side), a plaintext client
    ignored, block-wise over DTLS, recovery after a server restart, and garbage
    datagrams at a DTLS server.
  - The EDHOC tests found a duplicate-suppression bug: the server keyed seen
    Message IDs by ID alone, so a second client whose IDs collided with the
    first's had its requests silently dropped for 60 seconds. RFC 7252 Section
    4.5 scopes a Message ID to its source endpoint; the server now does too,
    and clients start from a random Message ID (Section 4.4).
  - OSCORE has a positive case and two negative controls, because a
    security test that only checks the happy path cannot tell protection from
    its absence.

- [x] 9. Documentation
  - `doc/coap_library_alternatives.md` becomes a three-way comparison with the
    predictions scored; `DEPENDENCIES.md` gains cantcoap.

## Follow-ups

- **Revocation, `cn_validator` and ACE-OAuth parity with libcoap**: closed by
  `.kiro/specs/coap-alternate-backend-security-parity` (October 3, 2026).
  Revocation used to be ignored here, and an ACE config crashed construction;
  the CRL check now runs in `check_established_peer()` ahead of the validator,
  and ACE runs in `plan_security()`. Tests:
  `tests/coap_cantcoap_security_parity_test.cpp`.
- **arm64**, unverified here as for the other backends — no cross toolchain.
- **Cross-backend interop tests** still need two processes, since no two CoAP
  backends can share a translation unit. EDHOC and DTLS are both meant to
  interoperate with the other backends on the wire; neither is asserted yet.
- **CI does not install the `coap-cantcoap` feature**, so CI compiles this
  backend's stub path only. The suites above were run locally against cantcoap
  and lakers, on OpenSSL 3.0 and 3.6, and under ASan, UBSan and TSan.
