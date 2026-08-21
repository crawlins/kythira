#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Load every configs/*_defconfig against Kconfig, fail on any parse warning.

Catches drift between Kconfig edits and stale defconfigs (Requirement 5.4).
Wired into CI as the `kconfig-check` CMake target, not into the normal build.
"""
import glob
import os
import sys

import kconfiglib


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <Kconfig> <configs-dir>", file=sys.stderr)
        return 2
    kconfig_file, configs_dir = sys.argv[1], sys.argv[2]

    defconfigs = sorted(glob.glob(os.path.join(configs_dir, "*_defconfig")))
    if not defconfigs:
        print(f"kconfig-check: no *_defconfig files found under {configs_dir}", file=sys.stderr)
        return 1

    failed = False
    for path in defconfigs:
        kconf = kconfiglib.Kconfig(kconfig_file)
        kconf.load_config(path)
        if kconf.warnings:
            failed = True
            print(f"kconfig-check: {path}:", file=sys.stderr)
            for warning in kconf.warnings:
                print(f"  {warning}", file=sys.stderr)
        else:
            print(f"kconfig-check: {path}: OK")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
