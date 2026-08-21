# Runbook — the OCI Object Storage `404 BucketNotFound` flake

**Status: EXECUTED end to end on August 21, 2026. The named suspect is
EXONERATED.** Every step below was run against the live tenancy; the results
are in `spike-notes.md` **Finding 26**. OCI is still the one provider without a
green least-privilege CI run, but the cause is no longer unknown and is no
longer thought to be tenancy policy.

**What it found, so nobody repeats it:**

- **The `Oracle-Tags` `where` clause is not the cause.** Removed, measured,
  restored: **81 declines / 5000 requests with it, 122 / 5000 without it**
  (Fisher p = 0.58 on episodes). Step 3's "rate goes to zero" branch did not
  happen; its "rate unchanged → exonerated" branch did.
- **Step 1's question has an answer, and the answer is that the log does not
  carry one.** The decline *is* recorded in the data plane — so the "log is
  empty ⇒ the request never reached the data plane" branch below is also
  ruled out — but the record has **24 fields and not one is an authorization
  outcome or a statement**. The entire authorization content is
  `errorCode: BucketNotFound`. Audit does not have it either. That step is not
  deliverable from OCI's customer-visible logging at all.
- **What the fault actually is:** it needs **concurrency** (serial 0.30% vs
  16-way 11.70%, same principal and request), it is **principal-specific** (an
  Administrator is clean across 5000 requests at the same concurrency), and it
  is **Object Storage only** (the same principal's Compute calls in the same
  compartment are clean). It reproduces under a plain **API key**, so the
  WIF/UPST path is not involved. Best supported: **Object Storage's
  authorization path for non-administrator principals in this tenancy fails
  intermittently under concurrent load.** That is Oracle-side.
- **The rate is not a rate.** It is *episodic* — twelve identical bursts read
  0.00% to 13.40%. Single before/after comparisons are worthless; use
  `probe-object-storage-decline-rate.py --repeat` and compare episode counts.

**The open action is an Oracle support ticket** carrying the `opc-request-id`s,
the byte-identical successful twins, the concurrency dependence and the two
matched controls. **It is not another policy edit, and not another run.**

The steps below are kept as executed, with their predictions intact, because
which predictions held and which did not is the record.

## What is wrong

The `kythira-ci` group's Object Storage requests to `kythira-ci-artifacts` are
declined `404 BucketNotFound` ("...or you are not authorized") at a measured
**3–16%**, across all verbs, while the same principal's Compute calls succeed.
The bucket plainly exists. This is a *wrong answer*, not a transient — which is
why the honest response to a red OCI run is this runbook and **not** another
run. Re-running until green is precisely how a real regression is laundered
into a flake.

## The named suspect

Tenancy policy `kythira-ci-launch-tags`:

```
Allow group kythira-ci to use tag-namespaces in tenancy
    where target.tag-namespace.name = 'Oracle-Tags'
```

OCI documents that a condition variable **inapplicable to a request declines
the request** rather than merely failing to match the statement, and the
services enforce that inconsistently — **Object Storage fails closed where
Compute does not**. `target.tag-namespace.name` is not a variable that
`PutObject`, `GetObject`, `DeleteObject` or `ListObjects` supplies.

The same shape broke this compartment once already: on **August 12 2026** a
`where` clause added to the *heartbeat* policy made this group's `put_object`
to this bucket fail with this exact error, while its `ListInstances` kept
working. It was reverted within the hour. `policies/heartbeat.txt` carries the
full account.

**This is a hypothesis with a clear test, not a conclusion.**

## Why step 0 exists

Finding 23's sequence opens with "take an `opc-request-id` from a live decline
to the audit log". That has been owed since the flake was first measured and
has **never been performable**: `oci audit event list` returns nothing over the
failure window, because Object Storage **data-plane** events are not audited by
default. Only control-plane events (create/delete bucket) are.

So the first action is not a measurement. It is turning on the instrument.

---

### Step 0 — enable data-plane logging

```bash
scripts/ci-cloud-credentials/oci/enable-object-storage-logging.sh \
    --compartment-id "$OCI_CI_COMPARTMENT_ID" --dry-run   # inspect first
scripts/ci-cloud-credentials/oci/enable-object-storage-logging.sh \
    --compartment-id "$OCI_CI_COMPARTMENT_ID"
```

Creates a log group and one `read` and one `write` service log on
`kythira-ci-artifacts`. It touches **no IAM policy**, and it refuses to create a
log for a category the service does not offer — a nonexistent category is
accepted by the API and yields a log that never fills, which would read as
"logging is on and the flake emits nothing": the most misleading outcome
available here.

> **Logging is prospective.** This backfills nothing. It says nothing about the
> August failures. Do not go looking for old request IDs — generate new ones.

### Step 1 — capture a live decline

Run the real suite, or a tight loop of `PutObject`/`ListObjects`, as the
`kythira-ci` principal until a `404` appears, and keep its `opc-request-id`
(every OCI response carries one; the client logs it). Then read it back:

```bash
oci logging-search search-logs \
  --search-query "search \"$OCI_CI_COMPARTMENT_ID/kythira-ci-logs\"
                  | where data.opcRequestId = '<the id>'" \
  --time-start "$(date -u -d '15 min ago' +%Y-%m-%dT%H:%M:%SZ)" \
  --time-end   "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
```

**What you are looking for** is whether the decline is recorded as an
authorization outcome at all, and against which statement. That single answer
either implicates the `where` clause or exonerates it, and it is worth more
than the rest of this runbook combined.

If the log is empty for a request you *know* was declined, that is itself a
finding: it means the request never reached the data plane and the decision was
taken earlier, at authorization.

### Step 2 — baseline the rate BEFORE changing anything

```bash
# fixed burst, same principal, same bucket; record N and the failure count
for i in $(seq 1 200); do …; done | tee /tmp/oci-burst-before.log
```

Finding 23 records "3–16%" as a *remembered range*. Step 3 is uninterpretable
without a number measured the same way, on the same day, immediately before the
change. Write down N, the count, and the wall-clock window.

### Step 3 — only now, the policy

Remove the `where` clause from `kythira-ci-launch-tags`. The statement's
purpose — letting the pool tag instances with `Oracle-Tags` — survives its
removal, at the cost of permitting other tag namespaces.

Re-run **the identical burst** from step 2 and compare.

- Rate goes to zero → the flake is a policy artifact, OCI's results stop being
  weaker evidence than the other four providers', and task 19 can close.
- Rate unchanged → the clause is exonerated, and step 1's audit answer is the
  remaining thread.

### Step 4 — re-verify everything else, not just Object Storage

**This is the step whose omission caused the August 12 incident to look like a
bucket outage.** Changing a tenancy policy changes it for every principal it
names. Before calling this done, re-verify every principal/service pair this
compartment serves:

```bash
cmake --build <build-dir> --target oci_tenancy_check
./<build-dir>/tests/oci_tenancy_check
```

plus the instance-pool, certificates and heartbeat bundles' real suites — not
only `oci_object_storage_persistence_real_test`.

---

## Rollback

Step 3 is the only mutation, and it is a single statement. Restore it with the
`where` clause intact. Steps 0–2 create logs and read them; the logs can be
deleted (`oci logging log delete`) but leaving them on is the point.

## When this closes

Fold the result back into `spike-notes.md` **in place** as a new Finding, per
the OCI doctrine this spec follows, and update task 19's OCI row. If the answer
is "the clause was innocent", that is a result worth the same write-up as a
fix — it removes the only named suspect and tells the next person where not to
look.
