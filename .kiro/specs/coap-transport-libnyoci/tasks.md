# Implementation Plan — CoAP Transport (libnyoci backend)

## Status: Implemented (Tasks 1-4, 6, 7 complete; Task 5 partial by necessity)

The overlay port is pinned and builds; the adapter is written and tested
against real loopback sockets. `coap_libnyoci_client`/`coap_libnyoci_server`
satisfy `network_client`/`network_server` and speak the same wire protocol as
the libcoap backend. Two limitations are inherent to libnyoci and are handled
by refusing loudly rather than degrading silently — see Tasks 3.6 and 5.
DTLS-PSK and DTLS-PKI work over libnyoci's own OpenSSL plugin; OSCORE and
DTLS-RPK are refused with specific reasons.

**Last Updated**: August 8, 2026

## Major Tasks Overview

### Tasks 1-2: Overlay port and build gating
*The autotools overlay port and the `LIBNYOCI_AVAILABLE` gating, so later tasks
can assume libnyoci is linkable.*

### Tasks 3-5: Adapter (bridge libnyoci ↔ futures)
*Client transaction path, server handler path, and security — thin, because
libnyoci owns sockets/retransmit/dedup/Block2.*

### Tasks 6-7: Testing and documentation

## Detailed Task List

- [x] 1. Finalize the `vcpkg-overlays/libnyoci` port
  - [x] 1.1 Pin a real `REF` and regenerate `SHA512`
    - Pinned to `11a6e1b107cb1577cb0fffca7e3fd261f26da94a` (master, 2019-04-15),
      **not** the sole tag. `refs/tags/latest-release` → `0.07.00rc1` is the
      2017 *initial* release; the 35 commits since it carry the fuzzing fixes
      (null deref in `nyoci-list`, a missing-parens option-length bug), the URI
      corruption fix in `nyoci_outbound_set_uri()`, and the signature change
      that added the maximum-length argument to `nyoci_inbound_get_path()` —
      which the adapter calls, so it does not compile against the tag at all.
  - [x] 1.2 Verify `vcpkg_configure_make AUTOCONFIG` builds from the archive tarball
    - Verified on **x64 Linux** by extracting the pinned GitHub archive tarball
      (no `.git`, exactly what vcpkg fetches), applying the patch, and running
      `autoreconf -fi && ./configure && make && make install` with the port's
      own flags. Produces `libnyoci.a`, the headers under `include/libnyoci/`,
      and `lib/pkgconfig/libnyoci.pc`.
    - **arm64 Linux is not verified** — no cross toolchain on the development
      host. Nothing in the port or the adapter is architecture-specific
      (libnyoci is portable C; the adapter touches no intrinsics), but this is
      an untested claim, not a verified one. Verifying it needs an arm64 runner.
  - [x] 1.3 Add the patches surfaced by the first real build
    - `0001-drop-CODE_COVERAGE_RULES-substitution.patch`: libnyoci was written
      against a pre-2018 autoconf-archive whose `AX_CODE_COVERAGE` `AC_SUBST`'d
      a `CODE_COVERAGE_RULES` make fragment. Modern autoconf-archive (serial
      ≥ 25) emits those rules through `AX_ADD_AM_MACRO_STATIC` /
      `aminclude_static.am` and never substitutes the variable, so all eleven
      `@CODE_COVERAGE_RULES@` interpolations survive into the generated
      Makefiles and `make` dies with `*** missing separator`. `configure.ac`'s
      `m4_ifdef` fallback does not save it: that only fires when the macro is
      *absent*, and it cannot be absent on a host that has the archive
      installed — which the port requires anyway, for `AX_PTHREAD`.
    - Two further corrections to the skeleton's guesses, in the portfile rather
      than a patch: the configure flags are `--disable-examples
      --disable-nyocictl --disable-plugtest` (there is no `--disable-tests` and
      no `--without-examples`), and `--disable-dependency-tracking` is
      **mandatory** — automake's depfile bootstrap fails otherwise, which is
      why upstream's own CI passes it (commit `1330755`).
  - [x] 1.4 Confirm `vcpkg_fixup_pkgconfig` yields a discoverable `libnyoci.pc`
    - `pkg-config --cflags --libs libnyoci` resolves, and the root
      `CMakeLists.txt` probe finds it (`-- libnyoci found: 0.07.00rc1`).

- [x] 2. Build gating (opt-in, gracefully degrading)
  - [x] 2.1 Add the `coap-libnyoci` feature to the root `vcpkg.json`
  - [x] 2.2 Add the `pkg_check_modules(LIBNYOCI ...)` probe + `LIBNYOCI_AVAILABLE`
        definition to the root `CMakeLists.txt`, mirroring the `LIBCOAP_FOUND` block
    - Plus a `COAP_TRANSPORT_LIBNYOCI` Kconfig symbol (default `n`) so the probe
      participates in the same gate/require machinery every other optional
      dependency does. It deliberately does **not** `depend on COAP_TRANSPORT`:
      the backends are additive, and a build may want either, both or neither.
    - Requirement 2.4 (the two backends must not conflict at link time) was
      verified, not assumed: a program calling into both archives links and
      runs. Caveat recorded in the CMakeLists comment — they export exactly one
      name in common, `coap_insert_option`, survivable only because these are
      static archives (the linker pulls an object solely to satisfy an
      undefined symbol, and no translation unit can reference both libraries).
      A shared build, or a second overlapping name, would change that.
  - [x] 2.3 Verify a default build neither fetches nor links libnyoci and still succeeds
    - The feature is opt-in and the Kconfig default is `n`. Both new test
      targets are unconditional and compile without libnyoci present, because
      the adapter keeps its full concept surface either way; the integration
      test then collapses to a single "skipped" case.

- [x] 3. Client adapter (`coap_libnyoci_client<Types>`)
  - [x] 3.1 Construct one `nyoci_t` and drive its process loop on a `std::jthread`
    - libnyoci's API is not thread-safe, most of it is only legal from inside
      one of its own callbacks, and its "current instance" is a pthread-key
      thread-local. So *every* libnyoci call happens on the one event thread;
      `send_*` only serializes, queues and returns. The loop's 20 ms
      `nyoci_plat_wait()` budget is what bounds both send latency and shutdown
      latency — libnyoci offers no way to interrupt its `poll()` from another
      thread.
  - [x] 3.2 Implement `send_rpc<Request,Response>`
    - Including the same Content-Format/Accept negotiation the libcoap client
      does, through `serializer_registry` + `peer_capability_cache`, so the two
      backends are wire-interoperable rather than merely concept-compatible.
  - [x] 3.3 Implement the `on_response` C trampoline
    - Correlation is by context pointer: libnyoci hands back the transaction's
      own context, and only after matching message-id/token *and* remote
      address, so exactly one future is resolved per response and duplicates
      never reach the adapter (Requirements 4.2, 4.4).
    - The transaction carries `NYOCI_TRANSACTION_ALWAYS_INVALIDATE`. That is
      not decoration: libnyoci only continues a Block2 sequence when the flag is
      set, and it is also what guarantees a final callback to reject a
      transaction that ends without a response. The cost is that the callback
      can fire more than once, so every settle path is guarded by a `settled`
      flag.
  - [x] 3.4 Translate libnyoci status codes into `coap_exceptions.hpp` types
  - [x] 3.5 Wire the three `send_*` methods onto `send_rpc`
  - [x] 3.6 Reject over-large requests with a descriptive error
    - *Not in the original plan; forced by the library.* libnyoci implements
      **Block2 and no Block1** — `COAP_OPTION_BLOCK1` appears only in its
      option-name/type tables. A request that does not fit one datagram cannot
      be split, so the adapter checks
      `nyoci_outbound_get_space_remaining()` (the live packet, not a
      compile-time constant) and rejects the future with an error naming the
      missing Block1 support, rather than truncating the payload or hanging
      until the timeout. This bounds InstallSnapshot; large-snapshot
      deployments should use the libcoap backend.

- [x] 4. Server adapter (`coap_libnyoci_server<Types>`)
  - [x] 4.1 Register one inbound handler per RPC path
    - libnyoci routes through a single default request handler, so dispatch is
      on the URI path from `nyoci_inbound_get_path()`. Paths are identical to
      the libcoap server's (`/raft/request_vote` etc.).
  - [x] 4.2 Deserialize → invoke the stored `std::function` (guarded by `mutex_`) → respond
    - The handler is *copied* under the lock and invoked outside it, so a
      blocking handler cannot stall `register_*_handler`. Content negotiation
      matches the libcoap server: 4.15 for a Content-Format this registry
      cannot decode, 4.06 for an Accept list it cannot satisfy.
  - [x] 4.3 `start`/`stop`/`is_running` with clean teardown; reject in-flight futures
    - On shutdown the event thread ends every live transaction, which fires
      libnyoci's invalidate callback and rejects the corresponding future; the
      destructor then drains anything still queued that never reached libnyoci.
      No future is left unresolved (Requirement 6.4), covered by
      `test_destroying_the_client_rejects_in_flight_requests`.
  - [x] 4.4 Serve Block2 responses
    - *Not in the original plan.* libnyoci gives the **client** side of Block2
      for free but nothing on the server side, so the adapter slices responses
      itself — statelessly, serving each block straight out of the request's
      own Block2 option, so there is no per-peer transfer state to expire and a
      lost block is simply re-requested.

- [~] 5. Security integration — **DTLS via libnyoci's own plugin; OSCORE/RPK refused**
  - [x] 5.1 Route DTLS through libnyoci's OpenSSL plugin
    - **Not through `coap_security_provider`, and that is settled rather than
      deferred.** Every method of that interface is expressed in libcoap types
      (`coap_context_t*`, `coap_session_t*`, `coap_pdu_t*`,
      `coap_address_t*`), and its `protect`/`unprotect` hooks are identity
      passthroughs precisely *because* every mode is implemented below
      libcoap's PDU API rather than as a byte-level transform an adapter could
      call. There is no seam to sit above.
    - What works instead: libnyoci's OpenSSL DTLS plugin, enabled by
      `--enable-tls` in the port. It turned out to be richer than the spec
      assumed — `nyoci_plat_tls_set_context()` takes a raw `SSL_CTX*`
      (`typedef struct ssl_ctx_st* nyoci_plat_tls_context_t`), so PKI is
      reachable by configuring the context directly, and PSK has first-class
      hooks (`set_client_psk_callback` / `set_server_psk_callback` /
      `set_psk_hint`). The `coaps://` URI scheme selects the DTLS session type
      on its own, via `nyoci_session_type_from_uri_scheme()`.
    - **dtls_psk** and **dtls_pki** are implemented and covered end to end by
      `tests/coap_libnyoci_dtls_test.cpp`.
    - The adapter always builds and owns its own `SSL_CTX`: libnyoci never
      frees one (there is no `SSL_CTX_free` anywhere in the plugin, and
      `nyoci_plat_finalize()` only closes file descriptors), so passing
      `NYOCI_PLAT_TLS_DEFAULT_CONTEXT` would both leak and leave no handle to
      configure PKI through.
    - Three costs, recorded rather than hidden. DTLS *configuration* now forks
      per backend (the config surface does not — `coap_client_config` and
      `translate_legacy_fields()` are shared). Upstream still labels its TLS
      support experimental, defaults it off, and calls OpenSSL 1.x-era APIs
      that are deprecated-but-present in 3.x. And **DTLS-PKI needs small
      certificates**: libnyoci reads every inbound datagram into a fixed
      `char packet[NYOCI_MAX_PACKET_LENGTH+1]` (1033 bytes by default), which
      applies to handshake records too, so an RSA-2048 certificate flight is
      silently truncated and the handshake stalls with no diagnostic. Found by
      writing the test with RSA-2048 and watching it time out; ECDSA P-256 fits
      and passes, and the test fixture says so at its definition.
  - [ ] 5.2 OSCORE and EDHOC — **refused, and not closable here**
    - libnyoci ships neither, and kythira has no implementation of its own to
      fall back on: `oscore_provider` delegates entirely to libcoap
      (`coap_context_oscore_server`, `coap_new_client_session_oscore`). There
      is no AES-CCM, no COSE and no key derivation anywhere in the tree, so a
      byte-level seam would have nothing to lift — closing this is an
      "acquire an OSCORE implementation" project, not a refactor. Written up
      in `doc/TODO.md` under Known Follow-ups.
    - EDHOC itself is already transport-neutral (`edhoc_transport` is an
      abstract send/receive pair and lakers does the crypto); it is only the
      OSCORE context consuming its output that is not.
    - `plan_security()` refuses `oscore` at construction with a message naming
      the reason, rather than downgrading to plaintext.
  - [ ] 5.3 DTLS-RPK — **refused; not expressible through this surface**
    - Raw public keys (RFC 7250) need the peer to negotiate a non-X.509
      certificate type. libnyoci's plugin hands the adapter nothing but an
      `SSL_CTX`, and OpenSSL only grew the certificate-type extensions in 3.2
      (this toolchain has 3.0.13). Refused with that reason; `dtls_pki` is the
      supported alternative.
  - [x] 5.4 Plain CoAP still works, and invalid configuration surfaces the
        same errors as libcoap
    - `translate_legacy_fields()` is shared *verbatim* (it moved to
      `coap_transport_config.hpp` for exactly this reason), so a populated
      `cert_file` still infers `dtls_pki` and the same
      `coap_security_config_error` is raised for the same malformed configs —
      including the "both security.mode and legacy fields set" case. Bad
      certificate/key *material* now fails at `start()` with a
      `coap_security_error`, where before it could not fail at all.

- [x] 6. Tests (skipped when `LIBNYOCI_AVAILABLE` is undefined)
  - [x] 6.1 Concept-conformance test
    - `tests/coap_libnyoci_concept_conformance_test.cpp`, 5 cases. Compiles and
      passes *with or without* libnyoci, which is the point: conformance is a
      property of the adapter, not of whether the C library was found.
  - [x] 6.2 Loopback end-to-end test for all three RPCs
    - `tests/coap_libnyoci_integration_test.cpp`, 14 cases, all on ephemeral
      ports. Beyond the three round trips: concurrent-request correlation
      (16 in flight, each response identifiable), unreachable-peer timeout,
      unknown-target rejection, in-flight cancellation on destruction,
      server restart on the same port, and unregistered-handler rejection.
  - [x] 6.3 Block-wise transfer end-to-end test
    - `test_block_wise_response_reassembly` forces a 16-byte block size (the
      smallest RFC 7959 defines) so an ordinary Raft response spans a dozen
      blocks — a sharper test of reassembly than a single 1024-byte boundary.
      `test_oversized_request_is_rejected_with_a_descriptive_error` covers the
      Block1 gap from Task 3.6.
  - [x] 6.4 No container harness was added
    - Nothing here needs one, so the Docker/rootless-Podman rules in `CLAUDE.md`
      are not yet exercised by this spec. They *would* be needed for the one
      thing no in-process test can cover: a libcoap client against a libnyoci
      server. See "Follow-ups" below.

- [x] 7. Documentation
  - [x] 7.1 `DEPENDENCIES.md` — libnyoci (opt-in, autotools, MIT), the host
        `autoconf-archive` requirement, both limitations, and the
        one-backend-per-translation-unit rule
  - [x] 7.2 `doc/coap_library_alternatives.md` — rewritten from a paper
        comparison into a record of what building it actually took, including
        the three facts the skeleton got wrong

## Design change forced by the implementation

`include/raft/coap_transport_config.hpp` is new and was not in the design
document. It exists because **libcoap's and libnyoci's C headers cannot share a
translation unit**: libcoap spells the CoAP option numbers as object-like macros
(`#define COAP_OPTION_IF_MATCH 1`) and libnyoci as enumerators
(`COAP_OPTION_IF_MATCH = 1,`), so including libcoap first rewrites the middle of
libnyoci's enum into `1 = 1,`. No include order fixes it.

The design assumed the adapter would reuse `pending_message`,
`block_transfer_state` and the config structs straight out of
`coap_transport.hpp` — which includes `<coap3/coap.h>` whenever
`LIBCOAP_AVAILABLE` is defined, and therefore cannot be included here. The
library-neutral declarations (`coap_client_config`, `coap_server_config`,
`pending_message`, `received_message_info`, `translate_legacy_fields()`) moved
into the new header, which `coap_transport.hpp` now includes; every existing
call site is unchanged. What stayed behind is what is genuinely libcoap-shaped:
`block_transfer_state` (holds a `coap_session_t*`), `coap_error_info` (keyed by
`coap_pdu_code_t`), and the `*_transport_types` bundles.

Requirement 2.4 is still met — the two libraries do not conflict at *link* time,
and a translation unit selects a backend by which header it includes.

## Follow-ups

- **arm64 port verification** (Task 1.2). Needs an arm64 runner.
- **Cross-backend interop test**: a libcoap client against a libnyoci server
  cannot be built in one process. It needs two processes, i.e. the container
  harness Task 6.4 did not require — and that harness must follow the
  Docker/rootless-Podman rules in `CLAUDE.md`.
- **OSCORE without libcoap** (Task 5.2), if a second backend ever needs it.
  Written up in `doc/TODO.md` under Known Follow-ups; it is an "acquire an
  OSCORE implementation" project rather than a refactor.
- **DTLS-RPK** (Task 5.3) would need OpenSSL >= 3.2 and certificate-type
  negotiation the libnyoci plugin does not expose.
- **Block1**, if InstallSnapshot over libnyoci ever matters: it would have to be
  implemented in the adapter, since libnyoci has no support to build on.
