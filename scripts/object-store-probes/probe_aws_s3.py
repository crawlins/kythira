#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Task 0 probes for AWS S3 (.kiro/specs/cloud-object-persistence/).

  1. create-only precondition — If-None-Match: *
  2. overwrite CAS — If-Match
  3. conditional DELETE — If-Match
  4. **412 vs 409** under real contention — THE cell: 412 `PreconditionFailed`
     means "you lost, a second writer exists" and must latch the fence; 409
     `ConditionalRequestConflict` means "two conditional requests raced, retry"
     and must NOT. Collapsing them either way is a correctness bug, so this
     probe races concurrent conditional PUTs at one key and reports the exact
     distribution of status/error codes observed.
  5. Content-MD5 end-to-end checksum, and ETag determinism
  6. list-after-write, immediately (S3 documents it; this is the confirming run)

Everything is written under `kythira-real-test/probe-<epoch>/` and deleted
before exit; the last line is the residual count.

Usage:
    KYTHIRA_S3_BUCKET=<bucket> [KYTHIRA_AWS_PROFILE=personal] \\
        python3 scripts/object-store-probes/probe_aws_s3.py
"""
import base64
import hashlib
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

from aws_probe import BUCKET, REGION, code_of, report, request, xml_value

PREFIX = "kythira-real-test/probe-%d/" % int(time.time())
print("bucket=%s region=%s\nprefix=%s\n" % (BUCKET, REGION, PREFIX))
created = []


def put(key, body, headers=None):
    created.append(key)
    return request("PUT", key, headers=headers or {}, body=body)


def classify_precondition(status, body, landed_bytes, bytes_if_write_did_not_happen):
    """Same rule as the OSS probe: key on the status code, never on the effect.

    An effect-based check cannot tell a precondition that was evaluated and
    rejected from a header the service never implemented.
    """
    code = code_of(body)
    if status in (409, 412):
        return "ENFORCED (rejected %s %s)" % (status, code)
    if status in (400, 501) and code in ("NotImplemented", "NotSupported"):
        return "UNSUPPORTED (%s %s) — the header is not implemented, not evaluated" % (status, code)
    if 200 <= status < 300 and landed_bytes == bytes_if_write_did_not_happen:
        return "SILENTLY IGNORED (%s) — accepted and disregarded" % status
    if 200 <= status < 300:
        return "ACCEPTED (%s)" % status
    return "UNEXPECTED (%s %s)" % (status, code)


print("== 1. create-only: If-None-Match: * ==")
key = PREFIX + "ifnonematch"
status, headers, body = put(key, b"first", {"If-None-Match": "*"})
report("PUT absent key, If-None-Match: *", status, headers, body)
status, headers, body = put(key, b"second", {"If-None-Match": "*"})
report("PUT existing key, If-None-Match: *", status, headers, body)
rejected_status, rejected_body = status, body
status, headers, landed = request("GET", key)
report("GET (did the second PUT land?)", status, headers, landed, extra=repr(landed))
print("  VERDICT: %s" % classify_precondition(rejected_status, rejected_body, landed, b"first"))

print("\n== 2. overwrite CAS: If-Match ==")
key = PREFIX + "ifmatch"
status, headers, body = put(key, b"v1")
etag_v1 = headers.get("ETag", "")
report("PUT initial", status, headers, body)
status, headers, body = put(key, b"v2-correct-etag", {"If-Match": etag_v1})
report("PUT If-Match: <current etag>", status, headers, body)
correct_status, correct_body = status, body
status, headers, body = put(key, b"v3-stale-etag", {"If-Match": etag_v1})
report("PUT If-Match: <stale etag>", status, headers, body)
stale_status, stale_body = status, body
status, headers, landed = request("GET", key)
report("GET (what is stored now?)", status, headers, landed, extra=repr(landed))
# The 4th argument is what would be stored had the write NOT happened — i.e.
# the PREVIOUS content. Passing the new content here would make a successful
# write read as "silently ignored", which is exactly how this probe mislabelled
# S3's working CAS on its first run.
print("  correct-ETag write: %s" % classify_precondition(correct_status, correct_body, landed,
                                                         b"v1"))
print("  stale-ETag  write: %s" % classify_precondition(stale_status, stale_body, landed, landed))
print(
    "  VERDICT: overwrite CAS is %s"
    % (
        "USABLE"
        if correct_status < 300 and stale_status in (409, 412)
        else "NOT USABLE — Requirement 9.8 would apply"
    )
)

print("\n== 3. conditional DELETE: If-Match ==")
key = PREFIX + "cond-delete"
status, headers, body = put(key, b"deleteme")
etag = headers.get("ETag", "")
status, headers, body = request(
    "DELETE", key, headers={"If-Match": '"00000000000000000000000000000000"'}
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
        if stale_delete_status in (409, 412) or survived
        else "SILENTLY IGNORED — the stale precondition did not stop the delete"
    )
)
status, headers, body = request("DELETE", key, headers={"If-Match": etag})
report("DELETE If-Match: <current etag>", status, headers, body)

print("\n== 4. 412 vs 409 under contention  [THE decisive cell] ==")
print("  Racing concurrent If-None-Match:* PUTs at one fresh key per round.")
print("  Exactly one winner is expected; the losers reveal which codes S3 uses.")
observed = Counter()
for round_index, workers in enumerate((8, 16, 32)):
    race_key = "%srace-%d" % (PREFIX, round_index)
    created.append(race_key)

    def contend(n, k=race_key):
        st, _, bd = request("PUT", k, headers={"If-None-Match": "*"}, body=b"racer-%d" % n)
        return st, code_of(bd)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(contend, range(workers)))
    counts = Counter("%s %s" % (st, code or "-") for st, code in results)
    observed.update(counts)
    print("  %2d concurrent PUTs -> %s" % (workers, dict(counts)))

winners = sum(c for k, c in observed.items() if k.startswith("200"))
print("  totals: %s" % dict(observed))
print("  winners across all rounds: %d (one per round is correct)" % winners)
conflict_seen = any(k.startswith("409") for k in observed)
precondition_seen = any(k.startswith("412") for k in observed)
print("  412 PreconditionFailed observed:      %s" % precondition_seen)
print("  409 ConditionalRequestConflict observed: %s%s" % (
    conflict_seen,
    "" if conflict_seen else "  (documented, but not elicited here — do NOT conclude it cannot happen)",
))

print("\n== 5. Content-MD5 and ETag determinism ==")
key = PREFIX + "md5"
payload = b"checksum-me" * 10
good = base64.b64encode(hashlib.md5(payload).digest()).decode()
bad = base64.b64encode(hashlib.md5(b"different").digest()).decode()
status, headers, body = put(key, payload, {"Content-MD5": good})
report("PUT correct Content-MD5", status, headers, body)
etag = headers.get("ETag", "").strip('"')
print("  ETag == md5 hex of content? %s" % (etag.lower() == hashlib.md5(payload).hexdigest()))
status, headers, body = put(PREFIX + "md5-bad", payload, {"Content-MD5": bad})
report("PUT wrong Content-MD5", status, headers, body)
print(
    "  VERDICT: end-to-end checksum is %s"
    % ("VERIFIED by the service" if status >= 400 else "NOT verified (HTTP %s)" % status)
)

print("\n== 6. list-after-write, immediately, 3 rounds x 25 objects ==")
for round_index in range(3):
    round_prefix = "%slaw-%d/" % (PREFIX, round_index)
    for i in range(25):
        key = "%s%020d" % (round_prefix, i)
        created.append(key)
        status, _, _ = request("PUT", key, body=b"x")
        if status != 200:
            print("  round %d: PUT %s failed HTTP %s" % (round_index, key, status))
    status, _, body = request("GET", "", query={"list-type": "2", "prefix": round_prefix})
    print(
        "  round %d: LIST immediately after 25 PUTs -> %d keys, IsTruncated=%s"
        % (round_index, body.count(b"<Key>"), xml_value(body, "IsTruncated"))
    )

print("\n== cleanup ==")
failures = 0
for key in created:
    status, _, _ = request("DELETE", key)
    if status not in (200, 204):
        failures += 1
status, _, body = request("GET", "", query={"list-type": "2", "prefix": PREFIX})
print("  deleted %d objects, %d failures" % (len(created), failures))
print("  residual objects under prefix: %d" % body.count(b"<Key>"))
