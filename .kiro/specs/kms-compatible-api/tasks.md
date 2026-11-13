# Implementation Plan

Depends on `.kiro/specs/virtual-hsm/` tasks 1-6 (the HSM module, its
token store and native client) and `.kiro/specs/service-colocation/`
tasks 1-3 and 8 (the service module interface, `kythira_node`, and IAM on
the multi-raft host). Tasks 1-4 need neither and can start now. Each task
is one PR unless noted.

- [ ] 1. Module skeleton and model
  - `KYTHIRA_KMS` Kconfig option (default off, depends on `KYTHIRA_HSM`
    and `KYTHIRA_IAM`), `services/kms/` layout, boundary check coverage
  - Pinned botocore `kms` model, generated operation and shape tables, a
    coverage check listing every operation as served or refused, as the
    DynamoDB and Kinesis modules do
  - Off-by-default build proved unchanged
  - _Requirements: 1.1-1.4, 3.1_

- [ ] 2. `kms` action table and IAM condition keys
  - `kms_actions.hpp` with every action, the `key` and `alias` resource
    ARN formats and the condition keys of 7.4, generated from AWS's
    service authorization reference and pinned
  - IAM policy corpus cases for KMS: default key policy delegation, key
    policy without delegation, cross-account, `kms:ViaService`,
    `kms:EncryptionContext:*`, `kms:CallerAccount`
  - _Requirements: 7.3, 7.4, 7.6_

- [ ] 3. Ciphertext format
  - `kms_ciphertext.hpp`: blob encoding, AAD canonicalization of the key
    ARN, backing version and encryption context; tamper, wrong context
    and wrong key tests; a fuzz target for the decoder
  - _Requirements: 5.2, 5.3, 14.1_

- [ ] 4. KMS catalog state machine
  - Keys, versions, aliases, policies, grants, tags, jobs; command format
    with a version byte; `Creating` and `PendingDeletion` states and
    tombstones; per-account key and alias limits
  - Property tests: replicas and snapshot restores agree; no key material
    can be represented in a command
  - _Requirements: 3.3-3.5, 6.1, 6.2, 8.1, 9.4, 13.4_

- [ ] 5. KMS service module and HSM binding
  - The module on the multi-raft host (module 7), `hsm.client` use for
    backing keys on the `kms` token, two-step key creation with the orphan
    sweep, the sealed-HSM error path
  - Plaintext scan test over the KMS group's log and snapshots
  - _Requirements: 2.1, 4.1-4.5_

- [ ] 6. Key management operations
  - `CreateKey`, `DescribeKey`, `ListKeys`, `EnableKey`, `DisableKey`,
    `UpdateKeyDescription`, `ScheduleKeyDeletion`, `CancelKeyDeletion`,
    aliases and tags, alias resolution everywhere AWS takes one
  - _Requirements: 3.1-3.6, 6.1-6.3_

- [ ] 7. Symmetric cryptographic operations
  - `Encrypt`, `Decrypt`, `ReEncrypt`, `GenerateDataKey`,
    `GenerateDataKeyWithoutPlaintext`, `GenerateRandom`; read-index
    serving on any replica
  - _Requirements: 5.1-5.3, 5.6_

- [ ] 8. Asymmetric and HMAC operations
  - RSA and ECC keys: `Encrypt`/`Decrypt` with RSA-OAEP, `Sign`/`Verify`
    (RAW and DIGEST), `GetPublicKey`, `GenerateDataKeyPair` and its
    without-plaintext form; HMAC keys: `GenerateMac`, `VerifyMac`
  - OpenSSL cross-checks of every signature and public key
  - _Requirements: 3.2, 5.1, 5.4, 5.5_

- [ ] 9. Key policies and authorization
  - `GetKeyPolicy`, `PutKeyPolicy`, `ListKeyPolicies`, default key policy,
    lockout safety check; every handler authorized through IAM with the
    key policy; static-roles mode with IAM off
  - _Requirements: 7.1-7.3, 7.5, 7.7_

- [ ] 10. Grants
  - `CreateGrant`, `ListGrants`, `ListRetirableGrants`, `RetireGrant`,
    `RevokeGrant`; grant evaluation with encryption context constraints;
    grant tokens
  - _Requirements: 8.1-8.4_

- [ ] 11. Rotation and deletion jobs
  - Scheduled and on-demand rotation, `ListKeyRotations`, deletion at the
    end of the window, idempotent across leader changes
  - _Requirements: 9.1-9.4_

- [ ] 12. `kms_node`, the published client and co-location
  - `kms_node` (KMS, HSM, IAM replica), `KYTHIRA_KMS_*` settings, the API
    listener; `kms.client` in-process and over HTTPS with forwarded
    principals and `KYTHIRA_KMS_TRUSTED_SERVICES`; `kms` in
    `KYTHIRA_SERVICES`
  - _Requirements: 2.2-2.5_

- [ ] 13. AWS managed and owned keys
  - First-use creation of `alias/aws/<service>` per account with the
    `kms:ViaService` policy, yearly rotation, customer changes refused;
    owned keys per consumer and account
  - _Requirements: 10.1-10.3_

- [ ] 14. Audit, metrics and quotas
  - CloudTrail-shaped audit records through IAM's audit sink, Prometheus
    metrics, per-account request quotas with operator overrides
  - _Requirements: 13.1-13.3_

- [ ] 15. Kinesis integration
  - `kythira` `kms_key_provider` over `kms.client` with the caller's
    principal, the default when the KMS runs in the same deployment;
    provider tag on wrapped generations; `alias/aws/kinesis`
  - `StartStreamEncryption` and `StopStreamEncryption` resolving every
    `KeyId` form through the KMS; `kms:GenerateDataKey` for producers and
    `kms:Decrypt` for consumers authorized through IAM; disabled and
    rotated keys; error mapping
  - Integration tests co-located and stand-alone, including an IAM case
    where the key policy denies a role that the stream policy allows
  - _Requirements: 11.1-11.8, 14.4_

- [ ] 16. DynamoDB table keys and `SSESpecification` (two PRs)
  - `SSESpecification` on `CreateTable`, `UpdateTable` and restores;
    `SSEDescription`; table key made and re-wrapped through `kms.client`;
    owned key default with `KYTHIRA_DYNAMODB_ENCRYPT_AT_REST`; catalog
    format bump
  - Item value sealing in the item state machine with derived, stored
    nonces; `INACCESSIBLE_ENCRYPTION_CREDENTIALS`; background
    re-encryption of existing tables; item format bump
  - The shared `kms_key_provider` library (moved from Kinesis) with the
    `kythira`, `aws` and `local` providers and `KYTHIRA_DYNAMODB_KMS`;
    provider tag on table keys
  - _Requirements: 12.1-12.7, 12.10-12.12_

- [ ] 17. DynamoDB archives and replicas
  - Sealing of change log records, backups, PITR archives and exports;
    `S3SseKmsKeyId` for Kythira stores and passed through for AWS buckets;
    `KMSMasterKeyId` per replica in `ReplicaUpdates`
  - Remove the matching lines from `services/dynamodb/doc/unsupported.md`
  - _Requirements: 12.3, 12.8, 12.9_

- [ ] 18. Client suites and differential fixtures
  - AWS CLI v2, boto3 and AWS Encryption SDK suites in CI against
    `kms_node`
  - Differential fixtures from real AWS KMS for request validation and
    errors (needs the maintainer's approval of the spend before
    recording; CI replays the fixtures)
  - _Requirements: 14.2, 14.3_

- [ ] 19. docker-chaos and plaintext scan
  - A scenario killing and partitioning KMS and HSM nodes under encrypted
    DynamoDB and Kinesis writes, on Docker and rootless Podman; no lost
    acknowledged writes; the plaintext scan over every data directory
  - _Requirements: 14.5_

- [ ] 20. Documentation
  - `doc/kms.md` (deployment in both layouts, the AWS CLI walkthrough,
    authorization, the honest limits), module README, updates to
    `doc/dynamodb.md` (what is visible on disk) and the Kinesis encryption
    doc
  - _Requirements: 1.2, 7.7, 12.3_
