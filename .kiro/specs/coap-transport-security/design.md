# CoAP Transport Security Design Document

## Overview

This design extends the `coap-transport` spec's DTLS-only, field-inference
security configuration into five explicit, mutually-exclusive
**channel-security modes** (`none`, `dtls_psk`, `dtls_pki`, `dtls_rpk`,
`oscore`), plus two mechanisms that provision credentials *into* whichever
mode is selected rather than standing as modes of their own: **EDHOC**
(derives an OSCORE context dynamically) and **ACE-OAuth**/RFC 9200 (obtains
a PSK or OSCORE context from an Authorization Server). It does not redesign
the existing PSK/PKI DTLS setup logic (`coap_transport_impl.hpp:671-899`,
`:2224-2430`) — that logic is preserved behind a `coap_security_provider`
abstraction, with RPK and OSCORE added alongside it as new providers built
against libcoap APIs already confirmed present in the linked library
(≥4.3.2; this project pins ≥4.3.5).

Two axes, not one flat list:

```
Channel-security mode (mutually exclusive, selects a provider):
  none | dtls_psk | dtls_pki | dtls_rpk | oscore

Credential source (orthogonal, populates a mode's credentials before
the provider is constructed):
  static configuration | EDHOC (oscore only) | ACE-OAuth (psk or oscore only)
```

## Architecture

```
                    coap_security_config
                    { mode, credentials (variant), ace_bootstrap? }
                             │
              ┌──────────────┼──────────────────────┐
              │ (if ace_bootstrap set)               │ (if oscore + bootstrap==edhoc)
              ▼                                      ▼
      ACE-OAuth token exchange                EDHOC handshake (lakers)
      (existing HTTP client,                  edhoc_exporter() derives
       out-of-band, before                    sender_id/recipient_id/
       provider construction)                 master_secret/master_salt
              │                                      │
              └──────────────┬───────────────────────┘
                              ▼
                   populates psk_credentials
                   or oscore_credentials
                              │
                              ▼
                make_security_provider(coap_security_config)
                              │
        ┌──────────┬──────────┬──────────┬──────────┐
        ▼          ▼          ▼          ▼          ▼
   no_auth_    dtls_psk_  dtls_pki_  dtls_rpk_   oscore_
   provider    provider   provider   provider    provider
        │          │          │          │          │
        │  configure_session() — session/handshake setup (DTLS modes)
        │                                            │
        │                                  protect()/unprotect() — per-
        │                                  message wrap (OSCORE; composes
        │                                  with any block-transfer mode)
        ▼          ▼          ▼          ▼          ▼
              coap_context_t / coap_session_t (libcoap)
```

### Request flow additions (relative to `coap-transport` design.md)

Client-side, before the existing "construct CoAP POST request" step:
1. If `security.ace_bootstrap` is set, perform the AS token exchange once
   (cached/renewed per token lifetime, not per-request) and populate
   `psk_credentials` or `oscore_credentials`.
2. If `security.mode == oscore` and `bootstrap_method == edhoc`, run the
   EDHOC handshake once per session and populate `oscore_credentials` from
   `edhoc_exporter()`.
3. Construct the provider via `make_security_provider(security)`, call
   `configure_session()` once at session setup, and call `protect()` on the
   outgoing PDU immediately before transmission (`unprotect()` on the
   incoming PDU immediately after receipt) for every message, including
   each individual block in a block-wise transfer.

## Components and Interfaces

### 1. `coap_auth_mode` and `coap_security_config`

New header `include/raft/coap_security.hpp`:

```cpp
namespace kythira {

enum class coap_auth_mode { none, dtls_psk, dtls_pki, dtls_rpk, oscore };

struct psk_credentials {
    std::string identity;
    std::vector<std::byte> key;
};

struct pki_credentials {
    std::string cert_file, key_file, ca_file;
    bool verify_peer_cert{true};
    std::vector<std::string> cipher_suites;
};

struct rpk_credentials {
    std::vector<std::byte> public_key;
    std::vector<std::byte> private_key;
    std::vector<std::vector<std::byte>> trusted_peer_keys;
};

enum class oscore_bootstrap { static_provisioned, edhoc };

struct edhoc_params {
    std::vector<std::byte> identity_credential;  // device's own EDHOC credential
    std::vector<std::byte> peer_credential;      // expected peer credential
    // method/suite selection per RFC 9528; defaults chosen for constrained devices
};

struct oscore_credentials {
    std::vector<std::byte> sender_id, recipient_id, master_secret, master_salt;
    std::string aead_algorithm{"AES-CCM-16-64-128"};
    oscore_bootstrap bootstrap_method{oscore_bootstrap::static_provisioned};
    edhoc_params edhoc;  // used only when bootstrap_method == edhoc
};

enum class ace_target_profile { dtls_psk, oscore };

struct ace_oauth_config {
    std::string as_token_endpoint;
    std::string client_id, client_secret, scope;
    ace_target_profile target_profile;
};

struct coap_security_config {
    coap_auth_mode mode{coap_auth_mode::none};
    std::variant<std::monostate, psk_credentials, pki_credentials,
                 rpk_credentials, oscore_credentials> credentials;
    std::optional<ace_oauth_config> ace_bootstrap;
};

}  // namespace kythira
```

### 2. `coap_security_provider` interface

```cpp
class coap_security_provider {
public:
    virtual ~coap_security_provider() = default;
    virtual auto configure_session(coap_context_t*) -> void = 0;
    virtual auto protect(coap_pdu_t*) -> coap_pdu_t* = 0;   // identity for non-OSCORE modes
    virtual auto unprotect(coap_pdu_t*) -> coap_pdu_t* = 0; // identity for non-OSCORE modes
};

auto make_security_provider(const coap_security_config&) -> std::unique_ptr<coap_security_provider>;
```

Five concrete implementations:

- **`no_auth_provider`** — both hooks are identity/no-op.
- **`dtls_psk_provider`** — wraps the existing logic at
  `coap_transport_impl.hpp:763-787` (client) / `:2295-2422` (server) behind
  `configure_session()`; `protect`/`unprotect` are no-ops (DTLS handles
  message protection transparently at the session layer).
- **`dtls_pki_provider`** — wraps the existing logic at
  `coap_transport_impl.hpp:674-757` (client) / `:2236-2340`ish (server);
  same no-op message hooks.
- **`dtls_rpk_provider`** — new. Shares `dtls_pki_provider`'s
  `coap_dtls_pki_t` setup, differing only in `pki_key.key_type`
  (raw-public-key credential type instead of `COAP_PKI_KEY_PEM`) and in
  validating the peer's raw public key against `rpk_credentials
  ::trusted_peer_keys` instead of a CA chain.
- **`oscore_provider`** — new. `configure_session()` builds the context via
  `coap_new_oscore_conf()` and either `coap_new_client_session_oscore()` or
  the `_psk`/`_pki` combined variant if an underlying DTLS mode is also
  selected (defense in depth); server side calls
  `coap_context_oscore_server()` and `coap_new_oscore_recipient()` per peer.
  `protect()`/`unprotect()` are real: they wrap/unwrap each outgoing/
  incoming PDU through libcoap's OSCORE message processing, called once per
  block in block-wise transfer so it composes with either RFC 7959 or
  Q-Block without special-casing.

### 3. Credential-provisioning steps (run before provider construction)

```cpp
// EDHOC: only relevant when mode == oscore && bootstrap_method == edhoc
auto run_edhoc_handshake(const edhoc_params&, /* session/transport handle */)
    -> oscore_credentials;  // derived via lakers' edhoc_exporter()

// ACE-OAuth: only relevant when security.ace_bootstrap is set
auto run_ace_token_exchange(const ace_oauth_config&)
    -> std::variant<psk_credentials, oscore_credentials>;
```

Both are pure "populate credentials" steps; neither knows about the
provider that will consume their output. `run_ace_token_exchange` uses the
project's existing HTTP client (cpp-httplib, already a vcpkg dependency),
not a new HTTP stack. The composition order in `coap_client`/`coap_server`
construction:

```cpp
auto security = config.security;
if (security.ace_bootstrap) {
    auto result = run_ace_token_exchange(*security.ace_bootstrap);
    // populate security.credentials from result, per target_profile
}
if (security.mode == coap_auth_mode::oscore) {
    auto& osc = std::get<oscore_credentials>(security.credentials);
    if (osc.bootstrap_method == oscore_bootstrap::edhoc) {
        osc = run_edhoc_handshake(osc.edhoc, /* ... */);
    }
}
auto provider = make_security_provider(security);
provider->configure_session(_coap_context);
```

### 4. Legacy field translation (Requirement 8)

`coap_client_config`/`coap_server_config` gain a `coap_security_config
security{}` member. A translation function runs once at construction:

```cpp
auto translate_legacy_fields(const coap_client_config& cfg) -> coap_security_config {
    if (cfg.security.mode != coap_auth_mode::none) {
        // explicit mode set: legacy fields must be empty, else throw
        return cfg.security;
    }
    if (!cfg.cert_file.empty()) {
        return {coap_auth_mode::dtls_pki,
                pki_credentials{cfg.cert_file, cfg.key_file, cfg.ca_file,
                                 cfg.verify_peer_cert, cfg.cipher_suites}};
    }
    if (!cfg.psk_identity.empty()) {
        return {coap_auth_mode::dtls_psk, psk_credentials{cfg.psk_identity, cfg.psk_key}};
    }
    return {coap_auth_mode::none, std::monostate{}};
}
```

This reproduces today's field-inference behavior exactly when
`security.mode` is left at its default, satisfying Requirement 8.1 without
touching `setup_dtls_context()`'s existing branches — they become the
implementation of `dtls_psk_provider`/`dtls_pki_provider` rather than being
rewritten.

## Data Models

See Component 1 for `coap_auth_mode`, `coap_security_config`, and the four
credential structs. New exception types, added to the existing
`coap_transport_error` hierarchy (`.kiro/specs/coap-transport/design.md`):

```cpp
class coap_unsupported_security_mode_error : public coap_security_error {
public:
    coap_unsupported_security_mode_error(coap_auth_mode mode, const std::string& missing_capability);
};

class coap_credential_bootstrap_error : public coap_security_error {
public:
    coap_credential_bootstrap_error(const std::string& mechanism, const std::string& reason);
    // mechanism: "edhoc" or "ace-oauth"
};

class coap_security_config_error : public coap_security_error {
public:
    explicit coap_security_config_error(const std::string& message);
    // raised on inconsistent mode/credential combinations (Requirement 1.3, 2.4)
};
```

## Correctness Properties

### Property 1: Mode selection is explicit, never inferred

**Validates: Requirements 1.2, 1.3**

`make_security_provider()` switches solely on `coap_security_config::mode`;
no code path inspects which credential fields are non-empty to *decide* the
mode (only `translate_legacy_fields()`, confined to the migration path,
does that inference, and only when `mode == none`).

### Property 2: Session-level and message-level hooks are independent

**Validates: Requirements 1.4, 1.5, 4.5**

Every provider except `oscore_provider` leaves `protect()`/`unprotect()` as
identity functions; only `oscore_provider` does real work there, and its
`configure_session()` is a no-op unless composed with an underlying DTLS
mode. This means OSCORE protection is applied uniformly per-PDU regardless
of which block-transfer mechanism (RFC 7959 or Q-Block) produced that PDU.

### Property 3: Existing PSK/PKI behavior is preserved byte-for-byte

**Validates: Requirements 2.1, 2.2, 8.1, 8.3**

`dtls_psk_provider` and `dtls_pki_provider` call the exact existing
validation and setup code paths (length checks, chain depth, CN callback);
they are thin wrappers, not reimplementations, so the full existing
`coap-transport` DTLS test suite exercises identical logic through the new
entry point.

### Property 4: RPK reuses PKI's session mechanics

**Validates: Requirement 3.3**

`dtls_rpk_provider` and `dtls_pki_provider` both configure libcoap through
`coap_dtls_pki_t`; they differ only in `pki_key.key_type` and in what
"peer matches trust set" means (key equality vs. chain validation), so RPK
introduces no new session-establishment code path.

### Property 5: OSCORE composes with an underlying DTLS mode

**Validates: Requirement 4.2**

`oscore_provider::configure_session()` calls the plain
`coap_new_client_session_oscore()` when no DTLS mode is layered underneath,
or the `_psk`/`_pki` combined variant when one is — the choice is
determined by whether `coap_security_config` for the *session* names a
combined mode, not by two independent, potentially-conflicting provider
instances running at once.

### Property 6: Credential source is invisible to the provider

**Validates: Requirements 5.2, 5.3, 6.1, 6.4**

`oscore_provider` and `dtls_psk_provider` read only the final, populated
`oscore_credentials`/`psk_credentials` struct — they contain no branch on
whether that struct was filled by static config, EDHOC, or ACE-OAuth. This
guarantees Requirement 6.1 (ACE-OAuth is not a sixth mode) structurally,
not just by convention.

### Property 7: Capability checks precede session establishment

**Validates: Requirements 7.1, 7.2, 7.3**

`oscore_provider::configure_session()` calls `coap_oscore_is_supported()`
as its first statement and throws `coap_unsupported_security_mode_error`
before any libcoap context/session mutation, so a capability mismatch never
leaves partially-initialized libcoap state behind.

### Property 8: Bootstrap failures never degrade to a weaker mode

**Validates: Requirements 5.4, 6.3**

Both `run_edhoc_handshake()` and `run_ace_token_exchange()` propagate
failure as `coap_credential_bootstrap_error` and are called before
`make_security_provider()` — there is no code path that catches a
bootstrap failure and proceeds to construct a `no_auth_provider` or a
statically-keyed provider as a fallback.

## Error Handling

- **Configuration-time errors** (Requirement 1.3, 2.3): thrown as
  `coap_security_config_error` during `coap_client`/`coap_server`
  construction, before any network or libcoap resource is allocated.
- **Capability mismatches** (Requirement 7): thrown as
  `coap_unsupported_security_mode_error` at the start of
  `configure_session()`.
- **Bootstrap failures** (EDHOC or ACE-OAuth): thrown as
  `coap_credential_bootstrap_error` before `configure_session()` is ever
  reached for the target provider.
- **Peer-side authentication failures** (bad certificate, RPK mismatch, PSK
  identity mismatch, OSCORE replay/decryption failure): continue to surface
  through the existing `coap_security_error` path
  (`.kiro/specs/coap-transport/design.md`), unchanged.

## Testing Strategy

- `tests/coap_security_mode_selection_test.cpp` — Requirement 9.1, 9.2:
  each `coap_auth_mode` selects the correct provider type; inconsistent
  mode/credential pairs throw `coap_security_config_error`.
- `tests/coap_legacy_config_migration_test.cpp` — Requirement 8.3, 9.8:
  every existing `coap-transport` DTLS test re-run unmodified against the
  new config path (via `translate_legacy_fields`).
- `tests/coap_dtls_rpk_test.cpp` — Requirement 9.3: RPK peer-key match
  succeeds, mismatch is rejected.
- `tests/coap_oscore_integration_test.cpp` — Requirement 9.4: client/server
  round-trip under plain OSCORE and under OSCORE-over-DTLS-PSK.
- `tests/coap_edhoc_oscore_bootstrap_test.cpp` — Requirement 9.5: EDHOC
  handshake derives a working OSCORE context; simulated handshake failure
  prevents session establishment.
- `tests/coap_ace_oauth_test.cpp` — Requirement 9.6: mock AS issues a token
  that correctly populates PSK credentials (DTLS profile) and, separately,
  OSCORE credentials (OSCORE profile); a rejected/failed exchange prevents
  transport initialization.
- `tests/coap_security_capability_check_test.cpp` — Requirement 9.7:
  `coap_oscore_is_supported()` stubbed false produces
  `coap_unsupported_security_mode_error` before any session mutation.

## Dependencies

### External Libraries

- **libcoap ≥ 4.3.5** (already pinned, `vcpkg.json`): `none`, `dtls_psk`,
  `dtls_pki`, `dtls_rpk`, and `oscore` modes require no version bump —
  confirmed via symbol inspection that OSCORE (`coap_new_oscore_conf` et
  al.) and Q-Block support are already compiled into the linked build.
- **lakers** (new dependency, EDHOC only): BSD-3-Clause, pre-built C static
  library + headers from the project's GitHub releases — no Rust toolchain
  required in this project's build/CI. Not in the vcpkg registry; requires
  a vcpkg overlay port (see Build Integration).
- **cpp-httplib** (already a vcpkg dependency): reused for the ACE-OAuth
  token-endpoint HTTP call rather than adding a second HTTP client.

## Build Integration

### vcpkg overlay port for `lakers`

Since `lakers` has no upstream vcpkg port, add an overlay port
(`vcpkg-overlays/lakers/`) that downloads the project's published pre-built
C static libraries and headers for the target triplet(s) in use, exposing a
`lakers::lakers` CMake target. This keeps dependency acquisition consistent
with the rest of the project's vcpkg-based workflow rather than introducing
a separate vendoring mechanism.

### CMakeLists.txt additions

```cmake
# EDHOC support (optional; only needed if oscore.bootstrap_method == edhoc
# is exercised)
find_package(lakers CONFIG QUIET)  # via overlay port
if(lakers_FOUND)
    target_link_libraries(raft_coap_transport INTERFACE lakers::lakers)
    target_compile_definitions(raft_coap_transport INTERFACE LAKERS_AVAILABLE)
endif()
```

`LAKERS_AVAILABLE` gates EDHOC bootstrap support the same way
`LIBCOAP_AVAILABLE` already gates real-vs-stub CoAP behavior in
`coap-transport`'s design — when absent, `bootstrap_method == edhoc` fails
construction with a clear "EDHOC support not compiled in" error rather than
silently behaving as `static_provisioned`.

## Migration Strategy

### Phase 1 — Config model and provider abstraction (no new libcoap features)
Introduce `coap_security_config`, `coap_security_provider`, and
`no_auth_provider`/`dtls_psk_provider`/`dtls_pki_provider` as thin wrappers
around existing logic, plus `translate_legacy_fields()`. No behavior change
for existing deployments.

### Phase 2 — RPK
Add `dtls_rpk_provider`, sharing `coap_dtls_pki_t` plumbing with the PKI
provider.

### Phase 3 — OSCORE (static-provisioned)
Add `oscore_provider` against the confirmed-present libcoap OSCORE API.

### Phase 4 — EDHOC bootstrap for OSCORE
Add the vcpkg overlay port, `run_edhoc_handshake()`, and
`oscore_bootstrap::edhoc` support.

### Phase 5 — ACE-OAuth
Add `run_ace_token_exchange()` against either the `dtls_psk` or `oscore`
target profile; no dependency on Phase 4 being complete, since ACE-OAuth
and EDHOC are independent provisioning mechanisms for the same credential
structs.
