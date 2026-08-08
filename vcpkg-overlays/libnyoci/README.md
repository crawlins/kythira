# libnyoci overlay port (SKELETON)

Candidate **alternate CoAP backend** to libcoap for kythira's
`coap_transport`. libnyoci ([darconeous/libnyoci][repo], formerly SMCP) is a
full-stack RFC 7252 client/server library in C — it owns the socket, the
message layer, retransmission, observe, async responses and block-wise
transfer, so the C++ adapter over it (`include/raft/coap_transport_libnyoci_impl.hpp`)
stays comparatively thin.

This is a **skeleton**, not a working port. Two things must be done before it
builds:

1. **Pin a real `REF`.** Replace the placeholder `v0.1.2` in `portfile.cmake`
   with a real release tag or commit SHA.
2. **Regenerate `SHA512`.** The placeholder all-zero digest is intentionally
   invalid. Run the install once and copy the "Actual hash" vcpkg prints:

   ```sh
   vcpkg install libnyoci --overlay-ports=vcpkg-overlays
   ```

## Why this port looks different from `ion-c`

`ion-c` and `stdexec` are CMake projects, so their ports use
`vcpkg_cmake_configure`. **libnyoci is an autotools project**, so this port
uses `vcpkg_configure_make` + `vcpkg_install_make` + `vcpkg_fixup_pkgconfig`
instead. Consequences:

- **Host build tools required:** `autoconf`, `automake`, `libtool`,
  `pkg-config` (the `AUTOCONFIG` step re-runs `autoreconf`, because GitHub
  archive tarballs omit the generated `./configure`).
- **No CMake config package.** Downstream discovery is via pkg-config
  (`pkg_check_modules(LIBNYOCI QUIET libnyoci)`), the same fallback path the
  root `CMakeLists.txt` already has for system libcoap — not
  `find_package(... CONFIG)`.

## Consuming it

Gate it behind an opt-in `coap-libnyoci` feature in the root `vcpkg.json`
(mirroring how `edhoc`/`ion` gate lakers/ion-c), and add a probe in the root
`CMakeLists.txt` that sets `LIBNYOCI_AVAILABLE` — structurally identical to the
`LIBCOAP_FOUND` → `LIBCOAP_AVAILABLE` block. See
`doc/coap_library_alternatives.md` for the full wiring and the libcoap-vs-this
trade-off.

[repo]: https://github.com/darconeous/libnyoci
