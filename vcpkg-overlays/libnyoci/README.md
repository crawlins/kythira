# libnyoci overlay port

The **alternate CoAP backend** to libcoap for kythira's `coap_transport`.
libnyoci ([darconeous/libnyoci][repo], formerly SMCP) is a full-stack RFC 7252
client/server library in C — it owns the socket, the message/token layer,
retransmission of confirmable messages, duplicate suppression, observe, async
responses and Block2 transfer, so the C++ adapter over it
(`include/raft/coap_transport_libnyoci_impl.hpp`) stays comparatively thin.

There is no libnyoci port in the vcpkg registry, so this overlay builds it from
source, the same way `vcpkg-overlays/ion-c` and `vcpkg-overlays/lakers` do for
their upstreams.

## Host build tools

Unlike the CMake overlays, this one needs the autotools chain on the host,
because `vcpkg_configure_make(... AUTOCONFIG)` re-runs `autoreconf` (GitHub
archive tarballs omit the generated `./configure`):

- `autoconf`, `automake`, `libtool`
- **`autoconf-archive`** — `configure.ac` calls `AX_PTHREAD`, which lives there.
  Without it `autoreconf` leaves `AX_PTHREAD` unexpanded and `./configure` dies
  with a shell syntax error.
- `pkg-config`

On Debian/Ubuntu: `apt-get install autoconf automake libtool autoconf-archive pkg-config`.

## The pin

`REF` is master's head (`11a6e1b`, 2019-04-15), **not** the single published
tag. `refs/tags/latest-release` points at `0.07.00rc1`, the 2017 *initial*
release; the 35 commits since it include the fuzzing fixes (null dereference in
`nyoci-list`, a missing-parens option-length bug), the URI corruption fix in
`nyoci_outbound_set_uri()`, and the signature change that added the
maximum-length argument to `nyoci_inbound_get_path()` — which the adapter
calls, so it does not compile against the tag at all. Upstream has been quiet
since 2019, so this commit is effectively the current release.

To re-pin, change `REF` in `portfile.cmake`, then regenerate `SHA512`:

```sh
curl -sL -o libnyoci.tar.gz \
  https://github.com/darconeous/libnyoci/archive/<REF>.tar.gz
sha512sum libnyoci.tar.gz
```

or run the install once and copy the "Actual hash" vcpkg prints on the failing
fetch:

```sh
vcpkg install libnyoci --overlay-ports=vcpkg-overlays
```

## The patch

`0001-drop-CODE_COVERAGE_RULES-substitution.patch` removes the eleven
`@CODE_COVERAGE_RULES@` interpolations from libnyoci's `Makefile.am` files.
libnyoci was written against a pre-2018 autoconf-archive whose
`AX_CODE_COVERAGE` `AC_SUBST`'d that variable; modern autoconf-archive
(serial ≥ 25) emits the rules via `AX_ADD_AM_MACRO_STATIC` /
`aminclude_static.am` and never substitutes it, so every generated `Makefile`
keeps a literal `@CODE_COVERAGE_RULES@` line and `make` fails with
`*** missing separator`. `configure.ac`'s `m4_ifdef` fallback does not save us:
it only fires when the macro is *absent*, and it cannot be absent on a host
that has the archive installed — which this port requires anyway, for
`AX_PTHREAD`.

kythira never builds libnyoci with coverage instrumentation, so dropping the
lines costs nothing.

## Why this port looks different from `ion-c`

`ion-c` and `stdexec` are CMake projects, so their ports use
`vcpkg_cmake_configure`. **libnyoci is an autotools project**, so this port
uses `vcpkg_configure_make` + `vcpkg_install_make` + `vcpkg_fixup_pkgconfig`
instead. The consequence downstream: **no CMake config package**. Discovery is
pkg-config (`pkg_check_modules(LIBNYOCI QUIET libnyoci)`), the same fallback
path the root `CMakeLists.txt` already has for system libcoap — not
`find_package(... CONFIG)`.

## Consuming it

It is gated behind the opt-in `coap-libnyoci` feature in the root `vcpkg.json`
(mirroring how `edhoc`/`ion` gate lakers/ion-c), and the root `CMakeLists.txt`
probe sets `LIBNYOCI_AVAILABLE` — structurally identical to the
`LIBCOAP_FOUND` → `LIBCOAP_AVAILABLE` block:

```sh
cmake --preset default -DVCPKG_MANIFEST_FEATURES=coap-libnyoci
```

See `.kiro/specs/coap-transport-libnyoci/` for the spec and
`doc/coap_library_alternatives.md` for the libcoap-vs-libnyoci trade-off.

## Known upstream limitations

libnyoci implements **Block2** (block-wise *responses*) but has **no Block1**
at all — there is no code path for a block-wise request payload. A request
larger than the negotiated packet budget cannot be sent, and the adapter
rejects it with a descriptive error rather than truncating. See
`coap_transport_libnyoci_impl.hpp`.

[repo]: https://github.com/darconeous/libnyoci
