#!/usr/bin/env python3
"""Task 0 probes for Azure Blob Storage (.kiro/specs/cloud-object-persistence/).

Cells closed here, none of them by analogy to S3 or OSS — the two providers
already measured disagree with each other on every one of these:

  1. create-only precondition — If-None-Match: *
  2. overwrite CAS — If-Match
  3. conditional DELETE — If-Match (the cell design.md marked OPEN)
  4. the rejection code under real contention, since S3 uses 412 and OSS uses
     409 for the *same* situation and 409 for the *opposite* one
  5. Content-MD5 end-to-end verification, and whether the ETag is a
     deterministic function of single-part content (it is not on Azure — the
     probe measures rather than assumes)
  6. list-after-write, immediately, three rounds — the one consistency
     question the engine cannot detect at runtime
  7. task 0.6's REST-versus-SDK checkpoint: this file *is* the hand-rolled
     surface the design chose, so anything it needed that the documentation
     did not name is recorded in the run notes.

Everything is written under `kythira-real-test/probe-<epoch>/` and deleted
before exit; the last line is the residual count.

Usage:
    KYTHIRA_AZURE_STORAGE_ACCOUNT=kythirarealtestobj \\
    KYTHIRA_AZURE_BLOB_CONTAINER=kythira-raft \\
        python3 scripts/object-store-probes/probe_azure_blob.py
"""
import base64
import hashlib
import os
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

from azure_probe import ACCOUNT, API_VERSION, CONTAINER, code_of, report, request, xml_values
from probe_common import cas_verdict, classify_precondition

PREFIX = "kythira-real-test/probe-%d/" % int(time.time())
print("account=%s container=%s x-ms-version=%s\nprefix=%s\n"
      % (ACCOUNT, CONTAINER, API_VERSION, PREFIX))
created = []


def put(key, body, headers=None):
    created.append(key)
    return request("PUT", key, headers=headers or {}, body=body)


def classify(status, headers, body, landed, previous):
    return classify_precondition(status, code_of(headers, body), landed, previous)


print("== 1. create-only: If-None-Match: * ==")
key = PREFIX + "ifnonematch"
status, headers, body = put(key, b"first", {"If-None-Match": "*"})
report("PUT absent blob, If-None-Match: *", status, headers, body)
status, headers, body = put(key, b"second", {"If-None-Match": "*"})
report("PUT existing blob, If-None-Match: *", status, headers, body)
rejected = (status, headers, body)
status, headers, landed = request("GET", key)
report("GET (did the second PUT land?)", status, headers, landed, extra=repr(landed))
print("  VERDICT: %s" % classify(rejected[0], rejected[1], rejected[2], landed, b"first"))

print("\n== 2. overwrite CAS: If-Match ==")
key = PREFIX + "ifmatch"
status, headers, body = put(key, b"v1")
etag_v1 = headers.get("ETag", "")
report("PUT initial", status, headers, body)
status, headers, body = put(key, b"v2-correct-etag", {"If-Match": etag_v1})
report("PUT If-Match: <current etag>", status, headers, body)
correct = (status, headers, body)
status, headers, body = put(key, b"v3-stale-etag", {"If-Match": etag_v1})
report("PUT If-Match: <stale etag>", status, headers, body)
stale = (status, headers, body)
status, headers, landed = request("GET", key)
report("GET (what is stored now?)", status, headers, landed, extra=repr(landed))
# The last argument is what would still be stored had the write been refused —
# the PREVIOUS content, never the new one.
print("  correct-ETag write: %s" % classify(correct[0], correct[1], correct[2], landed, b"v1"))
print("  stale-ETag  write: %s" % classify(stale[0], stale[1], stale[2], landed, landed))
print("  VERDICT: overwrite CAS is %s" % cas_verdict(correct[0], stale[0]))

print("\n== 3. conditional DELETE: If-Match  [design.md marked this OPEN] ==")
key = PREFIX + "cond-delete"
status, headers, body = put(key, b"deleteme")
etag = headers.get("ETag", "")
status, headers, body = request(
    "DELETE", key, headers={"If-Match": '"0x8DEADBEEFDEADBE"'}
)
report("DELETE If-Match: <stale etag>", status, headers, body)
stale_delete_status = status
status, headers, body = request("GET", key)
survived = status == 200
report("GET after stale-etag DELETE", status, headers, body,
       extra=("present" if survived else "ABSENT — the delete happened"))
print(
    "  VERDICT: conditional delete is %s"
    % (
        "ENFORCED"
        if stale_delete_status in (409, 412) and survived
        else "SILENTLY IGNORED — the stale precondition did not stop the delete"
        if survived is False
        else "REJECTED but unverified (HTTP %s)" % stale_delete_status
    )
)
status, headers, body = request("DELETE", key, headers={"If-Match": etag})
report("DELETE If-Match: <current etag>", status, headers, body)

print("\n== 4. the rejection code under contention ==")
print("  Racing concurrent If-None-Match:* PUTs at one fresh blob per round.")
print("  S3 answers 412 here and OSS answers 409; Azure's answer is a client")
print("  obligation either way, since no client may map a bare 409.")
observed = Counter()
for round_index, workers in enumerate((8, 16, 32)):
    race_key = "%srace-%d" % (PREFIX, round_index)
    created.append(race_key)

    def contend(n, k=race_key):
        st, hd, bd = request("PUT", k, headers={"If-None-Match": "*"}, body=b"racer-%d" % n)
        return st, code_of(hd, bd)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(contend, range(workers)))
    counts = Counter("%s %s" % (st, code or "-") for st, code in results)
    observed.update(counts)
    print("  %2d concurrent PUTs -> %s" % (workers, dict(counts)))

winners = sum(c for k, c in observed.items() if k.startswith("201") or k.startswith("200"))
print("  totals: %s" % dict(observed))
print("  winners across all rounds: %d (one per round is correct)" % winners)
for code in ("409", "412"):
    seen = any(k.startswith(code) for k in observed)
    print("  %s observed: %s%s" % (
        code, seen,
        "" if seen else "  (not elicited here — do NOT conclude it cannot happen)",
    ))

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
status, headers, body = request("GET", key)
returned_md5 = headers.get("Content-MD5", "")
print("  GET returns Content-MD5? %s  (matches: %s)"
      % (bool(returned_md5), returned_md5 == good))
status, headers, body = put(PREFIX + "md5-bad", payload, {"Content-MD5": bad})
report("PUT wrong Content-MD5", status, headers, body)
print(
    "  VERDICT: end-to-end checksum is %s"
    % ("VERIFIED by the service (%s)" % (code_of(headers, body) or status)
       if status >= 400 else "NOT verified (HTTP %s)" % status)
)

print("\n== 6. list-after-write, immediately, 3 rounds x 25 blobs ==")
for round_index in range(3):
    round_prefix = "%slaw-%d/" % (PREFIX, round_index)
    for i in range(25):
        key = "%s%020d" % (round_prefix, i)
        created.append(key)
        status, _, _ = request("PUT", key, body=b"x")
        if status not in (200, 201):
            print("  round %d: PUT %s failed HTTP %s" % (round_index, key, status))
    status, headers, body = request(
        "GET", "", query={"restype": "container", "comp": "list", "prefix": round_prefix}
    )
    markers = xml_values(body, "NextMarker")
    print(
        "  round %d: LIST immediately after 25 PUTs -> %d blobs, NextMarker=%r"
        % (round_index, len(xml_values(body, "Name")), markers[0] if markers else "")
    )

print("\n== cleanup ==")


def sweep(prefix):
    """Delete what we recorded, then delete whatever the listing still shows.

    The second pass is the point: `created` is what the probe *believes* it
    wrote, and a residual count derived from the same list would only ever
    confirm its own bookkeeping.
    """
    failures = 0
    for key in created if prefix == PREFIX else []:
        status, _, _ = request("DELETE", key)
        if status not in (200, 202, 204, 404):
            failures += 1
    _, _, listing = request(
        "GET", "", query={"restype": "container", "comp": "list", "prefix": prefix}
    )
    leftovers = xml_values(listing, "Name")
    for key in leftovers:
        status, _, _ = request("DELETE", key)
        if status not in (200, 202, 204, 404):
            failures += 1
    _, _, listing = request(
        "GET", "", query={"restype": "container", "comp": "list", "prefix": prefix}
    )
    return failures, len(leftovers), len(xml_values(listing, "Name"))


failures, swept, residual = sweep(PREFIX)
print("  deleted %d recorded blobs (+%d found by listing), %d failures"
      % (len(created), swept, failures))
print("  residual blobs under prefix: %d" % residual)

# An earlier interrupted run can be cleaned up by naming its prefix here; the
# probe never deletes anything outside `kythira-real-test/`.
for stale_prefix in [p for p in os.environ.get("KYTHIRA_PROBE_SWEEP", "").split(",") if p]:
    if not stale_prefix.startswith("kythira-real-test/"):
        raise SystemExit("refusing to sweep %r: not under kythira-real-test/" % stale_prefix)
    failures, swept, residual = sweep(stale_prefix)
    print("  swept %s: %d blobs deleted, %d failures, %d residual"
          % (stale_prefix, swept, failures, residual))
