# CoAP Transport Security Requirements Document

## Introduction

The `coap-transport` spec (`.kiro/specs/coap-transport/`) already implements
two DTLS credential modes — pre-shared key and certificate-based mutual TLS —
selected implicitly by which of `cert_file`/`psk_identity` happen to be
populated on `coap_client_config`/`coap_server_config`
(`include/raft/coap_transport.hpp`, `include/raft/coap_transport_impl.hpp`).
This document specifies extending that transport to a total of five
mutually-exclusive **channel-security modes** — none, DTLS-PSK, DTLS-PKI,
DTLS-RPK (raw public key), and OSCORE — selected by an explicit mode field
rather than inferred from which fields happen to be set, plus two
**orthogonal, composable** mechanisms that provision credentials *into*
whichever mode is selected rather than standing as modes themselves: EDHOC
(a lightweight handshake that derives an OSCORE security context) and
ACE-OAuth / RFC 9200 (a token-exchange authorization flow that can hand back
either a PSK or an OSCORE context).

This document does not specify CoAP block-transfer or Q-Block performance
work (a separate, already-scoped concern — libcoap ≥4.3.2, which this
project already pins via `vcpkg.json` at ≥4.3.5, has Q-Block compiled in and
needs only a context flag, not new protocol work). It does not specify
building or operating an ACE Authorization Server — only the CoAP client/
server's consumption of a token the AS already issued. It does not modify
the Raft consensus protocol itself, only the transport layer beneath it.

Empirical groundwork already confirmed (by installing libcoap3-dev 4.3.4 and
inspecting exported symbols) before this document was written:
- `coap_new_oscore_conf`, `coap_new_client_session_oscore[_psk|_pki]`,
  `coap_context_oscore_server`, `coap_oscore_is_supported`, and recipient-
  management calls are all present in the OpenSSL-backed build variant this
  project links against. OSCORE requires no new external dependency.
- No EDHOC symbols exist in libcoap. EDHOC requires a new external
  dependency; `lakers` (Rust core, pre-built C bindings, BSD-3-Clause,
  actively maintained, documented `edhoc_exporter` → OSCORE-context flow) is
  the recommended library, though it is not present in the vcpkg registry
  and needs an overlay port.

## Glossary

- **Channel-security mode**: The mutually-exclusive choice of how a CoAP
  session is secured at the transport/session or message level: `none`,
  `dtls_psk`, `dtls_pki`, `dtls_rpk`, or `oscore`.
- **DTLS-PKI**: Certificate-based mutual TLS, already implemented
  (`coap_transport_impl.hpp:674` client, `:2236` server) via libcoap's
  `coap_dtls_pki_t`.
- **DTLS-PSK**: Pre-shared-key DTLS, already implemented
  (`coap_transport_impl.hpp:763` client, `:2295` server).
- **DTLS-RPK**: Raw Public Key DTLS — a `coap_dtls_pki_t` credential-type
  variant (peer identity pinned by public key rather than a CA-issued
  certificate chain), not yet implemented.
- **OSCORE**: Object Security for Constrained RESTful Environments
  (RFC 8613) — message-level (not session-level) encryption of the CoAP
  PDU itself, so it survives proxies and works with or without an
  underlying DTLS session. Compiled into the project's linked libcoap
  version but not yet wired into Kythira's transport.
- **EDHOC**: Ephemeral Diffie-Hellman Over COSE (RFC 9528) — a ~3-message
  handshake whose primary purpose here is deriving an OSCORE security
  context dynamically instead of provisioning one out-of-band.
- **ACE-OAuth**: RFC 9200, "Authentication and Authorization for
  Constrained Environments" — an OAuth2-style token exchange with an
  Authorization Server that, depending on profile, returns a PSK (DTLS
  profile) or an OSCORE security context (OSCORE profile) for the CoAP
  client to use.
- **Security provider**: The strategy-pattern abstraction (Requirement 1)
  that encapsulates one channel-security mode's session-level and/or
  message-level behavior behind a common interface.
- **Credential source**: Whether a mode's credentials come from static
  configuration, or are provisioned dynamically via EDHOC or ACE-OAuth.

## Requirements

### Requirement 1: Explicit, Mutually-Exclusive Channel-Security Mode Selection

**User Story:** As a developer configuring the CoAP transport, I want to
select a channel-security mode explicitly, so that misconfiguration (e.g.
accidentally populating both `cert_file` and `psk_identity`) cannot silently
produce an unintended or ambiguous security posture.

#### Acceptance Criteria

1. The system SHALL define a `coap_auth_mode` enumeration with exactly five
   values: `none`, `dtls_psk`, `dtls_pki`, `dtls_rpk`, `oscore`.
2. WHEN a `coap_client_config` or `coap_server_config` is constructed THEN
   the system SHALL determine its channel-security mode from an explicit
   `security.mode` field, never by inferring it from which legacy fields
   (`cert_file`, `psk_identity`, etc.) are populated.
3. WHEN `security.mode` requires credentials of a type inconsistent with
   what is present in `security.credentials` (e.g. `dtls_pki` selected but
   no PKI credentials supplied) THEN the system SHALL fail construction
   with a descriptive exception rather than silently falling back to
   `none`.
4. The system SHALL provide a `coap_security_provider` interface with a
   session-level hook (`configure_session`) and a message-level hook
   (`protect`/`unprotect`), so that DTLS-based modes (which act at session
   setup) and OSCORE (which acts per-message) can each implement only the
   hook relevant to them.
5. WHEN a mode has no work to do at a given hook (e.g. `none` at both hooks,
   or `dtls_pki` at the message-level hook) THEN that hook SHALL be a no-op
   for that mode, never an error.

### Requirement 2: Preserve Existing DTLS-PSK and DTLS-PKI Behavior

**User Story:** As an operator with an existing DTLS-PSK or DTLS-PKI
deployment, I want the new explicit-mode configuration to produce identical
behavior to today's field-inference configuration, so that adopting the new
config model is not a breaking change.

#### Acceptance Criteria

1. WHEN `security.mode == dtls_psk` THEN the system SHALL perform PSK setup
   identical to the existing behavior at `coap_transport_impl.hpp:763`
   (client) and `:2295` (server), including PSK length validation (4-64
   bytes) and identity length validation (≤128 characters).
2. WHEN `security.mode == dtls_pki` THEN the system SHALL perform PKI setup
   identical to the existing behavior at `coap_transport_impl.hpp:674`
   (client) and `:2236` (server), including mutual authentication
   (`require_peer_cert == verify_peer_cert`), chain validation to depth 10,
   and the CN-validation callback.
2. A test SHALL confirm that a legacy configuration (populating
   `cert_file`/`key_file` or `psk_identity`/`psk_key` directly, with
   `security.mode` left unset) continues to behave as it does today via an
   internal translation shim, so existing deployments are not forced to
   migrate immediately.
3. WHEN both the legacy fields and an explicit `security.mode` are set
   inconsistently (e.g. `psk_identity` populated but `security.mode ==
   dtls_pki`) THEN the system SHALL fail construction with a descriptive
   exception rather than guessing which the caller intended.

### Requirement 3: DTLS-RPK (Raw Public Key) Mode

**User Story:** As an operator who wants per-node key-based identity without
operating a CA, I want a raw-public-key DTLS mode, so that I get mutual
authentication cheaper than a full certificate chain but stronger than a
shared PSK.

#### Acceptance Criteria

1. WHEN `security.mode == dtls_rpk` THEN the system SHALL configure
   libcoap's DTLS-PKI context using a raw-public-key credential type rather
   than a PEM certificate chain.
2. WHEN a peer's raw public key does not match any key in the configured
   trust set THEN the connection SHALL be rejected, mirroring the existing
   PKI mode's rejection behavior on certificate validation failure.
3. RPK mode SHALL reuse the existing `coap_security_provider` session-level
   hook and MAY share implementation with the PKI provider, differing only
   in credential type, since both operate through libcoap's same
   `coap_dtls_pki_t` structure.

### Requirement 4: OSCORE Channel Security

**User Story:** As an operator whose CoAP traffic crosses proxies, or who
wants end-to-end message protection independent of the DTLS session, I want
an OSCORE mode, so that individual CoAP messages are protected regardless of
how many hops or proxies they pass through.

#### Acceptance Criteria

1. WHEN `security.mode == oscore` THEN the system SHALL build an OSCORE
   security context via libcoap's `coap_new_oscore_conf()`, using configured
   sender ID, recipient ID, master secret, master salt, and AEAD algorithm.
2. WHEN a client session is created in OSCORE mode THEN the system SHALL use
   `coap_new_client_session_oscore()`, or the combined
   `coap_new_client_session_oscore_psk()` / `_pki()` variant when OSCORE is
   layered with an underlying DTLS session for defense in depth.
3. WHEN a server is configured in OSCORE mode THEN the system SHALL register
   the context's default OSCORE configuration via
   `coap_context_oscore_server()` and add per-peer recipients via
   `coap_new_oscore_recipient()`.
4. Before establishing any OSCORE session THEN the system SHALL call
   `coap_oscore_is_supported()` and fail fast with a descriptive exception
   if it returns false, rather than failing deep inside a handshake or
   silently falling back to an unprotected session.
5. OSCORE protection SHALL be applied at the message level (the `protect`/
   `unprotect` hooks of `coap_security_provider`), independent of whichever
   block-transfer mechanism (classic block-wise or Q-Block) is in use for a
   given message, since OSCORE protects each block-wise PDU individually.

### Requirement 5: EDHOC-Based OSCORE Bootstrap

**User Story:** As an operator deploying to constrained devices where
out-of-band OSCORE key provisioning is impractical, I want to derive an
OSCORE security context dynamically via EDHOC, so that devices can be
provisioned with an identity credential and establish OSCORE contexts
on demand rather than requiring a pre-shared OSCORE master secret per peer.

#### Acceptance Criteria

1. The system SHALL support an `oscore_credentials.bootstrap_method` value
   of `edhoc`, distinct from `static_provisioned`.
2. WHEN `bootstrap_method == edhoc` THEN the system SHALL run an EDHOC
   handshake (via the `lakers` library) before OSCORE session establishment,
   and SHALL derive the OSCORE sender ID, recipient ID, master secret, and
   master salt from the handshake's `edhoc_exporter` output rather than from
   static configuration.
3. WHEN `bootstrap_method == static_provisioned` (the default) THEN no EDHOC
   handshake SHALL be performed, and OSCORE credentials SHALL come directly
   from configuration, preserving Requirement 4's behavior unchanged.
4. WHEN the EDHOC handshake fails (e.g. peer authentication failure,
   malformed message) THEN the system SHALL fail OSCORE session
   establishment with a descriptive exception rather than falling back to
   an unprotected or statically-keyed session.
5. The system SHALL vendor `lakers`'s pre-built C static library and headers
   via a vcpkg overlay port, since `lakers` is not present in the vcpkg
   registry, rather than requiring a Rust toolchain in the consuming
   project's build or CI.

### Requirement 6: ACE-OAuth (RFC 9200) Credential Provisioning

**User Story:** As an operator running a fleet with centralized access
control, I want CoAP nodes to obtain their channel-security credentials from
an Authorization Server via a standard token exchange, so that credential
issuance and revocation are managed centrally rather than baked into static
per-node configuration.

#### Acceptance Criteria

1. The system SHALL treat ACE-OAuth as a credential-provisioning step that
   runs before channel-security establishment, never as a sixth
   `coap_auth_mode` value — its output SHALL populate either
   `psk_credentials` (DTLS profile) or `oscore_credentials`
   (OSCORE profile), after which the ordinary `dtls_psk` or `oscore`
   provider takes over unaware of how its credentials were obtained.
2. WHEN `security.ace_bootstrap` is configured THEN the system SHALL perform
   the token request to the configured Authorization Server endpoint before
   constructing the channel-security provider, and SHALL populate the
   target profile's credentials from the token response.
3. WHEN the AS token exchange fails (network error, invalid client
   credentials, denied scope) THEN the system SHALL fail transport
   initialization with a descriptive exception rather than falling back to
   an unauthenticated session.
4. WHEN no `security.ace_bootstrap` is configured (the default) THEN no
   token exchange SHALL occur, and credentials SHALL come directly from
   static configuration or EDHOC (Requirement 5), preserving all other
   requirements' behavior unchanged.
5. The AS token endpoint interaction SHALL use the project's existing HTTP
   client capability rather than introducing a second HTTP stack, since
   ACE-OAuth's token endpoint is a standard HTTPS call, not a CoAP
   exchange.

### Requirement 7: Runtime Capability Verification

**User Story:** As an operator, I want the transport to detect at startup
whether the linked libcoap build actually supports the configured
channel-security mode, so that a build/deployment mismatch fails immediately
and clearly rather than manifesting as a mysterious handshake failure in
production.

#### Acceptance Criteria

1. WHEN OSCORE mode is configured THEN the system SHALL verify
   `coap_oscore_is_supported()` returns true before attempting session
   establishment.
2. WHEN any DTLS-based mode is configured THEN the system SHALL verify
   DTLS support is compiled into the linked libcoap before attempting
   session establishment, consistent with existing behavior.
3. WHEN a capability check fails THEN the system SHALL raise a descriptive
   exception identifying which mode was requested and which capability was
   missing, distinct from the exception types raised for peer-side
   authentication failures.

### Requirement 8: Backward-Compatible Migration Path

**User Story:** As an operator with an existing deployment using the current
flat DTLS configuration fields, I want a documented, tested migration path
to the explicit mode-based configuration, so that I can adopt new modes
(RPK, OSCORE, EDHOC, ACE-OAuth) without a disruptive rewrite of existing
configuration.

#### Acceptance Criteria

1. The system SHALL continue to accept the existing flat fields
   (`cert_file`, `key_file`, `ca_file`, `psk_identity`, `psk_key`,
   `verify_peer_cert`) with `security.mode` left at its default, translating
   them internally to the equivalent explicit `coap_security_config`.
2. The system SHALL document the equivalent explicit configuration for every
   legacy field combination in current use.
3. A test SHALL verify that every existing DTLS-related test in
   `.kiro/specs/coap-transport/` continues to pass unmodified after this
   feature is implemented.

### Requirement 9: Tests

**User Story:** As a developer, I need automated tests for all five
channel-security modes and both orthogonal provisioning mechanisms, so that
each mode's behavior, composability, and failure handling are verified
independently and in combination.

#### Acceptance Criteria

1. A unit test SHALL verify that each of the five `coap_auth_mode` values
   selects the correct `coap_security_provider` implementation.
2. A unit test SHALL verify that inconsistent mode/credential combinations
   (Requirement 1.3) fail construction with a descriptive exception.
3. A unit test SHALL verify RPK peer-key mismatch rejection (Requirement
   3.2).
4. An integration test SHALL verify OSCORE message protection round-trips
   correctly between a client and server pair, including the case where
   OSCORE is layered on top of an underlying DTLS-PSK session.
5. An integration test SHALL verify the EDHOC-to-OSCORE bootstrap flow
   derives a working OSCORE context and that a handshake failure prevents
   session establishment (Requirement 5.4).
6. An integration test (using a mock Authorization Server) SHALL verify the
   ACE-OAuth token exchange populates PSK credentials correctly for the
   DTLS profile and OSCORE credentials correctly for the OSCORE profile,
   and that a failed exchange prevents transport initialization
   (Requirement 6.3).
7. A test SHALL verify the runtime capability check (Requirement 7) fails
   descriptively when `coap_oscore_is_supported()` is stubbed to return
   false.
8. A regression test SHALL run the full existing `coap-transport` test
   suite unmodified to confirm Requirement 8.3.
