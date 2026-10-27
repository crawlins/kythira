# Task 1 spike notes: encrypted-bucket checksums

Measured on **2026-10-27** (14:15-14:22 UTC), with Clark's go (Requirement 1.6).

- **S3:** account 827617851594, region **us-east-1**, AWS SDK for C++
  **1.11.665** (the vcpkg-pinned version), program
  `scripts/object-store-probes/s3_checksum_spike/`.
- **OSS:** account 5633986662052576, region **ap-southeast-1** (`ALIBABA_CI_REGION`),
  signed V4 requests via `scripts/object-store-probes/oss_probe.py`.
- **LocalStack:** `localstack/localstack:4.14.0` (the digest CI pins), under rootless Podman.

## Method

The S3 program builds its client the way `aws_s3_client`'s constructor does:
`DefaultRetryStrategy(0)`, `requestChecksumCalculation = WHEN_REQUIRED`,
response validation left at the SDK default, and the default credentials
chain. Every PUT sends `Content-MD5` (base64) and `Content-Type:
application/octet-stream`. Request headers were read after signing
(`RequestSignedHandler`), so they show what went on the wire. Raw response
headers come from the SDK's trace log (`--trace`). A request's
`HeadersReceivedEventHandler` does not fire on this path in 1.11.665.

The body is the same for every service: 4096 bytes, `kythira encrypted-bucket
checksum spike line N\n` repeated and then truncated. Its local digests:

| Digest | Value |
|---|---|
| MD5 (hex) | `d28c5eaad273940c21fffd29d49c0509` |
| MD5 (base64, sent as `Content-MD5`) | `0oxeqtJzlAwh//0p1JwFCQ==` |
| SHA-256 (base64) | `C3xRKvfe8MJrWtMn2pjcrueDzhrr4EfdfzIUL0vWXiU=` |
| CRC32C (base64) | `RJxAvw==` |
| CRC64NVME (base64) | `R4NjxwqUDEk=` |
| CRC64-ECMA (OSS decimal) | `11957008789720121444` |
| Wrong SHA-256 used in case d (SHA-256 of the empty string) | `47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=` |

Both CRC-64 values were also recomputed with a bit-by-bit reflected CRC, using
the parameters the design names. `0xAD93D23594C93659` gives `R4NjxwqUDEk=`
(check value `0xae8b14860a799888`, CRC-64/NVME). `0x42F0E1EBA9EA3693` gives
`11957008789720121444` (check value `0x995dc9bbdf1939fa`, CRC-64/XZ). So the
design's Boost.CRC parameters agree with what each service returned.

## Buckets

| Bucket | Default encryption | Fate |
|---|---|---|
| `kythira-objpersist-kms-827617851594` | `aws:kms`, `alias/aws/s3`, BucketKeyEnabled=false | **kept**, `AWS_OBJECT_PERSISTENCE_KMS_BUCKET` |
| `kythira-spike-sses3-827617851594` | `AES256` | deleted |
| `kythira-spike-dsse-827617851594` | `aws:kms:dsse`, `alias/aws/s3` | deleted |
| `kythira-spike-ssec-827617851594` | `AES256`, SSE-C unblocked, policy denies PutObject without SSE-C | deleted |
| `kythira-objpersist-kms-5633986662052576` (OSS) | `KMS` (default OSS-managed key) | **kept**, `ALIBABA_OBJECT_PERSISTENCE_KMS_BUCKET` |
| `kythira-spike-plain-5633986662052576` (OSS) | none | deleted |
| `kythira-spike-sseoss-5633986662052576` (OSS) | `AES256` | deleted |

**Side finding.** Every new S3 bucket came back from `get-bucket-encryption`
with `"BlockedEncryptionTypes": {"EncryptionType": ["SSE-C"]}`, which
confirms introduction point 2's April 2026 default. To make the SSE-C case
mean something, the SSE-C bucket was first set to `BlockedEncryptionTypes:
NONE`. Its policy was then:
`Deny s3:PutObject` on `bucket/*` when
`Null s3:x-amz-server-side-encryption-customer-algorithm = true`.

## Finding 1: S3 PUT, per encryption mode (Requirement 1.1)

Case legend. All cases also carry `Content-MD5`.

- **a**: no checksum.
- **b**: `SetChecksumSHA256(value)` only.
- **b2**: `SetChecksumAlgorithm(SHA256)` plus the value.
- **c** / **c2**: the same two variants for CRC32C.
- **d** / **d2**: a wrong SHA-256, value only / algorithm plus value.
- **e**: `SetChecksumAlgorithm(SHA256)` with no value.

### Raw response headers (from the SDK trace log)

| Mode | Case | Status | ETag | ETag == MD5 | `x-amz-server-side-encryption` | Checksum headers |
|---|---|---|---|---|---|---|
| SSE-S3 | a | 200 | `"d28c5eaad273940c21fffd29d49c0509"` | **yes** | `AES256` | `x-amz-checksum-crc64nvme: R4NjxwqUDEk=`, `x-amz-checksum-type: FULL_OBJECT` |
| SSE-S3 | b, b2 | 200 | `"d28c5eaad273940c21fffd29d49c0509"` | **yes** | `AES256` | `x-amz-checksum-sha256: C3xRKvfe…WXiU=`, `x-amz-checksum-type: FULL_OBJECT` |
| SSE-S3 | c, c2 | 200 | `"d28c5eaad273940c21fffd29d49c0509"` | **yes** | `AES256` | `x-amz-checksum-crc32c: RJxAvw==`, `x-amz-checksum-type: FULL_OBJECT` |
| SSE-KMS | a | 200 | `"7ce0d6da6d8cb635dd20ab62afc88984"` | **no** | `aws:kms` | `x-amz-checksum-crc64nvme: R4NjxwqUDEk=`, `x-amz-checksum-type: FULL_OBJECT` |
| SSE-KMS | b | 200 | `"eab45a9a7ce58f9922d2e5e3bb41fade"` | **no** | `aws:kms` | `x-amz-checksum-sha256: C3xRKvfe…WXiU=`, `FULL_OBJECT` |
| SSE-KMS | b2 | 200 | `"2d552d85c496def956d461c7a5a4ba41"` | **no** | `aws:kms` | `x-amz-checksum-sha256: C3xRKvfe…WXiU=`, `FULL_OBJECT` |
| SSE-KMS | c | 200 | `"15a1735cc2d1dce9a771cb896388357b"` | **no** | `aws:kms` | `x-amz-checksum-crc32c: RJxAvw==`, `FULL_OBJECT` |
| SSE-KMS | c2 | 200 | `"b7e11357880b7013802eab138f49b197"` | **no** | `aws:kms` | `x-amz-checksum-crc32c: RJxAvw==`, `FULL_OBJECT` |
| DSSE-KMS | a | 200 | `"f4db5671a496f4e7d1983949696ab34d"` | **no** | `aws:kms:dsse` | `x-amz-checksum-crc64nvme: R4NjxwqUDEk=`, `FULL_OBJECT` |
| DSSE-KMS | b | 200 | `"78d909467a8d20f2389bfda3eb051ad5"` | **no** | `aws:kms:dsse` | `x-amz-checksum-sha256: C3xRKvfe…WXiU=`, `FULL_OBJECT` |
| DSSE-KMS | b2 | 200 | `"05b9f457b84bc8731d1676e1d9d2b4ef"` | **no** | `aws:kms:dsse` | `x-amz-checksum-sha256: C3xRKvfe…WXiU=`, `FULL_OBJECT` |
| DSSE-KMS | c | 200 | `"dc5f73999a7148965461225b2df614bd"` | **no** | `aws:kms:dsse` | `x-amz-checksum-crc32c: RJxAvw==`, `FULL_OBJECT` |
| DSSE-KMS | c2 | 200 | `"b223378ba903f16510e173d8337d1355"` | **no** | `aws:kms:dsse` | `x-amz-checksum-crc32c: RJxAvw==`, `FULL_OBJECT` |
| all three | d, d2 | **400 `BadDigest`** | none | n/a | none | none. Message: "The SHA256 you specified did not match the calculated checksum." |
| all three | e | **400 `InvalidRequest`** | none | n/a | none | none. Message: "x-amz-sdk-checksum-algorithm specified, but no corresponding x-amz-checksum-* or x-amz-trailer headers were found." |

In every KMS and DSSE row, the response also carried
`x-amz-server-side-encryption-aws-kms-key-id:
arn:aws:kms:us-east-1:827617851594:key/15533005-752c-418d-bc4b-480d614e3fa9`.

`PutObjectResult` models every one of these headers: `GetETag`,
`GetServerSideEncryption`, `GetChecksumSHA256`, `GetChecksumCRC32C`,
`GetChecksumCRC64NVME` and `GetChecksumType`. Each echoed or default
checksum compared **equal** to the local value in all three modes.

**The SSE-KMS / DSSE-KMS ETag is not just "not the MD5". It is a different
value on every PUT of identical bytes.** A first run (14:15:59Z) gave
`0367e8ba…`, `cf5a0555…` and others for the same cases. So no client could
predict or verify it, even in principle.

The default checksum and an explicit one **replace** each other; they are
never both present. A PUT with no explicit checksum gets
`x-amz-checksum-crc64nvme`. A PUT with an explicit SHA-256 or CRC32C gets that
echo and **no** CRC64NVME.

### Request headers and framing (RequestSignedHandler)

| Case | Body framing | Checksum-related request headers |
|---|---|---|
| a | `content-length: 4096`. `x-amz-content-sha256` is the real payload SHA-256 (`0b7c512a…5e25`). | `content-md5` only |
| b, b2 | `content-length: 4096`, same signed payload hash | `x-amz-checksum-sha256: C3xRKvfe…WXiU=` **and** `x-amz-sdk-checksum-algorithm: SHA256` |
| c, c2 | `content-length: 4096`, same signed payload hash | `x-amz-checksum-crc32c: RJxAvw==` **and** `x-amz-sdk-checksum-algorithm: CRC32C` |
| e | `content-length: 4096` | `x-amz-sdk-checksum-algorithm: SHA256`, no value (S3 rejects it, see above) |

The trace log for every request has **no** `aws-chunked`, `x-amz-trailer`,
`x-amz-decoded-content-length`, `Content-Encoding` or `STREAMING-*` payload
hash. The only match on "x-amz-trailer" is S3's own error text for case e.
Under `WHEN_REQUIRED`, a checksum value set on the request goes out as a
plain header, with no recomputation and no trailer.

Two details matter for Requirement 5.3:

- The SDK adds `x-amz-sdk-checksum-algorithm` itself whenever a checksum
  value is set, **even if `SetChecksumAlgorithm` was never called** (case b).
  So route A adds **two** request headers per PUT, not one.
- Setting only the algorithm makes the SDK compute nothing under
  `WHEN_REQUIRED`, and S3 refuses the request (case e). Route A must therefore
  set the value. Setting only the algorithm is not a shortcut.

### Conditional requests on the SSE-KMS bucket

| Request | Result |
|---|---|
| PUT `If-None-Match: *`, new key | 200, ETag `"b1787e2e62b8bb70d76e086e50d47716"` (non-MD5), crc64nvme `R4NjxwqUDEk=` |
| PUT `If-None-Match: *`, existing key | **412 `PreconditionFailed`** |
| PUT `If-Match: "b1787e2e…"` (right) | 200, new ETag `"17c797f65924ec2da06250f80ab51d0e"` |
| PUT `If-Match: "0123456789abcdef0123456789abcdef"` (wrong) | **412 `PreconditionFailed`** |
| DELETE `If-Match` wrong ETag | **412 `PreconditionFailed`** |
| DELETE `If-Match` right ETag | 204 |

These match the unencrypted bucket's behaviour recorded in
cloud-object-persistence spike-notes Finding 10. Fencing treats the KMS ETag
as an opaque version and works (introduction point 3, confirmed).

## Finding 2: SSE-C (Requirement 1.3)

A plain PUT from the same configuration to the SSE-C-policy bucket:

> **403 `AccessDenied`**: `User: arn:aws:iam::827617851594:root is not
> authorized to perform: s3:PutObject on resource:
> "arn:aws:s3:::kythira-spike-ssec-827617851594/spike/…/ssec-policy" with an
> explicit deny in a resource-based policy`

So this client never sees a stored object with a non-MD5 ETag under SSE-C.
The bucket refuses the write outright, with 403, not with a checksum mismatch.
Introduction point 2 is **confirmed**. The `aws_s3_client.hpp` comment's "SSE-C
default encryption" is wrong on two counts: SSE-C cannot be a default
encryption, and new buckets block SSE-C anyway.

## Finding 3: SDK headers (open question 1)

The vcpkg-installed `aws-sdk-cpp` is **1.11.665**
(`vcpkg_installed/x64-linux/share/aws-sdk-cpp/vcpkg.spdx.json`).
`aws/s3/model/PutObjectResult.h` declares
`const Aws::String& GetChecksumCRC64NVME() const` (line 120) and
`ChecksumType GetChecksumType() const` (line 174). Finding 1 shows both are
populated on a live PUT. **Open question 1: yes.**

`Aws::Utils::HashingUtils::CalculateCRC64` in this SDK version is CRC-64/NVME.
It produced `R4NjxwqUDEk=`, the value S3 returned. It is usable as a test
oracle, but the engine should use Boost.CRC per Requirement 2.5.

## Finding 4: LocalStack (Requirement 1.4)

The same program ran against LocalStack 4.14.0 with
`--endpoint http://127.0.0.1:4566`, so path-style and IMDS off, as the client
behaves.

| Bucket | ETag == MD5 | Notes |
|---|---|---|
| SSE-S3 | yes | as real S3 |
| SSE-KMS, `alias/aws/s3` | **yes** | real S3: no |
| SSE-KMS, a created CMK | **yes** | real S3: no |
| DSSE-KMS | n/a | `put-bucket-encryption` with `aws:kms:dsse` refused: "a KMSMasterKeyID is not applicable if the default sse algorithm is not aws:kms or aws:kms:dsse". The bucket stayed AES256. |

The other differences from real S3:

- Case e (algorithm only) **succeeds** on LocalStack and returns CRC64NVME.
  Real S3 answers 400.
- Conditional DELETE with `If-Match` returns **501 `NotImplemented`**.
- These match real S3:
  - echoed SHA-256 / CRC32C;
  - default CRC64NVME (`R4NjxwqUDEk=`);
  - `x-amz-server-side-encryption: aws:kms`;
  - `BadDigest` on a wrong SHA-256;
  - 412 on a failed conditional PUT.

**LocalStack does not reproduce the SSE-KMS ETag behaviour.** Requirement 1.4
applies: the unit-level reproduction of an encrypted bucket is the mock
alone. LocalStack can still exercise the digest path (it returns CRC64NVME),
but it cannot show the ETag failing.

## Finding 5: OSS (Requirement 1.2)

Each bucket received two identical PUTs with `Content-MD5`, then a DELETE.

| Bucket | Status | ETag | ETag == MD5 (upper hex) | `x-oss-hash-crc64ecma` | `Content-MD5` (response) | `x-oss-server-side-encryption` |
|---|---|---|---|---|---|---|
| none | 200, 200 | `"D28C5EAAD273940C21FFFD29D49C0509"` both | **yes** | `11957008789720121444` | `0oxeqtJzlAwh//0p1JwFCQ==` | absent |
| SSE-OSS (AES256) | 200, 200 | `"D28C5EAAD273940C21FFFD29D49C0509"` both | **yes** | `11957008789720121444` | `0oxeqtJzlAwh//0p1JwFCQ==` | `AES256` |
| SSE-KMS | 200, 200 | `"D28C5EAAD273940C21FFFD29D49C0509"` both | **yes** | `11957008789720121444` | `0oxeqtJzlAwh//0p1JwFCQ==` | `KMS`, key id `561a5a5d-85eb-4e38-bc79-391e7fb12ecc` |

**The OSS ETag is still the content MD5 under SSE-KMS**, and it stays the same
across repeated PUTs. Requirement 4.1 is the branch: no code change.
`alibaba_oss_client.hpp` makes no claim about KMS that needs correcting. Task 5
should add a sentence saying that KMS was measured and is unaffected.

## Finding 6: CI access to the kept buckets

**AWS.** The CI role `kythira-ci-real-cloud-tests` gets the object-persistence
bundle from its inline policy. That policy is shared by every bundle.
Adding the KMS bucket to `policies/object-persistence.json` pushed the
all-bundles render to **10,340 of 10,240** characters
(`render-ci-policy.py --check` failed), so the policy was left unchanged.

The KMS bucket instead carries a **bucket policy** that grants the role the
same scope as the bundle: Get/Put/DeleteObject on `kythira-real-test/*`, and
ListBucket conditioned on that prefix. It is applied by
`provision-object-persistence-bucket.sh --encryption sse-kms`.

The role's permissions boundary allows everything except `iam:*`,
`organizations:*` and `account:*`. The `aws/s3` key policy grants any
principal in the account through S3 (`kms:ViaService =
s3.us-east-1.amazonaws.com`, `kms:CallerAccount = 827617851594`), so no KMS
grant is needed. `iam simulate-principal-policy`, with the bucket or key
policy as the resource policy, returns:

| Action | Resource | Decision |
|---|---|---|
| s3:PutObject / GetObject / DeleteObject | `…kms…/kythira-real-test/x` | allowed |
| s3:PutObject | `…kms…/other/x` | implicitDeny |
| s3:ListBucket, prefix `kythira-real-test/` | bucket | allowed |
| kms:GenerateDataKey, kms:Decrypt (via S3) | `aws/s3` key | allowed |

**Alibaba.** The RAM bundle `alibaba/policies/oss-persistence.json` now also
names `kythira-objpersist-kms-*`. This is **not yet applied** to the live
`kythira-ci-oss-persistence` policy. Default version v5 still names only
`kythira-ci-5633986662052576`.

It is also unmeasured whether OSS SSE-KMS with the **default** OSS-managed key
needs `kms:GenerateDataKey` / `kms:Decrypt` on the caller. Alibaba's
documentation lists those permissions for a *specified* CMK and says nothing
about the default key. The CI role can be assumed only through GitHub OIDC,
so it could not be tried locally. Task 6's first OSS run will answer it.
