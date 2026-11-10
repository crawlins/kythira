# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""The scenarios of the operations corpus
(.kiro/specs/dynamodb-compatible-api/ task 8, Requirement 21.1).

Each scenario starts from fresh tables, runs its `setup` requests (not
recorded), then its `steps`, whose responses record.py stores. A step is a
real operation or one of two pseudo-operations that page to the end, so
that results are comparable even though DynamoDB Local scans in a different
order from Kythira:

    ScanAll       Scan with `input`, following LastEvaluatedKey to the end
    ScanSegments  ScanAll of every segment 0..`segments`-1, as one union

A step with `"continue": true` sends the previous step's LastEvaluatedKey as
its ExclusiveStartKey. A step whose expected answer is known to differ on
Kythira carries `divergence` (why) and, when Kythira's answer is checked
instead, `kythira`.
"""

import json


def table(name, pk, pk_type, sk=None, sk_type=None):
    keys = [{"AttributeName": pk, "KeyType": "HASH"}]
    defs = [{"AttributeName": pk, "AttributeType": pk_type}]
    if sk:
        keys.append({"AttributeName": sk, "KeyType": "RANGE"})
        defs.append({"AttributeName": sk, "AttributeType": sk_type})
    return {"TableName": name, "KeySchema": keys, "AttributeDefinitions": defs,
            "BillingMode": "PAY_PER_REQUEST"}


def put(t, item):
    return {"op": "PutItem", "input": {"TableName": t, "Item": item}}


def S(v):
    return {"S": v}


def N(v):
    return {"N": str(v)}


def B(v):
    return {"B": v}


def step(op, **inp):
    extra = {}
    for k in ("continue", "divergence", "kythira", "segments"):
        if k in inp:
            extra[k] = inp.pop(k)
    return {"op": op, "input": inp, **extra}


def q(t, kce, values, names=None, **more):
    inp = {"TableName": t, "KeyConditionExpression": kce, "ExpressionAttributeValues": values}
    if names:
        inp["ExpressionAttributeNames"] = names
    inp.update(more)
    return step("Query", **inp)


KEY_DIVERGENCE = ("key message: Kythira gives AWS's wording, DynamoDB Local says "
                  "\"The number of conditions on the keys is invalid\"")
KEY_MISMATCH = {"error": {"type": "ValidationException",
                          "message": "The provided key element does not match the schema"}}


MODEL_DIVERGENCE = ("model constraint: Kythira answers with the Coral validation message AWS's "
                    "model layer gives; DynamoDB Local has its own wording")


def model_error(message):
    return {"error": {"type": "ValidationException", "message": "1 validation error detected: " + message}}


LIMIT_ZERO = model_error("Value '0' at 'limit' failed to satisfy constraint: Member must have value "
                         "greater than or equal to 1")
EMPTY_REQUESTS = model_error("Value '{}' at 'requestItems' failed to satisfy constraint: Member must "
                             "have length greater than or equal to 1")
ITEM_DIVERGENCE = "item key message: Kythira gives AWS's wording, which names the key"


def invalid(message):
    return {"error": {"type": "ValidationException",
                      "message": "One or more parameter values were invalid: " + message}}


def query_scenario():
    t = "ops_comp"
    setup = []
    for i in range(1, 11):
        setup.append(put(t, {"pk": S("a"), "sk": N(i), "v": N(i * 10), "tag": S("even" if i % 2 == 0 else "odd")}))
    for p in ("b", "c"):
        for i in range(1, 4):
            setup.append(put(t, {"pk": S(p), "sk": N(i), "m": {"M": {"x": N(i)}}}))
    a = {":p": S("a")}
    steps = [
        q(t, "pk = :p", a),
        q(t, "pk = :p", a, ScanIndexForward=False),
        q(t, "pk = :p AND sk = :s", {**a, ":s": N(3)}),
        q(t, "pk = :p AND sk < :s", {**a, ":s": N(3)}),
        q(t, "pk = :p AND sk <= :s", {**a, ":s": N(3)}),
        q(t, "pk = :p AND sk > :s", {**a, ":s": N(8)}),
        q(t, "pk = :p AND sk >= :s", {**a, ":s": N(8)}),
        q(t, "pk = :p AND sk BETWEEN :lo AND :hi", {**a, ":lo": N(3), ":hi": N(5)}),
        q(t, "pk = :p AND sk BETWEEN :lo AND :hi", {**a, ":lo": N(5), ":hi": N(3)}),
        q(t, "pk = :p AND sk BETWEEN :lo AND :hi", {**a, ":lo": N("2.5"), ":hi": N("2.75")}),
        q(t, "pk = :p AND begins_with(sk, :s)", {**a, ":s": N(1)}),
        q(t, "pk = :p AND sk > :s", {**a, ":s": N(100)}),
        q(t, "pk = :p", {":p": S("zz")}),
        q(t, "#k = :p", a, names={"#k": "pk"}, ConsistentRead=True),
        # Paging forward and in reverse.
        q(t, "pk = :p", a, Limit=3),
        {**q(t, "pk = :p", a, Limit=3), "continue": True},
        {**q(t, "pk = :p", a, Limit=3), "continue": True},
        {**q(t, "pk = :p", a, Limit=3), "continue": True},
        q(t, "pk = :p", a, Limit=10),
        {**q(t, "pk = :p", a, Limit=10), "continue": True},
        q(t, "pk = :p", a, Limit=11),
        q(t, "pk = :p", a, Limit=4, ScanIndexForward=False),
        {**q(t, "pk = :p", a, Limit=4, ScanIndexForward=False), "continue": True},
        {**q(t, "pk = :p", a, Limit=4, ScanIndexForward=False), "continue": True},
        q(t, "pk = :p AND sk BETWEEN :lo AND :hi", {**a, ":lo": N(2), ":hi": N(9)}, Limit=3),
        {**q(t, "pk = :p AND sk BETWEEN :lo AND :hi", {**a, ":lo": N(2), ":hi": N(9)}, Limit=3),
         "continue": True},
        # Filters, Select and projections.
        q(t, "pk = :p", {**a, ":x": N(45)}, FilterExpression="v > :x"),
        q(t, "pk = :p", {**a, ":x": N(45)}, FilterExpression="v > :x", Limit=6),
        q(t, "pk = :p", {**a, ":e": S("even")}, FilterExpression="tag = :e", Select="COUNT"),
        q(t, "pk = :p", a, Select="COUNT"),
        q(t, "pk = :p", a, ProjectionExpression="sk, #t", ExpressionAttributeNames={"#t": "tag"}),
        q(t, "pk = :p", {":p": S("b")}, ProjectionExpression="m.x"),
        q(t, "pk = :p", a, ProjectionExpression="v", Select="SPECIFIC_ATTRIBUTES"),
        step("Query", TableName=t, KeyConditions={"pk": {"ComparisonOperator": "EQ", "AttributeValueList": [S("a")]},
                                                  "sk": {"ComparisonOperator": "BETWEEN", "AttributeValueList": [N(2), N(4)]}}),
        step("Query", TableName=t, KeyConditions={"pk": {"ComparisonOperator": "EQ", "AttributeValueList": [S("a")]}},
             AttributesToGet=["sk"], Select="SPECIFIC_ATTRIBUTES", Limit=2),
        step("Query", TableName=t, KeyConditions={"pk": {"ComparisonOperator": "EQ", "AttributeValueList": [S("a")]}},
             QueryFilter={"v": {"ComparisonOperator": "GE", "AttributeValueList": [N(80)]}}),
        # Errors.
        q(t, "pk = :p", a, Select="SPECIFIC_ATTRIBUTES"),
        q(t, "pk = :p", a, Select="ALL_PROJECTED_ATTRIBUTES"),
        q(t, "pk = :p", a, Select="ALL_ATTRIBUTES", ProjectionExpression="v"),
        q(t, "pk = :p", a, Select="COUNT", ProjectionExpression="v"),
        step("Query", TableName=t, KeyConditions={"pk": {"ComparisonOperator": "EQ", "AttributeValueList": [S("a")]}},
             AttributesToGet=["sk"], Select="ALL_ATTRIBUTES"),
        q(t, "pk = :p", a, ExclusiveStartKey={"pk": S("a")}),
        q(t, "pk = :p", a, ExclusiveStartKey={"pk": S("a"), "sk": N(3), "x": S("1")}),
        q(t, "pk = :p", a, ExclusiveStartKey={"pk": S("b"), "sk": N(3)}),
        q(t, "pk = :p AND sk > :s", {**a, ":s": N(5)}, ExclusiveStartKey={"pk": S("a"), "sk": N(7)}),
        q(t, "pk = :p", a, ExclusiveStartKey={"pk": S("a"), "sk": N("3.5")}),
        q(t, "pk = :p", {":p": N(1)}),
        q(t, "pk = :p", a, IndexName="nope"),
        q(t, "pk = :p", a, Limit=0, divergence=MODEL_DIVERGENCE, kythira=LIMIT_ZERO),
        q("ops_missing", "pk = :p", a),
    ]
    return {"name": "query", "tables": [table(t, "pk", "S", "sk", "N")], "setup": setup, "steps": steps}


def strings_scenario():
    t = "ops_str"
    sks = ["a", "aa", "ab", "abc", "b", "ba", "Z", "z", "é", "été", "a b", "a\u0000b", "0", "~"]
    setup = [put(t, {"pk": S("p"), "sk": S(s)}) for s in sks]
    p = {":p": S("p")}
    steps = [
        q(t, "pk = :p", p),
        q(t, "pk = :p", p, ScanIndexForward=False),
        q(t, "pk = :p AND begins_with(sk, :s)", {**p, ":s": S("a")}),
        q(t, "pk = :p AND begins_with(sk, :s)", {**p, ":s": S("ab")}),
        q(t, "pk = :p AND begins_with(sk, :s)", {**p, ":s": S("é")}),
        q(t, "pk = :p AND begins_with(sk, :s)", {**p, ":s": S("a\u0000")}),
        q(t, "pk = :p AND sk BETWEEN :lo AND :hi", {**p, ":lo": S("a"), ":hi": S("b")}),
        q(t, "pk = :p AND sk < :s", {**p, ":s": S("ab")}),
        q(t, "pk = :p AND sk >= :s", {**p, ":s": S("b")}),
        q(t, "pk = :p AND sk > :s", {**p, ":s": S("a")}, Limit=2),
        {**q(t, "pk = :p AND sk > :s", {**p, ":s": S("a")}, Limit=2), "continue": True},
        q(t, "pk = :p AND begins_with(sk, :s)", {**p, ":s": S("a")}, Limit=2, ScanIndexForward=False),
        {**q(t, "pk = :p AND begins_with(sk, :s)", {**p, ":s": S("a")}, Limit=2, ScanIndexForward=False),
         "continue": True},
    ]
    return {"name": "strings", "tables": [table(t, "pk", "S", "sk", "S")], "setup": setup, "steps": steps}


def numbers_scenario():
    t = "ops_num"
    sks = ["-1E+125", "-99999999999999999999", "-10", "-1.5", "-0.001", "0", "1E-130", "0.001", "1",
           "1.000000000000000000000000000000000001", "2", "10", "1E3", "99999999999999999999", "9.9E125"]
    setup = [put(t, {"pk": N(7), "sk": N(s), "orig": S(s)}) for s in sks]
    p = {":p": N(7)}
    steps = [
        q(t, "pk = :p", p),
        q(t, "pk = :p", p, ScanIndexForward=False),
        q(t, "pk = :p AND sk < :s", {**p, ":s": N(0)}),
        q(t, "pk = :p AND sk BETWEEN :lo AND :hi", {**p, ":lo": N("-1.5"), ":hi": N("1")}),
        q(t, "pk = :p AND sk = :s", {**p, ":s": N("1000")}),
        q(t, "pk = :p AND sk >= :s", {**p, ":s": N("1.0000")}),
        q(t, "pk = :p", {":p": N("7.0")}),
        q(t, "pk = :p AND begins_with(sk, :s)", {**p, ":s": S("1")}),
    ]
    return {"name": "numbers", "tables": [table(t, "pk", "N", "sk", "N")], "setup": setup, "steps": steps}


def binary_scenario():
    t = "ops_bin"
    vals = ["AA==", "AAA=", "AAE=", "AQ==", "/w==", "//8=", "gA==", "YWJj", "YWI=", "YQ=="]
    setup = [put(t, {"pk": B("cGs="), "sk": B(v)}) for v in vals]
    p = {":p": B("cGs=")}
    steps = [
        q(t, "pk = :p", p),
        q(t, "pk = :p", p, ScanIndexForward=False),
        q(t, "pk = :p AND begins_with(sk, :s)", {**p, ":s": B("AA==")}),
        q(t, "pk = :p AND begins_with(sk, :s)", {**p, ":s": B("YQ==")}),
        q(t, "pk = :p AND sk > :s", {**p, ":s": B("gA==")}),
        q(t, "pk = :p AND sk BETWEEN :lo AND :hi", {**p, ":lo": B("AAA="), ":hi": B("AQ==")}),
    ]
    return {"name": "binary", "tables": [table(t, "pk", "B", "sk", "B")], "setup": setup, "steps": steps}


def scan_scenario():
    t = "ops_scan"
    h = "ops_hash"
    setup = []
    for p in range(12):
        for s in range(3):
            setup.append(put(t, {"pk": S("p%02d" % p), "sk": N(s), "n": N(p * 3 + s)}))
    for p in range(20):
        setup.append(put(h, {"id": S("id%02d" % p), "big": S("x" * (p * 10))}))
    steps = [
        step("ScanAll", TableName=t),
        step("ScanAll", TableName=t, Limit=7),
        step("ScanAll", TableName=t, Limit=1, ConsistentRead=True),
        step("ScanAll", TableName=t, FilterExpression="n >= :x", ExpressionAttributeValues={":x": N(30)}),
        step("ScanAll", TableName=t, FilterExpression="n >= :x", ExpressionAttributeValues={":x": N(30)}, Limit=5),
        step("ScanAll", TableName=t, ProjectionExpression="n"),
        step("ScanAll", TableName=h),
        step("ScanAll", TableName=h, ScanFilter={"big": {"ComparisonOperator": "BEGINS_WITH", "AttributeValueList": [S("xxxxxxxxxxxxxxxxxxxx")]}}),
        step("ScanSegments", TableName=t, segments=4),
        step("ScanSegments", TableName=t, segments=3, Limit=2),
        step("ScanSegments", TableName=h, segments=7),
        step("Scan", TableName=t, Select="COUNT"),
        step("Scan", TableName=t, Select="COUNT", FilterExpression="n < :x", ExpressionAttributeValues={":x": N(10)}),
        step("Scan", TableName=t, Limit=36),
        step("Scan", TableName=t, Limit=37),
        step("Scan", TableName=t, Segment=0),
        step("Scan", TableName=t, TotalSegments=2),
        step("Scan", TableName=t, Segment=2, TotalSegments=2),
        step("Scan", TableName=t, Segment=0, TotalSegments=1000001),
        step("Scan", TableName=t, ExclusiveStartKey={"pk": S("p00")}),
        step("Scan", TableName=t, Select="SPECIFIC_ATTRIBUTES"),
        step("Scan", TableName=t, Select="ALL_ATTRIBUTES", ProjectionExpression="n"),
        step("Scan", TableName=t, IndexName="nope"),
        step("Scan", TableName=t, Limit=0, divergence=MODEL_DIVERGENCE, kythira=LIMIT_ZERO),
        step("Scan", TableName="ops_missing"),
    ]
    return {"name": "scan", "tables": [table(t, "pk", "S", "sk", "N"), table(h, "id", "S")],
            "setup": setup, "steps": steps}


def batch_scenario():
    t = "ops_bt"
    h = "ops_bh"
    setup = [put(t, {"pk": S("a"), "sk": N(i), "v": N(i)}) for i in range(1, 6)]
    setup += [put(h, {"id": S("x%d" % i), "w": S("w%d" % i)}) for i in range(1, 4)]
    k = lambda i: {"pk": S("a"), "sk": N(i)}  # noqa: E731
    steps = [
        step("BatchGetItem", RequestItems={t: {"Keys": [k(1), k(3), k(9)]}, h: {"Keys": [{"id": S("x2")}]}}),
        step("BatchGetItem", RequestItems={t: {"Keys": [k(2)], "ProjectionExpression": "#v",
                                               "ExpressionAttributeNames": {"#v": "v"}, "ConsistentRead": True}}),
        step("BatchGetItem", RequestItems={t: {"Keys": [k(2)], "AttributesToGet": ["sk"]}}),
        step("BatchGetItem", RequestItems={t: {"Keys": [k(1), k(1)]}}),
        step("BatchGetItem", RequestItems={}, divergence=MODEL_DIVERGENCE, kythira=EMPTY_REQUESTS),
        step("BatchGetItem", RequestItems={t: {"Keys": []}}, divergence=MODEL_DIVERGENCE,
             kythira=model_error("Value '[]' at 'requestItems.ops_bt.member.keys' failed to satisfy "
                                 "constraint: Member must have length greater than or equal to 1")),
        step("BatchGetItem", RequestItems={t: {"Keys": [k(i) for i in range(101)]}},
             divergence=MODEL_DIVERGENCE),
        step("BatchGetItem", RequestItems={"ops_missing": {"Keys": [{"pk": S("a")}]}}),
        step("BatchGetItem", RequestItems={t: {"Keys": [{"pk": S("a")}]}},
             divergence=KEY_DIVERGENCE, kythira=KEY_MISMATCH),
        step("BatchWriteItem", RequestItems={
            t: [{"PutRequest": {"Item": {"pk": S("b"), "sk": N(1), "v": N(100)}}},
                {"DeleteRequest": {"Key": k(5)}},
                {"PutRequest": {"Item": {"pk": S("a"), "sk": N(1), "v": N(-1)}}}],
            h: [{"DeleteRequest": {"Key": {"id": S("x1")}}}, {"DeleteRequest": {"Key": {"id": S("nope")}}}]}),
        step("ScanAll", TableName=t),
        step("ScanAll", TableName=h),
        step("BatchWriteItem", RequestItems={t: [{"PutRequest": {"Item": k(7)}}, {"DeleteRequest": {"Key": k(7)}}]}),
        step("BatchWriteItem", RequestItems={t: [{"PutRequest": {"Item": {"pk": S("c"), "sk": N(i)}}} for i in range(26)]},
             divergence=MODEL_DIVERGENCE),
        step("BatchWriteItem", RequestItems={t: [{"PutRequest": {"Item": k(8)}, "DeleteRequest": {"Key": k(8)}}]}),
        step("BatchWriteItem", RequestItems={t: [{}]}),
        step("BatchWriteItem", RequestItems={t: [{"DeleteRequest": {"Key": {"pk": S("a")}}}]},
             divergence=KEY_DIVERGENCE, kythira=KEY_MISMATCH),
        step("BatchWriteItem", RequestItems={t: [{"PutRequest": {"Item": {"pk": S("c")}}}]},
             divergence=ITEM_DIVERGENCE, kythira=invalid("Missing the key sk in the item")),
        step("BatchWriteItem", RequestItems={t: [{"PutRequest": {"Item": {"pk": S("c"), "sk": S("1")}}}]},
             divergence=ITEM_DIVERGENCE, kythira=invalid("Type mismatch for key sk expected: N actual: S")),
        step("BatchWriteItem", RequestItems={"ops_missing": [{"PutRequest": {"Item": {"pk": S("c")}}}]}),
        step("BatchWriteItem", RequestItems={t: [{"PutRequest": {"Item": {"pk": S("d"), "sk": N(1)}}}],
                                             "ops_missing": [{"PutRequest": {"Item": {"pk": S("c")}}}]}),
        step("ScanAll", TableName=t),
        step("BatchWriteItem", RequestItems={}, divergence=MODEL_DIVERGENCE, kythira=EMPTY_REQUESTS),
    ]
    return {"name": "batch", "tables": [table(t, "pk", "S", "sk", "N"), table(h, "id", "S")],
            "setup": setup, "steps": steps}


MISSING_DIVERGENCE = ("missing table: Kythira gives AWS's data-plane wording, DynamoDB Local says "
                      "\"Cannot do operations on a non-existent table\"")
MISSING = {"error": {"type": "ResourceNotFoundException", "message": "Requested resource not found"}}


def mark_divergences(scenarios):
    for sc in scenarios:
        for st in sc["steps"]:
            if "ops_missing" in json.dumps(st["input"]):
                st["divergence"] = MISSING_DIVERGENCE
                st["kythira"] = MISSING
    return scenarios


def all_scenarios():
    return mark_divergences([query_scenario(), strings_scenario(), numbers_scenario(),
                             binary_scenario(), scan_scenario(), batch_scenario()])


if __name__ == "__main__":
    print(json.dumps(all_scenarios(), indent=1)[:2000])
