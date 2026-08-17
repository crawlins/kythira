# Object-store probes

Live probes that close the OPEN cells in
`.kiro/specs/cloud-object-persistence/design.md`'s consistency, durability and
conditional-write tables. Findings land in that spec's `spike-notes.md`, and
every correction is folded back into requirements.md/design.md **in place**.

These exist because the cells they close **cannot be closed from
documentation**. Two of them decide code: whether a provider can express an
overwrite precondition at all (Requirement 9.8 makes the answer a compile-time
property, not a runtime one), and which status code means "you lost the race"
versus "retry" (mapping those wrong latches a healthy engine permanently, or
silently overwrites another writer).

| Script | Provider | Status |
|---|---|---|
| `probe_alibaba_oss.py` | Alibaba OSS | **run August 16, 2026** — cells 0.1, 0.3, 0.5 closed. Verdict: **no overwrite CAS**, so OSS cannot be fenced |
| `probe_aws_s3.py` | AWS S3 | **run August 16, 2026** — cells 0.1, 0.3, 0.5 closed. Verdict: **every conditional primitive works**, conditional delete included |
| `probe_azure_blob.py` | Azure Blob | **run August 17, 2026** — cells 0.1, 0.3, 0.5 **and 0.6** closed. Verdict: **fenceable**; create-only rejects **409**, `If-Match` rejects **412** |
| `probe_gcs.py` | GCS | **run August 17, 2026** — cells 0.1, 0.3, 0.5 closed (0.7 still needs an emulator). Verdict: **fenceable** via generation preconditions |
| `probe_oci.py` | OCI Object Storage | **run August 17, 2026** — cells 0.1, 0.3, 0.5 **and 0.8** closed. Verdict: **fenceable**, conditional delete confirmed |

Four of the five providers can be fenced. Only Alibaba OSS cannot, and the
full side-by-side table is spike-notes.md Finding 14.

## Running

```sh
KYTHIRA_ALIBABA_OSS_BUCKET=<bucket> KYTHIRA_ALIBABA_REGION=ap-southeast-1 \
    python3 scripts/object-store-probes/probe_alibaba_oss.py

KYTHIRA_S3_BUCKET=<bucket> KYTHIRA_AWS_PROFILE=personal \
    python3 scripts/object-store-probes/probe_aws_s3.py

KYTHIRA_AZURE_STORAGE_ACCOUNT=<account> KYTHIRA_AZURE_BLOB_CONTAINER=<container> \
    python3 scripts/object-store-probes/probe_azure_blob.py

KYTHIRA_GCS_BUCKET=<bucket> python3 scripts/object-store-probes/probe_gcs.py

KYTHIRA_OCI_BUCKET=<bucket> python3 scripts/object-store-probes/probe_oci.py
```

Each probe takes credentials from that provider's own CLI resolver — the
Alibaba CLI config, `aws configure export-credentials`, `az account
get-access-token`, `gcloud auth application-default print-access-token`, and
`~/.oci/config`'s API key. Nothing is printed, placed on a command line, or
exported into the environment. Only the OCI probe needs a third-party package
(`cryptography`, for the RSA signature); the rest are standard library only.

An interrupted run leaves objects behind. The Azure, GCS and OCI probes accept
`KYTHIRA_PROBE_SWEEP=<prefix>[,<prefix>...]` to clean an earlier prefix, and
refuse any prefix outside `kythira-real-test/`.

Every object is written under a throwaway `spike/objstore-cond-<epoch>/`
prefix and deleted before exit; the final line is the **residual count**, so a
failed cleanup is visible rather than silent. Cost is a few dozen requests —
a rounding error against any storage minimum.

## Why the signing is hand-written here

`oss_probe.py` implements OSS V4 (`OSS4-HMAC-SHA256`) from the canonical form
documented in `include/raft/alibaba_oss_client.hpp`, rather than calling the
C++ client. The client cannot send conditional headers — whether the service
honours them is precisely what these probes decide — and an independent
implementation is a second witness to the signing scheme. It authenticated on
its first live call, which is itself a confirmation of the documented form.

## The rule these probes encode: read the status code, not the effect

The first version of the OSS probe asked *"did the stale write change the
bytes?"*, saw that it had not, and reported **"CAS works"**. It does not: the
write was refused `400 NotImplemented`, so no precondition was ever evaluated.
An effect-based check **cannot distinguish a working fence from a rejected
request**, and that run would have written a fence that does not exist into
the design.

`classify_precondition()` therefore keys on the status and error code:

| Outcome | Meaning |
|---|---|
| 409 / 412 | precondition **enforced** — this is a usable fence |
| 400 / 501 `NotImplemented` | **unsupported** — the header is not evaluated at all |
| 2xx and the object is unchanged | **silently ignored** — the most dangerous shape |

That rule now lives in `probe_common.py` and is shared by the Azure, GCS and
OCI probes. The two earlier probes carry their own copy from before that module
existed, and are deliberately left byte-for-byte as they were run — a probe's
value is that a specific text produced a specific finding.

The negative control is not optional: a *correct* precondition must be
**accepted**, or "rejected" proves nothing about the fence. That is not
theoretical. On its first run the OCI probe read the ETag out of a plain dict
as `"ETag"`, got nothing because OCI spells the header `etag`, and sent
`if-match: ""`; the service answered 412 to a *correct*-ETag write and the run
looked exactly like "OCI cannot do CAS" — the same finding Alibaba genuinely
earned. `cas_verdict()` printed "NOT USABLE — even a correct precondition was
refused" instead of a false conclusion, and response headers are now
case-insensitive for every provider.

Two further rules the S3 run added, both learned the same way:

- **The classifier's fourth argument is `bytes_if_write_did_not_happen`** —
  the content that would still be there had the write been refused, i.e. the
  *previous* content. Passing the new content makes a successful write read as
  "silently ignored"; that is exactly how this probe mislabelled S3's working
  CAS on its first run. A check whose inputs can be transposed without a type
  error eventually will be.
- **Absence of a status code is not evidence it cannot occur.** S3's 409
  `ConditionalRequestConflict` never appeared in 53 racing losers. The probe
  prints that as "not elicited here — do NOT conclude it cannot happen",
  because the client's error mapping still has to handle it.

## A status class is not a reason

The OCI run added a third form of the same mistake. The checksum verdict
originally read "status >= 400 → the service verified the digest". A request
that was refused for an unrelated reason — the tenancy declining it with
`404 BucketNotFound` — therefore printed **"VERIFIED by the service
(BucketNotFound)"**. The verdict now requires a **checksum-specific error
code** and prints UNDECIDED otherwise, which is how OCI's actual code
(`UnmatchedContentMD5`, a fifth distinct spelling) came to be recorded rather
than assumed.

The same discipline applies to counting. The list-after-write check compares
the listing against **acknowledged writes**, never against the number of writes
attempted: a round where ten PUTs were declined and the listing shows fifteen
is a *complete* listing, and reading it as a lagging one would have invented a
consistency defect out of an authorization flake.
