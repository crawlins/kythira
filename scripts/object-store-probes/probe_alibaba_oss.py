#!/usr/bin/env python3
"""Task 0 probes for Alibaba OSS (.kiro/specs/cloud-object-persistence/).

Answers, against the live service rather than the documentation:

  0. is bucket versioning on? (it silently disables x-oss-forbid-overwrite)
  1. create-only precondition — x-oss-forbid-overwrite
  2. overwrite CAS — If-Match. THE cell: it decides whether OSS can be fenced
  3. create-only via If-None-Match: *
  4. conditional DELETE — If-Match
  5. Content-MD5 end-to-end checksum, and ETag determinism
  6. list-after-write, immediately, under a fresh prefix

Every object it writes is under `spike/objstore-cond-<epoch>/` and is deleted
before it exits; the last thing it prints is the residual count, so a failed
cleanup is visible rather than silent.

Usage:
    KYTHIRA_ALIBABA_OSS_BUCKET=<bucket> [KYTHIRA_ALIBABA_REGION=<region>] \\
        python3 scripts/object-store-probes/probe_alibaba_oss.py
"""
import base64
import hashlib
import time

from oss_probe import BUCKET, REGION, code_of, report, request, xml_value

PREFIX = "spike/objstore-cond-%d/" % int(time.time())
print("bucket=%s region=%s\nprefix=%s\n" % (BUCKET, REGION, PREFIX))
created = []


def put(key, body, headers=None):
    created.append(key)
    return request("PUT", key, headers=headers or {}, body=body)


def classify_precondition(status, body, landed_bytes, bytes_if_write_did_not_happen):
    """Turn a conditional write's outcome into a verdict.

    Deliberately keyed on the STATUS CODE, not on whether the bytes changed.
    A probe that asks only "did the object change?" cannot tell a precondition
    that was evaluated and rejected from a header the service never
    implemented — and would record a fence that does not exist. That mistake
    was made once here, live, and is why this function exists.
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


print("== 0. bucket versioning status (silently disables x-oss-forbid-overwrite) ==")
status, _, body = request("GET", "", query={"versioning": ""})
print("  GET ?versioning -> HTTP %s  Status=%s" % (status, xml_value(body, "Status") or "unset"))

print("\n== 1. create-only: x-oss-forbid-overwrite ==")
key = PREFIX + "forbid"
status, headers, body = put(key, b"first", {"x-oss-forbid-overwrite": "true"})
report("PUT absent key, forbid-overwrite:true", status, headers, body)
status, headers, body = put(key, b"second", {"x-oss-forbid-overwrite": "true"})
report("PUT existing key, forbid-overwrite:true", status, headers, body)
rejected_status, rejected_body = status, body
status, headers, landed = request("GET", key)
report("GET (did the second PUT land?)", status, headers, landed, extra=repr(landed))
print("  VERDICT: %s" % classify_precondition(rejected_status, rejected_body, landed, b"first"))

print("\n== 2. overwrite CAS: If-Match  [THE decisive cell] ==")
key = PREFIX + "ifmatch"
status, headers, body = put(key, b"v1")
etag_v1 = headers.get("ETag", "")
report("PUT initial", status, headers, body)
status, headers, body = put(key, b"v2-correct-etag", {"If-Match": etag_v1})
report("PUT If-Match: <current etag>", status, headers, body)
current_etag_status, current_etag_body = status, body
status, headers, body = put(key, b"v3-stale-etag", {"If-Match": etag_v1})
report("PUT If-Match: <stale etag>", status, headers, body)
stale_status, stale_body = status, body
status, headers, landed = request("GET", key)
report("GET (what is actually stored now?)", status, headers, landed, extra=repr(landed))
# 4th argument = what would be stored had the write NOT happened.
print("  correct-ETag write: %s" % classify_precondition(current_etag_status, current_etag_body,
                                                         landed, b"v1"))
print("  stale-ETag  write: %s" % classify_precondition(stale_status, stale_body, landed, landed))
print(
    "  VERDICT: overwrite CAS is %s"
    % (
        "USABLE"
        if current_etag_status < 300 and stale_status in (409, 412)
        else "NOT USABLE — Requirement 9.8 applies"
    )
)

print("\n== 3. create-only: If-None-Match: * ==")
key = PREFIX + "ifnonematch"
status, headers, body = put(key, b"first", {"If-None-Match": "*"})
report("PUT absent key, If-None-Match: *", status, headers, body)
status, headers, body = put(key, b"second", {"If-None-Match": "*"})
report("PUT existing key, If-None-Match: *", status, headers, body)
rejected_status, rejected_body = status, body
status, headers, landed = request("GET", key)
report("GET (did the second PUT land?)", status, headers, landed, extra=repr(landed))
print("  VERDICT: %s" % classify_precondition(rejected_status, rejected_body, landed, b"first"))

print("\n== 4. conditional DELETE: If-Match ==")
key = PREFIX + "cond-delete"
status, headers, body = put(key, b"deleteme")
etag = headers.get("ETag", "")
status, headers, body = request(
    "DELETE", key, headers={"If-Match": '"00000000000000000000000000000000"'}
)
report("DELETE If-Match: <stale etag>", status, headers, body)
stale_delete_status, stale_delete_body = status, body
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
request("DELETE", key, headers={"If-Match": etag})

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
    % ("VERIFIED by the service" if status == 400 else "NOT verified (HTTP %s)" % status)
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
    status, _, body = request(
        "GET", "", query={"list-type": "2", "prefix": round_prefix, "encoding-type": "url"}
    )
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
status, _, body = request(
    "GET", "", query={"list-type": "2", "prefix": PREFIX, "encoding-type": "url"}
)
print("  deleted %d objects, %d failures" % (len(created), failures))
print("  residual objects under prefix: %d" % body.count(b"<Key>"))
