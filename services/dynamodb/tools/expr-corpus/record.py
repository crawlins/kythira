#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Records the expression corpus against DynamoDB Local
(.kiro/specs/dynamodb-compatible-api/ task 5, Requirement 21.3).

    record.py ENDPOINT OUT.jsonl

For each case of cases.py: reset the item (delete it, then put it when the
case has one), send the request, and store what came back as `expect`:

    {"error": {"type": "ValidationException", "message": "..."}}
    {"condition_failed": true}
    {"item": {...} | null}          GetItem, UpdateItem (ALL_NEW), DeleteItem
    {"items": [...]}                Query, Scan

Only the Python standard library is used. DynamoDB Local checks the form of
a SigV4 header but not the signature, so a fixed one serves.
"""

import json
import sys
import urllib.error
import urllib.request

sys.path.insert(0, __import__("os").path.dirname(__file__))
import cases as corpus  # noqa: E402

TABLE = "expr_corpus"
KEY = {"pk": {"S": "k"}, "sk": {"N": "1"}}


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


def ensure_table(endpoint):
    call(endpoint, "DeleteTable", {"TableName": TABLE})
    code, r = call(endpoint, "CreateTable", {
        "TableName": TABLE,
        "KeySchema": [{"AttributeName": "pk", "KeyType": "HASH"}, {"AttributeName": "sk", "KeyType": "RANGE"}],
        "AttributeDefinitions": [{"AttributeName": "pk", "AttributeType": "S"},
                                 {"AttributeName": "sk", "AttributeType": "N"}],
        "BillingMode": "PAY_PER_REQUEST"})
    if code != 200:
        raise SystemExit("CreateTable failed: " + json.dumps(r))


def run_case(endpoint, case):
    call(endpoint, "DeleteItem", {"TableName": TABLE, "Key": KEY})
    if case["item"] is not None:
        code, r = call(endpoint, "PutItem", {"TableName": TABLE, "Item": {**KEY, **case["item"]}})
        if code != 200:
            raise SystemExit("PutItem failed for case %d: %s" % (case["id"], json.dumps(r)))
    op = case["op"]
    body = {"TableName": TABLE, **case["input"]}
    if op in ("GetItem", "DeleteItem", "UpdateItem"):
        body["Key"] = KEY
    if op == "DeleteItem":
        body["ReturnValues"] = "ALL_OLD"
    if op == "PutItem":
        body.setdefault("Item", KEY)
    code, r = call(endpoint, op, body)
    if code != 200:
        typ = r.get("__type", "").split("#")[-1]
        if typ == "ConditionalCheckFailedException":
            return {"condition_failed": True}
        return {"error": {"type": typ, "message": r.get("message", r.get("Message"))}}
    if op in ("Query", "Scan"):
        return {"items": r.get("Items", [])}
    if op == "GetItem":
        return {"item": r.get("Item")}
    if op == "UpdateItem":
        return {"item": r.get("Attributes")}
    return {"item": None}


def main():
    endpoint, out_path = sys.argv[1], sys.argv[2]
    ensure_table(endpoint)
    with open(out_path, "w", encoding="utf-8") as out:
        for case in corpus.all_cases():
            case["expect"] = run_case(endpoint, case)
            out.write(json.dumps(case, ensure_ascii=False, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
