# Implementation Plan — CoAP Transport Security

## Status: Not Started

## Overview

Extend the existing `coap-transport` DTLS-only, field-inference security
configuration into five explicit channel-security modes (`none`, `dtls_psk`,
`dtls_pki`, `dtls_rpk`, `oscore`) behind a common `coap_security_provider`
abstraction, plus two orthogonal credential-provisioning mechanisms (EDHOC,
ACE-OAuth) that populate credentials for the `oscore`/`dtls_psk` modes
without becoming modes themselves. Existing PSK/PKI logic is wrapped, not
rewritten; RPK and OSCORE are new providers built against libcoap APIs
already confirmed present in the linked library (≥4.3.2; project pins
≥4.3.5); EDHOC is the one genuinely new external dependency (`lakers`).

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 2],
      "description": "Config model (coap_auth_mode, coap_security_config, credential structs) and the coap_security_provider interface + factory — no libcoap behavior changes yet"
    },
    {
      "wave": 2,
      "tasks": [3, 4],
      "description": "Wrap existing PSK/PKI logic as providers (task 3) and add the legacy-field translation shim (task 4, depends on task 3) — depends on wave 1"
    },
    {
      "wave": 3,
      "tasks": [5],
      "description": "DTLS-RPK provider, sharing PKI's coap_dtls_pki_t plumbing — depends on wave 2"
    },
    {
      "wave": 4,
      "tasks": [6, 7],
      "description": "OSCORE provider: static-provisioned context (task 6) and the runtime capability check (task 7, depends on task 6) — depends on wave 1, independent of waves 2-3"
    },
    {
      "wave": 5,
      "tasks": [8, 9],
      "description": "EDHOC: vcpkg overlay port for lakers (task 8) and the EDHOC-to-OSCORE bootstrap flow (task 9, depends on task 8 and task 6)"
    },
    {
      "wave": 6,
      "tasks": [10],
      "description": "ACE-OAuth token exchange targeting either the dtls_psk or oscore profile — depends on tasks 3 and 6, independent of wave 5"
    },
    {
      "wave": 7,
      "tasks": [11, 12, 13, 14, 15, 16, 17],
      "description": "Tests for each mode/mechanism and the full regression pass"
    }
  ]
}
```

## Tasks

---

## Phase 1: Config Model and Provider Abstraction (Tasks 1-2)

- [ ] 1. Add `coap_security.hpp` with `coap_auth_mode`, credential structs,
      and `coap_security_config`
  - New header `include/raft/coap_security.hpp`: `coap_auth_mode` enum
    (`none`, `dtls_psk`, `dtls_pki`, `dtls_rpk`, `oscore`);
    `psk_credentials`, `pki_credentials`, `rpk_credentials`,
    `oscore_credentials` (with nested `oscore_bootstrap` enum and
    `edhoc_params`), `ace_oauth_config` (with `ace_target_profile`); and
    `coap_security_config` holding `mode`, a `std::variant` over the four
    credential structs, and an optional `ace_bootstrap`.
  - Add `coap_security_config_error`, `coap_unsupported_security_mode_error`,
    and `coap_credential_bootstrap_error`, deriving from the existing
    `coap_security_error` (`.kiro/specs/coap-transport/design.md`).
  - Verify: unit test constructing each credential struct and the config
    variant; assert default-constructed `coap_security_config` has
    `mode == none` and `credentials` holding `std::monostate`.
  - _Requirements: 1.1_

- [ ] 2. Add `coap_security_provider` interface and `make_security_provider()`
      factory
  - Interface with `configure_session(coap_context_t*)`,
    `protect(coap_pdu_t*) -> coap_pdu_t*`, `unprotect(coap_pdu_t*) ->
    coap_pdu_t*`; a `no_auth_provider` implementing all three as
    identity/no-ops.
  - `make_security_provider(const coap_security_config&)` switches on
    `mode`; for now (before Tasks 3-6 exist) only `none` returns a real
    provider, other modes return a placeholder that throws
    "not yet implemented" — this task establishes the switch structure the
    later tasks fill in, not a stub that ships.
  - Verify: unit test — `make_security_provider({mode: none})` returns a
    `no_auth_provider`; `protect()`/`unprotect()` return their input
    unchanged (pointer or value equality, per whichever ownership model the
    implementation uses).
  - _Requirements: 1.4, 1.5_

---

## Phase 2: Wrap Existing PSK/PKI, Legacy Migration (Tasks 3-4)

- [ ] 3. Add `dtls_psk_provider` and `dtls_pki_provider`, wrapping existing
      logic
  - `dtls_psk_provider::configure_session()` calls the exact existing PSK
    setup code currently inline in `setup_dtls_context()`
    (`coap_transport_impl.hpp:763-787` client, `:2295-2422` server) —
    move-not-rewrite: extract the existing branch bodies into the provider,
    leaving behavior (length validation, identity matching) unchanged.
  - `dtls_pki_provider::configure_session()` does the same for the existing
    PKI branch (`:674-757` client, `:2236-`ish server), including the
    CN-validation callback and mutual-auth (`require_peer_cert ==
    verify_peer_cert`) behavior.
  - Both providers' `protect()`/`unprotect()` are no-ops (DTLS protects at
    the session level).
  - Update `make_security_provider()` to return these for `dtls_psk`/
    `dtls_pki`.
  - Verify: re-run every existing PSK/PKI test in `.kiro/specs/coap-transport/`
    unmodified against the new provider path; assert identical pass/fail
    behavior to the pre-refactor code for valid config, invalid PSK length,
    invalid identity length, and certificate rejection cases.
  - _Requirements: 2.1, 9.1_

- [ ] 4. Add legacy-field translation shim
  - `translate_legacy_fields(coap_client_config)` /
    `(coap_server_config)`: if `security.mode != none`, return
    `security` unchanged (and throw `coap_security_config_error` if legacy
    fields are also populated — Requirement 2.3); else infer from
    `cert_file`/`psk_identity` exactly as today's `setup_dtls_context()`
    does, returning the equivalent `coap_security_config`.
  - Call this once at `coap_client`/`coap_server` construction, before
    `make_security_provider()`.
  - Verify: unit test — legacy-only config (no `security.mode` set)
    produces the same provider type and behavior as an equivalent explicit
    `security.mode` config; a config setting both legacy fields and an
    inconsistent explicit mode throws `coap_security_config_error`.
  - _Requirements: 2.2, 2.3, 8.1, 8.3_

---

## Phase 3: DTLS-RPK Provider (Task 5)

- [ ] 5. Add `dtls_rpk_provider`
  - Shares `coap_dtls_pki_t` setup structure with `dtls_pki_provider`
    (Task 3), differing in `pki_key.key_type` (raw-public-key type instead
    of `COAP_PKI_KEY_PEM`) and in the validation callback: compare the
    peer's raw public key against `rpk_credentials::trusted_peer_keys`
    instead of running chain validation.
  - Update `make_security_provider()` to return this for `dtls_rpk`.
  - Verify: unit test — session establishes when peer's raw public key is
    in `trusted_peer_keys`; rejected when it is not.
  - _Requirements: 3.1, 3.2, 3.3_

---

## Phase 4: OSCORE Provider and Capability Check (Tasks 6-7)

- [ ] 6. Add `oscore_provider` (static-provisioned credentials)
  - `configure_session()`: build context via `coap_new_oscore_conf()` from
    `oscore_credentials` fields; client side calls
    `coap_new_client_session_oscore()`, or `_psk`/`_pki` combined variant
    when an underlying DTLS mode is also configured for the same session;
    server side calls `coap_context_oscore_server()` and
    `coap_new_oscore_recipient()` per known peer.
  - `protect()`/`unprotect()`: real implementations wrapping/unwrapping
    each outgoing/incoming PDU through libcoap's OSCORE message processing.
  - Update `make_security_provider()` to return this for `oscore` when
    `oscore_credentials.bootstrap_method == static_provisioned`.
  - Verify: integration test — client/server pair exchanges an
    AppendEntries-shaped request/response under plain OSCORE, and
    separately under OSCORE-over-DTLS-PSK (combined constructor); assert
    the payload round-trips and that a tampered ciphertext is rejected.
  - _Requirements: 4.1, 4.2, 4.3, 4.5_

- [ ] 7. Add OSCORE runtime capability check
  - At the start of `oscore_provider::configure_session()`, call
    `coap_oscore_is_supported()`; if false, throw
    `coap_unsupported_security_mode_error(coap_auth_mode::oscore, "OSCORE
    not compiled into linked libcoap")` before any context/session
    mutation.
  - Verify: unit test stubbing `coap_oscore_is_supported()` to return false
    (via a build flag or test seam) asserts the exception is thrown before
    `coap_new_oscore_conf()` is ever called.
  - _Requirements: 4.4, 7.1, 7.3_

---

## Phase 5: EDHOC Bootstrap (Tasks 8-9)

- [ ] 8. Add vcpkg overlay port for `lakers`
  - Create `vcpkg-overlays/lakers/` with a `portfile.cmake` that downloads
    `lakers`'s published pre-built C static library and headers for the
    project's target triplet(s), and a `vcpkg.json` exposing a
    `lakers::lakers` CMake target. Wire `find_package(lakers CONFIG QUIET)`
    into the top-level `CMakeLists.txt`, gating `LAKERS_AVAILABLE`.
  - Verify: a clean vcpkg bootstrap using this overlay resolves and links
    `lakers::lakers` successfully; a minimal smoke program calls one
    `lakers` C API function and links without a Rust toolchain present in
    the build environment.
  - _Requirements: 5.5_

- [ ] 9. Add `run_edhoc_handshake()` and wire `oscore_bootstrap::edhoc`
  - `run_edhoc_handshake(edhoc_params, session_handle) -> oscore_credentials`:
    drives the EDHOC exchange via `lakers`, then calls its
    `edhoc_exporter()` to derive `sender_id`/`recipient_id`/
    `master_secret`/`master_salt`, returning a populated
    `oscore_credentials` with `bootstrap_method` left as whatever the
    caller already had (informational only at this point).
  - In `coap_client`/`coap_server` construction, when `security.mode ==
    oscore` and `oscore_credentials.bootstrap_method == edhoc`, call this
    before `make_security_provider()` and use its result in place of the
    statically-configured credentials.
  - Behind `#ifdef LAKERS_AVAILABLE`: when absent, `bootstrap_method ==
    edhoc` throws `coap_credential_bootstrap_error("edhoc", "lakers not
    compiled in")` rather than silently behaving as
    `static_provisioned`.
  - Verify: integration test — two peers configured with
    `bootstrap_method == edhoc` and matching identity/peer credentials
    complete a handshake and can then exchange an OSCORE-protected
    message; a peer-credential mismatch fails the handshake and prevents
    session establishment.
  - _Requirements: 5.1, 5.2, 5.3, 5.4_

---

## Phase 6: ACE-OAuth Credential Provisioning (Task 10)

- [ ] 10. Add `run_ace_token_exchange()`
  - `run_ace_token_exchange(ace_oauth_config) ->
    std::variant<psk_credentials, oscore_credentials>`: performs the AS
    token request over HTTPS using the project's existing HTTP client
    (cpp-httplib), returning credentials shaped by `target_profile`.
  - In `coap_client`/`coap_server` construction, when `security.ace_bootstrap`
    is set, call this before any EDHOC step (Task 9) or provider
    construction, and populate `security.credentials` from the result.
  - Verify: integration test against a mock AS — DTLS profile response
    populates working `psk_credentials` (session establishes and PSK
    identity matches the token response); OSCORE profile response
    populates working `oscore_credentials` (session establishes and
    messages round-trip); a mock AS error response (4xx, malformed token)
    raises `coap_credential_bootstrap_error` and prevents transport
    initialization.
  - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5_

---

## Phase 7: Tests (Tasks 11-17)

- [ ] 11. `tests/coap_security_mode_selection_test.cpp` (new file)
  - Each of the five `coap_auth_mode` values selects its corresponding
    provider type; inconsistent mode/credential combinations throw
    `coap_security_config_error`.
  - _Requirements: 9.1, 9.2_

- [ ] 12. `tests/coap_legacy_config_migration_test.cpp` (new file)
  - Every pre-existing DTLS-related test from `.kiro/specs/coap-transport/`
    re-run against the new `translate_legacy_fields()` path with identical
    results.
  - _Requirements: 8.3, 9.8_

- [ ] 13. `tests/coap_dtls_rpk_test.cpp` (new file)
  - RPK peer-key match succeeds; mismatch is rejected.
  - _Requirements: 9.3_

- [ ] 14. `tests/coap_oscore_integration_test.cpp` (new file)
  - Client/server round-trip under plain OSCORE and OSCORE-over-DTLS-PSK;
    tampered-ciphertext rejection.
  - _Requirements: 9.4_

- [ ] 15. `tests/coap_edhoc_oscore_bootstrap_test.cpp` (new file)
  - EDHOC handshake derives a working OSCORE context; handshake failure
    (credential mismatch) prevents session establishment.
  - _Requirements: 9.5_

- [ ] 16. `tests/coap_ace_oauth_test.cpp` (new file)
  - Mock-AS-driven population of PSK and OSCORE credentials; failed
    exchange prevents transport initialization.
  - _Requirements: 9.6_

- [ ] 17. `tests/coap_security_capability_check_test.cpp` (new file)
  - `coap_oscore_is_supported()` stubbed false produces
    `coap_unsupported_security_mode_error` before any session mutation.
  - _Requirements: 9.7_

---

## Notes

- Task 1 and Task 2 have no dependency on each other's internals beyond
  shared types and are a reasonable pair to implement together, but Task 2's
  factory switch statement needs Task 1's `coap_auth_mode` enum to exist
  first.
- Tasks 3 and 4 are sequenced (not parallel) because the translation shim
  (Task 4) needs the providers it translates *to* (Task 3) to exist so its
  test can assert equivalent behavior, not just equivalent configuration
  values.
- Wave 4 (OSCORE) has no dependency on waves 2-3 (RPK, PSK/PKI wrapping) —
  it only needs the Wave 1 config model — so it can proceed in parallel
  with Phase 2/3 if capacity allows.
- Wave 5 (EDHOC) and Wave 6 (ACE-OAuth) are independent of each other; both
  depend on Wave 4 (OSCORE provider) existing as a credential consumer, and
  ACE-OAuth additionally depends on Task 3 (`dtls_psk_provider`) since its
  DTLS profile targets PSK credentials.
- Per the design's Phase ordering, RPK and OSCORE (Tasks 5-7) require no
  new external dependencies — confirmed via symbol inspection before this
  plan was written. Only Task 8 (EDHOC) introduces one.
