# Design Document

## Overview

```
AWS CLI / SDK / Encryption SDK ──HTTPS, SigV4, JSON 1.1──┐
                                                          ▼
Kinesis, DynamoDB (same node) ──in-process kms.client──► kms module
Kinesis, DynamoDB (other cluster) ──HTTPS kms.client──►    │
                                                           │
             ┌──────────────── authorize ──────────────────┤
             ▼                                             ▼
   IAM evaluator (embedded replica)           kms catalog group (Raft)
   identity + key policy + grants             keys, versions, aliases,
   + kms:* condition keys                     policies, grants, tags,
                                              rotation and deletion jobs
                                                           │ object ids only
                                                           ▼
                                         hsm.client ─► virtual HSM "kms" token
                                         (or an external PKCS#11 module)
                                         backing keys: AES-256, RSA, EC, HMAC
```

The KMS is a service module (`.kiro/specs/service-colocation/`), number 7.
It owns one Raft group, the KMS catalog. It holds no key material: every
backing key is a non-extractable object on the virtual HSM's `kms` token,
and every cryptographic operation is an HSM call. It authorizes every
request through IAM's evaluator, which already implements AWS's key policy
rule.

## Key design decisions

### Metadata in the KMS, material in the HSM

The KMS catalog is a persistent state machine with the account's keys,
their versions (each a backing key's HSM object id), aliases, the key
policy, grants, tags and pending jobs (rotation, deletion). The HSM token
has only objects with an opaque label (`kms:<key-id>:<version>`).

Splitting the two keeps the virtual HSM's guarantees intact: material is
generated on the HSM's leader and only wrapped bytes enter its log
(virtual HSM design, "Generation on the leader"). It also means the KMS
can move to an external PKCS#11 module by configuration, which is why
custom key stores are out of scope: the backing store is already
swappable for the whole service, and per-key custom stores would add an
AWS API surface (`CreateCustomKeyStore`, `ConnectCustomKeyStore`) for a
choice operators already have.

Creating a key is two steps that must not leave orphans: the KMS commits
the key as `Creating` with a fresh id, asks the HSM for a backing key
labelled with it, then commits the object id and `Enabled`. A leader that
fails between the steps leaves `Creating`; the next leader looks the label
up on the HSM and either records the object or makes it. A label that
exists on the HSM with no key in the catalog is destroyed by the same
sweep after a grace period.

### Ciphertext blob

AWS's blob is undocumented and opaque, so Kythira defines its own:

```
u8 version (1) | u8 key-id length | key id (UUID text) | u32 backing version
| 12-byte nonce | ciphertext | 16-byte tag
```

The GCM additional data is the key ARN, the backing version, and the
encryption context canonicalized (keys sorted, UTF-8, length-prefixed).
Callers treat the blob as opaque bytes, as they do on AWS, so its format
is Kythira's to change; the version byte allows that. Blobs are larger
than AWS's for long key ids, which no caller depends on.

The KMS, not the HSM, builds the additional data: the HSM offers
AES-GCM with caller-supplied AAD (`CKM_AES_GCM` with `pAAD`), so binding
the context is one call.

### Use on any replica

A cryptographic operation reads the key's metadata (state, versions,
policy, grants) at a read-index point and calls the HSM, which itself
serves from any replica (virtual HSM design, "Use on any replica"). No
Raft write happens on the hot path. Throttling counters are per node,
scaled by the number of serving nodes, so the account quota holds within
a factor that the documentation states, as DynamoDB's throttling does.

### Authorization

IAM's evaluator gets the request facts: action `kms:<Operation>`, the key
ARN as resource, the key policy as a `key` resource policy, and the KMS
condition keys. Grants are evaluated by the KMS: a request no policy
allows is allowed when a live grant for the caller covers the operation
and its encryption context constraint holds. That matches AWS, where
grants are an alternative to policies, not a policy type.

In-process callers (co-located Kinesis and DynamoDB) present the end
caller's principal and set `kms:ViaService`; the KMS authorizes that
principal, never the consumer's node identity, so that an AWS policy that
says "this role may use the key only through DynamoDB" means the same
here. A consumer on another cluster forwards the caller's principal in a
signed envelope that the KMS accepts only from consumer principals it is
configured to trust (`KYTHIRA_KMS_TRUSTED_SERVICES`), which is Kythira's
equivalent of AWS's service principals.

### AWS managed and owned keys

`alias/aws/<service>` resolves, per account, to a key the KMS creates on
first use with a policy that allows the account's principals only through
that service. Owned keys are the same mechanism with no alias and no
listing in the account: they are created under a reserved Kythira account
id per consumer and named by the consumer's own principal; their policy
allows only that principal. Using a KMS key for "AWS owned" encryption
gives one code path for every table instead of a special unencrypted one.

### DynamoDB: seal what is stored, not what is evaluated

Conditions, filters and update expressions are evaluated in the item
state machine's apply on every replica, so the state machine needs
plaintext. Sealing is therefore at the storage boundary, inside the item
state machine:

- Each replica of a table's groups holds the table key unwrapped in memory
  (fetched from the KMS when it first applies a command for the table,
  cached until the table's key changes).
- Item values are sealed when written to the engine and opened when read.
  The nonce is derived (HKDF over the table key, the group id, the item
  key and the log index of the write), so every replica writes the same
  bytes and snapshots stay identical; a nonce is never reused because a
  log index is unique per write within a group. The nonce is stored with
  the sealed value, so a value moved by a split or merge, or restored from
  a backup, opens without knowing where it was written.
- Item keys (partition and sort key bytes) stay in the clear because the
  engine orders by them; GSI and LSI keys likewise. The documentation says
  so, and a table that needs its keys hidden must use opaque key values.
- Change log records, backups, PITR archives and exports are sealed with
  per-purpose keys derived from the table key, with a random nonce, since
  they are written once by one node.

`INACCESSIBLE_ENCRYPTION_CREDENTIALS`: a replica that cannot unwrap the
table key refuses data commands for the table. The catalog leader, which
already watches tables, marks the table inaccessible when unwraps fail
for longer than a threshold and back when they succeed.

Upgrade: a format bump marks sealed values; old values without the marker
read as plaintext. `UpdateTable` with `SSESpecification` on an existing
table starts a background job that rewrites each item sealed, through the
log, like the existing bulk jobs, and the table shows `UPDATING` in
`SSEDescription.Status` until it ends.

### One key provider interface for both consumers

Kinesis's `kms_key_provider` (with `local` and `aws` implementations)
moves out of the Kinesis module into a small shared library, and DynamoDB
uses it too, so both services choose among the same three backends:
Kythira's KMS, real AWS KMS, or a local sealed master key. The sealing
code in DynamoDB sees only "make a data key, unwrap a data key, re-wrap a
data key"; which KMS answers is configuration. That means DynamoDB
encryption with AWS KMS does not wait for Kythira's KMS to exist: the
`aws` and `local` providers can ship with the sealing work, and `kythira`
joins when the KMS module does. Each wrapped key records its provider, as
Kinesis's generations will, so switching backends never strands data.

### Kinesis: the KMS as its native key backend

Kinesis already uses KMS: per-stream data keys, wrapped by a
`kms_key_provider` (AWS KMS or a local sealed key today), with generations
recorded in the catalog. The `kythira` provider makes Kythira's KMS the
backend, and the default wherever the KMS runs in the same deployment.

- `StartStreamEncryption` resolves the `KeyId` (key id, ARN or alias)
  through the KMS and checks the key is enabled and symmetric before it
  answers, so a bad key fails the call rather than the next `PutRecord`.
  The stream records the key ARN, which `DescribeStream` reports.
- Producers' data keys come from `GenerateDataKey` and consumers' unwraps
  from `Decrypt`, each made with the caller's principal and
  `kms:ViaService`, so IAM evaluates the key policy, grants and identity
  policies for the end caller exactly as AWS does for Kinesis.
- Unwrapped generations are cached per stream for a bounded time (the same
  bound as DynamoDB's table key cache), so the hot path makes no KMS call
  per record while a disabled key still takes effect within that bound.
- Each wrapped generation gains a provider tag so a stream written under
  `local` or `aws` keeps reading after the switch.

### What stays out, and why

- **Custom key stores and XKS**: the HSM's external module support swaps
  the store for the whole KMS (see above).
- **Imported key material** (`GetParametersForImport`,
  `ImportKeyMaterial`): needs the HSM to unwrap with RSA-OAEP into a
  non-extractable object, which virtual HSM Requirement 6 allows; left for
  a follow-up spec to keep this one bounded.
- **Multi-Region keys**: need key material replicated across regions'
  HSMs, which the virtual HSM does not do. DynamoDB global tables use a
  key per region, as AWS allows.
- **FIPS endpoints**: the HSM is not validated (virtual HSM non-goal).

## Components

| Component | Where | Role |
|---|---|---|
| `kms_model.hpp` | `services/kms/include/kythira/kms/model/` | generated from the pinned botocore `kms` model, as DynamoDB's and Kinesis's are |
| `kms_catalog.hpp` | module | the replicated state machine and command format |
| `kms_ciphertext.hpp` | module | blob encoding, AAD canonicalization |
| `kms_actions.hpp` | module | the `kms` action table and condition keys for IAM |
| `kms_authorizer.hpp` | module | IAM call plus grants |
| `kms_api.hpp` | module | operation handlers over the catalog and `hsm.client` |
| `kms_client.hpp` | module, published | the consumer interface, in-process and HTTPS |
| `kms_jobs.hpp` | module | rotation, deletion, orphan sweep, managed key creation |
| `kms_node` | `cmd/kms_node/` | stand-alone node: KMS, HSM, IAM replica |
| Kinesis `kythira` provider | `services/kinesis/` | `kms_key_provider` over `kms_client` |
| DynamoDB sealing | `services/dynamodb/` | table keys, item and archive sealing, `SSEDescription` |

## Error handling

AWS's KMS errors throughout: `NotFoundException`, `DisabledException`,
`KMSInvalidStateException`, `InvalidCiphertextException`,
`IncorrectKeyException`, `InvalidKeyUsageException`,
`KMSInternalException` (HSM sealed or unreachable),
`DependencyTimeoutException` (HSM call timed out), `LimitExceededException`,
`ThrottlingException`, `AccessDeniedException`,
`MalformedPolicyDocumentException`, `UnsupportedOperationException`.
Consumers map these to their own APIs' KMS errors.

## Testing strategy

- Unit and property tests for the catalog (replicas agree, snapshots
  round trip), ciphertext (tamper detection), grants and authorization.
- The IAM policy corpus extended with KMS cases.
- CLI, boto3 and AWS Encryption SDK suites in CI against `kms_node`.
- Recorded differential fixtures from real AWS KMS, replayed in CI;
  re-recording needs the maintainer's approval of the spend.
- Kinesis and DynamoDB integration in both layouts.
- docker-chaos with HSM and KMS faults under encrypted writes, on Docker
  and rootless Podman.
- A plaintext scan: every key and data key a test made, searched for in
  the KMS, HSM, DynamoDB and Kinesis data directories after the run.

## Open questions

- Whether DynamoDB's owned key should be one per account or one per
  table. Per account matches AWS's documentation; per table limits what
  one leaked table key exposes. The design takes per account with
  per-table derived keys, and the question is left for review.
- How long the table key cache on a replica should outlive a key disable
  before the replica stops serving. AWS documents minutes; the design
  starts with five minutes, configurable.
