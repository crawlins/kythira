# Spike notes — cloud key-object persistence (task 0)

Findings recorded in the OCI spec's CONFIRMED / CORRECTED / WAS format. Every
correction is folded back into requirements.md and design.md **in place**;
this file is the evidence trail, not a second source of truth.

## Status

| Sub-task | Provider coverage | State |
|---|---|---|
| 0.1 list-after-write, empirically | **all five: CLOSED** (live, 3 × 25 objects each) | — |
| 0.2 OCI durability-on-response wording | — | OPEN |
| 0.3 conditional-write matrix, live | **all five: CLOSED** (live; each decided a design outcome) | — |
| 0.4 single-PUT size limits | — | OPEN (documentation-only so far) |
| 0.5 checksum spelling / ETag determinism | **all five: CLOSED** (live) | — |
| 0.6 Azure REST-vs-SDK checkpoint | **CLOSED** — the recorded REST decision holds (Finding 11) | — |
| 0.7 GCS mock-tier decision | the **fidelity bar** is now measured (Finding 12) | OPEN — needs `fake-gcs-server` run, and this host has no container runtime |
| 0.8 OCI namespace and endpoint | **CLOSED** — endpoint confirmed, namespace is both configurable and resolvable via `GET /n/` (Finding 13) | — |

**Three of the eight sub-tasks remain**, and each is a different kind of work:
**0.2** is a documentation search (and is allowed to conclude that the wording
does not exist), **0.4** is a per-provider limit table feeding
`max_object_bytes`'s default, and **0.7** needs a container runtime this
development host does not have. Every cell that required a live service is
closed: credentials work for all five providers and all five have a bucket
(Finding 8), and Findings 11-13 close Azure Blob, GCS and OCI the same way
Findings 1-6 and 10 closed Alibaba OSS and AWS S3. Finding 14 is the
side-by-side table the client tasks read from.

### Credentials on the development host, as of August 16, 2026

| Provider | Where | State |
|---|---|---|
| Alibaba | `~/.aliyun/config.json`, the CLI's current profile | works — RAM user `kythira-ci-user`, account `5633986662052576` |
| AWS | **the `personal` CLI profile** — `aws --profile personal` | works, account `827617851594`. `default` and `clark` are expired (`InvalidClientTokenId`), which is what made AWS look unreachable |
| OCI | default CLI config | works — namespace `axunmw4f0mln` |
| Azure | `az` login | works — subscription `65845058-136c-4846-ae74-6b45808544f4`. Management plane only: the signed-in Owner had **no blob-data access** until a role was assigned (Finding 9) |
| GCP | `gcloud` | **the user credential is expired** (`Reauthentication failed`, and this host is non-interactive so `gcloud auth login` cannot run). **Application-default credentials still work**, and `CLOUDSDK_AUTH_ACCESS_TOKEN="$(gcloud auth application-default print-access-token)"` makes the whole CLI usable — that is how the GCS bucket was provisioned. Project `prefab-sky-500619-s9` |

**One caveat on the AWS profile, and it is a provisioning input rather than a
nit:** `aws --profile personal sts get-caller-identity` returns
`arn:aws:iam::827617851594:root` — the account root, not an IAM user or role.
That is workable for a read-only spike and makes the S3 cells (the 412-vs-409
split especially) reachable today. It is **not** what task 17's least-privilege
provisioning should use or assume, and it should not be wired into anything
that outlives the spike; a scoped IAM principal is the thing to create before
any of this repeats.

### How these were measured

`scripts/object-store-probes/` — a standalone OSS V4 (`OSS4-HMAC-SHA256`)
signer written from the canonical form documented in
`include/raft/alibaba_oss_client.hpp`, deliberately **not** calling the C++
client. Two reasons: the client cannot send conditional headers yet (that is
what this spike decides), and an independent implementation is a second
witness to the signing scheme. It authenticated first try — PUT/GET/LIST/DELETE
all 2xx — which is itself a confirmation of the documented canonical form.

Run August 16, 2026 against bucket `kythira-ci-5633986662052576`,
`ap-southeast-1`, under a throwaway `spike/objstore-cond-<epoch>/` prefix.
85 objects created, 85 deleted, 0 residual. Re-run from the promoted location
the same day, reproducing every finding below verdict for verdict — the
verdicts are now derived from status codes by
`classify_precondition()` rather than from whether the bytes changed, which is
Finding 1's lesson encoded in the tool.

---

## Finding 1 — Alibaba OSS has NO overwrite compare-and-swap. Requirement 9.8 fires.

**WAS (design.md, conditional-write table):** "Overwrite CAS — **OPEN — not
confirmed.** `If-Match`/`If-None-Match` are not listed among PutObject's
supported headers."

**CORRECTED to: not supported, and refused outright.**

```
PUT <key> If-Match: <the object's CURRENT etag>   → HTTP 400  NotImplemented
PUT <key> If-Match: <a STALE etag>                → HTTP 400  NotImplemented
PUT <key> If-None-Match: *                        → HTTP 400  NotImplemented
```

Both the legitimate conditional overwrite and the losing one are rejected the
same way, so this is not a fence that fails open — it is an API that does not
take the header at all. There is no ETag-predicated write on OSS PutObject.

**Consequence, which is a design outcome and not an observation:**
Requirement 9.8 applies exactly as written. `alibaba_oss_client` will satisfy
`key_object_store` and **not** `conditional_key_object_store`, so
`fencing_mode::compare_and_swap` becomes a **compile error** for the OSS
engine rather than a runtime degradation. The provider this spec's reference
implementation came from is the provider that cannot fence — the case the
spec's Notes section named in advance and forbade working around.

**Methodological trap, worth more than the finding:** the probe's own verdict
line asked "did the stale write change the bytes?", saw that it had not, and
printed *"REJECTED (CAS works)"*. That inference was wrong — the write was
refused because the header is unimplemented, not because a precondition was
evaluated. A live probe whose success criterion is "the object did not change"
**cannot tell a working fence from a rejected request**, and would have
recorded a fence that does not exist. Read the status code, not the effect.

## Finding 2 — Create-only works, and its rejection is **409**, not 412

```
PUT <absent key>   x-oss-forbid-overwrite: true   → HTTP 200
PUT <existing key> x-oss-forbid-overwrite: true   → HTTP 409  FileAlreadyExists
                                                    (and the body did NOT change)
```

CONFIRMED live, including the negative control that the refused PUT left the
existing object intact.

**The cross-provider trap this creates, and it is a sharp one:** S3 uses
**409 `ConditionalRequestConflict`** for a *benign* race that the caller should
**retry**, while OSS uses **409 `FileAlreadyExists`** for the definitive "you
lost, another writer got there first". A client layer that mapped "409 →
retryable" generically would turn OSS's fence into a retry loop that
eventually overwrites; one that mapped "409 → precondition failed" generically
would latch a healthy S3 engine permanently. **Status code alone is not
sufficient on either provider — the error code must be read.** design.md's
consequence 1 said this about 412-vs-409 within S3; it is now also true
*across* S3 and OSS, with the same number meaning opposite things.

## Finding 3 — OSS conditional DELETE is **silently ignored**, which is the worst shape

```
DELETE <key> If-Match: <a STALE etag>   → HTTP 204   … and the object was GONE
GET <key> afterwards                    → HTTP 404  NoSuchKey
```

Not a 400 like the unsupported PUT preconditions, and not a 412: the header is
accepted, disregarded, and the delete proceeds. A client that predicated a
delete on a version here would believe it held a guarantee it does not hold.

This costs the design nothing — `<prefix>/snapshot`, `<prefix>/snapshots/` and
**every DELETE are already unconditional by decision** (Requirement 9.3's
stated residual). But it upgrades that decision's justification from "buys
little" to "is not available on at least one provider", and it is a standing
argument for never adding a conditional delete to the concept without a
per-provider live negative control.

## Finding 4 — Content-MD5 is verified end to end, and the ETag is deterministic

```
PUT with a correct Content-MD5    → HTTP 200
PUT with a wrong  Content-MD5     → HTTP 400  InvalidDigest
ETag == uppercase MD5 hex of the single-part content  →  true
```

Both halves of Requirement 7 are therefore available on OSS: the service
rejects a corrupted upload (7.1), **and** the returned ETag can be verified
against a locally computed MD5 (7.2). The ETag arrives quoted
(`"5D41402ABC4B2A76B9719D911017C592"`); the quotes are part of the opaque
token and are carried through verbatim.

## Finding 5 — List-after-write on OSS: 3 × 25 objects, immediate LIST, complete every time

```
round 0: 25 PUTs, LIST immediately → 25 keys, IsTruncated=false
round 1: 25 PUTs, LIST immediately → 25 keys, IsTruncated=false
round 2: 25 PUTs, LIST immediately → 25 keys, IsTruncated=false
```

The N3 cell for OSS moves from **OPEN** to **empirically consistent**, which is
deliberately weaker language than "guaranteed": three rounds of 25 objects in
one region on one day is evidence that the listing is not lagging by a
detectable window, not a vendor guarantee that it never will. The vendor still
publishes no listing-specific consistency statement — that half of the cell
stays open, and the honest table entry says both things.

## Finding 6 — The CI bucket has versioning **unset**, so the forbid-overwrite caveat is dormant, not absent

`GET ?versioning` returns 200 with **no** `<Status>` element — versioning has
never been enabled on `kythira-ci-5633986662052576`. The documented hazard
(`x-oss-forbid-overwrite` is *silently ignored when bucket versioning is
enabled or suspended*) therefore did not affect any measurement above.

It remains an **operator-documentation obligation**: the one precondition OSS
does support can be silently disabled by a bucket setting the engine does not
control and cannot see without an extra request. This is the concrete instance
of the design's "no dependence on provider-native versioning" non-goal.

## Finding 7 — The hoisted engine passed the real suite live (task 19, Alibaba half)

`alibaba_oss_persistence_real_test`, all 4 cases green against the live bucket
on August 16, 2026, **after** tasks 2-3 rebuilt the engine as
`object_store_persistence_engine<alibaba_oss_client>`:

| Case | Wall time |
|---|---|
| `term_and_vote_survive_a_fresh_engine` | 8.6 s |
| `log_entries_round_trip_in_index_order` (12 appends + reload) | 29.5 s |
| `truncate_removes_objects_for_real` | 14.5 s |
| `binary_command_survives_the_real_service` | 5.2 s |

So the live-verified claim now belongs to the **generic** engine over the OSS
client, which is the whole reason the hoist was acceptable.

**These are case wall times, not per-operation latency**, and they include
fixture setup and cleanup DELETEs. They are consistent with the earlier
pre-hoist run (9.6 s for term+vote, ~34 s for 12 appends) — cross-ocean from a
developer machine to `ap-southeast-1`, an upper bound dominated by geography.
Requirement 4.5's measured p50/p99 is still owed, and is task 16's job; nothing
here may be quoted as a production number.

## Finding 8 — The three missing buckets are provisioned (task 17, bucket half)

August 16, 2026. Idempotent scripts, each run twice and each verified with a
real write/read/delete round trip under `kythira-real-test/` — the prefix the
shipped Alibaba real suite already namespaces by, so one lifecycle rule per
bucket covers every provider's tests.

| Provider | Resource | Region | Settings |
|---|---|---|---|
| AWS S3 | `kythira-ci-827617851594` | `us-east-1` | public access blocked (all four), SSE-S3 + bucket key, lifecycle expiring `kythira-real-test/` at 7 days |
| Azure Blob | account `kythirarealtestobj`, container `kythira-raft` (rg `kythira-realtest-rg`) | `eastus` | **Standard_ZRS**, TLS 1.2 min, HTTPS only, public blob access off |
| GCS | `kythira-ci-prefab-sky-500619-s9` | `us-central1` | uniform bucket-level access, public access prevention enforced, **soft delete off**, lifecycle expiring `kythira-real-test/` at 7 days |

OCI reuses `kythira-ci-artifacts` and Alibaba reuses
`kythira-ci-5633986662052576`, as the spec planned — so all five providers now
have somewhere to write.

**Azure's ZRS is a durability decision, not a default.** It is the only
redundancy mode Microsoft documents as writing synchronously to all three zone
replicas before returning success, which is what the engine's
fsync-equivalence argument needs from design.md's N1 row. LRS is
single-datacenter; GRS's cross-region copy is asynchronous, so a 2xx there
means durable in the primary region only.

**GCS soft delete is switched off deliberately.** The 7-day default bills
deleted objects for a week — on a bucket whose whole workload is
create-and-delete test objects that is the dominant cost, and it is invisible
in a listing. It also keeps the bucket honest about the design's
"no dependence on provider-native versioning or soft-delete" non-goal.

**What is deliberately NOT done**, and is the rest of task 17: the
least-privilege grants for each CI identity (an S3 bundle on the AWS CI role
scoped to the bucket and prefix; `Storage Blob Data Contributor` for
`AZURE_CI_CLIENT_ID`; a bucket-scoped `roles/storage.objectAdmin` for
`GCP_CI_SERVICE_ACCOUNT`), the repository variables, and the
`REAL_CLOUD_TESTS_*_OBJECT_PERSISTENCE_ENABLED` toggles. No suite exists to
enable yet.

## Finding 9 — Two provisioning traps, both of which report something misleading

**Azure: a subscription Owner can create a storage account and a container and
still not write a single blob.** Containers are management-plane resources, so
`az storage container create --auth-mode login` **succeeded** while every
blob operation returned "You do not have the required permissions". Container
creation is therefore *not* evidence that anything works. The fix is a
`Storage Blob Data Contributor` role assignment scoped to the account, which
took **~45 seconds** to become effective; the provisioning script now proves
the data plane with a real write/read/delete probe instead of inferring it,
and `--grant-caller-data-role` performs the assignment.

This one nearly escaped: the first smoke test piped `az` into `tail` and
checked `&&`, so it read **`tail`'s** exit status and printed "uploaded" for a
command that had failed. Pipelines hide the exit status of everything but the
last stage — the same shape as Finding 1's effect-versus-status-code trap, in
a different costume.

**Azure: an unregistered `Microsoft.Storage` provider reports
`SubscriptionNotFound`.** A subscription that has never held a storage account
fails `az storage account check-name` with a message that reads like the
subscription itself is gone. Registration is one-time and free and took ~60 s;
the script now checks and waits.

## Finding 10 — AWS S3: every conditional primitive works, and 409 did not appear

Run August 16, 2026 against `kythira-ci-827617851594` (`us-east-1`) with
`scripts/object-store-probes/probe_aws_s3.py` — a SigV4 sibling of the OSS
probe, written for the same reason: the decisive question is a **status code**
distinction that an SDK reports as an exception message.

```
PUT <absent>   If-None-Match: *              → HTTP 200
PUT <existing> If-None-Match: *              → HTTP 412  PreconditionFailed   (object unchanged)
PUT If-Match: <current etag>                 → HTTP 200
PUT If-Match: <stale etag>                   → HTTP 412  PreconditionFailed
DELETE If-Match: <stale etag>                → HTTP 412  PreconditionFailed   (object SURVIVED)
DELETE If-Match: <current etag>              → HTTP 204
PUT with a wrong Content-MD5                 → HTTP 400  BadDigest
ETag == md5 hex of single-part content       → true (lowercase; OSS returns uppercase)
list-after-write, 3 × 25 objects             → 25 keys every round
```

**S3 satisfies `conditional_key_object_store` in full**, including the
**conditional delete** that design.md had marked OPEN — a stale `If-Match`
DELETE is refused *and the object survives*, which is the exact negative
control Alibaba failed (Finding 3). S3 is therefore the first provider that
could carry `fencing_mode::compare_and_swap`.

### The 409 cell: documented, and NOT elicited

The probe raced concurrent create-only PUTs at one fresh key — 8, then 16,
then 32 at a time. Every round produced **exactly one 200 and every loser
412 `PreconditionFailed`**: 53 losers, **zero 409 `ConditionalRequestConflict`**.

This is evidence about what a *loser* normally gets, and it is **not** evidence
that 409 cannot happen. AWS documents 409 for two conditional requests racing,
and 56 requests from one host is a thin sample of a distributed
coordination path. So the design's rule stands unchanged and now rests
explicitly on documentation rather than on this run: **the S3 client maps 412
to `object_precondition_failed` and 409 to a retryable exception, separately**,
and a unit case pins it. Recording "not observed" as "does not occur" is
exactly how a rare-but-real path becomes an unrecoverable latch in production.

**Cross-provider spellings now differ in three places**, none of which the
engine sees because `object_version` is opaque — but every one of which a
client must get right: rejection code (S3 **412 PreconditionFailed** vs OSS
**409 FileAlreadyExists**), checksum-mismatch code (S3 **BadDigest** vs OSS
**InvalidDigest**), and ETag case (S3 lowercase, OSS uppercase — so a client
that verifies an ETag against a locally computed MD5 must compare
case-insensitively).

### The classifier trap, third instance — this time in the fix for it

`classify_precondition()` exists because the first OSS run inferred "CAS
works" from the bytes not changing. Its S3 debut then printed
**"SILENTLY IGNORED"** for a write that had plainly succeeded: the call site
passed the *post-write* content as "what would be there if the write did not
happen", so the helper compared the landed bytes against themselves. The
verdict line — which keys only on status codes — was right throughout; the
per-write label was wrong.

The parameter is now named `bytes_if_write_did_not_happen`, both call sites
pass the previous content, and the correct-ETag write reads `ACCEPTED (200)`.
The lesson is not "be careful": it is that **a check whose inputs can be
transposed without a type error will eventually be transposed**, and that the
same trap recurred three times in one spike — once in prose, once in a
verdict, once in the helper written to prevent it.

## Finding 11 — Azure Blob: fenceable, and its create-only rejection is 409

Run August 17, 2026 against `kythirarealtestobj`/`kythira-raft` (`eastus`,
Standard_ZRS) with `scripts/object-store-probes/probe_azure_blob.py` — Blob
REST over an AAD bearer token, `x-ms-version: 2023-11-03`,
`x-ms-blob-type: BlockBlob`.

```
PUT <absent>   If-None-Match: *              → HTTP 201
PUT <existing> If-None-Match: *              → HTTP 409  BlobAlreadyExists   (blob unchanged)
PUT If-Match: <current etag>                 → HTTP 201
PUT If-Match: <stale etag>                   → HTTP 412  ConditionNotMet
DELETE If-Match: <stale etag>                → HTTP 412  ConditionNotMet     (blob SURVIVED)
DELETE If-Match: <current etag>              → HTTP 202
PUT with a wrong Content-MD5                 → HTTP 400  Md5Mismatch
ETag == md5 hex of content                   → FALSE — the ETag is an opaque
                                               timestamp token (0x8DEFC5FC0A7294B)
GET returns Content-MD5 matching the content → true
list-after-write, 3 × 25 blobs               → 25 blobs every round
racing create-only, 8/16/32                  → one 201 per round, 53 losers,
                                               every one 409 BlobAlreadyExists
```

**Azure satisfies `conditional_key_object_store` in full**, conditional delete
included — the second provider that can carry `fencing_mode::compare_and_swap`.

**One provider now answers with two different codes for two different
preconditions**, which no earlier provider did: **409 `BlobAlreadyExists` for
`If-None-Match: *`** and **412 `ConditionNotMet` for `If-Match`**. Both mean
"you lost" and both must latch the fence. Set against S3's **409
`ConditionalRequestConflict`**, which means "benign race, retry", the rule the
OSS run produced gets stronger rather than merely repeated: **the status alone
is never sufficient — a client must read the error code**, and on Azure it must
map *both* statuses to `object_precondition_failed`.

**Requirement 7's two halves come apart here.** The service verifies
`Content-MD5` end to end (fourth distinct mismatch spelling: `BadDigest`,
`InvalidDigest`, `Md5Mismatch`), but the **ETag is not a function of the
content**, so the local ETag check that S3 and OSS allow is unavailable. What
Azure does return is `Content-MD5` on GET, matching the value sent, so local
verification is still possible — from a different header. A client that assumed
the S3 shape here would compare an opaque token against an MD5 and fail every
write.

**Task 0.6 (REST vs SDK) is closed in favour of the recorded decision.** This
probe *is* the hand-rolled surface, and nothing about it was harder than
documented: bearer token from the CLI's own resolver, one dated `x-ms-version`,
`x-ms-blob-type: BlockBlob` on PUT, `?restype=container&comp=list&prefix=` for
listing with an empty `NextMarker` at 25 blobs. Three details the client header
must record, because each is a plausible wrong guess:

- **success is 201 on PUT and 202 on DELETE**, not 200 — a client that treats
  only 200 as success fails every write;
- **the machine-readable error code is the `x-ms-error-code` response header**,
  present on every error, with the XML body as a fallback;
- a PUT with no body still needs an explicit `Content-Length: 0`.

No `azure-storage-blobs-cpp` dependency is needed.

## Finding 12 — GCS: fenceable via generations, and its version is a counter

Run August 17, 2026 against `kythira-ci-prefab-sky-500619-s9` (`us-central1`)
with `scripts/object-store-probes/probe_gcs.py`, deliberately against the
**JSON API** — the surface `google-cloud-cpp`'s storage client speaks, since a
cell closed against the XML API of the same service would be closed by analogy.

```
upload <absent>   ifGenerationMatch=0        → HTTP 200
upload <existing> ifGenerationMatch=0        → HTTP 412  conditionNotMet   (object unchanged)
upload ifGenerationMatch=<current>           → HTTP 200
upload ifGenerationMatch=<stale>             → HTTP 412  conditionNotMet
DELETE ifGenerationMatch=<stale>             → HTTP 412  conditionNotMet   (object SURVIVED)
DELETE ifGenerationMatch=<current>           → HTTP 204
upload with a wrong md5Hash in metadata      → HTTP 400  invalid
returned md5Hash == md5 of content           → true
etag == md5 hex of content                   → FALSE (etag is 'CKzagZTdp5YDEAE=')
list-after-write, 3 × 25 objects             → 25 objects every round
racing create-only, 8/16/32                  → one 200 per round, 53 losers,
                                               every one 412 conditionNotMet
```

**GCS satisfies `conditional_key_object_store` in full**, conditional delete
included — the third fenceable provider.

**The generation is a decimal counter, not a hash** (`1786971917733520` →
`1786971917901538`), and it travels as a **query parameter**, not a header.
This is the concrete justification for `object_version` being an opaque string
the engine never inspects: an engine that assumed "quoted hash in a header"
would be wrong here, and one that assumed "decimal counter" would be wrong
everywhere else.

**Requirement 7 again comes apart, and differently from Azure.** The checksum
is not a request header at all — it is `md5Hash` in the object *metadata*, sent
as the JSON part of a multipart upload (the shape `MD5HashValue` produces in
`google-cloud-cpp`), and a mismatch is `400 invalid`, a fifth spelling and a
notably uninformative one. The ETag is opaque, but the response's `md5Hash`
field is exactly the content MD5, so local verification is available from the
metadata rather than from the ETag.

**Task 0.7 (GCS mock tier) is NOT closed by this run** and is deliberately not
guessed at: `fake-gcs-server`'s fidelity on generation preconditions cannot be
assessed without running it, and this host has no container runtime. What this
run does contribute is the **fidelity bar** the emulator must clear, now stated
as observations rather than expectations: `ifGenerationMatch=0` on an existing
object must be `412 conditionNotMet` and must leave the object unchanged; a
stale `ifGenerationMatch` must refuse both an upload and a delete; and the
generation must advance on every write.

## Finding 13 — OCI: fenceable, conditional delete confirmed, and the ETag is a UUID

Run August 17, 2026 against `kythira-ci-artifacts` (`us-phoenix-1`,
namespace `axunmw4f0mln`) with `scripts/object-store-probes/probe_oci.py`,
whose request signatures are written from the canonical form documented in
`include/raft/oci_signing.hpp` — an independent second witness to that form,
which authenticated on its first live call.

```
PUT <absent>   if-none-match: *              → HTTP 200
PUT <existing> if-none-match: *              → HTTP 412  IfNoneMatchFailed  (object unchanged)
PUT if-match: <current etag>                 → HTTP 200
PUT if-match: <stale etag>                   → HTTP 412  IfMatchFailed
DELETE if-match: <stale etag>                → HTTP 412  IfMatchFailed      (object SURVIVED)
DELETE if-match: <current etag>              → HTTP 204
PUT with a wrong Content-MD5                 → HTTP 400  UnmatchedContentMD5
ETag == md5 hex of content                   → FALSE — the ETag is a UUID
                                               (9a86a492-43fa-4c8c-a80e-dbd650188ff8)
GET returns opc-content-md5 matching content → true
list-after-write, 3 × 25 objects             → 25 objects every round
racing create-only, 8/16/32                  → one 200 per round, 52 losers,
                                               every one 412 IfNoneMatchFailed
```

**OCI satisfies `conditional_key_object_store` in full**, and this closes the
**conditional-delete cell design.md recorded as unconfirmed** (task 0.3): a
stale `if-match` DELETE is refused *and the object survives*. Four of the five
providers can be fenced; **only Alibaba OSS cannot** (Finding 1).

**Task 0.8 is closed.** The endpoint is
`https://objectstorage.<region>.oraclecloud.com` and the namespace is **both**
configurable and resolvable: `GET /n/` returns it as a bare JSON string
(`"axunmw4f0mln"`). So `oci_object_storage_client` should accept a configured
namespace and resolve-and-cache only when none is given — one call at
construction, never per request.

**A signing divergence the client must handle, found by writing the signer:**
`oci_signing.hpp` signs `content-type: application/json` as a **constant**,
because every existing caller is a control-plane API. Object Storage is a data
plane and its bodies are opaque bytes. The signed content type must therefore
become a parameter as part of task 10's raw-bytes request path — a client that
sends `application/octet-stream` while signing `application/json` gets a 401
that says nothing about content types.

### Two probe defects this run exposed, both caught by controls rather than by luck

- **`headers.get("ETag")` returned nothing, because OCI spells it `etag`.** The
  probe then sent `if-match: ""`, the service answered `412 IfMatchFailed` to a
  *correct*-ETag write, and the run read exactly like "OCI cannot do CAS" —
  the same conclusion Alibaba genuinely earned. What caught it was the
  **negative control**: `cas_verdict()` refuses to call CAS usable unless a
  correct precondition is *accepted*, so the run printed "NOT USABLE — even a
  correct precondition was refused" instead of a false finding. Response
  headers are now case-insensitive (`probe_common.Headers`) for every provider.
- **The checksum verdict trusted any 4xx.** On the run before the fix, the
  tenancy declined the wrong-MD5 PUT with `404 BucketNotFound` and the probe
  printed *"VERIFIED by the service (BucketNotFound)"*. The verdict now keys on
  a **checksum-specific error code** and otherwise reports UNDECIDED — which is
  how the real code, `UnmatchedContentMD5`, came to be recorded rather than
  assumed. This is the effect-versus-status-code trap one level up: a status
  class is not a reason.

### An environment observation, recorded as such and NOT as a cell

This tenancy **intermittently declines otherwise valid requests with
`404 BucketNotFound`** ("...does not exist **or you are not authorized**"),
against a bucket that plainly exists, with surrounding requests in the same
second succeeding. Measured at roughly 3–16% depending on the moment, across
every request shape — PUT, GET, DELETE and LIST, body-carrying or not — so it
is not the signed body headers. Pacing requests one second apart reduced but
did not remove it (1/25). The official `oci` CLI went 40/40 in one window,
which at a 3% rate is unsurprising and therefore **not** evidence that the CLI
is immune; the comparison is inconclusive rather than exculpatory.

It resembles the previously recorded case where a policy `where` clause
declined a *different* principal's Object Storage requests and presented as
`BucketNotFound`, and it deserves its own investigation with an `opc-request-id`
(e.g. `phx-1:5mJ4Le_9BVx5HBWhO1DZ2xS7M7ICsz1rZEOu0dZ4WrJfRwvQqJJz42MdkV6MTtS0`)
against the compartment's audit log. It is **not** an object-storage
consistency or conditional-write finding, and no cell above rests on a request
that hit it: measurement requests are re-issued once past a decline, every
re-issue prints, and the count is reported at the end of the run (0 on the run
quoted above). Two consequences worth carrying forward: **a real-tier OCI suite
will need this understood before it can distinguish a flake from a regression**,
and **the list-after-write comparison must be listing-versus-acknowledged-
writes**, never listing-versus-25 — the earlier run's "0 of 25 listed" was ten
declined PUTs, and reading it as a lagging listing would have invented a
consistency defect out of an authorization flake.

## Finding 14 — What the five providers now look like side by side

Every cell below is from a live run, none by analogy. The table is the input to
tasks 5-10, and the differences are the point: no two providers agree on all
four columns.

| | create-only rejection | overwrite CAS | conditional DELETE | checksum mismatch | version token |
|---|---|---|---|---|---|
| AWS S3 | 412 `PreconditionFailed` | **yes** | **yes** | `BadDigest` | ETag = md5 hex (lowercase) |
| Azure Blob | **409** `BlobAlreadyExists` | **yes** (412 `ConditionNotMet`) | **yes** | `Md5Mismatch` | opaque timestamp token |
| GCS | 412 `conditionNotMet` | **yes** | **yes** | `invalid` | **decimal generation** (query param) |
| OCI | 412 `IfNoneMatchFailed` | **yes** | **yes** | `UnmatchedContentMD5` | ETag = **UUID** |
| Alibaba OSS | **409** `FileAlreadyExists` | **no — `400 NotImplemented`** | **silently ignored** | `InvalidDigest` | ETag = md5 hex (uppercase) |

Read across the rows, three design consequences follow directly:

1. **Four providers can carry `fencing_mode::compare_and_swap`; OSS cannot.**
   Requirement 9.8's compile-time unavailability applies to exactly one
   provider, which is the outcome the plan anticipated and forbade working
   around.
2. **`object_version` must stay opaque.** It is an MD5 hex on two providers, a
   UUID on one, a timestamp token on one and a decimal counter carried as a
   query parameter on the last.
3. **Requirement 7's local ETag check applies to only two providers.** On Azure,
   GCS and OCI the ETag is not a function of the content, and local verification
   must read `Content-MD5` / `md5Hash` / `opc-content-md5` instead. The
   end-to-end service-side check, by contrast, is available on **all five** —
   in five different spellings, none of which a client may guess.
