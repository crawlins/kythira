#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Summarize recorded wire evidence into tables.

Reads the `<client>.jsonl` files the recording stub wrote and writes:

- `summary.json`: per client, the operations it called and, per operation,
  the set of request member paths it sent (list elements as `[]`, map keys
  as `{}` for maps whose keys are data: items, keys, attribute names and
  values); plus the headers and SigV4 signed-header lists seen. Sets, not
  counts, so a re-recording that differs only in timing compares equal.
- `README.md`'s generated section: the same as tables for people.

Usage:
    summarize.py EVIDENCE_DIR            write summary.json and README.md
    summarize.py EVIDENCE_DIR --diff OTHER_DIR
                                         print how OTHER_DIR's recording
                                         differs (exit 0 either way)
"""

import argparse
import json
import pathlib
import re
import sys

# Members whose object keys are data (attribute names), not shape members.
DATA_MAPS = {
    "Item",
    "Key",
    "Keys",
    "ExclusiveStartKey",
    "LastEvaluatedKey",
    "ExpressionAttributeNames",
    "ExpressionAttributeValues",
    "Expected",
    "AttributeUpdates",
    "KeyConditions",
    "QueryFilter",
    "ScanFilter",
    "RequestItems",
    "M",
    "NewImage",
    "OldImage",
}

BEGIN = "<!-- BEGIN GENERATED (summarize.py) -->"
END = "<!-- END GENERATED -->"


def paths(value, prefix=""):
    out = set()
    if isinstance(value, dict):
        data = prefix.rsplit(".", 1)[-1] in DATA_MAPS
        for k, v in value.items():
            p = (prefix + ".{}") if data else (prefix + "." + k if prefix else k)
            out.add(p)
            # An attribute value's type tag (S, N, M, L, ...) is a shape
            # member again below a data key.
            out |= paths(v, p)
    elif isinstance(value, list):
        for v in value:
            out.add(prefix + "[]")
            out |= paths(v, prefix + "[]")
    return out


def load(directory):
    clients = {}
    for f in sorted(pathlib.Path(directory).glob("*.jsonl")):
        c = clients.setdefault(
            f.stem, {"operations": {}, "headers": set(), "signed_headers": set(), "user_agents": set()}
        )
        for line in f.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            r = json.loads(line)
            op = "%s.%s" % (r["service"], r["operation"])
            c["operations"].setdefault(op, set()).update(paths(r["body"]))
            c["headers"].update(r["header_names"])
            auth = r["headers"].get("authorization", "")
            m = re.search(r"SignedHeaders=([^,]+)", auth)
            if m:
                c["signed_headers"].add(m.group(1))
            ua = r["headers"].get("user-agent", "")
            if ua:
                c["user_agents"].add(ua.split(" ")[0])
    return clients


def to_json(clients):
    return {
        name: {
            "operations": {op: sorted(p) for op, p in sorted(c["operations"].items())},
            "headers": sorted(c["headers"]),
            "signed_headers": sorted(c["signed_headers"]),
            "user_agents": sorted(c["user_agents"]),
        }
        for name, c in sorted(clients.items())
    }


def short(op):
    return op.replace("DynamoDB_20120810.", "").replace(
        "DynamoDBStreams_20120810.", "streams:"
    ).replace("Kinesis_20131202.", "kinesis:")


def markdown(summary):
    names = sorted(summary)
    ops = sorted({op for c in summary.values() for op in c["operations"]})
    lines = [BEGIN, "", "### Operations by client", ""]
    lines.append("| Operation | " + " | ".join(names) + " |")
    lines.append("|---|" + "---|" * len(names))
    for op in ops:
        row = ["x" if op in summary[n]["operations"] else "" for n in names]
        lines.append("| `%s` | %s |" % (short(op), " | ".join(row)))
    lines += ["", "### SDKs", "", "| Client | User agent | Headers sent |", "|---|---|---|"]
    for n in names:
        lines.append(
            "| %s | %s | %s |"
            % (
                n,
                ", ".join("`%s`" % u for u in summary[n]["user_agents"]),
                ", ".join("`%s`" % h for h in summary[n]["headers"]),
            )
        )
    lines += ["", "### Request members by operation", ""]
    lines.append(
        "Union over all clients; `[]` is a list element, `{}` a key that is data "
        "(an attribute name or a table name)."
    )
    lines.append("")
    for op in ops:
        members = sorted({p for c in summary.values() for p in c["operations"].get(op, [])})
        top = sorted({p for p in members if "." not in p and "[" not in p})
        lines.append("- `%s`: %s" % (short(op), ", ".join("`%s`" % m for m in top) or "(empty body)"))
    lines += ["", END]
    return "\n".join(lines)


def diff(a, b):
    out = []
    for name in sorted(set(a) | set(b)):
        if name not in a or name not in b:
            out.append("client %s only in %s" % (name, "recorded" if name in b else "checked-in"))
            continue
        oa, ob = a[name]["operations"], b[name]["operations"]
        for op in sorted(set(oa) | set(ob)):
            if op not in oa:
                out.append("%s: new operation %s" % (name, op))
            elif op not in ob:
                out.append("%s: no longer calls %s" % (name, op))
            else:
                for p in sorted(set(ob[op]) - set(oa[op])):
                    out.append("%s: %s now sends %s" % (name, op, p))
                for p in sorted(set(oa[op]) - set(ob[op])):
                    out.append("%s: %s no longer sends %s" % (name, op, p))
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("evidence_dir")
    ap.add_argument("--diff", metavar="OTHER_DIR")
    args = ap.parse_args(argv)
    base = pathlib.Path(args.evidence_dir)
    if args.diff:
        a = json.loads((base / "summary.json").read_text(encoding="utf-8"))
        b = to_json(load(args.diff))
        changes = diff(a, b)
        print("\n".join(changes) if changes else "no difference in operations or members")
        return 0
    summary = to_json(load(base))
    (base / "summary.json").write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    readme = base / "README.md"
    text = readme.read_text(encoding="utf-8") if readme.exists() else BEGIN + "\n" + END + "\n"
    start, end = text.index(BEGIN), text.index(END) + len(END)
    readme.write_text(text[:start] + markdown(summary) + text[end:], encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
