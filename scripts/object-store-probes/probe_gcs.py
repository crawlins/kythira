#!/usr/bin/env python3
"""Task 0 probes for Google Cloud Storage (.kiro/specs/cloud-object-persistence/).

GCS expresses preconditions differently from every other provider in this
spec — **generation numbers, as query parameters**, not ETags in headers:

  1. create-only            — ifGenerationMatch=0
  2. overwrite CAS          — ifGenerationMatch=<generation>
  3. conditional DELETE     — ifGenerationMatch=<generation>
  4. the rejection code and reason under real contention
  5. checksum: `md5Hash` in the object metadata (the shape
     `google-cloud-cpp`'s `MD5HashValue` uses), plus whether the returned
     ETag or `md5Hash` is a deterministic function of single-part content
  6. list-after-write, immediately, three rounds

That difference is the reason the design carries `object_version` as an
**opaque string the engine never inspects**: a generation is a decimal counter
and an ETag is a quoted hash, and an engine that assumed either shape would be
wrong on some provider. This probe is also the evidence for that claim rather
than an assertion of it.

Everything is written under `kythira-real-test/probe-<epoch>/` and deleted
before exit; the last line is the residual count.

Usage:
    KYTHIRA_GCS_BUCKET=<bucket> python3 scripts/object-store-probes/probe_gcs.py
"""
import base64
import hashlib
import os
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

from gcs_probe import (
    BUCKET,
    delete_object,
    get_object,
    list_objects,
    parse,
    reason_of,
    report,
    upload,
    upload_with_md5,
)
from probe_common import cas_verdict, classify_precondition

PREFIX = "kythira-real-test/probe-%d/" % int(time.time())
print("bucket=%s (JSON API)\nprefix=%s\n" % (BUCKET, PREFIX))
created = []


def put(key, body, query=None):
    created.append(key)
    return upload(key, body, query=query)


def classify(status, body, landed, previous):
    return classify_precondition(status, reason_of(body), landed, previous)


print("== 1. create-only: ifGenerationMatch=0 ==")
key = PREFIX + "ifgenmatch0"
status, headers, body = put(key, b"first", {"ifGenerationMatch": 0})
report("upload absent object, ifGenerationMatch=0", status, headers, body)
status, headers, body = put(key, b"second", {"ifGenerationMatch": 0})
report("upload existing object, ifGenerationMatch=0", status, headers, body)
rejected = (status, body)
status, headers, landed = get_object(key)
report("GET (did the second upload land?)", status, headers, landed, extra=repr(landed))
print("  VERDICT: %s" % classify(rejected[0], rejected[1], landed, b"first"))

print("\n== 2. overwrite CAS: ifGenerationMatch=<generation> ==")
key = PREFIX + "ifgenmatch"
status, headers, body = put(key, b"v1")
generation = parse(body).get("generation", "")
report("upload initial", status, headers, body)
status, headers, body = put(key, b"v2-correct-gen", {"ifGenerationMatch": generation})
new_generation = parse(body).get("generation", "")
report("upload ifGenerationMatch=<current>", status, headers, body)
correct = (status, body)
status, headers, body = put(key, b"v3-stale-gen", {"ifGenerationMatch": generation})
report("upload ifGenerationMatch=<stale>", status, headers, body)
stale = (status, body)
status, headers, landed = get_object(key)
report("GET (what is stored now?)", status, headers, landed, extra=repr(landed))
# The last argument is what would still be stored had the write been refused —
# the PREVIOUS content, never the new one.
print("  correct-generation write: %s" % classify(correct[0], correct[1], landed, b"v1"))
print("  stale-generation  write: %s" % classify(stale[0], stale[1], landed, landed))
print("  VERDICT: overwrite CAS is %s" % cas_verdict(correct[0], stale[0]))
print("  generation is a decimal counter, not a hash: %r -> %r" % (generation, new_generation))

print("\n== 3. conditional DELETE: ifGenerationMatch ==")
key = PREFIX + "cond-delete"
status, headers, body = put(key, b"deleteme")
generation = parse(body).get("generation", "")
stale_generation = str(int(generation) - 1) if generation.isdigit() else "1"
status, headers, body = delete_object(key, {"ifGenerationMatch": stale_generation})
report("DELETE ifGenerationMatch=<stale>", status, headers, body)
stale_delete_status = status
status, headers, body = get_object(key)
survived = status == 200
report("GET after stale-generation DELETE", status, headers, body,
       extra=("present" if survived else "ABSENT — the delete happened"))
print(
    "  VERDICT: conditional delete is %s"
    % (
        "ENFORCED"
        if stale_delete_status in (409, 412) and survived
        else "SILENTLY IGNORED — the stale precondition did not stop the delete"
        if not survived
        else "REJECTED but unverified (HTTP %s)" % stale_delete_status
    )
)
status, headers, body = delete_object(key, {"ifGenerationMatch": generation})
report("DELETE ifGenerationMatch=<current>", status, headers, body)

print("\n== 4. the rejection code and reason under contention ==")
print("  Racing concurrent ifGenerationMatch=0 uploads at one fresh object per round.")
observed = Counter()
for round_index, workers in enumerate((8, 16, 32)):
    race_key = "%srace-%d" % (PREFIX, round_index)
    created.append(race_key)

    def contend(n, k=race_key):
        st, _, bd = upload(k, b"racer-%d" % n, query={"ifGenerationMatch": 0})
        return st, reason_of(bd)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(contend, range(workers)))
    counts = Counter("%s %s" % (st, reason or "-") for st, reason in results)
    observed.update(counts)
    print("  %2d concurrent uploads -> %s" % (workers, dict(counts)))

winners = sum(c for k, c in observed.items() if k.startswith("200"))
print("  totals: %s" % dict(observed))
print("  winners across all rounds: %d (one per round is correct)" % winners)
for code in ("409", "412", "429", "503"):
    seen = any(k.startswith(code) for k in observed)
    if seen:
        print("  %s observed: True" % code)
print("  Anything not listed above was not elicited here — which is not")
print("  evidence that it cannot occur; the client's mapping must still cover it.")

print("\n== 5. checksum: md5Hash in metadata, and hash determinism ==")
payload = b"checksum-me" * 10
good = base64.b64encode(hashlib.md5(payload).digest()).decode()
bad = base64.b64encode(hashlib.md5(b"different").digest()).decode()
key = PREFIX + "md5"
created.append(key)
status, headers, body = upload_with_md5(key, payload, good)
report("upload correct md5Hash", status, headers, body)
returned = parse(body)
print("  returned md5Hash == md5 of content? %s" % (returned.get("md5Hash", "") == good))
print("  returned etag: %r  (== md5 hex? %s)"
      % (returned.get("etag", ""),
         returned.get("etag", "").strip('"').lower() == hashlib.md5(payload).hexdigest()))
key = PREFIX + "md5-bad"
created.append(key)
status, headers, body = upload_with_md5(key, payload, bad)
report("upload wrong md5Hash", status, headers, body)
print(
    "  VERDICT: end-to-end checksum is %s"
    % ("VERIFIED by the service (%s)" % (reason_of(body) or status)
       if status >= 400 else "NOT verified (HTTP %s)" % status)
)

print("\n== 6. list-after-write, immediately, 3 rounds x 25 objects ==")
for round_index in range(3):
    round_prefix = "%slaw-%d/" % (PREFIX, round_index)
    for i in range(25):
        key = "%s%020d" % (round_prefix, i)
        created.append(key)
        status, _, _ = upload(key, b"x")
        if status != 200:
            print("  round %d: upload %s failed HTTP %s" % (round_index, key, status))
    status, headers, body = list_objects(round_prefix)
    listing = parse(body)
    print(
        "  round %d: LIST immediately after 25 uploads -> %d objects, nextPageToken=%r"
        % (round_index, len(listing.get("items", [])), listing.get("nextPageToken", ""))
    )

print("\n== cleanup ==")


def sweep(prefix, recorded=()):
    """Delete what we recorded, then delete whatever the listing still shows.

    The second pass is the point: a residual count computed from the probe's
    own bookkeeping would only ever confirm that bookkeeping.
    """
    failures = 0
    for key in recorded:
        status, _, _ = delete_object(key)
        if status not in (200, 204, 404):
            failures += 1
    _, _, body = list_objects(prefix)
    leftovers = [item["name"] for item in parse(body).get("items", [])]
    for key in leftovers:
        status, _, _ = delete_object(key)
        if status not in (200, 204, 404):
            failures += 1
    _, _, body = list_objects(prefix)
    return failures, len(leftovers), len(parse(body).get("items", []))


failures, swept, residual = sweep(PREFIX, created)
print("  deleted %d recorded objects (+%d found by listing), %d failures"
      % (len(created), swept, failures))
print("  residual objects under prefix: %d" % residual)

# An interrupted earlier run can be cleaned up by naming its prefix here; the
# probe refuses to sweep anything outside `kythira-real-test/`.
for stale_prefix in [p for p in os.environ.get("KYTHIRA_PROBE_SWEEP", "").split(",") if p]:
    if not stale_prefix.startswith("kythira-real-test/"):
        raise SystemExit("refusing to sweep %r: not under kythira-real-test/" % stale_prefix)
    failures, swept, residual = sweep(stale_prefix)
    print("  swept %s: %d objects deleted, %d failures, %d residual"
          % (stale_prefix, swept, failures, residual))
