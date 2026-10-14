#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Assert that a configured build tree is the configuration its CI job claims.

Every CI leg is defined by a few configure-time choices: a defconfig, a
compiler, a build type, sometimes a sanitizer or a future backend. A green
leg is only evidence about that configuration if those choices took effect,
and several ways of losing one are silent:

  * a -D option the project no longer declares (renamed or removed) is kept
    in CMakeCache.txt as UNINITIALIZED, ignored, and configure still succeeds
    (so every option a job asserts must be declared with option() or
    set(... CACHE ...));
  * a stale cache in a reused build directory keeps an older value;
  * a defconfig line asking for CONFIG_X=y whose `depends on` is unmet loads
    without a Kconfig warning and resolves to n, so strict mode never asks
    for X's package and every test behind X quietly stops being built.

This reads the build tree itself (CMakeCache.txt and the Kconfig output in
generated/autoconf.cmake) and fails on any difference from what the job
passes in, so the assertion lives in the workflow instead of in whoever last
checked by hand.

Usage:
  check-build-config.py --build-dir DIR [--defconfig FILE] [--compiler NAME]
                        [--expect NAME=VALUE ...]

  --defconfig FILE   the defconfig the job applied: KYTHIRA_KCONFIG must name
                     it, and every CONFIG_<X>=y/n (and "# CONFIG_<X> is not
                     set") in it must have resolved to that value.
  --compiler NAME    CMAKE_CXX_COMPILER must be NAME (compared by basename,
                     then by resolved path, so g++-13 matches /usr/bin/g++-13).
  --expect NAME=VAL  the cache entry NAME must exist, be declared by the
                     project (not UNINITIALIZED), and equal VAL. ON/OFF style
                     values compare by CMake truthiness. Repeatable.

Exits 0 if everything matches, 1 otherwise, 2 on bad usage.
"""

import argparse
import os
import re
import shutil
import sys
from pathlib import Path

TRUE_VALUES = {"1", "ON", "YES", "TRUE", "Y"}
FALSE_VALUES = {"0", "OFF", "NO", "FALSE", "N", ""}

failures = []


def fail(msg):
    failures.append(msg)
    print(f"::error::{msg}")


def read_cache(build_dir):
    cache = {}
    path = build_dir / "CMakeCache.txt"
    if not path.is_file():
        fail(f"{path} not found: the build directory was never configured")
        return None
    for line in path.read_text(errors="replace").splitlines():
        if not line or line.startswith(("#", "//")):
            continue
        m = re.match(r"^([^:=]+):([A-Z]+)=(.*)$", line)
        if m:
            cache[m.group(1)] = (m.group(2), m.group(3))
    return cache


def same_bool_or_text(actual, expected):
    a, e = actual.strip().upper(), expected.strip().upper()
    if e in TRUE_VALUES | FALSE_VALUES and a in TRUE_VALUES | FALSE_VALUES:
        return (a in TRUE_VALUES) == (e in TRUE_VALUES)
    return actual.strip() == expected.strip()


def same_path(actual, expected):
    if actual == expected:
        return True
    try:
        return Path(actual).resolve(strict=True) == Path(expected).resolve(strict=True)
    except OSError:
        return False


def check_expect(cache, spec):
    if "=" not in spec:
        print(f"--expect wants NAME=VALUE, got: {spec}", file=sys.stderr)
        sys.exit(2)
    name, expected = spec.split("=", 1)
    if name not in cache:
        fail(f"{name} is not in CMakeCache.txt; this job claims {name}={expected}")
        return
    typ, actual = cache[name]
    if typ == "UNINITIALIZED":
        fail(f"{name} was passed on the command line but the project never declared it as a "
             f"cache option (UNINITIALIZED in CMakeCache.txt), which is exactly how a renamed "
             f"or misspelled option looks; declare it with option() or set(... CACHE ...)")
        return
    if not same_bool_or_text(actual, expected):
        fail(f"this job claims {name}={expected}, but CMakeCache.txt says '{actual}'")
        return
    print(f"confirmed: {name}={actual}")


def check_compiler(cache, name):
    entry = cache.get("CMAKE_CXX_COMPILER")
    if entry is None:
        fail("CMAKE_CXX_COMPILER is not in CMakeCache.txt")
        return
    actual = entry[1]
    if os.path.basename(actual) == name:
        print(f"confirmed: CMAKE_CXX_COMPILER={actual}")
        return
    wanted = shutil.which(name)
    if wanted and same_path(actual, wanted):
        print(f"confirmed: CMAKE_CXX_COMPILER={actual} (resolves to {name})")
        return
    fail(f"this job claims compiler {name}, but CMakeCache.txt says CMAKE_CXX_COMPILER={actual}")


def parse_defconfig(path):
    wanted = {}
    for line in path.read_text().splitlines():
        m = re.match(r"^CONFIG_([A-Za-z0-9_]+)=(.*)$", line)
        if m:
            value = m.group(2).strip()
            if value in ("y", "m"):
                wanted[m.group(1)] = True
            elif value == "n":
                wanted[m.group(1)] = False
            # Non-bool symbols (strings, ints) have no KCONFIG_ variable.
            continue
        m = re.match(r"^# CONFIG_([A-Za-z0-9_]+) is not set", line)
        if m:
            wanted[m.group(1)] = False
    return wanted


def check_defconfig(build_dir, cache, defconfig):
    if not defconfig.is_file():
        print(f"--defconfig file not found: {defconfig}", file=sys.stderr)
        sys.exit(2)

    entry = cache.get("KYTHIRA_KCONFIG")
    if entry is None or not entry[1]:
        fail(f"this job claims {defconfig}, but KYTHIRA_KCONFIG is unset in CMakeCache.txt")
        return
    if not same_path(entry[1], str(defconfig)):
        fail(f"this job claims {defconfig}, but CMakeCache.txt says KYTHIRA_KCONFIG={entry[1]}")
        return
    print(f"confirmed: KYTHIRA_KCONFIG={entry[1]}")

    autoconf = build_dir / "generated" / "autoconf.cmake"
    resolved = {}
    if autoconf.is_file():
        for line in autoconf.read_text().splitlines():
            m = re.match(r"^set\(KCONFIG_([A-Za-z0-9_]+) (ON|OFF)\)$", line)
            if m:
                resolved[m.group(1)] = m.group(2) == "ON"
    if not resolved:
        fail(f"{autoconf} defines no KCONFIG_ variables: Kconfig did not resolve "
             f"{defconfig} at all")
        return

    wanted = parse_defconfig(defconfig)
    bad = 0
    for sym, want in sorted(wanted.items()):
        if sym not in resolved:
            fail(f"{defconfig} sets CONFIG_{sym}, which Kconfig does not define")
            bad += 1
        elif resolved[sym] != want:
            fail(f"{defconfig} asks for CONFIG_{sym}={'y' if want else 'n'}, but it resolved to "
                 f"{'y' if resolved[sym] else 'n'}; check its `depends on` / `select` in Kconfig")
            bad += 1
    if not bad:
        print(f"confirmed: all {len(wanted)} CONFIG_ settings in {defconfig} took effect")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build-dir", required=True, type=Path)
    ap.add_argument("--defconfig", type=Path)
    ap.add_argument("--compiler")
    ap.add_argument("--expect", action="append", default=[])
    args = ap.parse_args()

    cache = read_cache(args.build_dir)
    if cache is not None:
        if args.defconfig:
            check_defconfig(args.build_dir, cache, args.defconfig)
        if args.compiler:
            check_compiler(cache, args.compiler)
        for spec in args.expect:
            check_expect(cache, spec)

    if failures:
        print(f"[check-build-config] FAILED: {len(failures)} claim(s) about {args.build_dir} "
              f"did not hold; this job would have tested a different configuration than its "
              f"name says", file=sys.stderr)
        return 1
    print(f"[check-build-config] OK: {args.build_dir} is configured as claimed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
