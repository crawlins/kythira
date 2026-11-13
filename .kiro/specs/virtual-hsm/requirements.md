# Requirements Document

## Introduction

Kythira's services hold their own secrets today. The cluster CA keeps its
private key in Raft state, encrypted under a key derived from an operator
passphrase. Kinesis wraps each stream's data key with a `kms_key_provider`
that is either AWS KMS or a local provider sealed like the CA key. IAM keeps
its OIDC signing keys in its own state. Each one rolls its own sealing, and
none of them can move its keys into a real HSM without new code.

This spec adds a **virtual HSM**: a software security module that stores
keys, performs cryptographic operations with them, and speaks **PKCS#11**,
the standard interface that OpenSSL, p11-kit, NSS, Java, Go and most HSM
vendors use. It runs as one more Kythira service, either co-located with
other services in one node or stand-alone on its own cluster, and it
replicates its key store through Raft.

The name says what it is not. A hardware HSM keeps keys inside a tamper
resistant boundary that host software cannot read. The virtual HSM cannot:
while a key is in use it sits in the memory of the host process, and anyone
with root on that host, a debugger, a memory dump or a hypervisor can read
it. What it does give is a single, audited place where keys live; keys that
are never written to disk, the Raft log or a snapshot in plaintext; a master
key that no single node's disk holds whole; PKCS#11 attribute enforcement
(a key marked non-extractable never leaves in plaintext through the API);
and a standard boundary, so the same consumers can later use a YubiHSM, a
network HSM or AWS CloudHSM by changing configuration only.

It is meant as the key store under a future Kythira KMS. The KMS itself
(key policies, grants, aliases, the AWS KMS wire API) is **out of scope**.

**Scope**: the HSM state machine and key store, master key handling with
Shamir secret sharing, the PKCS#11 v3.0 client library, the native C++
client, the stand-alone daemon and the co-located module, swapping in an
external PKCS#11 module, moving the CA, Kinesis and IAM keys behind it,
audit, and tests. **Out of scope**: a KMS API, FIPS 140-3 validation,
threshold signatures (no key is ever split for use, only the master key for
storage), remote attestation, and the PKCS#11 features listed as excluded in
Requirement 6.

## Glossary

- **Token**: a PKCS#11 token: a named, independent key store with its own
  Security Officer (SO) PIN, user PIN and objects. One token is one slot.
- **Object**: a PKCS#11 object on a token: a key (secret, public, private),
  a certificate or a data object, with its attributes.
- **Token object**: an object with `CKA_TOKEN=true`, stored and replicated.
  A **session object** lives only in the serving node's memory for the life
  of its session.
- **Master key (MK)**: the AES-256 key that wraps every token key. It exists
  in plaintext only in the memory of an unsealed node.
- **Token key (TK)**: a per-token AES-256 key, stored wrapped by the MK;
  it wraps every object's secret material on that token.
- **Share**: one of `n` Shamir shares of the MK, any `k` of which rebuild it.
- **Sealed / unsealed**: a node is sealed until it holds the MK; a sealed
  node serves no cryptographic operation.
- **External module**: a vendor PKCS#11 library (`yubihsm_pkcs11.so`,
  CloudHSM's, a network HSM's, SoftHSM2) used in place of the virtual HSM.

## Requirements

### Requirement 1: Opt-in module

**User Story:** As a maintainer, I want the virtual HSM built only when
asked for, and kept separable like every other service module.

#### Acceptance Criteria

1. The HSM SHALL be built only with a new Kconfig option `KYTHIRA_HSM`
   (default off), depending on OpenSSL 3.
2. Its sources SHALL follow `services/README.md`: `services/hsm/` with
   `include/kythira/hsm/`, `cmd/hsm_node/`, `tests/` and `docker/`, and the
   PKCS#11 client library as its own target under `services/hsm/pkcs11/`.
3. HSM code SHALL include only Kythira's public headers; the module
   boundary check SHALL cover it.
4. With the option off, no existing target, test or binary SHALL change.

### Requirement 2: Co-located or stand-alone

**User Story:** As an operator, I want to run the HSM inside a node that
already serves other Kythira services, or on its own hardened cluster.

#### Acceptance Criteria

1. The HSM SHALL be a service module under
   `.kiro/specs/service-colocation/`, so the same code runs in the shared
   node (`kythira_node`) next to other modules and in `hsm_node`, which is
   that node with only the HSM enabled.
2. Co-located, the HSM SHALL use the node's multi-raft host, Raft port,
   TLS identity and metrics endpoint, with its own groups and data
   directory.
3. Co-located consumers in the same process SHALL reach it through the
   native client in process, with no network hop, and still be
   authenticated and authorized (Requirement 8).
4. Clients in other processes SHALL reach it over a Unix domain socket
   (same host) or mutual TLS (other hosts).
5. Stand-alone SHALL be the documented recommendation for production
   secrets: co-location shares the process, so any memory disclosure in a
   co-located service is a disclosure of the HSM's keys (Requirement 12).

### Requirement 3: Replicated key store

**User Story:** As an operator, I want keys to survive the loss of a node,
and every node to agree on which keys exist.

#### Acceptance Criteria

1. Token objects, token metadata, PIN verifiers and wrapped token keys SHALL
   live in one Raft group per token, on the persistent state machine.
2. Object secret material SHALL be stored, logged, snapshotted and
   replicated only sealed by its token key (AES-256-GCM), with the token,
   object handle and attributes bound as additional data so a sealed value
   cannot be moved to another object.
3. Key generation and key import SHALL happen on the leader; the Raft entry
   SHALL carry the wrapped material, so replicas apply it without
   generating anything and stay identical.
4. Operations that only use a key (sign, verify, encrypt, decrypt, wrap,
   unwrap to a session object, MAC, derive to a session object) SHALL be
   served by any unsealed replica that has applied the object, after a
   read-index check that the object still exists and has not been
   destroyed or changed.
5. Destroying an object or changing its attributes SHALL be linearizable:
   once `C_DestroyObject` returns, no replica SHALL perform an operation
   with that object.

### Requirement 4: Master key, Shamir shares and unseal

**User Story:** As a security owner, I want no single disk to hold what
decrypts the keys, and a choice between people and machines for unsealing.

#### Acceptance Criteria

1. Initialization SHALL generate the MK in memory, split it with Shamir
   secret sharing over GF(2^8) into `n` shares with threshold `k`
   (`1 <= k <= n <= 16`, `k >= 2` unless an explicit development flag is
   set), and print or hand out each share exactly once. The MK SHALL never
   be written anywhere.
2. Each share SHALL carry a checksum and the MK's key check value, so a
   wrong or corrupted share is rejected with a clear error before
   combination.
3. **Operator unseal** SHALL accept shares from `k` operators through an
   authenticated admin endpoint (or `hsm_ctl unseal`) and hold partial
   shares only in memory, discarding them after a timeout.
4. **Node unseal** SHALL give each of `n` nodes one share, sealed at rest
   to that node (a key file with owner-only permissions, or a TPM 2.0 or
   cloud KMS key when configured). A starting node SHALL collect `k - 1`
   other shares from unsealed or sealed peers over mutual TLS, after the
   peer checks the requester's node certificate against the HSM group's
   membership.
5. The mode SHALL be chosen at initialization and recorded in the HSM
   catalog; switching modes SHALL require an unsealed cluster and a re-split.
6. `hsm_ctl rekey` SHALL generate a new MK, re-wrap every token key in one
   Raft entry, and hand out new shares; old shares SHALL stop working.
7. `hsm_ctl seal` SHALL make every node discard the MK and all unwrapped
   keys from memory.
8. The docs SHALL state that in node unseal mode every unsealed node holds
   the whole MK in memory, and that Shamir protects the data at rest, not a
   running node.

### Requirement 5: PKCS#11 client library

**User Story:** As an application developer, I want to point any PKCS#11
consumer at a library and have it work.

#### Acceptance Criteria

1. `libkythira_pkcs11.so` SHALL implement PKCS#11 v3.0
   (`C_GetFunctionList` and `C_GetInterface`) with the functions of
   Requirement 6, and SHALL return `CKR_FUNCTION_NOT_SUPPORTED` for the
   rest.
2. It SHALL find its HSM through a configuration file named by
   `KYTHIRA_PKCS11_CONF` (endpoints, client certificate, CA bundle, socket
   path), and SHALL keep no key material: every operation goes to the HSM.
3. It SHALL link its dependencies statically with hidden symbols, so it
   loads into processes that use another OpenSSL or protobuf.
4. It SHALL be safe for multi-threaded callers (`CKF_OS_LOCKING_OK`) and
   SHALL honour `C_Initialize` locking arguments.
5. On failover to another HSM node it SHALL reconnect and re-login an
   open session transparently for single-part operations; a multi-part
   operation in flight SHALL fail with `CKR_DEVICE_ERROR`, and session
   objects SHALL be reported lost with `CKR_SESSION_HANDLE_INVALID`.
6. It SHALL work with OpenSSL 3 through `pkcs11-provider`, with p11-kit,
   with `pkcs11-tool` (OpenSC), and with Go's and Java's (SunPKCS11)
   PKCS#11 bindings.
7. Slot info SHALL NOT set `CKF_HW_SLOT`, and token info SHALL name the
   model "Kythira virtual HSM", so a consumer that checks can tell it is
   software.

### Requirement 6: Mechanisms and objects

**User Story:** As a consumer, I want the keys and algorithms that TLS,
code signing, JWT signing and envelope encryption need.

#### Acceptance Criteria

1. Key types SHALL include AES (128, 192, 256), generic secret (HMAC),
   RSA (2048, 3072, 4096), EC on P-256, P-384 and P-521, and Ed25519.
2. Mechanisms SHALL include: AES-GCM, AES-CBC-PAD, AES-KW and AES-KWP;
   SHA-256/384/512 digests and HMACs; RSA PKCS#1 v1.5, PSS and OAEP; ECDSA
   with and without hashing; EdDSA; ECDH1 derive; HKDF; key generation for
   each key type; `C_WrapKey` and `C_UnwrapKey`; `C_GenerateRandom`.
3. Attributes SHALL be enforced as PKCS#11 specifies, at least
   `CKA_SENSITIVE`, `CKA_EXTRACTABLE`, `CKA_NEVER_EXTRACTABLE`,
   `CKA_ALWAYS_SENSITIVE`, the usage flags (`CKA_SIGN`, `CKA_DECRYPT`,
   `CKA_WRAP`, ...), `CKA_WRAP_WITH_TRUSTED`, `CKA_TRUSTED` (settable only
   by the SO) and `CKA_WRAP_TEMPLATE`/`CKA_UNWRAP_TEMPLATE`.
4. A sensitive key's value SHALL never be returned by `C_GetAttributeValue`,
   and a non-extractable key SHALL never be wrapped.
5. Excluded, returning `CKR_FUNCTION_NOT_SUPPORTED` or
   `CKR_MECHANISM_INVALID`: DES/3DES, MD5, SHA-1 signing, DSA, RSA
   without padding, PKCS#11 3.0 message-based functions, `C_SignRecover`
   and `C_VerifyRecover`, `C_GetOperationState`/`C_SetOperationState`, and
   protected authentication paths.

### Requirement 7: Tokens, PINs and sessions

#### Acceptance Criteria

1. `C_InitToken`, `C_InitPIN` and `C_SetPIN` SHALL work as specified; PINs
   SHALL be stored only as scrypt verifiers.
2. Failed logins SHALL be counted per token in replicated state; after a
   configurable number (default 10) the PIN SHALL be locked
   (`CKR_PIN_LOCKED`) until the SO resets it, and the counter flags
   (`CKF_USER_PIN_COUNT_LOW`, `..._FINAL_TRY`, `..._LOCKED`) SHALL be
   reported.
3. Sessions and multi-part operation state SHALL live on the serving node
   only.
4. An admin API SHALL create and delete tokens; deleting a token SHALL need
   the SO PIN and SHALL destroy its token key, so its objects are
   unrecoverable even from old snapshots once the MK is rekeyed.

### Requirement 8: Client authentication and authorization

#### Acceptance Criteria

1. Every connection SHALL be authenticated: mutual TLS for remote clients,
   peer credentials (`SO_PEERCRED`) checked against an allow list for the
   Unix socket, and the calling module's identity for in-process calls.
2. A client identity SHALL be bound to the tokens it may open; opening any
   other token SHALL fail with `CKR_TOKEN_NOT_PRESENT`, so a client cannot
   even see which other tokens exist.
3. The PKCS#11 login SHALL be required on top of the transport identity.
4. When IAM is co-located or reachable, a token MAY name IAM principals
   instead of certificate subjects, authorized with `hsm:` actions on the
   token's ARN.

### Requirement 9: External PKCS#11 modules

**User Story:** As an operator, I want to move to real hardware later
without changing the services that use keys.

#### Acceptance Criteria

1. Kythira consumers of keys (Requirement 10) SHALL refer to keys by a
   PKCS#11 URI (RFC 7512) and load the module named in their configuration,
   so pointing them at `yubihsm_pkcs11.so`, CloudHSM's library, a network
   HSM's library or SoftHSM2 needs no code change.
2. A consumer SHALL use only mechanisms and attributes that the YubiHSM 2
   and AWS CloudHSM PKCS#11 libraries both support, and the docs SHALL list
   them.
3. `hsm_ctl migrate` SHALL copy keys from the virtual HSM to an external
   module by wrapping them under a key the external module holds, for keys
   marked extractable; non-extractable keys SHALL be reported as needing
   re-issue.
4. CI SHALL run the consumer tests against SoftHSM2 as a stand-in for an
   external module.

### Requirement 10: Kythira consumers

#### Acceptance Criteria

1. The cluster CA SHALL be able to keep its signing key on a PKCS#11 token
   and sign certificates through it, as an alternative to the passphrase
   sealed key.
2. Kinesis SHALL gain a `pkcs11` `kms_key_provider` that wraps and unwraps
   stream data keys with an AES key on a token.
3. IAM SHALL be able to keep its OIDC and STS signing keys on a token.
4. Each consumer SHALL keep its current key handling as the default, so
   nothing changes for an existing deployment.

### Requirement 11: Audit and observability

#### Acceptance Criteria

1. Every login, object creation, attribute change, destruction, wrap,
   unwrap, unseal, seal and rekey SHALL be written to an audit log with the
   client identity, token, object handle and label, and result; signing
   and encryption calls SHALL be counted per client and object and sampled
   into the log.
2. The audit log SHALL be able to append to a verifiable ledger
   (`.kiro/specs/verifiable-ledger/`) when one is configured.
3. Metrics SHALL report sealed state, operations by mechanism and result,
   latency, login failures, PIN locks and replica lag. Metrics and logs
   SHALL never carry key material, PINs or shares.

### Requirement 12: Honest limits and memory hygiene

#### Acceptance Criteria

1. The module README and `doc/hsm.md` SHALL open with a threat model table:
   what the virtual HSM protects against (stolen disks, backups and
   snapshots; a lost node; consumers that should not read raw keys) and
   what it does not (root on a serving host, a hypervisor, memory dumps,
   side channels, a compromised co-located service).
2. Each node SHALL log at startup that it is a software HSM and keys are in
   host memory while in use.
3. Unwrapped key material SHALL live in `mlock`ed, `MADV_DONTDUMP` memory,
   be zeroized on release, and the process SHALL disable core dumps and
   ptrace attachment (`PR_SET_DUMPABLE=0`) while the HSM module is loaded.
4. A failure to lock memory SHALL be a startup error unless an explicit
   flag allows it.

### Requirement 13: Testing

#### Acceptance Criteria

1. Shamir split and combine SHALL be property-tested: any `k` shares
   rebuild the MK, any `k - 1` reveal nothing testable, and corrupted
   shares are rejected.
2. The PKCS#11 library SHALL pass Google's `pkcs11test` suite for the
   supported functions, and scripted `pkcs11-tool` and OpenSSL
   `pkcs11-provider` runs (generate, sign, verify, encrypt, TLS handshake
   with a token key).
3. Attribute enforcement SHALL have a test per rule in 6.3 and 6.4.
4. A test SHALL show that no plaintext key bytes appear in the data
   directory, the Raft log or a snapshot after generating and using keys.
5. Cluster tests SHALL kill the leader during key generation and during
   rekey, and SHALL check that every replica agrees and every key still
   works; node unseal SHALL stay sealed with only `k - 1` reachable nodes.
6. A docker-chaos scenario SHALL run the HSM stand-alone and co-located,
   under Docker and rootless Podman, with service-name addressing.
