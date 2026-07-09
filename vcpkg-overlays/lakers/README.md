# `lakers` vcpkg overlay port

Provides EDHOC (RFC 9528) support for the coap-transport-security spec's
`oscore_bootstrap::edhoc` path, via [lake-rs/lakers](https://github.com/lake-rs/lakers).

## Why this port builds from source

`lakers`'s GitHub releases ship pre-built `lakers-c` static libraries only
for embedded-MCU targets (e.g. `lakers-c-crypto-cryptocell310-thumbv7em-none-eabihf.zip`).
There is no desktop/server binary (e.g. `x64-linux`) published anywhere. A
vcpkg overlay port that just downloads a release asset — the approach
originally sketched in `.kiro/specs/coap-transport-security/design.md` —
therefore doesn't work for the platform this project actually builds and
tests on. This port instead invokes `cargo build --release` against the
vendored FFI crate at `ffi/`, using `lakers`'s `crypto-rustcrypto` backend
(a pure-Rust, `std`-compatible backend intended for exactly this kind of
host build, per `crypto/Cargo.toml`'s own comment).

**This means a Rust toolchain (cargo + rustc) must be present on any
machine that installs this port.** It is not a default dependency of the
top-level `vcpkg.json` — it's behind an opt-in `edhoc` feature — so its
absence never affects the default build; `LAKERS_AVAILABLE` (the C++-side
compile-time gate) simply stays undefined, and `oscore_bootstrap::edhoc`
fails construction with `coap_credential_bootstrap_error` rather than
silently behaving as `static_provisioned` (see
`include/raft/coap_edhoc.hpp`).

## Why this wraps a new FFI crate instead of upstream `lakers-c`

`lakers-c` (the crate lakers itself publishes for C interop) only exposes
the **Initiator** role over FFI — there is no Responder binding, and no
`edhoc_exporter` binding either, in the version pinned here (v0.8.0).
Kythira's `coap_client` needs Initiator, but `coap_server` needs Responder,
so upstream's C bindings aren't sufficient on their own.

`ffi/` is a small (~500 line) crate authored for this integration,
depending directly on the `lakers` and `lakers-crypto` crates (not
`lakers-c`), exposing exactly the Initiator + Responder + exporter surface
Kythira needs, using ordinary `Box`-owned opaque handles rather than
`lakers-c`'s `#![no_std]` / `#[repr(C)]` state-struct-copying style (which
is aimed at microcontrollers without a heap — not a constraint on Kythira's
x86_64 Linux target). It implements the 3-message EDHOC exchange only
(no message_4 — RFC 9528 permits either side to treat the exchange as
complete after message_3), which is all `oscore_bootstrap::edhoc` needs.

See `ffi/src/lib.rs` for the full API and `ffi/tests/handshake.rs` for an
end-to-end test using the same test vectors as upstream `lakers`'s own
`test_handshake` (`lib/src/lib.rs`).

## Version pin

`ffi/Cargo.toml` pins `lakers`/`lakers-crypto` to git tag `v0.8.0`
(commit `32d09af75d44c771582c21a82831ee3a5dc8aecc`), inspected directly
while writing this port, rather than floating on a branch — `lakers` isn't
in the vcpkg registry so there's no baseline mechanism tracking it; the pin
is the only version-stability guarantee here.
