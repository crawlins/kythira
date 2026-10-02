# CoAP Alternate-Backend Security Parity — Requirements Document

## Introduction

Kythira's CoAP transport has three backends: libcoap (`coap_transport_impl.hpp`),
libnyoci (`coap_transport_libnyoci_impl.hpp`) and cantcoap
(`coap_transport_cantcoap_impl.hpp` plus `coap_cantcoap_dtls.hpp`). All three
read the same `coap_security_config` (`include/raft/coap_security.hpp`), so a
node can switch backend without touching its configuration. That promise is
broken for three of the config's fields, and broken silently:

| Field | libcoap | libnyoci | cantcoap |
|---|---|---|---|
| `pki_credentials::revocation` | enforced (`coap_security_impl.hpp:270-282`, `coap_transport_impl.hpp:2223,3748`) | **ignored** — `apply_pki_credentials` (`coap_transport_libnyoci_impl.hpp:483-523`) never reads it | **ignored** — `dtls_layer::configure_pki` never reads it |
| `pki_credentials::cn_validator` | enforced (`coap_security_impl.hpp:284-305`) | **ignored** — `SSL_CTX_set_verify(ctx, mode, nullptr)` | enforced after the handshake (`coap_cantcoap_dtls.hpp:686-697`) |
| `coap_security_config::ace_bootstrap` | token exchange at construction (`coap_transport_impl.hpp:202,544`) | **ignored** by `plan_security` | **ignored** by `plan_security` |

Verified against `main` at `9c738d2` (2026-10-02), after PR #383 gave cantcoap
DTLS and EDHOC. The parity audit
(`parallel-implementation-parity-audit.md`, findings C2 and C3) predates #383;
#383 closed cantcoap's `cn_validator` gap but added a DTLS-PKI path with the
same revocation gap libnyoci has.

The consequences:

- A node configured with `revocation.enabled = true` on libnyoci or cantcoap
  accepts a peer whose certificate is on the CRL, with no warning. The
  revocation config is fail-closed on libcoap by design
  (`coap_revocation.hpp`: "a revocation check that silently passes when it
  cannot run is worse than none, because it is believed"); on the alternates
  it does not run at all.
- A node relying on `cn_validator` to pin peer identity (peers are addressed
  by IP, so there is no hostname check) accepts any CA-signed peer on
  libnyoci.
- A node configured for ACE-OAuth with no static credentials crashes
  construction on both alternates with `std::bad_variant_access`
  (`coap_transport_cantcoap_impl.hpp:532-533` and
  `coap_transport_libnyoci_impl.hpp:916` / server equivalents for OSCORE),
  or with a misleading "requires psk_credentials" / "credentials of the wrong
  kind" for `dtls_psk`. If static credentials *are* also present, the
  alternates silently use them and never contact the Authorization Server.

This spec brings both alternates to the libcoap behaviour for all three
fields, using the code libcoap already uses (`coap_revocation::check`,
`run_ace_token_exchange`) rather than a second implementation.

## Glossary

- **Alternate backends**: the libnyoci and cantcoap CoAP backends.
- **Revocation check**: `kythira::coap_revocation::check()` — CRL-based, fail-closed, OpenSSL `X509_V_FLAG_CRL_CHECK_ALL`.
- **Peer-certificate policy**: the triple (`ca_file`, `revocation`, `cn_validator`) applied to a DTLS-PKI peer's leaf certificate on top of OpenSSL's chain validation.
- **ACE bootstrap**: `run_ace_token_exchange()` (`coap_ace_oauth.hpp`), the RFC 9200 client-credentials token request that yields `psk_credentials` or `oscore_credentials`.
- **AS**: the ACE Authorization Server named by `ace_oauth_config::as_token_endpoint`.

## Requirements

### Requirement 1 — Revocation on libnyoci DTLS-PKI

**User Story:** As an operator, I want a libnyoci node with revocation enabled
to refuse a revoked peer, so switching to libnyoci does not quietly re-admit
peers I have revoked.

#### Acceptance Criteria

1. WHEN a libnyoci server is configured `dtls_pki` with `revocation.enabled = true` AND a client presents a certificate listed on the configured CRL THEN the server SHALL fail the handshake and SHALL NOT deliver any request from that client to a registered handler.
2. WHEN a libnyoci client is configured `dtls_pki` with `revocation.enabled = true` AND the server presents a revoked certificate THEN the client SHALL fail the handshake, the pending RPC SHALL fail, and no response from that server SHALL be delivered.
3. WHEN the CRL file cannot be read, `ca_file` is empty, or the issuer has no CRL and `allow_missing_crl` is false THEN the peer SHALL be refused (fail-closed, identical to `coap_revocation::check`).
4. WHEN `revocation.enabled` is false THEN libnyoci's handshake behaviour SHALL be unchanged from today.
5. WHEN revocation is configured through the legacy `cert_file`/`revocation` fields of `coap_client_config`/`coap_server_config` THEN `translate_legacy_fields()` SHALL carry it into `pki_credentials` and it SHALL be enforced exactly as in criteria 1-3.
6. The revocation decision SHALL be made by `coap_revocation::check()`, given the peer's untrusted intermediates; the libnyoci backend SHALL NOT parse CRLs itself.

### Requirement 2 — `cn_validator` on libnyoci DTLS-PKI

**User Story:** As an integrator, I want my peer-certificate validator to run
on libnyoci exactly as it runs on libcoap and cantcoap, so identity pinning is
backend-independent.

#### Acceptance Criteria

1. WHEN `pki_credentials::cn_validator` is set THEN libnyoci SHALL call it with the peer's leaf certificate in PEM form after OpenSSL's chain validation has succeeded, on both client and server.
2. WHEN the validator returns false or throws THEN the handshake SHALL fail and no request or response SHALL be delivered over that session.
3. WHEN the validator is null THEN OpenSSL's chain-validation verdict SHALL stand unchanged.
4. WHEN both revocation and `cn_validator` are configured THEN revocation SHALL be checked first and a revoked peer SHALL be refused without calling the validator, matching `dtls_pki_provider::validate_cn`.

### Requirement 3 — Revocation on cantcoap DTLS-PKI

**User Story:** As an operator, I want the same revocation guarantee on the
cantcoap backend that #383 gave it DTLS for.

#### Acceptance Criteria

1. WHEN a cantcoap client or server is configured `dtls_pki` with `revocation.enabled = true` AND the peer's certificate is revoked THEN the session SHALL be failed before it is marked established, and no queued or inbound CoAP message SHALL cross it.
2. Criteria 1.3, 1.4, 1.5 and 1.6 SHALL hold for cantcoap as written for libnyoci.
3. WHEN both revocation and `cn_validator` are configured THEN revocation SHALL be checked first (as Requirement 2.4).
4. A refused session SHALL be reported through the dtls_layer's existing failure path, with a reason that names revocation.

### Requirement 4 — Consistent handling of a policy that cannot run

**User Story:** As an operator, I want a revocation or validator setting that
cannot take effect to be refused, not believed.

#### Acceptance Criteria

1. WHEN `revocation.enabled` is true or `cn_validator` is set, AND `verify_peer_cert` is false, THEN all three backends SHALL behave identically.
2. The spec's chosen behaviour is to refuse at construction with `coap_security_config_error` naming the conflicting fields, because with `SSL_VERIFY_NONE` a server never requests a client certificate and a client ignores the verify callback's verdict, so the configured check could not reject anything. If Task 1 finds libcoap currently honours the check in this combination, the libcoap behaviour wins and this criterion is amended in place before implementation.

### Requirement 5 — ACE-OAuth on both alternate backends

**User Story:** As an operator, I want `ace_bootstrap` to provision
credentials on every backend, so an ACE-configured node does not crash or
silently fall back to stale static keys when the backend changes.

#### Acceptance Criteria

1. WHEN `coap_security_config::ace_bootstrap` is set THEN the libnyoci and cantcoap client and server constructors SHALL run the ACE bootstrap before choosing a channel, and SHALL replace `credentials` with its result, as the libcoap constructors do.
2. WHEN the AS is unreachable, returns a non-2xx status, or returns a malformed body THEN construction SHALL fail with `coap_credential_bootstrap_error`, and the backend SHALL NOT fall back to static or absent credentials.
3. WHEN the bootstrap returns `psk_credentials` and `mode == dtls_psk` THEN the alternate SHALL complete a DTLS-PSK exchange using the AS-issued identity and key.
4. WHEN the bootstrap returns `oscore_credentials` and `mode == oscore` THEN the alternate SHALL complete an OSCORE exchange using the AS-issued context.
5. The alternates SHALL call the same `run_ace_token_exchange()` as libcoap; there SHALL be one ACE client implementation in the tree.

### Requirement 6 — ACE profile/mode agreement, on all three backends

**User Story:** As an operator, I want a mismatched ACE profile refused with a
clear message, the same way on every backend.

#### Acceptance Criteria

1. WHEN `ace_bootstrap.target_profile == dtls_psk` AND `mode != dtls_psk`, OR `target_profile == oscore` AND `mode != oscore`, THEN all three backends SHALL raise `coap_security_config_error` naming the mode and profile, before contacting the AS.
2. WHEN `oscore` is selected with `bootstrap_method == edhoc` in the static credentials AND `ace_bootstrap` is also set THEN the configuration SHALL be refused as ambiguous (the AS result is always `static_provisioned`, so the EDHOC request would otherwise be dropped silently).
3. The check SHALL live in one shared helper used by all three backends, so the libcoap behaviour changes only by gaining the earlier, clearer error.

### Requirement 7 — No `std::bad_variant_access` from security config

**User Story:** As an operator, I want a malformed security config to fail
with a security-config error on every backend, not an STL exception.

#### Acceptance Criteria

1. WHEN `mode == oscore` AND `credentials` does not hold `oscore_credentials` after any ACE bootstrap THEN the libnyoci and cantcoap client and server constructors SHALL raise `coap_security_config_error` with the libcoap message ("security.mode == oscore requires oscore_credentials in security.credentials").
2. No `std::get<...>(security.credentials)` on either alternate's construction path SHALL be reachable without a preceding `holds_alternative` check or equivalent.

### Requirement 8 — Tests

**User Story:** As a maintainer, I want each new guarantee proven per backend
by real handshakes, so the parity cannot regress unnoticed.

#### Acceptance Criteria

1. For each alternate: a revoked client is refused by a revocation-enabled server; a revoked server is refused by a revocation-enabled client; a good certificate under the same CRL is accepted; a missing CRL is refused unless `allow_missing_crl`.
2. For libnyoci: a refusing `cn_validator` fails the handshake; an accepting one lets the RPC complete; a throwing one fails the handshake.
3. For each alternate: an ACE `dtls_psk` and an ACE `oscore` client/server pair, provisioned by the mock AS already used by `coap_ace_oauth_test.cpp`, complete an RPC; a denied scope fails construction with `coap_credential_bootstrap_error`.
4. For all three backends: the mismatch and ambiguity cases of Requirement 6 and the `verify_peer_cert = false` case of Requirement 4 raise `coap_security_config_error`.
5. Tests SHALL reuse the CRL and CA fixtures of `tests/coap_certificate_revocation_test.cpp` rather than adding a second certificate generator, and SHALL collapse to a single skipped case when the backend is not built, following the existing alternate-backend test pattern.

### Requirement 9 — Documentation

1. `doc/coap_library_alternatives.md` and `doc/coap_dtls_configuration.md` SHALL state that revocation, `cn_validator` and ACE-OAuth behave identically on all three backends.
2. The libnyoci and cantcoap spec task lists SHALL note the closed gap, and `CHANGELOG.md` SHALL carry an entry.

## Out of scope

- OCSP. `coap_revocation.hpp` deliberately does not implement it on libcoap either.
- ACE token refresh or expiry handling. libcoap exchanges once at construction; parity means the alternates do the same. A refresh mechanism is a separate change to all three.
- DTLS-RPK revocation (an RPK has no issuer or serial to revoke).
- Revocation for non-CoAP transports (audit finding T10).
