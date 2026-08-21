# Spike notes — cloud key-object persistence (task 0)

Findings recorded in the OCI spec's CONFIRMED / CORRECTED / WAS format. Every
correction is folded back into requirements.md and design.md **in place**;
this file is the evidence trail, not a second source of truth.

## Status

| Sub-task | Provider coverage | State |
|---|---|---|
| 0.1 list-after-write, empirically | **all five: CLOSED** (live, 3 × 25 objects each) | — |
| 0.2 OCI durability-on-response wording | **CLOSED as "no such wording exists"** (Finding 15) — searched and not found, which is the answer Requirement 4.2 asks for | — |
| 0.3 conditional-write matrix, live | **all five: CLOSED** (live; each decided a design outcome) | — |
| 0.4 single-PUT size limits | **CLOSED** — all five documented; smallest is 5 GB (S3, OSS), and the recommended default is far below it for engine-shape reasons (Finding 16) | — |
| 0.5 checksum spelling / ETag determinism | **all five: CLOSED** (live) | — |
| 0.6 Azure REST-vs-SDK checkpoint | **CLOSED** — the recorded REST decision holds (Finding 11) | — |
| 0.7 GCS mock-tier decision | the **fidelity bar** is now measured (Finding 12) | OPEN — needs `fake-gcs-server` run, and this host has no container runtime |
| 0.8 OCI namespace and endpoint | **CLOSED** — endpoint confirmed, namespace is both configurable and resolvable via `GET /n/` (Finding 13) | — |

**One sub-task remains: 0.7**, which needs a container runtime this development
host does not have — `fake-gcs-server`'s fidelity on generation preconditions
cannot be assessed without running it, and the fidelity bar it must clear is
recorded in Finding 12. **0.2 closed with a negative result** (the wording does
not exist; Finding 15) and **0.4 closed from documentation** (Finding 16).
Every cell that required a live service is closed: credentials work for all five providers and all five have a bucket
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

## Finding 15 — OCI publishes no "durable before the response" wording (task 0.2)

**The answer is that the sentence does not exist**, and Requirement 4.2's
honesty rule says to write that down rather than let the four confirmed
providers' wording stand in for it.

Searched August 17, 2026: the Object Storage overview
(`docs.oracle.com/en-us/iaas/Content/Object/Concepts/objectstorageoverview.htm`),
the object-management task pages, the REST API reference for `PutObject`, and
the Python/Java SDK reference for `put_object`. What Oracle does state:

> "Data is stored redundantly across multiple storage servers. Object Storage
> actively monitors data integrity using checksums and automatically detects
> and repairs corrupt data. … If a redundancy loss is detected, Object Storage
> automatically creates more data copies."

and, separately,

> "**Strong consistency**: When a read request is made, Object Storage always
> serves the most recent copy of the data that was written to the system."

Both are about the steady state and about *reads*. **Neither says when the
redundancy exists relative to the PUT response**, which is precisely the claim
this engine's durability contract rests on — "the method returned" implying
"the bytes survive the loss of this instance". S3 says it
("if you receive a success response, Amazon S3 added the entire object"), GCS
says it, Azure says it per redundancy mode, OSS implies it; OCI says the
adjacent things and not this one.

**Consequences, and they are deliberately unexciting:**

- The N1 cell for OCI stays **OPEN in the durability table**, now with "searched
  and not found, August 17, 2026" rather than "not yet searched". A cell that
  has been looked for and not found is a different and more useful state than
  an unexamined one.
- `doc/cloud_object_persistence.md` must carry that row with the gap visible
  (Requirement 4.2). An operator choosing OCI for a Raft log is entitled to know
  that the fsync-equivalence argument is weaker there than on the other four —
  not because OCI is less durable, but because Oracle has not published the
  sentence the argument needs.
- This is **not** a blocker for `oci_object_storage_client`: the engine's
  behaviour is identical either way. It is a documentation obligation, and the
  honest version of it is "unstated by the vendor", not "presumed equivalent".

The remaining way to close it is a support ticket asking Oracle to state the
write-path durability boundary. That is recorded as the next step rather than
attempted here, because a support answer that is not published documentation is
evidence of a different kind and should be labelled as such if it arrives.

## Finding 16 — Single-PUT size limits, and why the default cap is far below the smallest

Task 0.4, from primary documentation, August 17, 2026:

| Provider | Documented single-request limit | On exceeding it |
|---|---|---|
| **AWS S3** | **5 GB** in one `PutObject` (multipart 5 MB-5 TB); AWS advises multipart above **100 MB** | request rejected |
| **Azure Blob** | **5,000 MiB** via a single `Put Blob`, for `x-ms-version` **2019-12-12 and later** (256 MiB for 2016-05-31 through 2019-07-07; 64 MiB before that) | **413 Request Entity Too Large**, with the permitted maximum in the body |
| **GCS** | **5 TiB** — the object-size limit; single-request upload has no smaller documented cap | request rejected |
| **OCI Object Storage** | **50 GiB** via `PutObject` | request rejected |
| **Alibaba OSS** | **5 GB** via `PutObject` (multipart to 48.8 TB) | request rejected |

**The smallest documented limit is 5 GB (S3 and OSS).** Azure's is version-
dependent, which is a second reason its client pins `x-ms-version` to a dated
constant: a client that let the version float could silently drop from 5,000
MiB to 64 MiB.

**But the default `max_object_bytes` should not be 5 GB, and Requirement 7.3 is
amended to say so.** The limit that binds here is not the service's — it is this
engine's own shape:

- the only latency this project has measured is **~2-3 s per object round
  trip** (cross-ocean, `.kiro/specs/alibaba-cloud-services/spike-notes.md`
  Finding 7), so a multi-gigabyte single PUT is an hours-long request;
- the engine has **exactly one retry and no multipart, no resumption and no
  progress reporting** (multipart is a documented non-goal), so a failure at
  the end of such a request re-sends the whole thing once and then throws;
- `save_snapshot` holds the engine's mutex for the entire round trip, so an
  oversized snapshot stalls every other persistence call behind it;
- AWS's own guidance is to stop using single-PUT above **100 MB**.

**Recommended default: 64 MiB**, with the cap configurable upward. That is
comfortably below every provider's limit *and* below AWS's multipart advice,
and it is far above any state-machine snapshot this repo produces. The point of
the cap is not to predict the service's 413 — it is to turn "this deployment
has outgrown a single-PUT persistence engine" into a loud error naming the
size, the cap and the multipart non-goal, at the first snapshot that reaches
it, instead of an hours-long write that may still fail.

## Finding 17 — GCS, second pass: what task 9 had to measure that Finding 12 did not

Run August 17, 2026 against `kythira-ci-prefab-sky-500619-s9`, partly with
`curl` against the JSON API and partly with a throwaway `google-cloud-cpp`
program. Finding 12 closed the conditional-write matrix; writing the client
turned up five more facts, and **three of them are library behaviour rather
than service behaviour** — which is exactly the sort of thing that is invisible
until something depends on it. Residual objects after every run: 0, counted
from a listing.

### 17.1 GCS gives an absent object and an absent bucket the *same* error code

```
GET  <real bucket>/<absent key>  → 404  reason=notFound
                                        "No such object: <bucket>/<key>"
GET  <absent bucket>/<any key>   → 404  reason=notFound
                                        "The specified bucket does not exist."
```

**This is the one place where this design's usual rule cannot be followed.**
S3 answers `NoSuchKey` vs `NoSuchBucket`; OCI answers `ObjectNotFound` vs
`BucketNotFound`; GCS answers `notFound` to both, and only the human-readable
message differs. So `gcp_gcs_client` keys absence on the message text, as a
**whitelist** — anything not recognisably an absent object throws. The failure
direction is what justifies it: reading a misconfigured bucket as "not written
yet" would show the engine an empty Raft log, which is the one failure it
cannot detect at runtime, whereas a reworded message makes absence *throw*.

### 17.2 DELETE of an absent object is 404, not 204

S3 and OSS answer 204. GCS answers `404 notFound`, so the idempotence the
engine's truncation path requires is supplied by the client, not the service —
and it is supplied through the same whitelist as 17.1, so a missing *bucket*
still throws instead of being reported as a successful delete.

### 17.3 `ifGenerationMatch=0` is accepted on a DELETE

```
DELETE ifGenerationMatch=0, object EXISTS  → 412 conditionNotMet
DELETE ifGenerationMatch=0, object ABSENT  → 404 notFound
```

Unlike S3, whose `DeleteObject` models `If-Match` only, "delete only if it does
not exist" has a wire spelling here. That makes the conditional delete's two
preconditions read a 404 **differently**: `if_absent` treats it as the
precondition holding, `if_version` treats it as having lost the race. Both are
in the suite, as one case, because it is one decision.

### 17.4 google-cloud-cpp *rewrites* the status message

The retry loop wraps it: what the service sends as `No such object: b/k`
reaches the caller as `Permanent error, with a last message of No such object:
b/k` (also "Retry policy exhausted, …" and "Retry loop cancelled, …"). The
original text is embedded verbatim in each form.

**Recorded because it invalidated a measurement that looked sound.** The
mapping from HTTP status to `StatusCode` was first probed by calling
`rest_internal::AsStatus` directly — correct as far as it went (404 → kNotFound,
409 → kAborted, 412 → kFailedPrecondition, 429/503 → kUnavailable, and *not* the
blanket `[400,500) → kInvalidArgument` the header comment describes) but taken
one layer below where the client actually reads. A prefix match written from
that probe recognised nothing, and six cases failed on the suite's first run.
Check that the repro reproduces the layer you are reasoning about.

### 17.5 A zero-byte object reports no generation, and no headers at all

```
insert 0 bytes → generation 1787006876920424
read   0 bytes → status ok, body 0 bytes, generation ABSENT, headers 0
insert 5 bytes → generation 1787006877042595
read   5 bytes → status ok, body 5 bytes, generation 1787006877042595, headers 22
```

With no bytes to stream, the library never captures the response, so
`ObjectReadStream::generation()` is empty. Entirely reasonable, and completely
invisible until a caller wants the version — which this one does, because the
version a fenced engine reads is what it later predicates a conditional write
on. `gcp_gcs_client` falls back to `GetObjectMetadata` on that path and checks
the returned `size` is still 0, so a version is never reported for bytes that
were not read. The five-byte leg is the negative control: without it, "GCS does
not report generations on reads" would have been a plausible and wrong reading.

### 17.6 `CLOUD_STORAGE_EMULATOR_ENDPOINT` overrides an explicit endpoint option

Measured with the variable pointing at one local port and
`storage::RestEndpointOption` at another: **every request went to the
variable's port.** So the obvious way to point a storage client at a local
test server would have made the whole unit suite exercise the environment
variable and never the `endpoint_override` field the client reads. The suite
therefore does not set it, and authenticates with a throwaway service account
whose RSA key is generated at run time — google-cloud-cpp turns that into a
locally-signed JWT with no token-endpoint round trip, so the tests need no
network and no key material is committed.

## Finding 18 — Task 16's real tier found two shipped OCI defects, each fatal on its own

Run August 18-19, 2026 against `kythira-ci-artifacts` (`us-phoenix-1`). Both
defects were in code that shipped with task 10, both made **OCI object
persistence completely non-functional against the real service**, and both were
structurally invisible to the 51 OCI unit cases that pass.

### 18.1 The endpoint suffix was wrong

`oci_http_client::domain_suffix_for` defaults unknown services to the newer
`.oci.oraclecloud.com` form. Object Storage needs the bare one:

```
objectstorage.us-phoenix-1.oraclecloud.com      → HTTP 401 (reachable, wants auth)
objectstorage.us-phoenix-1.oci.oraclecloud.com  → TLS hostname verification failure
```

Every request failed on the first connection. `objectstorage` now sits beside
`iaas` in the bare-form list.

**The default is the trap, not the list.** A service nobody names gets a
plausible host that may not exist, and no local test can tell — every unit case
sets `endpoint_override`, which replaces the host outright. That header's own
comment already said the mock tier cannot see the derivation; it was right.

### 18.2 A percent-encoded `/` in a query value breaks the signature

`encode_query_value` encoded `/` as `%2F`. OCI answers **401
NotAuthenticated** for that. Isolated by varying only the prefix, which is what
turned "LIST is broken" into a one-character cause:

```
prefix=            → 200, 52 keys
prefix=heartbeat   → 200, 4 keys
prefix=heartbeat/  → 401 NotAuthenticated
```

Every listing this engine performs uses a prefix ending in `/`, so **every
`list_keys` call failed** — the engine could not recover a log at all. `/` is a
legal query character under RFC 3986 §3.4 (`query = *( pchar / "/" / "?" )`),
so leaving it literal is correct rather than a workaround.

**Why the signature-verifying mocks could not catch it**, and this generalises:
those mocks verify the signature against the bytes that *arrived*, so the client
and the mock encode identically and always agree. A signature bug of this shape
is only observable against a party that computes the signature **independently**
— which is the entire argument for a real tier, stated more sharply than task 16
stated it.

An unrelated correctness fix was made while chasing this and is recorded as
such: `httplib::Client::set_url_encode(false)`, because the client signs an
already-encoded target and letting httplib re-encode it would break the same
invariant for the first prefix containing a space. It was **not** the cause of
this defect.

## Finding 19 — GCS rate-limits object mutations to ~1/second, per object

Measured while taking task 16's latency samples: 20 rapid `save_current_term`
writes to one key returned `429 rateLimitExceeded` ("exceeded the rate limit for
object mutation operations"). **S3 took the identical pattern without
throttling.**

This is an operational constraint on GCS specifically, not a test artifact. The
engine's `term`, `voted_for`, `snapshot` and `owner` are all **single-slot
keys**, so a node that advances terms rapidly — a partitioned node campaigning
repeatedly, say — can hit it on GCS and not on the other providers. The latency
case spaces its samples at 1100 ms for exactly this reason.

## Finding 20 — Measured write latency, all five providers

Client-side, around the engine call, so it includes TLS, signing, the round trip
and the engine's own bookkeeping — which is what a Raft node actually waits for.
**Not** service-side latency, and not comparable with a vendor's published
figures.

| provider | `save_current_term` p50 | `append_log_entry` p50 | list-after-write |
|---|---|---|---|
| GCS | 145 ms | 113 ms | 25/25 × 3 |
| S3 | 128 ms | 128 ms | 25/25 × 3 |
| OCI | 356 ms | 342 ms | 25/25 × 3 |
| Azure Blob | 348 ms | 365 ms | 25/25 × 3 |
| Alibaba OSS | 1648 ms | 1476 ms | 25/25 × 3 |

**Alibaba's figures are a distance measurement, not a provider one.** Its bucket
is in `ap-southeast-1` and every other provider's is in a US region, so the
~4-10x gap is the cross-ocean round trip this repo already measured at 2-3 s per
object (`alibaba-cloud-services/spike-notes.md` Finding 7). Comparing it against
the rows above as if it said something about OSS would be wrong.

`get_log_entry` measures **sub-microsecond** on every provider. That is a
memory-mirror hit, not a round trip, and reading it as a storage read latency
would be badly wrong — the engine answers reads from the mirror it maintains.

The list-after-write column closes task 0.1's cells empirically for all five:
25 objects written and immediately listed, three rounds, complete every time.

## Finding 21 — `fake-gcs-server` does not clear task 0.7's fidelity bar. Task 0.7 closes NO.

Measured August 19, 2026 against `fsouza/fake-gcs-server:latest` under rootless
Podman. Finding 12 deliberately recorded the bar as **observations of the real
service** rather than expectations, so this is a comparison and not a judgement
call:

| bar | result |
|---|---|
| generation advances on every write | **pass** |
| `ifGenerationMatch=0` on an existing object, **multipart** upload → 412 | **pass** |
| `ifGenerationMatch=0` on an existing object, **media** upload → 412 | **FAIL — 200, and it overwrites** |
| stale `ifGenerationMatch` on a **media** upload → 412 | **FAIL — 200, and it overwrites** |
| stale `ifGenerationMatch` on **DELETE** → 412, object survives | **FAIL — 200, and it DELETES** |
| 404 body distinguishes an absent object | **FAIL — `Not Found`, where GCS sends `No such object: <bucket>/<key>`** |

**The decision is no.** Not because two of six rows are amber but because of
what the failures *are*.

**The conditional DELETE gap is disqualifying on its own.** An unmodelled
precondition there does not fail loudly — it succeeds and destroys the object.
A fenced suite run against this emulator would go **green while the emulator
deleted objects a real bucket would have refused**. That is a false green
asserting the fence works when it was never exercised, which is worse than
having no tier at all.

**The 404 body gap forced the decision that settles it.** `gcp_gcs_client`
treats absence as a whitelist keyed on GCS's own message (Finding 17.1), because
GCS gives an absent object and an absent bucket the same code and reason. The
emulator's `Not Found` does not match, so absence throws. The fix that would
make the suite pass is to widen the whitelist — and that trades a **real-service
safety property** (a misconfigured bucket can never be read as an empty Raft
log) for emulator convenience. Refused. A test tier that requires loosening
production safety to go green is not paying for itself.

**What GCS gets instead.** The engine is proved over the in-memory substrates
and over the **real bucket** (`gcp_gcs_object_persistence_real_test`, 5/5 live
including fencing). The hand-written mock 0.7 named as the alternative is *not*
written: it would be a third encoding of the same wire format, and Finding 18
showed what a mock that agrees with its client is worth — the two OCI defects
were invisible to exactly that arrangement. GCS's evidence is the real tier.

Also measured, and it is why the multipart row passes: `gcp_gcs_client` sends
`MD5HashValue` on every insert, which puts google-cloud-cpp on the multipart
path. So the *upload* preconditions this engine actually issues would have been
modelled faithfully. It is delete and absence that are not.

## Finding 22 — The emulator tier does not land, for three independent reasons

Measured August 19, 2026 under rootless Podman 4.9.3. Task 15's mock tiers
(15.4 OCI, 15.5 Alibaba) are **done and green**; it is the three
*external-emulator* subtasks that do not, and each fails for its own reason
rather than for want of effort.

### 15.1 S3 → LocalStack: the free image was discontinued

```
docker.io/localstack/localstack:latest    → exit 55, "License activation failed"
docker.io/localstack/localstack:s3-latest → "This image was discontinued with the
                                             release of v2026.03 on 23rd March 2026.
                                             Please switch to localstack/localstack-pro."
```

LocalStack's community S3 image no longer exists; the replacement is licensed.
This is an upstream product change, not a technical obstacle, and no amount of
work here removes it. **A paid LocalStack licence would unblock 15.1
immediately** — the harness is the same shape as 15.4's and would be a short
change.

Note the existing `aws_quorum_manager_localstack_test` is affected by the same
change and will skip rather than run, since it probes for a reachable endpoint.
That is pre-existing and outside this spec.

### 15.2 Azure Blob → Azurite: an auth model this client deliberately does not have

Azurite authenticates with **SharedKey**. `azure_blob_client` speaks **AAD
bearer tokens only** — task 0.6 decided against `azure-storage-blobs-cpp`
precisely because the credential chain already produces bearer tokens, and
"no SharedKey signing is written at all" is a recorded design decision.

Azurite's `--oauth basic` mode does start over plain HTTP (contrary to its
older documentation), but rejected a hand-formed JWT with `403
AuthenticationFailed`, so its acceptance criteria are stricter than "any
well-formed token" and are not documented.

Two routes exist and both are refused:

- **Add SharedKey to `azure_blob_client`** — production code, security-relevant,
  written solely so an emulator can be talked to. That is the same trade
  Finding 21 refused for GCS and it is refused here for the same reason.
- **Reverse-engineer what Azurite's `--oauth basic` accepts** — undocumented,
  and a tier built on it breaks whenever the emulator tightens.

`azure_client_config` *does* accept an injected `TokenCredential`, so if
Azurite's OAuth requirements are ever documented this becomes cheap. Recorded
so the next attempt starts from what was measured rather than from scratch.

**One piece of scaffolding was written and then deleted rather than committed
unused**: a `counting_object_store` decorator, which a black-box emulator needs
because — unlike the mock tiers — there is no server-side request log to read
and the conformance suite counts requests. Its shape, for whoever picks this up:
wrap the store, record `"PUT <key>"`/`"GET <key>"`/`"DELETE <key>"`/`"LIST
<prefix>"`, share the counter through a `shared_ptr` so it survives the engine
copying its store, forward `put_object_if`/`delete_object_if` under a
`requires conditional_key_object_store<Store>` clause so a fenceable store is
not silently downgraded, and mirror `version_is_content_md5`. Note it counts
what the client *issued*, which is strictly weaker than the mock tiers'
server-side log.

The other thing that attempt learned: an emulator harness **must reset its
bucket per case**. The in-memory harness gets a fresh store per instance; an
external emulator does not, and its objects outlive the process. Without a reset
the second case inherits the first's state and the suite fails in ways that read
as engine bugs — `load_current_term() == 4` where 0 was expected.

### 15.3 GCS → fake-gcs-server: refused on fidelity (Finding 21)

Measured, decided no, and the reasoning is in Finding 21. The short form: an
unmodelled conditional DELETE **succeeds and destroys the object**, so a fenced
suite would go green having tested nothing.

### What this costs, stated plainly

The engine is proved over five substrates: the in-memory store (plain, fenced
and MD5-versioned), the **OCI mock server**, the **Alibaba OSS mock server**,
and — for all five providers — a **real bucket** (task 16). What the emulator
tier would have added is a middle rung: more realistic than a mock, cheaper than
a real bucket, runnable in CI without credentials.

Its absence is a genuine gap in CI economics, not in correctness evidence. The
real tier covers everything the emulators would have, and covers it better —
Finding 18's two OCI defects were invisible to every local tier and would have
been invisible to an emulator too.

## Finding 23 — Task 17's grants provisioned live, and a concrete lead on the OCI flake

August 19-20, 2026. Provisioning the five CI identities' object-persistence
grants surfaced two things worth recording: one about what the grants actually
needed, and one about the **open `404 BucketNotFound` flake** that has made
every OCI result weaker evidence than the other four providers'.

### Two of the five needed no grant at all, for opposite reasons

| Provider | Applied | Result |
|---|---|---|
| AWS | `provision-oidc-role.sh --bundles …,object-persistence --bucket kythira-ci-827617851594` | 2 statements added; **verified nothing was lost** (see below) |
| Azure | `provision-federated-identity.sh --bundles object-persistence` | `Storage Blob Data Contributor` assigned at **container** scope on `kythirarealtestobj/kythira-raft` |
| GCP | `provision-workload-identity.sh --bundles gcp-object-persistence` | `roles/storage.objectUser` bound **on the bucket**, confirmed in the returned bucket IAM policy |
| **OCI** | *(nothing)* | **Already granted.** Policy `kythira-ci-artifacts` carries `Allow group kythira-ci to manage object-family in compartment kythira-ci` — a strict superset of the fragment. Running the provisioner would have **replaced** policy `kythira-ci`'s statements to add a permission the group already had |
| **Alibaba** | *(nothing)* | `kythira-ci-oss-persistence` was already attached to the RAM role, from the bundle that predates this spec |

**The AWS run is the one that needed a control, and got one.**
`put-role-policy` replaces the inline policy wholesale, so the invocation had to
name **all six** bundles rather than only the new one. The Sid set was captured
before and diffed after: `lost: NONE`, `added:
{ObjectPersistenceObjectPlane, ObjectPersistenceListTestPrefixOnly}`. Writing
the diff down is the point — "I remembered to pass the other bundles" and "the
other bundles are still there" are different claims, and only the second is
evidence.

### The lead: a tenancy-level `where` clause on a variable no Object Storage request has

Listing the tenancy's policies to work out OCI's bundle set turned up a policy
that nothing in this spec had looked at:

```
Policy kythira-ci-launch-tags (in tenancy):
  Allow group kythira-ci to use tag-namespaces in tenancy
      where target.tag-namespace.name = 'Oracle-Tags'
  Allow dynamic-group kythira-ci-pool-dg to use tag-namespaces in tenancy
      where target.tag-namespace.name = 'Oracle-Tags'
```

**That is the exact shape that broke this compartment once already.** The
mechanism is documented by Oracle and was measured here on August 12, 2026
(`scripts/ci-cloud-credentials/oci/policies/heartbeat.txt`): *a condition
variable that is inapplicable to a request declines the request rather than
merely failing to match the statement*, and the services enforce that
inconsistently — **Object Storage fails closed where Compute does not**. On that
occasion a `where` clause added to the *heartbeat* policy made the **CI group's
`put_object` to `kythira-ci-artifacts` fail with `404 BucketNotFound`**, while
the same principal's `ListInstances` kept succeeding.

The open flake has every one of those characteristics:

- the same group, `kythira-ci`;
- the same error, `404 BucketNotFound` ("...or you are not authorized");
- **intermittent, 3-16%** — consistent with a condition evaluated per request
  against a variable the request may or may not carry, rather than with a
  permission that is simply absent;
- across **all verbs**, body-carrying or not, which rules out most
  request-shape explanations.

`target.tag-namespace.name` is not a variable an Object Storage `PutObject`,
`GetObject`, `DeleteObject` or `ListObjects` supplies. By the documented rule
that is an *inapplicable* variable, and the recorded local behaviour of an
inapplicable variable in this compartment is a declined request.

**This is a hypothesis with a clear test, not a conclusion.** It has not been
tested, deliberately: mutating a tenancy policy is precisely the action that
caused the August 12 breakage, and the honest sequence is to establish the
before-picture first. The test, in order:

1. Take an `opc-request-id` from a live decline to the **compartment audit
   log** and read what the authorization decision actually says. This step is
   owed regardless and has been owed since the flake was first measured.
2. Measure the decline rate over a fixed burst as the `kythira-ci` principal,
   so there is a baseline number rather than a remembered range.
3. Only then, remove the `where` clause from `kythira-ci-launch-tags` (the
   statement's purpose — letting the pool tag instances with `Oracle-Tags` —
   survives the clause's removal, at the cost of allowing other tag
   namespaces), and re-measure the same burst.
4. **Re-verify every other principal/service pair the compartment serves**,
   not just Object Storage. That is what the August 12 attempt failed to do in
   the other direction, and it is why the breakage looked like a bucket outage.

If the rate goes to zero, the flake is a policy artifact and OCI's results stop
being weaker evidence than the other four providers'. If it does not, the clause
is exonerated and step 1's audit-log answer is the remaining thread.

**Until then nothing changes**: an OCI real-tier failure stays ambiguous, and
the honest response to red stays the audit log rather than re-running until
green.


## Finding 24 — The CI runs, and what measuring from a second position taught

August 21, 2026. Runs
[32432380565](https://github.com/crawlins/kythira/actions/runs/32432380565) and
[32441129124](https://github.com/crawlins/kythira/actions/runs/32441129124):
every provider's object-persistence bundle dispatched against its real service,
**under the least-privilege CI grants** rather than an operator's credentials.
AWS (x64 and arm64), Azure Blob, Alibaba OSS and GCS all pass; OCI's pre-flight
was declined `404 BucketNotFound` and was **not re-run**.

**Every least-privilege grant was sufficient, first try.** Task 17 recorded the
expectation that at least one would come up short, on the reasoning that no
grant had ever been exercised by a principal holding only it. That expectation
was wrong, and it is worth recording as wrong: the grants were derived from the
call lists in each client header, and that turned out to be enough.

**The measurement-position lesson, which is the real finding.** Finding 20's
figures came from a developer machine. These come from a GitHub-hosted runner,
and they disagree by 2-12×. Both are correct; neither is "the" number:

| provider | dev machine p50 | runner p50 | runner p99 |
|---|---|---|---|
| Azure Blob | 348 ms | **28.7 ms** | 30.4 ms |
| S3 | 128 ms | 66.5 ms | 81.3 ms |
| GCS | 145 ms | 135.1 ms | 153.2 ms |
| Alibaba OSS | 1648 ms | 1096.9 ms | 1140.5 ms |

**Azure moved 12×, and the reason is that GitHub's runners run on Azure.** That
row is a node talking to storage inside its own provider's network — the
in-provider case, which is what a correctly placed production node looks like.
Read bare it says "Azure Blob is 5× faster than GCS", which is false; it
measures proximity, and it is now labelled inline in every table that carries
it, exactly as Alibaba's cross-ocean figure already was.

**The spread is placement, not provider.** 30 ms to 1141 ms p99 across the same
engine and the same five checks — 38×, of which almost none is attributable to
which object store was chosen. `doc/cloud_object_persistence.md`'s
election-timeout guidance was rewritten around that: co-location is a bigger
lever than provider selection.

**"p99" needs an asterisk and now carries one.** The suite takes 8 samples for
`save_current_term` and 20 for `append_log_entry`, and picks percentiles by
nearest rank, *clamped*. At those counts the clamp lands on the last element,
so **the reported p99 is the slowest observed request** — a worst-of-run, not a
tail estimate. Useful for sizing; not a distribution claim. The
non-interpolating choice is right (interpolating would invent precision), but
it makes the label misleading unless said out loud.

**Two failures, two different kinds.** GCS's first attempt hit a Google-side
`502` carrying the service's own "temporary error, try again in 30 seconds"
page, after the client's retry policy was exhausted — a transient, re-run,
green. OCI was told a bucket that exists does not exist. Only the first
justifies a re-run, and treating them alike is how a regression becomes a
flake. Noted in passing: the GCS transient landed in **restore**, a bulk
unconditional write where `write_retries`' default of 1 is thin; the default is
right for the Raft hot path and is left alone.

## Finding 24 — The OCI decline, captured. It is principal-bound, and the request is not the variable.

August 21, 2026. The step this flake has carried since it was first measured —
*"take an `opc-request-id` from a decline to the compartment audit log"* — was
**never performable, and the audit log was never the right place.** Object
Storage data-plane operations are not audited by default, which is why
`oci audit event list` over the exact failure window returns one unrelated
event. The mechanism is **Logging service logs**, not Audit.

### What was enabled, and the order it had to happen in

Log group `kythira-ci-object-storage` in compartment `kythira-ci`, carrying two
`OCISERVICE` logs over `bucket kythira-ci-artifacts` — category `read`
(`kythira-ci-artifacts-read`) and category `write`
(`kythira-ci-artifacts-write`), 30-day retention, both `ACTIVE`.

**Enabled before the next decline, deliberately**, since the entire point is to
capture one. Then run [32444971282](https://github.com/crawlins/kythira/actions/runs/32444971282)
dispatched the object-persistence bundle alone and was allowed to fail.

### The decline and its byte-identical twin

The suite's first case, `fresh_engine_read_back`, **passed**. Its second,
`measured_latency`, was declined on the pre-flight LIST 6.7 seconds later. Both
requests are in the log, and they are the same request:

| field | 03:54:37.523Z | 03:54:44.236Z |
|---|---|---|
| `requestResourcePath` | `/n/axunmw4f0mln/b/kythira-ci-artifacts/o?limit=1000&prefix=kythira-real-test/` | **identical** |
| `credentials` (the UPST) | `ST$eyJraWQiOiJhc3dfcGh4…` | **identical** |
| `principalId` | `…aaaaaaaartsswebilvdi6pt3ym2lqxssriw5…` (`kythira-ci-wif`) | **identical** |
| `bucketId` | `ocid1.bucket.oc1.phx.aaaaaaaajtoovqotaslyvkxxcdpzvg26o3d5zdehucghkqumsplx2m7ojiba` | **identical** |
| `clientIpAddress`, `userAgent` | `20.102.46.193`, `cpp-httplib/0.27.0` | **identical** |
| `statusCode` / `errorCode` | **200**, "List of Objects retrieved." | **404 `BucketNotFound`** |

`opcRequestId` of the decline:
`phx-1:EfvQoI4P714JGboXITYkDCUlKBpRtAs01rUhPH7URKzBcRUNVbc2gKUuLmjng7aF`.

**This settles two things and constrains a third.**

**The client is exonerated, from the service's own side of the wire.** Every
remaining client-shaped explanation — wrong namespace, wrong bucket name,
wrong endpoint derivation, a signing defect that corrupts the path — requires
the two requests to differ. They do not differ in any field the service
recorded.

**The bucket was resolved before the 404 was chosen.** The declined entry
carries `bucketId` and `bucketCreator`, populated, for the very bucket it then
reported as not found. The 404 is an **authorization decline wearing a
not-found status** — which is what Oracle's own "...or you are not authorized"
wording permits, and which no client can distinguish at runtime.

**The variable is not in the request; it is server-side state.** This is the
new constraint, and it is the one the earlier evidence could not supply. A
policy `where` clause evaluated against an inapplicable request variable
(Finding 23) is *deterministic per request shape*: it would decline this LIST
every time, not 1 time in 2. Whatever selects between 200 and 404 changes
between two identical requests 6.7 s apart, so it is state on the service side
— an authorization cache, a statement set hydrated per request, or a
replica that disagrees — with the policy as the thing that state is *about*.
Finding 23's suspect is not refuted; it is now known to be at most half the
mechanism.

### The control: the same request as an Administrator, 60 times

| principal | request | N | declines |
|---|---|---|---|
| `kythira-ci-wif` (group `kythira-ci`), UPST | the LIST above | 2 logged | **1** |
| `clark@bit63.org` (group `Administrators`), API key | **byte-identical LIST** | **60** | **0** |

**The flake tracks the principal, not the bucket, the request, or the service.**
That is the first evidence that discriminates between "this compartment's
Object Storage is unwell" and "this *principal's* authorization is unwell", and
it points at the second. It is also, for whatever it is worth, consistent with
Finding 23: `Administrators` holds an unconditional `manage all-resources IN
TENANCY` and is not subject to either `where` clause in the tenancy.

Both `where` clauses were enumerated rather than assumed. The whole tenancy
contains exactly two, both in `kythira-ci-launch-tags`, both
`where target.tag-namespace.name = 'Oracle-Tags'`, one on `group kythira-ci`
and one on `dynamic-group kythira-ci-pool-dg`. No other policy in the tenancy
or the compartment carries a condition.

### Recorded but explicitly not folded in: the Logging service did the same thing

While reading these logs, `logging-search search-logs` declined **as an
Administrator** with `404 NotAuthorizedOrNotFound` — 2 of 8, then 3 of 12,
then **0 of 10** on a third burst with the logs 30 minutes old. That kills the
obvious "freshly-created log has not propagated" explanation without
establishing anything in its place.

It is a different service, a different principal and a different credential
type from the Object Storage flake, and 5-in-30-then-0-in-10 is a bursty
shape rather than a rate. **It is written down because it was seen, not
because it is the same phenomenon** — asserting one cause for two observations
this far apart is the guess this spec keeps refusing to make.

### Two traps in reading these logs, both of which produced a wrong answer first

**A log search that comes back short is not evidence of absence.** The first
query, run ~10 minutes after the job, returned the job's successful requests
and *not* the decline, and the conclusion drawn from it — "declines are not
logged, the request never reaches the bucket" — was wrong, interesting, and
would have redirected the whole investigation. A re-query after the ingestion
lag returned the 404. Always re-query before concluding a log is missing an
entry.

**The search scope must be `<compartment>/<logGroup>/<logOcid>`, all three.**
One mistake produces three unrelated errors: the two-part
`<compartment>/<logGroup>` form answers `400 InvalidParameter — "No log sources
found to be read"`, and scoping to the tenancy answers `401 NotAuthenticated`.
Neither says "name the log".

### The rate, enumerated — and why the suite is more fragile than the engine

Reading **both** categories rather than only `read` turned up a **second
decline in the same job that the first pass had missed**, and with it the first
decline rate that is counted rather than remembered:

| | |
|---|---|
| logged Object Storage requests in the job | **19** (11 GET, 8 PUT) |
| declined `404 BucketNotFound` | **2 — 10.5%** |
| which requests | `PUT` at 03:54:40.427Z, `GET`/LIST at 03:54:44.236Z |

10.5% sits inside the 3-16% band this flake has always been quoted at, which
is the first time that band has been confirmed against an enumeration of the
actual requests.

**The two declines had opposite outcomes, and the difference is retry.** The
declined PUT was **absorbed**: the engine reissued it on the same path 286 ms
later and got a 200, which is why the suite's first case,
`fresh_engine_read_back`, passed with no sign of trouble. The declined LIST was
the **pre-flight**, which issues one request and skips the suite if it fails.

So: **the suite is more fragile than the engine it tests.** A red OCI run does
not imply a broken engine; it means the one request in the whole run that has
no retry drew the short straw. That cuts both ways and both are worth saying —
`write_retries`' default of 1 is quietly doing real work against this tenancy,
and it is now measured doing it.

**The obvious fix is deliberately not applied.** Giving the pre-flight a retry
would have turned this run green. It would also have hidden a tenancy fault
behind the same engine retry that already hides it in the hot path, and left
the project with no signal at all for a defect that is not yet understood. That
is a decision about how much red to tolerate, and it belongs to whoever is
weighing the flake against a green board — not to a change made while
investigating it.

### What is owed now

Finding 23's step 2 — a decline *rate* for the `kythira-ci` principal — is
still owed and is now cheap to collect, since every request that principal makes
to this bucket is logged with its status. It needs dispatched runs to generate
the traffic, and those runs must be dispatched **to measure**, with the count
recorded before they start. That is a different act from re-running until green,
and the distinction is the whole reason this flake has not been laundered.

Step 3 — removing the `where` clause — stays blocked behind that number, for
the reason Finding 23 gave: mutating a tenancy policy is what caused the
August 12 breakage. The new constraint above also means a clean re-measurement
will be harder to interpret than expected: if the mechanism is partly a cache,
the rate may move slowly after the policy changes, and "it got better" over a
short window will not be evidence.

## Finding 25 — Pre-registration: the `kythira-ci` decline rate, N = 10 runs

**Written before the first run was dispatched.** That is the whole point of
this section. Dispatching *to measure* and re-running *until green* produce
identical-looking rows in the actions list, and the only thing that separates
them is a count fixed in advance by someone who did not yet know the outcome.
Finding 23 asked for this rate as its step 2; Finding 24 made it cheap by
putting every one of this principal's requests in a log with its status.

### Protocol, fixed in advance

| | |
|---|---|
| **N** | **exactly 10 dispatched runs** |
| what | `real-cloud-tests.yml` on `main`, object-persistence bundle **alone**, every other provider and bundle explicitly `false` so nothing launches |
| principal | `kythira-ci-wif` via GitHub OIDC → UPST, i.e. group `kythira-ci` |
| measured from | the Object Storage data-plane service logs, **both** categories, over the union of the 10 runs' windows |
| unit of measurement | **the individual request**, not the run — a run is a bundle of ~19 requests and one un-retried pre-flight, so run-level pass/fail measures the suite's shape rather than the tenancy's |
| statistic | declined requests ÷ logged requests, with the per-verb split |

### Stopping rule, fixed in advance

**Exactly ten. No early stop, no extension.**

- **Not stopped early if a run comes back green.** That is re-running until
  green wearing a measurement's clothes.
- **Not extended if the rate looks wrong.** Ten runs at ~19 requests is ~190
  observations; at a ~10% rate that is a wide interval and the write-up must
  say so rather than quietly buying precision with more runs after seeing the
  answer.
- **Runs that fail for any reason other than a `404 BucketNotFound` decline**
  — a build break, a credentials-step failure, a runner problem — are **not
  counted toward the ten and not counted in the denominator**, because they
  produce no Object Storage requests to count. Each such run is listed
  individually below with its reason. This is the one place the protocol has
  give, so it is the one place a thumb could rest on the scale.

### What each outcome would mean

- **A rate near 10%**, consistent with the single-run 10.5% of Finding 24 and
  the long-quoted 3-16%: the flake is stable and the `where`-clause test
  (Finding 23 step 3) has a baseline to be compared against.
- **A rate near zero**: something changed between August 19 and now, and the
  first suspect is the observation itself — enabling service logs on the
  bucket is a change to the bucket. That would be an awkward result and it is
  named here in advance so it cannot be quietly dropped.
- **A rate far above 16%**: the flake is worsening, and the priority order
  changes.

### Results

**All ten ran. None was excluded** — the escape clause in the protocol (runs
failing before issuing any Object Storage request) was never invoked, so the
denominator is every request the ten produced.

Runs `32479406650`, `32479434587`, `32479462555`, `32479491479`, `32479517102`,
`32479543322`, `32479568311`, `32479597711`, `32479627546`, `32479655246`,
dispatched 11:55:08Z-11:58:22Z on 2026-08-21.

| | |
|---|---|
| logged requests by `kythira-ci-wif` | **1222** |
| declined `404 BucketNotFound` | **84** |
| **rate** | **6.87%** |
| 95% Wilson interval | **5.6% - 8.4%** |
| error codes among declines | `BucketNotFound` × 84, nothing else |

| verb | requests | declined | rate |
|---|---|---|---|
| PUT | 612 | 49 | 8.0% |
| GET | 325 | 24 | 7.4% |
| DELETE | 285 | 11 | 3.9% |

Six `IfMatchFailed` responses were **excluded**: those are the fencing case's
own negative control succeeding, and counting a suite's passing assertions as
service declines would have inflated the rate by nearly a tenth.

**The rate is stable.** 6.87% [5.6-8.4] sits inside the long-quoted 3-16% band
and is statistically indistinguishable from Finding 24's single-run 10.5%
(n=19, an interval far too wide to separate the two). Nothing has escalated
and nothing has healed. The pre-registered "near zero" branch — which would
have implicated the act of enabling service logs — **did not occur**, so
observing the bucket does not appear to change it.

**Run-level, recorded but not the measurement: 1 of 10 green** (`32479543322`).
The request rate does not translate into a run rate by any simple power,
because the engine retries PUTs and the suite's pre-flight is un-retried, so
which requests are load-bearing matters more than how many there are. This is
exactly why the protocol fixed the request as the unit **before** the numbers
existed.

**That one green run is not "OCI verified", and must not be cited as such.**
It is the ~1-in-10 outcome of a suite whose every request drew from a 7%
decline distribution. The other four providers' green runs mean the provider
works *reliably*; this one means the dice cooperated once. Task 19's OCI row
stays red.

**But it is not worth nothing either, and the distinction is worth being
precise about.** That run passed **all five checks** — `fresh_engine_read_back`,
`measured_latency`, `fencing_race_with_negative_control`,
`backup_verify_restore_read_back`, `list_after_write` — against the live
service, "No errors detected". As a *verification* that is near-worthless: you
cannot establish reliability from a 1-in-10 draw. As an **existence proof** it
is real, and it is the second independent line of evidence pointing the same
way as Finding 24's byte-identical twin:

- **The client is correct.** Every check this design specifies has now been
  observed passing end-to-end against real OCI Object Storage, including
  fencing over `compare_and_swap`.
- **What is unverified is reliability against *this tenancy*** — which is a
  property of the tenancy, not of `oci_object_storage_client`.

Keeping those two apart is what stops the flake from being either overstated
(as a client defect) or understated (as a green board). The engine is
demonstrated; the environment is not trustworthy.

### The concurrency confound, named and then partly answered

The ten runs **overlapped in time**, where the Finding 24 baseline was a single
run in isolation. The protocol did not say "serially", and it should have — a
10× change in offered concurrency is exactly the kind of difference that makes
two numbers incomparable, and it was introduced by accident rather than chosen.

It happens to be answerable from the result. **If throttling were the
mechanism, ten concurrent runs should have pushed the rate up; it went from
10.5% to 6.87%, i.e. not up.** That is weak evidence against a rate-limiter
and it is worth exactly that much — the intervals overlap, so the honest
statement is "concurrency did not inflate the rate", not "concurrency is
irrelevant".

### A mistake this measurement caught, recorded because the protocol caught it

On seeing the first five runs come back red — where the baseline run had
passed its first case — the reaction was that the flake had escalated
dramatically. **It had not.** That was run-level outcomes being read as
request-level rates, and at ~7% per request over ~120 requests per run, almost
every run failing is the *expected* result rather than a surprising one.

The pre-registration's line that "the unit of measurement is the individual
request, not the run" was written to guard against exactly this, before there
was anything to be wrong about, and it is the reason the wrong conclusion
lasted minutes rather than reaching a document.

### A third silent-truncation near-miss, and the fix

Fetching the window in slices returned **150 records where a single wider query
had returned 903**. The slice queries were being declined by
`logging-search` itself (Finding 24's separate observation), and a declined
query with stderr redirected looks exactly like *an empty window*.

That is the third time in this investigation that a short log result nearly
became a small number in a document. **`read-object-storage-log.sh` now retries
six times, verifies the payload rather than the exit code, exits non-zero
rather than reporting zero entries, and warns when a window hits the
1000-record page cap.** A counting script that can silently under-count is
worse than no counting script.
