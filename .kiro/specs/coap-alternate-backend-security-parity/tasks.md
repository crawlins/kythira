# Implementation Plan — CoAP Alternate-Backend Security Parity

## Status: Complete (Tasks 1-7)

Implemented October 3, 2026. Closes parity-audit findings C2 (libnyoci ignores `revocation` and
`cn_validator`) and C3 (ACE-OAuth only on libcoap), plus the cantcoap
revocation gap PR #383 introduced alongside cantcoap DTLS-PKI.

**Last Updated**: October 3, 2026

Verification: the shared cases in
`tests/coap_alternate_security_parity_cases.hpp` pass on cantcoap and on
libnyoci, and fail on the previous `main` for every gap this spec names
(22 failures on cantcoap, 32 on libnyoci). CI still does not build either
alternate backend (see `ci-build-matrix-coverage`), so the libnyoci handshake
cases were run locally against a libnyoci built for IPv4
(`NYOCI_PLAT_NET_POSIX_FAMILY=AF_INET`), the sandbox having no IPv6.

## Detailed Task List

- [x] 1. Pin down libcoap's current behaviour before matching it
  - [x] 1.1 Determine whether libcoap invokes `validate_cn` once (leaf) or per
    chain depth on the pinned libcoap, and therefore what `cn_validator` and
    the revocation check see there. Record the answer in design.md decision 3.
  - [x] 1.2 Determine what libcoap does with `revocation.enabled` or
    `cn_validator` set and `verify_peer_cert = false`. If it enforces them,
    amend Requirement 4.2 and design decision 5 before Task 2.
  - _Requirements: 2.4, 4_

- [x] 2. Shared helpers
  - [x] 2.1 Add `resolve_ace_bootstrap(coap_security_config&)` to
    `coap_ace_oauth.hpp`: profile/mode agreement, ACE+EDHOC refusal, exchange,
    credential replacement
  - [x] 2.2 Add `validate_pki_peer_policy(const pki_credentials&)` to
    `coap_security.hpp`
  - [x] 2.3 Replace libcoap's two inline ACE blocks
    (`coap_transport_impl.hpp:202`, `:544`) with `resolve_ace_bootstrap()`;
    call `validate_pki_peer_policy()` from `dtls_pki_provider` and the legacy
    PKI paths
  - [x] 2.4 Extend `coap_ace_oauth_test.cpp` and
    `coap_certificate_revocation_test.cpp` with the mismatch, ACE+EDHOC and
    `verify_peer_cert = false` refusals on libcoap
  - _Requirements: 4, 5.5, 6_

- [x] 3. libnyoci DTLS-PKI peer policy
  - [x] 3.1 Move CA/CRL generation from `coap_certificate_revocation_test.cpp`
    into `tests/coap_revocation_fixtures.hpp` (pure move)
  - [x] 3.2 Write the first libnyoci test, revoked client refused by server,
    and confirm it fails on current code (proves the `SSL_CTX` verify callback
    is the seam — design Risks)
  - [x] 3.3 Add `pki_peer_policy` to `dtls_state`; pass `dtls_state&` into
    `apply_pki_credentials`; add `libnyoci_pki_verify_trampoline`; install it
    only when the policy is non-empty
  - [x] 3.4 Call `validate_pki_peer_policy()` from `apply_pki_credentials`
  - [x] 3.5 Remaining revocation cases (revoked server, good cert, missing CRL
    with and without `allow_missing_crl`, legacy fields) and `cn_validator`
    cases (accept, refuse, throw, revocation-before-validator)
  - _Requirements: 1, 2, 4, 8.1, 8.2_

- [x] 4. cantcoap DTLS-PKI revocation
  - [x] 4.1 Store `ca_file` and `revocation` in `dtls_layer::configure_pki`;
    call `validate_pki_peer_policy()` there
  - [x] 4.2 Run `coap_revocation::check()` first in `check_established_peer`,
    with the peer chain as untrusted, reason naming revocation
  - [x] 4.3 Revocation cases as 3.5, plus revocation-before-validator
  - _Requirements: 3, 4, 8.1_

- [x] 5. ACE-OAuth on both alternates
  - [x] 5.1 Move the mock AS from `coap_ace_oauth_test.cpp` into
    `tests/coap_ace_mock_as.hpp` (pure move)
  - [x] 5.2 Include `coap_ace_oauth.hpp` in both alternate impls; call
    `resolve_ace_bootstrap()` in both `plan_security` copies after
    `translate_legacy_fields()`
  - [x] 5.3 Add the `mode == oscore` requires `oscore_credentials` check to
    both `plan_security` copies, with libcoap's message
  - [x] 5.4 Per alternate: ACE `dtls_psk` RPC, ACE `oscore` RPC, denied scope
    fails construction, OSCORE with monostate credentials raises
    `coap_security_config_error` (the former `bad_variant_access`)
  - _Requirements: 5, 6, 7, 8.3, 8.4_

- [x] 6. Build wiring
  - [x] 6.1 Add `coap_libnyoci_security_parity_test` and
    `coap_cantcoap_security_parity_test` to `tests/CMakeLists.txt`, linked
    like the existing alternate DTLS suites (plus `httplib::httplib` when the
    target exists), `TIMEOUT 600`, labels `coap;<backend>;dtls;integration`
  - [x] 6.2 Each suite collapses to one skipped case when its backend is not
    built
  - _Requirements: 8.5_

- [x] 7. Documentation
  - [x] 7.1 `doc/coap_dtls_configuration.md`: revocation, `cn_validator` and
    ACE behave the same on all three backends; the `verify_peer_cert = false`
    refusal; any libcoap per-depth difference from Task 1
  - [x] 7.2 `doc/coap_library_alternatives.md`: update the security rows
  - [x] 7.3 Note the closed gap in the libnyoci and cantcoap tasks.md;
    `CHANGELOG.md` entry
  - _Requirements: 9_
