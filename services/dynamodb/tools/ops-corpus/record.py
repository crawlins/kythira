#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Records the operations corpus against DynamoDB Local
(.kiro/specs/dynamodb-compatible-api/ task 8, Requirement 21.1).

    record.py ENDPOINT OUT.jsonl

For each scenario of cases.py: delete and create its tables, run its setup,
then run each step and store the answer as `expect`:

    {"error": {"type": ..., "message": ...}}   a refused request
    {"items": [...]}                           ScanAll and ScanSegments
    {"response": {...}}                        any other success, without
                                               ConsumedCapacity

Only the Python standard library is used. DynamoDB Local checks the form of
a SigV4 header but not the signature, so a fixed one serves.
"""

import copy
import json
import os
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(__file__))
import cases as corpus  # noqa: E402


def call(endpoint, op, body):
    req = urllib.request.Request(endpoint, data=json.dumps(body).encode(), method="POST", headers={
        "Content-Type": "application/x-amz-json-1.0",
        "X-Amz-Target": "DynamoDB_20120810." + op,
        "X-Amz-Date": "20261110T000000Z",
        "Authorization": "AWS4-HMAC-SHA256 Credential=corpus/20261110/us-east-1/dynamodb/aws4_request, "
                         "SignedHeaders=host;x-amz-date;x-amz-target, Signature=" + "0" * 64,
    })
    try:
        with urllib.request.urlopen(req) as r:
            return 200, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def error_of(r):
    return {"error": {"type": r.get("__type", "").split("#")[-1],
                      "message": r.get("message", r.get("Message"))}}


def scan_all(endpoint, inp):
    items = []
    body = copy.deepcopy(inp)
    while True:
        code, r = call(endpoint, "Scan", body)
        if code != 200:
            return error_of(r)
        items.extend(r.get("Items", []))
        if "LastEvaluatedKey" not in r:
            return {"items": items}
        body["ExclusiveStartKey"] = r["LastEvaluatedKey"]


def run_scenario(endpoint, sc):
    for t in sc["tables"]:
        call(endpoint, "DeleteTable", {"TableName": t["TableName"]})
        code, r = call(endpoint, "CreateTable", t)
        if code != 200:
            raise SystemExit("CreateTable failed: " + json.dumps(r))
    for s in sc["setup"]:
        code, r = call(endpoint, s["op"], s["input"])
        if code != 200:
            raise SystemExit("setup failed in %s: %s" % (sc["name"], json.dumps(r)))
    previous = None
    for st in sc["steps"]:
        inp = copy.deepcopy(st["input"])
        if st.get("continue"):
            lek = (previous or {}).get("response", {}).get("LastEvaluatedKey")
            if lek is None:
                raise SystemExit("continue step without a previous LastEvaluatedKey in " + sc["name"])
            inp["ExclusiveStartKey"] = lek
        if st["op"] == "ScanAll":
            st["expect"] = scan_all(endpoint, inp)
        elif st["op"] == "ScanSegments":
            items = []
            result = None
            for seg in range(st["segments"]):
                part = scan_all(endpoint, {**inp, "Segment": seg, "TotalSegments": st["segments"]})
                if "error" in part:
                    result = part
                    break
                items.extend(part["items"])
            st["expect"] = result or {"items": items}
        else:
            code, r = call(endpoint, st["op"], inp)
            if code != 200:
                st["expect"] = error_of(r)
            else:
                r.pop("ConsumedCapacity", None)
                st["expect"] = {"response": r}
        previous = st["expect"]
    for t in sc["tables"]:
        call(endpoint, "DeleteTable", {"TableName": t["TableName"]})
    return sc


def main():
    endpoint, out_path = sys.argv[1], sys.argv[2]
    with open(out_path, "w", encoding="utf-8") as out:
        for sc in corpus.all_scenarios():
            out.write(json.dumps(run_scenario(endpoint, sc), ensure_ascii=False, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
