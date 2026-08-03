# `ion-c` overlay port

Provides [`amazon-ion/ion-c`](https://github.com/amazon-ion/ion-c), the C
reference implementation of Amazon Ion, to vcpkg. It backs kythira's
`ion_rpc_serializer` (`include/raft/ion_serializer.hpp`) — an alternative
`Types::serializer_type` to `json_rpc_serializer`, using Amazon Ion instead of
JSON as the RPC wire format.

No official vcpkg registry port for `ion-c` exists, so — like
`vcpkg-overlays/stdexec` and `vcpkg-overlays/lakers` — this project ships its
own overlay port. `ion-c` is a plain CMake C project, so the port follows the
CMake-port shape (`vcpkg_from_github` + `vcpkg_cmake_configure` /
`vcpkg_cmake_install` / `vcpkg_cmake_config_fixup`), not the cargo-build shape
`lakers` needs.

## Opt-in only

The port is gated behind the opt-in `"ion"` feature in the root `vcpkg.json`
(mirroring the `"edhoc"` feature that gates `lakers`). A default install does
**not** fetch or build `ion-c`; omitting the feature leaves
`KYTHIRA_ION_SERIALIZER_AVAILABLE` undefined and `ion_rpc_serializer`
unavailable, and the rest of the project configures and builds unaffected
(graceful degradation, mirroring `LIBCOAP_FOUND`).

To build with Ion support:

```sh
vcpkg install ion-c --overlay-ports=vcpkg-overlays
# or add the "ion" feature to your manifest install
cmake -B build -DVCPKG_MANIFEST_FEATURES=ion ...
```

The `ION_SERIALIZER` Kconfig symbol (root `Kconfig`) plus
`KYTHIRA_KCONFIG_STRICT=ON` turns a missing `ion-c` into a hard configure
failure, exactly as `COAP_TRANSPORT` does for `libcoap`.

## Pin / SHA512 regeneration

`portfile.cmake` pins `REF v1.1.3`, with the real `SHA512` of
`https://github.com/amazon-ion/ion-c/archive/refs/tags/v1.1.3.tar.gz` already
filled in (computed directly: `curl -sSL <url> | sha512sum`). On a pin bump,
regenerate it the same way, or run the `vcpkg install` command above with a
deliberately wrong placeholder: vcpkg fails the download with the actual hash,
which you copy into the `SHA512` field. This only affects opt-in Ion builds;
it never affects the default (Ion-less) build.

## Patch: version header without `git describe`

`0001-fix-version-header-without-git-describe.patch` fixes a real build
failure, not a vcpkg-specific workaround: `cmake/VersionHeader.cmake`
generates `build_version.h`'s `IONC_VERSION_MAJOR`/`MINOR`/`PATCH` macros by
regex-parsing `git describe --long --tags --dirty --match "v*"`'s output.
`vcpkg_from_github` extracts a plain source tarball with no `.git` directory,
so `git describe` fails, the regex never matches, and the three macros
substitute as empty — which fails `ion_version.c`'s build with "expected
expression before ';' token" (`*major = ;`). The patch falls back to
`CMAKE_PROJECT_VERSION_MAJOR`/`MINOR`/`PATCH` (already known from
`project(IonC VERSION 1.1.3 ...)`) when the git-describe regex doesn't
match — the same numbers a real git checkout with the `v1.1.3` tag would
have produced. Confirmed by a real build under `vcpkg install
--x-feature=ion`; regenerate the patch (`diff -u` against a fresh checkout of
`cmake/VersionHeader.cmake`) if a pin bump changes that file upstream.

## Patch: `ASSERT()` infinite loop under `NDEBUG`

`0002-fix-assert-infinite-loop-under-ndebug.patch` fixes a real, and
serious, upstream bug, not a vcpkg-specific workaround: `ionc/ion_internal.h`
defines `ASSERT(x)` as `while (!(x)) { ion_helper_breakpoint(), assert(x); }`
unconditionally. Under `NDEBUG` (this port's Release build, the default),
`assert(x)` compiles away entirely, so a failed `ASSERT`'s `while` condition
never becomes false — the process spins at 100% CPU **forever** instead of
either aborting (a debug build) or being silently skipped (a release build,
matching plain `assert()`'s own `NDEBUG` semantics). Confirmed directly: a
`kythira::ion_rpc_serializer` deserialize call on **truncated** Ion input
(the exact case `tests/ion_malformed_message_property_test.cpp`'s
`property_truncated_message_rejected` exercises — Property 4, "malformed
input is rejected, never crashes") reliably hangs a Release build
indefinitely inside this exact macro: `ion_reader_step_out()` on a container
whose declared length was never fully consumed, because the buffer was
truncated mid-container, trips an internal reader-state invariant. The
patch makes `ASSERT(x)` a true no-op under `NDEBUG`, restoring the
(evidently intended, just miswritten) debug-vs-release split. This is a
real hazard for *any* consumer building `ion-c` in Release mode and feeding
it malformed/truncated input, not specific to kythira's own reader code.
