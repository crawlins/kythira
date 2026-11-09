#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Fail if a module under services/ reaches past Kythira's public surface.

Every directory services/<m>/ is a module (see services/README.md). A module
may use its own headers, the public core headers under include/, generated/
(the Kconfig autoconf header) and system or third-party headers. It may not
use core internals, core test code, core daemons or another module, and core
may not use it. This keeps every module buildable on its own, which is what
makes a later split into a separate repository a mechanical step
(redis-gateway-module Requirement 3).

The check needs no build and no compiler. It reads #include lines and CMake
files, resolves each include the way the compiler would on the module's
include path, classifies the target and applies these rules:

  other-module           a module file includes another module's header,
                         other than a composition-root include its
                         boundary-allow.txt lists as a `module-adapter`.
  impl-header            a module file includes a core *_impl.hpp that its
                         boundary-allow.txt does not list for that file.
  core-tests             a module file includes something under the top-level
                         tests/. Module tests may include the test fabric
                         (tests/ and tests/docker_chaos/, the directories the
                         kythira_test_support target puts on the include path),
                         and only by search path, never by a relative path.
  core-cmd               a module file includes something under the top-level
                         cmd/.
  core-includes-module   a core file (include/, src/, cmd/, tests/) includes
                         a module header.
  cmake-source-dir       a module CMake file uses ${CMAKE_SOURCE_DIR} or
                         ${PROJECT_SOURCE_DIR} to reach outside the module.
                         Core is reached through exported targets instead.
  cmake-escape           a module CMake file reaches outside the module via
                         ${CMAKE_CURRENT_SOURCE_DIR}/.. or
                         ${CMAKE_CURRENT_LIST_DIR}/...
  core-cmake-target      a core CMake file names a target a module defines.
                         The top-level add_subdirectory(services/<m>) is
                         exempt, as is any line carrying a `# module-hook`
                         comment (the documented install and workflow hooks).
  allow-entry            a boundary-allow.txt line is malformed, names a file
                         outside the module's cmd/ (the composition root) and
                         tests/, names a header that is not a core *_impl.hpp,
                         or is never used. A `module-adapter` line must name a
                         composition-root file and another module's header.

services/<m>/boundary-allow.txt holds the exceptions, one per line, `#`
starting a comment. Only the composition root (the daemon under cmd/) and
module tests that stand up an in-process cluster may be listed: both
instantiate core templates the way an application does. Library headers
never may.

    <file, relative to the module>  <core *_impl.hpp>  <why>

The header is either a basename (multi_raft_impl.hpp) or a path relative to
include/ (raft/multi_raft_impl.hpp). The reason is required.

A second kind of line lets a composition root wire in another module's
adapter to a core concept (iam-compatible-auth Requirement 17.5), the one
place two modules may meet:

    <file under cmd/>  module-adapter  <header as included>  <why>

where the header is spelled as the include spells it, relative to the other
module's include/ (kythira/iam/iam_redis_verifier.hpp). Library headers and
tests may never include another module.

Each violation prints as `<file>:<line>: [<rule>] <message>`, and as a GitHub
annotation when GITHUB_ACTIONS is set. The exit status is 1 if there is any
violation, 0 otherwise. With no services/ directory, or an empty one, there is
nothing to check and the exit status is 0.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

CXX_SUFFIXES = {".hpp", ".h", ".hh", ".hxx", ".ipp", ".tpp", ".inl",
                ".cpp", ".cc", ".cxx", ".c"}
CMAKE_NAMES = ("CMakeLists.txt",)
CMAKE_SUFFIXES = (".cmake",)

# Core directories whose files must never include a module header.
CORE_CODE_DIRS = ("include", "src", "cmd", "tests")
# Core directories searched for CMake files that must not name module targets.
CORE_CMAKE_DIRS = ("cmake", "src", "cmd", "tests")
# Directories kythira_test_support puts on a module test's include path.
TEST_SUPPORT_DIRS = ("tests", "tests/docker_chaos")

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
TARGET_DEF_RE = re.compile(
    r'\b(?:add_library|add_executable|add_custom_target|'
    r'add_raft_completion_test|add_docker_chaos_scenario_test)\s*\(\s*'
    r'([A-Za-z0-9_.+-]+)')
SOURCE_DIR_RE = re.compile(r'\$\{(CMAKE_SOURCE_DIR|PROJECT_SOURCE_DIR)\}([^\s")]*)')
CURRENT_DIR_RE = re.compile(
    r'\$\{(CMAKE_CURRENT_SOURCE_DIR|CMAKE_CURRENT_LIST_DIR)\}([^\s")]*)')
SUBDIR_HOOK_RE = re.compile(r'\badd_subdirectory\s*\(\s*"?services/')
MODULE_HOOK_RE = re.compile(r'#\s*module-hook\b')


@dataclass(frozen=True, order=True)
class Violation:
    file: str
    line: int
    rule: str
    message: str

    def render(self) -> str:
        return f"{self.file}:{self.line}: [{self.rule}] {self.message}"


@dataclass
class AllowEntry:
    line: int
    file: str
    header: str
    used: bool = False
    # A `module-adapter` line: `header` is another module's, by its path
    # under that module's include/.
    adapter: bool = False

    def matches(self, rel_file: str, include_rel: str) -> bool:
        if rel_file != self.file:
            return False
        if "/" in self.header:
            return include_rel == self.header
        return include_rel.rsplit("/", 1)[-1] == self.header


class Checker:
    def __init__(self, root: Path) -> None:
        self.root = root.resolve()
        self.services = self.root / "services"
        self.violations: list[Violation] = []
        self.modules: list[str] = []
        if self.services.is_dir():
            self.modules = sorted(
                p.name for p in self.services.iterdir()
                if p.is_dir() and not p.name.startswith("."))

    # ── helpers ──────────────────────────────────────────────────────────
    def rel(self, path: Path) -> str:
        return path.relative_to(self.root).as_posix()

    def report(self, path: Path, line: int, rule: str, message: str) -> None:
        self.violations.append(Violation(self.rel(path), line, rule, message))

    @staticmethod
    def walk(base: Path, want) -> list[Path]:
        out = []
        if not base.is_dir():
            return out
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
            for name in sorted(filenames):
                p = Path(dirpath) / name
                if want(p):
                    out.append(p)
        return out

    @staticmethod
    def is_cxx(p: Path) -> bool:
        return p.suffix in CXX_SUFFIXES

    @staticmethod
    def is_cmake(p: Path) -> bool:
        return p.name in CMAKE_NAMES or p.suffix in CMAKE_SUFFIXES

    @staticmethod
    def includes(path: Path):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return
        for n, line in enumerate(text.splitlines(), 1):
            m = INCLUDE_RE.match(line)
            if m:
                yield n, m.group(1), m.group(2).strip()

    @staticmethod
    def first_existing(candidates):
        for kind, base, spelled in candidates:
            p = (base / spelled)
            if p.is_file():
                return kind, p.resolve()
        return None, None

    def owner(self, path: Path) -> str | None:
        """The module a resolved path belongs to, or None for core."""
        try:
            parts = path.relative_to(self.services).parts
        except ValueError:
            return None
        return parts[0] if parts else None

    def under(self, path: Path, top: str) -> bool:
        try:
            path.relative_to(self.root / top)
            return True
        except ValueError:
            return False

    # ── module includes ──────────────────────────────────────────────────
    def load_allow(self, module: str) -> list[AllowEntry]:
        mod_dir = self.services / module
        allow_path = mod_dir / "boundary-allow.txt"
        entries: list[AllowEntry] = []
        if not allow_path.is_file():
            return entries
        for n, raw in enumerate(allow_path.read_text().splitlines(), 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.split(None, 1)[1:2] and line.split()[1] == "module-adapter":
                entry = self.load_adapter_entry(module, allow_path, n, line)
                if entry is not None:
                    entries.append(entry)
                continue
            fields = line.split(None, 2)
            if len(fields) < 3:
                self.report(allow_path, n, "allow-entry",
                            "expected `<file> <core *_impl.hpp> <why>`; "
                            "the reason is required")
                continue
            file, header, _why = fields
            target = mod_dir / file
            if not target.is_file():
                self.report(allow_path, n, "allow-entry",
                            f"{file} does not exist in services/{module}/")
                continue
            if not file.startswith(("cmd/", "tests/")):
                self.report(allow_path, n, "allow-entry",
                            f"{file} is neither the composition root "
                            f"(services/{module}/cmd/) nor a module test; "
                            f"library headers may not include core "
                            f"*_impl.hpp headers")
                continue
            if not header.endswith("_impl.hpp"):
                self.report(allow_path, n, "allow-entry",
                            f"{header} is not a *_impl.hpp header; public "
                            f"headers need no entry")
                continue
            found = ((self.root / "include" / header).is_file() if "/" in header
                     else any(p.name == header for p in
                              self.walk(self.root / "include",
                                        lambda p: p.suffix == ".hpp")))
            if not found:
                self.report(allow_path, n, "allow-entry",
                            f"{header} is not a core header under include/")
                continue
            entries.append(AllowEntry(n, file, header))
        return entries

    def load_adapter_entry(self, module: str, allow_path: Path, n: int,
                           line: str) -> AllowEntry | None:
        fields = line.split(None, 3)
        if len(fields) < 4:
            self.report(allow_path, n, "allow-entry",
                        "expected `<file> module-adapter <header> <why>`; "
                        "the reason is required")
            return None
        file, _kind, header, _why = fields
        if not (self.services / module / file).is_file():
            self.report(allow_path, n, "allow-entry",
                        f"{file} does not exist in services/{module}/")
            return None
        if not file.startswith("cmd/"):
            self.report(allow_path, n, "allow-entry",
                        f"{file} is not the composition root "
                        f"(services/{module}/cmd/); only it may wire in "
                        f"another module's adapter")
            return None
        owners = [m for m in self.modules if m != module
                  and (self.services / m / "include" / header).is_file()]
        if not owners:
            self.report(allow_path, n, "allow-entry",
                        f"{header} is not a header under another module's "
                        f"include/")
            return None
        return AllowEntry(n, file, header, adapter=True)

    def check_module_file(self, module: str, path: Path,
                          allow: list[AllowEntry]) -> None:
        mod_dir = self.services / module
        rel_in_mod = path.relative_to(mod_dir).as_posix()
        is_test = rel_in_mod.startswith("tests/")
        search = [("module", mod_dir / "include"),
                  ("core", self.root / "include"),
                  ("core", self.root / "generated")]
        if is_test:
            search += [("support", self.root / d) for d in TEST_SUPPORT_DIRS]
        # Not on any include path a module gets. Searched last so a spelling
        # that only works through a stray -I (`<tests/x.hpp>`, `<x.hpp>` with
        # -Itests) is still classified and reported.
        search += [("stray", self.root)]
        if not is_test:
            search += [("stray", self.root / d) for d in TEST_SUPPORT_DIRS]
        search += [("module", self.services / m / "include")
                   for m in self.modules if m != module]

        for n, delim, spelled in self.includes(path):
            cands = []
            if delim == '"':
                cands.append(("relative", path.parent, spelled))
            cands += [(kind, base, spelled) for kind, base in search]
            how, target = self.first_existing(cands)
            if target is None:
                continue  # system or third-party
            tgt_mod = self.owner(target)
            if tgt_mod == module:
                continue
            if tgt_mod is not None:
                inc_rel = target.relative_to(
                    self.services / tgt_mod / "include").as_posix() \
                    if self.under(target, f"services/{tgt_mod}/include") else None
                hit = [e for e in allow if e.adapter and e.file == rel_in_mod
                       and e.header == inc_rel]
                for e in hit:
                    e.used = True
                if hit:
                    continue
                self.report(path, n, "other-module",
                            f"includes {self.rel(target)} from module "
                            f"'{tgt_mod}'; share it through a core concept "
                            f"wired in the composition root instead")
                continue
            if self.under(target, "include"):
                inc_rel = target.relative_to(self.root / "include").as_posix()
                if target.name.endswith("_impl.hpp"):
                    hit = [e for e in allow
                           if not e.adapter and e.matches(rel_in_mod, inc_rel)]
                    for e in hit:
                        e.used = True
                    if not hit:
                        self.report(path, n, "impl-header",
                                    f"includes core internal {inc_rel}; only "
                                    f"composition-root files listed in "
                                    f"services/{module}/boundary-allow.txt "
                                    f"may")
                continue
            if self.under(target, "generated"):
                continue
            if self.under(target, "tests"):
                if is_test and how == "support":
                    continue
                if is_test:
                    self.report(path, n, "core-tests",
                                f"reaches {self.rel(target)} by a path; "
                                f"link kythira_test_support and include it "
                                f"by name")
                else:
                    self.report(path, n, "core-tests",
                                f"includes core test code {self.rel(target)}"
                                f"; only module tests may use the test "
                                f"fabric, through kythira_test_support")
                continue
            if self.under(target, "cmd"):
                self.report(path, n, "core-cmd",
                            f"includes core daemon code {self.rel(target)}")
                continue
            # Anything else in the tree (scripts/, tools/) is no header a
            # build would find; treat it like a system header.

    # ── module CMake ─────────────────────────────────────────────────────
    def module_targets(self, module: str) -> set[str]:
        names: set[str] = set()
        for p in self.walk(self.services / module, self.is_cmake):
            for line in p.read_text(errors="replace").splitlines():
                code = line.split("#", 1)[0]
                for m in TARGET_DEF_RE.finditer(code):
                    if "${" not in m.group(1):
                        names.add(m.group(1))
        return names

    def check_module_cmake(self, module: str, path: Path) -> None:
        mod_dir = (self.services / module).resolve()
        for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            code = line.split("#", 1)[0]
            for m in SOURCE_DIR_RE.finditer(code):
                tail = m.group(2)
                if not (tail == f"/services/{module}"
                        or tail.startswith(f"/services/{module}/")):
                    self.report(path, n, "cmake-source-dir",
                                f"${{{m.group(1)}}}{tail} reaches outside "
                                f"services/{module}/; use an exported core "
                                f"target or ${{CMAKE_CURRENT_SOURCE_DIR}}")
            for m in CURRENT_DIR_RE.finditer(code):
                tail = m.group(2)
                if ".." not in tail:
                    continue
                dest = Path(os.path.normpath(path.parent / tail.lstrip("/")))
                try:
                    dest.relative_to(mod_dir)
                except ValueError:
                    self.report(path, n, "cmake-escape",
                                f"${{{m.group(1)}}}{tail} resolves outside "
                                f"services/{module}/")

    # ── core side ────────────────────────────────────────────────────────
    def check_core_includes(self) -> None:
        core_search = [self.root / "include", self.root / "generated"]
        core_search += [self.root / d for d in TEST_SUPPORT_DIRS]
        mod_search = [self.services / m / "include" for m in self.modules]
        for top in CORE_CODE_DIRS:
            for path in self.walk(self.root / top, self.is_cxx):
                for n, delim, spelled in self.includes(path):
                    cands = []
                    if delim == '"':
                        cands.append(("relative", path.parent, spelled))
                    cands += [("core", b, spelled) for b in core_search]
                    cands += [("module", b, spelled) for b in mod_search]
                    _, target = self.first_existing(cands)
                    if target is None:
                        continue
                    mod = self.owner(target)
                    if mod is not None:
                        self.report(path, n, "core-includes-module",
                                    f"core file includes {self.rel(target)} "
                                    f"from module '{mod}'; core must build "
                                    f"without any module")

    def core_cmake_files(self) -> list[Path]:
        files = [self.root / n for n in CMAKE_NAMES if (self.root / n).is_file()]
        for top in CORE_CMAKE_DIRS:
            files += self.walk(self.root / top, self.is_cmake)
        return files

    def check_core_cmake(self, targets: dict[str, set[str]]) -> None:
        if not any(targets.values()):
            return
        patterns = {
            name: (mod, re.compile(r'(?<![A-Za-z0-9_.+-])' + re.escape(name)
                                   + r'(?![A-Za-z0-9_.+-])'))
            for mod, names in targets.items() for name in names}
        for path in self.core_cmake_files():
            for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
                if SUBDIR_HOOK_RE.search(line) or MODULE_HOOK_RE.search(line):
                    continue
                code = line.split("#", 1)[0]
                for name, (mod, pat) in sorted(patterns.items()):
                    if pat.search(code):
                        self.report(path, n, "core-cmake-target",
                                    f"core CMake names '{name}', a target of "
                                    f"module '{mod}'; mark a deliberate hook "
                                    f"with a `# module-hook` comment")

    # ── driver ───────────────────────────────────────────────────────────
    def run(self) -> list[Violation]:
        targets: dict[str, set[str]] = {}
        for module in self.modules:
            mod_dir = self.services / module
            allow = self.load_allow(module)
            for path in self.walk(mod_dir, self.is_cxx):
                self.check_module_file(module, path, allow)
            for e in allow:
                if not e.used:
                    self.report(mod_dir / "boundary-allow.txt", e.line,
                                "allow-entry",
                                f"{e.file} never includes {e.header}; "
                                f"remove the stale entry")
            for path in self.walk(mod_dir, self.is_cmake):
                self.check_module_cmake(module, path)
            targets[module] = self.module_targets(module)
        if self.modules:
            self.check_core_includes()
            self.check_core_cmake(targets)
        return sorted(set(self.violations))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("--root", type=Path,
                    default=Path(__file__).resolve().parent.parent,
                    help="source tree to check (default: this repository)")
    args = ap.parse_args(argv)

    checker = Checker(args.root)
    violations = checker.run()
    gha = os.environ.get("GITHUB_ACTIONS") == "true"
    for v in violations:
        print(v.render())
        if gha:
            print(f"::error file={v.file},line={v.line}::[{v.rule}] {v.message}")
    if not checker.modules:
        print("check_module_boundaries: no modules under services/; "
              "nothing to check")
    elif not violations:
        print(f"check_module_boundaries: {len(checker.modules)} module(s) "
              f"clean: {', '.join(checker.modules)}")
    else:
        print(f"check_module_boundaries: {len(violations)} violation(s)",
              file=sys.stderr)
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
