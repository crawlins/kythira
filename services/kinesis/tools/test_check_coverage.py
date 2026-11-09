#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Fixture tests for check_coverage.py, so a broken checker cannot pass the
module by reporting nothing (ctest kinesis_model_coverage_checker)."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

CHECKER = pathlib.Path(__file__).with_name("check_coverage.py")
HEAD = "| Operation | Planned | Reason | Error |\n|---|---|---|---|\n"


class CheckCoverageTest(unittest.TestCase):
    def run_checker(self, ops, implemented, rows):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            (d / "model.json").write_text(json.dumps({"operations": {o: {} for o in ops}}))
            (d / "impl.txt").write_text("# comment\n" + "".join(f"{o}\n" for o in implemented))
            (d / "unsup.md").write_text(HEAD + "".join(rows))
            return subprocess.run(
                [sys.executable, str(CHECKER), str(d / "model.json"), str(d / "impl.txt"),
                 str(d / "unsup.md")], capture_output=True, text=True, check=False)

    @staticmethod
    def row(op, reason="r", error="e"):
        return f"| `{op}` | task 1 | {reason} | {error} |\n"

    def test_every_operation_placed_passes(self):
        r = self.run_checker(["A", "B"], ["A"], [self.row("B")])
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_an_unplaced_operation_fails(self):
        r = self.run_checker(["A", "B", "C"], ["A"], [self.row("B")])
        self.assertEqual(r.returncode, 1)
        self.assertIn("C is in the pinned model", r.stderr)

    def test_an_operation_in_both_lists_fails(self):
        r = self.run_checker(["A"], ["A"], [self.row("A")])
        self.assertEqual(r.returncode, 1)
        self.assertIn("both", r.stderr)

    def test_a_name_the_model_lacks_fails(self):
        r = self.run_checker(["A"], ["A", "Z"], [])
        self.assertEqual(r.returncode, 1)
        self.assertIn("Z is listed as implemented", r.stderr)

    def test_a_row_without_its_error_fails(self):
        r = self.run_checker(["A"], [], [self.row("A", error="")])
        self.assertEqual(r.returncode, 1)
        self.assertIn("needs", r.stderr)


if __name__ == "__main__":
    unittest.main()
