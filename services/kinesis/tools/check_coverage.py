#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Fail unless every operation of the pinned Kinesis model is accounted for.

.kiro/specs/kinesis-compatible-streams/ Requirement 2: every operation in
services/kinesis/model/service-2.json is either implemented (listed in
services/kinesis/implemented-operations.txt) or listed in
services/kinesis/doc/unsupported.md with the reason and the exact error it
returns. A model upgrade that adds an operation fails this check until the
operation is placed in one of the two.

    check_coverage.py MODEL IMPLEMENTED UNSUPPORTED_MD

An operation in both lists, a listed name the model does not have, and a
row of unsupported.md without a reason or an error are errors too.
"""

import json
import pathlib
import re
import sys

ROW = re.compile(r"^\|\s*`(?P<op>[A-Za-z]+)`\s*\|(?P<rest>.*)\|\s*$")


def implemented_ops(path: pathlib.Path) -> list[str]:
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.append(line)
    return out


def unsupported_ops(path: pathlib.Path) -> tuple[list[str], list[str]]:
    ops, problems = [], []
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        m = ROW.match(line)
        if not m:
            continue
        cells = [c.strip() for c in m.group("rest").split("|")]
        if len(cells) != 3 or not all(cells):
            problems.append(f"{path}:{n}: `{m.group('op')}` needs a planned task, "
                            "a reason and the error it returns")
        ops.append(m.group("op"))
    return ops, problems


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    model_path, impl_path, unsup_path = map(pathlib.Path, sys.argv[1:])
    model = set(json.loads(model_path.read_text(encoding="utf-8"))["operations"])
    impl = implemented_ops(impl_path)
    unsup, problems = unsupported_ops(unsup_path)

    for name, listed in (("implemented", impl), ("unsupported", unsup)):
        seen = set()
        for op in listed:
            if op in seen:
                problems.append(f"{op} is listed twice as {name}")
            seen.add(op)
            if op not in model:
                problems.append(f"{op} is listed as {name} but is not in the pinned model")
    for op in sorted(set(impl) & set(unsup)):
        problems.append(f"{op} is listed both as implemented and as unsupported")
    for op in sorted(model - set(impl) - set(unsup)):
        problems.append(f"{op} is in the pinned model but neither implemented nor listed in "
                        f"{unsup_path}")

    for p in problems:
        print(f"error: {p}", file=sys.stderr)
    if problems:
        return 1
    print(f"{len(model)} operations: {len(impl)} implemented, {len(unsup)} listed unsupported")
    return 0


if __name__ == "__main__":
    sys.exit(main())
