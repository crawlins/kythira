# libcoap overlay port

The registry's own `libcoap` port at kythira's `builtin-baseline`
(`9a7f734`, libcoap 4.3.5), copied unchanged except for one configure
option: **`-DENABLE_OSCORE=OFF`**.

## Why

Kythira does OSCORE (RFC 8613) itself, in `include/raft/oscore.hpp`, on every
CoAP backend. That is what lets each Raft group have its own Security Context
(`.kiro/specs/coap-transport-multi-raft/` tasks 9–11): a context per
(peer, group), derived on demand from the `kid context` of a peer's first
request, under bounds.

A libcoap built with its own OSCORE cannot carry that. In `coap_dispatch()`
(`src/coap_net.c`) it decrypts every incoming request that has an OSCORE
option, before any resource handler runs. When no libcoap OSCORE context
matches, it drops the request ("OSCORE: Not enabled", or "PDU could not be
decrypted"). Its server holds a fixed set of contexts, and libcoap 4.3.5
offers no hook to derive one when an unknown `kid context` arrives. That hook
(`coap_oscore_register_external_handlers()`) exists only on libcoap's
`develop` branch, in no release.

Built with OSCORE off, libcoap treats option 9 as an ordinary option once the
context registers it (`coap_register_option()`), and requests reach Kythira's
handler intact. The libcoap backend refuses to start in OSCORE mode against a
libcoap that has its own OSCORE compiled in, naming this overlay, rather than
silently losing every request.

## Updating

When the baseline moves, re-copy `portfile.cmake`, `vcpkg.json` and the patch
from `ports/libcoap/` at the new baseline. Then keep `-DENABLE_OSCORE=OFF` and
bump `port-version`. Drop this overlay once a libcoap release ships the
external find handler and the backend is moved onto it.
