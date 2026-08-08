# Implementation Plan — CoAP Transport (cantcoap backend)

## Status: Proposed (0/N tasks) — skeleton only

The overlay port (with its vendored `CMakeLists.txt`) and the adapter header
exist as **skeletons** (`vcpkg-overlays/cantcoap/`,
`include/raft/coap_transport_cantcoap_impl.hpp`) with placeholder `REF`/`SHA512`
and `TODO` bodies. Nothing is wired into the default build. This plan tracks
turning the skeleton into a working, tested backend. Because cantcoap is only a
codec, the adapter (Tasks 3-7) is the bulk of the work — the port (Tasks 1-2) is
trivial. See `doc/coap_library_alternatives.md` for the rationale.

**Last Updated**: August 8, 2026 (spec created alongside the skeletons)

## Major Tasks Overview

### Tasks 1-2: Overlay port and build gating
*Vendored-CMake overlay port and `CANTCOAP_AVAILABLE` gating — the easy part.*

### Tasks 3-8: Adapter (own the transport around the codec)
*Socket + loop, reliability, block-wise, security, then the concept surface —
each reusing existing kythira scaffolding rather than reimplementing it.*

### Tasks 9-10: Testing and documentation

## Detailed Task List

- [ ] 1. Finalize the `vcpkg-overlays/cantcoap` port
  - [ ] 1.1 Pin a real commit SHA and regenerate `SHA512` per the port README
  - [ ] 1.2 Reconfirm the vendored `CMakeLists.txt` source/header list against the pinned commit
  - [ ] 1.3 Verify the port builds and exports `cantcoap::cantcoap` on x64 and arm64 Linux

- [ ] 2. Build gating (opt-in, gracefully degrading)
  - [ ] 2.1 Add the `coap-cantcoap` feature to the root `vcpkg.json`
  - [ ] 2.2 Add `find_package(cantcoap QUIET CONFIG)` + `CANTCOAP_AVAILABLE` to the root `CMakeLists.txt`, mirroring the `LIBCOAP_FOUND` block
  - [ ] 2.3 Verify a default build (feature off) neither fetches nor links cantcoap

- [ ] 3. Socket + event loop (new, thin)
  - [ ] 3.1 Open/bind a UDP socket for client and server
  - [ ] 3.2 Combined RX + retransmission-timer loop on a `std::jthread`, with clean stop
  - [ ] 3.3 Synchronize shared state (`pending_`, `seen_`, handlers) between loop and callers

- [ ] 4. Encode/parse via `CoapPDU`
  - [ ] 4.1 Build request/response PDUs (version/type/code/token/Message_ID/URI/payload)
  - [ ] 4.2 Parse inbound datagrams; drop on `validate() != 1`
  - [ ] 4.3 `next_message_id()` and token generation (respect `coap_max_token_length`)

- [ ] 5. Reliability layer (reuse `pending_message`/`received_message_info`)
  - [ ] 5.1 Track confirmable sends; retransmit with CoAP exponential backoff
  - [ ] 5.2 Reject the correlated future on MAX_RETRANSMIT with a `coap_*` exception
  - [ ] 5.3 Correlate responses to pending futures by token
  - [ ] 5.4 Duplicate suppression by Message_ID; non-confirmable path skips tracking

- [ ] 6. Block-wise transfer (reuse `block_transfer_state`/`coap_block_option.hpp`)
  - [ ] 6.1 Split oversized payloads into Block1/Block2 sequences
  - [ ] 6.2 Reassemble inbound blocks; time out stalled transfers

- [ ] 7. Security layer (wrap the socket)
  - [ ] 7.1 Insert `coap_security_provider` for DTLS: decrypt on RX, encrypt on TX
  - [ ] 7.2 Reuse `coap_edhoc`/OSCORE exactly as the libcoap path
  - [ ] 7.3 Drop undecryptable datagrams without crashing

- [ ] 8. Concept surface
  - [ ] 8.1 `send_rpc<Request,Response>` and the three `send_*` client methods
  - [ ] 8.2 `register_*_handler`, `start`, `stop`, `is_running` on the server
  - [ ] 8.3 Uncomment and pass the `static_assert(network_client/server<...>)` lines

- [ ] 9. Tests (skipped when `CANTCOAP_AVAILABLE` is undefined)
  - [ ] 9.1 Concept-conformance test
  - [ ] 9.2 Loopback end-to-end for all three RPCs
  - [ ] 9.3 Reliability property tests: backoff, duplicate suppression, max-retransmit failure
  - [ ] 9.4 Block-wise end-to-end (payload > one block)
  - [ ] 9.5 Malformed/undecryptable-datagram robustness test
  - [ ] 9.6 Any container harness follows the Docker/rootless-Podman rules in `CLAUDE.md`

- [ ] 10. Documentation
  - [ ] 10.1 Update `DEPENDENCIES.md` with cantcoap (opt-in, vendored CMake, BSD-2)
  - [ ] 10.2 Cross-link this spec from `doc/coap_library_alternatives.md`
