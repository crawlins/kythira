#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Cache key for one clang-tidy translation unit.

.kiro/specs/clang-tidy-result-cache/ Requirement 2, task 3. Imported by
scripts/tidy-cache.py; on its own it prints keys:

  tidy_cache_key.py -p BUILD [--src SRC] [--clang-tidy BIN] FILE...

A TU's key is a SHA-256 over, in this order, each part length-prefixed:

  1. KEY_FORMAT, bumped whenever this computation changes;
  2. clang-tidy's `--version` output and the SHA-256 of its binary;
  3. `clang-tidy --dump-config <TU>` (every .clang-tidy on the path, folded)
     and the extra arguments the caller passes clang-tidy;
  4. the compile command, normalised: the workspace and build paths become
     @SRC@ and @BUILD@, and only options that cannot change parsing go
     (the sccache/ccache launcher, -o, -c, -MD/-MMD, -MF/-MT/-MQ);
  5. the TU's own path, then the path and raw-byte SHA-256 of every file
     the compiler reads for it, from a dependency-only (-M) run of the
     same command.

Raw bytes, not preprocessed text: preprocessing drops comments, so a
removed NOLINT would keep its stale clean result. Headers outside the
workspace (system, vcpkg_installed/) are keyed by content too, under their
absolute path.

CMake's clang PCH flags are rewritten for the scan: the `-include-pch
<x.pch>` pair goes (a .pch is not byte-reproducible across runners) and
the cc1-level `-include <cmake_pch.hxx>` stays, so headers reached only
through the PCH appear in the dependency list. A driver-level `-include`
would not do: the driver finds <hxx>.pch beside the header and uses it,
hiding those headers again.

Anything that stops a key being computed raises KeyUnavailable. The
caller treats that TU as a miss and never stores a result for it
(Requirement 2.6).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import threading
from pathlib import Path

KEY_FORMAT = "tidy-cache-v1"

# Options that cannot change what clang-tidy parses or reports. They are
# dropped from both the keyed command and the dependency scan.
DROP_WITH_ARG = {"-o", "-MF", "-MT", "-MQ"}
DROP_ALONE = {"-c", "-MD", "-MMD"}
LAUNCHERS = {"sccache", "ccache"}


class KeyUnavailable(Exception):
    """No key for this TU: analyse it, and never store its result."""


def sha256_parts(*parts: bytes) -> str:
    """SHA-256 over length-prefixed parts, so no two part lists collide."""
    h = hashlib.sha256()
    for p in parts:
        h.update(len(p).to_bytes(8, "little"))
        h.update(p)
    return h.hexdigest()


def compile_argv(entry: dict) -> list[str]:
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def _skip_launcher(argv: list[str]) -> int:
    return 1 if argv and os.path.basename(argv[0]) in LAUNCHERS else 0


def _droppable(argv: list[str], i: int) -> int:
    """How many arguments at argv[i] to drop (0 keeps argv[i])."""
    a = argv[i]
    if a in DROP_WITH_ARG:
        return 2
    if a in DROP_ALONE:
        return 1
    if a.startswith("-o") and len(a) > 2:
        return 1
    return 0


def scan_argv(argv: list[str]) -> list[str]:
    """The compile command as a dependency-only run printing to stdout."""
    out: list[str] = []
    i = _skip_launcher(argv)
    while i < len(argv):
        a = argv[i]
        nxt = argv[i + 1] if i + 1 < len(argv) else None
        if a == "-Xclang" and nxt == "-include-pch":
            i += 4          # -Xclang -include-pch -Xclang <x.pch>
            continue
        if a == "-Winvalid-pch":
            i += 1
            continue
        n = _droppable(argv, i)
        if n:
            i += n
            continue
        out.append(a)
        i += 1
    return out + ["-M", "-MF", "-"]


def parse_deps(text: str) -> list[str]:
    """Paths from Make-style `-M` output (`target: dep dep \\` lines)."""
    body = text.split(":", 1)[1] if ":" in text else ""
    body = body.replace("\\\n", " ")
    deps: list[str] = []
    cur: list[str] = []
    i = 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body) and body[i + 1] == " ":
            cur.append(" ")
            i += 2
            continue
        if c.isspace():
            if cur:
                deps.append("".join(cur))
                cur = []
        else:
            cur.append(c)
        i += 1
    if cur:
        deps.append("".join(cur))
    return deps


class KeyContext:
    """Everything shared between the TUs of one run, memoised.

    File hashes are memoised by (path, size, mtime_ns), so a file edited
    during the run is hashed again; `--dump-config` output by directory.
    Safe to use from several threads.
    """

    def __init__(self, build: Path, src: Path, clang_tidy: str = "clang-tidy-18",
                 tidy_args: list[str] | None = None):
        self.build = Path(build).resolve()
        self.src = Path(src).resolve()
        self.clang_tidy = clang_tidy
        self.tidy_args = list(tidy_args or [])
        self._lock = threading.Lock()
        self._file_hash: dict[tuple[str, int, int], str] = {}
        self._dir_config: dict[str, bytes] = {}
        db_path = self.build / "compile_commands.json"
        try:
            db = json.loads(db_path.read_text())
        except (OSError, ValueError) as e:
            raise SystemExit(f"tidy-cache: cannot read {db_path}: {e}") from e
        self.commands: dict[str, dict] = {}
        for entry in db:
            f = Path(entry["directory"], entry["file"]).resolve()
            self.commands[str(f)] = entry
        tidy_path = shutil.which(clang_tidy)
        if not tidy_path:
            raise SystemExit(f"tidy-cache: {clang_tidy} not found")
        version = subprocess.run([tidy_path, "--version"], capture_output=True).stdout
        binary = Path(os.path.realpath(tidy_path)).read_bytes()
        self.tool_part = version + b"\0" + hashlib.sha256(binary).hexdigest().encode()

    def rel(self, path: str) -> str:
        """@BUILD@/... or @SRC@/... for workspace paths; others unchanged."""
        p = os.path.normpath(path)
        for root, token in ((str(self.build), "@BUILD@"), (str(self.src), "@SRC@")):
            if p == root or p.startswith(root + os.sep):
                return token + p[len(root):]
        return p

    def _tokenise(self, arg: str) -> str:
        return arg.replace(str(self.build), "@BUILD@").replace(str(self.src), "@SRC@")

    def normalised(self, argv: list[str]) -> list[str]:
        out: list[str] = []
        i = _skip_launcher(argv)
        while i < len(argv):
            n = _droppable(argv, i)
            if n:
                i += n
                continue
            out.append(self._tokenise(argv[i]))
            i += 1
        return out

    def file_hash(self, path: str) -> str:
        try:
            st = os.stat(path)
        except OSError as e:
            raise KeyUnavailable(f"cannot stat dependency {path}: {e}") from e
        memo = (path, st.st_size, st.st_mtime_ns)
        with self._lock:
            cached = self._file_hash.get(memo)
        if cached is not None:
            return cached
        try:
            digest = hashlib.sha256(Path(path).read_bytes()).hexdigest()
        except OSError as e:
            raise KeyUnavailable(f"cannot read dependency {path}: {e}") from e
        with self._lock:
            self._file_hash[memo] = digest
        return digest

    def dir_config(self, tu: str) -> bytes:
        d = os.path.dirname(tu)
        with self._lock:
            cached = self._dir_config.get(d)
        if cached is not None:
            return cached
        r = subprocess.run([self.clang_tidy, *self.tidy_args, "--dump-config", tu],
                           capture_output=True)
        if r.returncode != 0:
            raise KeyUnavailable(f"--dump-config failed for {tu}: "
                                 f"{r.stderr.decode(errors='replace')[-2000:]}")
        with self._lock:
            self._dir_config[d] = r.stdout
        return r.stdout

    def key(self, tu: str) -> tuple[str, dict]:
        """(hex key, {"deps": n}) for one TU, or raise KeyUnavailable."""
        tu = str(Path(tu).resolve())
        entry = self.commands.get(tu)
        if entry is None:
            raise KeyUnavailable(f"{tu} is not in compile_commands.json")
        argv = compile_argv(entry)
        try:
            r = subprocess.run(scan_argv(argv), cwd=entry["directory"],
                               capture_output=True, text=True)
        except OSError as e:
            raise KeyUnavailable(f"dependency scan could not start for {tu}: {e}") from e
        if r.returncode != 0:
            raise KeyUnavailable(f"dependency scan failed for {tu}: {r.stderr[-2000:]}")
        absolute = {os.path.normpath(os.path.join(entry["directory"], d))
                    for d in parse_deps(r.stdout)}
        if not absolute:
            raise KeyUnavailable(f"dependency scan listed no files for {tu}")
        # Sorted by the tokenised path, so moving the workspace (which
        # reorders absolute paths against /usr/... ones) keeps the key.
        deps = sorted((self.rel(d), d) for d in absolute)
        dep_part = "\n".join(f"{r} {self.file_hash(d)}" for r, d in deps)
        key = sha256_parts(
            KEY_FORMAT.encode(),
            self.tool_part,
            self.dir_config(tu) + b"\0" + "\0".join(self.tidy_args).encode(),
            "\0".join(self.normalised(argv)).encode(),
            self.rel(tu).encode(),
            dep_part.encode(),
        )
        return key, {"deps": len(deps)}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", required=True, help="build directory with compile_commands.json")
    ap.add_argument("--src", default=".", help="workspace root (default: cwd)")
    ap.add_argument("--clang-tidy", default="clang-tidy-18")
    ap.add_argument("--tidy-arg", action="append", default=[],
                    help="extra argument passed to clang-tidy (keyed)")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()
    ctx = KeyContext(Path(args.p), Path(args.src), args.clang_tidy, args.tidy_arg)
    rc = 0
    for tu in args.files:
        try:
            k, info = ctx.key(tu)
            print(k, ctx.rel(str(Path(tu).resolve())), info["deps"])
        except KeyUnavailable as e:
            print(f"tidy-cache: no key: {e}", file=sys.stderr)
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
