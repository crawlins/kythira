#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""The cairn tool server's front for agents (.kiro/specs/cairn-chaos-testing/
design §3.1, task 10).

Cairn v0.2.1 has no MCP: a worker's tools are its agent CLI's own, and the
agent reaches anything else by running a command (task 1a). So the tool
server answers plain HTTP on cairn_net, and the worker image carries a
`chaos` command that calls it (docker/cairn/worker/chaos). This file maps
those calls onto chaos_ctl and does nothing else:

  GET  /tools         chaos_ctl tools, as it printed it
  POST /tool/<name>   chaos_ctl <name> -, with the body (a JSON object) on
                      stdin; replies with chaos_ctl's JSON

Only names that `chaos_ctl tools` lists are callable, so init, replay and
minimize are not: the run is set up from the host (scripts/cairn/
start-run.sh) and replayed there. Calls run one at a time, in arrival
order, because they share one cluster and one schedule log, and each call's
raft_safety check (design §3.3) must see the state the call left.

The reply's status says whether the call happened, not what it found:
chaos_ctl exit 0 or 1 (the tool ran, or reported an error the agent should
read) is 200; 2 (bad usage) is 400. X-Chaos-Exit carries the exit status
itself. An unknown tool is 404, a body that is not a JSON object is 400, an
oversized one 413, and a call that overruns CHAOS_HTTP_TIMEOUT seconds is
killed and answered 504.

Environment: CHAOS_HTTP_PORT (default 7700), CHAOS_HTTP_TIMEOUT (default
300), CHAOS_CTL (default chaos_ctl on PATH). chaos_ctl itself reads
KYTHIRA_CHAOS_RUN_DIR, set by the tool server's Dockerfile.
"""

import http.server
import json
import os
import subprocess
import sys
import threading

MAX_BODY = 64 * 1024


def load_tools(ctl):
    """Return chaos_ctl's tool list (a JSON array) and the set of names."""
    out = subprocess.run([ctl, "tools"], capture_output=True, text=True, check=True, timeout=60)
    listing = json.loads(out.stdout.strip().splitlines()[-1])
    return listing, frozenset(t["name"] for t in listing)


def last_json_line(text):
    """chaos_ctl prints one JSON object; return its last non-empty line."""
    for line in reversed(text.splitlines()):
        if line.strip():
            return line.strip()
    return ""


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, ctl, timeout):
        self.ctl = ctl
        self.call_timeout = timeout
        self.listing, self.names = load_tools(ctl)
        self.call_lock = threading.Lock()
        super().__init__(address, Handler)

    def call(self, name, body):
        """Run one tool; returns (http status, exit status, reply text)."""
        with self.call_lock:
            try:
                out = subprocess.run(
                    [self.ctl, name, "-"],
                    input=body,
                    capture_output=True,
                    text=True,
                    timeout=self.call_timeout,
                )
            except subprocess.TimeoutExpired:
                err = {"status": "failed", "error": f"{name} overran {self.call_timeout}s and was killed"}
                return 504, None, json.dumps(err)
        reply = last_json_line(out.stdout)
        if not reply:
            reply = json.dumps({"status": "failed", "error": out.stderr.strip()[-2000:] or "no output"})
        return (400 if out.returncode == 2 else 200), out.returncode, reply


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "kythira-chaos-http"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("chaos_http: " + (fmt % args) + "\n")

    def reply(self, status, text, exit_status=None):
        data = (text + "\n").encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        if exit_status is not None:
            self.send_header("X-Chaos-Exit", str(exit_status))
        self.end_headers()
        self.wfile.write(data)

    def error(self, status, message):
        self.reply(status, json.dumps({"status": "usage", "error": message}))

    def do_GET(self):
        if self.path == "/tools":
            self.reply(200, json.dumps(self.server.listing))
        elif self.path == "/health":
            self.reply(200, json.dumps({"ok": True}))
        else:
            self.error(404, "try GET /tools or POST /tool/<name>")

    def do_POST(self):
        prefix = "/tool/"
        if not self.path.startswith(prefix):
            self.error(404, "try GET /tools or POST /tool/<name>")
            return
        name = self.path[len(prefix) :]
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            self.error(400, "bad Content-Length")
            return
        if length > MAX_BODY:
            # Read what was sent, up to a bound, before answering: hanging
            # up on an unread body resets the connection, and the client
            # may lose the reply.
            if length <= 16 * MAX_BODY:
                self.rfile.read(length)
            self.close_connection = True
            self.error(413, f"arguments over {MAX_BODY} bytes")
            return
        raw = self.rfile.read(length) if length > 0 else b""
        if name not in self.server.names:
            self.error(404, f"unknown tool '{name}'; GET /tools lists them")
            return
        try:
            args = json.loads(raw or b"{}")
        except ValueError as e:
            self.error(400, f"arguments are not JSON: {e}")
            return
        if not isinstance(args, dict):
            self.error(400, "arguments must be a JSON object")
            return
        status, exit_status, text = self.server.call(name, json.dumps(args))
        self.reply(status, text, exit_status)


def main():
    port = int(os.environ.get("CHAOS_HTTP_PORT", "7700"))
    timeout = int(os.environ.get("CHAOS_HTTP_TIMEOUT", "300"))
    ctl = os.environ.get("CHAOS_CTL", "chaos_ctl")
    server = Server(("0.0.0.0", port), ctl, timeout)
    sys.stderr.write(f"chaos_http: {len(server.names)} tools on :{port}\n")
    server.serve_forever()


if __name__ == "__main__":
    main()
