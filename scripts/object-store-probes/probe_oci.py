#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Task 0 probes for OCI Object Storage (.kiro/specs/cloud-object-persistence/).

Cells closed here:

  0.1 list-after-write, immediately, three rounds
  0.3 the conditional matrix — create-only (`if-none-match: *`), `if-match`
      overwrite, and the **conditional DELETE that design.md marked
      unconfirmed**
  0.5 checksum spelling (`Content-MD5`) and whether the ETag is a
      deterministic function of single-part content
  0.8 the `objectstorage` regional endpoint spelling, and whether the
      namespace must be resolved from the API or can be configured

Everything is written under `kythira-real-test/probe-<epoch>/` and deleted
before exit; the last line is the residual count.

Usage:
    KYTHIRA_OCI_BUCKET=kythira-ci-artifacts \\
        python3 scripts/object-store-probes/probe_oci.py
"""
import base64
import hashlib
import os
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

from oci_probe import (
    BUCKET,
    DECLINES,
    HOST,
    REGION,
    code_of,
    measure,
    parse,
    report,
    request,
    resolve_namespace,
)
from probe_common import CHECKSUM_MISMATCH_CODES, cas_verdict, classify_precondition

if not BUCKET:
    raise SystemExit("set KYTHIRA_OCI_BUCKET")

print("== 0.8. endpoint and namespace ==")
NAMESPACE, how = resolve_namespace()
print("  endpoint: https://%s   (region=%s)" % (HOST, REGION))
print("  namespace: %s   (%s)" % (NAMESPACE, how))
print("  bucket:    %s" % BUCKET)

BASE = "/n/%s/b/%s/o/" % (NAMESPACE, BUCKET)
LIST_PATH = "/n/%s/b/%s/o" % (NAMESPACE, BUCKET)
PREFIX = "kythira-real-test/probe-%d/" % int(time.time())
print("  prefix:    %s\n" % PREFIX)
created = []


def put(key, body, headers=None):
    created.append(key)
    return measure("PUT", BASE + key, headers=headers or {}, body=body)


def get(key):
    return measure("GET", BASE + key)


def delete(key, headers=None):
    return measure("DELETE", BASE + key, headers=headers or {})


def listing(prefix):
    return measure("GET", LIST_PATH, query={"prefix": prefix, "limit": 1000})


def classify(status, headers, body, landed, previous):
    return classify_precondition(status, code_of(headers, body), landed, previous)


print("== 1. create-only: if-none-match: * ==")
key = PREFIX + "ifnonematch"
status, headers, body = put(key, b"first", {"if-none-match": "*"})
report("PUT absent object, if-none-match: *", status, headers, body)
status, headers, body = put(key, b"second", {"if-none-match": "*"})
report("PUT existing object, if-none-match: *", status, headers, body)
rejected = (status, headers, body)
status, headers, landed = get(key)
report("GET (did the second PUT land?)", status, headers, landed, extra=repr(landed))
print("  VERDICT: %s" % classify(rejected[0], rejected[1], rejected[2], landed, b"first"))

print("\n== 2. overwrite CAS: if-match ==")
key = PREFIX + "ifmatch"
status, headers, body = put(key, b"v1")
etag_v1 = headers.get("ETag", "")
report("PUT initial", status, headers, body)
status, headers, body = put(key, b"v2-correct-etag", {"if-match": etag_v1})
report("PUT if-match: <current etag>", status, headers, body)
correct = (status, headers, body)
status, headers, body = put(key, b"v3-stale-etag", {"if-match": etag_v1})
report("PUT if-match: <stale etag>", status, headers, body)
stale = (status, headers, body)
status, headers, landed = get(key)
report("GET (what is stored now?)", status, headers, landed, extra=repr(landed))
# The last argument is what would still be stored had the write been refused —
# the PREVIOUS content, never the new one.
print("  correct-ETag write: %s" % classify(correct[0], correct[1], correct[2], landed, b"v1"))
print("  stale-ETag  write: %s" % classify(stale[0], stale[1], stale[2], landed, landed))
print("  VERDICT: overwrite CAS is %s" % cas_verdict(correct[0], stale[0]))

print("\n== 3. conditional DELETE: if-match  [design.md marked this unconfirmed] ==")
key = PREFIX + "cond-delete"
status, headers, body = put(key, b"deleteme")
etag = headers.get("ETag", "")
status, headers, body = delete(key, {"if-match": "0000000-0000-0000-0000-000000000000"})
report("DELETE if-match: <stale etag>", status, headers, body)
stale_delete_status = status
status, headers, body = get(key)
survived = status == 200
report("GET after stale-etag DELETE", status, headers, body,
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
status, headers, body = delete(key, {"if-match": etag})
report("DELETE if-match: <current etag>", status, headers, body)

print("\n== 4. the rejection code under contention ==")
print("  Racing concurrent if-none-match:* PUTs at one fresh object per round.")
print("  S3 answers 412 here, OSS and Azure answer 409 — and OSS's 409 and")
print("  S3's 409 mean opposite things, so the code matters as much as the status.")
observed = Counter()
for round_index, workers in enumerate((8, 16, 32)):
    race_key = "%srace-%d" % (PREFIX, round_index)
    created.append(race_key)

    def contend(n, k=race_key):
        st, hd, bd = request(
            "PUT", BASE + k, headers={"if-none-match": "*"}, body=b"racer-%d" % n
        )
        return st, code_of(hd, bd)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(contend, range(workers)))
    counts = Counter("%s %s" % (st, code or "-") for st, code in results)
    observed.update(counts)
    print("  %2d concurrent PUTs -> %s" % (workers, dict(counts)))

winners = sum(c for k, c in observed.items() if k.startswith("200"))
print("  totals: %s" % dict(observed))
print("  winners across all rounds: %d (one per round is correct)" % winners)
print("  Any code absent above was not elicited here — which is not evidence")
print("  that it cannot occur; the client's mapping must still cover it.")

print("\n== 5. Content-MD5 and ETag determinism ==")
key = PREFIX + "md5"
payload = b"checksum-me" * 10
good = base64.b64encode(hashlib.md5(payload).digest()).decode()
bad = base64.b64encode(hashlib.md5(b"different").digest()).decode()
status, headers, body = put(key, payload, {"Content-MD5": good})
report("PUT correct Content-MD5", status, headers, body)
etag = headers.get("ETag", "").strip('"')
print("  ETag == md5 hex of content? %s   (ETag=%s)"
      % (etag.lower() == hashlib.md5(payload).hexdigest(), etag))
status, headers, body = get(key)
returned = headers.get("Content-MD5", "") or headers.get("opc-content-md5", "")
print("  GET returns a content MD5? %s  (matches: %s)"
      % (bool(returned), returned == good))
status, headers, body = put(PREFIX + "md5-bad", payload, {"Content-MD5": bad})
report("PUT wrong Content-MD5", status, headers, body)
# The verdict keys on a *checksum-specific* error code. "Any 4xx" is not
# evidence the service checked the digest: this probe's first run had the
# tenancy decline that exact request with 404 BucketNotFound and duly printed
# "VERIFIED by the service (BucketNotFound)". A rejection for an unrelated
# reason is not a rejection of the checksum.
mismatch_code = code_of(headers, body)
checksum_codes = CHECKSUM_MISMATCH_CODES
print(
    "  VERDICT: end-to-end checksum is %s"
    % (
        "VERIFIED by the service (%s %s)" % (status, mismatch_code)
        if status >= 400 and mismatch_code in checksum_codes
        else "NOT verified (HTTP %s)" % status
        if status < 400
        else "UNDECIDED — refused %s %s, which is not a checksum error"
        % (status, mismatch_code or "-")
    )
)

print("\n== 6. list-after-write, immediately, 3 rounds x 25 objects ==")
for round_index in range(3):
    round_prefix = "%slaw-%d/" % (PREFIX, round_index)
    written = 0
    for i in range(25):
        key = "%s%020d" % (round_prefix, i)
        created.append(key)
        status, headers, body = put(key, b"x")
        if status == 200:
            written += 1
        else:
            print("  round %d: PUT %s failed HTTP %s %s"
                  % (round_index, key, status, code_of(headers, body)))
    status, headers, body = listing(round_prefix)
    parsed = parse(body)
    listed = len(parsed.get("objects", []))
    # The comparison is listing-versus-*acknowledged writes*, not
    # listing-versus-25. A round where four PUTs were declined and the listing
    # shows 21 is a complete listing, and reading it as a lagging one would
    # invent a consistency defect out of an authorization flake.
    print(
        "  round %d: %d/25 PUTs acknowledged, LIST immediately -> %d objects (%s), "
        "nextStartWith=%r"
        % (round_index, written, listed,
           "complete" if listed == written else "SHORT BY %d" % (written - listed),
           parsed.get("nextStartWith", ""))
    )

print("\n== cleanup ==")


def sweep(prefix, recorded=()):
    """Delete what we recorded, then delete whatever the listing still shows.

    The second pass is the point: a residual count derived from the probe's own
    bookkeeping would only confirm that bookkeeping.
    """
    failures = 0
    for key in recorded:
        status, _, _ = delete(key)
        if status not in (200, 204, 404):
            failures += 1
    _, _, body = listing(prefix)
    leftovers = [item["name"] for item in parse(body).get("objects", [])]
    for key in leftovers:
        status, _, _ = delete(key)
        if status not in (200, 204, 404):
            failures += 1
    _, _, body = listing(prefix)
    return failures, len(leftovers), len(parse(body).get("objects", []))


failures, swept, residual = sweep(PREFIX, created)
print("  deleted %d recorded objects (+%d found by listing), %d failures"
      % (len(created), swept, failures))
print("  residual objects under prefix: %d" % residual)
print("  tenancy declines (404 BucketNotFound, re-issued): %d" % DECLINES["count"])
if DECLINES["count"]:
    print("  ^ an environment observation, not a cell: this tenancy intermittently")
    print("    refuses a valid request against an existing bucket. Every measurement")
    print("    above was re-issued past it; the race totals below include the raw")
    print("    declines because a re-issued racer would not be a racer.")

# An interrupted earlier run can be cleaned up by naming its prefix here; the
# probe refuses to sweep anything outside `kythira-real-test/`.
for stale_prefix in [p for p in os.environ.get("KYTHIRA_PROBE_SWEEP", "").split(",") if p]:
    if not stale_prefix.startswith("kythira-real-test/"):
        raise SystemExit("refusing to sweep %r: not under kythira-real-test/" % stale_prefix)
    failures, swept, residual = sweep(stale_prefix)
    print("  swept %s: %d objects deleted, %d failures, %d residual"
          % (stale_prefix, swept, failures, residual))
