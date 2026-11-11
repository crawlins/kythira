#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""A recording stub for DynamoDB, DynamoDB Streams and Kinesis clients.

`.kiro/specs/dynamodb-compatible-api/` task 1: the wire evidence for the
AWS CLI, boto3, aws-sdk-java-v2, KCL 2.x and 3.x, the DynamoDB lock client
and the DynamoDB Streams Kinesis Adapter is recorded against this stub, as
`redis-compatible-kv` recorded sccache. It answers just enough of each API,
from an in-memory table store, for those clients to run their normal flows,
and writes every request it gets to a JSON-lines file, one file per client.

It is evidence tooling, not an emulator: conditions are evaluated only in
the simple forms the recorded clients use (attribute_exists,
attribute_not_exists, equality, the legacy Expected forms), update
expressions only as SET, REMOVE and ADD of plain paths, and nothing is
validated. Behaviour fidelity is the job of the differential tests against
DynamoDB Local (task 8), not of this file.

Which client sent a request is read from the access key id in its SigV4
`Authorization` header: each client is run with the access key
`EVIDENCE-<client>`, so one stub can serve several clients at once.

What is recorded per request: the X-Amz-Target service and operation, the
HTTP method and path, the request body as parsed JSON, every header name,
and the values of the headers that shape the protocol (content type,
encoding, the SigV4 form with the signature and dates masked, the SDK's
retry headers). The user agent is kept, since it names the SDK and its
version.

With `--dynamodb-upstream HOST:PORT` the stub keeps serving Kinesis itself
but passes every DynamoDB and DynamoDB Streams request, bytes and signed
headers unchanged, to that server (a Kythira gateway node or DynamoDB
Local) and relays its answer; each exchange is also written to
`<client>.upstream.jsonl` with the upstream's status and body. That is how
the KCL clients run on Kythira's DynamoDB API with a stand-in Kinesis
stream (kcl_on_kythira.sh, task 11), and the Streams Kinesis Adapter reads
a real Kythira stream (task 16).

Usage:
    recording_stub.py --port 8000 --out DIR [--dynamodb-upstream HOST:PORT]
"""

import argparse
import base64
import copy
import http.client
import json
import os
import re
import socket
import sys
import threading
import time
import uuid
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DDB = "DynamoDB_20120810"
STREAMS = "DynamoDBStreams_20120810"
KINESIS = "Kinesis_20131202"
ACCOUNT = "123456789012"
REGION = "us-east-1"

# Headers whose values are protocol evidence. Others are recorded by name.
KEEP_VALUES = {
    "content-type",
    "x-amz-target",
    "accept-encoding",
    "content-encoding",
    "accept",
    "expect",
    "amz-sdk-request",
    "x-amz-content-sha256",
    "user-agent",
    "x-amz-user-agent",
    "x-amz-account-id",
    "x-amzn-query-mode",
}
AUTH_RE = re.compile(
    r"^AWS4-HMAC-SHA256 Credential=([^/]+)/(\d{8})/([^/]+)/([^/]+)/aws4_request,\s*"
    r"SignedHeaders=([^,]+),\s*Signature=([0-9a-f]+)$"
)


def client_of(headers):
    m = AUTH_RE.match(headers.get("authorization", ""))
    if not m:
        return "unsigned", None
    key = m.group(1)
    return (key[len("EVIDENCE-") :] if key.startswith("EVIDENCE-") else "other"), m


UUID_RE = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")


def record_body(body):
    """The body as recorded: an SDK-generated idempotency token (a random
    UUID) becomes a fixed placeholder, so recordings are reproducible and
    secret scanners do not mistake it for a credential."""
    token = body.get("ClientRequestToken") if isinstance(body, dict) else None
    if isinstance(token, str) and UUID_RE.fullmatch(token):
        return {**body, "ClientRequestToken": "<generated uuid>"}
    return body


def record_headers(headers, auth_match):
    kept = {}
    for k, v in headers.items():
        if k in KEEP_VALUES:
            kept[k] = v
    sha = kept.get("x-amz-content-sha256", "")
    if re.fullmatch(r"[0-9a-f]{64}", sha):
        kept["x-amz-content-sha256"] = "<sha256 of the body>"
    if auth_match:
        kept["authorization"] = (
            "AWS4-HMAC-SHA256 Credential=<key>/<date>/%s/%s/aws4_request, "
            "SignedHeaders=%s, Signature=<signature>"
            % (auth_match.group(3), auth_match.group(4), auth_match.group(5))
        )
    return kept


# --------------------------------------------------------------------------
# A small item store
# --------------------------------------------------------------------------


def key_schema(desc):
    return [k["AttributeName"] for k in desc["KeySchema"]]


def item_key(desc, item, schema=None):
    names = schema or key_schema(desc)
    return tuple(json.dumps(item.get(n), sort_keys=True) for n in names)


def resolve(path, names):
    return names.get(path, path) if path.startswith("#") else path


def number(v):
    return float(v) if "." in v or "e" in v.lower() else int(v)


def eval_condition(expr, item, names, values):
    """The simple forms the recorded clients use; True for anything else."""
    if not expr:
        return True
    ok = True
    for clause in re.split(r"\s+(?:AND|and)\s+", expr.strip()):
        clause = clause.strip().strip("()").strip()
        m = re.match(r"attribute_not_exists\s*\(\s*([#\w.]+)\s*\)$", clause)
        if m:
            ok = ok and resolve(m.group(1), names) not in item
            continue
        m = re.match(r"attribute_exists\s*\(\s*([#\w.]+)\s*\)$", clause)
        if m:
            ok = ok and resolve(m.group(1), names) in item
            continue
        m = re.match(r"([#\w.]+)\s*(=|<>)\s*(:\w+)$", clause)
        if m:
            cur = item.get(resolve(m.group(1), names))
            same = cur == values.get(m.group(3))
            ok = ok and (same if m.group(2) == "=" else not same)
            continue
    return ok


def eval_expected(expected, item):
    for name, cond in (expected or {}).items():
        if "Exists" in cond and not cond["Exists"]:
            if name in item:
                return False
        elif "Value" in cond:
            if item.get(name) != cond["Value"]:
                return False
        elif cond.get("ComparisonOperator") == "NULL":
            if name in item:
                return False
        elif cond.get("ComparisonOperator") == "NOT_NULL":
            if name not in item:
                return False
        elif cond.get("ComparisonOperator") == "EQ":
            if item.get(name) != cond["AttributeValueList"][0]:
                return False
    return True


def add_values(cur, delta):
    if cur is None:
        return delta
    if "N" in delta:
        return {"N": str(number(cur["N"]) + number(delta["N"]))}
    for t in ("SS", "NS", "BS"):
        if t in delta:
            return {t: sorted(set(cur.get(t, [])) | set(delta[t]))}
    return delta


def split_top(body):
    """Split on commas outside parentheses."""
    out, depth, cur = [], 0, ""
    for ch in body:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return [c.strip() for c in out if c.strip()]


def apply_update(item, req):
    names = req.get("ExpressionAttributeNames", {})
    values = req.get("ExpressionAttributeValues", {})
    for name, upd in (req.get("AttributeUpdates") or {}).items():
        action = upd.get("Action", "PUT")
        if action == "PUT":
            item[name] = upd["Value"]
        elif action == "DELETE":
            item.pop(name, None)
        elif action == "ADD":
            item[name] = add_values(item.get(name), upd["Value"])
    expr = req.get("UpdateExpression")
    if not expr:
        return
    parts = re.split(r"\b(SET|REMOVE|ADD|DELETE)\b", expr)
    i = 1
    while i < len(parts):
        verb, body = parts[i], parts[i + 1]
        i += 2
        for clause in split_top(body):
            if verb == "SET":
                lhs, rhs = [x.strip() for x in clause.split("=", 1)]
                lhs = resolve(lhs, names)
                m = re.match(
                    r"if_not_exists\s*\(\s*([#\w]+)\s*,\s*(:\w+)\s*\)\s*(?:([+-])\s*(:\w+))?$",
                    rhs,
                )
                if m:
                    cur = item.get(resolve(m.group(1), names), values.get(m.group(2)))
                    if m.group(3):
                        d = values.get(m.group(4))
                        sign = 1 if m.group(3) == "+" else -1
                        cur = {"N": str(number(cur["N"]) + sign * number(d["N"]))}
                    item[lhs] = cur
                    continue
                m = re.match(r"([#\w]+)\s*([+-])\s*(:\w+)", rhs)
                if m:
                    cur = item.get(resolve(m.group(1), names), {"N": "0"})
                    d = values.get(m.group(3))
                    sign = 1 if m.group(2) == "+" else -1
                    item[lhs] = {"N": str(number(cur["N"]) + sign * number(d["N"]))}
                    continue
                if rhs in values:
                    item[lhs] = values[rhs]
            elif verb == "REMOVE":
                item.pop(resolve(clause, names), None)
            elif verb == "ADD":
                path, val = clause.split()
                path = resolve(path, names)
                item[path] = add_values(item.get(path), values.get(val))


def project(item, req, old=None):
    rv = req.get("ReturnValues", "NONE")
    if rv == "ALL_NEW":
        return item
    if rv in ("ALL_OLD", "UPDATED_OLD"):
        return old
    if rv == "UPDATED_NEW":
        return item
    return None


class Store:
    def __init__(self):
        self.lock = threading.Lock()
        self.tables = {}  # name -> {"desc":..., "items": {key: item}}
        self.kinesis_reads = {}

    # ---------------- DynamoDB ----------------

    def table(self, name):
        name = name.rsplit("/", 1)[-1] if name.startswith("arn:") else name
        t = self.tables.get(name)
        if t is None:
            raise ApiError(
                "com.amazonaws.dynamodb.v20120810#ResourceNotFoundException",
                "Requested resource not found",
            )
        return t

    def describe(self, t):
        d = copy.deepcopy(t["desc"])
        d["ItemCount"] = len(t["items"])
        d["TableSizeBytes"] = len(json.dumps(list(t["items"].values())))
        return d

    def ddb(self, op, req):
        h = getattr(self, "ddb_" + op, None)
        if h is None:
            return {}
        return h(req)

    def ddb_CreateTable(self, req):
        name = req["TableName"]
        if name in self.tables:
            raise ApiError(
                "com.amazonaws.dynamodb.v20120810#ResourceInUseException",
                "Table already exists: " + name,
            )
        now = time.time()
        desc = {
            "TableName": name,
            "TableArn": "arn:aws:dynamodb:%s:%s:table/%s" % (REGION, ACCOUNT, name),
            "TableId": str(uuid.uuid4()),
            "KeySchema": req["KeySchema"],
            "AttributeDefinitions": req.get("AttributeDefinitions", []),
            "CreationDateTime": now,
            "TableStatus": "ACTIVE",
            "ProvisionedThroughput": {
                "ReadCapacityUnits": req.get("ProvisionedThroughput", {}).get(
                    "ReadCapacityUnits", 0
                ),
                "WriteCapacityUnits": req.get("ProvisionedThroughput", {}).get(
                    "WriteCapacityUnits", 0
                ),
                "NumberOfDecreasesToday": 0,
            },
            "BillingModeSummary": {"BillingMode": req.get("BillingMode", "PROVISIONED")},
            "DeletionProtectionEnabled": req.get("DeletionProtectionEnabled", False),
        }
        gsis = []
        for g in req.get("GlobalSecondaryIndexes", []):
            gd = dict(g)
            gd["IndexStatus"] = "ACTIVE"
            gd["IndexArn"] = desc["TableArn"] + "/index/" + g["IndexName"]
            gd["ItemCount"] = 0
            gd["IndexSizeBytes"] = 0
            gsis.append(gd)
        if gsis:
            desc["GlobalSecondaryIndexes"] = gsis
        if "StreamSpecification" in req and req["StreamSpecification"].get("StreamEnabled"):
            desc["StreamSpecification"] = req["StreamSpecification"]
            desc["LatestStreamLabel"] = "2026-11-09T00:00:00.000"
            desc["LatestStreamArn"] = desc["TableArn"] + "/stream/" + desc["LatestStreamLabel"]
        self.tables[name] = {"desc": desc, "items": {}, "tags": req.get("Tags", [])}
        out = copy.deepcopy(desc)
        out["TableStatus"] = "CREATING"  # SDK waiters then see ACTIVE
        return {"TableDescription": out}

    def ddb_DescribeTable(self, req):
        return {"Table": self.describe(self.table(req["TableName"]))}

    def ddb_ListTables(self, req):
        return {"TableNames": sorted(self.tables)}

    def ddb_DeleteTable(self, req):
        t = self.table(req["TableName"])
        del self.tables[t["desc"]["TableName"]]
        d = self.describe(t)
        d["TableStatus"] = "DELETING"
        return {"TableDescription": d}

    def ddb_UpdateTable(self, req):
        t = self.table(req["TableName"])
        desc = t["desc"]
        names = {a["AttributeName"] for a in desc["AttributeDefinitions"]}
        for a in req.get("AttributeDefinitions", []):
            if a["AttributeName"] not in names:
                desc["AttributeDefinitions"].append(a)
        for u in req.get("GlobalSecondaryIndexUpdates", []):
            if "Create" in u:
                g = dict(u["Create"])
                g.update(
                    IndexStatus="ACTIVE",
                    IndexArn=desc["TableArn"] + "/index/" + g["IndexName"],
                    ItemCount=0,
                    IndexSizeBytes=0,
                )
                desc.setdefault("GlobalSecondaryIndexes", []).append(g)
            elif "Delete" in u:
                desc["GlobalSecondaryIndexes"] = [
                    g
                    for g in desc.get("GlobalSecondaryIndexes", [])
                    if g["IndexName"] != u["Delete"]["IndexName"]
                ]
        if "BillingMode" in req:
            desc["BillingModeSummary"] = {"BillingMode": req["BillingMode"]}
        return {"TableDescription": self.describe(t)}

    def ddb_DescribeTimeToLive(self, req):
        self.table(req["TableName"])
        return {"TimeToLiveDescription": {"TimeToLiveStatus": "DISABLED"}}

    def ddb_UpdateTimeToLive(self, req):
        self.table(req["TableName"])
        return {"TimeToLiveSpecification": req["TimeToLiveSpecification"]}

    def ddb_DescribeContinuousBackups(self, req):
        self.table(req["TableName"])
        return {
            "ContinuousBackupsDescription": {
                "ContinuousBackupsStatus": "ENABLED",
                "PointInTimeRecoveryDescription": {"PointInTimeRecoveryStatus": "DISABLED"},
            }
        }

    def ddb_UpdateContinuousBackups(self, req):
        self.table(req["TableName"])
        on = req["PointInTimeRecoverySpecification"]["PointInTimeRecoveryEnabled"]
        return {
            "ContinuousBackupsDescription": {
                "ContinuousBackupsStatus": "ENABLED",
                "PointInTimeRecoveryDescription": {
                    "PointInTimeRecoveryStatus": "ENABLED" if on else "DISABLED"
                },
            }
        }

    def ddb_TagResource(self, req):
        return {}

    def ddb_UntagResource(self, req):
        return {}

    def ddb_ListTagsOfResource(self, req):
        return {"Tags": []}

    def ddb_DescribeLimits(self, req):
        return {
            "AccountMaxReadCapacityUnits": 80000,
            "AccountMaxWriteCapacityUnits": 80000,
            "TableMaxReadCapacityUnits": 40000,
            "TableMaxWriteCapacityUnits": 40000,
        }

    def ddb_DescribeEndpoints(self, req):
        return {"Endpoints": [{"Address": "localhost", "CachePeriodInMinutes": 1440}]}

    def _check(self, req, cur):
        ok = eval_condition(
            req.get("ConditionExpression"),
            cur or {},
            req.get("ExpressionAttributeNames", {}),
            req.get("ExpressionAttributeValues", {}),
        ) and eval_expected(req.get("Expected"), cur or {})
        if not ok:
            err = ApiError(
                "com.amazonaws.dynamodb.v20120810#ConditionalCheckFailedException",
                "The conditional request failed",
            )
            if req.get("ReturnValuesOnConditionCheckFailure") == "ALL_OLD" and cur:
                err.extra["Item"] = cur
            raise err

    def ddb_PutItem(self, req):
        t = self.table(req["TableName"])
        k = item_key(t["desc"], req["Item"])
        cur = t["items"].get(k)
        self._check(req, cur)
        t["items"][k] = req["Item"]
        out = {}
        if req.get("ReturnValues") == "ALL_OLD" and cur:
            out["Attributes"] = cur
        return out

    def ddb_GetItem(self, req):
        t = self.table(req["TableName"])
        cur = t["items"].get(item_key(t["desc"], req["Key"]))
        return {"Item": cur} if cur else {}

    def ddb_DeleteItem(self, req):
        t = self.table(req["TableName"])
        k = item_key(t["desc"], req["Key"])
        cur = t["items"].get(k)
        self._check(req, cur)
        t["items"].pop(k, None)
        out = {}
        if req.get("ReturnValues") == "ALL_OLD" and cur:
            out["Attributes"] = cur
        return out

    def ddb_UpdateItem(self, req):
        t = self.table(req["TableName"])
        k = item_key(t["desc"], req["Key"])
        cur = t["items"].get(k)
        self._check(req, cur)
        new = copy.deepcopy(cur) if cur else dict(req["Key"])
        apply_update(new, req)
        t["items"][k] = new
        attrs = project(new, req, cur)
        return {"Attributes": attrs} if attrs else {}

    def _matches_key_condition(self, req, item):
        names = req.get("ExpressionAttributeNames", {})
        values = req.get("ExpressionAttributeValues", {})
        expr = req.get("KeyConditionExpression")
        if expr:
            for clause in re.split(r"\s+(?:AND|and)\s+", expr):
                m = re.match(r"\(?\s*([#\w]+)\s*=\s*(:\w+)\s*\)?$", clause.strip())
                if m and item.get(resolve(m.group(1), names)) != values.get(m.group(2)):
                    return False
            return True
        for name, cond in (req.get("KeyConditions") or {}).items():
            if cond.get("ComparisonOperator") == "EQ":
                if item.get(name) != cond["AttributeValueList"][0]:
                    return False
        return True

    def _filter(self, req, items):
        expr = req.get("FilterExpression")
        if expr:
            items = [
                i
                for i in items
                if eval_condition(
                    expr,
                    i,
                    req.get("ExpressionAttributeNames", {}),
                    req.get("ExpressionAttributeValues", {}),
                )
            ]
        return items

    def _page(self, req, items, t):
        if req.get("Select") == "COUNT":
            return {"Count": len(items), "ScannedCount": len(items)}
        limit = req.get("Limit")
        start = req.get("ExclusiveStartKey")
        if start:
            keys = [item_key(t["desc"], i) for i in items]
            sk = item_key(t["desc"], start)
            if sk in keys:
                items = items[keys.index(sk) + 1 :]
        out = {}
        if limit and len(items) > limit:
            items = items[:limit]
            last = items[-1]
            out["LastEvaluatedKey"] = {n: last[n] for n in key_schema(t["desc"]) if n in last}
        out.update({"Items": items, "Count": len(items), "ScannedCount": len(items)})
        return out

    def ddb_Scan(self, req):
        t = self.table(req["TableName"])
        items = list(t["items"].values())
        if req.get("TotalSegments", 1) > 1 and req.get("Segment", 0) != 0:
            items = []
        return self._page(req, self._filter(req, items), t)

    def ddb_Query(self, req):
        t = self.table(req["TableName"])
        items = [i for i in t["items"].values() if self._matches_key_condition(req, i)]
        if req.get("ScanIndexForward") is False:
            items.reverse()
        return self._page(req, self._filter(req, items), t)

    def ddb_BatchGetItem(self, req):
        resp = {}
        for name, ka in req["RequestItems"].items():
            t = self.table(name)
            got = []
            for k in ka["Keys"]:
                cur = t["items"].get(item_key(t["desc"], k))
                if cur:
                    got.append(cur)
            resp[name] = got
        return {"Responses": resp, "UnprocessedKeys": {}}

    def ddb_BatchWriteItem(self, req):
        for name, writes in req["RequestItems"].items():
            t = self.table(name)
            for w in writes:
                if "PutRequest" in w:
                    item = w["PutRequest"]["Item"]
                    t["items"][item_key(t["desc"], item)] = item
                elif "DeleteRequest" in w:
                    t["items"].pop(item_key(t["desc"], w["DeleteRequest"]["Key"]), None)
        return {"UnprocessedItems": {}}

    def ddb_TransactWriteItems(self, req):
        for a in req["TransactItems"]:
            if "Put" in a:
                self.ddb_PutItem(a["Put"])
            elif "Update" in a:
                self.ddb_UpdateItem(a["Update"])
            elif "Delete" in a:
                self.ddb_DeleteItem(a["Delete"])
        return {}

    def ddb_TransactGetItems(self, req):
        out = []
        for a in req["TransactItems"]:
            r = self.ddb_GetItem(a["Get"])
            out.append({"Item": r["Item"]} if "Item" in r else {})
        return {"Responses": out}

    # ---------------- DynamoDB Streams ----------------

    def streams(self, op, req):
        arn = "arn:aws:dynamodb:%s:%s:table/evidence/stream/2026-11-09T00:00:00.000" % (
            REGION,
            ACCOUNT,
        )
        for t in self.tables.values():
            if "LatestStreamArn" in t["desc"]:
                arn = t["desc"]["LatestStreamArn"]
        shard = {
            "ShardId": "shardId-00000001700000000000-00000001",
            "SequenceNumberRange": {"StartingSequenceNumber": "100000000000000000001"},
        }
        if op == "ListStreams":
            return {
                "Streams": [
                    {"StreamArn": arn, "TableName": "evidence", "StreamLabel": "2026-11-09"}
                ]
            }
        if op == "DescribeStream":
            return {
                "StreamDescription": {
                    "StreamArn": arn,
                    "StreamLabel": "2026-11-09T00:00:00.000",
                    "StreamStatus": "ENABLED",
                    "StreamViewType": "NEW_AND_OLD_IMAGES",
                    "CreationRequestDateTime": time.time(),
                    "TableName": "evidence",
                    "KeySchema": [{"AttributeName": "pk", "KeyType": "HASH"}],
                    "Shards": [shard],
                }
            }
        if op == "GetShardIterator":
            return {"ShardIterator": "it-0"}
        if op == "GetRecords":
            n = int(req["ShardIterator"].split("-")[1])
            recs = []
            if n == 0:
                for i in range(2):
                    recs.append(
                        {
                            "eventID": str(uuid.uuid4()),
                            "eventName": "INSERT",
                            "eventVersion": "1.1",
                            "eventSource": "aws:dynamodb",
                            "awsRegion": REGION,
                            "dynamodb": {
                                "ApproximateCreationDateTime": time.time(),
                                "Keys": {"pk": {"S": "k%d" % i}},
                                "NewImage": {"pk": {"S": "k%d" % i}, "v": {"N": str(i)}},
                                "SequenceNumber": "1000000000000000000%02d" % (i + 1),
                                "SizeBytes": 20,
                                "StreamViewType": "NEW_AND_OLD_IMAGES",
                            },
                        }
                    )
            return {"Records": recs, "NextShardIterator": "it-%d" % (n + 1)}
        return {}

    # ---------------- Kinesis (for KCL) ----------------

    def kinesis(self, op, req):
        name = req.get("StreamName") or (req.get("StreamARN") or "evidence").rsplit("/", 1)[-1]
        arn = "arn:aws:kinesis:%s:%s:stream/%s" % (REGION, ACCOUNT, name)
        shard = {
            "ShardId": "shardId-000000000000",
            "HashKeyRange": {
                "StartingHashKey": "0",
                "EndingHashKey": "340282366920938463463374607431768211455",
            },
            "SequenceNumberRange": {"StartingSequenceNumber": "49600000000000000000000000000"},
        }
        now = time.time()
        if op == "DescribeStreamSummary":
            return {
                "StreamDescriptionSummary": {
                    "StreamName": name,
                    "StreamARN": arn,
                    "StreamStatus": "ACTIVE",
                    "StreamModeDetails": {"StreamMode": "PROVISIONED"},
                    "RetentionPeriodHours": 24,
                    "StreamCreationTimestamp": now - 3600,
                    "EnhancedMonitoring": [{"ShardLevelMetrics": []}],
                    "EncryptionType": "NONE",
                    "OpenShardCount": 1,
                    "ConsumerCount": 0,
                }
            }
        if op == "DescribeStream":
            return {
                "StreamDescription": {
                    "StreamName": name,
                    "StreamARN": arn,
                    "StreamStatus": "ACTIVE",
                    "Shards": [shard],
                    "HasMoreShards": False,
                    "RetentionPeriodHours": 24,
                    "StreamCreationTimestamp": now - 3600,
                    "EnhancedMonitoring": [{"ShardLevelMetrics": []}],
                }
            }
        if op == "ListShards":
            return {"Shards": [shard]}
        if op == "GetShardIterator":
            return {"ShardIterator": "kit-0"}
        if op == "GetRecords":
            n = int(req["ShardIterator"].split("-")[1])
            recs = []
            if n == 0:
                for i in range(3):
                    recs.append(
                        {
                            "SequenceNumber": "4960000000000000000000000%04d" % (i + 1),
                            "ApproximateArrivalTimestamp": now,
                            "Data": base64.b64encode(b"record-%d" % i).decode(),
                            "PartitionKey": "pk-%d" % i,
                        }
                    )
            return {
                "Records": recs,
                "NextShardIterator": "kit-%d" % (n + 1),
                "MillisBehindLatest": 0,
                "ChildShards": [],
            }
        if op == "ListStreamConsumers":
            return {"Consumers": []}
        return {}


class ApiError(Exception):
    def __init__(self, type_, message, status=400):
        super().__init__(message)
        self.type = type_
        self.message = message
        self.status = status
        self.extra = {}


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    store = None
    out_dir = None
    upstream = ""
    files = {}
    files_lock = threading.Lock()
    seq = {}

    def log_message(self, fmt, *args):  # quiet
        pass

    def write_record(self, client, rec):
        with self.files_lock:
            n = self.seq.get(client, 0) + 1
            self.seq[client] = n
            rec = {"seq": n, **rec}
            f = self.files.get(client)
            if f is None:
                f = open(os.path.join(self.out_dir, client + ".jsonl"), "a", encoding="utf-8")
                self.files[client] = f
            f.write(json.dumps(rec, sort_keys=True) + "\n")
            f.flush()

    def send(self, status, payload, content_type):
        body = json.dumps(payload, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("x-amzn-RequestId", uuid.uuid4().hex.upper())
        self.send_header("x-amz-crc32", str(zlib.crc32(body)))
        self.end_headers()
        self.wfile.write(body)

    def forward(self, client, op, raw, body):
        """Passes one DynamoDB or Streams request to the upstream as it came and relays
        the answer, writing the exchange to <client>.upstream.jsonl."""
        host, _, port = self.upstream.rpartition(":")
        headers = {
            k: v for k, v in self.headers.items() if k.lower() not in ("connection", "expect")
        }
        # A container runtime's DNS can lag the first requests after the
        # services start (seen once under rootless Podman). A failed lookup
        # sent nothing upstream, so it is safe to try again; any other
        # error is relayed at once.
        for attempt in range(20):
            conn = http.client.HTTPConnection(host, int(port), timeout=60)
            try:
                conn.request("POST", self.path, body=raw, headers=headers)
                resp = conn.getresponse()
                status, out, out_headers = resp.status, resp.read(), resp.getheaders()
                break
            except socket.gaierror as e:
                status, out_headers = 502, []
                out = json.dumps({"__type": "UpstreamUnreachable", "message": repr(e)}).encode()
                if attempt < 19:
                    time.sleep(0.5)
            except OSError as e:
                status, out_headers = 502, []
                out = json.dumps({"__type": "UpstreamUnreachable", "message": repr(e)}).encode()
                break
            finally:
                conn.close()
        try:
            parsed = json.loads(out) if out else {}
        except ValueError:
            parsed = {"<unparsed>": base64.b64encode(out).decode()}
        self.write_record(
            client + ".upstream",
            {"operation": op, "body": record_body(body), "status": status, "response": parsed},
        )
        self.send_response(status)
        for k, v in out_headers:
            if k.lower() not in ("connection", "content-length", "transfer-encoding"):
                self.send_header(k, v)
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def do_GET(self):
        body = b"healthy: recording stub"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b""
        headers = {k.lower(): v for k, v in self.headers.items()}
        client, auth = client_of(headers)
        target = headers.get("x-amz-target", "")
        service, _, op = target.rpartition(".")
        content_type = headers.get("content-type", "application/x-amz-json-1.0")
        try:
            body = json.loads(raw.decode("utf-8")) if raw else {}
        except ValueError:
            body = {"<unparsed>": base64.b64encode(raw).decode()}
        self.write_record(
            client,
            {
                "method": "POST",
                "path": self.path,
                "service": service,
                "operation": op,
                "header_names": sorted(headers),
                "headers": record_headers(headers, auth),
                "body": record_body(body),
            },
        )
        if service in (DDB, STREAMS) and self.upstream:
            self.forward(client, op, raw, body)
            return
        try:
            with self.store.lock:
                if service == DDB:
                    resp = self.store.ddb(op, body)
                elif service == STREAMS:
                    resp = self.store.streams(op, body)
                elif service == KINESIS:
                    resp = self.store.kinesis(op, body)
                else:
                    raise ApiError("UnknownOperationException", None)
            self.send(200, resp, content_type)
        except Exception as e:  # noqa: BLE001 - keep serving; the record is written
            if not isinstance(e, ApiError):
                print("stub error on %s.%s: %r" % (service, op, e), file=sys.stderr, flush=True)
                e = ApiError("com.amazonaws.dynamodb.v20120810#InternalServerError", "stub", 500)
            payload = {"__type": e.type}
            if e.message:
                payload["message"] = e.message
            payload.update(e.extra)
            self.send(e.status, payload, content_type)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--out", required=True)
    ap.add_argument(
        "--dynamodb-upstream",
        default="",
        metavar="HOST:PORT",
        help="pass DynamoDB requests, unchanged, to this plain-HTTP server",
    )
    args = ap.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    Handler.store = Store()
    Handler.out_dir = args.out
    Handler.upstream = args.dynamodb_upstream
    srv = ThreadingHTTPServer((args.bind, args.port), Handler)
    print("recording stub on %s:%d, writing %s" % (args.bind, args.port, args.out), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
