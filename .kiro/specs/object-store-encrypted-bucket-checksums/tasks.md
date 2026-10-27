# Implementation Plan

Keeping `verify_checksums` on for SSE-KMS buckets. Task 1 is a measurement
that decides which of the later tasks run. Each later task is one mergeable
PR. Every real-cloud step (tasks 1 and 6) needs Clark's go before it runs.

- [x] 1. Spike: measure S3 and OSS behaviour on encrypted buckets
  - Needs Clark's go, and the encrypted test buckets (design "Real-cloud
    leg") created first
  - Small standalone program on `aws_s3_client`'s exact SDK configuration:
    PUT a known body to SSE-S3, SSE-KMS and DSSE-KMS buckets, with no
    explicit checksum, with `ChecksumSHA256`, with `ChecksumCRC32C`, and with
    a deliberately wrong `ChecksumSHA256`; record every header of
    Requirement 1.1 and the request's body framing
  - Conditional PUT/DELETE (`If-Match`, `If-None-Match: *`) on the SSE-KMS
    bucket
  - A PUT to a transient bucket whose policy requires SSE-C (Requirement
    1.3); delete the bucket afterwards
  - OSS: PUT to unencrypted, SSE-OSS and SSE-KMS buckets; record ETag,
    `x-oss-hash-crc64ecma`, `Content-MD5` response header
  - LocalStack: does an SSE-KMS bucket return a non-MD5 ETag?
  - Read the pinned SDK headers for `GetChecksumCRC64NVME` (open question 1)
  - Output: `spike-notes.md` here, and a design amendment choosing route A,
    route C or "cannot be overcome" for S3, and the OSS branch of
    Requirement 4
  - _Requirements: 1.1-1.6_

- [x] 2. Engine: reported content digests
  - `content_digest`, `digest_algorithm` (only the algorithms task 1
    chose), `put_result::digest`, `content_digest_reporting_store`
  - `verify_returned_digest` per the design's four steps; local digest
    helpers for the chosen algorithms, CRCs pinned against their published
    check values
  - Conformance: fault-harness `wrong_digest_for_next_puts` and
    `no_digest_for_next_puts`; the four cases of Requirement 6.2 on every
    substrate
  - No client declares the new trait yet, so no behaviour changes
  - _Requirements: 2.1-2.6, 5.1, 5.2, 6.2_

- [ ] 3. `aws_s3_client`: report the digest (skipped if task 1 says "cannot
  be overcome"; then do 3b)
  - Route A or C per task 1; declare `reports_content_digest`, drop
    `version_is_content_md5`
  - `MockS3` encrypted-bucket mode from task 1's recorded headers; unit case
    that the default options succeed against it and fail with the change
    reverted; request-shape case failing on `aws-chunked` / `x-amz-trailer`
  - _Requirements: 3.1-3.3, 5.3, 6.1, 6.3_

- [ ] 3b. (Only if no S3 route survives) error message names the encryption
  mode and the remedy
  - Read `x-amz-server-side-encryption` from the PUT response; carry it into
    the mismatch message
  - _Requirements: 3.4, 3.5_

- [ ] 4. `alibaba_oss_client`: per task 1
  - ETag still MD5 under KMS: comment fix only, folded into task 5
  - Otherwise: report `Content-MD5` (preferred) or `x-oss-hash-crc64ecma`;
    declare the trait; mock encrypted-bucket mode and unit case as in
    task 3
  - _Requirements: 4.1-4.3, 6.1, 6.3_

- [ ] 5. Documentation
  - File comments in `aws_s3_client.hpp` and `alibaba_oss_client.hpp`, the
    `verify_checksums` comment, `doc/cloud_object_persistence.md`, and the
    SSE-C correction
  - _Requirements: 7.1, 7.2_

- [ ] 6. Real-cloud encrypted-bucket legs
  - `real-cloud-tests.yml`: second run of the S3 (and OSS) object-persistence
    test against `AWS_OBJECT_PERSISTENCE_KMS_BUCKET` /
    `ALIBABA_OBJECT_PERSISTENCE_KMS_BUCKET`, default options, skipping when
    unset
  - One dispatched run, with Clark's go, passing on x64 and arm64
  - _Requirements: 6.4_
