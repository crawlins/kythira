#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
"""Render the external comparison register into the comparison document.

Requirement 11.4 wants the register of Requirement 9 reproduced *in full*
inside `doc/multi_raft_performance_comparison.md`, and Requirement 9.1 wants
one record per external number carrying every field of Appendix A. Those two
together mean the same content exists in a data file and in a document, which
is a drift hazard: a register edited without the document being regenerated
publishes a table that no longer matches its own source.

So the document holds a generated block between two markers and this script
owns everything between them. `--check` fails when the block is stale, which
is what CI runs; running without it rewrites the block in place.

The validation is not incidental to the rendering — it is the point of doing
this in a program at all:

  * every record must carry every field of Appendix A, present and non-empty;
  * a field the source does not state must say so in those words, because
    Requirement 9.2 forbids inference and an empty string is indistinguishable
    from an oversight;
  * `kind` must be `library` or `database`, because Requirement 9.5 turns that
    classification into a comparison-class consequence;
  * and the register must cover the four implementations Requirement 9.4 names
    at minimum, so that dropping one is a failure rather than a shorter table.

Usage:
    scripts/render-external-register.py [--check]
"""

import argparse
import json
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
REGISTER = REPO / "doc" / "data" / "multi_raft_external_comparison_register.json"
DOCUMENT = REPO / "doc" / "multi_raft_performance_comparison.md"

BEGIN = "<!-- BEGIN GENERATED REGISTER: scripts/render-external-register.py -->"
END = "<!-- END GENERATED REGISTER -->"

# Requirement 9.4's floor. Matched case-insensitively against the
# implementation field so that "etcd 3.2.0" and "Dragonboat v3" both count,
# and so that renaming a record cannot silently drop a required system.
REQUIRED = ("tikv", "dragonboat", "braft", "etcd")

NOT_STATED = "not stated"

# Appendix A's field list, in Appendix A's order. The order is part of the
# contract: a reader comparing two records reads down the same rows.
LABELS = {
    "implementation_version": "Implementation, version",
    "kind": "Kind",
    "source_url": "Source URL or document",
    "date_retrieved": "Date retrieved",
    "author_of_number": "Author of the number",
    "hardware": "Hardware",
    "cluster_size_replication_factor": "Cluster size / replication factor",
    "raft_group_count": "Raft group count",
    "payload_size": "Payload size",
    "read_write_mix": "Read/write mix",
    "client_concurrency": "Client concurrency",
    "durability": "Durability",
    "batching_configuration": "Batching configuration",
    "metric_and_unit_as_stated": "Metric and unit as stated",
    "comparison_class": "Comparison class",
}


def load():
    with REGISTER.open() as fh:
        return json.load(fh)


def validate(data) -> list:
    problems = []
    order = data["field_order"]
    if list(LABELS) != order:
        problems.append(
            "field_order in the register does not match this script's Appendix A "
            f"field list.\n  register: {order}\n  script:   {list(LABELS)}"
        )
    seen_ids = set()
    for record in data["records"]:
        rid = record.get("id", "<no id>")
        if rid in seen_ids:
            problems.append(f"{rid}: duplicate record id")
        seen_ids.add(rid)
        for field in order:
            value = record.get(field)
            if value is None:
                problems.append(f"{rid}: missing Appendix A field '{field}'")
            elif not str(value).strip():
                # An empty field and an unstated one look identical in a
                # rendered table and mean opposite things. Requirement 9.2
                # asks for the words.
                problems.append(
                    f"{rid}: field '{field}' is empty. A field the source does not "
                    f"state must say '{NOT_STATED}' in those words, never be blank"
                )
        kind = record.get("kind")
        if kind not in ("library", "database"):
            problems.append(
                f"{rid}: kind is {kind!r}; Requirement 9.1 admits only "
                "'library' or 'database'"
            )
    joined = " ".join(r.get("implementation_version", "") for r in data["records"]).lower()
    for required in REQUIRED:
        if required not in joined:
            problems.append(
                f"Requirement 9.4 names {required} as a minimum member of the "
                "comparison set and no record mentions it"
            )
    return problems


def render(data) -> str:
    lines = [BEGIN, ""]
    lines.append(
        f"*{len(data['records'])} records. Generated from "
        "`doc/data/multi_raft_external_comparison_register.json` by "
        "`scripts/render-external-register.py`; edit the JSON, not this block.*"
    )
    lines.append("")
    for record in data["records"]:
        lines.append(f"#### `{record['id']}`")
        lines.append("")
        lines.append("| Field | Value |")
        lines.append("|---|---|")
        for field in data["field_order"]:
            value = str(record[field])
            # A markdown table cell is one line, and a pipe inside one ends
            # the cell. Both are silent corruptions of the table rather than
            # errors, so both are escaped here rather than trusted to the
            # register's authors.
            value = value.replace("|", "\\|").replace("\n", " ")
            lines.append(f"| {LABELS[field]} | {value} |")
        lines.append("")
    lines.append(END)
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Fail if the document's generated block is stale, instead of rewriting it",
    )
    args = parser.parse_args()

    data = load()
    problems = validate(data)
    if problems:
        print("The external comparison register is invalid:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1

    document = DOCUMENT.read_text()
    if BEGIN not in document or END not in document:
        print(
            f"{DOCUMENT} is missing the generated-register markers. Expected both:\n"
            f"  {BEGIN}\n  {END}",
            file=sys.stderr,
        )
        return 1

    head, _, rest = document.partition(BEGIN)
    _, _, tail = rest.partition(END)
    updated = head + render(data) + tail

    if args.check:
        if updated != document:
            print(
                f"{DOCUMENT.relative_to(REPO)}'s generated register block is stale.\n"
                "Run scripts/render-external-register.py and commit the result.",
                file=sys.stderr,
            )
            return 1
        print(f"Register block is up to date ({len(data['records'])} records).")
        return 0

    DOCUMENT.write_text(updated)
    print(f"Rendered {len(data['records'])} records into {DOCUMENT.relative_to(REPO)}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
