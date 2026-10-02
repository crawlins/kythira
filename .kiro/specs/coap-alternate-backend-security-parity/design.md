# CoAP Alternate-Backend Security Parity — Design Document

## Overview

Two pieces of shared code already do the work this spec needs:

- `coap_revocation::check(cert, ca_file, config, untrusted)` — the CRL
  decision, fail-closed, built on OpenSSL. Only libcoap calls it.
- `run_ace_token_exchange(config)` — the ACE client-credentials request.
  Only libcoap calls it.

The design is therefore wiring, not new security logic: give each alternate
backend a place to call these from, and move the one piece of ACE handling
that lives inline in libcoap's constructors into a shared helper so all three
backends call the same thing.

```
                       coap_security_config
                                │
                 resolve_ace_bootstrap()   ← new, coap_ace_oauth.hpp (all 3 backends)
                                │
            ┌───────────────────┼────────────────────┐
         libcoap             libnyoci             cantcoap
    dtls_pki_provider     SSL_CTX verify cb    dtls_layer::check_established_peer
      ::validate_cn       (new, PKI policy)       (+ revocation, new)
            └───────── coap_revocation::check() ──────┘
```

## Key design decisions

### 1. ACE resolves before `plan_security` chooses a channel

libcoap runs `run_ace_token_exchange` inside each constructor's security
block, after `translate_legacy_fields`. The alternates both funnel their
constructors through `plan_security(config, role)`, which already calls
`translate_legacy_fields` first. Adding the ACE step there gives both
alternates the libcoap ordering with one call site each.

The step itself becomes a shared function in `coap_ace_oauth.hpp`:

```cpp
// Validates profile/mode agreement (Requirement 6), then, if ace_bootstrap is
// set, runs the token exchange and replaces config.credentials with the
// result. Throws coap_security_config_error on a mismatch (before any network
// I/O) and lets coap_credential_bootstrap_error from the exchange propagate.
inline auto resolve_ace_bootstrap(coap_security_config& config) -> void;
```

libcoap's two inline copies (`coap_transport_impl.hpp:202-207`, `:544-549`)
are replaced by calls to it, which is how Requirement 6.3 keeps the backends
from drifting again. libcoap's only behavioural change is the earlier,
clearer error for a mismatched profile; today the same config fails later
with a provider-level message.

The bootstrap stays synchronous and at construction on every backend. It
blocks for up to the exchange's 10 s connect/read timeouts, exactly as it does
on libcoap; moving it off the constructor would be a change for all three and
is out of scope.

**Dependency.** `coap_ace_oauth.hpp` includes `<httplib.h>`. That is already
true of every libcoap build, because `coap_transport.hpp` includes it
unconditionally, and every alternate test target links `network_simulator`,
which carries `httplib::httplib` when found. The alternates gain no dependency
the CoAP transport does not already have. If a build ever lacks httplib, the
existing failure mode (the libcoap transport does not compile) is unchanged.

### 2. `plan_security` stops reaching `std::get` unguarded

Both alternates `std::get<oscore_credentials>` immediately after
`plan_security` returns `channel::oscore`. With ACE resolved first, the
remaining way to reach that line with the wrong alternative is a malformed
config. `plan_security` (both copies) gains the libcoap check — `mode ==
oscore` requires `oscore_credentials` — and throws the libcoap message, so the
constructors' `std::get` is guarded by construction (Requirement 7).

### 3. libnyoci: the verify callback is the only seam

libnyoci's OpenSSL plugin creates and drives each `SSL` itself; the adapter
only ever holds the `SSL_CTX`. There is no post-handshake hook to run a check
from. The adapter already solved this for DTLS-RPK:
`libnyoci_rpk_verify_trampoline` is installed with `SSL_CTX_set_verify` and
finds its state through `SSL_CTX_get_app_data` → `dtls_state`.

DTLS-PKI uses the same pattern:

- `dtls_state` gains a `pki_peer_policy` member: `ca_file`, `revocation`,
  `cn_validator`.
- `apply_pki_credentials` takes the `dtls_state&` (as `apply_rpk_credentials`
  does), fills the policy, sets the app data, and installs
  `libnyoci_pki_verify_trampoline` instead of `nullptr` — only when the
  policy is non-empty, so a config with neither revocation nor a validator
  keeps today's exact code path (Requirement 1.4, 2.3).
- The trampoline:
  1. returns `preverify_ok` unchanged for depth > 0 and whenever
     `preverify_ok == 0` (chain validation failed — nothing to add);
  2. at depth 0 runs `coap_revocation::check(leaf, ca_file, revocation,
     X509_STORE_CTX_get0_untrusted(store))`; a reason → set
     `X509_V_ERR_CERT_REVOKED` (or `X509_V_ERR_APPLICATION_VERIFICATION`
     for a policy failure such as an unreadable CRL) and return 0;
  3. then runs `cn_validator` on the PEM of the leaf inside `try`; false or
     a throw → `X509_V_ERR_APPLICATION_VERIFICATION`, return 0.

Rejecting inside the handshake means the peer receives a fatal alert and no
application record is ever exchanged, which is stronger than cantcoap's
post-handshake check and the same as libcoap's.

**Leaf only.** The validator and revocation run once, on the leaf. libcoap's
CN callback is invoked by libcoap per chain depth; `dtls_pki_provider::
validate_cn` ignores the depth argument, so on libcoap a `cn_validator` may
also see intermediates and the CRL check may run per depth. Task 1 confirms
which libcoap does on the pinned version. If it is per depth, the spec keeps
leaf-only on the alternates (the validator's documented purpose is peer
identity, and `coap_revocation::check` with `CRL_CHECK_ALL` already covers the
whole chain from the leaf) and records the libcoap difference in
`doc/coap_dtls_configuration.md` rather than changing libcoap's validator
semantics under existing callers.

### 4. cantcoap: extend the existing post-handshake check

cantcoap owns its `SSL` objects, and `dtls_layer::drive_handshake` already
calls `check_established_peer` before marking a session established and
before flushing anything queued on it. `cn_validator` runs there today.
Revocation is added at the top of the same function, ahead of the validator
(Requirement 3.3), using `SSL_get0_peer_certificate` for the leaf and
`SSL_get_peer_cert_chain` for the untrusted intermediates. `configure_pki`
stores `ca_file` and `revocation` alongside the existing `_cn_validator`.

Keeping the check post-handshake rather than moving it into a verify callback
is deliberate: it is where cantcoap's other peer checks live, it is already
proven not to leak data (no record crosses until `established`), and moving
`cn_validator` would change cantcoap behaviour this spec does not need to
touch. The cost is that the refused peer sees its session go silent rather
than receiving a fatal alert; `fail()` already records the reason locally.

### 5. Policy that cannot run is refused at construction

With `verify_peer_cert == false` the alternates set `SSL_VERIFY_NONE`: a
server never asks for a client certificate, and a client ignores the verify
callback's verdict. A revocation config or validator would be believed and
never enforced. The shared `pki_credentials` check (new
`validate_pki_peer_policy(const pki_credentials&)` in `coap_security.hpp`)
refuses that combination, and is called by all three backends where they
already validate PKI credentials (Requirement 4). Task 1 checks libcoap's
current behaviour first, because if libcoap genuinely enforces the policy
with `verify_peer_cert = false` (it sets `verify_peer_cert = 0` in
`coap_dtls_pki_t`, which may still invoke the CN callback), the parity
direction flips and the requirement is amended before code is written.

## Components and interfaces

| File | Change |
|---|---|
| `include/raft/coap_ace_oauth.hpp` | add `resolve_ace_bootstrap()` (profile/mode check, exchange, credential replacement) |
| `include/raft/coap_security.hpp` | add `validate_pki_peer_policy()` |
| `include/raft/coap_transport_impl.hpp` | replace both inline ACE blocks with `resolve_ace_bootstrap()`; call `validate_pki_peer_policy()` |
| `include/raft/coap_security_impl.hpp` | call `validate_pki_peer_policy()` in `dtls_pki_provider` |
| `include/raft/coap_transport_libnyoci_impl.hpp` | include `coap_ace_oauth.hpp`; ACE + oscore-credential check in `plan_security`; `pki_peer_policy` in `dtls_state`; `libnyoci_pki_verify_trampoline`; `apply_pki_credentials(ctx, state, creds, role)` |
| `include/raft/coap_transport_cantcoap_impl.hpp` | include `coap_ace_oauth.hpp`; ACE + oscore-credential check in `plan_security` |
| `include/raft/coap_cantcoap_dtls.hpp` | store `ca_file`/`revocation` in `configure_pki`; revocation first in `check_established_peer` |

No public type changes: every field this spec enforces already exists in
`coap_security_config`.

## Error handling

| Condition | Exception | When |
|---|---|---|
| ACE profile ≠ mode, or ACE + EDHOC | `coap_security_config_error` | construction, before network I/O |
| AS unreachable / non-2xx / malformed | `coap_credential_bootstrap_error` (from `run_ace_token_exchange`) | construction |
| `mode == oscore` without `oscore_credentials` | `coap_security_config_error` | construction |
| revocation or validator with `verify_peer_cert == false` | `coap_security_config_error` | construction |
| revoked peer, unreadable CRL, validator false/throws | handshake failure; RPC fails / request never delivered | handshake |

## Testing strategy

New files, one per backend, so each collapses to a single skipped case when
its backend is not built:

- `tests/coap_libnyoci_security_parity_test.cpp`
- `tests/coap_cantcoap_security_parity_test.cpp`

Both pull the CA/CRL generation out of `tests/coap_certificate_revocation_test.cpp`
into a shared `tests/coap_revocation_fixtures.hpp` (a pure move; the libcoap
test then includes it) and the mock AS out of `tests/coap_ace_oauth_test.cpp`
into `tests/coap_ace_mock_as.hpp`.

Cases per backend (Requirement 8): revoked client refused, revoked server
refused, good cert accepted, missing CRL refused / accepted with
`allow_missing_crl`; libnyoci validator accept / refuse / throw; ACE
`dtls_psk` RPC, ACE `oscore` RPC, denied scope; profile mismatch, ACE+EDHOC,
and `verify_peer_cert = false` refusals.

The construction-time refusals are also added to the libcoap side
(`coap_ace_oauth_test.cpp`, `coap_certificate_revocation_test.cpp`) so the
shared helpers are proven on all three backends.

Handshake tests run on loopback and follow the existing alternate DTLS suites'
`TIMEOUT 600` budget and `coap;<backend>;dtls;integration` labels. None needs
containers.

## Risks

- **libnyoci plugin verify-mode override.** If libnyoci's OpenSSL plugin calls
  `SSL_set_verify` on each `SSL` it creates, the `SSL_CTX` callback is
  replaced. The RPK path already relies on the `SSL_CTX` callback surviving,
  and its tests pass, so this is expected to hold; Task 3's first test proves
  it for PKI before the rest is built on it.
- **Shared CI coverage.** The audit notes cantcoap and libnyoci are not built
  in CI today. These tests only run where a developer or a future CI job
  selects the `coap-libnyoci` / `coap-cantcoap` features; this spec does not
  add that job.
