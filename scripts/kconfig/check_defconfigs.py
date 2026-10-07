#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Load every configs/*_defconfig against Kconfig, fail on any parse warning
or on any assignment that does not take effect.

Catches drift between Kconfig edits and stale defconfigs (Requirement 5.4).
Exposed as the `kconfig-check` CMake target, and run directly (no configure
needed) by the `kconfig-check` job in .github/workflows/ci.yml.
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
        # Off by default in Kconfiglib, which silently drops an assignment to a
        # symbol Kconfig no longer defines. That stale-symbol case is the one
        # this check exists for, so turn it on, plus the two duplicate-
        # assignment warnings (a later line silently winning over an earlier).
        kconf.warn_assign_undef = True
        kconf.warn_assign_override = True
        kconf.warn_assign_redun = True
        kconf.load_config(path)
        # Kconfiglib does not warn when an assignment cannot take effect: a
        # CONFIG_X=y whose `depends on` is unmet loads silently and resolves
        # to n, and strict mode then never asks for X's package, so every test
        # behind X drops out of that job without a failure anywhere. Compare
        # each bool/tristate the file assigns with what it resolved to.
        problems = list(kconf.warnings)
        for sym in kconf.unique_defined_syms:
            if (sym.type in (kconfiglib.BOOL, kconfiglib.TRISTATE)
                    and sym.user_value is not None
                    and sym.user_value != sym.tri_value):
                problems.append(
                    f"CONFIG_{sym.name} is assigned "
                    f"{kconfiglib.TRI_TO_STR[sym.user_value]} but resolves to "
                    f"{kconfiglib.TRI_TO_STR[sym.tri_value]} (unmet `depends on`, "
                    f"or forced by a `select`)")
        if problems:
            failed = True
            print(f"kconfig-check: {path}:", file=sys.stderr)
            for problem in problems:
                print(f"  {problem}", file=sys.stderr)
        else:
            print(f"kconfig-check: {path}: OK")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
