# Implementation Plan — CoAP Transport (libnyoci backend)

## Status: Proposed (0/N tasks) — skeleton only

The overlay port and adapter headers exist as **skeletons**
(`vcpkg-overlays/libnyoci/`, `include/raft/coap_transport_libnyoci_impl.hpp`)
with placeholder `REF`/`SHA512` and `TODO` method bodies. Nothing is wired into
the default build. This plan tracks turning the skeleton into a working,
tested backend. See `doc/coap_library_alternatives.md` for the design rationale.

**Last Updated**: August 8, 2026 (spec created alongside the skeletons)

## Major Tasks Overview

### Tasks 1-2: Overlay port and build gating
*Stand up the autotools overlay port and the `LIBNYOCI_AVAILABLE` gating before
any adapter body is written, so later tasks can assume libnyoci is linkable.*

### Tasks 3-5: Adapter (bridge libnyoci ↔ futures)
*Client transaction path, server handler path, and security wiring — thin,
because libnyoci owns sockets/retransmit/block-wise.*

### Tasks 6-7: Testing and documentation

## Detailed Task List

- [ ] 1. Finalize the `vcpkg-overlays/libnyoci` port
  - [ ] 1.1 Pin a real `REF` (release tag or commit) and regenerate `SHA512` per the port README
  - [ ] 1.2 Verify `vcpkg_configure_make AUTOCONFIG` builds from the archive tarball on x64 and arm64 Linux
  - [ ] 1.3 Add any patches surfaced by the first real build (e.g. version-header fallback), documented in the portfile like ion-c's patches
  - [ ] 1.4 Confirm `vcpkg_fixup_pkgconfig` yields a discoverable `libnyoci.pc`

- [ ] 2. Build gating (opt-in, gracefully degrading)
  - [ ] 2.1 Add the `coap-libnyoci` feature to the root `vcpkg.json`
  - [ ] 2.2 Add the `pkg_check_modules(LIBNYOCI ...)` probe + `LIBNYOCI_AVAILABLE` definition to the root `CMakeLists.txt`, mirroring the `LIBCOAP_FOUND` block
  - [ ] 2.3 Verify a default build (feature off) neither fetches nor links libnyoci and still succeeds

- [ ] 3. Client adapter (`coap_libnyoci_client<Types>`)
  - [ ] 3.1 Construct one `nyoci_t` and drive its process loop on a `std::jthread`
  - [ ] 3.2 Implement `send_rpc<Request,Response>`: serialize → begin transaction → return `future_template<Response>`
  - [ ] 3.3 Implement the `on_response` C trampoline bridging into the `pending_message` resolve/reject callbacks
  - [ ] 3.4 Translate libnyoci status codes into the existing `coap_exceptions.hpp` types
  - [ ] 3.5 Wire `send_request_vote`/`send_append_entries`/`send_install_snapshot` onto `send_rpc`

- [ ] 4. Server adapter (`coap_libnyoci_server<Types>`)
  - [ ] 4.1 Register one inbound resource/handler per RPC path
  - [ ] 4.2 Deserialize → invoke the stored `std::function` (guarded by `mutex_`) → emit response echoing the token
  - [ ] 4.3 `start`/`stop`/`is_running` with clean socket + thread teardown; reject in-flight futures on shutdown

- [ ] 5. Security integration
  - [ ] 5.1 Route DTLS through `coap_security_provider` above a plain-CoAP libnyoci core
  - [ ] 5.2 Reuse `coap_edhoc`/OSCORE exactly as the libcoap path (libnyoci ships neither)
  - [ ] 5.3 Confirm invalid cert/key material surfaces the same errors as libcoap

- [ ] 6. Tests (skipped when `LIBNYOCI_AVAILABLE` is undefined)
  - [ ] 6.1 Concept-conformance test (`static_assert` both concepts); uncomment the asserts in the adapter header
  - [ ] 6.2 Loopback end-to-end test for all three RPCs
  - [ ] 6.3 Block-wise transfer end-to-end test
  - [ ] 6.4 Any container harness follows the Docker/rootless-Podman rules in `CLAUDE.md`

- [ ] 7. Documentation
  - [ ] 7.1 Update `DEPENDENCIES.md` with libnyoci (opt-in, autotools, MIT)
  - [ ] 7.2 Cross-link this spec from `doc/coap_library_alternatives.md`
