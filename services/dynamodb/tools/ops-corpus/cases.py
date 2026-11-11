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


LSI_TABLE = {
    "TableName": "ops_lsi", "BillingMode": "PAY_PER_REQUEST",
    "AttributeDefinitions": [{"AttributeName": "pk", "AttributeType": "S"},
                             {"AttributeName": "sk", "AttributeType": "N"},
                             {"AttributeName": "ls", "AttributeType": "S"},
                             {"AttributeName": "lt", "AttributeType": "N"}],
    "KeySchema": [{"AttributeName": "pk", "KeyType": "HASH"}, {"AttributeName": "sk", "KeyType": "RANGE"}],
    "LocalSecondaryIndexes": [
        {"IndexName": "by_ls", "KeySchema": [{"AttributeName": "pk", "KeyType": "HASH"},
                                             {"AttributeName": "ls", "KeyType": "RANGE"}],
         "Projection": {"ProjectionType": "KEYS_ONLY"}},
        {"IndexName": "by_lt", "KeySchema": [{"AttributeName": "pk", "KeyType": "HASH"},
                                             {"AttributeName": "lt", "KeyType": "RANGE"}],
         "Projection": {"ProjectionType": "INCLUDE", "NonKeyAttributes": ["a"]}},
        {"IndexName": "all_ls", "KeySchema": [{"AttributeName": "pk", "KeyType": "HASH"},
                                              {"AttributeName": "ls", "KeyType": "RANGE"}],
         "Projection": {"ProjectionType": "ALL"}},
    ],
}

METRICS_DIVERGENCE = ("ItemCollectionMetrics: DynamoDB Local never returns them; Kythira gives AWS's "
                      "documented shape")
UPDATE_INDEX_DIVERGENCE = ("index key type on UpdateItem: DynamoDB Local accepts a value of the wrong "
                           "type for an index sort key; AWS and Kythira refuse it")


def lsi_scenario():
    t = "ops_lsi"
    setup = []
    # No two items of a collection share an index sort key: the order of
    # ties is DynamoDB's to choose.
    for i in range(1, 9):
        it = {"pk": S("p"), "sk": N(i), "a": N(i), "b": N(i * 2)}
        if i != 3:
            it["ls"] = S("x%02d" % (20 - i))
        if i % 2:
            it["lt"] = N(100 - i)
        setup.append(put(t, it))
    for i in range(1, 4):
        setup.append(put(t, {"pk": S("q"), "sk": N(i), "ls": S("y%d" % i), "lt": N(i)}))
    p = {":p": S("p")}
    ix = lambda name, **more: q(t, "pk = :p", p, IndexName=name, **more)  # noqa: E731
    steps = [
        ix("by_ls"),
        ix("by_ls", ScanIndexForward=False),
        ix("by_lt"),
        ix("all_ls"),
        ix("by_ls", Select="COUNT"),
        ix("by_ls", ConsistentRead=True),
        q(t, "pk = :p AND ls = :x", {**p, ":x": S("x15")}, IndexName="by_ls"),
        q(t, "pk = :p AND ls < :x", {**p, ":x": S("x15")}, IndexName="by_ls"),
        q(t, "pk = :p AND ls <= :x", {**p, ":x": S("x15")}, IndexName="by_ls"),
        q(t, "pk = :p AND ls > :x", {**p, ":x": S("x15")}, IndexName="by_ls"),
        q(t, "pk = :p AND ls >= :x", {**p, ":x": S("x15")}, IndexName="by_ls"),
        q(t, "pk = :p AND ls BETWEEN :lo AND :hi", {**p, ":lo": S("x13"), ":hi": S("x16")}, IndexName="by_ls"),
        q(t, "pk = :p AND begins_with(ls, :x)", {**p, ":x": S("x1")}, IndexName="all_ls"),
        q(t, "pk = :p AND lt BETWEEN :lo AND :hi", {**p, ":lo": N(94), ":hi": N(97)}, IndexName="by_lt"),
        # Paging on an index.
        ix("by_ls", Limit=3),
        {**ix("by_ls", Limit=3), "continue": True},
        {**ix("by_ls", Limit=3), "continue": True},
        ix("by_lt", Limit=2, ScanIndexForward=False),
        {**ix("by_lt", Limit=2, ScanIndexForward=False), "continue": True},
        # Attributes the index does not project come from the table.
        ix("by_ls", Select="ALL_ATTRIBUTES", Limit=3),
        ix("by_lt", Select="ALL_PROJECTED_ATTRIBUTES"),
        ix("by_lt", ProjectionExpression="b, a"),
        ix("by_ls", ProjectionExpression="sk, ls"),
        ix("all_ls", Select="ALL_ATTRIBUTES", Limit=2),
        # Filters.
        q(t, "pk = :p", {**p, ":x": N(2)}, IndexName="by_ls", FilterExpression="sk > :x"),
        q(t, "pk = :p", {**p, ":x": N(4)}, IndexName="by_lt", FilterExpression="a > :x"),
        q(t, "pk = :p", {**p, ":x": N(4)}, IndexName="all_ls", FilterExpression="b > :x", Limit=4),
        q(t, "pk = :p", {**p, ":x": S("x15")}, IndexName="by_ls", FilterExpression="ls = :x"),
        q(t, "pk = :p", {**p, ":x": N(4)}, IndexName="by_ls", FilterExpression="zed = :x OR b = :x OR a.c = :x"),
        step("Query", TableName=t, IndexName="by_ls",
             KeyConditions={"pk": {"ComparisonOperator": "EQ", "AttributeValueList": [S("p")]}},
             QueryFilter={"b": {"ComparisonOperator": "EQ", "AttributeValueList": [N(4)]}}),
        # Errors.
        q(t, "pk = :p AND sk > :x", {**p, ":x": N(2)}, IndexName="by_ls"),
        q(t, "pk = :p AND ls = :x", {**p, ":x": N(4)}, IndexName="by_ls"),
        ix("nope"),
        ix("by_ls", ExclusiveStartKey={"pk": S("p"), "sk": N(1)}),
        ix("by_ls", ExclusiveStartKey={"pk": S("p"), "sk": N(5), "ls": S("x15")}),
        ix("by_ls", ExclusiveStartKey={"pk": S("p"), "sk": N(5), "zz": S("x15")}),
        ix("by_ls", ExclusiveStartKey={"pk": S("p"), "sk": N(5), "ls": N(15)}),
        q(t, "pk = :p AND ls > :x", {**p, ":x": S("x15")}, IndexName="by_ls",
          ExclusiveStartKey={"pk": S("p"), "sk": N(7), "ls": S("x13")}),
        # Scans of an index, and of the table, which leaves entries out.
        step("ScanAll", TableName=t, IndexName="by_ls"),
        step("ScanAll", TableName=t, IndexName="by_lt"),
        step("ScanAll", TableName=t, IndexName="by_lt", Select="ALL_ATTRIBUTES"),
        step("ScanAll", TableName=t, IndexName="by_ls", FilterExpression="b = :x",
             ExpressionAttributeValues={":x": N(4)}),
        step("ScanAll", TableName=t),
        step("ScanAll", TableName=t, IndexName="all_ls", Limit=2),
        step("ScanSegments", TableName=t, IndexName="by_lt", segments=3),
        step("Scan", TableName=t, IndexName="by_ls", Limit=2),
        step("Scan", TableName=t, IndexName="nope"),
        # Writes move entries.
        step("UpdateItem", TableName=t, Key={"pk": S("p"), "sk": N(2)}, UpdateExpression="SET ls = :x",
             ExpressionAttributeValues={":x": S("x30")}),
        step("UpdateItem", TableName=t, Key={"pk": S("p"), "sk": N(4)}, UpdateExpression="REMOVE ls"),
        step("UpdateItem", TableName=t, Key={"pk": S("p"), "sk": N(3)}, UpdateExpression="SET ls = :x, lt = :y",
             ExpressionAttributeValues={":x": S("x00"), ":y": N(1)}),
        step("DeleteItem", TableName=t, Key={"pk": S("p"), "sk": N(5)}),
        put(t, {"pk": S("p"), "sk": N(6), "a": N(60)}),
        ix("by_ls"),
        ix("by_lt"),
        ix("all_ls"),
        step("BatchWriteItem", RequestItems={t: [
            {"PutRequest": {"Item": {"pk": S("p"), "sk": N(9), "ls": S("x01"), "a": N(9)}}},
            {"DeleteRequest": {"Key": {"pk": S("p"), "sk": N(8)}}}]}),
        ix("all_ls"),
        step("ScanAll", TableName=t, IndexName="by_ls"),
        step("ScanAll", TableName=t),
        # Index keys are checked on write.
        put(t, {"pk": S("p"), "sk": N(21), "ls": N(1)}),
        put(t, {"pk": S("p"), "sk": N(21), "ls": S("")}),
        step("BatchWriteItem", RequestItems={t: [{"PutRequest": {"Item": {"pk": S("p"), "sk": N(22), "lt": S("1")}}}]}),
        step("UpdateItem", TableName=t, Key={"pk": S("p"), "sk": N(1)}, UpdateExpression="SET ls = :x",
             ExpressionAttributeValues={":x": N(1)}, divergence=UPDATE_INDEX_DIVERGENCE,
             kythira=invalid("Type mismatch for Index Key")),
        step("PutItem", TableName=t, Item={"pk": S("q"), "sk": N(9), "ls": S("y9")},
             ReturnItemCollectionMetrics="SIZE", divergence=METRICS_DIVERGENCE,
             kythira={"response": {"ItemCollectionMetrics": {"ItemCollectionKey": {"pk": S("q")},
                                                             "SizeEstimateRangeGB": [0.0, 1.0]}}}),
        step("DeleteItem", TableName=t, Key={"pk": S("q"), "sk": N(9)},
             ReturnItemCollectionMetrics="SIZE", divergence=METRICS_DIVERGENCE,
             kythira={"response": {"ItemCollectionMetrics": {"ItemCollectionKey": {"pk": S("q")},
                                                             "SizeEstimateRangeGB": [0.0, 1.0]}}}),
        step("PutItem", TableName=t, Item={"pk": S("q"), "sk": N(9)}, ReturnItemCollectionMetrics="NONE"),
    ]
    return {"name": "lsi", "tables": [LSI_TABLE], "setup": setup, "steps": steps}


def lsi_create_scenario():
    """CreateTable's checks of LocalSecondaryIndexes; a created table is
    compared by its LocalSecondaryIndexes."""
    n = [0]

    def lsi(name="ix1", h="pk", r="ls", proj=None):
        ks = ([{"AttributeName": h, "KeyType": "HASH"}] if h else []) + \
             ([{"AttributeName": r, "KeyType": "RANGE"}] if r else [])
        return {"IndexName": name, "KeySchema": ks, "Projection": {"ProjectionType": "ALL"} if proj is None else proj}

    defs = [{"AttributeName": "pk", "AttributeType": "S"}, {"AttributeName": "sk", "AttributeType": "S"},
            {"AttributeName": "ls", "AttributeType": "N"}]
    keys = [{"AttributeName": "pk", "KeyType": "HASH"}, {"AttributeName": "sk", "KeyType": "RANGE"}]

    def ct(lsis, ad=None, ks=None):
        n[0] += 1
        return step("CreateTable", TableName="ops_lc%d" % n[0], BillingMode="PAY_PER_REQUEST",
                    AttributeDefinitions=defs if ad is None else ad, KeySchema=keys if ks is None else ks,
                    LocalSecondaryIndexes=lsis)

    include = lambda names: {"ProjectionType": "INCLUDE", "NonKeyAttributes": names}  # noqa: E731
    steps = [
        ct([lsi()]),
        step("DescribeTable", TableName="ops_lc1"),
        ct([lsi("ix1", proj={"ProjectionType": "KEYS_ONLY"}), lsi("ix2", proj=include(["b", "a"])),
            lsi("ix3", r="sk")]),
        ct([lsi(r="sk")], ad=defs[:2]),
        ct([lsi(proj=include(["sk"]))]),
        ct([lsi(proj=include(["a%d" % i for i in range(20)]))]),
        ct([lsi("ix%d" % i, proj=include(["i%da%d" % (i, j) for j in range(20)])) for i in range(5)]),
        ct([]),
        ct([lsi()], ad=defs[:1] + defs[2:], ks=keys[:1]),
        ct([lsi(h="ls", r="sk")]),
        ct([lsi(r=None)]),
        ct([lsi(h=None)]),
        ct([lsi(r="zz")]),
        ct([lsi(r="pk")]),
        ct([lsi(), lsi()]),
        ct([lsi("ix%d" % i) for i in range(6)]),
        ct([lsi()], ad=defs + [{"AttributeName": "zz", "AttributeType": "S"}]),
        ct([lsi(proj={"ProjectionType": "ALL", "NonKeyAttributes": ["a"]})]),
        ct([lsi(proj={"ProjectionType": "KEYS_ONLY", "NonKeyAttributes": ["a"]})]),
        ct([lsi(proj={"ProjectionType": "INCLUDE"})]),
        {**ct([lsi(proj=include([]))]), "divergence": MODEL_DIVERGENCE,
         "kythira": model_error("Value '[]' at 'localSecondaryIndexes.1.member.projection.nonKeyAttributes' "
                                "failed to satisfy constraint: Member must have length greater than or equal to 1")},
        ct([lsi(proj=include(["a", "a"]))]),
        {**ct([lsi(proj=include(["a%d" % i for i in range(21)]))]), "divergence": MODEL_DIVERGENCE,
         "kythira": model_error("Value '[" + ", ".join("a%d" % i for i in range(21)) + "]' at "
                                "'localSecondaryIndexes.1.member.projection.nonKeyAttributes' failed to satisfy "
                                "constraint: Member must have length less than or equal to 20")},
        ct([lsi(proj={})]),
        {**ct([lsi(name="ab")]), "divergence": MODEL_DIVERGENCE,
         "kythira": model_error("Value 'ab' at 'localSecondaryIndexes.1.member.indexName' failed to satisfy "
                                "constraint: Member must have length greater than or equal to 3")},
        {**ct([lsi(name="a b c")]), "divergence": MODEL_DIVERGENCE,
         "kythira": model_error("Value 'a b c' at 'localSecondaryIndexes.1.member.indexName' failed to satisfy "
                                "constraint: Member must satisfy regular expression pattern: [a-zA-Z0-9_.-]+")},
        ct([{"IndexName": "ix1", "KeySchema": [{"AttributeName": "ls", "KeyType": "RANGE"},
                                               {"AttributeName": "pk", "KeyType": "HASH"}],
             "Projection": {"ProjectionType": "ALL"}}]),
    ]
    return {"name": "lsi_create", "tables": [], "setup": [], "steps": steps}


TXN_KEY_DIVERGENCE = ("transaction key type: DynamoDB Local cancels the transaction with a "
                      "ValidationError reason; Kythira refuses the request as the single-item "
                      "call does, before anything runs")
TXN_KEY_UPDATE_DIVERGENCE = ("update of a key attribute in a transaction: DynamoDB Local cancels "
                             "with a ValidationError reason; Kythira refuses it when the "
                             "expression compiles, as UpdateItem does")


def transactions_scenario():
    t = "ops_tx"
    u = "ops_tu"
    setup = [put(t, {"pk": S("a"), "sk": N(1), "v": N(1)}),
             put(t, {"pk": S("a"), "sk": N(2), "v": N(2)}),
             put(t, {"pk": S("s"), "sk": N(1), "w": S("text")}),
             put(t, {"pk": S("e"), "sk": N(1), "v": N(3)}),
             put(u, {"id": S("x"), "n": N(10)})]
    k = lambda pk, sk: {"pk": S(pk), "sk": N(sk)}  # noqa: E731
    tok = {"TransactItems": [{"Put": {"TableName": u, "Item": {"id": S("tok"), "n": N(1)}}}],
           "ClientRequestToken": "token-one"}
    tok_changed = json.loads(json.dumps(tok))
    tok_changed["TransactItems"][0]["Put"]["Item"]["n"] = N(2)
    failing = {"TransactItems": [{"ConditionCheck": {"TableName": u, "Key": {"id": S("absent")},
                                                     "ConditionExpression": "attribute_exists(id)"}}],
               "ClientRequestToken": "token-two"}
    steps = [
        # Every action kind across two tables, all applied.
        step("TransactWriteItems", TransactItems=[
            {"Put": {"TableName": t, "Item": {"pk": S("b"), "sk": N(1), "v": N(5)}}},
            {"Update": {"TableName": t, "Key": k("a", 1), "UpdateExpression": "SET v = v + :one",
                        "ExpressionAttributeValues": {":one": N(1)}}},
            {"Delete": {"TableName": t, "Key": k("a", 2)}},
            {"ConditionCheck": {"TableName": u, "Key": {"id": S("x")}, "ConditionExpression": "n > :z",
                                "ExpressionAttributeValues": {":z": N(0)}}}]),
        step("ScanAll", TableName=t),
        # One failing condition of each kind: nothing applies, and each
        # action has its reason, with the item where ALL_OLD asked for it.
        step("TransactWriteItems", TransactItems=[
            {"Put": {"TableName": t, "Item": {"pk": S("c"), "sk": N(1)}}},
            {"ConditionCheck": {"TableName": u, "Key": {"id": S("x")}, "ConditionExpression": "n = :nine",
                                "ExpressionAttributeValues": {":nine": N(9)},
                                "ReturnValuesOnConditionCheckFailure": "ALL_OLD"}},
            {"Delete": {"TableName": t, "Key": k("b", 1), "ConditionExpression": "attribute_not_exists(pk)"}},
            {"Put": {"TableName": t, "Item": {"pk": S("a"), "sk": N(1)},
                     "ConditionExpression": "attribute_not_exists(pk)",
                     "ReturnValuesOnConditionCheckFailure": "ALL_OLD"}},
            {"Update": {"TableName": t, "Key": k("e", 1), "UpdateExpression": "SET v = :z",
                        "ConditionExpression": "v < :z", "ExpressionAttributeValues": {":z": N(0)}}}]),
        step("ScanAll", TableName=t),
        # An update that cannot apply to the stored item.
        step("TransactWriteItems", TransactItems=[
            {"Update": {"TableName": t, "Key": k("s", 1), "UpdateExpression": "SET w = w + :one",
                        "ExpressionAttributeValues": {":one": N(1)}}},
            {"Put": {"TableName": t, "Item": {"pk": S("d"), "sk": N(1)}}}]),
        step("ScanAll", TableName=t),
        # Request checks.
        step("TransactWriteItems", TransactItems=[{"Put": {"TableName": t, "Item": k("a", 1)}},
                                                  {"Delete": {"TableName": t, "Key": k("a", 1)}}]),
        step("TransactWriteItems", TransactItems=[{"Put": {"TableName": t, "Item": k("a", 1)},
                                                   "Delete": {"TableName": t, "Key": k("a", 1)}}]),
        step("TransactWriteItems", TransactItems=[{}]),
        step("TransactWriteItems", TransactItems=[{"Put": {"TableName": "ops_missing", "Item": k("a", 1)}}]),
        step("TransactWriteItems", TransactItems=[{"Put": {"TableName": t, "Item": {"pk": S("q")}}}],
             divergence=ITEM_DIVERGENCE, kythira=invalid("Missing the key sk in the item")),
        step("TransactWriteItems", TransactItems=[{"Delete": {"TableName": t, "Key": {"pk": N(1), "sk": N(1)}}}],
             divergence=TXN_KEY_DIVERGENCE, kythira=KEY_MISMATCH),
        step("TransactWriteItems", TransactItems=[
            {"Update": {"TableName": t, "Key": k("a", 1), "UpdateExpression": "SET pk = :o",
                        "ExpressionAttributeValues": {":o": S("z")}}}],
             divergence=TXN_KEY_UPDATE_DIVERGENCE,
             kythira=invalid("Cannot update attribute pk. This attribute is part of the key")),
        step("TransactWriteItems", TransactItems=[
            {"Put": {"TableName": t, "Item": k("q", 1), "ConditionExpression": "attribute_not_exists(("}}]),
        step("TransactWriteItems", TransactItems=[
            {"Put": {"TableName": t, "Item": k("q", 1), "ExpressionAttributeValues": {":x": N(1)}}}]),
        # ClientRequestToken: a retry of a committed request answers success
        # and changes nothing; a changed request with the token is refused;
        # a cancelled request runs again.
        step("TransactWriteItems", **tok),
        step("PutItem", TableName=u, Item={"id": S("tok"), "n": N(7)}),
        step("TransactWriteItems", **tok),
        step("GetItem", TableName=u, Key={"id": S("tok")}),
        step("TransactWriteItems", **tok_changed),
        step("TransactWriteItems", **failing),
        step("TransactWriteItems", **failing),
        # TransactGetItems: found, missing and projected, then the checks.
        step("TransactGetItems", TransactItems=[
            {"Get": {"TableName": t, "Key": k("a", 1)}},
            {"Get": {"TableName": t, "Key": k("zz", 1)}},
            {"Get": {"TableName": u, "Key": {"id": S("x")}, "ProjectionExpression": "#n",
                     "ExpressionAttributeNames": {"#n": "n"}}}]),
        step("TransactGetItems", TransactItems=[{"Get": {"TableName": t, "Key": k("a", 1)}},
                                                {"Get": {"TableName": t, "Key": k("a", 1)}}]),
        step("TransactGetItems", TransactItems=[{"Get": {"TableName": "ops_missing", "Key": k("a", 1)}}]),
    ]
    return {"name": "transactions", "tables": [table(t, "pk", "S", "sk", "N"), table(u, "id", "S")],
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
                             binary_scenario(), scan_scenario(), batch_scenario(),
                             lsi_scenario(), lsi_create_scenario(), transactions_scenario()])


if __name__ == "__main__":
    print(json.dumps(all_scenarios(), indent=1)[:2000])
