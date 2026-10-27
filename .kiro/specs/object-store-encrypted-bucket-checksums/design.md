# Design Document

## Overview

The question is whether the engine's local integrity check (cloud-object-
persistence Requirement 7.2) can stay on against an SSE-KMS bucket. Today it
compares the ETag with the content MD5, and on an encrypted bucket the ETag is
not the MD5, so every PUT fails twice and the operator is told to set
`verify_checksums = false`.

The answer this design expects, pending task 1's measurements, is **yes on
S3, probably yes on OSS**, by verifying a different returned value that is a
content digest under every encryption mode:

| Provider | Today's local check | Proposed local check | Request change |
|---|---|---|---|
| S3 | ETag == MD5 hex | `x-amz-checksum-sha256` echoed on PUT == local SHA-256 | one header (`x-amz-checksum-sha256`) |
| S3 (fallback) | | `x-amz-checksum-crc64nvme` (S3's default checksum) == local CRC64NVME | none |
| OSS | ETag == MD5 hex | ETag if task 1 shows it is still MD5 under KMS; else `x-oss-hash-crc64ecma` == local CRC64-ECMA | none |

The engine gains one generic hook ("the store reported this digest of what
it stored; check it"), and each client chooses what to report. That keeps
the layering of cloud-object-persistence Requirement 7.0: the client knows
its headers, the engine holds the bytes.

## Spike result (task 1, 2026-10-27)

The measurements are in `spike-notes.md`. The decision:

- **S3: route C.** Under `WHEN_REQUIRED`, on SSE-S3, SSE-KMS and DSSE-KMS
  alike, a PUT with no explicit checksum returns
  `x-amz-checksum-crc64nvme` (`FULL_OBJECT`). It equals the local
  CRC-64/NVME of the body, and SDK 1.11.665 models it as
  `PutObjectResult::GetChecksumCRC64NVME()`. Route C needs no request change
  at all, so Requirement 5.3 holds with nothing added.
- **Route A works on the wire but fails 5.3 as written.** The echo comes back
  under all three modes, the body stays plain `Content-Length`, and a wrong
  value gets 400 `BadDigest`. But the SDK adds `x-amz-sdk-checksum-algorithm`
  whenever a checksum value is set, so route A costs **two** request headers
  per PUT. It is the alternative if Clark prefers a second service-side check
  and relaxes 5.3 to two headers.
- **Route A excludes route C.** An explicit checksum replaces the default
  CRC64NVME in the response; the two are never returned together.
- **The KMS ETag is a different value on every PUT of the same bytes**, so
  nothing ETag-based (route B included) could ever verify it. Fencing is
  unaffected: If-Match and If-None-Match return 412 / 200 exactly as on an
  unencrypted bucket.
- **SSE-C.** A plain PUT to a bucket whose policy requires SSE-C gets
  **403 `AccessDenied`** (explicit deny), not a mismatch. New buckets also
  block SSE-C by default. Requirement 7.2's correction is "SSE-C buckets
  refuse this client's writes".
- **OSS: Requirement 4.1.** The ETag is still the uppercase MD5 hex under
  SSE-KMS, as with no encryption and with SSE-OSS. No code change. Task 4
  folds into task 5 as documentation.
- **LocalStack 4.14.0 does not reproduce the KMS ETag** (it returns the MD5)
  and rejects DSSE bucket config. The encrypted-bucket unit case therefore
  rests on `MockS3` alone (Requirement 1.4). LocalStack does return
  CRC64NVME, so the digest path itself can still run there.

Consequences for the later tasks:

- `digest_algorithm` needs only `crc64nvme` (Requirement 2.5). The Boost.CRC
  parameters below reproduce S3's value and the published check value
  `0xae8b14860a799888`.
- Task 3 declares `reports_content_digest`, returns
  `{crc64nvme, GetChecksumCRC64NVME()}`, and keeps the request untouched.
- **One trade-off task 3 must settle explicitly.** An S3-compatible endpoint
  (`endpoint_override`) whose ETag *is* the MD5 but which sends no
  `x-amz-checksum-crc64nvme` verifies today. Under 2.4 it would throw on
  every PUT once the client declares the digest. Route A has the same
  exposure for a store that ignores the header. The options:
  - declare the digest only for AWS endpoints;
  - fall back to the ETag comparison when no digest arrives and the ETag is
    an MD5-shaped value (this weakens 2.4 for that case only);
  - accept the regression and document `verify_checksums = false` for such
    stores.

  The first keeps 2.4 strict where it matters.

## Routes considered

### A. An explicit flexible checksum on the request, verified from the echo (recommended for S3)

The client computes SHA-256 of the body, sends it as `x-amz-checksum-sha256`
(base64), and S3 validates it and stores it with the object. AWS documents
that this validation "works consistently across encryption modes". The
`PutObjectResult` carries the checksum back (`GetChecksumSHA256()`), and the
engine compares that against its own SHA-256.

- **What it buys.** A local check that holds under SSE-S3, SSE-KMS and
  DSSE-KMS alike. A second, stronger service-side check on the same request
  (S3 evaluates both `Content-MD5` and the flexible checksum). And an
  endpoint that silently ignores the header (some S3-compatible stores) is
  caught, because it returns no echo and Requirement 2.4 turns that into an
  error instead of a pass.
- **What it costs.** One SHA-256 per PUT. OpenSSL is already linked and runs
  at GB/s, against log segments and snapshots that are capped at 64 MiB and
  are usually KiB, so this is far below a round trip. One extra request
  header.
- **The risk task 1 must close.** The client pins
  `requestChecksumCalculation = WHEN_REQUIRED` because the SDK's default
  computes its own checksum and can move the body onto `aws-chunked`
  trailer framing. A checksum *value* set on the request should go out as a
  plain header, with no recomputation and no trailer, but this has to be
  measured on the pinned SDK version rather than assumed. If the SDK
  insists on a trailer, the fallback is route C.
- **Why SHA-256 rather than CRC32C.** CRC32C is AWS's speed recommendation,
  but both hashes are cheap at these object sizes, SHA-256 needs no new code
  (`EVP_sha256`), and a cryptographic digest leaves no question about
  collisions on adversarial content. The algorithm is a client constant, so
  changing it later is a one-line change plus its tests.

### B. Detect the encryption mode and skip the check (rejected alone, kept for messages)

The PUT response's `x-amz-server-side-encryption` says `AES256`, `aws:kms` or
`aws:kms:dsse`. A client could stop vouching for the ETag whenever it is not
`AES256`. On its own this is just `verify_checksums = false` applied
automatically, which turns a loud configuration error into a silently
missing check. It is rejected as a route. It is kept for the error message
(Requirement 3.4) if no route survives, so the operator reads "this bucket
uses aws:kms, whose ETag is not an MD5" instead of "checksum mismatch".

### C. S3's default CRC64NVME checksum (fallback for S3)

Since late 2024 S3 stores a CRC64NVME full-object checksum on every new
object, including ones whose client sent no checksum. If the PUT response
carries it under `WHEN_REQUIRED`, the client can return it with **no change
to the request at all**. The costs are a local CRC64NVME implementation
(`boost::crc_optimal<64, 0xAD93D23594C93659, ~0, ~0, true, true>` from
Boost.CRC, pinned against AWS's published check value) and a dependence on
undocumented response behaviour. S3-compatible stores are less likely to
emit it than an echo of a header they were sent. Task 1 decides whether it
is available. It is the fallback if route A's framing risk materialises.

### D. Read the object back and hash it (rejected)

A GET after every PUT, hashed locally, works under any encryption. It
doubles the requests on the commit path, adds a full round trip to every
append (the group-commit spec measured a PUT p50 of ~35-160 ms), and adds a
KMS decrypt to every read-back. It also only proves what a later reader
would get, which the next recovery proves anyway. Rejected for the write
path. Not offered as an option either, because an option whose cost is
"halve your commit rate" will not be turned on.

### E. `Content-MD5` on the PUT (already in place)

Every PUT already carries `Content-MD5`, and S3 and OSS refuse a mismatch
before encryption. It is the service-side half, it is unaffected by
encryption, and it stays. It is recorded here because "send Content-MD5"
is the first suggestion anyone makes, and it does not address the gap: the
gap is the local half.

### OSS

OSS returns `x-oss-hash-crc64ecma` and a `Content-MD5` response header on
every PUT, and its documentation does not say what its ETag is under KMS.
Task 1 measures that first. If the ETag stays the MD5, the only change is
correcting the `alibaba_oss_client.hpp` comment (Requirement 4.1). If not,
the client reports one of the two headers, preferring `Content-MD5` because
the engine already computes MD5, and falling back to CRC64-ECMA
(`boost::crc_optimal<64, 0x42F0E1EBA9EA3693, ~0, ~0, true, true>`, pinned
against OSS's published check value). Either way, no new request header and
no change to signing.

### SSE-C

`aws_s3_client` sends no SSE-C headers, SSE-C cannot be a bucket default,
and S3 has disabled it on new buckets since April 2026. A bucket that
requires it refuses this client's PUTs. So SSE-C is not a case where
"every write fails verification". It is a case where every write fails, and
that is out of scope (supporting it means carrying customer keys on every
request). Task 1 confirms the refusal and the comment is corrected.

## Data types

```cpp
// key_object_store.hpp
enum class digest_algorithm { md5, sha256, crc64nvme, crc64ecma };

/// What the store says it stored, as a function of the bytes alone.
/// `value` is in the algorithm's wire encoding as the client received it
/// (hex for MD5, base64 for SHA-256 and CRC64NVME, decimal for OSS's
/// CRC64-ECMA); the engine compares after normalising per algorithm.
struct content_digest {
    digest_algorithm algorithm;
    std::string value;
};

struct put_result {
    object_version version;
    std::optional<content_digest> digest;  // new; absent = nothing reported
};

/// Declared by `static constexpr bool reports_content_digest = true;`.
/// A promise that every successful PUT returns `digest`.
template<typename S>
concept content_digest_reporting_store =
    key_object_store<S> && requires { S::reports_content_digest; } &&
    S::reports_content_digest;
```

`put_result` is an aggregate, so every existing `put_result{version}` still
compiles with `digest` empty. Only the algorithms some client actually
returns after task 1 are added to the enum (Requirement 2.5).

## Engine change

`verify_returned_digest` becomes:

1. If `verify_checksums` is off, return (Requirement 5.2).
2. If the store is a `content_digest_reporting_store`:
   - `digest` present: compute that algorithm over `bytes`, compare, throw on
     mismatch naming key, algorithm, both values and provider.
   - `digest` absent: throw "promised a digest and sent none" (2.4).
   - Return. The version is not consulted.
3. Else, if the store is a `content_md5_versioned_store`: today's ETag
   comparison, unchanged.
4. Else: nothing, as today.

The check stays inside `put_with_retry`'s loop, so a mismatch is retried once
exactly as now. The digest is computed only on the branch that needs it, so a
store that reports nothing still costs nothing.

## Client changes (subject to task 1)

**`aws_s3_client`**, route A: `send_put` sets `ChecksumAlgorithm::SHA256` and
`SetChecksumSHA256(base64(sha256(bytes)))` alongside the existing
`SetContentMD5`, and returns `content_digest{sha256, GetChecksumSHA256()}`.
The client declares `reports_content_digest` and stops declaring
`version_is_content_md5`, whose promise is false on exactly the buckets this
spec is about. On an unencrypted bucket the check is as strong as before
(stronger: SHA-256 over MD5), so nothing is lost by dropping the old
declaration. A unit test inspects the request the mock receives and fails on
`Content-Encoding: aws-chunked` or `x-amz-trailer` (Requirement 3.2).

Route C instead: no request change, and the digest comes from
`GetChecksumCRC64NVME()`.

**`alibaba_oss_client`**: reads one more response header and returns it.

## Mocks and tests

- `MockS3` (tests/aws_s3_client_unit_test.cpp) and the OSS mock gain an
  "encrypted bucket" mode replaying task 1's headers: a non-MD5 ETag, the
  encryption header, and the echoed checksum, or no checksum for the
  negative case.
- The conformance suite's fault harness gains `wrong_digest_for_next_puts`
  and `no_digest_for_next_puts` beside the existing
  `wrong_version_for_next_puts`, and the cases of Requirement 6.2. The
  existing `verification_can_be_disabled` case keeps its meaning.
- Mutation checks, as the rest of the object-store work does: reverting the
  client change makes the encrypted-bucket unit case fail with a checksum
  mismatch; removing the "absent digest" throw makes the 6.2 omission case
  pass wrongly and so fail.

## Real-cloud leg

`real-cloud-tests.yml`'s S3 object-persistence step runs
`aws_s3_object_persistence_real_test` a second time with
`KYTHIRA_AWS_S3_BUCKET` set to a new repository variable,
`AWS_OBJECT_PERSISTENCE_KMS_BUCKET`: a bucket whose default encryption is
SSE-KMS with the AWS-managed `aws/s3` key (no monthly key charge), Bucket
Key off so every PUT really calls KMS. OSS likewise with
`ALIBABA_OBJECT_PERSISTENCE_KMS_BUCKET`. Each leg skips when its variable is
unset. Creating those buckets, and any run, needs Clark's go. The cost is
the existing bundle's (a few hundred small PUTs) plus KMS requests at
$0.03 per 10,000.

The spike (task 1) uses the same buckets plus one transient SSE-C-policy
bucket, by a small standalone program built on `aws_s3_client`'s SDK
configuration rather than the CLI, because the CLI's checksum defaults are
not the client's.

## What "cannot be overcome" would look like

If task 1 shows the SDK cannot send an explicit checksum without trailer
framing **and** S3 does not return its default CRC64NVME on PUT under
`WHEN_REQUIRED`, there is no route on S3 that keeps the request shape and
the round-trip count. Then this spec closes with Requirement 3.4/3.5:
keep the escape hatch, make the error name the encryption mode, record the
evidence. Relaxing the framing pin to get route A would be a separate
decision for Clark, because the pin exists to keep the wire format of
every write independent of SDK defaults.

## Open questions

1. Does the AWS SDK version pinned by the vcpkg baseline expose
   `GetChecksumCRC64NVME()` on `PutObjectResult`? It is needed for route C
   only. Task 1 answers it from the installed headers.
2. Should the digest also be checked on GET (`ChecksumMode = ENABLED`)?
   Out of scope here, because the engine's recovery path already rejects
   objects that do not parse, but it is the natural next step and costs no
   extra request.
