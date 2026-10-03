#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Fail if a configs/*_defconfig is neither applied by a workflow nor allowlisted.

A defconfig nothing builds is a claim nothing checks (ci-build-matrix-coverage
Requirement 4.3). A defconfig counts as applied when a non-comment line of some
.github/workflows/*.yml names it as `configs/<name>`; a mention inside a YAML or
shell comment does not count, since the comments in ci.yml describe defconfigs
other jobs apply.

configs/defconfig-usage-allowlist.txt holds the exceptions, one per line:

    <name>_defconfig  # <reason>

The reason is required. An allowlist entry for a defconfig that no longer exists,
or that a workflow now applies, is also an error, so the list cannot outlive the
gap it records.

Run from the repository root, or pass the root as the only argument.
"""
import glob
import os
import re
import sys

ALLOWLIST = os.path.join("configs", "defconfig-usage-allowlist.txt")


def load_allowlist(path: str) -> tuple[dict[str, str], list[str]]:
    entries: dict[str, str] = {}
    errors: list[str] = []
    if not os.path.exists(path):
        return entries, errors
    with open(path, encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            name, sep, reason = line.partition("#")
            name, reason = name.strip(), reason.strip()
            if not sep or not reason:
                errors.append(f"{path}:{lineno}: '{name}' has no '# reason'")
            elif name in entries:
                errors.append(f"{path}:{lineno}: '{name}' is listed twice")
            entries[name] = reason
    return entries, errors


def applied_defconfigs(workflow_paths: list[str]) -> dict[str, list[str]]:
    pattern = re.compile(r"configs/([A-Za-z0-9_.-]+_defconfig)\b")
    applied: dict[str, list[str]] = {}
    for path in workflow_paths:
        with open(path, encoding="utf-8") as fh:
            for lineno, line in enumerate(fh, 1):
                if line.lstrip().startswith("#"):
                    continue
                for name in pattern.findall(line):
                    applied.setdefault(name, []).append(f"{path}:{lineno}")
    return applied


def main() -> int:
    if len(sys.argv) > 2:
        print(f"usage: {sys.argv[0]} [repo-root]", file=sys.stderr)
        return 2
    if len(sys.argv) == 2:
        os.chdir(sys.argv[1])

    defconfigs = sorted(os.path.basename(p) for p in glob.glob("configs/*_defconfig"))
    if not defconfigs:
        print("defconfig-usage: no configs/*_defconfig found; run from the repo root",
              file=sys.stderr)
        return 1
    workflows = sorted(glob.glob(".github/workflows/*.yml")
                       + glob.glob(".github/workflows/*.yaml"))
    applied = applied_defconfigs(workflows)
    allowlist, errors = load_allowlist(ALLOWLIST)

    for name in defconfigs:
        if name in applied:
            where = applied[name][0]
            more = f" (+{len(applied[name]) - 1} more)" if len(applied[name]) > 1 else ""
            print(f"defconfig-usage: {name}: applied at {where}{more}")
            if name in allowlist:
                errors.append(f"{ALLOWLIST}: '{name}' is applied by a workflow now; "
                              f"remove its allowlist entry")
        elif name in allowlist:
            print(f"defconfig-usage: {name}: allowlisted ({allowlist[name]})")
        else:
            errors.append(f"configs/{name} is applied by no workflow. Apply it in a "
                          f"CI job, or add it to {ALLOWLIST} with a '# reason'")

    for name in sorted(set(allowlist) - set(defconfigs)):
        errors.append(f"{ALLOWLIST}: '{name}' does not exist under configs/; "
                      f"remove its allowlist entry")

    for error in errors:
        print(f"::error::defconfig-usage: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
