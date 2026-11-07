#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""clang-tidy clean-result cache driver.

.kiro/specs/clang-tidy-result-cache/ design Component 2, task 4. Analyses
translation units with clang-tidy, skipping any whose cache key
(scripts/tidy_cache_key.py) matches a stored clean result:

  tidy-cache.py run -p BUILD --mode MODE [options] FILE...

MODE:
  off              analyse everything, look nothing up, store nothing
  read             look up in the S3-compatible bucket, never store
  readwrite        look up, and store clean results
  local:<dir>      the same against a local directory, no credentials
  env              read or readwrite from $OCI_BUILD_CACHE_MODE, which the
                   build-cache action exports

The bucket comes from the variables .github/actions/oci-build-cache
exports: AWS_ENDPOINT_URL, AWS_REGION, AWS_ACCESS_KEY_ID,
AWS_SECRET_ACCESS_KEY and SCCACHE_BUCKET. Objects live under clang-tidy/.

Only clean results are stored: clang-tidy exited 0 and printed no
`file:line:col: warning|error:` line. Nothing stored is replayed, so every
finding comes from a fresh run. A hit prints `cached: <tu>`; a miss prints
clang-tidy's output under `== <tu>`. Exit status: 0 when every TU is clean,
1 when any TU has a finding or clang-tidy failed, 2 when the cache was
requested (read/readwrite/env) and no lookup succeeded (the silent no-op),
3 when --verify-sample found a cached clean that is no longer clean.

Fail open: a backend error on a lookup is a miss, on a store a warning.
Each request has a 10 s timeout and 2 retries; after 20 consecutive
backend errors the backend is switched off for the rest of the run. A TU
with no key (failed dependency scan, say) is analysed and never stored.
TUs with no compile command are skipped and counted, as run-clang-tidy
skips them.

With --costs, misses start longest first, so the slowest file does not
start last and run alone at the end.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime as dt
import hashlib
import hmac
import http.client
import importlib.util
import json
import os
import random
import re
import subprocess
import sys
import threading
import time
import urllib.parse
from pathlib import Path

_spec = importlib.util.spec_from_file_location(
    "tidy_cache_key", Path(__file__).resolve().parent / "tidy_cache_key.py")
tidy_cache_key = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tidy_cache_key)
KeyUnavailable = tidy_cache_key.KeyUnavailable

DIAG_RE = re.compile(r"^.+:\d+:\d+: (warning|error):", re.MULTILINE)
PREFIX = "clang-tidy/"
TIMEOUT_S = 10
RETRY_BACKOFF_S = (1, 3)
ERROR_BUDGET = 20


# ── Backends ────────────────────────────────────────────────────────────────

class BackendError(Exception):
    """A request failed after its retries."""


class LocalBackend:
    writable = True

    def __init__(self, root: Path):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)

    def _path(self, key: str) -> Path:
        return self.root / key[:2] / key

    def lookup(self, key: str) -> bool:
        return self._path(key).exists()

    def close(self) -> None:
        pass

    def store(self, key: str, body: bytes) -> str:
        p = self._path(key)
        p.parent.mkdir(exist_ok=True)
        tmp = p.with_name(f"{p.name}.tmp{os.getpid()}.{threading.get_ident()}")
        tmp.write_bytes(body)
        os.replace(tmp, p)
        return "stored"


def sigv4_headers(method: str, host: str, path: str, region: str, access_key: str,
                  secret_key: str, body: bytes, now: dt.datetime | None = None,
                  session_token: str | None = None) -> dict[str, str]:
    """AWS Signature Version 4 headers for one S3 request, no query string."""
    now = now or dt.datetime.now(dt.timezone.utc)
    amz_date = now.strftime("%Y%m%dT%H%M%SZ")
    date = now.strftime("%Y%m%d")
    payload_hash = hashlib.sha256(body).hexdigest()
    headers = {"host": host, "x-amz-content-sha256": payload_hash, "x-amz-date": amz_date}
    if session_token:
        headers["x-amz-security-token"] = session_token
    names = sorted(headers)
    canonical = "\n".join([
        method,
        urllib.parse.quote(path, safe="/-_.~"),
        "",
        "".join(f"{n}:{headers[n].strip()}\n" for n in names),
        ";".join(names),
        payload_hash,
    ])
    scope = f"{date}/{region}/s3/aws4_request"
    to_sign = "\n".join(["AWS4-HMAC-SHA256", amz_date, scope,
                         hashlib.sha256(canonical.encode()).hexdigest()])

    def h(key: bytes, msg: str) -> bytes:
        return hmac.new(key, msg.encode(), hashlib.sha256).digest()

    k = h(h(h(h(f"AWS4{secret_key}".encode(), date), region), "s3"), "aws4_request")
    signature = hmac.new(k, to_sign.encode(), hashlib.sha256).hexdigest()
    headers["authorization"] = (f"AWS4-HMAC-SHA256 Credential={access_key}/{scope}, "
                                f"SignedHeaders={';'.join(names)}, Signature={signature}")
    return headers


class S3Backend:
    """HEAD and PUT on <endpoint>/<bucket>/clang-tidy/<key>, path style."""

    def __init__(self, endpoint: str, region: str, bucket: str, access_key: str,
                 secret_key: str, writable: bool, session_token: str | None = None,
                 timeout: float = TIMEOUT_S, backoff: tuple[float, ...] = RETRY_BACKOFF_S):
        u = urllib.parse.urlsplit(endpoint)
        if u.scheme not in ("http", "https") or not u.hostname:
            raise ValueError(f"bad endpoint {endpoint!r}")
        self.scheme = u.scheme
        self.netloc = u.netloc
        self.base = u.path.rstrip("/")
        self.region = region
        self.bucket = bucket
        self.access_key = access_key
        self.secret_key = secret_key
        self.session_token = session_token
        self.writable = writable
        self.timeout = timeout
        self.backoff = backoff
        self._local = threading.local()
        self._all_conns: list[http.client.HTTPConnection] = []
        self._conns_lock = threading.Lock()

    @classmethod
    def from_env(cls, writable: bool) -> "S3Backend":
        env = os.environ
        missing = [v for v in ("AWS_ENDPOINT_URL", "AWS_ACCESS_KEY_ID",
                               "AWS_SECRET_ACCESS_KEY", "SCCACHE_BUCKET") if not env.get(v)]
        if missing:
            raise ValueError("missing " + ", ".join(missing))
        return cls(env["AWS_ENDPOINT_URL"], env.get("AWS_REGION") or env.get("SCCACHE_REGION")
                   or "us-east-1", env["SCCACHE_BUCKET"], env["AWS_ACCESS_KEY_ID"],
                   env["AWS_SECRET_ACCESS_KEY"], writable, env.get("AWS_SESSION_TOKEN"))

    def _conn(self) -> http.client.HTTPConnection:
        c = getattr(self._local, "conn", None)
        if c is None:
            cls = http.client.HTTPSConnection if self.scheme == "https" else http.client.HTTPConnection
            c = cls(self.netloc, timeout=self.timeout)
            self._local.conn = c
            with self._conns_lock:
                self._all_conns.append(c)
        return c

    def close(self) -> None:
        with self._conns_lock:
            for c in self._all_conns:
                c.close()
            self._all_conns.clear()

    def _drop_conn(self) -> None:
        c = getattr(self._local, "conn", None)
        if c is not None:
            c.close()
            self._local.conn = None

    def request(self, method: str, key: str, body: bytes = b"") -> tuple[int, bytes]:
        """(status, body) for one request, retried on transport errors and 5xx."""
        path = f"{self.base}/{self.bucket}/{PREFIX}{key}"
        last = ""
        for attempt in range(len(self.backoff) + 1):
            if attempt:
                time.sleep(self.backoff[attempt - 1])
            headers = sigv4_headers(method, self.netloc, path, self.region, self.access_key,
                                    self.secret_key, body, session_token=self.session_token)
            if method == "PUT":
                headers["content-type"] = "application/json"
                headers["content-length"] = str(len(body))
            try:
                conn = self._conn()
                conn.request(method, urllib.parse.quote(path, safe="/-_.~"),
                             body=body if method == "PUT" else None, headers=headers)
                resp = conn.getresponse()
                data = resp.read()
            except (OSError, http.client.HTTPException) as e:
                self._drop_conn()
                last = f"{method} {key[:12]}: {type(e).__name__}: {e}"
                continue
            if resp.status >= 500:
                last = f"{method} {key[:12]}: HTTP {resp.status}"
                continue
            return resp.status, data
        raise BackendError(last)

    def lookup(self, key: str) -> bool:
        status, _ = self.request("HEAD", key)
        if status == 200:
            return True
        if status == 404:
            return False
        raise BackendError(f"HEAD {key[:12]}: HTTP {status}")

    def store(self, key: str, body: bytes) -> str:
        status, data = self.request("PUT", key, body)
        if status in (200, 201, 204):
            return "stored"
        # The pull-request key may create but not overwrite, and OCI reports
        # a refused overwrite as NoSuchBucket (404) or a 403/409. Content
        # keys make an existing entry equivalent, so confirm with a HEAD.
        if status in (403, 404, 409, 412):
            try:
                if self.lookup(key):
                    return "present"
            except BackendError:
                pass
        snippet = data[:200].decode(errors="replace")
        raise BackendError(f"PUT {key[:12]}: HTTP {status} {snippet}")


# ── Analysis ────────────────────────────────────────────────────────────────

def analyse(clang_tidy: str, tidy_args: list[str], build: Path, tu: str) -> tuple[bool, str, float]:
    """(clean, output, seconds) from one fresh clang-tidy run."""
    t0 = time.monotonic()
    try:
        r = subprocess.run([clang_tidy, *tidy_args, "-p", str(build), "--quiet", tu],
                           capture_output=True, text=True)
    except OSError as e:
        return False, f"tidy-cache: cannot run {clang_tidy}: {e}\n", time.monotonic() - t0
    output = r.stdout + r.stderr
    clean = r.returncode == 0 and not DIAG_RE.search(output)
    if r.returncode != 0 and not output.strip():
        output = f"clang-tidy exited {r.returncode} with no output\n"
    return clean, output, time.monotonic() - t0


def read_costs(path: str | None) -> dict[str, float]:
    costs: dict[str, float] = {}
    if not path or not Path(path).exists():
        return costs
    for line in Path(path).read_text().splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and not line.startswith("#"):
            try:
                costs[parts[1].strip()] = float(parts[0])
            except ValueError:
                pass
    return costs


class Driver:
    """One run over a file list. Key function, backend and analyser are
    injectable so the tests can drive it without clang."""

    def __init__(self, *, key_fn, rel_fn, analyse_fn, backend, requested: bool,
                 jobs: int, commit: str = "", tidy_version: str = "",
                 costs: dict[str, float] | None = None, verify_sample: float = 0.0,
                 seed: int = 0, out=sys.stdout, err=sys.stderr):
        self.key_fn = key_fn
        self.rel_fn = rel_fn
        self.analyse_fn = analyse_fn
        self.backend = backend
        self.requested = requested
        self.jobs = max(1, jobs)
        self.commit = commit
        self.tidy_version = tidy_version
        self.costs = costs or {}
        self.verify_sample = verify_sample
        self.rng = random.Random(seed)
        self.out = out
        self.err = err
        self._print = threading.Lock()
        self._lock = threading.Lock()
        self._consecutive_errors = 0
        self._disabled = False
        self.stats = {
            "mode": "", "tus": 0, "skipped_no_command": 0, "hits": 0, "misses": 0,
            "stores": 0, "already_present": 0, "lookups_ok": 0, "backend_errors": 0,
            "backend_disabled": False, "key_unavailable": 0, "findings": 0,
            "verified": 0, "false_cleans": [], "key_s": 0.0, "analysis_s": 0.0,
            "saved_s_estimate": 0.0, "wall_s": 0.0, "per_tu": [],
        }

    # -- output and counters --------------------------------------------------
    def say(self, text: str, err: bool = False) -> None:
        with self._print:
            print(text, file=self.err if err else self.out, flush=True)

    def _backend_ok(self) -> None:
        with self._lock:
            self._consecutive_errors = 0

    def _backend_error(self, what: str) -> None:
        with self._lock:
            self.stats["backend_errors"] += 1
            self._consecutive_errors += 1
            trip = self._consecutive_errors >= ERROR_BUDGET and not self._disabled
            if trip:
                self._disabled = True
                self.stats["backend_disabled"] = True
        self.say(f"tidy-cache: warning: {what}", err=True)
        if trip:
            self.say(f"tidy-cache: warning: {ERROR_BUDGET} consecutive backend errors; "
                     "cache off for the rest of this run", err=True)

    def _live(self) -> bool:
        return self.backend is not None and not self._disabled

    # -- one TU ---------------------------------------------------------------
    def one(self, tu: str) -> bool:
        rel = self.rel_fn(tu)
        rec = {"tu": rel, "hit": False, "seconds": None}
        key = None
        t0 = time.monotonic()
        if self.backend is not None:
            try:
                key = self.key_fn(tu)
            except KeyUnavailable as e:
                with self._lock:
                    self.stats["key_unavailable"] += 1
                self.say(f"tidy-cache: warning: no key, analysing and not storing: {e}", err=True)
        hit = False
        if key is not None and self._live():
            try:
                hit = self.backend.lookup(key)
                self._backend_ok()
                with self._lock:
                    self.stats["lookups_ok"] += 1
            except BackendError as e:
                self._backend_error(f"lookup failed, analysing: {e}")
        key_s = time.monotonic() - t0
        ok = True
        if hit:
            rec["hit"] = True
            self.say(f"cached: {rel}")
            if self.verify_sample > 0:
                with self._lock:
                    pick = self.rng.random() < self.verify_sample
                if pick:
                    clean, output, secs = self.analyse_fn(tu)
                    with self._lock:
                        self.stats["verified"] += 1
                        if not clean:
                            self.stats["false_cleans"].append(rel)
                    if not clean:
                        self.say(f"tidy-cache: FALSE CLEAN: {rel}\n{output}")
        else:
            clean, output, secs = self.analyse_fn(tu)
            rec["seconds"] = round(secs, 2)
            self.say(f"== {rel} ({secs:.1f}s{'' if clean else ', FINDINGS'})"
                     + (f"\n{output.rstrip()}" if not clean else ""))
            ok = clean
            if clean and key is not None and self._live() and self.backend.writable:
                body = json.dumps({"v": 1, "tu": rel, "tidy": self.tidy_version,
                                   "commit": self.commit,
                                   "at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())},
                                  sort_keys=True, separators=(",", ":")).encode()
                try:
                    result = self.backend.store(key, body)
                    self._backend_ok()
                    with self._lock:
                        self.stats["stores" if result == "stored" else "already_present"] += 1
                except BackendError as e:
                    self._backend_error(f"store failed: {e}")
        with self._lock:
            s = self.stats
            s["tus"] += 1
            s["hits" if hit else "misses"] += 1
            s["key_s"] += key_s
            if rec["seconds"] is not None:
                s["analysis_s"] += rec["seconds"]
            if hit:
                s["saved_s_estimate"] += self.costs.get(rel.removeprefix("@SRC@/"), 0.0)
            if not ok:
                s["findings"] += 1
            s["per_tu"].append(rec)
        return ok

    def order(self, files: list[str]) -> list[str]:
        """Longest measured cost first, then path, so the slowest TU starts early."""
        def cost(f: str) -> float:
            return self.costs.get(self.rel_fn(f).removeprefix("@SRC@/"), 0.0)
        return sorted(files, key=lambda f: (-cost(f), f))

    def run(self, files: list[str]) -> int:
        t0 = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=self.jobs) as pool:
            results = list(pool.map(self.one, self.order(files)))
        if self.backend is not None:
            self.backend.close()
        s = self.stats
        s["wall_s"] = time.monotonic() - t0
        s["per_tu"].sort(key=lambda r: r["tu"])
        for k in ("key_s", "analysis_s", "saved_s_estimate", "wall_s"):
            s[k] = round(s[k], 1)
        if s["false_cleans"]:
            return 3
        if not all(results):
            return 1
        if self.requested and s["tus"] and not s["lookups_ok"]:
            return 2
        return 0

    def summary(self) -> str:
        s = self.stats
        return (f"tidy-cache ({s['mode']}): {s['tus']} TUs, {s['hits']} hits, {s['misses']} misses, "
                f"{s['stores']} stored, {s['already_present']} already present, "
                f"{s['backend_errors']} backend errors"
                f"{' (backend switched off)' if s['backend_disabled'] else ''}, "
                f"{s['key_unavailable']} without a key, {s['skipped_no_command']} skipped "
                f"(no compile command), {s['findings']} with findings; wall {s['wall_s']:.0f} s, "
                f"key {s['key_s']:.0f} s, analysis {s['analysis_s']:.0f} s, "
                f"about {s['saved_s_estimate']:.0f} s saved")

    def markdown(self) -> str:
        s = self.stats
        rows = [("Mode", s["mode"]), ("TUs", s["tus"]), ("Hits", s["hits"]),
                ("Misses", s["misses"]), ("Stored", s["stores"]),
                ("Already present", s["already_present"]),
                ("Backend errors", s["backend_errors"]
                 if not s["backend_disabled"] else f"{s['backend_errors']} (switched off)"),
                ("Without a key", s["key_unavailable"]),
                ("Skipped, no compile command", s["skipped_no_command"]),
                ("With findings", s["findings"]), ("Key seconds", s["key_s"]),
                ("Analysis seconds", s["analysis_s"]),
                ("Estimated seconds saved", s["saved_s_estimate"]), ("Wall seconds", s["wall_s"])]
        return ("### clang-tidy cache\n\n| | |\n|---|---|\n"
                + "".join(f"| {k} | {v} |\n" for k, v in rows) + "\n")


# ── CLI ─────────────────────────────────────────────────────────────────────

def resolve_mode(mode: str) -> str:
    if mode == "env":
        env_mode = os.environ.get("OCI_BUILD_CACHE_MODE", "")
        if env_mode not in ("read", "readwrite"):
            raise SystemExit(f"tidy-cache: --mode env but OCI_BUILD_CACHE_MODE={env_mode!r}")
        return env_mode
    if mode in ("off", "read", "readwrite") or mode.startswith("local:"):
        return mode
    raise SystemExit(f"tidy-cache: unknown mode {mode!r}")


def cmd_run(args) -> int:
    mode = resolve_mode(args.mode)
    build = Path(args.p).resolve()
    ctx = tidy_cache_key.KeyContext(build, Path(args.src), args.clang_tidy, args.tidy_arg)
    requested = mode in ("read", "readwrite")
    backend = None
    if mode.startswith("local:"):
        backend = LocalBackend(Path(mode[len("local:"):]))
    elif requested:
        try:
            backend = S3Backend.from_env(writable=(mode == "readwrite"))
        except ValueError as e:
            print(f"tidy-cache: warning: no bucket ({e}); analysing everything", file=sys.stderr)

    files, skipped = [], 0
    for f in args.files:
        if str(Path(f).resolve()) in ctx.commands:
            files.append(f)
        else:
            skipped += 1
            print(f"tidy-cache: skipped (no compile command): {ctx.rel(str(Path(f).resolve()))}")

    version = ctx.tool_part.split(b"\0", 1)[0].decode(errors="replace")
    m = re.search(r"version (\S+)", version)
    driver = Driver(
        key_fn=lambda tu: ctx.key(tu)[0],
        rel_fn=lambda tu: ctx.rel(str(Path(tu).resolve())),
        analyse_fn=lambda tu: analyse(args.clang_tidy, args.tidy_arg, build, tu),
        backend=backend, requested=requested, jobs=args.jobs,
        commit=os.environ.get("GITHUB_SHA") or args.commit,
        tidy_version=m.group(1) if m else "", costs=read_costs(args.costs),
        verify_sample=args.verify_sample, seed=args.seed)
    driver.stats["mode"] = mode
    driver.stats["skipped_no_command"] = skipped
    rc = driver.run(files)
    print(driver.summary())
    stats_path = Path(args.stats) if args.stats else build / "tidy-cache-stats.json"
    stats_path.write_text(json.dumps(driver.stats, indent=1, sort_keys=True) + "\n")
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write(driver.markdown())
    if rc == 2:
        print("tidy-cache: error: tidy cache requested but made no successful lookups "
              "(silent no-op)", file=sys.stderr)
    elif rc == 3:
        print(f"tidy-cache: error: {len(driver.stats['false_cleans'])} false cleans",
              file=sys.stderr)
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("-p", required=True, help="build directory with compile_commands.json")
    r.add_argument("--src", default=".", help="workspace root (default: cwd)")
    r.add_argument("--mode", default="off")
    r.add_argument("--clang-tidy", default="clang-tidy-18")
    r.add_argument("--tidy-arg", action="append", default=[],
                   help="extra argument passed to clang-tidy (keyed)")
    r.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    r.add_argument("--costs", help="configs/tidy-file-costs.txt: start long TUs first")
    r.add_argument("--stats", help="JSON counters (default BUILD/tidy-cache-stats.json)")
    r.add_argument("--verify-sample", type=float, default=0.0,
                   help="re-analyse this fraction of hits and report false cleans")
    r.add_argument("--seed", type=int, default=0)
    r.add_argument("--commit", default="")
    r.add_argument("--files-from", help="read TU paths from this file, one per line")
    r.add_argument("files", nargs="*")
    args = ap.parse_args()
    if args.files_from:
        args.files += [l.strip() for l in Path(args.files_from).read_text().splitlines()
                       if l.strip()]
    return cmd_run(args)


if __name__ == "__main__":
    sys.exit(main())
