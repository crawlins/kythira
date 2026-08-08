# Alternate CoAP backends: libnyoci vs cantcoap

The CoAP transport (`include/raft/coap_transport.hpp`) is backed by **libcoap**
by default (vcpkg port with the `dtls` feature, plus OSCORE/EDHOC via the
`lakers` overlay, block-wise transfer, multicast and ACE-OAuth). This document
compares two *alternate* backends.

**libnyoci is implemented** (`.kiro/specs/coap-transport-libnyoci/`); cantcoap
remains a skeleton for comparison (`.kiro/specs/coap-transport-cantcoap/`).

| | **libnyoci** (implemented) | **cantcoap** (skeleton) |
|---|---|---|
| Repo | [darconeous/libnyoci][nyoci] (formerly SMCP) | [staropram/cantcoap][cant] |
| License | MIT | BSD-2-Clause |
| Scope | Full RFC 7252 client/server stack | PDU codec only (build/parse one message) |
| Sockets / event loop | Provided | **You write it** |
| Retransmission / dedup | Provided | **You write it** (reuse `pending_message`) |
| Block-wise transfer | **Block2 only** — no Block1 at all | **You write it** (reuse `coap_block_option.hpp`) |
| DTLS | Optional OpenSSL plugin (`--enable-tls`) | **You wire it** (reuse `coap_security.hpp`) |
| OSCORE / EDHOC | Not built in | Not built in |
| Build system | **autotools** | **none** (source files only) |
| vcpkg port shape | `vcpkg_configure_make` (hard) | vendored `CMakeLists` (easy) |
| Adapter size | Thin (bridge callbacks → futures) | Thick (own the whole transport) |

**The trade-off in one line:** libnyoci is a *hard port + easy adapter*;
cantcoap is an *easy port + hard adapter*. Neither ships OSCORE, so kythira's
existing `coap_security` / `coap_edhoc` security layer stays in place for both —
which is an argument for keeping security *above* whichever CoAP core is chosen
rather than delegating it to the library. See "Security" below for how that
argument survived contact with the code.

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

### 3. The security layer has no transport-neutral seam

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

What the libnyoci backend does instead:

- plain CoAP works (Requirement 5.3);
- `translate_legacy_fields()` is shared verbatim, so a `cert_file` still infers
  `dtls_pki` and the same `coap_security_config_error` is raised for the same
  malformed configs (Requirement 5.4, for configuration errors);
- **any mode that would have to encrypt bytes is refused at construction** with
  a `coap_security_error` naming the reason.

Refusing is the only safe answer. Silently downgrading a node that asked for
DTLS to plaintext Raft traffic is strictly worse than not starting.

Closing the gap means one of two things, and it is a decision for whoever needs
secured CoAP on a second backend:

1. **Lift a byte-level seam out of `coap_security_provider`** — a
   `protect(bytes) -> bytes` / `unprotect(bytes) -> bytes` pair that OSCORE can
   genuinely implement (it is a COSE transform over the message, so it can), and
   that both backends call. DTLS cannot go through such a seam; it would remain
   libcoap-only or move to libnyoci's own plugin.
2. **Enable libnyoci's OpenSSL DTLS plugin** (`--enable-tls` in the port) and
   accept that DTLS configuration forks per backend while OSCORE/EDHOC stay
   libcoap-only.

Neither is in scope for this spec; both are recorded in
`.kiro/specs/coap-transport-libnyoci/tasks.md` under Task 5.

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
  least adapter code, plain CoAP is acceptable, request payloads stay under one
  datagram, and the autotools port cost is acceptable. It is the closer analog
  to today's libcoap integration and it works today.
- Pick **cantcoap** if the goal is to *own* the CoAP wire behaviour end to end
  and reuse kythira's existing retransmission/block-wise/security machinery over
  a tiny, trivially-vendored codec — accepting that the adapter becomes the bulk
  of the work. Note that it would inherit fact 1 (its own header collision with
  libcoap) and would have to solve fact 3 itself, but it would *not* inherit
  fact 2: owning the block layer means Block1 is implementable.

[nyoci]: https://github.com/darconeous/libnyoci
[cant]: https://github.com/staropram/cantcoap
