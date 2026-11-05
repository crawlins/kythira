#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Fixture tests for scripts/check_module_boundaries.py.

module_boundaries/tree/ is a miniature source tree with two modules:
services/clean/ uses every allowed kind of include and CMake reference, and
services/bad/ breaks every rule at least once. Each expected violation is
marked in the fixture itself, so the expectations cannot drift from the files:

    // rule <name>        (or `# rule <name>`) on a line of its own expects a
                          violation of <name> on the next line;
    <code>  # rule <name> expects it on that same line.

Run: python3 scripts/tests/test_check_module_boundaries.py
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "check_module_boundaries.py"
TREE = HERE / "module_boundaries" / "tree"

sys.path.insert(0, str(SCRIPT.parent))
import check_module_boundaries as cmb  # noqa: E402

RULES = {"other-module", "impl-header", "core-tests", "core-cmd",
         "core-includes-module", "cmake-source-dir", "cmake-escape",
         "core-cmake-target", "allow-entry"}
MARK_RE = re.compile(r'(?://|#)\s*rule\s+([a-z-]+)')


def expected_from_markers(root: Path) -> set[tuple[str, int, str]]:
    out = set()
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            path = Path(dirpath) / name
            rel = path.relative_to(root).as_posix()
            for n, line in enumerate(path.read_text().splitlines(), 1):
                m = MARK_RE.search(line)
                if not m:
                    continue
                own_line = line.strip().startswith(("//", "#"))
                out.add((rel, n + 1 if own_line else n, m.group(1)))
    return out


def run(root: Path) -> list[cmb.Violation]:
    return cmb.Checker(root).run()


def run_script(root: Path, gha: bool = False) -> subprocess.CompletedProcess:
    """Run the checker as CI does. GITHUB_ACTIONS is set explicitly either
    way, so the output format does not depend on where the tests run."""
    env = {k: v for k, v in os.environ.items() if k != "GITHUB_ACTIONS"}
    if gha:
        env["GITHUB_ACTIONS"] = "true"
    return subprocess.run([sys.executable, str(SCRIPT), "--root", str(root)],
                          capture_output=True, text=True, check=False, env=env)


class FixtureTree(unittest.TestCase):
    def test_reports_exactly_the_marked_violations(self):
        got = {(v.file, v.line, v.rule) for v in run(TREE)}
        want = expected_from_markers(TREE)
        self.assertEqual(sorted(want - got), [], "missed violations")
        self.assertEqual(sorted(got - want), [], "unexpected violations")

    def test_fixture_covers_every_rule(self):
        rules = {r for _f, _l, r in expected_from_markers(TREE)}
        self.assertEqual(rules, RULES)

    def test_clean_module_has_no_violation(self):
        bad = [v for v in run(TREE) if v.file.startswith("services/clean/")]
        self.assertEqual(bad, [])

    def test_each_violation_names_file_line_and_rule(self):
        proc = run_script(TREE)
        self.assertEqual(proc.returncode, 1)
        lines = [ln for ln in proc.stdout.splitlines()
                 if not ln.startswith("check_module_boundaries:")]
        self.assertTrue(lines)
        for ln in lines:
            self.assertRegex(ln, r'^[^:]+:\d+: \[[a-z-]+\] \S')


class Trees(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def copy(self, *drop: str) -> Path:
        root = self.tmp / "tree"
        shutil.copytree(TREE, root)
        for rel in drop:
            shutil.rmtree(root / rel)
        return root

    def test_no_services_directory_is_a_no_op(self):
        root = self.copy("services")
        proc = run_script(root)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("nothing to check", proc.stdout)

    def test_clean_module_alone_passes(self):
        root = self.copy("services/bad")
        # The core files that include services/bad/ now resolve to nothing.
        # src/core.cpp still reaches services/clean/, so drop that one.
        (root / "src" / "core.cpp").unlink()
        proc = run_script(root)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("1 module(s) clean: clean", proc.stdout)

    def test_github_annotations(self):
        proc = run_script(TREE, gha=True)
        self.assertIn("::error file=services/bad/include/kythira/bad/bad.hpp,"
                      "line=6::[other-module]", proc.stdout)


if __name__ == "__main__":
    unittest.main()
