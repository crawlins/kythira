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
| — | Azure Blob, GCS, OCI Object Storage | not written; those cells are still OPEN |

## Running

```sh
KYTHIRA_ALIBABA_OSS_BUCKET=<bucket> \
KYTHIRA_ALIBABA_REGION=ap-southeast-1 \
python3 scripts/object-store-probes/probe_alibaba_oss.py
```

Credentials come from `~/.aliyun/config.json` (`$ALIBABA_PROFILE`, else the
CLI's current profile). They are never printed, never placed on a command
line, and never exported into the environment. No third-party Python packages
are needed — standard library only.

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

Any new provider probe should reuse that distinction rather than re-deriving
it, and should include the negative control: a *correct* precondition must
succeed, or "rejected" proves nothing about the fence.

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
