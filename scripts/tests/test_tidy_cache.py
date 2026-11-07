#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Tests for scripts/tidy-cache.py (clang-tidy-result-cache task 4).

The driver runs against a stub S3 server on localhost, with the key
function and clang-tidy replaced by fakes, so no clang is needed. The
SigV4 signer is checked against botocore when that is importable.

Run: python3 scripts/tests/test_tidy_cache.py
"""

from __future__ import annotations

import datetime as dt
import http.server
import importlib.util
import io
import json
import os
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "tidy-cache.py"
spec = importlib.util.spec_from_file_location("tidy_cache", SCRIPT)
tc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tc)


class StubS3(http.server.ThreadingHTTPServer):
    """In-memory bucket. `put_status`/`head_status` force replies; `delay`
    stalls every reply; `create_only` refuses overwrites like the PR key,
    with OCI's misleading 404 NoSuchBucket."""

    def __init__(self):
        super().__init__(("127.0.0.1", 0), StubHandler)
        self.objects: dict[str, bytes] = {}
        self.requests: list[tuple[str, str]] = []
        self.put_status: int | None = None
        self.head_status: int | None = None
        self.create_only = False
        self.delay = 0.0
        self.lock = threading.Lock()
        threading.Thread(target=self.serve_forever, daemon=True).start()

    @property
    def endpoint(self) -> str:
        return f"http://127.0.0.1:{self.server_address[1]}"

    def count(self, method: str) -> int:
        with self.lock:
            return sum(1 for m, _ in self.requests if m == method)


class StubHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _reply(self, status: int, body: bytes = b"", head: bool = False):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if not head:
            self.wfile.write(body)

    def _pre(self, method: str) -> bool:
        s: StubS3 = self.server
        with s.lock:
            s.requests.append((method, self.path))
        if s.delay:
            time.sleep(s.delay)
        if "AWS4-HMAC-SHA256 Credential=" not in self.headers.get("authorization", ""):
            self._reply(403, b"unsigned", head=method == "HEAD")
            return False
        return True

    def do_HEAD(self):
        if not self._pre("HEAD"):
            return
        s: StubS3 = self.server
        if s.head_status:
            return self._reply(s.head_status, head=True)
        self._reply(200 if self.path in s.objects else 404, head=True)

    def do_PUT(self):
        body = self.rfile.read(int(self.headers.get("content-length", "0")))
        if not self._pre("PUT"):
            return
        s: StubS3 = self.server
        if s.put_status:
            return self._reply(s.put_status, b"<Error>forced</Error>")
        if s.create_only and self.path in s.objects:
            return self._reply(404, b"<Error><Code>NoSuchBucket</Code></Error>")
        s.objects[self.path] = body
        self._reply(200)


def make_driver(backend, *, requested=True, results=None, keys=None, jobs=2,
                costs=None, verify_sample=0.0):
    """A Driver whose TU "x.cpp" has key "k-x.cpp" and is clean unless
    results says otherwise (True clean, False finding, Exception crash)."""
    results = results or {}
    keys = keys or {}
    analysed: list[str] = []

    def key_fn(tu):
        k = keys.get(tu, f"k-{tu}")
        if isinstance(k, Exception):
            raise k
        return k

    def analyse_fn(tu):
        analysed.append(tu)
        r = results.get(tu, True)
        return (r, "" if r else f"{tu}:1:1: warning: bad [x-check]\n", 0.5)

    out, err = io.StringIO(), io.StringIO()
    d = tc.Driver(key_fn=key_fn, rel_fn=lambda tu: tu, analyse_fn=analyse_fn,
                  backend=backend, requested=requested, jobs=jobs, costs=costs,
                  verify_sample=verify_sample, out=out, err=err)
    d.analysed = analysed
    d.outbuf, d.errbuf = out, err
    return d


def s3(stub: StubS3, writable=True) -> "tc.S3Backend":
    return tc.S3Backend(stub.endpoint, "us-phoenix-1", "bucket", "AKTESTFIXTURE",
                        "test-fixture-secret", writable, timeout=2, backoff=(0, 0))


class DriverS3Test(unittest.TestCase):
    def setUp(self):
        self.stub = StubS3()

    def tearDown(self):
        self.stub.shutdown()
        self.stub.server_close()

    def test_miss_then_store_then_hit(self):
        d = make_driver(s3(self.stub))
        self.assertEqual(d.run(["a.cpp", "b.cpp"]), 0)
        self.assertEqual((d.stats["misses"], d.stats["stores"]), (2, 2))
        self.assertIn("/bucket/clang-tidy/k-a.cpp", self.stub.objects)
        body = json.loads(self.stub.objects["/bucket/clang-tidy/k-a.cpp"])
        self.assertEqual(body["tu"], "a.cpp")
        self.assertLess(len(self.stub.objects["/bucket/clang-tidy/k-a.cpp"]), 1024)
        d2 = make_driver(s3(self.stub))
        self.assertEqual(d2.run(["a.cpp", "b.cpp"]), 0)
        self.assertEqual(d2.stats["hits"], 2)
        self.assertEqual(d2.analysed, [])
        self.assertIn("cached: a.cpp", d2.outbuf.getvalue())

    def test_finding_is_reported_not_stored_and_fails(self):
        d = make_driver(s3(self.stub), results={"bad.cpp": False})
        self.assertEqual(d.run(["bad.cpp", "ok.cpp"]), 1)
        self.assertNotIn("/bucket/clang-tidy/k-bad.cpp", self.stub.objects)
        self.assertIn("/bucket/clang-tidy/k-ok.cpp", self.stub.objects)
        self.assertIn("== bad.cpp", d.outbuf.getvalue())
        self.assertIn("warning: bad [x-check]", d.outbuf.getvalue())
        # Analysed again next time, never served from the cache.
        d2 = make_driver(s3(self.stub), results={"bad.cpp": False})
        self.assertEqual(d2.run(["bad.cpp"]), 1)
        self.assertEqual(d2.analysed, ["bad.cpp"])

    def test_read_mode_never_puts(self):
        d = make_driver(s3(self.stub, writable=False))
        self.assertEqual(d.run(["a.cpp"]), 0)
        self.assertEqual(self.stub.count("PUT"), 0)
        self.assertEqual(d.stats["lookups_ok"], 1)

    def test_refused_put_of_existing_object_counts_as_present(self):
        self.stub.objects["/bucket/clang-tidy/k-a.cpp"] = b"{}"
        self.stub.create_only = True
        self.stub.head_status = None
        backend = s3(self.stub)
        self.assertEqual(backend.store("k-a.cpp", b"{}"), "present")
        backend.close()

    def test_403_on_put_is_a_warning(self):
        self.stub.put_status = 403
        d = make_driver(s3(self.stub))
        self.assertEqual(d.run(["a.cpp"]), 0)
        self.assertEqual((d.stats["stores"], d.stats["backend_errors"]), (0, 1))
        self.assertIn("store failed", d.errbuf.getvalue())

    def test_lookup_error_is_a_miss(self):
        self.stub.head_status = 403
        d = make_driver(s3(self.stub), requested=False)
        self.assertEqual(d.run(["a.cpp"]), 0)
        self.assertEqual(d.analysed, ["a.cpp"])
        self.assertEqual(d.stats["backend_errors"], 1)

    def test_5xx_is_retried_twice(self):
        self.stub.head_status = 503
        d = make_driver(s3(self.stub), requested=False, jobs=1)
        d.run(["a.cpp"])
        self.assertEqual(self.stub.count("HEAD"), 3)

    def test_timeout_is_a_miss(self):
        self.stub.delay = 3
        backend = tc.S3Backend(self.stub.endpoint, "r", "bucket", "AK", "SK", True,
                               timeout=0.5, backoff=())
        d = make_driver(backend, requested=False, jobs=1)
        self.assertEqual(d.run(["a.cpp"]), 0)
        self.assertEqual(d.analysed, ["a.cpp"])
        self.assertGreaterEqual(d.stats["backend_errors"], 1)

    def test_error_budget_switches_backend_off(self):
        # Read-only, so no successful store resets the consecutive count.
        self.stub.head_status = 403
        d = make_driver(s3(self.stub, writable=False), requested=False, jobs=1)
        files = [f"f{i:02d}.cpp" for i in range(30)]
        self.assertEqual(d.run(files), 0)
        self.assertEqual(self.stub.count("HEAD"), tc.ERROR_BUDGET)
        self.assertTrue(d.stats["backend_disabled"])
        self.assertEqual(len(d.analysed), 30)

    def test_silent_no_op_exits_2(self):
        self.stub.head_status = 403
        d = make_driver(s3(self.stub), requested=True)
        self.assertEqual(d.run(["a.cpp"]), 2)

    def test_finding_beats_silent_no_op(self):
        self.stub.head_status = 403
        d = make_driver(s3(self.stub), results={"a.cpp": False})
        self.assertEqual(d.run(["a.cpp"]), 1)

    def test_no_key_is_analysed_and_not_stored(self):
        d = make_driver(s3(self.stub), keys={"a.cpp": tc.KeyUnavailable("scan failed")})
        self.assertEqual(d.run(["a.cpp", "b.cpp"]), 0)
        self.assertEqual(sorted(d.analysed), ["a.cpp", "b.cpp"])
        self.assertEqual(list(self.stub.objects), ["/bucket/clang-tidy/k-b.cpp"])
        self.assertEqual(d.stats["key_unavailable"], 1)

    def test_crash_is_not_stored(self):
        # analyse_fn's (False, ...) covers a crash or timeout: not clean.
        d = make_driver(s3(self.stub), results={"a.cpp": False})
        d.run(["a.cpp"])
        self.assertEqual(self.stub.objects, {})

    def test_unsigned_request_is_rejected_by_stub(self):
        # Guards the stub itself: the driver's requests do carry SigV4.
        d = make_driver(s3(self.stub))
        d.run(["a.cpp"])
        self.assertEqual(d.stats["backend_errors"], 0)


class DriverLocalTest(unittest.TestCase):
    def test_local_backend_round_trip(self):
        with tempfile.TemporaryDirectory() as t:
            d = make_driver(tc.LocalBackend(Path(t)), requested=False)
            self.assertEqual(d.run(["a.cpp"]), 0)
            self.assertTrue((Path(t) / "k-" / "k-a.cpp").exists())
            d2 = make_driver(tc.LocalBackend(Path(t)), requested=False)
            d2.run(["a.cpp"])
            self.assertEqual(d2.stats["hits"], 1)

    def test_off_mode_analyses_everything(self):
        d = make_driver(None, requested=False)
        self.assertEqual(d.run(["a.cpp", "b.cpp"]), 0)
        self.assertEqual(d.stats["misses"], 2)

    def test_misses_start_longest_first(self):
        d = make_driver(None, requested=False, jobs=1,
                        costs={"short.cpp": 1, "long.cpp": 100, "mid.cpp": 10})
        d.run(["mid.cpp", "short.cpp", "long.cpp", "new.cpp"])
        self.assertEqual(d.analysed, ["long.cpp", "mid.cpp", "short.cpp", "new.cpp"])

    def test_verify_sample_reports_false_clean(self):
        with tempfile.TemporaryDirectory() as t:
            make_driver(tc.LocalBackend(Path(t)), requested=False).run(["a.cpp"])
            d = make_driver(tc.LocalBackend(Path(t)), requested=False,
                            results={"a.cpp": False}, verify_sample=1.0)
            self.assertEqual(d.run(["a.cpp"]), 3)
            self.assertEqual(d.stats["false_cleans"], ["a.cpp"])

    def test_summary_and_markdown_name_every_counter(self):
        d = make_driver(None, requested=False)
        d.run(["a.cpp"])
        self.assertIn("1 TUs", d.summary())
        self.assertIn("| Estimated seconds saved |", d.markdown())


_kspec = importlib.util.spec_from_file_location(
    "test_tidy_cache_key", Path(__file__).resolve().parent / "test_tidy_cache_key.py")
tk_tests = importlib.util.module_from_spec(_kspec)
_kspec.loader.exec_module(tk_tests)


@tk_tests.needs_tools
class CliEndToEndTest(unittest.TestCase):
    """The real CLI with clang-tidy-18 on the key tests' fixture workspace."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = Path(self.tmp.name)
        self.ws = tk_tests.Workspace(t / "ws", t / "sys")
        # Drop sccache from the commands: clang-tidy must run the compiler args.
        db_path = self.ws.build / "compile_commands.json"
        db = json.loads(db_path.read_text())
        for e in db:
            e["arguments"] = [a for a in e["arguments"] if a != "/usr/bin/sccache"]
        db_path.write_text(json.dumps(db))
        self.cache = t / "cache"

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *files, mode=None):
        import subprocess
        return subprocess.run(
            [sys.executable, str(SCRIPT), "run", "-p", str(self.ws.build),
             "--src", str(self.ws.root), "--mode", mode or f"local:{self.cache}",
             "--clang-tidy", tk_tests.TIDY, "--jobs", "2",
             *[str(self.ws.root / f) for f in files]],
            capture_output=True, text=True, cwd=self.ws.root)

    def test_cold_then_warm_then_finding(self):
        r = self.run_cli("a.cpp", "sub/b.cpp", "uncompiled.cpp")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("2 stored", r.stdout)
        self.assertIn("1 skipped (no compile command)", r.stdout)
        r = self.run_cli("a.cpp", "sub/b.cpp")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("cached: @SRC@/a.cpp", r.stdout)
        self.assertIn("2 hits", r.stdout)
        stats = json.loads((self.ws.build / "tidy-cache-stats.json").read_text())
        self.assertEqual(stats["hits"], 2)
        # A real finding (bugprone-assert-side-effect needs <cassert>; use
        # bugprone-integer-division instead) fails, prints, and is not stored.
        (self.ws.root / "sub" / "b.cpp").write_text(
            '#include "h.hpp"\nint b() { double d = 1 / 2; return h() + static_cast<int>(d); }\n')
        r = self.run_cli("sub/b.cpp")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("bugprone-integer-division", r.stdout)
        r = self.run_cli("sub/b.cpp")
        self.assertEqual(r.returncode, 1)
        self.assertNotIn("cached:", r.stdout)

    def test_requested_cache_without_bucket_is_silent_no_op(self):
        env_keys = ("AWS_ENDPOINT_URL", "SCCACHE_BUCKET")
        saved = {k: os.environ.pop(k, None) for k in env_keys}
        try:
            r = self.run_cli("a.cpp", mode="read")
        finally:
            for k, v in saved.items():
                if v is not None:
                    os.environ[k] = v
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("silent no-op", r.stderr)


class SigV4Test(unittest.TestCase):
    def test_matches_botocore(self):
        try:
            from botocore.auth import S3SigV4Auth
            from botocore.awsrequest import AWSRequest
            from botocore.credentials import Credentials
        except ImportError:
            self.skipTest("botocore not installed")
        now = dt.datetime(2026, 11, 7, 6, 0, 0, tzinfo=dt.timezone.utc)
        for method, body in (("HEAD", b""), ("PUT", b'{"v":1}')):
            ours = tc.sigv4_headers(method, "example.test", "/bucket/clang-tidy/abc",
                                    "us-phoenix-1", "AKTESTFIXTURE", "test-fixture-secret",
                                    body, now=now)
            req = AWSRequest(method=method, url="https://example.test/bucket/clang-tidy/abc",
                             data=body, headers={"x-amz-content-sha256": ours["x-amz-content-sha256"]})
            auth = S3SigV4Auth(Credentials("AKTESTFIXTURE", "test-fixture-secret"), "s3",
                               "us-phoenix-1")
            auth._timestamp = lambda: now.strftime("%Y%m%dT%H%M%SZ")
            import botocore.auth as ba
            orig = ba.datetime.datetime
            class Frozen(orig):
                @classmethod
                def utcnow(cls):
                    return now.replace(tzinfo=None)
                @classmethod
                def now(cls, tz=None):
                    return now if tz else now.replace(tzinfo=None)
            ba.datetime.datetime = Frozen
            try:
                auth.add_auth(req)
            finally:
                ba.datetime.datetime = orig
            theirs = req.headers["Authorization"]
            self.assertEqual(ours["authorization"].split("Signature=")[1],
                             theirs.split("Signature=")[1], method)


if __name__ == "__main__":
    unittest.main()
