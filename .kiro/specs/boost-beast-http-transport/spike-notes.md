# Spike Notes — Boost.Beast HTTP Transport

**Date**: 2026-07-26
**Boost version**: 1.89.0 (`BOOST_VERSION 108900`, vcpkg `boost-beast`/
`boost-asio` at this project's `builtin-baseline`
`9a7f7340a6c5f11f24c3d59f85e07143feb84e06`, confirmed via `vcpkg search
boost-beast` and `vcpkg_installed/x64-linux/include/boost/version.hpp`
after a scratch install — matches the Boost version already used
elsewhere in this project's `vcpkg_installed`, not a separate/older
resolution)
**Compilers validated**: `clang++-18` and `g++-13`, both `-std=c++23`,
default optimization level (not this project's actual `-O3 -DNDEBUG`
release flags, extracted from `build-clang/compile_commands.json` for
reference but not used here — the behaviors under test, `expires_after`
cancellation semantics and coroutine composition, are not the kind of
thing `-O3` has previously been found to affect in this project, unlike
the GCC13+`stdexec` `-O3`-specific ICE documented elsewhere in
`doc/TODO.md`; re-validate at `-O3` if that assumption turns out wrong
during actual implementation)

## Method

Two throwaway standalone `.cpp` files (not part of the CMake build),
compiled and run directly against a scratch `vcpkg install` of
`boost-beast`/`boost-asio`/`openssl` at this project's exact
`builtin-baseline`, confirming each `requirements.md` Requirement 18
assumption empirically rather than from documentation alone.

## Findings

### 1. `beast::tcp_stream::expires_after` genuinely cancels an in-flight read — confirmed, no correction needed

Requirement 18.2's concern (Beast's own documentation describes
cancellation of already-issued syscalls as best-effort/OS-dependent) does
not manifest as a practical problem at this version. Test: a loopback
"server" coroutine that accepts a connection and then holds it open for
10s without reading or writing anything; a client coroutine that connects,
sets `expires_after(500ms)`, then issues `async_read_some` expecting it to
hang. Result on both compilers:

```
read_ec=The socket was closed due to a timeout elapsed_ms=500
```

The read returned in ~500ms (matching the configured expiry, not the
server's 10s keep-open window) with a clear, specific error
(`beast::error::timeout`-equivalent, surfaced through
`boost::system::error_code`). **Resolution**: `design.md`'s Property 4
("Timeout Bounds the Whole RPC, Not Each Step") and Requirement 10.1-10.2
proceed as designed — `expires_after` set once per RPC is sufficient, no
per-step manual timer/cancellation-signal fallback is needed.

### 2. `net::awaitable<T>` + `co_spawn` composition compiles cleanly on both compilers — settles the Phase 0 spike's open composition-style question in its favor

`design.md`'s Phase 0 section left the coroutine/completion-handler
composition style as spike-dependent, noting `awaitable<T>` support
quality "has historically varied across compiler versions more than plain
callback composition does." The same spike program (Finding 1, using
`co_await`/`net::co_spawn`/`net::use_awaitable`/`net::redirect_error`
throughout) compiled without warnings or errors on both `clang++-18` and
`g++-13` at `-std=c++23`. **Resolution**: use `net::awaitable<T>` +
`net::co_spawn` for the composition style referenced throughout
`design.md`'s Phase 2.5/3/4 code samples (which were written in plain
`.thenValue`-chain style independent of this choice — the coroutine
composition question is about how `async_connect_kf`/`async_write_kf`/
`async_read_kf` themselves are implemented *inside*, not the
`.thenValue`/`.thenError` chain their callers compose, which stays
`future_transformable`-based regardless of this choice).

### 3. `boost::asio::ssl::context::native_handle()` provides direct `SSL_CTX*` access — the existing cert-loading code is reusable as-is, no adaptation layer needed

Requirement 18.3's open question: whether `http_transport_impl.hpp`'s
existing certificate-loading code (`load_client_certificates`/
`validate_certificate_files`, written against raw `SSL_CTX*`-level OpenSSL
calls like `SSL_CTX_set_min_proto_version`) can be reused as-is against
`boost::asio::ssl::context`. Confirmed: `ssl::context::native_handle()`
returns the underlying `SSL_CTX*` directly, and calling
`SSL_CTX_set_min_proto_version` on it through that handle compiles and
links cleanly, with `beast::ssl_stream<beast::tcp_stream>` constructible
from the resulting `ssl::context`. **Resolution**: `design.md`'s Phase 5
("no new certificate-loading logic is invented, only re-pointed at a
different context type") proceeds as designed — reuse the existing
functions' logic directly, retargeted from whatever type they currently
take (`httplib::SSLServer&`/raw `SSL_CTX*`) to
`boost::asio::ssl::context&`, calling `.native_handle()` wherever they
need the raw pointer. No adaptation layer.

## Conclusions

- **No design corrections needed** — unlike `boost-future-backend`'s spike
  (which found a real `design.md` error, `BOOST_THREAD_PROVIDES_EXECUTORS`
  not auto-defining at version 4 as originally assumed from source
  inspection), all three of this spike's questions confirmed the
  `requirements.md`/`design.md` assumptions as written.
- **Composition style resolved**: `net::awaitable<T>` + `net::co_spawn`,
  for the three primitive adaptors' own internal implementation
  (`design.md` Phase 2.5).
- **Boost release validated**: 1.89.0. **Minimum compiler versions
  validated**: `clang++-18`, `g++-13` (both already this project's
  existing CI matrix minimums — no new compiler-version floor introduced
  by this feature).
- Not yet spiked (deferred to actual implementation, per `design.md`'s own
  scoping): connection-pool strand behavior under real concurrent load,
  and the exact shape of adapting `load_client_certificates` to take
  `ssl::context&` rather than confirming only that the underlying
  `native_handle()` mechanism exists.
