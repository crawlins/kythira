# Implementation Plan

Depends on `.kiro/specs/service-colocation/` tasks 1-3 (the service module
interface and `kythira_node`) for task 6 on, and on the persistent state
machine for the Raft groups. Tasks 1-5 need neither and can start now. Each
task is one PR unless noted.

- [ ] 1. Module skeleton and vendored headers
  - `KYTHIRA_HSM` Kconfig option (default off), `services/hsm/` layout per
    `services/README.md`, boundary check coverage
  - OASIS PKCS#11 v3.0 headers vendored unchanged under
    `services/hsm/third_party/pkcs11/`, with their licence
  - Off-by-default build proved unchanged
  - _Requirements: 1.1-1.4_

- [ ] 2. Shamir secret sharing and share encoding
  - GF(2^8) split and combine, share format with checksum and MK check
    value, property tests (any `k` rebuild, `k - 1` reveal nothing
    testable, corruption rejected)
  - _Requirements: 4.1, 4.2, 13.1_

- [ ] 3. Secure memory
  - `secure_arena` (mlock, `MADV_DONTDUMP`, zeroize), `PR_SET_DUMPABLE`
    and `RLIMIT_CORE` on load, startup error on lock failure unless
    allowed
  - _Requirements: 12.3, 12.4_

- [ ] 4. Mechanisms and attribute policy
  - OpenSSL 3 EVP implementations for every key type and mechanism in
    6.1-6.2; `attribute_policy` with one function and one test per rule in
    6.3-6.4; excluded functions and mechanisms answer as 6.5
  - _Requirements: 6.1-6.5, 13.3_

- [ ] 5. Token and catalog state machines
  - `hsm_catalog_state_machine` (tokens, client bindings, wrapped TKs,
    unseal mode) and `token_state_machine` (objects, attributes, sealed
    material, scrypt PIN verifiers, login counters) on the persistent
    concept; leader-side generation with wrapped material in the entry
  - Plaintext-scan test over data directory, log and snapshots
  - _Requirements: 3.1-3.3, 7.1, 7.2, 7.4, 13.4_

- [ ] 6. HSM service module and session layer
  - `hsm_module` on the service module interface: groups on the node's
    multi-raft host, data directory, admin API (init, unseal, seal,
    status, tokens)
  - Session layer: logins, sessions, op state, read-index check before
    use, key cache by `(handle, version)`
  - `native_client` for co-located modules
  - _Requirements: 2.1-2.3, 3.4, 3.5, 7.3_

- [ ] 7. Operator unseal, init, seal and rekey
  - `hsm_ctl init|unseal|seal|rekey|status`; partial shares in memory with
    timeout; rekey in one entry
  - _Requirements: 4.1, 4.3, 4.5-4.7_

- [ ] 8. Node unseal
  - Share sealing backends: key file, TPM 2.0, AWS KMS, Azure Key Vault,
    GCP KMS; peer share exchange over mTLS checked against catalog
    membership; stays sealed below `k`
  - _Requirements: 4.4, 4.8, 13.5_

- [ ] 9. Wire protocol and remote access
  - Framed protobuf protocol over TLS 1.3 with client certificates and
    over a Unix socket with `SO_PEERCRED` allow list; identity-to-token
    bindings; hidden tokens answer `CKR_TOKEN_NOT_PRESENT`
  - _Requirements: 2.4, 8.1-8.3_

- [ ] 10. `libkythira_pkcs11.so`
  - PKCS#11 v3.0 entry points, config file, static deps with a version
    script, OS locking, failover and re-login, slot and token info
  - `pkcs11test`, `pkcs11-tool`, OpenSSL `pkcs11-provider` (including a
    TLS handshake with a token key), p11-kit, Go and Java smoke tests in CI
  - _Requirements: 5.1-5.7, 13.2_

- [ ] 11. `hsm_node`, cluster tests and docker-chaos
  - Stand-alone daemon (`kythira_node` with only the HSM module), cluster
    tests with kills during generation and rekey
  - docker-chaos scenario stand-alone and co-located, Docker and rootless
    Podman, service-name addressing
  - _Requirements: 2.1, 2.5, 13.5, 13.6_

- [ ] 12. Consumer wrapper and external modules
  - Core `kythira::pkcs11::module` (dlopen or native client, RFC 7512 URI
    lookup); documented YubiHSM 2 / CloudHSM common subset; consumer tests
    against SoftHSM2; `hsm_ctl migrate`
  - _Requirements: 9.1-9.4_

- [ ] 13. Consumers
  - Cluster CA signing key on a token; Kinesis `pkcs11` key provider; IAM
    OIDC/STS signing keys on a token; each opt-in, defaults unchanged
  - _Requirements: 10.1-10.4_

- [ ] 14. Audit, metrics and IAM authorization
  - Audit log with optional ledger sink; metrics per 11.3; `hsm:` actions
    on token ARNs when IAM is present
  - _Requirements: 8.4, 11.1-11.3_

- [ ] 15. Documentation
  - `services/hsm/README.md` and `doc/hsm.md` opening with the threat model
    table; startup log line; operator guide for both unseal modes and for
    moving to an external module
  - _Requirements: 4.8, 12.1, 12.2_
