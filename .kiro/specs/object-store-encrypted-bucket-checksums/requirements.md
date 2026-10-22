# Requirements Document

## Introduction

`object_store_persistence_engine` verifies every PUT twice
(cloud-object-persistence Requirement 7):

- **Service-side (7.1).** The client sends `Content-MD5`. The service
  recomputes it and refuses the write on a mismatch (`BadDigest` on S3,
  `InvalidDigest` on OSS). This is the client's configuration and is always on.
- **Local (7.2).** On a store that declares `content_md5_versioned_store`
  (`aws_s3_client`, `alibaba_oss_client`), the engine compares the returned
  ETag against the MD5 it computed itself and throws on a mismatch.
  `object_persistence_options::verify_checksums` governs this half only.

The local half rests on "the ETag of a single-part upload is the content MD5".
AWS documents that this is false for an object encrypted with SSE-KMS or SSE-C
("that object has an ETag that is not an MD5 digest of its object data"). So,
per the comment in `aws_s3_client.hpp`, against an SSE-KMS bucket **every
write fails verification**, twice (the mismatch is retried as a corrupted
transfer), and the documented remedy is `verify_checksums = false`.

This spec decides **whether that remedy can be retired**: whether the local
check can stay on against an encrypted bucket by verifying a value that *is*
a content digest under every encryption mode. It is an investigation first.
Task 1 measures the real behaviour on the real services. The design commits
to an implementation only for the routes the measurement leaves standing,
and records "cannot be overcome" as an acceptable outcome if none does.

What is already known, before any measurement:

1. **The service-side half is unaffected.** `Content-MD5` is evaluated on the
   bytes received, before encryption. An operator who sets
   `verify_checksums = false` today keeps 7.1, so the gap is the local half
   only: the defence against an endpoint that ignores `Content-MD5` (an
   S3-compatible store, a proxy) and against a response that does not belong
   to the request.
2. **SSE-C is a different problem from SSE-KMS.** SSE-C is not a bucket
   default encryption. It is a per-request customer key, and
   `aws_s3_client` sends no SSE-C headers. A bucket that requires SSE-C
   (via bucket policy) refuses this client's PUTs outright rather than
   storing them with a non-MD5 ETag. Since April 2026, S3 also disables SSE-C
   by default on new buckets. The `aws_s3_client.hpp` comment's "SSE-C default
   encryption" is therefore at best imprecise, and task 1 settles it.
3. **Fencing is unaffected.** `If-Match` / `If-None-Match: *` treat the ETag
   as an opaque version. That is true whatever the ETag is computed from.
4. **Both affected providers return a second digest on PUT.** S3 echoes an
   `x-amz-checksum-<algorithm>` header for a checksum the client supplied and,
   since S3's late-2024 default-integrity change, stores a CRC64NVME
   full-object checksum on every new object. OSS returns
   `x-oss-hash-crc64ecma` and a `Content-MD5` response header on every PUT.
   Whether any of these survive SSE-KMS, and whether the AWS SDK returns them
   under `requestChecksumCalculation = WHEN_REQUIRED`, is unmeasured.

**Scope:** `aws_s3_client`, `alibaba_oss_client`, the engine's local
verification hook and the store trait it reads, `put_result`, the mocks and
unit tests, the real-cloud S3/OSS object-persistence bundles, and the
operator documentation.

**Out of scope:** SSE-C support (sending customer keys on every request and
holding them in client configuration); multipart uploads (still a non-goal
of the engine); Azure, GCS and OCI, which do not declare
`content_md5_versioned_store` and so never fail this way (their
customer-managed-key behaviour is recorded by task 1 only as a note for a
future local check on those providers); verification on GET.

## Glossary

- **Local check** — the engine's comparison of a value the store returned on
  PUT against a digest the engine computed from the bytes it sent
  (Requirement 7.2).
- **Content digest** — a value that is a deterministic function of the
  object's plaintext bytes and nothing else: MD5, SHA-256, CRC32C, CRC64NVME,
  CRC64-ECMA.
- **Echoed checksum** — an `x-amz-checksum-*` value the client sent and the
  service returns after validating it.
- **Encrypted bucket** — a bucket whose default encryption is SSE-KMS or
  DSSE-KMS, or one whose policy forces either on every PUT. SSE-S3 (`AES256`,
  the S3 default since January 2023) is *not* an encrypted bucket for this
  spec, because its ETag is still the MD5.

## Requirements

### Requirement 1: Measure before deciding

**User Story:** As a maintainer, I want the real services' behaviour on an
encrypted bucket recorded, so that the design rests on measurement rather than
on documentation that is silent on half the questions.

#### Acceptance Criteria

1. A spike SHALL record, for S3 with SSE-S3, SSE-KMS (AWS-managed `aws/s3`
   key) and DSSE-KMS bucket defaults, for a single-part PUT of a known body
   made by `aws_s3_client`'s exact SDK configuration:
   - the returned ETag and whether it equals the content MD5;
   - the `x-amz-server-side-encryption` response header;
   - every `x-amz-checksum-*` and `x-amz-checksum-type` response header, with
     and without an explicit `ChecksumSHA256` / `ChecksumCRC32C` on the
     request;
   - whether an explicit checksum leaves the body as a plain
     `Content-Length` request (no `aws-chunked` framing) under
     `WHEN_REQUIRED`;
   - whether S3 refuses a wrong explicit checksum, and with what code;
   - whether `If-Match` / `If-None-Match: *` behave as on an unencrypted
     bucket.
2. The spike SHALL record, for OSS with no encryption, SSE-OSS (`AES256`) and
   SSE-KMS, the returned ETag and whether it equals the content MD5, and the
   `x-oss-hash-crc64ecma` and `Content-MD5` response headers.
3. The spike SHALL record what a PUT from `aws_s3_client` gets from a bucket
   whose policy requires SSE-C, confirming or refuting introduction point 2.
4. The spike SHALL record whether LocalStack reproduces the SSE-KMS ETag
   behaviour. If it does not, the unit-level reproduction is the mock alone,
   and the spec SHALL say so.
5. Findings SHALL be written to `spike-notes.md` in this spec's directory,
   each with the date, region, and raw header values.
6. Any real-cloud run, including the spike, SHALL wait for Clark's go.

### Requirement 2: A store reports a content digest, not only a version

**User Story:** As a maintainer, I want the engine's local check to read a
digest the client vouches for, so that the check does not depend on the ETag
happening to be an MD5.

#### Acceptance Criteria

1. `put_result` SHALL gain an optional content digest: an algorithm and a
   value in that algorithm's canonical encoding. A client that returns none
   SHALL compile and behave exactly as today.
2. A store SHALL be able to declare that **every** successful PUT carries a
   content digest, by a static declaration in the style of
   `version_is_content_md5`. The engine SHALL NOT infer it.
3. WHEN `verify_checksums` is on AND the store declares the digest AND a PUT
   returns one THEN the engine SHALL compute the same algorithm over the bytes
   it sent and throw on a mismatch, naming the key, the algorithm, both
   values, and the provider. A mismatch SHALL stay retryable, as today.
4. WHEN `verify_checksums` is on AND the store declares the digest AND a PUT
   returns none THEN the engine SHALL throw, naming the key and saying that the
   store promised a digest and sent none. A promised check that silently does
   not happen is the failure this requirement exists to prevent.
5. The engine SHALL support exactly the algorithms some client returns after
   task 1, computed with code already in the tree (OpenSSL for MD5 and
   SHA-256; Boost.CRC for any CRC). No new dependency.
6. `content_md5_versioned_store` SHALL keep working for any client that still
   declares it, and the engine SHALL prefer a reported digest over the
   version when a store provides both.

### Requirement 3: The S3 client keeps the local check on an encrypted bucket

**User Story:** As an operator on an SSE-KMS bucket, I want writes verified
with the default options, so that I am not told to turn integrity checking
off.

#### Acceptance Criteria

1. IF task 1 shows an echoed or default checksum that is a content digest
   under SSE-KMS and DSSE-KMS THEN `aws_s3_client` SHALL return it as
   `put_result`'s digest on every PUT, and SHALL declare the digest (2.2).
2. IF the route needs an explicit checksum on the request THEN the client
   SHALL send it as a request header, SHALL keep `Content-MD5` on the same
   request, and SHALL keep `requestChecksumCalculation = WHEN_REQUIRED`, so
   the body framing stays the one the existing file comment pins. A unit test
   SHALL fail if the body becomes `aws-chunked`.
3. The client SHALL keep returning the ETag as `put_result::version`
   unchanged, because fencing reads it.
4. WHEN a PUT's ETag does not equal the content MD5 AND the response's
   `x-amz-server-side-encryption` is `aws:kms` or `aws:kms:dsse` AND no
   digest route is available THEN any mismatch error SHALL name the
   encryption mode and the remedy, rather than reading as a corrupted
   transfer. This criterion applies only if Requirement 3.1 cannot be met.
5. IF no route survives task 1 THEN the client SHALL keep today's behaviour,
   the error message SHALL meet 3.4, and the outcome "cannot be overcome on
   S3" SHALL be recorded in the design with its evidence.

### Requirement 4: The OSS client keeps the local check on an encrypted bucket

**User Story:** As an operator on an OSS bucket with KMS encryption, I want
the same as on S3.

#### Acceptance Criteria

1. IF task 1 shows the OSS ETag is still the content MD5 under SSE-KMS THEN
   no code change SHALL be made, and the `alibaba_oss_client.hpp` comment
   that says otherwise SHALL be corrected.
2. ELSE IF `x-oss-hash-crc64ecma` or the `Content-MD5` response header is a
   content digest under SSE-KMS THEN `alibaba_oss_client` SHALL return it as
   `put_result`'s digest and declare the digest (2.2).
3. The client's request signing SHALL be unchanged: reading a response
   header needs no new signed header.

### Requirement 5: The default stays safe, and the escape hatch stays

1. `verify_checksums` SHALL keep its name, its default (`true`), and its
   meaning (the local half only).
2. `verify_checksums = false` SHALL still disable the local check entirely,
   on every route, so an operator on an S3-compatible service that returns
   no digest and a non-MD5 ETag is not stuck.
3. On a bucket with no encryption or SSE-S3, the number and shape of requests
   SHALL be unchanged, except for at most one added request header per PUT
   when Requirement 3.2 applies. No added request, no added round trip.

### Requirement 6: Tests

1. The S3 and OSS mock servers SHALL be able to answer a PUT as an encrypted
   bucket does, per task 1's recorded headers: a non-MD5 ETag, the encryption
   header, and the digest headers (or their absence).
2. The conformance suite SHALL gain cases, run on every substrate, for: a
   reported digest that matches; one that does not (throws, then a retry
   succeeds); a store that declares the digest and omits it (throws, 2.4);
   and `verify_checksums = false` with a wrong digest (no throw).
3. Each client's unit suite SHALL pin the encrypted-bucket response with the
   default options: it passes after this spec and fails, naming the checksum
   mismatch, with the change reverted.
4. The real-cloud S3 and OSS object-persistence bundles SHALL gain an
   encrypted-bucket leg with `verify_checksums` left at its default. Each leg
   SHALL skip, not fail, when its bucket variable is unset.

### Requirement 7: Documentation

1. The `aws_s3_client.hpp` and `alibaba_oss_client.hpp` file comments, the
   `verify_checksums` comment, and `doc/cloud_object_persistence.md` SHALL
   state what task 1 measured: which encryption modes break the ETag, which
   digest the client verifies instead, and what still needs
   `verify_checksums = false`.
2. The SSE-C statement SHALL be corrected to whatever Requirement 1.3
   records.
