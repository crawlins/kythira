#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Fail if a configuration silently drops tests a reference configuration has.

.kiro/specs/ci-build-matrix-coverage/ Requirement 2.5: the no-folly job must
publish which tests CONFIG_FOLLY=n disables, and fail when a test registered
under ci_full_defconfig disappears without a checked-in reason.

Usage:
    scripts/check-disabled-tests.py <reference-build-dir> <build-dir> <allowlist>

Both build directories only need to be configured; the test lists come from
`ctest --show-only=json-v1`, which reads CTestTestfile.cmake and compiles
nothing.

The allowlist holds one `<test-name> <reason>` per line; blank lines and
lines starting with `#` are ignored. The check fails when:

  * a test is registered in the reference tree, missing from the build tree
    and not allowlisted;
  * an allowlist entry has no reason;
  * an allowlist entry is stale: the test is registered in the build tree
    after all, or the reference tree no longer registers it. A stale entry
    would otherwise hide the next regression under the same name.

The disabled-test table is printed, and appended to $GITHUB_STEP_SUMMARY
when that is set.
"""

import json
import os
import subprocess
import sys


def registered_tests(build_dir):
    out = subprocess.run(
        ["ctest", "--test-dir", build_dir, "--show-only=json-v1"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return {test["name"] for test in json.loads(out).get("tests", [])}


def read_allowlist(path):
    entries = {}
    errors = []
    with open(path, encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            name, _, reason = line.partition(" ")
            reason = reason.strip()
            if not reason:
                errors.append(f"{path}:{lineno}: {name}: entry has no reason")
            if name in entries:
                errors.append(f"{path}:{lineno}: {name}: listed twice")
            entries[name] = reason
    return entries, errors


def main(argv):
    if len(argv) != 4:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        print(f"usage: {argv[0]} <reference-build-dir> <build-dir> <allowlist>",
              file=sys.stderr)
        return 2
    reference_dir, build_dir, allowlist_path = argv[1:]

    reference = registered_tests(reference_dir)
    built = registered_tests(build_dir)
    if not reference:
        print(f"error: {reference_dir} registers no tests", file=sys.stderr)
        return 1
    if not built:
        print(f"error: {build_dir} registers no tests", file=sys.stderr)
        return 1

    allowlist, errors = read_allowlist(allowlist_path)
    disabled = sorted(reference - built)

    for name in disabled:
        if name not in allowlist:
            errors.append(f"{name}: registered in {reference_dir} but not in "
                          f"{build_dir}, and not in {allowlist_path}")
    for name in sorted(allowlist):
        if name in built:
            errors.append(f"{allowlist_path}: {name}: stale, the test is "
                          f"registered in {build_dir}")
        elif name not in reference:
            errors.append(f"{allowlist_path}: {name}: stale, {reference_dir} "
                          f"no longer registers it")

    lines = [
        "### Tests disabled relative to the reference configuration",
        "",
        f"{len(built)} registered, {len(reference)} in the reference tree, "
        f"{len(disabled)} disabled.",
        "",
        "| Test | Reason |",
        "|---|---|",
    ]
    for name in disabled:
        lines.append(f"| `{name}` | {allowlist.get(name, '**not allowlisted**')} |")
    report = "\n".join(lines) + "\n"
    print(report)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(report)

    for error in errors:
        print(f"error: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
