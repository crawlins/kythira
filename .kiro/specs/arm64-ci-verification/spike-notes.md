# Spike Notes — vcpkg `arm64-linux` Dependency Availability

**Requirement**: 10 (`requirements.md`)
**Date**: July 15, 2026
**Conclusion**: No blocking gaps found. Safe to proceed to Wave 1.

## Methodology (and why it differs from "run `vcpkg install`")

Requirement 10 asks for `vcpkg install --triplet arm64-linux` to actually be
run against the full dependency set. The implementation environment
available for this spike is an x86_64 sandbox with no functioning container
runtime (`docker version` succeeds for the client, but there is no daemon
socket — `dial unix /var/run/docker.sock: connect: no such file or
directory`) and no arm64 hardware or QEMU user-mode emulation configured.
Actually building Folly, the AWS SDK for C++, and Boost under QEMU emulation
would also likely exceed any single command's practical timeout even if
emulation were set up.

Given that, this spike instead performed **static verification against
vcpkg's own port manifests**, at the exact commit pinned by this project's
`builtin-baseline` (`9a7f7340a6c5f11f24c3d59f85e07143feb84e06`):

1. Shallow-cloned `microsoft/vcpkg` and checked out that exact commit.
2. Read each dependency's `ports/<name>/vcpkg.json` `"supports"` field —
   vcpkg's own boolean platform-expression gate that determines whether a
   port is even attempted for a given triplet. A port lacking `arm64` (or
   excluding it) in its `supports` expression is vcpkg's own documented
   signal that it can't be built for `arm64-linux`.
3. Confirmed `triplets/community/arm64-linux.cmake` exists and is
   structurally identical to the built-in `triplets/x64-linux.cmake` (same
   `VCPKG_CRT_LINKAGE`, `VCPKG_LIBRARY_LINKAGE`, `VCPKG_CMAKE_SYSTEM_NAME` —
   only `VCPKG_TARGET_ARCHITECTURE` differs), so the triplet itself carries
   no arm64-specific quirks.
4. Checked the two Kythira-authored overlay ports
   (`vcpkg-overlays/lakers`, `vcpkg-overlays/stdexec`) for the same
   `"supports"` restriction.

This is weaker evidence than an actual successful build log, but it is the
same gate vcpkg itself uses to refuse an install outright — if any of these
dependencies excluded arm64, `vcpkg install --triplet arm64-linux` would
fail immediately with an "unsupported" message before attempting to compile
anything. **This spike does not prove the build succeeds; it proves nothing
here is turned away at the door.** The genuinely empirical confirmation is
Wave 3 (Tasks 7–8), where the dependency set is actually installed and built
on a native `ubuntu-24.04-arm` GitHub-hosted runner as part of enabling the
real CI job — that is the point at which a real compile failure, if one
exists despite a clean `supports` expression, will surface.

## Findings — main `vcpkg.json` dependencies

| Dependency | `supports` expression | arm64-linux? | Notes |
|---|---|---|---|
| `boost-algorithm` | *(none — unrestricted)* | ✅ | |
| `boost-asio` | *(none)* | ✅ | |
| `boost-json` | *(none)* | ✅ | |
| `boost-system` | *(none)* | ✅ | |
| `boost-test` | `!uwp` | ✅ | Only excludes UWP |
| `boost-thread` | *(none)* | ✅ | |
| `cpp-httplib` | `!x86 & !arm32` | ✅ | Explicitly excludes 32-bit x86 and 32-bit ARM, which by construction *allows* `arm64` |
| `folly` | `(windows & x64 & !uwp & !mingw) \| (!windows & !android & (x64 \| arm64))` | ✅ | Non-Windows, non-Android branch explicitly lists `arm64` as a supported architecture alongside `x64` |
| `libcoap` (feature `dtls`) | *(none)*; `dtls` feature depends only on `openssl` | ✅ | |
| `openssl` | *(none)* | ✅ | |
| `poco` (feature `net`) | `!uwp` | ✅ | |
| `aws-sdk-cpp` (features `acm-pca`, `autoscaling`, `ec2`, `iam`, `s3`, `sts`) | *(none, on the port and every one of these six features)* | ✅ | |
| `libssh2` | *(none)* | ✅ | |
| `stdexec` (feature `tbb`) | *(none on stdexec itself; overlay port, see below)* | ✅ | `tbb` feature pulls in `tbb` port, which has `supports: !uwp` only |

## Findings — overlay ports (`vcpkg-configuration.json`)

| Overlay port | `supports` expression | arm64-linux? | Notes |
|---|---|---|---|
| `vcpkg-overlays/lakers` (optional `edhoc` feature) | `!(windows \| uwp)` | ✅ | No architecture restriction at all; builds via `cargo build --release` against the host's native Rust target. `aarch64-unknown-linux-gnu` is a Rust Tier 1 target, and `dtolnay/rust-toolchain@stable` installs it the same way it installs `x86_64-unknown-linux-gnu` — this is a very well-trodden path (Rust's own CI matrix includes native aarch64 Linux), not something specific to this project's overlay port |
| `vcpkg-overlays/stdexec` | *(none — unrestricted)* | ✅ | Depends on `vcpkg-cmake`/`vcpkg-cmake-config` (host tools, triplet-independent by design) and, via the `tbb` feature, on `tbb` (`supports: !uwp`, confirmed above) |

## Conclusion

Every dependency in the current `vcpkg.json`, both overlay ports, and the
`arm64-linux` triplet definition itself declare (or fail to declare any
restriction against) `arm64` support at the pinned `builtin-baseline`. No
baseline bump, overlay-port patch, or upstream issue is required before
proceeding.

**No blocking follow-up identified.** Wave 1 (CMake triplet
parameterization) may proceed. The residual risk this spike does not cover
— an actual build failure specific to this project's exact dependency
versions/feature combination, uncaught by the `supports` gate — is exactly
what Task 8's "verify green" step on the real `ubuntu-24.04-arm` CI runner
is for.
