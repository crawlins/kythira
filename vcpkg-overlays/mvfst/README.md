# `mvfst` overlay port

Provides [`facebook/mvfst`](https://github.com/facebook/mvfst), Facebook's IETF
QUIC implementation, to vcpkg. kythira never includes mvfst headers itself — it
arrives as a transitive dependency of `proxygen`, which backs the opt-in
Proxygen HTTP transport.

## Why this overlay exists

Unlike the project's other overlay ports, this one is **not** filling a gap in
the vcpkg registry. `mvfst` has a perfectly good registry port, and this
directory is a verbatim copy of it at the baseline-resolved version, plus a
single `PATCHES` line.

The overlay exists only to carry
`0001-include-cstdlib-for-std-abort.patch`, because vcpkg offers no way to patch
a registry port in place.

## The bug

`quic/common/third-party/optional.h` is mvfst's vendored copy of
[Sedeniono/tiny-optional](https://github.com/Sedeniono/tiny-optional). Four
`value()` overloads report a failed precondition and then call `std::abort()`:

```cpp
if (!has_value()) {
  std::fprintf(stderr, "FATAL ERROR: ... called on empty optional\n");
  std::abort();
}
```

The file includes `<cstdio>` for `std::fprintf` but never includes `<cstdlib>`
for `std::abort`. That is a deliberate bet by upstream rather than a missed
include — the comment directly beneath the include block spells it out:

> In principle the following headers are required, but we rely on the standard
> header `<optional>` to include the necessary pieces from the omitted headers.
> This is a build performance optimization, especially when using gcc's
> libstdc++, which includes certain internal smaller headers directly.

libstdc++ 13's `<optional>` transitively declared `std::abort`; libstdc++ 14's
does not. On g++ 14 the port fails within seconds of starting its first debug
compile:

```
quic/common/third-party/optional.h:1591:14: error: 'abort' is not a member of 'std'
```

— four times, once per overload.

**CI does not see this.** The `Build & Test` matrix uses `g++-13` and
`clang++-18`, both of which still satisfy the transitive include. It reproduces
on any toolchain shipping libstdc++ 14 or newer; Debian 13 (trixie) is the case
that surfaced it, since it ships g++ 14 and has no `g++-13` package.

The patch adds the one missing include. It is additive, so it changes nothing
for the compilers that were already building this fine.

## Keeping it in sync

Treat this as a copy, not a fork. `portfile.cmake` and `vcpkg.json` should
differ from the registry port at the same version by exactly two things: the
`PATCHES` block, and the explanatory comment header.

`vcpkg.json`'s `version-string` must match the version that the root
`vcpkg.json`'s `builtin-baseline` resolves `mvfst` to — currently
`2025.05.19.00`, the same release train as `folly`, `fizz`, `wangle` and
`proxygen`. This matters more than it looks: **an overlay port overrides every
version of a package regardless of the baseline**, so if the baseline moves and
this file does not, vcpkg will quietly build a stale mvfst against a newer folly
and the ABI mismatch will surface as a link or runtime failure far from here.

When the baseline bumps `mvfst`:

1. Copy `portfile.cmake` and `vcpkg.json` fresh from `${VCPKG_ROOT}/ports/mvfst`
   (or from the versioned copy under
   `${VCPKG_ROOT}/buildtrees/versioning_/versions/mvfst/` that the baseline
   actually resolves to — these differ whenever the vcpkg checkout is ahead of
   the pinned baseline, which is the normal case).
2. Re-add the `PATCHES` line and this port's comment header.
3. Check whether the new mvfst still needs the patch — if upstream has added the
   include, **delete this overlay entirely** and drop it from
   `vcpkg-configuration.json` rather than carrying a no-op patch that will fail
   to apply.
