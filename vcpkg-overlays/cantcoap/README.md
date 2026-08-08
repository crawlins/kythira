# cantcoap overlay port (SKELETON)

Candidate **"own-the-stack" alternate CoAP backend** for kythira's
`coap_transport`. cantcoap ([staropram/cantcoap][repo]) is a minimal,
BSD-2-licensed CoAP **message codec** — it constructs and parses RFC 7252 PDUs
and does nothing else. There are **no sockets, no retransmission, no
duplicate detection, no block-wise state machine, and no DTLS**. Everything
above the wire format is yours to build in
`include/raft/coap_transport_cantcoap_impl.hpp` (reusing the existing
`coap_security`, `coap_block_option`, `pending_message` and
`block_transfer_state` scaffolding).

This is a **skeleton**, not a working port. Before it builds:

1. **Pin a real `REF`.** cantcoap is unversioned; replace the all-zero commit
   SHA in `portfile.cmake` with a real commit.
2. **Regenerate `SHA512`** (same procedure as `vcpkg-overlays/ion-c/README.md`):
   ```sh
   vcpkg install cantcoap --overlay-ports=vcpkg-overlays
   ```
3. **Reconfirm the source/header lists** in the vendored `CMakeLists.txt`
   against the pinned commit.

## Why this port vendors a `CMakeLists.txt`

Unlike `ion-c`/`libnyoci`, cantcoap ships **no build system** — just source
files and a test-only Makefile. The port therefore copies a vendored
`CMakeLists.txt` (in this directory) into the extracted tree and drives it with
the ordinary `vcpkg_cmake_configure` / `vcpkg_cmake_install` /
`vcpkg_cmake_config_fixup` helpers, exporting a `cantcoap::cantcoap` target for
`find_package(cantcoap CONFIG)`. The *port* is thus the simplest of the three
CoAP options — but that simplicity is exactly what pushes the complexity into
the adapter.

## The trade-off in one line

- **libnyoci:** hard port (autotools), easy adapter (full stack).
- **cantcoap:** easy port (this), hard adapter (you write the transport).

See `doc/coap_library_alternatives.md` for the full side-by-side.

[repo]: https://github.com/staropram/cantcoap
