# `stdexec` vcpkg overlay port

Provides [NVIDIA/stdexec](https://github.com/NVIDIA/stdexec), the reference
implementation of C++26's `std::execution` (P2300), for the
`stdexec-future-backend` spec's second, sender/receiver-based
`kythira::Future`/`Promise`/`Executor` implementation.

## Why this is an overlay port instead of a plain `vcpkg.json` dependency

`stdexec` genuinely is present in the upstream vcpkg registry — but only as
of a commit newer than this project's pinned `builtin-baseline`
(`9a7f7340a6c5f11f24c3d59f85e07143feb84e06`, 2025-11-17; `stdexec`'s port
was added 2026-05-26). Bumping `builtin-baseline` project-wide to pick up
one new port would re-resolve the version of every other dependency
(Folly, Boost, AWS SDK, etc.) against a newer registry snapshot — a much
larger, riskier change than adding one optional library, and not something
to do just to avoid an overlay port. This directory is therefore a direct
copy of the upstream registry's own port files (`portfile.cmake`,
`vcpkg.json`, and its four patches) at the commit inspected during this
spec's Phase 0 spike — unmodified, not a fork with local changes — so it
behaves identically to what a `builtin-baseline` bump would eventually
provide, without the blast radius.

## Build requirements

Unlike the `lakers` overlay port, this one needs no extra toolchain beyond
what the project already requires (CMake, a C++ compiler) — `stdexec`
itself is header-only. Its build step does need network access during
`vcpkg install` (via CPM, it fetches `rapids-cmake`, the `icm` regex
helper, and the P2300 `execution.bs` spec text as part of its own
`CMakeLists.txt`), the same as any other vcpkg port that isn't already
binary-cached.

## The `tbb` feature

This project requests the `tbb` feature (`{"name": "stdexec", "features":
["tbb"]}` in the top-level `vcpkg.json`). Without it, code using
`exec::single_thread_context` (and likely other `exec/` scheduler types)
fails to *link*, not compile — `undefined reference to
tbb::detail::r1::execution_slot` — because some `exec/` headers reference
TBB symbols unconditionally regardless of whether the `STDEXEC_ENABLE_TBB`
CMake option was on when `stdexec` itself was built. Requesting the `tbb`
vcpkg feature makes vcpkg provision a hermetic `libtbb` build alongside
`stdexec` rather than depending on whatever the host system happens to
have installed already (confirmed present on the spike's development
machine, but not something to rely on implicitly for CI reproducibility).

## Version pin

Upstream commit `NVIDIA/stdexec@fee4d651494014610a277540f209cae56011e47f`,
vcpkg port version-date `2026-05-25` — the exact commit `vcpkg_from_github`
resolves in `portfile.cmake`, inspected directly (compiled and run a probe
program against the real installed headers with both `g++-13` and
`clang++-18`) during this spec's Phase 0 spike; see `spike-notes.md` in
`.kiro/specs/stdexec-future-backend/` for the full findings.
