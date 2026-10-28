# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Tests for chaos_http.py against a stand-in chaos_ctl.

Run with: python3 -m unittest discover -s docker/cairn/tool_server -p '*_test.py'
"""

import http.client
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import chaos_http  # noqa: E402

# Lists three tools; fault.kill echoes its arguments, check.run exits 1 with a
# tool error, sleepy overruns, and anything else is chaos_ctl's usage error.
# Every call is appended to $FAKE_LOG so ordering can be checked.
FAKE_CTL = r"""#!/usr/bin/env python3
import json, os, sys, time
cmd = sys.argv[1]
if cmd == "tools":
    print(json.dumps([{"name": n, "summary": "s"} for n in ("fault.kill", "check.run", "sleepy")]))
    sys.exit(0)
args = json.loads(sys.stdin.read()) if sys.argv[2:] == ["-"] else {}
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(f"start {cmd} {json.dumps(args)}\n")
if cmd == "sleepy":
    time.sleep(float(args.get("s", 5)))
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(f"end {cmd}\n")
if cmd == "sleepy":
    print(json.dumps({"status": "ok"}))
    sys.exit(0)
if cmd == "fault.kill":
    print("noise on stdout first")
    print(json.dumps({"status": "ok", "tool": cmd, "args": args}))
    sys.exit(0)
if cmd == "check.run":
    print(json.dumps({"status": "error", "error": "no run"}))
    sys.exit(1)
print(json.dumps({"status": "usage", "error": "bad"}))
sys.exit(2)
"""


class ChaosHttpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        ctl = os.path.join(cls.tmp.name, "chaos_ctl")
        with open(ctl, "w") as f:
            f.write(FAKE_CTL)
        os.chmod(ctl, os.stat(ctl).st_mode | stat.S_IXUSR)
        cls.log = os.path.join(cls.tmp.name, "calls.log")
        os.environ["FAKE_LOG"] = cls.log
        cls.server = chaos_http.Server(("127.0.0.1", 0), ctl, timeout=1)
        cls.port = cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.tmp.cleanup()

    def setUp(self):
        open(self.log, "w").close()

    def request(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        conn.request(method, path, body=body, headers=headers or {})
        resp = conn.getresponse()
        text = resp.read().decode()
        conn.close()
        return resp, json.loads(text)

    def test_lists_tools(self):
        resp, body = self.request("GET", "/tools")
        self.assertEqual(resp.status, 200)
        self.assertEqual([t["name"] for t in body], ["fault.kill", "check.run", "sleepy"])

    def test_calls_a_tool_with_its_arguments(self):
        resp, body = self.request("POST", "/tool/fault.kill", body='{"node": 2}')
        self.assertEqual(resp.status, 200)
        self.assertEqual(resp.getheader("X-Chaos-Exit"), "0")
        self.assertEqual(body, {"status": "ok", "tool": "fault.kill", "args": {"node": 2}})

    def test_empty_body_is_no_arguments(self):
        resp, body = self.request("POST", "/tool/fault.kill")
        self.assertEqual(resp.status, 200)
        self.assertEqual(body["args"], {})

    def test_tool_error_is_still_200_with_its_exit_status(self):
        resp, body = self.request("POST", "/tool/check.run", body="{}")
        self.assertEqual(resp.status, 200)
        self.assertEqual(resp.getheader("X-Chaos-Exit"), "1")
        self.assertEqual(body["status"], "error")

    def test_refuses_what_chaos_ctl_tools_does_not_list(self):
        for name in ("init", "replay", "minimize", "tools", "../chaos_ctl", "fault.kill/x"):
            resp, body = self.request("POST", "/tool/" + name, body="{}")
            self.assertEqual(resp.status, 404, name)
        with open(self.log) as f:
            self.assertEqual(f.read(), "", "nothing reached chaos_ctl")

    def test_refuses_arguments_that_are_not_an_object(self):
        for raw in ("[1]", '"x"', "{not json"):
            resp, _ = self.request("POST", "/tool/fault.kill", body=raw)
            self.assertEqual(resp.status, 400, raw)

    def test_refuses_an_oversized_body(self):
        resp, _ = self.request("POST", "/tool/fault.kill", body="x" * (chaos_http.MAX_BODY + 1))
        self.assertEqual(resp.status, 413)

    def test_overrun_is_killed_and_answered_504(self):
        resp, body = self.request("POST", "/tool/sleepy", body='{"s": 5}')
        self.assertEqual(resp.status, 504)
        self.assertIn("overran", body["error"])

    def test_calls_run_one_at_a_time(self):
        results = []

        def call():
            results.append(self.request("POST", "/tool/sleepy", body='{"s": 0.3}')[0].status)

        threads = [threading.Thread(target=call) for _ in range(3)]
        for t in threads:
            t.start()
            time.sleep(0.05)
        for t in threads:
            t.join()
        self.assertEqual(results, [200, 200, 200])
        with open(self.log) as f:
            lines = [line.split()[0] for line in f]
        self.assertEqual(lines, ["start", "end"] * 3)

    def test_unknown_paths(self):
        self.assertEqual(self.request("GET", "/tool/fault.kill")[0].status, 404)
        self.assertEqual(self.request("POST", "/tools")[0].status, 404)
        self.assertEqual(self.request("GET", "/health")[0].status, 200)

    @unittest.skipUnless(shutil.which("node"), "the worker's chaos client needs node")
    def test_worker_client_end_to_end(self):
        client = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "worker", "chaos")
        env = dict(os.environ, CHAOS_URL=f"http://127.0.0.1:{self.port}")

        def chaos(*args, stdin=None):
            return subprocess.run(["node", client, *args], input=stdin, capture_output=True, text=True, env=env)

        out = chaos("tools")
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(len(json.loads(out.stdout)), 3)
        out = chaos("fault.kill", '{"node":3}')
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(json.loads(out.stdout)["args"], {"node": 3})
        out = chaos("fault.kill", "-", stdin='{"node":1}')
        self.assertEqual(json.loads(out.stdout)["args"], {"node": 1})
        self.assertEqual(chaos("check.run").returncode, 1)
        self.assertEqual(chaos("init").returncode, 2)
        self.assertEqual(chaos("fault.kill", "{oops").returncode, 2)
        self.assertEqual(chaos().returncode, 2)
        env["CHAOS_URL"] = "http://127.0.0.1:1"
        out = chaos("tools")
        self.assertEqual(out.returncode, 2)
        self.assertIn("cannot reach the tool server", out.stderr)


if __name__ == "__main__":
    unittest.main()
