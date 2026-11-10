# Encryption at rest

Task 14 of `.kiro/specs/kinesis-compatible-streams/` (Requirement 14.1).

## What gets encrypted

`StartStreamEncryption` turns on encryption for a stream. After that, every
new record is sealed with AES-256-GCM before the gateway proposes it. That
covers the partition key and the data. Everything downstream only ever sees
ciphertext:

- the Raft log;
- the segments on every replica;
- segment fetches between peers;
- tiered objects in S3.

Sealing happens once, on the node that takes the write. So the replicas
stay byte-identical, as the segment store requires.

A sealed record is stored like this:

    u16 0xFFFF | u32 generation | 12-byte nonce | ciphertext | 16-byte tag

How it works:

- **Telling sealed records from plain ones.** A plaintext record starts with
  its partition key's length, which is at most 256, so it can never start
  with `0xFFFF`. That means one shard can hold both kinds, before and after
  a start or stop.
- **What the tag protects.** The GCM additional data is the shard's routing
  key and the generation. A record moved to another shard, or relabelled
  with another generation, will not open.
- **Nonces.** Each record gets a random 96-bit nonce.
- **What the log checks.** The state machine refuses a sealed record that
  lacks the marker or is too short to be one.

## Data keys

Data keys are tracked per stream:

- **New keys.** Each `StartStreamEncryption` that changes the key makes a
  fresh 256-bit data key. The key provider wraps it under the caller's
  `KeyId`.
- **Generations.** The catalog keeps every wrapped data key with its
  generation (1, 2, ...). The generation is assigned when the entry
  applies, so two racing starts each get their own.
- **Plaintext keys.** A plaintext data key is never replicated. Each node
  unwraps a generation the first time it needs it, then keeps it in memory.
- **The encryption context.** Data keys are bound to
  `{"kythira:kinesis:stream": "<account>/<name>/<stream id>"}`.
- **`StopStreamEncryption`.** Stopping keeps every data key, so records
  sealed earlier stay readable. New records are stored in the clear again.
- **Re-keying.** Starting with another `KeyId` re-keys the stream. Records
  already written keep their old data key.

The catalog keeps every generation for the life of the stream. Each one
costs about 80 bytes (local provider) or about 200 bytes (AWS KMS). Nothing
prunes generations yet, even once retention has dropped every record under
one.

## The calls

- **`StartStreamEncryption`.** Needs `EncryptionType` `KMS` and a
  `KeyId`, on an ACTIVE stream.
  - If no key provider is configured, it answers
    `InvalidArgumentException`.
  - Starting with the key the stream already uses changes nothing.
  - The change is one catalog entry, so the stream stays ACTIVE throughout
    (AWS shows UPDATING for a few seconds). A write racing the change may
    land on either side of it.
- **`StopStreamEncryption`.** Needs `EncryptionType` `KMS` and the
  stream's `KeyId`; any other key gets `InvalidArgumentException`. Stopping
  a stream that is not encrypted changes nothing.
- **What callers see.**
  - `DescribeStream` and `DescribeStreamSummary` show `EncryptionType` and
    `KeyId` as the caller named it.
  - `PutRecord` and `PutRecords` answer `EncryptionType` `KMS` for sealed
    records.
  - `GetRecords` marks each sealed record with `EncryptionType` `KMS`.
- **Write limits.** A sealed record costs its partition key and data
  against the shard's write limits, as a plaintext one does. The 36 bytes
  sealing adds are not billed.

## Key providers

`kinesis_kms.hpp` defines the `kms_key_provider` concept. A provider needs
two calls:

- `generate_data_key(key_id, context)`;
- `decrypt_data_key(key_id, wrapped, context)`.

A failure throws `kms_error`. The service answers it with the matching
Kinesis exception:

| Provider failure | Answer |
|---|---|
| `not_found`, or no provider on this node | `KMSNotFoundException` |
| `access_denied` | `KMSAccessDeniedException` |
| `disabled` | `KMSDisabledException` |
| `invalid_state` | `KMSInvalidStateException` |
| `throttling` | `KMSThrottlingException` |
| `opt_in_required` | `KMSOptInRequired` |

A sealed record whose tag fails is corruption, not a key problem, and gets
`InternalFailure`.

`KINESIS_KMS` picks the provider:

- **`off`** (the default). There is no provider: `StartStreamEncryption`
  is refused.
- **`local`**: `local_kms_provider`. It seals data keys the way
  `encrypt_ca_private_key` seals the CA key:
  - AES-256-GCM, under a key derived with PBKDF2-HMAC-SHA256 (200 000
    iterations, a fresh random salt per data key);
  - the passphrase comes from `KINESIS_KMS_PASSPHRASE` or
    `KINESIS_KMS_PASSPHRASE_FILE`, at least 16 bytes, and must be the same
    on every node;
  - any `KeyId` names a key. The id and the encryption context are bound
    to the wrapped key, so it does not unwrap under another id or for
    another stream;
  - a node with a different passphrase answers `KMSInvalidStateException`.
- **`aws`**: `aws_kms_provider` in `kinesis_kms_aws.hpp`. It talks to AWS
  KMS directly over the JSON 1.1 protocol, signed with SigV4, so it needs
  no AWS SDK component:
  - it calls `GenerateDataKey` (`AES_256`) and `Decrypt`, with the
    encryption context;
  - the catalog stores the key ARN that `GenerateDataKey` returns, so
    moving an alias later does not affect reads;
  - the region is `KINESIS_KMS_REGION` (default `KINESIS_REGION`);
  - the endpoint is `KINESIS_KMS_ENDPOINT` as host:port (default AWS's);
  - credentials come from `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY` and
    `AWS_SESSION_TOKEN`, read at startup.

  Another cloud's key service would be one more class that satisfies the
  concept.

## Not yet checked against AWS

Each of these is our reading of AWS's documentation. None has been checked
against a recording of AWS:

- the refusals and their messages;
- `StopStreamEncryption` on a stream that is not encrypted, and with a
  different `KeyId`;
- whether `GetRecords` puts `EncryptionType` on every record.

The AWS KMS adapter is tested against a fake KMS that checks its SigV4
signatures with the gateways' own verifier. It has not been run against
real KMS. That run needs a KMS key, which costs money, so it waits for
Clark's go.

## Tests

`kinesis_encryption_test` has 7 cases:

- the local provider, with its bindings, refusals and type erasure;
- sealed records opening only in their shard, under their key and
  generation;
- the log refusing malformed sealed records;
- an encrypted stream end to end:
  - start, re-key and stop;
  - the segment files holding no plaintext from the encrypted stretch;
  - a second gateway that shares only the passphrase reading every record;
  - the KMS errors from a gateway with a wrong passphrase or with no
    provider;
- the refusals and the error mapping;
- the AWS adapter against a fake KMS: signature checks, request shape and
  error mapping;
- the encryption entries and data keys surviving the codec.
