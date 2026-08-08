# Alternate CoAP backends: libnyoci and cantcoap

The CoAP transport (`include/raft/coap_transport.hpp`) is backed by **libcoap**
by default (vcpkg port with the `dtls` feature, plus OSCORE/EDHOC via the
`lakers` overlay, block-wise transfer, multicast and ACE-OAuth). This document
compares two *alternate* backends.

**Both are now implemented** — `.kiro/specs/coap-transport-libnyoci/` and
`.kiro/specs/coap-transport-cantcoap/`. This document was written as a paper
comparison before either existed; the sections below record which of its
predictions survived contact with the code, because several did not.

| | **libnyoci** | **cantcoap** |
|---|---|---|
| Repo | [darconeous/libnyoci][nyoci] (formerly SMCP) | [staropram/cantcoap][cant] |
| License | MIT | BSD-2-Clause |
| Scope | Full RFC 7252 client/server stack | PDU codec only (build/parse one message) |
| Sockets / event loop | Provided | **Written here** (~150 lines) |
| Retransmission / dedup | Provided | **Written here**, over `pending_message` |
| Block-wise transfer | **Block2 only** — no Block1 at all | **Written here**: Block1 *and* Block2 |
| DTLS | **PSK + PKI, via its OpenSSL plugin** (`--enable-tls`); no RPK | **You wire it** (reuse `coap_security.hpp`) |
| OSCORE / EDHOC | Not built in — supplied by kythira's own `raft/oscore.hpp` | Not built in — could reuse `raft/oscore.hpp` |
| Build system | **autotools** | **none** (source files only) |
| vcpkg port shape | `vcpkg_configure_make` (hard) | vendored `CMakeLists` (easy) |
| Adapter size | Thin (bridge callbacks → futures) | Thicker, but far less than expected — see below |
| Tests | 39 cases | 22 cases |

**The trade-off in one line:** libnyoci is a *hard port + easy adapter*;
cantcoap is an *easy port + thicker adapter*. That held — but "hard adapter"
turned out to overstate it, for a reason worth reading: see "What cantcoap
actually cost" below. Neither ships OSCORE, so kythira's
existing `coap_security` / `coap_edhoc` security layer stays in place for both —
which was an argument for keeping security *above* whichever CoAP core is
chosen rather than delegating it to the library. Fact 3 below is what happened
to that argument on contact with the code: it is not implementable, and DTLS
ended up delegated to libnyoci's plugin after all.

## What shipped

```
vcpkg-overlays/libnyoci/                              overlay port (autotools)
  ├─ vcpkg.json
  ├─ portfile.cmake                                   pinned REF + SHA512
  ├─ 0001-drop-CODE_COVERAGE_RULES-substitution.patch
  └─ README.md                                        host tools, pin, patch rationale
include/raft/
  ├─ coap_transport_config.hpp                        libcoap-free shared configs
  └─ coap_transport_libnyoci_impl.hpp                 the adapter
tests/
  ├─ coap_libnyoci_concept_conformance_test.cpp
  └─ coap_libnyoci_integration_test.cpp               loopback round trips + Block2
.kiro/specs/coap-transport-libnyoci/                  requirements + design + tasks
```

cantcoap is still skeleton-only: `vcpkg-overlays/cantcoap/` carries placeholder
`REF`/`SHA512` and `include/raft/coap_transport_cantcoap_impl.hpp` has `TODO`
bodies.

## Three things the skeleton got wrong

Building the libnyoci backend for real turned up three facts the paper design
did not anticipate. All three are load-bearing for anyone evaluating cantcoap
next.

### 1. The two C libraries cannot share a translation unit

libcoap spells option numbers as object-like macros:

```c
#define COAP_OPTION_IF_MATCH 1   /* coap3/coap_pdu.h */
```

libnyoci spells them as enumerators:

```c
enum { COAP_OPTION_IF_MATCH = 1, ... };   /* libnyoci/coap.h */
```

Include libcoap first and the preprocessor rewrites the middle of libnyoci's
enum into `1 = 1,`. There is no include order that fixes it, and it is not
specific to libnyoci — any second CoAP library with the same identifiers will
collide the same way, cantcoap included.

The consequence is architectural, not cosmetic: **an alternate backend's header
cannot include `raft/coap_transport.hpp`**, because that header includes
`<coap3/coap.h>` whenever `LIBCOAP_AVAILABLE` is defined. The design document
had assumed the new adapter would reuse `pending_message`,
`block_transfer_state` and the config structs straight out of it.

The fix was to lift the genuinely library-neutral declarations —
`coap_client_config`, `coap_server_config`, `pending_message`,
`received_message_info`, `translate_legacy_fields()` — into
`include/raft/coap_transport_config.hpp`, which `coap_transport.hpp` now
includes. One definition of each, shared by every backend, with no libcoap type
anywhere in it. What stayed behind is what is genuinely libcoap-shaped:
`block_transfer_state` (holds a `coap_session_t*`), `coap_error_info` (keyed by
`coap_pdu_code_t`), and the `*_transport_types` bundles.

They still do not conflict at *link* time, which is what Requirement 2.4 asks
for; the conflict is purely at compile time, and a translation unit picks a
backend by which header it includes. That was verified rather than assumed — a
program calling into both archives links and runs — but with one caveat worth
knowing: the two export exactly one name in common, `coap_insert_option`. It is
survivable only because these are static archives, where the linker pulls an
object file solely to satisfy an undefined symbol and no translation unit can
reference both libraries. If either is ever built shared, or a second name
starts overlapping, that changes.

### 2. libnyoci has Block2 but no Block1

libnyoci's transaction layer requests the next block of a block-wise *response*
by itself (`nyoci-transaction.c`, "Preparing to request next block") — but only
when the transaction carries `NYOCI_TRANSACTION_ALWAYS_INVALIDATE`, which is
why the adapter sets that flag and guards every settle path with a `settled`
flag. There is no Block1 code path anywhere in the library: `COAP_OPTION_BLOCK1`
appears only in the option-name and option-type tables.

So the backend is asymmetric, and the adapter is explicit about it:

- **Large responses** work end to end. libnyoci drives the follow-up requests;
  the adapter accumulates the blocks. The *server* side has no libnyoci support
  at all, so the adapter slices responses itself — statelessly, serving each
  block straight out of the request's own Block2 option, so there is no
  per-peer transfer state to expire.
- **Large requests** cannot be sent. The adapter checks
  `nyoci_outbound_get_space_remaining()` before appending, and rejects the
  future with an error that names the missing Block1 support rather than
  truncating the payload or hanging until the timeout.

That matters most for InstallSnapshot, which is the one Raft RPC with an
unbounded request body. A deployment that ships large snapshots should use the
libcoap backend.

### 3. The security layer has no transport-neutral seam — so DTLS came from libnyoci

The plan was to keep kythira's `coap_security_provider` *above* a plain-CoAP
libnyoci core. That is not possible as the interface stands: every method is
expressed in libcoap types —

```c++
virtual auto configure_session(coap_context_t* ctx) -> void = 0;
virtual auto protect(coap_pdu_t* pdu) -> coap_pdu_t*;
virtual auto create_client_session(coap_context_t*, const coap_address_t*, ...) -> coap_session_t*;
```

— and `protect`/`unprotect` are identity passthroughs precisely *because* every
mode (DTLS, OSCORE, EDHOC) is implemented below libcoap's PDU API rather than as
a byte-level transform the adapter could call.

**What closed the gap for DTLS was libnyoci's own OpenSSL plugin**, which turned
out to be richer than the comparison table suggested:

- `nyoci_plat_tls_set_context()` takes a raw `SSL_CTX*` — the type really is
  `typedef struct ssl_ctx_st* nyoci_plat_tls_context_t` — so **PKI is reachable
  by configuring the context directly**, rather than needing per-mode plugin
  support.
- **PSK is first-class**: `set_client_psk_callback`, `set_server_psk_callback`,
  `set_psk_hint`.
- The `coaps://` scheme selects the DTLS session type by itself, through
  `nyoci_session_type_from_uri_scheme()`.

So `dtls_psk` and `dtls_pki` work, end to end, over a real handshake. Three
costs worth knowing:

- DTLS *configuration* forks per backend. The config surface does not —
  `coap_client_config` and `translate_legacy_fields()` are shared, which is half
  of why fact 1's refactor was worth doing.
- Upstream labels its TLS support experimental, defaults it off, and calls
  OpenSSL 1.x-era APIs that are deprecated-but-present in 3.x.
- **DTLS-PKI needs small certificates.** libnyoci reads every inbound datagram
  into a fixed `char packet[NYOCI_MAX_PACKET_LENGTH+1]` — 1033 bytes by default
  — and that buffer applies to DTLS handshake records too. An RSA-2048
  certificate flight overruns it, is silently truncated, and the handshake
  stalls: the request just times out, with nothing in any log to say why. ECDSA
  P-256 is a few hundred bytes and works. This was found the hard way, by
  writing the PKI test with RSA-2048 first.

**What is still refused, with specific reasons rather than a downgrade:**

- **DTLS-RPK.** Raw public keys (RFC 7250) need the peer to negotiate a
  non-X.509 certificate type, and OpenSSL only added the certificate-type
  extensions in 3.2. The plugin exposes nothing but an `SSL_CTX`.
- **The EDHOC bootstrap**, for now: the handshake is already transport-neutral,
  but carrying its messages needs a `.well-known/edhoc` exchange this backend
  does not offer. Static OSCORE provisioning works today.

Refusing at construction is the only safe answer for both: silently downgrading
a node that asked for encryption to plaintext Raft traffic is strictly worse
than not starting.

**OSCORE is no longer on that list.** It was, and the reason it came off is
worth recording, because the first assessment was wrong in an instructive way.
libnyoci ships no OSCORE — but neither did kythira: `oscore_provider` delegates
entirely to libcoap, so there was no AES-CCM, COSE or key derivation anywhere in
the tree. The obvious-sounding "lift a byte-level `protect(bytes)` /
`unprotect(bytes)` seam out of `coap_security_provider`" therefore had *nothing
to lift*; it was an acquire-an-implementation project rather than a refactor.
`include/raft/oscore.hpp` is that implementation: RFC 8613 over CoAP message
bytes, no CoAP library involved, verified against every Appendix C test vector
including the C.4 request and C.7 response byte for byte. Object security is
now the one security mode that is genuinely backend-independent, and cantcoap
would inherit it for free.

## What cantcoap actually cost

The spec predicted the adapter would be "the bulk of the work". It was the bulk,
but it was *smaller than the libnyoci adapter*, and the reason is instructive:
**almost everything above the socket already existed.**

| Concern | cantcoap gives | Where it came from |
|---|---|---|
| PDU encode/parse | `CoapPDU` | cantcoap |
| UDP socket + loop | nothing | new, ~150 lines |
| Retransmit / dedup | nothing | `pending_message`, `received_message_info` |
| Block-wise | nothing | `block_option` + our own sequencing |
| OSCORE | nothing | `oscore::security_context` — **inherited free** |

That last row is the payoff of a decision made for the *other* backend.
`raft/oscore.hpp` was written for libnyoci and made transport-neutral on
principle, working on CoAP message bytes rather than any library's types. This
backend picked it up with **no changes at all**, because it already owns the
bytes on both sides of the socket. The principle paid for itself the first time
it was tested.

Two structural conveniences also carried over: the `coap_transport_config.hpp`
split (see fact 1) meant cantcoap had a libcoap-free home for the shared configs
waiting for it, and `pending_message` / `received_message_info` / `block_option`
were already the right shapes.

**What cantcoap does *not* get** is DTLS. libnyoci at least had a plugin to
drive; cantcoap is cleartext-only with nothing behind it, so channel security
would mean running an OpenSSL DTLS BIO over this backend's own socket —
handshake, retransmission, cookie exchange — which is a transport in its own
right. It is refused at construction, pointing at OSCORE instead. That is a
genuinely different refusal from libnyoci's RPK one: there the surface existed
and OpenSSL lacked the feature; here the surface does not exist.

One prediction that scored well: cantcoap really can do Block1, and does. That
is the one capability it has that libnyoci does not, exactly as this document
guessed — owning the block layer means you can implement the half libnyoci
omits.

## Autotools port: what it actually took

The port needs the autotools chain on the host **plus `autoconf-archive`** —
`configure.ac` calls `AX_PTHREAD`, which lives there. Three things beyond the
skeleton's guess:

- **The tag is unusable.** libnyoci's only tag (`0.07.00rc1`, also
  `refs/tags/latest-release`) is the 2017 *initial* release. The 35 commits
  since carry the fuzzing fixes, a URI-corruption fix in
  `nyoci_outbound_set_uri()`, and the signature change that gave
  `nyoci_inbound_get_path()` its maximum-length argument — which the adapter
  calls, so it does not even compile against the tag. The port pins master's
  head (`11a6e1b`, 2019).
- **A patch is required.** libnyoci was written against a pre-2018
  autoconf-archive whose `AX_CODE_COVERAGE` `AC_SUBST`'d a `CODE_COVERAGE_RULES`
  make fragment. Modern autoconf-archive emits those rules through
  `aminclude_static.am` and never substitutes the variable, so all eleven
  `@CODE_COVERAGE_RULES@` interpolations survive into the generated Makefiles
  and `make` dies with `*** missing separator`. The port drops them.
- **`--disable-dependency-tracking` is mandatory**, not optional: automake's
  depfile bootstrap fails in the vcpkg build tree. Upstream's own CI passes the
  same flag (commit `1330755`).

The skeleton's guessed configure flags (`--disable-tests`, `--without-examples`)
do not exist. The real ones are `--disable-examples --disable-nyocictl
--disable-plugtest`.

## Threading

libnyoci's API is not thread-safe, most of it is only legal from inside one of
its own callbacks, and its "current instance" is a pthread-key thread-local. So
each client and each server owns exactly one `nyoci_t` driven by exactly one
`std::jthread`, and *every* libnyoci call happens on that thread. `send_*` does
not touch libnyoci at all: it serializes, parks a record on a queue and returns
the future; the loop thread starts the transaction and later fulfils the promise
from the response callback.

libnyoci has no way to interrupt its own `poll()` from another thread, so the
loop uses a 20 ms wait budget. That is what bounds send latency and shutdown
latency both.

## Build integration

Both backends follow the graceful-degradation pattern libcoap already uses in
the root `CMakeLists.txt` (probe → set `*_FOUND` → define `*_AVAILABLE`,
warn-and-skip when absent).

### Opt-in vcpkg feature (root `vcpkg.json`)

`coap-libnyoci` is wired up and mirrors how `edhoc`/`ion` gate `lakers`/`ion-c`,
so a default install never fetches it:

```sh
cmake -S . -B build -DVCPKG_MANIFEST_FEATURES=coap-libnyoci
```

### CMake probe (root `CMakeLists.txt`, next to the libcoap block)

libnyoci is autotools + pkg-config, so discovery is `pkg_check_modules` — the
same fallback path system-libcoap uses, *not* `find_package(... CONFIG)`. The
probe is gated on `CONFIG_COAP_TRANSPORT_LIBNYOCI` (Kconfig, default `n`) and
does **not** depend on `CONFIG_COAP_TRANSPORT`: the backends are additive.

## Concept conformance

`coap_libnyoci_client<Types>` / `coap_libnyoci_server<Types>` are templated on
`Types` and constrained by `kythira::transport_types<Types>`, and satisfy
`kythira::network_client` / `kythira::network_server` — asserted in
`tests/coap_libnyoci_concept_conformance_test.cpp`, which compiles and passes
*with or without* libnyoci present, because the adapter keeps its full concept
surface either way. On the wire the two backends are interoperable by
construction: same `/raft/<rpc>` POST resources, same 2.05 responses, same
Content-Format/Accept negotiation through `serializer_registry`, same 4.15/4.06
answers for an unsatisfiable format.

The one thing no test can assert is a libcoap client against a libnyoci server
*in one process*: see fact 1 above. Cross-backend interop needs two processes,
which is what a container harness would be for.

## Recommendation

- Pick **libnyoci** if the goal is a genuine second *full* CoAP stack with the
  least adapter code, DTLS-PSK/PKI is enough security, request payloads stay
  under one datagram, and the autotools port cost is acceptable. It is the
  closer analog to today's libcoap integration and it works today.
- Pick **cantcoap** if you want full control of the wire behaviour, need
  **Block1** (it has it; libnyoci does not), and object security is enough.
  It inherited fact 1 exactly as predicted — its option enum collides with
  libcoap's macros the same way — and it inherited the OSCORE implementation
  for free. It cannot do DTLS.

**If you need DTLS, that narrows it to libcoap or libnyoci. If you need Block1,
that narrows it to libcoap or cantcoap.** Only libcoap does both, which is a
reasonable argument for it remaining the default.

[nyoci]: https://github.com/darconeous/libnyoci
[cant]: https://github.com/staropram/cantcoap
