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

## Patch: GCC 14 `-Wincompatible-pointer-types`

`0003-fix-gcc-14-incompatible-pointer-types.patch` is what makes this port
build at all on a GCC 14 toolchain. GCC 14 promoted
`-Wincompatible-pointer-types` from a warning to an error by default, and
ion-c 1.1.3 trips it in two places. Both were found in one pass by sweeping
every `.c` in the tree with `-fsyntax-only` under the newly-fatal
diagnostics, rather than by a fix-and-rebuild loop — ninja reports only the
first error per file, so each iteration of that loop costs a full rebuild to
surface one more site.

The first hunk (`ionc/ion_allocation.c`) is pure type repair: a `memcpy`
source argument is a conditional whose arms are `char *` and `BYTE *`, so it
has no composite type. Identical bytes are copied either way.

The second hunk (`ionc/ion_binary.c`, `ion_binary_read_int_64_and_sign()`)
is a real latent bug. `ION_GET(pstream, b)` expands on its slow path to
`ion_stream_read_byte(pstream, &b)`, whose parameter is `int *`, but `b` is
declared `uint64_t` — **the only one of ~28 `ION_GET` call sites in the
library whose variable is not an `int`** (all were checked). The callee
writes an `int` through a `uint64_t *`, which violates strict aliasing and
initialises 4 of the variable's 8 bytes. It works today only by luck: `b` is
zero-initialised and the host is little-endian. The fix reads into an `int`
and widens explicitly, which is what every other call site already does.
The argument for why this is behaviour-preserving even at EOF — where
`ion_stream_read_byte` sets `*p_c = EOF` and still returns `IERR_OK` — is
written out in the patch header; check it rather than take it on faith.

### How this patch is guarded

Both hunks are compile-time fixes for a compiler the rest of CI does not
use, so neither is guarded by anything the main legs do. Two things cover
them, and they are meant to be read together:

- **`ion-serializer-build` in `.github/workflows/ci.yml`** — the only job
  that installs the `ion` vcpkg feature, pinned to `gcc-14`/`g++-14`
  precisely so that reverting this patch is a hard build failure. Under
  g++-13 or clang++-18 both sites are warnings at most, so a leg on either
  compiler would build a reverted patch happily and prove nothing.
- **`tests/ion_binary_decimal_regression_test.cpp`** — value coverage for
  the rewritten function. Reaching it is not obvious: the binary reader's
  `int64` path does not route through it, so none of the other five `ion_*`
  tests execute a single instruction of it (confirmed under gdb with a
  positive control). Its only reachable caller is a binary decimal's
  mantissa decode, and only for mantissas of 8 bytes or fewer. That test
  cannot detect a straight revert on its own — on little-endian the
  truncating write still lands the right value — which is exactly why the
  compiler leg above is the primary guard.

<!--
MEASUREMENT BRANCH ONLY — not for main.

This line exists to change hashFiles('vcpkg.json', 'vcpkg-overlays/**'),
which is the key of the vcpkg_installed/ tree cache (the L1). Without a
change here that cache HITS, the "Bootstrap vcpkg and install dependencies"
step is skipped entirely, and vcpkg never runs — so the x-aws binary cache
is never exercised and Requirement 1.2 measures nothing. That is exactly
what happened on the first attempt at this measurement: every measured leg
logged "Bootstrap vcpkg and install dependencies: skipped".

It deliberately does NOT change any portfile or patch, so no port's ABI
hash moves: this forces an L1 miss without changing what gets built.
-->
