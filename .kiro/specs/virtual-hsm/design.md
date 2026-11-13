# Design Document

## Overview

```
PKCS#11 consumer (OpenSSL, Go, Java, pkcs11-tool)
   │ dlopen
   ▼
libkythira_pkcs11.so ──mTLS or Unix socket──┐
                                            ▼
Kythira service in the same node ──in-process native client──► hsm module
                                                               │
                     ┌─────────────────────────────────────────┤
                     ▼                                         ▼
          session layer (per node)                    key cache (mlock'd)
          logins, sessions, op state                  unwrapped TKs and
                     │                                objects, by handle
                     ▼
     hsm catalog group ── tokens, client bindings, wrapped TKs, MK check
     token group (one per token) ── objects, attributes, wrapped material,
                                    PIN verifiers, login counters
```

The module is a service module (`.kiro/specs/service-colocation/`). In
`kythira_node` it shares the multi-raft host with other modules; `hsm_node`
is the same node with only the HSM module enabled.

## Key design decisions

### PKCS#11 is the only boundary consumers see

Consumers never call a Kythira-specific key API: the CA, Kinesis and IAM
use PKCS#11 through a small wrapper (`kythira::pkcs11::module`, a dlopen of
any module plus RFC 7512 URI lookup). In-process, the wrapper is bound to
the HSM module's native client instead of a dlopen'd library, but it
exposes the same calls. This is what makes Requirement 9 a configuration
change: the consumer cannot tell a YubiHSM from the virtual HSM, so it
cannot come to depend on anything only the virtual HSM does. The cost is
that consumers are limited to the PKCS#11 subset both YubiHSM 2 and
CloudHSM implement; that subset covers ECDSA and RSA signing, AES-GCM
and AES key wrap, which is all the three consumers need.

### Two key layers under the master key

MK wraps one TK per token; a TK wraps that token's objects. Rekeying the
MK re-wraps a handful of TKs in one Raft entry, not every object.
Deleting a token destroys its TK, so its objects become noise even in old
snapshots once the MK they were wrapped under is retired (Requirement 7.4).
Stored material is sealed with AES-256-GCM under the TK, with a random
96-bit nonce and additional data of `token id | object handle | attribute
digest`, so a sealed value cannot be moved to another object or have its
attributes swapped. AES-KWP is used only where PKCS#11 asks for it, in
`C_WrapKey` and `C_UnwrapKey`.

### Generation on the leader, material in the log

Raft replicas must apply identically, so a replica cannot generate a key.
The leader generates the key, wraps it under the TK and proposes the
wrapped bytes. Every replica stores exactly those bytes. The Raft log and
snapshots therefore hold only ciphertext, and a node that joins later gets
keys through the ordinary snapshot path.

### Use on any replica, existence through read index

Signing and decryption dominate the load and change nothing, so they run
on whichever unsealed node the client reached. Before using an object
the node does a read-index round (or uses a valid leader lease) and checks
the object's version, so a destroyed or narrowed key is never used after
its change committed (Requirement 3.4, 3.5). Unwrapped keys are cached by
`(handle, version)` and dropped when a newer version applies.

### Shamir over the MK only

The MK is split with Shamir's scheme over GF(2^8), byte by byte, with
shares in the `{version, k, n, index, bytes, checksum, MK check value}`
form. Nothing else is split: a threshold signature scheme would keep keys
split while in use, but it needs per-algorithm protocols, and PKCS#11
consumers expect ordinary keys. Splitting the MK alone protects the data at
rest (every disk, backup and snapshot) and is what HashiCorp Vault and
SoftHSM-style products do.

Node unseal trades that protection for availability: each node keeps one
share sealed to itself, and a starting node asks peers for theirs. Peers
answer only a node certificate that names a current voter of the HSM
catalog group, and send the share encrypted to that node's TLS session. A
thief needs `k` nodes' disks and their node keys; a running node holds the
whole MK in memory. Operator unseal keeps the MK off every disk, at the
price of a person at each restart. The share sealing key can be a file,
a TPM 2.0 key (via `tpm2-tss`), or a cloud KMS key (AWS KMS, Azure Key
Vault, GCP KMS through the existing cloud clients), chosen per node.

### Sessions are node-local

PKCS#11 sessions, logins and multi-part operation state live where the
client connected. Replicating them would put every `C_SignUpdate` through
Raft for a rare failover. The client library pins a session to one node
and, after a failover, reopens and re-logs-in sessions (it keeps the PIN
only in the caller's process memory, as any PKCS#11 library must to
honour `C_Login` semantics across reconnects). A multi-part operation in
flight fails; single-part calls retry once.

### Wire protocol

Framed protobuf over TLS 1.3 with client certificates, or over a Unix
socket. One request maps to one PKCS#11 call, so the server stays a thin
dispatcher in front of the same code the in-process client calls. The
client library links protobuf-lite and OpenSSL statically with a version
script that exports only `C_GetFunctionList` and `C_GetInterface`.

### Memory hygiene

A `secure_arena` allocates key buffers from `mmap`ed pages with `mlock`
and `madvise(MADV_DONTDUMP)`, zeroizes with `OPENSSL_cleanse` on release,
and is the only allocator the key cache uses. OpenSSL key objects are made
with `EVP_PKEY_fromdata` from arena buffers and freed immediately after
use, so OpenSSL's own copies live only for one operation. The module sets
`PR_SET_DUMPABLE=0` and `RLIMIT_CORE=0` on load. None of this stops root,
and the docs say so.

## Components

| Component | Where | Role |
|---|---|---|
| `shamir.hpp` | `include/kythira/hsm/` | split, combine, share encoding |
| `secure_arena.hpp` | same | locked, zeroized key memory |
| `hsm_catalog_state_machine` | same | tokens, bindings, wrapped TKs, mode |
| `token_state_machine` | same | objects, attributes, PINs, counters |
| `mechanisms.hpp` | same | OpenSSL 3 EVP implementations per mechanism |
| `attribute_policy.hpp` | same | PKCS#11 attribute rules, one function per rule |
| `session_layer.hpp` | same | logins, sessions, op state, read-index checks |
| `unseal.hpp` | same | operator and node unseal, share sealing backends |
| `hsm_module.hpp` | same | the service module: groups, listeners, admin API |
| `native_client.hpp` | same | in-process client used by co-located modules |
| `libkythira_pkcs11` | `services/hsm/pkcs11/` | the PKCS#11 v3.0 library |
| `kythira::pkcs11::module` | `include/raft/pkcs11_module.hpp` (core) | consumer wrapper: dlopen or native, URI lookup |
| `hsm_ctl` | `services/hsm/cmd/hsm_ctl/` | init, unseal, seal, rekey, tokens, migrate |
| `hsm_node` | `services/hsm/cmd/hsm_node/` | stand-alone daemon |

The consumer wrapper lives in core because the CA (core), Kinesis and IAM
(modules) all use it and it has no HSM dependency: it speaks PKCS#11 to
whatever module it is given. The PKCS#11 headers come from the OASIS
PKCS#11 v3.0 distribution and are vendored unchanged.

## Error handling

- A sealed node answers every cryptographic call `CKR_DEVICE_ERROR` and
  the admin API reports `sealed`.
- A read-index failure (no quorum) answers `CKR_DEVICE_ERROR`, never a
  stale result.
- A wrapped blob whose GCM tag fails stops the token group and logs the
  handle; it means corruption or tampering and must not be retried.
- Unknown mechanisms and excluded functions answer as Requirement 6.5.

## Testing strategy

Unit tests per component, Shamir property tests, `pkcs11test` and
`pkcs11-tool`/`pkcs11-provider` scripts in CI, a plaintext-scan test over
the data directory, Raft log and snapshots, in-process cluster tests with
kills during generation and rekey, consumer tests run against both the
virtual HSM and SoftHSM2, and a docker-chaos scenario in both layouts.

## Open questions

- Whether node unseal should be allowed to bootstrap a brand-new cluster
  (first start) or always need one operator unseal first. The tasks assume
  the latter.
- Whether to offer a KMIP front end later; nothing here precludes it.
