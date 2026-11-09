#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Check the checked-in wire evidence (ctest kinesis_wire_evidence).

Every file in data/ must name its client, record only operations the pinned
model has (or STS calls, spelled sts:<Action>), show its scenario finishing
(every scripted request answered 200), and use only the protocols README.md
lists for it. A hand edit or a stale file that breaks any of that fails.

    check_evidence.py MODEL DATA_DIR
"""

import json
import pathlib
import sys

EXPECTED = {
    "awscli-v2.json": {"application/x-amz-json-1.1"},
    "boto3.json": {"application/x-amz-json-1.1"},
    "java-sdk-v2.json": {"application/x-amz-cbor-1.1"},
    "kpl.json": {"application/x-amz-json-1.1", "application/x-www-form-urlencoded"},
}
VOLATILE_HEADERS = {"x-amz-date", "amz-sdk-invocation-id"}


def check(model_ops, path):
    problems = []
    doc = json.loads(path.read_text(encoding="utf-8"))
    if not doc.get("client"):
        problems.append("no client name")
    reqs = doc.get("requests", [])
    if not reqs:
        problems.append("no requests recorded")
    for i, r in enumerate(reqs):
        op = r.get("operation", "")
        if not (op in model_ops or op.startswith("sts:")):
            problems.append(f"request {i}: {op!r} is not in the pinned model")
        if r.get("response_status") != 200:
            problems.append(f"request {i}: {op} was answered {r.get('response_status')}, "
                            "so the scenario did not run as scripted")
        for name, value in r.get("headers", []):
            if name.lower() in VOLATILE_HEADERS and not value.startswith("<"):
                problems.append(f"request {i}: header {name} was not normalised")
            if name.lower() == "authorization" and "Signature=<hex>" not in value:
                problems.append(f"request {i}: signature was not normalised")
    types = {r.get("content_type") for r in reqs}
    want = EXPECTED.get(path.name)
    if want is None:
        problems.append("not listed in check_evidence.py's EXPECTED table")
    elif types != want:
        problems.append(f"content types {sorted(types)} != {sorted(want)}")
    return problems


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    model_ops = set(json.loads(pathlib.Path(sys.argv[1]).read_text())["operations"])
    data = pathlib.Path(sys.argv[2])
    failed = False
    files = sorted(data.glob("*.json"))
    for name in sorted(set(EXPECTED) - {f.name for f in files}):
        print(f"error: {name} is missing", file=sys.stderr)
        failed = True
    for f in files:
        for p in check(model_ops, f):
            print(f"error: {f.name}: {p}", file=sys.stderr)
            failed = True
    if not failed:
        print(f"{len(files)} evidence files ok")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
