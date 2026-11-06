#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Tests for scripts/tidy-shard.py (clang-tidy-result-cache task 2).

Run: python3 scripts/tests/test_tidy_shard.py
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "tidy-shard.py"
spec = importlib.util.spec_from_file_location("tidy_shard", SCRIPT)
ts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ts)


class PlanTest(unittest.TestCase):
    def test_longest_first_to_least_loaded(self):
        costs = {"a": 10, "b": 7, "c": 6, "d": 5, "e": 4}
        shards, totals = ts.plan(list(costs), costs, 2)
        # a(10)->0, b(7)->1, c(6)->1 (7<10), d(5)->0 (10<13), e(4)->1 (13<15)
        self.assertEqual(shards, [["a", "d"], ["b", "c", "e"]])
        self.assertEqual(totals, [15, 17])

    def test_equal_totals_tie_to_lowest_index(self):
        costs = {"a": 4, "b": 4, "c": 1}
        shards, totals = ts.plan(list(costs), costs, 2)
        self.assertEqual(shards, [["a", "c"], ["b"]])
        self.assertEqual(totals, [5, 4])

    def test_jobs_charge_each_shards_longest_file_jobs_times(self):
        # One long file and many short ones, 4 jobs per shard. Plain LPT
        # would load the long file's shard to 40 like the other; with the
        # worst-case charge it counts 4*20=80, so the shorts go elsewhere.
        costs = {"long": 20, **{f"s{i:02d}": 1 for i in range(40)}}
        shards, totals = ts.plan(list(costs), costs, 2, jobs=4)
        self.assertEqual(shards[0], ["long"])
        self.assertEqual(len(shards[1]), 40)
        self.assertEqual(totals, [80, 43])   # s00 is shard 1's first: 4*1 + 39

    def test_jobs_one_is_plain_lpt(self):
        costs = {"a": 10, "b": 7, "c": 6, "d": 5, "e": 4}
        self.assertEqual(ts.plan(list(costs), costs, 2, jobs=1),
                         ts.plan(list(costs), costs, 2))

    def test_ties_break_by_path_then_lowest_index(self):
        costs = {"y": 1, "x": 1, "z": 1}
        shards, _ = ts.plan(list(costs), costs, 3)
        self.assertEqual(shards, [["x"], ["y"], ["z"]])

    def test_unknown_files_get_the_median(self):
        costs = {"a": 1, "b": 3, "c": 100}
        shards, totals = ts.plan(["a", "b", "c", "new"], costs, 2)
        self.assertEqual(sorted(totals), [7, 100])   # new costs median 3
        self.assertIn("new", shards[totals.index(7)])

    def test_no_costs_means_equal_weights(self):
        shards, totals = ts.plan(["d", "c", "b", "a"], {}, 2)
        self.assertEqual(totals, [2.0, 2.0])
        self.assertEqual(shards, [["a", "c"], ["b", "d"]])

    def test_deterministic_regardless_of_input_order(self):
        costs = {f"f{i}": (i * 37) % 11 + 1 for i in range(50)}
        files = list(costs)
        a = ts.plan(files, costs, 6)
        b = ts.plan(list(reversed(files)), costs, 6)
        self.assertEqual(a, b)

    def test_shards_are_disjoint_and_complete(self):
        costs = {f"f{i}": (i * 13) % 17 + 0.5 for i in range(100)}
        shards, _ = ts.plan(list(costs), costs, 6)
        flat = [f for s in shards for f in s]
        self.assertEqual(sorted(flat), sorted(costs))
        self.assertEqual(len(flat), len(set(flat)))

    def test_one_shard_keeps_every_file(self):
        shards, _ = ts.plan(["b", "a"], {"a": 5}, 1)
        self.assertEqual(shards, [["a", "b"]])


class CliTest(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), *args],
                              capture_output=True, text=True)

    def test_plan_writes_absolute_paths_back(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "costs.txt").write_text("# header\n5.00 src/a.cpp\n1.00 src/b.cpp\n")
            out = root / "out.txt"
            r = self.run_cli("plan", "--count", "2", "--index", "1", "--root", d,
                             "--costs", str(root / "costs.txt"), "--out", str(out),
                             f"{d}/src/a.cpp", f"{d}/src/b.cpp")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("tidy shard plan:", r.stdout)
            self.assertEqual(out.read_text(), f"{d}/src/b.cpp\n")

    def test_plan_rejects_zero_jobs(self):
        with tempfile.TemporaryDirectory() as d:
            r = self.run_cli("plan", "--count", "2", "--index", "0", "--jobs", "0",
                             "--out", f"{d}/o", "x")
            self.assertEqual(r.returncode, 2)

    def test_plan_rejects_bad_index(self):
        with tempfile.TemporaryDirectory() as d:
            r = self.run_cli("plan", "--count", "2", "--index", "2", "--out", f"{d}/o", "x")
            self.assertEqual(r.returncode, 2)

    def test_refresh_takes_misses_keeps_hits_and_drops_removed(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            costs = root / "costs.txt"
            costs.write_text("2.00 src/a.cpp\n3.00 src/b.cpp\n4.00 src/gone.cpp\n")
            stats = root / "s.json"
            stats.write_text(json.dumps({"per_tu": [
                {"tu": "@SRC@/src/a.cpp", "hit": False, "seconds": 9.5},
                {"tu": "@SRC@/src/b.cpp", "hit": True, "seconds": None},
                {"tu": "@SRC@/src/c.cpp", "hit": False, "seconds": 1.25},
            ]}))
            r = self.run_cli("refresh", "--costs", str(costs), "--root", d,
                             "--stats", str(stats), "--",
                             f"{d}/src/a.cpp", f"{d}/src/b.cpp", f"{d}/src/c.cpp")
            self.assertEqual(r.returncode, 0, r.stderr)
            got = ts.read_costs(costs)
            self.assertEqual(got, {"src/a.cpp": 9.5, "src/b.cpp": 3.0, "src/c.cpp": 1.25})
            self.assertTrue(costs.read_text().startswith("# Copyright (c) 2026 Clark Rawlins"))


class CheckedInCostsTest(unittest.TestCase):
    def test_checked_in_costs_parse_and_are_sorted(self):
        path = SCRIPT.parents[1] / "configs" / "tidy-file-costs.txt"
        costs = ts.read_costs(path)
        self.assertGreater(len(costs), 500)
        body = [l.split(None, 1)[1] for l in path.read_text().splitlines()
                if l.strip() and not l.startswith("#")]
        self.assertEqual(body, sorted(body))
        self.assertTrue(all(c > 0 for c in costs.values()))


if __name__ == "__main__":
    unittest.main()
