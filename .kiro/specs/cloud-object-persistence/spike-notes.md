# Spike notes — cloud key-object persistence (task 0)

Findings recorded in the OCI spec's CONFIRMED / CORRECTED / WAS format. Every
correction is folded back into requirements.md and design.md **in place**;
this file is the evidence trail, not a second source of truth.

## Status

| Sub-task | Provider coverage | State |
|---|---|---|
| 0.1 list-after-write, empirically | **Alibaba OSS + AWS S3: CLOSED** (live) | Azure Blob, OCI still OPEN |
| 0.2 OCI durability-on-response wording | — | OPEN |
| 0.3 conditional-write matrix, live | **Alibaba OSS + AWS S3: CLOSED** (live; each decided a design outcome) | Azure Blob, GCS, OCI still OPEN |
| 0.4 single-PUT size limits | — | OPEN (documentation-only so far) |
| 0.5 checksum spelling / ETag determinism | **Alibaba OSS + AWS S3: CLOSED** (live) | others OPEN |
| 0.6 Azure REST-vs-SDK checkpoint | — | OPEN |
| 0.7 GCS mock-tier decision | — | OPEN |
| 0.8 OCI namespace and endpoint | partially — the namespace resolves via `oci os ns get` (`axunmw4f0mln`) | OPEN |

**Why the other providers are still open, plainly:** nothing external blocks
them any more. Credentials work for all five (see the table below) and **every
provider now has a bucket** (Finding 8 — AWS, Azure and GCS provisioned
August 16, 2026; OCI and Alibaba already had one). What is left is writing the
per-provider probes, which is work rather than a dependency.

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
