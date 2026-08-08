# Alternate CoAP backends: libnyoci vs cantcoap

The CoAP transport (`include/raft/coap_transport.hpp`) is currently backed by
**libcoap** (vcpkg port with the `dtls` feature, plus OSCORE/EDHOC via the
`lakers` overlay, block-wise transfer, multicast and ACE-OAuth). This document
compares two candidate *alternate* backends and provides skeletons for each so
they can be evaluated side by side:

| | **libnyoci** | **cantcoap** |
|---|---|---|
| Repo | [darconeous/libnyoci][nyoci] (formerly SMCP) | [staropram/cantcoap][cant] |
| License | MIT | BSD-2-Clause |
| Scope | Full RFC 7252 client/server stack | PDU codec only (build/parse one message) |
| Sockets / event loop | Provided | **You write it** |
| Retransmission / dedup | Provided | **You write it** (reuse `pending_message`) |
| Block-wise transfer | Provided | **You write it** (reuse `coap_block_option.hpp`) |
| DTLS | Via ssl plugin | **You wire it** (reuse `coap_security.hpp`) |
| OSCORE / EDHOC | Not built in | Not built in |
| Build system | **autotools** | **none** (source files only) |
| vcpkg port shape | `vcpkg_configure_make` (hard) | vendored `CMakeLists` (easy) |
| Adapter size | Thin (bridge callbacks → futures) | Thick (own the whole transport) |

**The trade-off in one line:** libnyoci is a *hard port + easy adapter*;
cantcoap is an *easy port + hard adapter*. Neither ships OSCORE, so kythira's
existing `coap_security` / `coap_edhoc` security layer stays in place for both —
which is an argument for keeping security *above* whichever CoAP core is chosen
rather than delegating it to the library.

## Files in this skeleton

```
vcpkg-overlays/libnyoci/       overlay port (autotools; vcpkg_configure_make)
  ├─ vcpkg.json
  ├─ portfile.cmake
  └─ README.md
vcpkg-overlays/cantcoap/       overlay port (vendored CMakeLists)
  ├─ vcpkg.json
  ├─ portfile.cmake
  ├─ CMakeLists.txt            supplied because upstream has no build system
  └─ README.md
include/raft/
  ├─ coap_transport_libnyoci_impl.hpp   thin adapter over the full stack
  └─ coap_transport_cantcoap_impl.hpp   thick adapter around the codec
.kiro/specs/coap-transport-libnyoci/    requirements + design + tasks
.kiro/specs/coap-transport-cantcoap/    requirements + design + tasks
doc/coap_library_alternatives.md        (this file)
```

Each backend has a full spec under `.kiro/specs/`, following the same
requirements/design/tasks shape as `.kiro/specs/coap-transport/` and
`.kiro/specs/ion-rpc-serializer/`. Both are **Proposed / skeleton-only** — the
task lists track turning these skeletons into working, tested backends.

Everything here is a **skeleton**: overlay `REF`/`SHA512` values are
placeholders that must be pinned and regenerated (see each port's `README.md`,
same procedure as `vcpkg-overlays/ion-c/README.md`), and the adapter method
bodies are `TODO`. Nothing is wired into the default build, so the existing
libcoap path is untouched.

## How each would plug into the build

Both follow the **exact graceful-degradation pattern libcoap already uses** in
the root `CMakeLists.txt` (the `COAP_TRANSPORT` block around line 211: probe →
set `LIBCOAP_FOUND` → define `LIBCOAP_AVAILABLE`, warn-and-skip when absent).

### 1. Opt-in vcpkg feature (root `vcpkg.json`)

Mirror how `edhoc`/`ion` gate `lakers`/`ion-c` so a default install never
fetches either backend:

```jsonc
"features": {
  "coap-libnyoci": {
    "description": "Alternate libnyoci CoAP backend (overlay port).",
    "dependencies": ["libnyoci"]
  },
  "coap-cantcoap": {
    "description": "Alternate cantcoap CoAP backend (overlay port).",
    "dependencies": ["cantcoap"]
  }
}
```

### 2. CMake probe (root `CMakeLists.txt`, next to the libcoap block)

```cmake
# libnyoci is autotools + pkg-config — discover via pkg_check_modules, the same
# fallback path system-libcoap uses (NOT find_package CONFIG).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBNYOCI QUIET libnyoci)
endif()

# cantcoap ships a CMake config from our vendored CMakeLists — find_package CONFIG.
find_package(cantcoap QUIET CONFIG)
```

Then define the availability macro exactly as `LIBCOAP_AVAILABLE` is defined:

```cmake
if(LIBNYOCI_FOUND)
    target_link_libraries(network_simulator INTERFACE ${LIBNYOCI_LIBRARIES})
    target_include_directories(network_simulator INTERFACE ${LIBNYOCI_INCLUDE_DIRS})
    target_compile_definitions(network_simulator INTERFACE LIBNYOCI_AVAILABLE)
endif()
if(cantcoap_FOUND)
    target_link_libraries(network_simulator INTERFACE cantcoap::cantcoap)
    target_compile_definitions(network_simulator INTERFACE CANTCOAP_AVAILABLE)
endif()
```

The adapter headers `#ifdef` on `LIBNYOCI_AVAILABLE` / `CANTCOAP_AVAILABLE` and
compile to a stub otherwise — identical to `coap_transport.hpp`'s treatment of
`LIBCOAP_AVAILABLE`.

## Concept conformance

Both skeletons are templated on `Types` and constrained by
`kythira::transport_types<Types>`, and expose the same
`send_request_vote` / `send_append_entries` / `send_install_snapshot` (client)
and `register_*_handler` / `start` / `stop` / `is_running` (server) surface as
`coap_client` / `coap_server`. Once the bodies exist, the commented
`static_assert(network_client<...>)` / `static_assert(network_server<...>)`
lines in each header verify they satisfy `include/raft/network.hpp` — the same
contract the libcoap, Beast-HTTP and gRPC transports meet, so the Raft layer
consumes any of them interchangeably.

## Recommendation

- Pick **libnyoci** if the goal is a genuine second *full* CoAP stack with the
  least adapter code, and the autotools port cost (host `autoconf`/`automake`/
  `libtool`, `vcpkg_configure_make`) is acceptable.
- Pick **cantcoap** if the goal is to *own* the CoAP wire behaviour end to end
  and reuse kythira's existing retransmission/block-wise/security machinery over
  a tiny, trivially-vendored codec — accepting that the adapter becomes the bulk
  of the work.

For a drop-in replacement that changes the least, libnyoci is the closer analog
to today's libcoap integration. For maximal control (and to exercise the
`coap_security` / `coap_block_option` scaffolding that already exists), cantcoap
is the more instructive build.

[nyoci]: https://github.com/darconeous/libnyoci
[cant]: https://github.com/staropram/cantcoap
