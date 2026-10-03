# `libcoap` overlay port

Provides [`obgm/libcoap`](https://github.com/obgm/libcoap) 4.3.5b, the CoAP
library behind the default CoAP transport.

## Why this overlay exists

libcoap has a registry port, and `vcpkg-configuration.json` already takes the
newer baseline's `openssl`, `cpp-httplib` and `c-ares` from a pinned
microsoft/vcpkg registry for their crash fixes. This directory is a verbatim
copy of that registry's libcoap port at 4.3.5b (baseline
`e182cb4dd2df2ab02f66a1aabd5f35bbdc9522c7`, git-tree `8e35de30`) plus a single
extra `PATCHES` line. vcpkg offers no way to patch a registry port in place.

## The bug

4.3.5a reworked `tls_verify_call_back()` in `src/coap_openssl.c` to check the
`i2d_X509()` return values, and in doing so moved the call that actually
serialises the peer certificate inside an `assert()`:

```c
if (base_buf) {
  /* base_buf2 gets moved to the end */
  assert(i2d_X509(x509, &base_buf2) > 0);
  (void)base_buf2;
  ... setup_data->validate_cn_call_back(cn, base_buf, length, ...)
```

vcpkg's release builds define `NDEBUG`, so the `i2d_X509()` call disappears and
`validate_cn_call_back` is handed an uninitialised `OPENSSL_malloc()` buffer.
Kythira's callback (`include/raft/coap_transport_impl.hpp`) runs `d2i_X509()`
on it, logs `Failed to parse ASN.1 certificate data`, and rejects the peer, so
every DTLS-PKI handshake with `verify_peer_cert` fails with an `unknown CA`
alert. `coap_certificate_revocation_test` is the case that caught it. The
4.3.5 release did not have this problem; upstream `develop` still does.

`fix-ndebug-i2d-x509-in-assert.patch` performs the serialisation
unconditionally and requires it to write exactly `length` bytes, falling into
the existing `X509_V_ERR_UNSPECIFIED` rejection branch otherwise.

## Keeping it in sync

`portfile.cmake` and `vcpkg.json` should differ from the registry port only by
the extra `PATCHES` entry and the comment header. An overlay overrides every
version of a package, so when the registry baseline in
`vcpkg-configuration.json` moves libcoap, re-copy the port from it. If the new
upstream release no longer wraps `i2d_X509()` in `assert()`, delete this
overlay and drop it from `vcpkg-configuration.json` rather than carrying a
patch that will fail to apply.
