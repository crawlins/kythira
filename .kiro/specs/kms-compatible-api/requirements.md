# Requirements Document

## Introduction

Kythira has two halves of a key management service and no service. The
virtual HSM (`.kiro/specs/virtual-hsm/`) stores keys and performs
cryptographic operations behind PKCS#11, but it has no notion of an AWS
account, a key policy, an alias or a grant, and it left a KMS API out of
scope on purpose. IAM (`.kiro/specs/iam-compatible-auth/`) already
evaluates KMS key policies (`resource_policy_kind::key`: a key policy must
itself allow the principal, as on AWS), but nothing calls it with one.
Meanwhile the services that take KMS parameters in their AWS APIs either
reach out to AWS KMS or refuse them:

| Service | KMS in its AWS API | Kythira today |
|---|---|---|
| Kinesis | `StartStreamEncryption` `KeyId`, `EncryptionType=KMS`, `KeyId` in `DescribeStream` | Works, through a `kms_key_provider` that is AWS KMS or a local passphrase-sealed provider (`kinesis_kms.hpp`, `kinesis_kms_aws.hpp`) |
| DynamoDB | `SSESpecification` (`Enabled`, `SSEType=KMS`, `KMSMasterKeyId`) on `CreateTable`, `UpdateTable` and `RestoreTable*`; `SSEDescription` in every table description; `KMSMasterKeyId` per replica in `ReplicaUpdates`; `S3SseKmsKeyId` on exports and imports | `SSESpecification` is refused as unsupported (`table_api.hpp`); replica `KMSMasterKeyId` is refused; `S3SseKmsKeyId` is recorded and reported, not used. Items are stored in the clear |
| IAM | `kms:*` actions in policies; key policies as resource policies | The evaluator handles key policies, but no `kms` action table or KMS condition keys exist |
| Ledger, Redis gateway | None: the ledger's API is native, and the Redis gateway speaks RESP, not ElastiCache's control plane | Nothing to integrate |
| Object persistence and backups to S3 | SSE-KMS buckets on AWS | Uses AWS's own KMS through S3; out of scope |

This spec adds a **KMS-compatible service**: AWS KMS's JSON 1.1 API
(`TrentService`), so the AWS CLI, the SDKs and the AWS Encryption SDK work
against it unchanged, with key material kept in the virtual HSM. It runs
as a service module, co-located with the services that use it or
stand-alone (`.kiro/specs/service-colocation/`). It then wires Kinesis and
DynamoDB to it, so that a stream or table key named in their APIs is a
Kythira KMS key, authorized by IAM as AWS authorizes it.

Like the HSM it sits on, it is honest about what software can promise: a
KMS key's material is non-extractable through the API and never written in
plaintext, but it is in host memory while in use (virtual HSM Requirement
12), and a service's data keys are in that service's memory while it
serves the data.

**Scope**: the KMS API for symmetric, asymmetric and HMAC keys, aliases,
key policies, grants, tags, rotation and deletion; IAM authorization and
the KMS condition keys; AWS managed keys for Kythira services; the
Kinesis and DynamoDB integrations; the stand-alone daemon and the
co-located module; tests against the AWS CLI, boto3 and the AWS
Encryption SDK. **Out of scope** for this spec, with the reason given in
the design: custom key stores (the virtual HSM's external-module support
already swaps the backing store), imported key material, multi-Region
keys, XKS, and FIPS endpoints.

## Glossary

- **KMS key**: AWS's term for a customer master key: an id, an ARN, an
  owning account, a key spec, a usage, a state and one or more versions of
  key material.
- **Backing key**: one version of a KMS key's material, a non-extractable
  object on the virtual HSM's KMS token. Rotation adds a backing key; old
  ones stay for decryption.
- **Ciphertext blob**: what `Encrypt` and `GenerateDataKey` return. Its
  format is Kythira's (AWS's is opaque and undocumented); it names the key
  and backing key, so `Decrypt` needs no `KeyId` for symmetric keys.
- **Encryption context**: a map of non-secret strings bound to a ciphertext
  as additional authenticated data; `Decrypt` must present the same map.
- **Key policy**: a key's resource policy. With IAM, a request is allowed
  only if the key policy allows it, alone or by delegating to the account
  (AWS's rule, already in `iam_evaluator.hpp`).
- **Grant**: a delegation on one key to a grantee principal for listed
  operations, optionally constrained by encryption context.
- **AWS managed key**: a key a service creates in an account on first use,
  aliased `alias/aws/<service>`, usable only through that service
  (`kms:ViaService`).
- **Owned key**: a key a service holds for an account's resources without
  showing it in that account (AWS's "AWS owned key"), used when a table
  asks for no KMS key.
- **Consumer**: a Kythira service that encrypts its data under KMS keys:
  Kinesis and DynamoDB in this spec.

## Requirements

### Requirement 1: Opt-in module

**User Story:** As a maintainer, I want the KMS built only when asked for,
and kept separable like every other service module.

#### Acceptance Criteria

1. The KMS SHALL be built only with a new Kconfig option `KYTHIRA_KMS`
   (default off), which SHALL depend on `KYTHIRA_HSM` and `KYTHIRA_IAM`.
2. Its sources SHALL follow `services/README.md`: `services/kms/` with
   `include/kythira/kms/`, `cmd/kms_node/`, `tests/` and `docker/`.
3. KMS code SHALL include only Kythira's public headers and the HSM's and
   IAM's published client interfaces; the module boundary check SHALL
   cover it.
4. With the option off, no existing target, test or binary SHALL change,
   and Kinesis and DynamoDB SHALL behave as they do today.

### Requirement 2: Co-located or stand-alone

**User Story:** As an operator, I want to run the KMS inside the nodes that
already run DynamoDB and Kinesis, or on its own cluster, without changing
how clients or services reach it.

#### Acceptance Criteria

1. The KMS SHALL be a service module (service-colocation Requirement 1)
   with module number 7, its own Raft group range and key prefix.
2. `kms_node` SHALL be a node with the KMS module, the HSM module and an
   IAM replica, configured by `KYTHIRA_KMS_*`; in `kythira_node`, adding
   `kms` to `KYTHIRA_SERVICES` SHALL do the same alongside other modules.
3. A co-located consumer SHALL reach the KMS through the in-process client
   the module publishes (`kms.client`), authorized exactly as a network
   call from that consumer's principal would be (service-colocation
   Requirement 6), with `kms:ViaService` set to the consumer.
4. A consumer on another cluster SHALL reach a stand-alone KMS over HTTPS
   with SigV4, through the same client interface, configured by an
   endpoint and credentials.
5. The KMS SHALL serve its API on its own listener (default port 8443
   with TLS) in either layout, so the AWS CLI's `--endpoint-url` works the
   same way for both.

### Requirement 3: Keys and their metadata

**User Story:** As a developer, I want to create and manage KMS keys with
the AWS CLI and SDKs, and see the metadata AWS returns.

#### Acceptance Criteria

1. The KMS SHALL serve `CreateKey`, `DescribeKey`, `ListKeys`,
   `EnableKey`, `DisableKey`, `UpdateKeyDescription`,
   `ScheduleKeyDeletion` and `CancelKeyDeletion` with AWS's request and
   response shapes, errors and pagination, from the pinned botocore model.
2. Keys SHALL support the key specs `SYMMETRIC_DEFAULT`, `RSA_2048`,
   `RSA_3072`, `RSA_4096`, `ECC_NIST_P256`, `ECC_NIST_P384`,
   `ECC_NIST_P521`, `ECC_SECG_P256K1`, `HMAC_224`, `HMAC_256`, `HMAC_384`
   and `HMAC_512`, with the key usages AWS allows for each. Any other key
   spec SHALL be refused with AWS's error.
3. A key SHALL belong to the calling account; its ARN SHALL be
   `arn:<partition>:kms:<region>:<account>:key/<key-id>`, where the key id
   is a UUID.
4. Key states SHALL be AWS's (`Enabled`, `Disabled`, `PendingDeletion`,
   and `Creating` while a key's material is made) with AWS's transitions.
   A key pending deletion SHALL be usable by no cryptographic operation
   and SHALL be destroyed, material included, when its waiting period
   (7 to 30 days, default 30) ends, unless the deletion is cancelled.
5. Key metadata (id, account, description, spec, usage, state, dates,
   rotation settings, aliases, policy, grants, tags) SHALL live in the KMS
   state machine, replicated through Raft. Key material SHALL live only
   as virtual HSM objects (Requirement 4).
6. `DescribeKey` SHALL accept a key id, a key ARN, an alias name or an
   alias ARN, as AWS does.

### Requirement 4: Key material in the virtual HSM

**User Story:** As a security engineer, I want KMS key material held where
the rest of Kythira's keys are, never in the KMS's own state or log.

#### Acceptance Criteria

1. Each backing key SHALL be a non-extractable, sensitive token object on
   a dedicated virtual HSM token, generated on the HSM (virtual HSM
   Requirement 3) and referred to by the KMS only by its object id.
2. The KMS SHALL use only PKCS#11 mechanisms the virtual HSM offers
   (virtual HSM Requirement 6): AES-256-GCM for symmetric keys,
   RSA-OAEP and RSA-PSS or PKCS#1 v1.5 for RSA keys, ECDSA for ECC keys,
   and HMAC for HMAC keys.
3. The KMS SHALL be able to run against an external PKCS#11 module in
   place of the virtual HSM (virtual HSM Requirement 9), with any
   operation that module lacks refused with `UnsupportedOperationException`
   and named in the documentation for that module.
4. Plaintext key material SHALL never appear in the KMS state machine, the
   Raft log, a snapshot, a log line or an error message. A test SHALL
   scan the KMS group's log and snapshots for every generated key's bytes.
5. When the HSM is sealed or unreachable, cryptographic operations SHALL
   answer `KMSInternalException` (AWS's error for an unavailable key
   store) and management operations that need no material SHALL still
   work.

### Requirement 5: Cryptographic operations

**User Story:** As a developer, I want to encrypt, decrypt, sign and make
data keys with Kythira KMS exactly as I do with AWS KMS.

#### Acceptance Criteria

1. The KMS SHALL serve `Encrypt`, `Decrypt`, `ReEncrypt`,
   `GenerateDataKey`, `GenerateDataKeyWithoutPlaintext`,
   `GenerateDataKeyPair`, `GenerateDataKeyPairWithoutPlaintext`,
   `GenerateRandom`, `Sign`, `Verify`, `GetPublicKey`, `GenerateMac` and
   `VerifyMac`, with AWS's limits (4096-byte plaintext for `Encrypt`,
   1 to 1024 bytes for `GenerateRandom`, data key sizes 1 to 1024 bytes or
   `KeySpec` `AES_128`/`AES_256`) and errors.
2. Symmetric ciphertext SHALL bind the encryption context, the key ARN and
   the backing key version as additional authenticated data. `Decrypt`
   SHALL fail with `InvalidCiphertextException` on any change to the
   ciphertext, the context or the key.
3. `Decrypt` of a symmetric ciphertext SHALL work without `KeyId`, and
   SHALL refuse with `IncorrectKeyException` when a `KeyId` is given and
   names another key.
4. Asymmetric and HMAC operations SHALL accept the algorithms AWS accepts
   for each key spec (`EncryptionAlgorithm`, `SigningAlgorithm`,
   `MacAlgorithm`) and refuse others with AWS's errors.
5. `Sign` and `Verify` SHALL take `MessageType=RAW` or `DIGEST` as AWS
   does; signatures SHALL verify with OpenSSL against `GetPublicKey`.
6. Every cryptographic operation SHALL run on any replica that has the
   key's metadata at or after a read-index check, without a Raft write, so
   that throughput scales with nodes (virtual HSM design, "Use on any
   replica").

### Requirement 6: Aliases, tags and descriptions

**User Story:** As a developer, I want friendly names and tags on keys, as
on AWS.

#### Acceptance Criteria

1. The KMS SHALL serve `CreateAlias`, `UpdateAlias`, `DeleteAlias` and
   `ListAliases`, with AWS's alias name rules: `alias/` prefix,
   characters, length, one key per alias, and no customer alias under
   `alias/aws/`.
2. The KMS SHALL serve `TagResource`, `UntagResource` and
   `ListResourceTags` with AWS's tag limits, and SHALL take `Tags` on
   `CreateKey`.
3. Every operation that takes a key SHALL accept an alias where AWS does,
   and refuse it where AWS does (for example `CreateGrant` takes no
   alias).

### Requirement 7: Key policies and IAM authorization

**User Story:** As a security engineer, I want each key's access decided by
its key policy and IAM, the way AWS decides it, so that policies written
for AWS mean the same here.

#### Acceptance Criteria

1. The KMS SHALL serve `GetKeyPolicy`, `PutKeyPolicy` and
   `ListKeyPolicies`, with the policy name `default` only and AWS's
   32 KB limit, and SHALL take `Policy` and
   `BypassPolicyLockoutSafetyCheck` on `CreateKey` and `PutKeyPolicy`.
2. A key created without a policy SHALL get AWS's default key policy,
   which allows the account's root principal everything and so delegates
   to the account's IAM policies.
3. Every request SHALL be authorized through IAM's evaluator with the
   key's policy as a `key` resource policy, so that an identity policy
   alone does not suffice unless the key policy delegates to the account.
   Cross-account use SHALL need both sides, as on AWS.
4. The KMS SHALL register a `kms` action table with every KMS action, the
   resource types `key` and `alias`, and the KMS condition keys:
   `kms:CallerAccount`, `kms:EncryptionContext:${key}`,
   `kms:EncryptionContextKeys`, `kms:EncryptionAlgorithm`,
   `kms:SigningAlgorithm`, `kms:MacAlgorithm`, `kms:GrantOperations`,
   `kms:GrantIsForAWSResource`, `kms:GranteePrincipal`,
   `kms:RetiringPrincipal`, `kms:KeySpec`, `kms:KeyUsage`,
   `kms:KeyOrigin`, `kms:ViaService`, `kms:RequestAlias`,
   `kms:ResourceAliases`, `kms:DataKeyPairSpec`,
   `kms:MessageType`, `kms:BypassPolicyLockoutSafetyCheck` and
   `kms:ScheduleKeyDeletionPendingWindowInDays`.
5. `PutKeyPolicy` SHALL apply AWS's lockout safety check: a policy that
   would stop the caller from changing it again SHALL be refused unless
   the caller bypasses the check.
6. The policy corpus (`services/iam/tests/corpus/`) SHALL gain KMS cases:
   default policy delegation, key policy without delegation, cross-account,
   `kms:ViaService`, encryption context conditions and grants (Requirement
   8), each with the decision AWS's documentation gives.
7. With IAM off (static keys), the KMS SHALL authorize with the static
   roles (read-only: describe and list; read-write: cryptographic
   operations; admin: everything) and SHALL ignore key policies and
   grants, and its documentation SHALL say so.

### Requirement 8: Grants

**User Story:** As a service, I want grants on a key so that I can use it
for a customer's resource without the customer editing the key policy.

#### Acceptance Criteria

1. The KMS SHALL serve `CreateGrant`, `ListGrants`, `ListRetirableGrants`,
   `RetireGrant` and `RevokeGrant`, with AWS's limits (50,000 grants per
   key, grant operations AWS allows for each key usage).
2. A grant SHALL allow its grantee the listed operations on its key, alone
   or within `EncryptionContextEquals` or `EncryptionContextSubset`
   constraints, in addition to the key policy and IAM.
3. A grant SHALL take effect for calls that present its grant token at
   once, and for every call once the grant has replicated, as AWS's
   eventual consistency allows; Kythira's replication makes "once
   committed" the bound.
4. Grants made by AWS managed keys' services SHALL be visible to the
   account with `kms:GrantIsForAWSResource` true, as AWS shows them.

### Requirement 9: Rotation and deletion

**User Story:** As a security engineer, I want symmetric keys rotated on a
schedule or on demand, without breaking old ciphertexts.

#### Acceptance Criteria

1. The KMS SHALL serve `EnableKeyRotation` (with
   `RotationPeriodInDays` 90 to 2560, default 365),
   `DisableKeyRotation`, `GetKeyRotationStatus`, `RotateKeyOnDemand` and
   `ListKeyRotations` for symmetric keys, refusing them for other specs
   as AWS does.
2. Rotation SHALL add a backing key; new ciphertexts SHALL use it and old
   ones SHALL decrypt with the backing key they name.
3. Scheduled rotation SHALL run on the KMS group's leader and SHALL be
   idempotent across leader changes.
4. Destroying a key at the end of its deletion window SHALL destroy every
   backing key on the HSM and keep the metadata as a tombstone, so that a
   later `DescribeKey` answers `NotFoundException` and the id is never
   reused.

### Requirement 10: AWS managed and owned keys for Kythira services

**User Story:** As a developer, I want `alias/aws/kinesis` and
`alias/aws/dynamodb` to exist when a service uses them, as on AWS.

#### Acceptance Criteria

1. The first time a consumer uses its AWS managed key in an account, the
   KMS SHALL create it with alias `alias/aws/<service>`, a key policy that
   allows use only with `kms:ViaService` set to that service and
   `kms:CallerAccount` set to the account, and `KeyManager` `AWS` in
   `DescribeKey`.
2. Customers SHALL NOT be able to change, delete, rotate on demand or
   schedule deletion of an AWS managed key; AWS managed keys SHALL rotate
   every year.
3. A consumer MAY hold an owned key per account for resources that name
   no KMS key; owned keys SHALL be listed in no account and authorized
   only for the consumer's own principal.

### Requirement 11: Kinesis integration

**User Story:** As a Kinesis user, I want server-side stream encryption
with a Kythira KMS key to work exactly as it does with AWS KMS: the same
API calls, the same key identifiers, and the same IAM permissions.

Kinesis is a KMS consumer today (against AWS KMS or a local sealed key);
this requirement makes Kythira's KMS its native backend.

#### Acceptance Criteria

1. Kinesis SHALL gain a `kythira` `kms_key_provider`
   (`KINESIS_KMS=kythira`) over the KMS client: `GenerateDataKey` to make
   a stream's data key, `Decrypt` to unwrap a generation, with the stream's
   encryption context (`kythira:kinesis:stream`) and `kms:ViaService`
   `kinesis.<region>.amazonaws.com`. When the KMS module runs in the same
   deployment, `kythira` SHALL be the default provider.
2. `StartStreamEncryption` with `EncryptionType=KMS` SHALL accept every
   `KeyId` form AWS accepts (key id, key ARN, alias name, alias ARN),
   resolve it through the KMS before answering, and refuse a key that
   does not exist, is disabled or is not a symmetric encryption key with
   the matching Kinesis KMS error. `DescribeStream` and
   `DescribeStreamSummary` SHALL report `EncryptionType` and `KeyId` as
   AWS does.
3. `StopStreamEncryption` SHALL require the same `KeyId` the stream uses
   and stop wrapping new records; records written while encryption was on
   SHALL stay readable.
4. Producers SHALL be authorized for `kms:GenerateDataKey` and consumers
   (`GetRecords`, `SubscribeToShard`) for `kms:Decrypt` on the stream's
   key, through IAM with the key policy and grants, as AWS requires. The
   gateway SHALL forward the caller's principal with each KMS call, never
   its own, so that a key policy that denies a role denies it through
   Kinesis too.
5. `alias/aws/kinesis` SHALL resolve to the account's AWS managed key
   (Requirement 10).
6. KMS errors SHALL map to Kinesis's `KMSAccessDeniedException`,
   `KMSDisabledException`, `KMSInvalidStateException`,
   `KMSNotFoundException`, `KMSOptInRequired` and `KMSThrottlingException`
   as `kinesis_kms.hpp` maps AWS KMS's today.
7. The existing `local` and `aws` providers SHALL stay, and a stream's
   wrapped generations SHALL record which provider wrapped them so that a
   cluster can move to `kythira` without re-keying existing records.
8. Rotating or disabling the stream's KMS key SHALL behave as on AWS:
   rotation is transparent to producers and consumers, and a disabled key
   makes new writes and reads fail with `KMSDisabledException` until it
   is enabled again.

### Requirement 12: DynamoDB integration

**User Story:** As a DynamoDB user, I want `SSESpecification` to work, with
an owned key by default and an AWS managed or customer managed key when I
ask, and the table's data encrypted at rest under it.

#### Acceptance Criteria

1. `CreateTable`, `UpdateTable` and `RestoreTableFromBackup` /
   `RestoreTableToPointInTime` SHALL accept `SSESpecification` with
   `Enabled`, `SSEType=KMS` and `KMSMasterKeyId` (key id, ARN, alias name
   or alias ARN), as AWS does, and every table description SHALL carry
   `SSEDescription` (`Status`, `SSEType`, `KMSMasterKeyArn`,
   `InaccessibleEncryptionDateTime`) where AWS shows it.
2. Each encrypted table SHALL have a 256-bit table key made by
   `GenerateDataKey` under its KMS key with encryption context
   `{"aws:dynamodb:tableName": <name>, "aws:dynamodb:subscriberId":
   <account>}`, stored only wrapped in the table descriptor, and unwrapped
   by each replica of the table's groups into memory.
3. Item values, index entries, change log records, backups, PITR archives
   and exports in Kythira's own formats SHALL be sealed with AES-256-GCM
   under the table key (or a key derived from it per purpose) before they
   reach the storage engine, the segment store or the backup store; keys
   that order items SHALL stay in the clear, as they must sort, and the
   documentation SHALL say which parts of an item are visible on disk.
4. Sealing SHALL keep replicas identical: it happens before a command is
   proposed or deterministically in apply from replicated inputs, never
   with a per-replica nonce applied after the fact.
5. Switching a table's key with `UpdateTable` SHALL re-wrap the table key
   under the new KMS key (`ReEncrypt`), with no rewrite of stored data.
6. When the table's KMS key is disabled, pending deletion or unreachable,
   the table SHALL go `INACCESSIBLE_ENCRYPTION_CREDENTIALS`, answer data
   requests with AWS's errors, and recover when the key does, as AWS
   documents; Kythira SHALL NOT archive the table on its own.
7. A table that names no KMS key SHALL be encrypted under the DynamoDB
   owned key for its account when a key provider is configured and
   `KYTHIRA_DYNAMODB_ENCRYPT_AT_REST=owned` (the default when one is);
   with `off`, or with no provider, tables SHALL be stored as today and
   `SSEDescription` SHALL be absent.
8. Global table replicas SHALL take a `KMSMasterKeyId` per replica in
   `ReplicaUpdates` (now refused), each replica's table key wrapped by its
   own region's KMS.
9. Exports and imports SHALL use `S3SseKmsKeyId` for a Kythira bucket
   directory or store, and pass it to S3 unchanged for an AWS bucket.
10. Existing tables SHALL stay readable after an upgrade, unencrypted
    until a background re-encryption that `UpdateTable` with
    `SSESpecification` starts, and the catalog and item formats SHALL be
    bumped so that a node that does not know them refuses them rather than
    misreading them.
11. DynamoDB SHALL have the same choice of key provider that Kinesis has,
    set by `KYTHIRA_DYNAMODB_KMS=kythira|aws|local`, through one
    `kms_key_provider` interface that both services share (moved out of
    the Kinesis module into a library both link):
    - `kythira`: Kythira's KMS, as in criteria 1-10; the default when the
      KMS runs in the same deployment.
    - `aws`: real AWS KMS, called with SigV4 and the operator's
      credentials (`GenerateDataKey`, `Decrypt`, `ReEncrypt`), so a table
      can be encrypted under a customer managed key in an AWS account.
      `KMSMasterKeyId` SHALL name an AWS key, and the owned key SHALL be
      an AWS key the operator configures, since AWS owned keys cannot be
      used outside AWS. Authorization is AWS's, under the operator's
      credentials; the documentation SHALL say that Kythira's IAM still
      decides who may use the table.
    - `local`: a passphrase-sealed master key on the node, as Kinesis's
      `local` provider is, for deployments with neither KMS. It SHALL
      accept only the owned key and refuse a `KMSMasterKeyId` with
      `ValidationException`.
12. Table keys SHALL record which provider wrapped them, so that a table
    keeps opening after the cluster's provider changes and `UpdateTable`
    can re-wrap it under the new one.

### Requirement 13: Audit, metrics and limits

**User Story:** As an operator, I want to know who used which key, and to
keep one account from exhausting the KMS.

#### Acceptance Criteria

1. Every KMS request SHALL produce an audit record with the account,
   principal, operation, key ARN, encryption context keys (not values),
   grant id if any, `kms:ViaService`, source address, outcome and request
   id, in the shape CloudTrail records KMS events, written through the
   audit sink IAM uses.
2. The KMS SHALL expose Prometheus metrics per operation and outcome,
   labelled with the account for the operator, and SHALL never put key
   material or encryption context values in a metric or log.
3. The KMS SHALL apply AWS's per-account request quotas (shared
   cryptographic operations per second, per key spec), answering
   `ThrottlingException`, with per-account overrides by the operator as
   DynamoDB's account limits are set.
4. Per-account key and alias counts SHALL be limited as AWS limits them,
   with operator overrides.

### Requirement 14: Testing

**User Story:** As a maintainer, I want evidence that the KMS behaves like
AWS KMS and that consumers keep their data safe across failures.

#### Acceptance Criteria

1. Unit tests SHALL cover the state machine, the ciphertext format
   (tamper, wrong context, wrong key), the authorization rules and every
   operation's validation.
2. The AWS CLI v2, boto3 and the AWS Encryption SDK (Python) SHALL run
   against the KMS in CI: create, alias, encrypt and decrypt with
   context, data keys, envelope encryption through the Encryption SDK,
   sign and verify, rotate, grant, schedule and cancel deletion.
3. A differential run against real AWS KMS SHALL compare request
   validation and errors for every operation (not ciphertexts), recorded
   as fixtures so that CI replays them; it SHALL run only with the
   maintainer's approval of the spend.
4. Kinesis and DynamoDB SHALL each have integration tests with a
   co-located KMS and with a stand-alone one: encrypted writes and reads,
   key disable and re-enable, key switch, and the IAM denials AWS gives.
5. A docker-chaos scenario SHALL kill and partition KMS and HSM nodes
   while DynamoDB and Kinesis write to encrypted tables and streams, and
   check that no acknowledged write is lost and no plaintext reaches disk,
   on Docker and rootless Podman.
