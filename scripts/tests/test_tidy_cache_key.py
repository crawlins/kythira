#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Tests for scripts/tidy_cache_key.py (clang-tidy-result-cache task 3).

Each case builds a tiny workspace with a compile database, computes a key,
changes exactly one input from Requirement 2.1 and checks whether the key
moved. Needs clang++-18 and clang-tidy-18; the tests skip without them
unless TIDY_CACHE_KEY_TESTS_REQUIRED=1 (set in CI), which fails instead.

Run: python3 scripts/tests/test_tidy_cache_key.py
"""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import stat
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "tidy_cache_key.py"
spec = importlib.util.spec_from_file_location("tidy_cache_key", SCRIPT)
tk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tk)

CLANG = shutil.which("clang++-18")
TIDY = shutil.which("clang-tidy-18")
REQUIRED = os.environ.get("TIDY_CACHE_KEY_TESTS_REQUIRED") == "1"
HAVE_TOOLS = bool(CLANG and TIDY)
if REQUIRED and not HAVE_TOOLS:
    raise SystemExit("TIDY_CACHE_KEY_TESTS_REQUIRED=1 but clang++-18 or clang-tidy-18 is missing")
needs_tools = unittest.skipUnless(HAVE_TOOLS, "needs clang++-18 and clang-tidy-18")


class Workspace:
    """ws/{src,build} plus a header directory outside the workspace."""

    def __init__(self, root: Path, sysdir: Path):
        self.root = root
        self.sysdir = sysdir
        self.build = root / "build"
        (root / "inc").mkdir(parents=True)
        (root / "sub").mkdir()
        self.build.mkdir()
        sysdir.mkdir(parents=True, exist_ok=True)
        (root / ".clang-tidy").write_text("Checks: '-*,bugprone-*'\n")
        (root / "inc" / "h.hpp").write_text("#pragma once\ninline int h() { return 1; }\n")
        (root / "inc" / "only_pch.hpp").write_text("#pragma once\ninline int p() { return 2; }\n")
        (root / "inc" / "pch.hxx").write_text('#include "only_pch.hpp"\n')
        (sysdir / "s.hpp").write_text("#pragma once\ninline int s() { return 3; }\n")
        (root / "a.cpp").write_text(
            '#include "h.hpp"\n#include <s.hpp>\n'
            "int a() { return h() + s(); }  // NOLINT(bugprone-foo)\n")
        (root / "sub" / "b.cpp").write_text('#include "h.hpp"\nint b() { return h(); }\n')
        (root / "pch_user.cpp").write_text("int u() { return p(); }\n")
        self.defines = {"a.cpp": "-DMODE=1"}
        self.write_db()

    def command(self, rel: str) -> list[str]:
        argv = ["/usr/bin/sccache", CLANG or "clang++-18", "-std=c++20",
                self.defines.get(rel, "-DMODE=0"),
                f"-I{self.root}/inc", f"-isystem{self.sysdir}"]
        if rel == "pch_user.cpp":
            pch = self.build / "cmake_pch.hxx"
            argv += ["-Winvalid-pch", "-Xclang", "-include-pch", "-Xclang", f"{pch}.pch",
                     "-Xclang", "-include", "-Xclang", f"{self.root}/inc/pch.hxx"]
        argv += ["-MD", "-MT", f"{rel}.o", "-MF", f"{rel}.o.d",
                 "-o", f"{self.build}/{rel}.o", "-c", f"{self.root}/{rel}"]
        return argv

    def write_db(self):
        db = [{"directory": str(self.build), "file": str(self.root / rel),
               "arguments": self.command(rel)}
              for rel in ("a.cpp", "sub/b.cpp", "pch_user.cpp")]
        (self.build / "compile_commands.json").write_text(json.dumps(db))

    def ctx(self, **kw) -> "tk.KeyContext":
        return tk.KeyContext(self.build, self.root, kw.pop("clang_tidy", TIDY), **kw)

    def key(self, rel: str, **kw) -> str:
        return self.ctx(**kw).key(str(self.root / rel))[0]


@needs_tools
class KeyInputsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = Path(self.tmp.name)
        self.ws = Workspace(t / "ws", t / "sys")

    def tearDown(self):
        self.tmp.cleanup()

    def assert_changes(self, rel, mutate, **kw):
        before = self.ws.key(rel)
        mutate()
        self.assertNotEqual(before, self.ws.key(rel, **kw))

    def test_key_is_stable(self):
        self.assertEqual(self.ws.key("a.cpp"), self.ws.key("a.cpp"))

    def test_format_version_changes_key(self):
        before = self.ws.key("a.cpp")
        old = tk.KEY_FORMAT
        tk.KEY_FORMAT = "tidy-cache-v999"
        try:
            self.assertNotEqual(before, self.ws.key("a.cpp"))
        finally:
            tk.KEY_FORMAT = old

    def test_tool_binary_changes_key(self):
        # A different binary (a wrapper script around the real clang-tidy)
        # with identical --version output still moves the key.
        wrapper = Path(self.tmp.name) / "clang-tidy-wrapper"
        wrapper.write_text(f'#!/bin/sh\nexec "{TIDY}" "$@"\n')
        wrapper.chmod(wrapper.stat().st_mode | stat.S_IEXEC)
        self.assertNotEqual(self.ws.key("a.cpp"),
                            self.ws.key("a.cpp", clang_tidy=str(wrapper)))

    def test_root_clang_tidy_change_changes_key(self):
        self.assert_changes("a.cpp", lambda: (self.ws.root / ".clang-tidy").write_text(
            "Checks: '-*,bugprone-*,performance-*'\n"))

    def test_nested_clang_tidy_changes_only_that_directory(self):
        a = self.ws.key("a.cpp")
        self.assert_changes("sub/b.cpp", lambda: (self.ws.root / "sub" / ".clang-tidy").write_text(
            "InheritParentConfig: true\nChecks: '-bugprone-branch-clone'\n"))
        self.assertEqual(a, self.ws.key("a.cpp"))

    def test_extra_tidy_args_change_key(self):
        before = self.ws.key("a.cpp")
        self.assertNotEqual(before, self.ws.key("a.cpp", tidy_args=["--checks=-*,cert-*"]))

    def test_define_change_changes_key(self):
        def mutate():
            self.ws.defines["a.cpp"] = "-DMODE=2"
            self.ws.write_db()
        self.assert_changes("a.cpp", mutate)

    def test_nolint_removal_changes_key(self):
        a = self.ws.root / "a.cpp"
        self.assert_changes("a.cpp", lambda: a.write_text(
            a.read_text().replace("  // NOLINT(bugprone-foo)", "")))

    def test_header_change_changes_key(self):
        h = self.ws.root / "inc" / "h.hpp"
        self.assert_changes("a.cpp", lambda: h.write_text(h.read_text() + "// edit\n"))

    def test_header_reached_only_through_pch_changes_key(self):
        h = self.ws.root / "inc" / "only_pch.hpp"
        self.assert_changes("pch_user.cpp", lambda: h.write_text(h.read_text() + "// edit\n"))

    def test_pch_file_bytes_are_not_read(self):
        # The .pch named on the command line need not exist: the scan drops it.
        self.assertFalse((self.ws.build / "cmake_pch.hxx.pch").exists())
        self.ws.key("pch_user.cpp")

    def test_system_header_change_changes_key(self):
        s = self.ws.sysdir / "s.hpp"
        self.assert_changes("a.cpp", lambda: s.write_text(s.read_text() + "// edit\n"))

    def test_unrelated_header_change_keeps_key(self):
        before = self.ws.key("sub/b.cpp")
        (self.ws.root / "inc" / "only_pch.hpp").write_text("// edit\n")
        self.assertEqual(before, self.ws.key("sub/b.cpp"))

    def test_dropped_options_do_not_change_key(self):
        before = self.ws.key("a.cpp")
        db_path = self.ws.build / "compile_commands.json"
        db = json.loads(db_path.read_text())
        for e in db:
            e["arguments"] = [a.replace(".o.d", ".dep").replace(".o", ".obj")
                              for a in e["arguments"] if a != "/usr/bin/sccache"]
        db_path.write_text(json.dumps(db))
        self.assertEqual(before, self.ws.key("a.cpp"))

    def test_workspace_path_change_keeps_key(self):
        other = Workspace(Path(self.tmp.name) / "elsewhere" / "ws", self.ws.sysdir)
        for rel in ("a.cpp", "sub/b.cpp", "pch_user.cpp"):
            self.assertEqual(self.ws.key(rel), other.key(rel), rel)

    def test_edit_during_run_is_rehashed(self):
        ctx = self.ws.ctx()
        tu = str(self.ws.root / "a.cpp")
        before = ctx.key(tu)[0]
        h = self.ws.root / "inc" / "h.hpp"
        h.write_text(h.read_text() + "// a longer edit\n")
        self.assertNotEqual(before, ctx.key(tu)[0])

    def test_failed_scan_gives_no_key(self):
        a = self.ws.root / "a.cpp"
        a.write_text('#include "missing.hpp"\n' + a.read_text())
        with self.assertRaises(tk.KeyUnavailable):
            self.ws.key("a.cpp")

    def test_tu_missing_from_compile_db_gives_no_key(self):
        (self.ws.root / "c.cpp").write_text("int c() { return 0; }\n")
        with self.assertRaises(tk.KeyUnavailable):
            self.ws.key("c.cpp")


class PureHelpersTest(unittest.TestCase):
    def test_scan_argv_rewrites_pch_and_drops_outputs(self):
        argv = ["sccache", "clang++", "-DX", "-Winvalid-pch",
                "-Xclang", "-include-pch", "-Xclang", "p.pch",
                "-Xclang", "-include", "-Xclang", "p.hxx",
                "-MD", "-MT", "t", "-MF", "t.d", "-o", "t.o", "-c", "t.cpp"]
        self.assertEqual(tk.scan_argv(argv),
                         ["clang++", "-DX", "-Xclang", "-include", "-Xclang", "p.hxx",
                          "t.cpp", "-M", "-MF", "-"])

    def test_parse_deps_handles_continuations_and_escaped_spaces(self):
        text = "t.o: a.cpp /x/my\\ dir/h.hpp \\\n  /usr/include/s.h\n"
        self.assertEqual(tk.parse_deps(text), ["a.cpp", "/x/my dir/h.hpp", "/usr/include/s.h"])

    def test_parts_are_length_prefixed(self):
        self.assertNotEqual(tk.sha256_parts(b"ab", b"c"), tk.sha256_parts(b"a", b"bc"))


if __name__ == "__main__":
    unittest.main()
