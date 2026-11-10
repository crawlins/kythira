#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""The expression corpus's cases (.kiro/specs/dynamodb-compatible-api/ task 5).

Each case is one request against a table keyed `pk` (S) and `sk` (N), on an
item that may already exist:

    {"op": "UpdateItem", "input": {...}, "item": {...} | null}

`input` is the request without TableName and Key (a Query gets its key
condition from the case); `item` the attributes besides the key the item
holds first, or null for no item. `record.py` runs every case against
DynamoDB Local and stores what it answered; the C++ corpus test replays
them through the compiler and evaluator.

The cases are hand-written families (every probe that settled a rule) plus
seeded random ones: grammar-driven expressions, token mutations of them
for syntax errors, and random items for evaluation. The seed is fixed, so
the case list is the same on every run.
"""

import json
import random
import re
import sys

SEED = 20261110

N1 = {"N": "1"}
S1 = {"S": "x"}


def cond(expr, values=None, names=None, item=None, op="DeleteItem"):
    inp = {"ConditionExpression": expr}
    if values is not None:
        inp["ExpressionAttributeValues"] = values
    if names is not None:
        inp["ExpressionAttributeNames"] = names
    return {"op": op, "input": inp, "item": item}


def upd(expr, values=None, names=None, item=None, condition=None):
    inp = {"UpdateExpression": expr, "ReturnValues": "ALL_NEW"}
    if values is not None:
        inp["ExpressionAttributeValues"] = values
    if names is not None:
        inp["ExpressionAttributeNames"] = names
    if condition is not None:
        inp["ConditionExpression"] = condition
    return {"op": "UpdateItem", "input": inp, "item": item}


def req(op, item=None, **inp):
    if op == "UpdateItem":
        inp.setdefault("ReturnValues", "ALL_NEW")
    return {"op": op, "input": inp, "item": item}


def proj(expr, names=None, item=None, op="GetItem"):
    inp = {"ProjectionExpression": expr}
    if names is not None:
        inp["ExpressionAttributeNames"] = names
    return {"op": op, "input": inp, "item": item}


def query(kc, values=None, names=None, item=None, **extra):
    inp = {"KeyConditionExpression": kc, **extra}
    if values is not None:
        inp["ExpressionAttributeValues"] = values
    if names is not None:
        inp["ExpressionAttributeNames"] = names
    return {"op": "Query", "input": inp, "item": item}


V = {":v": N1}
ITEM = {
    "a": N1,
    "s": {"S": "hello"},
    "b": {"B": "AQID"},
    "t": {"BOOL": True},
    "z": {"NULL": True},
    "l": {"L": [N1, {"S": "two"}, {"M": {"x": N1}}]},
    "m": {"M": {"x": N1, "y": {"S": "why"}, "n": {"M": {"deep": {"N": "3"}}}}},
    "ss": {"SS": ["p", "q"]},
    "ns": {"NS": ["1", "2"]},
    "bs": {"BS": ["AA==", "AQ=="]},
    "e": {"S": "é"},
}


def syntax_family():
    exprs = [
        "a = = :v", "a =", "a = :v AND", "(a = :v", "a = :v)", "a == :v", "a !=:v", "a = :v b",
        "a.b.[1] = :v", "a[x] = :v", "a[1.5]=:v", "a[-1] = :v", "1 = :v", "a = :", "a = #",
        "a = :v.b", "#a-b = :v", "é = :v", "a = :vé", "a< >:v", "a => :v", "a =< :v", "a ! = :v",
        "a[01] = :v", "a IN ()", "a IN :v", "NOT a", "  ", "a = :v AND b", "a.1 = :v", "_a = :v",
        "a-b = :v", "a = :v,", "a = 'x'", "a = \"x\"", "a = 1", "@ = :v", "a = :v; b", "size()",
        "size() = :v", "foo()", "attribute_exists()", "between = :v", "a = :v AND :v",
        "not = :v", " 1 = :v", "(a)", "(a) AND b = :v", "((a = :v) AND b) = :v", "(a = :v) = :v",
        "a = (b = :v)", "(a, b)", "#f(a)", "a = :v = :v", "a BETWEEN :v", "a BETWEEN :v AND",
        "a BETWEEN :v OR :v", "a IN (:v,)", "a IN (:v :v)", "attribute_exists(a = :v)",
        "a.and = :v", "a[0 = :v", "a[] = :v", "a[0]] = :v", "a = :v + :v", "a AND b",
        "a = :v OR", "OR a = :v", "AND", "NOT", "a = :v NOT", "a NOT = :v", "a <> <> :v",
        "contains(a, :v", "contains a, :v)", "(", ")", "a.", ".a = :v", "a..b = :v",
        "a = :v", "a\t=\n:v", "a = :v\r", "a = :v\f", "a = :v\x0b", "x = :v ORb = :v",
        "a = :v ANDb = :v", "a = :v AND(b = :v)", "a=:v AND NOT(b=:v)", "a.#n = :v", "#n.a = :v",
        "#n[0] = :v", "a[0][1].b[2] = :v", "a[4294967296] = :v", "a[2147483648] = :v",
        "a[2147483647] = :v", "a[99999999999999999999] = :v",
    ]
    out = []
    for e in exprs:
        v = V if ":v" in e else None
        n = {"#n": "x"} if "#n" in e else None
        out.append(cond(e, values=v, names=n))
    return out


def condition_family():
    T = {":t": {"BOOL": True}}
    cases = [
        ("foo(a)", None, None), ("attribute_exists(a, b)", None, None), ("attribute_exists(:v)", V, None),
        ("size(a)", None, None), ("size(a) = :v", V, None), ("attribute_exists(a) = :v", V, None),
        ("name = :v", V, None), ("#n = :v", V, {"#n": "x"}), ("a = :w", V, None),
        ("a BETWEEN :v AND :v", V, None), ("a IN (:v)", V, None), ("a in (:v)", V, None),
        ("A = :v aNd b = :v", V, None), ("not attribute_exists(a)", None, None),
        ("a <> :v OR NOT (a < :v)", V, None), ("a.size = :v", V, None), ("size.a = :v", V, None),
        ("ATTRIBUTE_EXISTS(a)", None, None), ("Size(a) = :v", V, None),
        ("contains(a)", None, None), ("begins_with(a, :v, :v)", V, None),
        ("attribute_type(a, :v)", V, None), ("size(:v) = :v", V, None),
        ("foo(name)", None, None), ("size(name) = :v", V, None), ("name = :w", V, None),
        ("foo(:w)", V, None), ("attribute_exists(a, name)", None, None),
        ("a = :w AND name = :v", V, None), ("name = :v AND a = :w", V, None),
        ("a = :w AND foo(a)", V, None), ("foo(a) AND a = :w", V, None),
        ("#q = :v AND name = :v", V, None), ("a = :w AND #q = :v", V, None),
        ("a = a AND name = :v", None, None), ("a = a AND b = :w", V, None),
        ("b = :w AND a = a", V, None), ("((a = :v)) AND b = :w", V, None),
        ("b = :w AND ((a = :v))", V, None), ("((name = :v))", V, None),
        ("((a = :v)) AND foo(b)", V, None), ("foo(b) AND ((a = :v))", V, None),
        ("a < :t AND c = :w", T, None), ("c = :w AND a < :t", T, None),
        ("a < :t AND name = :t", T, None), ("attribute_exists(:w)", V, None),
        ("attribute_exists(:v, :w)", V, None), ("a = a AND x < :t", T, None),
        ("x < :t AND a = a", T, None), ("(a) = a", None, None), ("a = (a)", None, None),
        ("size(a) = :w AND foo(b)", V, None), ("(a = :v)", V, None), ("((a = :v))", V, None),
        ("a = :v AND (b = :v)", V, None), ("(a = :v) AND (b = :v)", V, None),
        ("((a = :v) AND b = :v)", V, None), ("NOT (a = :v)", V, None), ("NOT ((a = :v))", V, None),
        ("(a) = :v", V, None), ("a = (:v)", V, None), ("((a)) = :v", V, None),
        ("size((a)) = :v", V, None), ("attribute_exists((a))", None, None),
        ("(attribute_exists(a))", None, None), ("a IN ((:v))", V, None), ("a IN ((:v), b)", V, None),
        ("size(((a))) = :v", V, None), ("a IN (((:v)))", V, None), ("NOT NOT a = :v", V, None),
        ("#n = #n", None, {"#n": "x"}), ("a.b = a.b", None, None), ("a = b", None, None),
        ("size(a) = size(a)", None, None), ("a <> a", None, None), ("a < a", None, None),
        ("a BETWEEN a AND :v", V, None), ("a BETWEEN :v AND a", V, None), ("a IN (a)", None, None),
        ("a IN (:v, a)", V, None), ("begins_with(a, a)", None, None), ("contains(a, a)", None, None),
        ("a BETWEEN a AND :t", T, None), ("a < a AND b < :t", T, None), ("#n = a", None, {"#n": "a"}),
        ("a[0] = a[0]", None, None), ("a = a.b", None, None), ("a IN (b, b)", None, None),
        ("size(a, b)", None, None), ("size(a, b) = :v", V, None), ("attribute_exists(a, b) = :v", V, None),
        ("if_not_exists(a, :v) = :v", V, None), ("list_append(a, :v) = :v", V, None),
        ("attribute_exists(size(a))", None, None), ("size(size(a)) = :v", V, None),
        ("a = size(b)", V, None), ("attribute_type(a, size(b))", None, None),
        ("attribute_type(a, b)", None, None), ("begins_with(a, size(b))", None, None),
        ("contains(a, size(b))", None, None), ("begins_with(size(a), :s)", {":s": S1}, None),
        ("contains(size(a), :v)", V, None),
    ]
    out = [cond(e, values=v, names=n) for e, v, n in cases]
    typed = [
        ("a < :x", {"BOOL": True}), ("a < :x", {"L": []}), ("a < :x", {"NULL": True}),
        ("a < :x", {"M": {}}), ("a < :x", {"SS": ["a"]}), ("a < :x", {"B": "AA=="}),
        ("a < :x", S1), (":x < a", {"BOOL": True}), ("a <> :x", {"BOOL": True}),
        ("a >= :x", {"NS": ["1"]}), ("a <= :x", {"BS": ["AA=="]}), ("a > :x", {"L": []}),
        ("a = :x", {"L": []}), ("a IN (:x)", {"M": {}}), ("begins_with(a, :x)", {"B": "AA=="}),
        ("begins_with(a, :x)", {"SS": ["a"]}), ("begins_with(a, :x)", {"BOOL": True}),
        ("begins_with(a, :x)", N1), ("begins_with(:x, a)", S1), ("begins_with(:x, a)", N1),
        ("contains(a, :x)", {"L": []}), ("contains(a, :x)", {"NULL": True}),
        ("contains(:x, a)", N1), ("size(:x) = :v", {"S": "ab"}), ("size(:x) = :v", {"B": "AA=="}),
        ("size(:x) = :v", {"L": []}), ("size(:x) = :v", {"BOOL": True}),
        ("size(:x) = :v", {"NULL": True}), ("size(:x) = :v", {"SS": ["a"]}),
        ("size(:x) = :v", {"M": {}}), ("size(:x) = :v", N1), ("attribute_type(a, :x)", {"S": "FOO"}),
        ("attribute_type(a, :x)", {"S": "S"}), ("attribute_type(a, :x)", {"S": "s"}),
        ("attribute_type(a, :x)", {"S": "NULL"}), ("attribute_type(a, :x)", N1),
        ("attribute_type(a, :x)", {"SS": ["S"]}), ("attribute_type(:x, :v)", N1),
        ("size(a) < :x", S1), ("size(a) BETWEEN :x AND :x", S1),
        ("a BETWEEN :x AND :x", {"BOOL": True}), ("a BETWEEN :x AND :x", {"NULL": True}),
    ]
    for e, x in typed:
        vals = {":x": x}
        if ":v" in e:
            vals[":v"] = N1
        out.append(cond(e, values=vals))
    bounds = [
        ({"N": "5"}, N1), ({"N": "5"}, S1), ({"S": "b"}, {"S": "a"}), ({"B": "AQ=="}, {"B": "AA=="}),
        ({"B": "AQIDBAUG"}, {"B": "AA=="}), ({"SS": ["b"]}, {"SS": ["a"]}),
        ({"L": [N1]}, {"M": {"x": S1}}), ({"NS": ["2", "1"]}, {"BS": ["AA=="]}),
        ({"N": "1.50"}, {"S": "a\"b"}), ({"N": "1"}, {"N": "1.0"}), ({"S": "a"}, {"S": "a"}),
        ({"N": "-1E+3"}, {"N": "-1001"}), ({"S": "é"}, {"S": "z"}), ({"B": "/w=="}, {"B": "AA=="}),
    ]
    for lo, hi in bounds:
        out.append(cond("a BETWEEN :l AND :h", values={":l": lo, ":h": hi}))
    return out


def placeholder_family():
    out = [
        cond("a = :v", values={":v": N1, ":u": N1}),
        cond("a = :v", values=V, names={"#u": "x"}),
        cond("a = :w", values=V, names={"#u": "x"}),
        cond("#q = :v", values=V, names={"#n": "x"}),
        cond("#q = :w", values=V, names={"#n": "x"}),
        cond("a = :v", values={}),
        cond("a = :v", values=V, names={}),
        cond("a = :v", values={"v": N1}),
        cond("a = :v", values=V, names={"n": "x"}),
        cond("a = :v", values=V, names={"#n": ""}),
        cond("#n = :v", values=V, names={"#n": "name"}),
        cond("#n = :v", values=V, names={"#n": "a.b"}),
        cond("a.#n = :v", values=V, names={"#n": "x", "#m": "y"}),
        cond("a = :v", values={":v": {"S": ""}}),
        cond("a = :v", values={":v": {"N": "abc"}}),
        cond("a = :v", values={":v": {}}),
        cond("a = :v", values={":v": {"SS": ["a", "a"]}}),
        cond("a = :v", values={":v": {"NS": ["1", "1.0"]}}),
        cond("a = :v", values={":v": {"SS": []}}),
        cond("a = :v", values={":v": {"N": "1", "S": "x"}}),
        cond("a = :v", values={":v": {"NULL": False}}),
        cond("a = :v", values={":v": {"N": "1" + "0" * 40}}),
        cond("a = :v", values={":v": N1, ":z": N1, ":a": N1, ":m": N1}),
        cond("a = :v", values=V, names={"#z": "1", "#a": "1", "#B": "2"}),
        cond("a = :" + "v" * 254, values={":" + "v" * 254: N1}),
        cond("a = :" + "v" * 255, values={":" + "v" * 255: N1}),
        cond("#" + "v" * 255 + " = :v", values=V, names={"#" + "v" * 255: "x"}),
        cond("#" + "v" * 254 + " = :v", values=V, names={"#" + "v" * 254: "x"}),
        cond("a = :v", values={":v-1": N1}),
        cond("a = :v", values={":": N1}),
        cond("a = :v", values={"::v": N1}),
        cond("#n = :v", values=V, names={"#n": "a" * 65536}),
        cond("#n = :v", values=V, names={"#n": "é"}),
        cond("", values=None),
        cond("a = :v" + " " * (4096 - 6), values=V),
        cond("a = :v" + " " * (4097 - 6), values=V),
        cond("a = = :v" + " " * (4097 - 8), values=V),
        cond("a = :v" + " " * (4096 - 7) + "é", values=V),
        cond("a = :v" + " " * (4094 - 7) + "\u20ac", values=V),
        cond("a IN (" + ",".join([":v"] * 100) + ")", values=V),
        cond("a IN (" + ",".join([":v"] * 101) + ")", values=V),
        cond(" AND ".join(["a = :v"] * 150), values=V),
        cond(" AND ".join(["a = :v"] * 151), values=V),
        cond(" AND ".join(["attribute_exists(a)"] * 150)),
        cond(" AND ".join(["attribute_exists(a)"] * 151)),
        cond(" AND ".join(["size(a) = :v"] * 100), values=V),
        cond(" AND ".join(["size(a) = :v"] * 101), values=V),
        cond("NOT " * 299 + "a = :v", values=V),
        cond("NOT " * 300 + "a = :v", values=V),
        cond("a IN (" + ",".join([":v"] * 101) + ") AND name = :v", values=V),
        cond("name = :v AND a IN (" + ",".join([":v"] * 101) + ")", values=V),
        cond("a IN (" + ",".join([":v"] * 101) + ") AND b = :w", values=V),
        cond("a" + ".b" * 31 + " = :v", values=V),
        cond("a" + ".b" * 32 + " = :v", values=V),
        cond("a" + "[0]" * 32 + " = :v", values=V),
        cond("a" + ".b" * 32 + " = :v AND name = :v", values=V),
        cond("name = :v AND a" + ".b" * 32 + " = :v", values=V),
        cond("a" + ".b" * 32 + " = :w", values=V),
        cond("a[2147483648] = :v AND name = :v", values=V),
        cond("name = :v AND a[2147483648] = :v", values=V),
        cond("(" * 50 + "a = :v" + ")" * 50, values=V),
        cond("a" * 300 + " = :v", values=V),
    ]
    return out


def update_family():
    S = {":s": {"SS": ["a"]}}
    L = {":l": {"L": []}}
    cases = [
        ("SET a = :v", V), ("set a=:v", V), ("SET a = :v, b = :v", V), ("SET a = :v SET b = :v", V),
        ("SET a = :v REMOVE b", V), ("REMOVE b SET a = :v", V), ("REMOVE a, b.c[1]", None),
        ("REMOVE", None), ("SET", None), ("SET a", None), ("SET a =", None), ("SET a = :v,", V),
        ("SET a = :v + :v", V), ("SET a = :v - b", V), ("SET a = b + c + d", None),
        ("SET a = (b)", None), ("SET a = :v * :v", V), ("SET a = if_not_exists(a, :v)", V),
        ("SET a = if_not_exists(:v, a)", V), ("SET a = list_append(a, :l)", L),
        ("SET a = list_append(:v, a)", V), ("SET a = list_append(a)", None), ("SET a = size(b)", None),
        ("SET a = foo(b)", None), ("SET a = if_not_exists(a, :v) + :v", V),
        ("SET a = :v + if_not_exists(a, :v)", V), ("SET a = :s", S), ("ADD a :v", V), ("ADD a :s", S),
        ("ADD a :x", {":x": S1}), ("ADD a :x", {":x": {"L": []}}), ("ADD a :x", {":x": {"M": {}}}),
        ("ADD a :x", {":x": {"BOOL": True}}), ("ADD a :x", {":x": {"NULL": True}}),
        ("ADD a :x", {":x": {"B": "AA=="}}), ("ADD a :x", {":x": {"NS": ["1"]}}),
        ("ADD a :x", {":x": {"BS": ["AA=="]}}), ("ADD a.b :v", V), ("ADD a[0] :v", V), ("ADD a b", None),
        ("ADD a :v, b :v", V), ("ADD a = :v", V), ("DELETE a :s", S), ("DELETE a :v", V),
        ("DELETE a :x", {":x": S1}), ("DELETE a :x", {":x": {"L": []}}),
        ("DELETE a :x", {":x": {"NS": ["1"]}}), ("DELETE a[1] :s", S), ("DELETE a", None),
        ("SET a = :v REMOVE a", V), ("SET a = :v, a = :v", V), ("SET a.b = :v REMOVE a", V),
        ("SET a.b = :v, a[1] = :v", V), ("SET a[1] = :v, a[1].b = :v", V), ("SET a = :v, b = a", V),
        ("SET a = a", None), ("ADD a :v DELETE a :s", {**V, **S}), ("SET name = :v", V),
        ("SET a = :v + :x", {":v": N1, ":x": S1}), ("SET a = :x - :v", {":v": N1, ":x": S1}),
        ("SET a = list_append(:v, :l)", {**V, **L}), ("SET a = list_append(:l, :l)", L),
        ("SET a = if_not_exists(b.c, :v)", V), ("SET a = if_not_exists(b[0], :v)", V),
        ("SET a = if_not_exists(a)", None), ("SET a[0] = :v", V), ("REMOVE a[0], a[1]", None),
        ("REMOVE a, a", None), ("SET a = :v REMOVE b ADD c :v DELETE d :s", {**V, **S}),
        ("set a = :v remove b", V), ("SET a = :v AND b = :v", V), ("UPDATE a = :v", V), ("  ", None),
        ("SET a = b - :v + c", V), ("SET a = list_append(list_append(a, :l), :l)", L),
        ("SET a = if_not_exists(a, if_not_exists(b, :v))", V),
        ("SET a = list_append(a, :l) + :v", {**V, **L}), ("SET a = :v + :v + :v", V),
        ("SET a=:v-:v", V), ("SET a = -:v", V), ("SET a = :v REMOVE", V), ("SET a = :v, REMOVE b", V),
        ("REMOVE a SET", None), ("ADD a :v,", V), ("DELETE a :s b :s", S),
        ("SET a = ((:v)) SET b = :v", V), ("SET a = :v SET b = = :v", V),
        ("SET a = foo(b) SET b = :v", V), ("SET a = ((:v))", V), ("SET a = (:v + :v)", V),
        ("SET a = (:v + :v) + :v", V), ("SET a = ((:v + :v))", V), ("SET a = (:v) + (:v)", V),
        ("SET a = list_append((a), :l)", L), ("SET a = list_append(a, (:l))", L),
        ("SET a = if_not_exists(a, (:v + :v))", V), ("SET a = if_not_exists(a, :v + :v)", V),
        ("SET remove = :v", V), ("SET between = :v", V), ("SET a = :v REMOVE remove", V),
        ("REMOVE #r", None), ("SET a = attribute_exists(b)", None), ("SET a = contains(b, :v)", V),
        ("ADD #n :v", V), ("DELETE a :w", V), ("SET a = :w", V), ("REMOVE name", None),
        ("SET a = :v REMOVE b REMOVE c", V), ("ADD a :v ADD b :v", V), ("DELETE a :s DELETE b :s", S),
        ("REMOVE a REMOVE b SET c = :v SET d = :v", V), ("SET a = :v, b = :v, a = :v", V),
        ("SET a.b = :v, a.c = :v", V), ("SET a[0] = :v, a[1] = :v", V), ("SET a.b = :v, a.b.c = :v", V),
        ("REMOVE a.b, a[0]", None), ("SET a = :v ADD b :v REMOVE a", V),
        ("SET #a = :v, #b = :v", V), ("SET a = list_append(:l, :v)", {**V, **L}),
        ("SET a = list_append(if_not_exists(a, :l), :l)", L), ("SET a = :v - :v", V),
        ("SET a = if_not_exists(a, :x) + :v", {":x": S1, ":v": N1}),
        ("SET a = if_not_exists(a, :x)", {":x": S1}), ("SET a = list_append(a, size(b))", None),
    ]
    out = []
    for e, v in cases:
        names = None
        if "#r" in e:
            names = {"#r": "x"}
        if "#n" in e:
            names = {"#n": "x"}
        if "#a" in e:
            names = {"#a": "x", "#b": "x"}
        out.append(upd(e, values=v, names=names))
    out += [
        upd("SET #n.b = :v REMOVE #n", values=V, names={"#n": "x"}),
        upd("SET #n.b = :v REMOVE #n", values=V, names={"#n": "x.y"}),
        upd("SET a.#n = :v, a[1] = :v", values=V, names={"#n": "q"}),
        upd("SET pk = :v", values=V), upd("REMOVE sk"), upd("SET pk.x = :v", values=V),
        upd("REMOVE pk[0]"), upd("ADD sk :v", values=V), upd("SET a = :v, sk = :v, pk = :v", values=V),
        upd("SET pk = :v", values={":v": N1, ":u": N1}),
        upd("SET pk = :v", values=V, condition="a = = :v"),
        upd("SET a = = :v", values=V, condition="b = = :v"),
        upd("SET a = :w", values=V, condition="b = :x"),
        upd("SET name = :v", values=V, condition="b = = :v"),
        upd("SET a = :v", values={**V, ":u": N1}, condition="b = :v"),
        upd("SET a = :v", values={**V, ":u": N1, ":t": N1}, names={"#q": "x", "#r": "y"}, condition="b = :v"),
        upd("SET a = :v", values=V, condition="attribute_exists(#p)", names={"#p": "pk"}),
        upd(""),
        upd("SET " + ", ".join(f"a{i} = :v + :v" for i in range(160)), values=V),
        upd("SET " + ", ".join(f"a{i} = :v" for i in range(310)), values=V),
        upd("SET a = " + "(" * 3 + ":v" + ")" * 3, values=V),
        upd("SET a" + ".b" * 32 + " = :v", values=V),
        upd("SET a = b" + ".b" * 32, values=None),
        upd("SET a[2147483648] = :v", values=V),
        upd("SET a = :v" + " " * 4091, values=V),
    ]
    return out


def projection_family():
    exprs = [
        ("a", None), ("a, b", None), ("a,b.c[0]", None), ("a, a", None), ("a, a.b", None),
        ("a.b, a", None), ("a[0], a.b", None), ("a[0], a[1]", None), ("a.b, a.c", None), ("", None),
        ("a,", None), (", a", None), ("name", None), ("#n", {"#n": "x"}), ("#n, #n", {"#n": "x"}),
        ("#n, #m", {"#n": "x", "#m": "x"}), ("size(a)", None), ("a = b", None), (":v", None),
        ("a b", None), ("(a)", None), ("a.b.c.d", None), ("between", None), ("remove", None),
        ("#n", None), ("a", {"#n": "x"}), ("a" + ".b" * 32, None), ("a[2147483648]", None),
        ("name, a" + ".b" * 32, None), ("a" + ".b" * 32 + ", name", None), ("#q, name", {"#n": "x"}),
        ("name, #q", {"#n": "x"}), ("a.b[1].c, a.b[1]", None), ("a[1][2], a[1][2].c", None),
        ("a[1].b, a[1][0]", None), ("A, a", None),
    ]
    return [proj(e, names=n) for e, n in exprs]


def key_family():
    P = {":p": {"S": "k"}}
    PS = {":p": {"S": "k"}, ":s": N1}
    cases = [
        ("pk = :p", P, None, {}), ("pk = :p AND sk = :s", PS, None, {}), ("sk = :s AND pk = :p", PS, None, {}),
        ("pk = :p AND sk BETWEEN :a AND :b", {**P, ":a": N1, ":b": {"N": "2"}}, None, {}),
        ("pk = :p AND begins_with(sk, :s)", PS, None, {}),
        ("pk = :p AND begins_with(sk, :s)", {**P, ":s": {"S": "1"}}, None, {}),
        ("pk < :p", P, None, {}), ("pk = :p OR sk = :s", PS, None, {}), ("pk = :p AND sk <> :s", PS, None, {}),
        ("pk = :p AND sk IN (:s)", PS, None, {}), ("pk = :p AND a = :s", PS, None, {}), ("a = :p", P, None, {}),
        ("sk = :s", {":s": N1}, None, {}), ("pk = :p AND pk = :p", P, None, {}), ("pk = :s", {":s": N1}, None, {}),
        ("pk = :p AND sk = :s", {**P, ":s": S1}, None, {}), ("(pk = :p)", P, None, {}),
        ("(pk = :p) AND (sk = :s)", PS, None, {}), ("pk = :p AND sk = :s AND sk = :s", PS, None, {}),
        ("NOT pk = :p", P, None, {}), ("attribute_exists(pk)", None, None, {}), ("#k = :p", P, {"#k": "pk"}, {}),
        (":p = pk", P, None, {}), ("pk = pk", None, None, {}), ("pk.x = :p", P, None, {}),
        ("size(pk) = :p", P, None, {}), ("pk = :p AND sk > :s", PS, None, {}),
        ("pk = :p AND sk = :s", {**P, ":s": {"SS": ["1"]}}, None, {}),
        ("pk = :p AND sk BETWEEN :b AND :a", {**P, ":a": N1, ":b": {"N": "2"}}, None, {}),
        ("pk = :p AND contains(sk, :s)", PS, None, {}), ("pk = :p AND sk = :p", P, None, {}),
        ("pk=:p and sk>=:s", PS, None, {}), ("pk = :p AND attribute_exists(sk)", P, None, {}),
        ("pk = :p AND (sk = :s)", PS, None, {}), ("pk = :p AND sk = sk", P, None, {}),
        (":s < sk AND pk = :p", PS, None, {}), (":s >= sk AND pk = :p", PS, None, {}),
        ("pk = :p AND sk = :s AND x = :s", PS, None, {}), ("pk = :p AND sk[0] = :s", PS, None, {}),
        ("pk = x", None, None, {}), ("pk = :p AND sk = x", P, None, {}), ("pk BETWEEN :p AND :p", P, None, {}),
        ("begins_with(pk, :p)", P, None, {}), ("pk = :p AND NOT sk = :s", PS, None, {}),
        ("pk = :p AND size(sk) = :s", PS, None, {}), ("pk = :p", {**P, ":u": N1}, None, {}),
        ("pk < :p", {**P, ":u": N1}, None, {}), ("pk = :w", P, None, {"ProjectionExpression": "a,"}),
        ("pk = = :p", P, None, {"ProjectionExpression": "a,"}),
        ("pk = = :p", P, None, {"FilterExpression": "a = = :p", "ProjectionExpression": "a,"}),
        ("pk = :p", P, None, {"FilterExpression": "a = = :p", "ProjectionExpression": "a,"}),
        ("pk = :w", P, None, {"FilterExpression": "a = :x", "ProjectionExpression": "name"}),
        ("pk = :p", P, None, {"FilterExpression": "a = :x", "ProjectionExpression": "name"}),
        ("pk < :p", P, None, {"FilterExpression": "a = :w"}), ("pk < :p", P, None, {"ProjectionExpression": "name"}),
        ("pk = :p", P, None, {"FilterExpression": "pk = :p"}), ("pk = :p", PS, None, {"FilterExpression": "sk = :s"}),
        ("pk = :p", PS, None, {"FilterExpression": "a = :s"}), ("pk = :p", P, None, {"FilterExpression": "a.b = :p"}),
        ("pk = :p", P, None, {"ProjectionExpression": "pk, a"}), ("pk = :p", P, None, {"FilterExpression": "x = :p OR pk = :p"}),
        ("pk = :p", P, None, {"FilterExpression": "pk.x = :p"}), ("pk = :p", P, None, {"FilterExpression": "pk = :p", "ProjectionExpression": "name"}),
        ("pk = :p", {**P, ":u": N1}, None, {"FilterExpression": "pk = :p"}), ("pk < :p", P, None, {"FilterExpression": "sk = :p"}),
        ("pk = :p", P, {"#s": "sk"}, {"FilterExpression": "#s = :p"}),
    ]
    return [query(kc, values=v, names=n, **extra) for kc, v, n, extra in cases]


def request_family():
    """Parameter-combination rules and the legacy parameters."""
    P = {"pk": {"ComparisonOperator": "EQ", "AttributeValueList": [{"S": "k"}]}}
    VP = {":p": {"S": "k"}}
    out = []
    expected = [
        {"a": {"Exists": False}}, {"a": {"Exists": True}}, {"a": {"Value": N1}}, {"a": {"Value": N1, "Exists": True}},
        {"a": {"Value": N1, "Exists": False}}, {"a": {}}, {"a": {"ComparisonOperator": "EQ", "AttributeValueList": [N1]}},
        {"a": {"ComparisonOperator": "EQ"}}, {"a": {"ComparisonOperator": "EQ", "AttributeValueList": [N1, N1]}},
        {"a": {"ComparisonOperator": "NULL"}}, {"a": {"ComparisonOperator": "NULL", "AttributeValueList": [N1]}},
        {"a": {"ComparisonOperator": "NOT_NULL"}}, {"a": {"ComparisonOperator": "NE", "AttributeValueList": [N1]}},
        {"a": {"ComparisonOperator": "LT", "AttributeValueList": [{"BOOL": True}]}},
        {"a": {"ComparisonOperator": "LT", "AttributeValueList": [{"SS": ["x"]}]}},
        {"a": {"ComparisonOperator": "BETWEEN", "AttributeValueList": [N1]}},
        {"a": {"ComparisonOperator": "BETWEEN", "AttributeValueList": [{"N": "5"}, N1]}},
        {"a": {"ComparisonOperator": "BETWEEN", "AttributeValueList": [N1, S1]}},
        {"a": {"ComparisonOperator": "IN", "AttributeValueList": []}},
        {"a": {"ComparisonOperator": "IN", "AttributeValueList": [N1, S1]}},
        {"a": {"ComparisonOperator": "IN", "AttributeValueList": [{"L": []}]}},
        {"a": {"ComparisonOperator": "CONTAINS", "AttributeValueList": [{"SS": ["x"]}]}},
        {"a": {"ComparisonOperator": "CONTAINS", "AttributeValueList": [{"L": []}]}},
        {"a": {"ComparisonOperator": "CONTAINS", "AttributeValueList": [{"BOOL": True}]}},
        {"a": {"ComparisonOperator": "NOT_CONTAINS", "AttributeValueList": [N1]}},
        {"a": {"ComparisonOperator": "BEGINS_WITH", "AttributeValueList": [N1]}},
        {"a": {"ComparisonOperator": "BEGINS_WITH", "AttributeValueList": [S1]}},
        {"a": {"ComparisonOperator": "FOO", "AttributeValueList": [S1]}},
        {"a": {"ComparisonOperator": "EQ", "AttributeValueList": [S1], "Value": S1}},
        {"a": {"ComparisonOperator": "EQ", "AttributeValueList": [S1], "Exists": True}},
        {"a": {"Value": N1, "ComparisonOperator": "EQ"}}, {"a": {"Value": N1, "ComparisonOperator": "NULL"}},
        {"a": {"ComparisonOperator": "EQ", "AttributeValueList": [{"L": []}]}},
        {"a": {"ComparisonOperator": "NE", "AttributeValueList": [{"M": {}}]}},
        {"a": {"ComparisonOperator": "GT", "AttributeValueList": [{"B": "AA=="}]}},
        {"a": {"Exists": False}, "b": {"Exists": False}}, {"a": {"Exists": False, "Value": N1}},
        {"a": {"ComparisonOperator": "eq", "AttributeValueList": [N1]}}, {"a": {"Value": {"L": []}}},
        {"a": {"Exists": False, "AttributeValueList": [N1]}}, {"a": {"AttributeValueList": [N1]}},
        {"a": {"Value": {"N": "x"}}}, {"a": {"ComparisonOperator": "EQ", "AttributeValueList": [{"N": "x"}]}},
        {"pk": {"Exists": False}}, {"a.b": {"Exists": False}}, {"": {"Exists": False}},
        {"a": {"Exists": True, "ComparisonOperator": "NULL"}},
        {"a": {"ComparisonOperator": "IN", "AttributeValueList": [N1, {"N": "2"}, {"N": "3"}]}},
        {"a": {"ComparisonOperator": "BETWEEN", "AttributeValueList": [N1, N1, N1]}},
    ]
    for item in (None, ITEM):
        for e in expected:
            out.append(req("DeleteItem", item=item, Expected=e))
    for item in (None, ITEM):
        out.append(req("DeleteItem", item=item, Expected={"a": {"Exists": False}, "s": {"Value": {"S": "hello"}}}, ConditionalOperator="OR"))
        out.append(req("DeleteItem", item=item, Expected={"a": {"Exists": False}, "s": {"Value": {"S": "hello"}}}, ConditionalOperator="AND"))
        out.append(req("DeleteItem", item=item, Expected={"zz": {"ComparisonOperator": "NOT_CONTAINS", "AttributeValueList": [S1]}}))
        out.append(req("DeleteItem", item=item, Expected={"zz": {"ComparisonOperator": "NE", "AttributeValueList": [S1]}}))
        out.append(req("DeleteItem", item=item, Expected={"ss": {"ComparisonOperator": "NOT_CONTAINS", "AttributeValueList": [{"S": "p"}]}}))
        out.append(req("DeleteItem", item=item, Expected={"ss": {"ComparisonOperator": "CONTAINS", "AttributeValueList": [{"S": "p"}]}}))
        out.append(req("DeleteItem", item=item, Expected={"l": {"ComparisonOperator": "CONTAINS", "AttributeValueList": [N1]}}))
        out.append(req("DeleteItem", item=item, Expected={"s": {"ComparisonOperator": "BEGINS_WITH", "AttributeValueList": [{"S": "he"}]}}))
        out.append(req("DeleteItem", item=item, Expected={"a": {"ComparisonOperator": "IN", "AttributeValueList": [{"N": "3"}, N1]}}))
    out += [
        req("DeleteItem", Expected={"a": {"Exists": False}}, ConditionalOperator="OR"),
        req("DeleteItem", Expected={"a": {"Exists": False}}, ConditionalOperator="FOO"),
        req("DeleteItem", ConditionalOperator="OR"),
        req("DeleteItem", ConditionExpression="attribute_exists(a)", ConditionalOperator="OR"),
        req("DeleteItem", Expected={"a": {"Exists": False}}, ExpressionAttributeNames={"#a": "x"}),
        req("DeleteItem", ConditionExpression="a = :v", ExpressionAttributeValues=V, Expected={"a": {"Exists": False}}),
        req("DeleteItem", ExpressionAttributeNames={"#n": "x"}),
        req("DeleteItem", ExpressionAttributeValues=V),
        req("PutItem", Item={"pk": {"S": "k"}, "sk": N1}, ExpressionAttributeValues=V),
        req("PutItem", Item={"pk": {"S": "k"}, "sk": N1}, Expected={"a": {"Exists": False}}, ConditionExpression="attribute_not_exists(a)", ConditionalOperator="AND"),
        req("DeleteItem", Expected={"a": {"Exists": False}}, ConditionalOperator="AND", ConditionExpression="attribute_not_exists(a)"),
    ]
    updates = [
        {"a": {"Value": N1, "Action": "PUT"}}, {"a": {"Action": "PUT"}}, {"a": {"Action": "DELETE"}},
        {"a": {"Action": "DELETE", "Value": N1}}, {"ss": {"Action": "DELETE", "Value": {"SS": ["p"]}}},
        {"a": {"Action": "ADD", "Value": N1}}, {"l": {"Action": "ADD", "Value": {"L": [S1]}}},
        {"s": {"Action": "ADD", "Value": S1}}, {"a": {"Action": "ADD", "Value": {"SS": ["x"]}}},
        {"ss": {"Action": "ADD", "Value": {"SS": ["y"]}}}, {"ss": {"Action": "DELETE", "Value": {"SS": ["p", "q"]}}},
        {"a": {"Action": "ADD"}}, {"a": {"Action": "FOO", "Value": N1}}, {"pk": {"Action": "PUT", "Value": S1}},
        {"sk": {"Action": "DELETE"}}, {"a.b": {"Action": "PUT", "Value": N1}}, {"": {"Action": "PUT", "Value": N1}},
        {"zz": {"Action": "DELETE", "Value": {"SS": ["x"]}}}, {"zz": {"Action": "ADD", "Value": {"M": {}}}},
        {"zz": {"Action": "ADD", "Value": {"BOOL": True}}}, {"zz": {"Action": "DELETE", "Value": {"L": []}}},
        {"l": {"Action": "ADD", "Value": N1}}, {"zz": {"Action": "ADD", "Value": {"L": [N1]}}},
        {"ns": {"Action": "ADD", "Value": {"NS": ["3"]}}}, {"ns": {"Action": "DELETE", "Value": {"SS": ["1"]}}},
        {"a": {"Action": "DELETE", "Value": {"NS": ["1"]}}}, {"zz": {"Action": "ADD", "Value": {"NS": ["1"]}}},
        {"a": {"Action": "PUT", "Value": {"S": ""}}}, {},
    ]
    for item in (None, ITEM):
        for u in updates:
            out.append(req("UpdateItem", item=item, AttributeUpdates=u))
    out += [
        req("UpdateItem", AttributeUpdates={"a": {"Action": "PUT", "Value": N1}}, UpdateExpression="SET a = :v", ExpressionAttributeValues=V),
        req("UpdateItem", AttributeUpdates={"a": {"Action": "PUT", "Value": N1}}, ConditionExpression="attribute_exists(a)"),
        req("UpdateItem", item=ITEM, AttributeUpdates={"a": {"Action": "PUT", "Value": {"N": "2"}}}, Expected={"a": {"Value": N1}}),
        req("UpdateItem", item=ITEM, AttributeUpdates={"a": {"Action": "PUT", "Value": {"N": "2"}}}, Expected={"a": {"Value": {"N": "5"}}}),
        req("UpdateItem", Expected={"a": {"Exists": False}}, AttributeUpdates={"a": {"Action": "DELETE"}}, UpdateExpression="REMOVE a", ConditionExpression="attribute_exists(a)"),
        req("UpdateItem", AttributeUpdates={"a": {"Action": "DELETE"}}, ExpressionAttributeNames={"#a": "x"}),
        req("UpdateItem", AttributeUpdates={"a": {"Action": "DELETE"}}, ConditionExpression="attribute_not_exists(#a)", ExpressionAttributeNames={"#a": "a"}),
        req("UpdateItem", ExpressionAttributeValues=V),
        req("UpdateItem", ExpressionAttributeNames={"#p": "x"}),
        req("UpdateItem", Expected={"a": {"Exists": False}}, UpdateExpression="SET a = :p", ExpressionAttributeValues=VP),
        req("UpdateItem", AttributeUpdates={"a": {"Action": "DELETE"}}, ExpressionAttributeValues=VP),
    ]
    kcs = [
        P, {**P, "sk": {"ComparisonOperator": "GT", "AttributeValueList": [N1]}},
        {"pk": {"ComparisonOperator": "LT", "AttributeValueList": [{"S": "k"}]}},
        {"sk": {"ComparisonOperator": "GT", "AttributeValueList": [N1]}},
        {"pk": {"ComparisonOperator": "EQ", "AttributeValueList": [N1]}},
        {**P, "x": {"ComparisonOperator": "EQ", "AttributeValueList": [N1]}},
        {**P, "sk": {"ComparisonOperator": "BEGINS_WITH", "AttributeValueList": [S1]}},
        {**P, "sk": {"ComparisonOperator": "BETWEEN", "AttributeValueList": [N1, {"N": "3"}]}},
        {**P, "sk": {"ComparisonOperator": "BETWEEN", "AttributeValueList": [{"N": "3"}, N1]}},
        {"pk": {"ComparisonOperator": "EQ"}}, {"pk": {"ComparisonOperator": "NULL"}},
        {"pk": {"AttributeValueList": [{"S": "k"}]}}, {},
        {**P, "sk": {"ComparisonOperator": "IN", "AttributeValueList": [N1]}},
        {**P, "sk": {"ComparisonOperator": "CONTAINS", "AttributeValueList": [N1]}},
        {"pk": {"ComparisonOperator": "NE", "AttributeValueList": [{"S": "k"}]}},
        {"pk": {"ComparisonOperator": "EQ", "AttributeValueList": [{"S": "k"}], "Exists": True}},
        {**P, "sk": {"ComparisonOperator": "LE", "AttributeValueList": [N1]}},
    ]
    for kc in kcs:
        out.append(req("Query", item=ITEM, KeyConditions=kc))
    out += [
        req("Query", item=ITEM, KeyConditions=P, QueryFilter={"a": {"ComparisonOperator": "EQ", "AttributeValueList": [N1]}}),
        req("Query", item=ITEM, KeyConditions=P, QueryFilter={"sk": {"ComparisonOperator": "EQ", "AttributeValueList": [N1]}}),
        req("Query", item=ITEM, KeyConditions=P, QueryFilter={"a": {"ComparisonOperator": "EQ", "AttributeValueList": [N1]}, "b": {"ComparisonOperator": "NULL"}}, ConditionalOperator="OR"),
        req("Query", item=ITEM, KeyConditions=P, QueryFilter={"a": {"ComparisonOperator": "EQ", "AttributeValueList": [N1]}, "b": {"ComparisonOperator": "NULL"}}, ConditionalOperator="AND"),
        req("Query", item=ITEM, KeyConditions=P, QueryFilter={"a": {"Exists": True}}),
        req("Query", KeyConditionExpression="pk = :p", ExpressionAttributeValues=VP, QueryFilter={"a": {"ComparisonOperator": "NULL"}}),
        req("Query", item=ITEM, KeyConditions=P, FilterExpression="a = :v", ExpressionAttributeValues=V),
        req("Query", item=ITEM, KeyConditions=P, AttributesToGet=["a", "pk"]),
        req("Query", item=ITEM, KeyConditions=P, AttributesToGet=["a", "a"]),
        req("Query", item=ITEM, KeyConditions=P, AttributesToGet=["a"], ProjectionExpression="a"),
        req("Query", KeyConditionExpression="pk = :p", ExpressionAttributeValues=VP, AttributesToGet=["a"]),
        req("Query", item=ITEM, KeyConditions=P, AttributesToGet=[]),
        req("Query", item=ITEM, KeyConditions=P, AttributesToGet=["a"], Select="ALL_ATTRIBUTES"),
        req("Query"),
        req("Query", KeyConditions=P, KeyConditionExpression="pk = :p", ExpressionAttributeValues=VP),
        req("Query", KeyConditions=P, ExpressionAttributeValues=VP),
        req("Query", KeyConditions=P, ConditionalOperator="AND"),
        req("Query", item=ITEM, KeyConditions=P, ProjectionExpression="a"),
        req("Query", KeyConditions=P, KeyConditionExpression="pk = :p", FilterExpression="a = :p", ExpressionAttributeValues=VP),
        req("Query", KeyConditions=P, QueryFilter={"a": {"ComparisonOperator": "NULL"}}, FilterExpression="a = :p", ExpressionAttributeValues=VP),
        req("Query", KeyConditions=P, AttributesToGet=["a"], ProjectionExpression="a"),
        req("Query", KeyConditionExpression="pk = :p", ExpressionAttributeValues=VP, AttributesToGet=["a"], QueryFilter={"a": {"ComparisonOperator": "NULL"}, "b": {"ComparisonOperator": "NULL"}}, ConditionalOperator="OR"),
        req("Query", KeyConditions=P, AttributesToGet=["a"], QueryFilter={"a": {"ComparisonOperator": "NULL"}, "b": {"ComparisonOperator": "NULL"}}, ConditionalOperator="OR", FilterExpression="a = :p", ProjectionExpression="a", ExpressionAttributeValues=VP),
        req("Query", item=ITEM, KeyConditions=P, FilterExpression="#a = :p", ExpressionAttributeValues=VP, ExpressionAttributeNames={"#a": "x"}),
        req("Query", KeyConditions=P, ExpressionAttributeNames={"#a": "x"}),
        req("Query", item=ITEM, KeyConditions=P, ProjectionExpression="#a", ExpressionAttributeNames={"#a": "a"}),
        req("Scan", item=ITEM, ScanFilter={"pk": {"ComparisonOperator": "EQ", "AttributeValueList": [{"S": "k"}]}}),
        req("Scan", item=ITEM, ScanFilter={"a": {"ComparisonOperator": "NULL"}, "b": {"ComparisonOperator": "NULL"}}, ConditionalOperator="OR"),
        req("Scan", ScanFilter={"a": {"ComparisonOperator": "NULL"}}, FilterExpression="attribute_exists(a)"),
        req("Scan", ScanFilter={"a": {"ComparisonOperator": "NULL"}}, ProjectionExpression="a"),
        req("Scan", item=ITEM, ScanFilter={"zz": {"ComparisonOperator": "NULL"}}, AttributesToGet=["a"]),
        req("Scan", ScanFilter={"a": {"ComparisonOperator": "NULL"}, "b": {"ComparisonOperator": "NULL"}}, ConditionalOperator="OR", AttributesToGet=["a"], FilterExpression="attribute_exists(a)", ProjectionExpression="a"),
        req("Scan", ExpressionAttributeValues=VP), req("Scan", ProjectionExpression="a", ExpressionAttributeValues=VP),
        req("Scan", item=ITEM, FilterExpression="pk = :p", ExpressionAttributeValues=VP),
        req("Scan", FilterExpression="a = = :p", ProjectionExpression="a,", ExpressionAttributeValues=VP),
        req("Scan", FilterExpression="a = :p", ProjectionExpression="a,", ExpressionAttributeValues=VP),
        req("GetItem", item=ITEM, AttributesToGet=["a.b", "s"]),
        req("GetItem", item=ITEM, AttributesToGet=["l[0]", "a"]),
        req("GetItem", AttributesToGet=["a"], ProjectionExpression="a"),
        req("GetItem", ExpressionAttributeNames={"#a": "a"}),
        req("GetItem", item=ITEM, ExpressionAttributeValues=VP),
        req("GetItem", item=ITEM, ProjectionExpression="a", ExpressionAttributeValues=VP),
        req("GetItem", item=ITEM, ProjectionExpression="#a", ExpressionAttributeNames={"#a": "x", "#b": "y"}),
    ]
    return out


def eval_family():
    """Conditions, updates and projections evaluated on a known item."""
    out = []
    conds = [
        ("a = :x", {":x": N1}), ("a = :x", {":x": {"N": "1.0"}}), ("a = :x", {":x": {"S": "1"}}),
        ("a <> :x", {":x": {"S": "1"}}), ("zz <> :x", {":x": N1}), ("zz = :x", {":x": N1}),
        ("a < :x", {":x": {"N": "2"}}), ("s < :x", {":x": {"S": "hz"}}), ("s < :x", {":x": N1}),
        ("b < :x", {":x": {"B": "AQIE"}}), ("b > :x", {":x": {"B": "AQ=="}}), ("a BETWEEN :x AND :y", {":x": N1, ":y": N1}),
        ("s BETWEEN :x AND :y", {":x": {"S": "a"}, ":y": {"S": "i"}}), ("a BETWEEN :x AND :y", {":x": {"S": "a"}, ":y": {"S": "b"}}),
        ("a IN (:x, :y)", {":x": {"N": "3"}, ":y": N1}), ("a IN (:x)", {":x": S1}), ("zz IN (:x)", {":x": N1}),
        ("l IN (:x)", {":x": {"L": [N1, {"S": "two"}, {"M": {"x": N1}}]}}), ("m = :x", {":x": {"M": {"y": {"S": "why"}, "x": N1, "n": {"M": {"deep": {"N": "3"}}}}}}),
        ("ss = :x", {":x": {"SS": ["q", "p"]}}), ("ns = :x", {":x": {"NS": ["2", "1.00"]}}), ("bs = :x", {":x": {"BS": ["AQ==", "AA=="]}}),
        ("t = :x", {":x": {"BOOL": True}}), ("z = :x", {":x": {"NULL": True}}), ("t <> :x", {":x": {"BOOL": False}}),
        ("attribute_type(a, :x)", {":x": {"S": "N"}}), ("attribute_type(z, :x)", {":x": {"S": "NULL"}}),
        ("attribute_type(zz, :x)", {":x": {"S": "N"}}), ("attribute_type(m.n, :x)", {":x": {"S": "M"}}),
        ("begins_with(s, :x)", {":x": {"S": "he"}}), ("begins_with(s, :x)", {":x": {"S": "hello!"}}),
        ("begins_with(b, :x)", {":x": {"B": "AQ=="}}), ("begins_with(s, :x)", {":x": {"B": "aGU="}}),
        ("begins_with(a, :x)", {":x": {"S": "1"}}), ("contains(s, :x)", {":x": {"S": "ell"}}),
        ("contains(ss, :x)", {":x": {"S": "p"}}), ("contains(ss, :x)", {":x": {"S": "z"}}),
        ("contains(ns, :x)", {":x": {"N": "2.0"}}), ("contains(bs, :x)", {":x": {"B": "AQ=="}}),
        ("contains(l, :x)", {":x": {"S": "two"}}), ("contains(l, :x)", {":x": {"M": {"x": N1}}}),
        ("contains(m, :x)", {":x": {"S": "x"}}), ("contains(b, :x)", {":x": {"B": "Ag=="}}),
        ("contains(s, :x)", {":x": N1}), ("contains(a, :x)", {":x": N1}), ("contains(:x, s)", {":x": {"S": "say hello"}}),
        ("size(s) = :x", {":x": {"N": "5"}}), ("size(e) = :x", {":x": N1}), ("size(b) = :x", {":x": {"N": "3"}}),
        ("size(ss) = :x", {":x": {"N": "2"}}), ("size(l) = :x", {":x": {"N": "3"}}), ("size(m) = :x", {":x": {"N": "3"}}),
        ("size(a) = :x", {":x": N1}), ("size(t) = :x", {":x": N1}), ("size(zz) = :x", {":x": {"N": "0"}}),
        ("size(zz) <> :x", {":x": {"N": "0"}}), ("size(m.n) > :x", {":x": {"N": "0"}}),
        ("l[0] = :x", {":x": N1}), ("l[2].x = :x", {":x": N1}), ("l[5] = :x", {":x": N1}), ("m.n.deep = :x", {":x": {"N": "3"}}),
        ("m.x.y = :x", {":x": N1}), ("a.b = :x", {":x": N1}), ("a[0] = :x", {":x": N1}), ("m[0] = :x", {":x": N1}),
        ("attribute_exists(l[1])", None), ("attribute_exists(l[3])", None), ("attribute_not_exists(m.q)", None),
        ("attribute_exists(m.n.deep)", None), ("NOT attribute_exists(a) OR a = :x", {":x": N1}),
        ("a = :x AND s = :y", {":x": N1, ":y": {"S": "hello"}}), ("a = :x OR s = :y", {":x": {"N": "9"}, ":y": {"S": "nope"}}),
        ("size(s) BETWEEN :x AND :y", {":x": N1, ":y": {"N": "9"}}), ("a = s", None), ("ss = ss2", None),
        (":x = :y", {":x": N1, ":y": {"N": "1.0"}}), (":x < :y", {":x": {"S": "a"}, ":y": {"S": "b"}}),
        ("s = :x", {":x": {"S": "hello"}}), ("e = :x", {":x": {"S": "é"}}), ("e > :x", {":x": {"S": "z"}}),
        ("size(:x) = :y", {":x": {"S": "é"}, ":y": N1}), ("size(:x) = :y", {":x": {"S": "\U0001F600"}, ":y": {"N": "2"}}),
        ("a > :x", {":x": {"N": "-5"}}), ("a >= :x", {":x": {"N": "1"}}), ("a <= :x", {":x": {"N": "0.99999"}}),
    ]
    for item in (ITEM, None):
        for e, v in conds:
            out.append(cond(e, values=v, item=item))
    updates = [
        ("SET a = :x", {":x": {"N": "7"}}), ("SET n = :x", {":x": S1}), ("SET a = a + :x", {":x": {"N": "2.5"}}),
        ("SET a = a - :x", {":x": {"N": "2"}}), ("SET a = s + :x", {":x": N1}), ("SET a = zz + :x", {":x": N1}),
        ("SET q = if_not_exists(q, :x)", {":x": N1}), ("SET a = if_not_exists(a, :x)", {":x": {"N": "9"}}),
        ("SET l = list_append(l, :x)", {":x": {"L": [{"S": "end"}]}}), ("SET l = list_append(:x, l)", {":x": {"L": [{"S": "start"}]}}),
        ("SET s = list_append(s, :x)", {":x": {"L": []}}), ("SET q = list_append(zz, :x)", {":x": {"L": []}}),
        ("SET q = list_append(if_not_exists(q, :e), :x)", {":x": {"L": [N1]}, ":e": {"L": []}}),
        ("SET m.x = :x", {":x": {"N": "5"}}), ("SET m.new = :x", {":x": {"N": "5"}}), ("SET m.n.deep = :x", {":x": {"N": "5"}}),
        ("SET m.q.r = :x", {":x": N1}), ("SET a.b = :x", {":x": N1}), ("SET l[0] = :x", {":x": {"S": "zero"}}),
        ("SET l[9] = :x", {":x": {"S": "nine"}}), ("SET l[2].y = :x", {":x": N1}), ("SET s[0] = :x", {":x": N1}),
        ("SET m[0] = :x", {":x": N1}), ("SET zz.a = :x", {":x": N1}), ("SET zz[0] = :x", {":x": N1}),
        ("SET a = :x, b2 = a", {":x": {"N": "9"}}), ("SET b2 = a, a = :x", {":x": {"N": "9"}}),
        ("REMOVE a", None), ("REMOVE zz", None), ("REMOVE m.x", None), ("REMOVE m.q", None), ("REMOVE m.q.r", None),
        ("REMOVE l[0]", None), ("REMOVE l[0], l[1]", None), ("REMOVE l[1], l[0]", None), ("REMOVE l[7]", None),
        ("REMOVE a.b", None), ("REMOVE a[0]", None), ("REMOVE zz.a", None), ("REMOVE zz[0]", None),
        ("REMOVE m.n.deep", None), ("REMOVE l[2].x", None),
        ("ADD a :x", {":x": {"N": "4"}}), ("ADD q :x", {":x": {"N": "4"}}), ("ADD ss :x", {":x": {"SS": ["r", "p"]}}),
        ("ADD q :x", {":x": {"SS": ["r"]}}), ("ADD ss :x", {":x": {"NS": ["1"]}}), ("ADD s :x", {":x": N1}),
        ("ADD a :x", {":x": {"SS": ["r"]}}), ("ADD ns :x", {":x": {"NS": ["3", "1"]}}), ("ADD bs :x", {":x": {"BS": ["Ag=="]}}),
        ("ADD m.x :x", {":x": N1}), ("ADD m.q :x", {":x": N1}), ("ADD zz.q :x", {":x": N1}), ("ADD l[0] :x", {":x": N1}),
        ("ADD l :x", {":x": {"SS": ["a"]}}),
        ("DELETE ss :x", {":x": {"SS": ["p"]}}), ("DELETE ss :x", {":x": {"SS": ["p", "q"]}}), ("DELETE ss :x", {":x": {"SS": ["z"]}}),
        ("DELETE q :x", {":x": {"SS": ["p"]}}), ("DELETE ns :x", {":x": {"NS": ["2.0"]}}), ("DELETE a :x", {":x": {"NS": ["1"]}}),
        ("DELETE ss :x", {":x": {"NS": ["1"]}}), ("DELETE m.q :x", {":x": {"SS": ["p"]}}), ("DELETE zz.q :x", {":x": {"SS": ["p"]}}),
        ("DELETE bs :x", {":x": {"BS": ["AA==", "AQ=="]}}),
        ("SET a = :x REMOVE s ADD q :y DELETE ss :z", {":x": {"N": "2"}, ":y": N1, ":z": {"SS": ["q"]}}),
        ("SET a = a + :x", {":x": {"N": "9" * 38}}), ("SET a = :x + :y", {":x": {"N": "9" * 38}, ":y": {"N": "9" * 38}}),
        ("SET a = :x - :y", {":x": {"N": "1E-130"}, ":y": {"N": "0.9E-130"}}), ("ADD a :x", {":x": {"N": "9" * 38}}),
        ("SET a = :x + :y", {":x": {"N": "1E+125"}, ":y": {"N": "9E+125"}}),
        ("SET m.n = :x", {":x": {"M": {}}}), ("SET l = :x", {":x": {"L": []}}),
        ("SET m.n.deep = m.x + :x", {":x": N1}), ("SET l[1] = l[0]", None), ("SET q = m", None),
        ("SET #k = :x", {":x": N1}),
    ]
    for item in (ITEM, None):
        for e, v in updates:
            names = {"#k": "pk2"} if "#k" in e else None
            out.append(upd(e, values=v, names=names, item=item))
    for item in (ITEM, None):
        out.append(upd("SET a = :x", values={":x": {"N": "2"}}, item=item, condition="attribute_exists(a)"))
        out.append(upd("SET a = s + :x", values={":x": N1}, item=item, condition="attribute_not_exists(a)"))
    projs = [
        "a", "a, s", "m.x", "m.n.deep", "m.x, m.y", "m.n, m.x", "l[0]", "l[2], l[0]", "l[1], l[5]", "l[2].x",
        "l[0], l[2].x", "zz", "m.zz", "l[9]", "a.b", "s[0]", "m.n.zz", "l[2].zz", "pk", "pk, sk",
        "l[2].x, l[1]", "m.n.deep, m.y", "ss, ns, bs", "t, z, e",
    ]
    for item in (ITEM, None):
        for p in projs:
            out.append(proj(p, item=item))
            out.append(proj(p, item=item, op="Scan"))
    out.append(query("pk = :p", values={":p": {"S": "k"}, ":x": N1}, item=ITEM, FilterExpression="a = :x", ProjectionExpression="s, m.x"))
    out.append(query("pk = :p", values={":p": {"S": "k"}, ":x": {"N": "2"}}, item=ITEM, FilterExpression="a = :x"))
    out.append(query("pk = :p AND sk BETWEEN :x AND :y", values={":p": {"S": "k"}, ":x": N1, ":y": {"N": "2"}}, item=ITEM))
    out.append(query("pk = :p AND sk > :x", values={":p": {"S": "k"}, ":x": N1}, item=ITEM))
    out.append(query("pk = :p AND sk >= :x", values={":p": {"S": "k"}, ":x": N1}, item=ITEM))
    out.append(query(":x < sk AND pk = :p", values={":p": {"S": "k"}, ":x": {"N": "0"}}, item=ITEM))
    out.append(query("pk = :p", values={":p": {"S": "other"}}, item=ITEM))
    return out


# ---------------------------------------------------------------------------
# Random cases
# ---------------------------------------------------------------------------

NAMES = ["a", "s", "l", "m", "ss", "ns", "b", "t", "z", "zz", "q", "x", "e"]
VALUES = {
    ":n": N1, ":n2": {"N": "2"}, ":s": {"S": "he"}, ":s2": {"S": "hello"}, ":b": {"B": "AQ=="},
    ":t": {"BOOL": True}, ":z": {"NULL": True}, ":l": {"L": [N1]}, ":m": {"M": {"x": N1}},
    ":ss": {"SS": ["p"]}, ":ns": {"NS": ["1"]}, ":ty": {"S": "N"}, ":ty2": {"S": "SS"},
}


def rand_path(r, depth=0):
    p = r.choice(NAMES + ["#n", "#m"])
    for _ in range(r.choice([0, 0, 0, 1, 1, 2])):
        if r.random() < 0.5:
            p += "." + r.choice(["x", "y", "n", "deep", "#n"])
        else:
            p += "[" + str(r.choice([0, 1, 2, 5])) + "]"
    return p


def rand_operand(r):
    k = r.random()
    if k < 0.45:
        return rand_path(r)
    if k < 0.85:
        return r.choice(list(VALUES))
    if k < 0.95:
        return "size(" + rand_path(r) + ")"
    return "(" + rand_operand(r) + ")"


def rand_condition(r, depth=0):
    k = r.random()
    if depth < 3 and k < 0.25:
        op = r.choice(["AND", "OR", "and", "Or"])
        return rand_condition(r, depth + 1) + " " + op + " " + rand_condition(r, depth + 1)
    if depth < 3 and k < 0.32:
        return "NOT " + rand_condition(r, depth + 1)
    if depth < 3 and k < 0.38:
        return "(" + rand_condition(r, depth + 1) + ")"
    k = r.random()
    if k < 0.45:
        return rand_operand(r) + " " + r.choice(["=", "<>", "<", "<=", ">", ">="]) + " " + rand_operand(r)
    if k < 0.53:
        return rand_operand(r) + " BETWEEN " + rand_operand(r) + " AND " + rand_operand(r)
    if k < 0.61:
        return rand_operand(r) + " IN (" + ", ".join(rand_operand(r) for _ in range(r.randint(1, 3))) + ")"
    f = r.choice(["attribute_exists", "attribute_not_exists", "attribute_type", "begins_with", "contains"])
    if f.startswith("attribute_") and f != "attribute_type":
        return f + "(" + rand_path(r) + ")"
    second = r.choice([":ty", ":ty2"]) if f == "attribute_type" else rand_operand(r)
    return f + "(" + rand_path(r) + ", " + second + ")"


def rand_update(r):
    clauses = []
    kinds = r.sample(["SET", "REMOVE", "ADD", "DELETE"], r.randint(1, 3))
    for kind in kinds:
        acts = []
        for _ in range(r.randint(1, 2)):
            p = rand_path(r)
            if kind == "SET":
                v = r.choice([
                    rand_operand(r), rand_operand(r) + " + " + rand_operand(r), rand_operand(r) + " - " + rand_operand(r),
                    "if_not_exists(" + rand_path(r) + ", " + rand_operand(r) + ")",
                    "list_append(" + rand_operand(r) + ", " + rand_operand(r) + ")",
                ])
                acts.append(p + " = " + v)
            elif kind == "REMOVE":
                acts.append(p)
            else:
                acts.append(p + " " + r.choice([":n", ":ss", ":ns", ":s", ":l"]))
        clauses.append(kind + " " + ", ".join(acts))
    return " ".join(clauses)


def placeholders_for(text, r):
    vals = {k: v for k, v in VALUES.items() if k in tokens_of(text)}
    names = {}
    for n in ("#n", "#m"):
        if n in tokens_of(text):
            names[n] = r.choice(["x", "a", "m", "name"])
    if r.random() < 0.1:
        vals[":unused"] = N1
    if r.random() < 0.05 and vals:
        vals.pop(r.choice(list(vals)))
    return (vals or None), (names or None)


def tokens_of(text):
    import re
    return set(re.findall(r"[#:][A-Za-z0-9_]+", text))


TOKENS = ["=", "<>", "<", ">=", "(", ")", ",", ".", "[", "]", "0", "1", "AND", "OR", "NOT", "BETWEEN",
          "IN", "a", ":n", "#n", "size", "SET", "REMOVE", "+", "-", "!", "@", "é", " ", "attribute_exists("]


def mutate(r, text):
    """A token-level mutation: delete, insert, duplicate or swap."""
    import re
    toks = re.findall(r"[#:]?[A-Za-z0-9_]+|<>|<=|>=|\s+|.", text)
    if not toks:
        return text
    k = r.random()
    i = r.randrange(len(toks))
    if k < 0.3:
        del toks[i]
    elif k < 0.6:
        toks.insert(i, r.choice(TOKENS))
    elif k < 0.8:
        toks.insert(i, toks[i])
    else:
        j = r.randrange(len(toks))
        toks[i], toks[j] = toks[j], toks[i]
    return "".join(toks)


def random_family(r):
    out = []
    for _ in range(700):
        e = rand_condition(r)
        if r.random() < 0.45:
            e = mutate(r, e)
        v, n = placeholders_for(e, r)
        out.append(cond(e, values=v, names=n, item=ITEM if r.random() < 0.7 else None))
    for _ in range(500):
        e = rand_update(r)
        if r.random() < 0.4:
            e = mutate(r, e)
        v, n = placeholders_for(e, r)
        out.append(upd(e, values=v, names=n, item=ITEM if r.random() < 0.7 else None))
    for _ in range(200):
        e = ", ".join(rand_path(r) for _ in range(r.randint(1, 4)))
        if r.random() < 0.3:
            e = mutate(r, e)
        _, n = placeholders_for(e, r)
        out.append(proj(e, names=n, item=ITEM, op=r.choice(["GetItem", "Scan"])))
    for _ in range(150):
        e = rand_condition(r)
        if r.random() < 0.3:
            e = mutate(r, e)
        v, n = placeholders_for(e, r)
        v = dict(v or {})
        v[":pk"] = {"S": "k"}
        out.append(query("pk = :pk", values=v, names=n, item=ITEM, FilterExpression=e))
    return out


def order_family():
    """Check order inside an update, the clause-by-clause evaluation, and the
    rules settled after the first recording."""
    UV = {":n": N1, ":s": S1, ":l": {"L": []}, ":ss": {"SS": ["a"]}}
    out = []
    for e in [
        "SET a = :w DELETE b :n ADD c :s", "DELETE b :n ADD c :s", "ADD c :s DELETE b :n",
        "SET a = :w ADD c :s", "ADD c :s SET a = :w", "REMOVE #q ADD c :s", "ADD c :s REMOVE #q",
        "ADD c :s SET a = :n + :s", "SET a = :n + :s DELETE b :n", "DELETE b :n ADD c :w",
        "SET a = :n + :s, b = :w", "SET b = :w, a = :n + :s", "SET a = #q, b = :n + :s",
        "SET a = :n + :s, b = #q", "SET #q = :n, a = :n, a = :n", "SET a = :n, a = :n, #q = :n",
        "SET a = :w, a = :n", "SET b = :w, a = list_append(:n, :l)", "ADD a :s, b :l", "ADD a :l, b :s",
        "REMOVE #q, a, a", "REMOVE a, a, #q", "SET b = :w, a = if_not_exists(:n, a)",
        "SET b = :n + :s, a = list_append(a)", "SET name = :n SET b = :n", "SET a = :n SET name = :n",
        "SET a" + ".b" * 32 + " = :n SET b = :n", "SET a = :n SET b" + ".b" * 32 + " = :n",
        "SET a = :n REMOVE b ADD c :n DELETE d :ss SET e = :n", "DELETE a :ss REMOVE b ADD c :n",
        "SET a = :n + (:n + :n)", "SET a = (:n + :n) - :n", "SET a = (:n) + :n", "SET a = (:n + :n)",
        "SET a = (list_append(:l, :l)) + :n", "SET a = (a) + (:n)", "SET a = :n + (:n)",
        "SET a = (:n + :n), b = :n", "SET a = :n" + " " * 4100,
    ]:
        out.append(upd(e, values=UV))
    for e, v in [
        ("REMOVE e.n[2] DELETE s :ss, l.x[5] :ns", {":ss": {"SS": ["p"]}, ":ns": {"NS": ["1"]}}),
        ("DELETE t :ns REMOVE zz ADD q[5].deep :ss", {":ss": {"SS": ["p"]}, ":ns": {"NS": ["1"]}}),
        ("SET bb[1] = e.x ADD q :ns DELETE a :ss", {":ss": {"SS": ["p"]}, ":ns": {"NS": ["1"]}}),
        ("ADD m :n SET b.deep = :n", {":n": N1}), ("SET b.deep = :n ADD m :n", {":n": N1}),
        ("REMOVE t.x SET a = s - zz", None), ("SET a = s - zz REMOVE t.x", None),
        ("SET z = if_not_exists(b, x.n)", None), ("SET z = if_not_exists(zz, b)", None),
        ("SET z = if_not_exists(zz, yy)", None), ("DELETE ns.x :ss", {":ss": {"SS": ["p"]}}),
        ("ADD m[1].deep :n", {":n": N1}), ("SET a = if_not_exists(a, :x) + :v", {":x": S1, ":v": N1}),
        ("SET a = :x + :y", {":x": {"N": "1"}, ":y": {"N": "1E-37"}}),
        ("SET a = :x - :y", {":x": {"N": "1E-130"}, ":y": {"N": "0.9E-130"}}),
    ]:
        for item in (ITEM, None):
            out.append(upd(e, values=v, item=item))
    for e in ["a" + " " * 4100, "a, b" + " " * 4100]:
        out.append(proj(e))
    out.append(req("Scan", FilterExpression="a = :v" + " " * 4100, ExpressionAttributeValues=V))
    P = {":p": {"S": "k"}, ":s": N1}
    for kc in [
        "pk = :p AND sk = :s AND x <> :s", "pk = :p AND sk = :s AND x.y = :s",
        "pk = :p AND (sk = :s AND x = :s)", "pk = :p AND sk = :s AND size(x) = :s",
        "x <> :s AND pk = :p AND sk = :s", "pk = :p AND sk = :s AND pk.x = :p", "pk = :p AND x.y = :s",
        "pk = :p AND sk = :s AND x = :s AND y = :s", "pk = :p" + " " * 4100,
    ]:
        out.append(query(kc, values=P))
    P1 = {":p": {"S": "k"}}
    for f in [
        "pk.x = :p", "pk[0] = :p", "pk.x = :p AND pk = :p", "pk = :p AND pk.x = :p",
        "attribute_exists(pk.x)", "attribute_exists(pk)", "a = :p AND pk.x = :p", "sk = :p AND pk.x = :p",
        "pk.x = :p AND sk.y = :p", "sk.y = :p AND pk.x = :p", "pk[0].a = :p", "sk = :p AND pk = :p",
        "sk.y = :p AND pk = :p", "pk = :p AND sk.y = :p", "sk.y = :p", "size(pk.x) = :p",
    ]:
        out.append(query("pk = :p", values=P1, FilterExpression=f))
    for op in ["CONTAINS", "NOT_CONTAINS", "LT", "IN", "BETWEEN"]:
        for v in [{"BOOL": True}, {"NULL": True}, {"L": []}, {"M": {}}, {"SS": ["a"]}]:
            args = [v, v] if op == "BETWEEN" else [v]
            out.append(req("DeleteItem", Expected={"zz": {"ComparisonOperator": op, "AttributeValueList": args}}))
            out.append(req("DeleteItem", item=ITEM, Expected={"t": {"ComparisonOperator": op, "AttributeValueList": args}}))
    for n in ["", " 1", "1e", "+1", ".5", "1.", "-0", "1e400", "1e-400", "1E+125", "1E-130",
              "1" * 39, "1" * 38, "0.00" + "1" * 38, "1" + "0" * 50, "x"]:
        out.append(cond("a = :v", values={":v": {"N": n}}))
    for v in [{"SS": ["a", "a"]}, {"NS": ["1", "1.0"]}, {"BS": ["AA==", "AA=="]}, {"NS": ["abc"]},
              {"NS": ["1e400"]}, {"BS": ["AQ==", "AQ=="]}]:
        out.append(upd("SET a = :v", values={":v": v}))
    out.append(req("GetItem", item=ITEM, ExpressionAttributeValues={":p": {"S": "k"}}))
    out.append(req("GetItem", item=ITEM, ExpressionAttributeValues={":p": {"S": "k"}}, ProjectionExpression="a"))
    out.append(cond("a.:ndeep = :v", values=V))
    # DynamoDB Local answers "Internal server error" when the `:word` after
    # the dot is also a defined value; Kythira reports the undefined name.
    out.append({**cond("a.:v = :v", values=V), "divergence": "DynamoDB Local internal error"})
    out.append(cond("a.:x = :v", values=V, names={"#x": "x"}))
    out.append(cond(":v.a = :v", values=V))
    out.append(proj("a.:x"))
    return out


# DynamoDB Local's number messages differ from AWS's, and Kythira gives AWS's
# ("The parameter cannot be converted to a numeric value: X", "Attempting to
# store more than 38 significant digits in a Number"): any case with a
# malformed or over-long number literal, or arithmetic past 38 digits.
NUMBER_TEXT = re.compile(r"^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?$")


def significant_digits(text):
    mantissa = re.split("[eE]", text.lstrip("+-"))[0].replace(".", "").lstrip("0").rstrip("0")
    return len(mantissa)


def numbers_in(v):
    if isinstance(v, dict):
        for k, x in v.items():
            if k == "N" and isinstance(x, str):
                yield x
            elif k == "NS" and isinstance(x, list):
                yield from (n for n in x if isinstance(n, str))
            else:
                yield from numbers_in(x)
    elif isinstance(v, list):
        for x in v:
            yield from numbers_in(x)


def mark_divergences(cases):
    for c in cases:
        nums = list(numbers_in(c["input"]))
        if any(not NUMBER_TEXT.match(n) or significant_digits(n) > 38 for n in nums):
            c["divergence"] = "number message: Kythira gives AWS's wording"
        elif c["input"].get("UpdateExpression") == "SET a = :x + :y" and all(
                n == "9" * 38 for n in nums):
            c["divergence"] = "number message: Kythira gives AWS's wording"
        elif c["input"].get("UpdateExpression") == "SET a = :x + :y" and nums == ["1", "1E-38"]:
            c["divergence"] = "number message: Kythira gives AWS's wording"


def all_cases():
    r = random.Random(SEED)
    cases = (syntax_family() + condition_family() + placeholder_family() + update_family() +
             projection_family() + key_family() + request_family() + eval_family() + order_family() +
             random_family(r))
    mark_divergences(cases)
    for i, c in enumerate(cases):
        c["id"] = i
    return cases


def check(corpus_path):
    """Fails when the recorded corpus is not this case list (re-record with
    record.sh after editing a case)."""
    with open(corpus_path, encoding="utf-8") as f:
        recorded = [json.loads(line) for line in f if line.strip()]
    for c in recorded:
        c.pop("expect", None)
    want = all_cases()
    if len(want) != len(recorded):
        raise SystemExit("corpus has %d cases, cases.py %d: re-record with record.sh"
                         % (len(recorded), len(want)))
    for w, r in zip(want, recorded):
        if w != r:
            raise SystemExit("case %d differs from the recording: re-record with record.sh" % w["id"])


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        check(sys.argv[2])
    else:
        for c in all_cases():
            sys.stdout.write(json.dumps(c, ensure_ascii=False) + "\n")
