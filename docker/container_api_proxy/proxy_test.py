#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Tests for proxy.py, the container API filter (cairn-chaos-testing
task 8.1). No container runtime needed: the proxy forwards to a fake API
server on a unix socket, which records what reached it.

Run: python3 -m unittest discover -s docker/container_api_proxy -p '*_test.py'
"""

import http.client
import http.server
import json
import os
import socket
import socketserver
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import proxy  # noqa: E402

LABEL = "kythira.cairn.run=r1"
CHAOS = proxy.Config(mode="chaos_ctl", label=LABEL)
QUORUM = proxy.Config(cluster="c1", image="kythira-chaos-node:dev", network="net1")

NODE_ID = "a" * 64
OTHER_ID = "b" * 64
EXEC_ID = "c" * 64


def exec_body(cmd, **over):
    """The body the docker CLI writes for `docker exec <c> <cmd...>`."""
    body = {"User": "", "Privileged": False, "Tty": False, "AttachStdin": False,
            "AttachStderr": True, "AttachStdout": True, "Detach": False, "DetachKeys": "",
            "Env": None, "WorkingDir": "", "Cmd": cmd}
    body.update(over)
    return json.dumps(body).encode()


# Every vector tests/docker_chaos/os_faults.hpp writes after `<rt> exec <c>`,
# with the values its callers pass.
FAULT_CMDS = [
    ["tc", "qdisc", "replace", "dev", "eth0", "root", "netem", "loss", "0%", "delay", "0ms",
     "corrupt", "0%"],
    ["tc", "qdisc", "replace", "dev", "eth0", "root", "netem", "loss", "30%", "delay", "100ms",
     "corrupt", "0.5%"],
    ["tc", "qdisc", "replace", "dev", "eth0", "root", "netem", "delay", "40ms"],
    ["tc", "qdisc", "replace", "dev", "eth0", "root", "netem", "delay", "40ms", "10ms"],
    ["tc", "qdisc", "replace", "dev", "eth0", "root", "netem", "loss", "0.2%"],
    ["tc", "qdisc", "replace", "dev", "eth0", "root", "netem", "loss", "1e-05%"],
    ["tc", "qdisc", "del", "dev", "eth0", "root"],
    ["iptables", "-A", "CHAOS", "-s", "10.89.0.3", "-j", "DROP"],
    ["iptables", "-A", "OUTPUT", "-d", "172.18.0.4", "-j", "DROP"],
    ["iptables", "-F", "CHAOS"],
    ["iptables", "-F", "OUTPUT"],
]

REFUSED_CMDS = [
    ["sh", "-c", "tc qdisc del dev eth0 root"],
    ["tc", "qdisc", "del", "dev", "lo", "root"],
    ["tc", "qdisc", "replace", "dev", "eth0", "root", "netem"],
    ["tc", "qdisc", "replace", "dev", "eth0", "root", "netem", "loss", "30%", "loss", "1%"],
    ["tc", "qdisc", "replace", "dev", "eth0", "root", "netem", "rate", "1kbit"],
    ["tc", "qdisc", "replace", "dev", "eth0", "root", "netem", "loss", "30"],
    ["tc", "qdisc", "replace", "dev", "eth0", "root", "netem", "delay", "40ms", "10ms", "5ms"],
    ["tc", "qdisc", "replace", "dev", "eth0", "root", "netem", "delay", "$(id)"],
    ["tc", "filter", "add", "dev", "eth0"],
    ["iptables", "-F"],
    ["iptables", "-F", "INPUT"],
    ["iptables", "-A", "CHAOS", "-s", "10.0.0.0/8", "-j", "DROP"],
    ["iptables", "-A", "CHAOS", "-s", "node2", "-j", "DROP"],
    ["iptables", "-A", "CHAOS", "-s", "10.89.0.3", "-j", "ACCEPT"],
    ["iptables", "-A", "INPUT", "-s", "10.89.0.3", "-j", "DROP"],
    ["iptables", "-A", "CHAOS", "-d", "10.89.0.3", "-j", "DROP"],
    ["iptables", "-I", "CHAOS", "-s", "10.89.0.3", "-j", "DROP"],
    ["iptables", "-A", "CHAOS", "-s", "10.89.0.3", "-j", "DROP", "-m", "comment"],
    ["ip6tables", "-F", "CHAOS"],
    ["cat", "/etc/shadow"],
    [],
    "tc qdisc del dev eth0 root",
    ["tc", "qdisc", "del", "dev", "eth0", 1],
]


class CheckChaosCtlTest(unittest.TestCase):
    """chaos_ctl mode, as check() decides it before anything goes upstream."""

    def allowed(self, method, target, body=b"", known=frozenset()):
        return proxy.check(CHAOS, method, target, body, known)

    def refused(self, method, target, body=b"", known=frozenset()):
        with self.assertRaises(proxy.Refused, msg=f"{method} {target} {body!r}"):
            proxy.check(CHAOS, method, target, body, known)

    def test_lifecycle_calls_are_scoped_to_their_container(self):
        for target in ("/containers/chaos_node_1/kill", "/v1.43/containers/chaos_node_1/kill",
                       "/containers/chaos_node_1/kill?signal=KILL",
                       "/containers/chaos_node_1/kill?signal=SIGTERM",
                       "/containers/chaos_node_1/stop", "/containers/chaos_node_1/stop?t=10",
                       "/containers/chaos_node_1/start", "/containers/chaos_node_1/pause",
                       "/containers/chaos_node_1/unpause"):
            fwd = self.allowed("POST", target)
            self.assertEqual(fwd.container, "chaos_node_1", target)
            self.assertFalse(fwd.hijack)
        fwd = self.allowed("GET", "/v1.51/containers/chaos_node_1/json")
        self.assertEqual(fwd.container, "chaos_node_1")

    def test_ping_is_forwarded_unscoped(self):
        for method in ("GET", "HEAD"):
            fwd = self.allowed(method, "/_ping")
            self.assertIsNone(fwd.container)

    def test_every_os_faults_command_may_be_exec_created(self):
        for cmd in FAULT_CMDS:
            fwd = self.allowed("POST", "/v1.51/containers/chaos_node_2/exec", exec_body(cmd))
            self.assertEqual(fwd.container, "chaos_node_2")
            self.assertTrue(fwd.records_exec)
        # Podman's compat API and older CLIs omit keys or add ConsoleSize.
        self.allowed("POST", "/containers/chaos_node_2/exec",
                     json.dumps({"Cmd": FAULT_CMDS[6], "AttachStdout": True}).encode())
        self.allowed("POST", "/containers/chaos_node_2/exec",
                     exec_body(FAULT_CMDS[6], ConsoleSize=[24, 80]))

    def test_any_other_command_is_refused(self):
        for cmd in REFUSED_CMDS:
            self.refused("POST", "/containers/chaos_node_2/exec", exec_body(cmd))

    def test_exec_may_not_change_who_or_how_it_runs(self):
        cmd = FAULT_CMDS[6]
        for over in ({"Privileged": True}, {"User": "root"}, {"User": "0"},
                     {"Env": ["LD_PRELOAD=/tmp/x.so"]}, {"WorkingDir": "/tmp"}, {"Tty": True},
                     {"AttachStdin": True}, {"Detach": True}, {"DetachKeys": "ctrl-p"},
                     {"Privileged": 1}, {"Privileged": None}, {"AttachStdout": "yes"},
                     {"ConsoleSize": [1, 2, 3]}, {"HostConfig": {}}, {"Mounts": []},
                     {"Capabilities": ["CAP_SYS_ADMIN"]}):
            self.refused("POST", "/containers/chaos_node_2/exec", exec_body(cmd, **over))
        for body in (b"", b"null", b"[]", b"{", json.dumps({"User": ""}).encode()):
            self.refused("POST", "/containers/chaos_node_2/exec", body)
        self.refused("POST", "/containers/chaos_node_2/exec?x=1", exec_body(cmd))

    def test_exec_start_and_inspect_need_an_id_this_proxy_created(self):
        known = frozenset({EXEC_ID})
        fwd = self.allowed("POST", f"/v1.51/exec/{EXEC_ID}/start",
                           b'{"Detach":false,"Tty":false}', known)
        self.assertTrue(fwd.hijack)
        self.assertEqual(fwd.exec_id, EXEC_ID)
        self.allowed("POST", f"/exec/{EXEC_ID}/start", b"", known)
        self.allowed("POST", f"/exec/{EXEC_ID}/start", b'{"ConsoleSize":[0,0]}', known)
        self.assertFalse(self.allowed("GET", f"/exec/{EXEC_ID}/json", b"", known).hijack)

        other = "d" * 64
        self.refused("POST", f"/exec/{other}/start", b"{}", known)
        self.refused("GET", f"/exec/{other}/json", b"", known)
        self.refused("POST", f"/exec/{EXEC_ID}/start", b'{"Detach":true}', known)
        self.refused("POST", f"/exec/{EXEC_ID}/start", b'{"Tty":true}', known)
        self.refused("POST", f"/exec/{EXEC_ID}/start", b'{"Stdin":true}', known)
        self.refused("POST", f"/exec/{EXEC_ID}/resize?h=1&w=1", b"", known)
        self.refused("POST", f"/exec/{EXEC_ID}/start?x=1", b"{}", known)

    def test_every_other_call_is_refused(self):
        name = "chaos_node_1"
        for method, target, body in [
            # Create, remove, list, logs, attach and friends.
            ("POST", "/containers/create", b"{}"),
            ("POST", f"/containers/create?name={name}", b'{"Image":"alpine"}'),
            ("GET", "/containers/json", b""),
            ("GET", "/containers/json?filters=%7B%7D", b""),
            ("DELETE", f"/containers/{name}", b""),
            ("DELETE", f"/containers/{name}?force=1", b""),
            ("GET", f"/containers/{name}/logs?stdout=1", b""),
            ("POST", f"/containers/{name}/attach?stream=1&stdin=1", b""),
            ("GET", f"/containers/{name}/archive?path=/", b""),
            ("PUT", f"/containers/{name}/archive?path=/", b"x"),
            ("GET", f"/containers/{name}/export", b""),
            ("POST", f"/containers/{name}/restart", b""),
            ("POST", f"/containers/{name}/update", b'{"Privileged":true}'),
            ("POST", f"/containers/{name}/rename?name=x", b""),
            ("POST", f"/containers/{name}/commit", b""),
            ("POST", f"/containers/{name}/wait", b""),
            ("GET", f"/containers/{name}/top", b""),
            ("GET", f"/containers/{name}/changes", b""),
            ("GET", f"/containers/{name}/stats", b""),
            ("POST", f"/containers/{name}/resize?h=1&w=1", b""),
            ("POST", "/containers/prune", b""),
            # Lifecycle calls with anything extra.
            ("POST", f"/containers/{name}/kill?signal=HUP", b""),
            ("POST", f"/containers/{name}/kill?signal=KILL&signal=TERM", b""),
            ("POST", f"/containers/{name}/kill?x=1", b""),
            ("POST", f"/containers/{name}/kill", b"{}"),
            ("POST", f"/containers/{name}/stop?t=-1", b""),
            ("POST", f"/containers/{name}/stop?signal=HUP", b""),
            ("POST", f"/containers/{name}/start?detachKeys=x", b""),
            ("POST", f"/containers/{name}/start", b'{"Binds":["/:/host"]}'),
            ("POST", f"/containers/{name}/pause?x=1", b""),
            ("GET", f"/containers/{name}/kill", b""),
            ("DELETE", f"/containers/{name}/kill", b""),
            ("GET", f"/containers/{name}/json?size=1", b""),
            ("POST", f"/containers/{name}/json", b""),
            ("GET", f"/containers/{name}/../../images/json", b""),
            # The rest of the API.
            ("GET", "/version", b""),
            ("GET", "/info", b""),
            ("GET", "/images/json", b""),
            ("POST", "/images/create?fromImage=alpine", b""),
            ("POST", "/build", b""),
            ("GET", "/networks", b""),
            ("POST", "/networks/create", b"{}"),
            ("POST", "/networks/raft_net/connect", b'{"Container":"x"}'),
            ("GET", "/volumes", b""),
            ("POST", "/volumes/create", b"{}"),
            ("GET", "/secrets", b""),
            ("POST", "/swarm/init", b"{}"),
            ("GET", "/events", b""),
            ("POST", "/session", b""),
            ("GET", "/_ping?x=1", b""),
            ("POST", "/_ping", b""),
            ("POST", "/libpod/containers/create", b"{}"),
            ("POST", f"/libpod/containers/{name}/kill", b""),
            ("GET", "/", b""),
            ("GET", "", b""),
        ]:
            self.refused(method, target, body)


class CheckQuorumTest(unittest.TestCase):
    """quorum mode is unchanged by chaos_ctl mode's addition."""

    def test_quorum_calls(self):
        name = "kythira-c1-4"
        create = json.dumps({
            "Image": "kythira-chaos-node:dev", "Labels": {"kythira.cluster": "c1"},
            "Hostname": name, "Env": ["NODE_ID=4"], "Cmd": [],
            "NetworkingConfig": {"EndpointsConfig": {"net1": {}}}}).encode()
        allowed = [
            ("GET", "/containers/json?filters=" + json.dumps({"label": ["kythira.cluster=c1"]}),
             b""),
            ("POST", f"/containers/create?name={name}", create),
            ("GET", f"/containers/{name}/json", b""),
            ("POST", f"/containers/{name}/start", b""),
            ("DELETE", f"/v1.43/containers/{name}?force=1", b""),
        ]
        for method, target, body in allowed:
            fwd = proxy.check(QUORUM, method, target, body)
            self.assertIsNone(fwd.container)
        refused = [
            ("HEAD", "/_ping", b""),
            ("POST", f"/containers/{name}/kill", b""),
            ("POST", f"/containers/{name}/exec", exec_body(FAULT_CMDS[6])),
            ("GET", "/containers/chaos_node_1/json", b""),
            ("POST", f"/containers/create?name={name}",
             create.replace(b'"Cmd"', b'"HostConfig": {"Privileged": true}, "Cmd"')),
            ("GET", "/containers/json", b""),
        ]
        for method, target, body in refused:
            with self.assertRaises(proxy.Refused, msg=f"{method} {target}"):
                proxy.check(QUORUM, method, target, body)

    def test_config_needs_its_settings(self):
        self.assertEqual(QUORUM.missing(), [])
        self.assertEqual(CHAOS.missing(), [])
        self.assertEqual(len(proxy.Config().missing()), 3)
        self.assertEqual(len(proxy.Config(mode="chaos_ctl").missing()), 1)
        self.assertEqual(len(proxy.Config(mode="chaos_ctl", label="novalue").missing()), 1)
        self.assertEqual(len(proxy.Config(mode="other").missing()), 1)
        env = {"PROXY_MODE": "chaos_ctl", "PROXY_LABEL": LABEL}
        self.assertEqual(proxy.Config.from_env(env).label_value, "r1")


class ScopeTest(unittest.TestCase):

    def test_in_scope_needs_the_exact_label(self):
        self.assertTrue(proxy.in_scope(CHAOS, {"Config": {"Labels": {"kythira.cairn.run": "r1"}}}))
        for inspect in ({"Config": {"Labels": {"kythira.cairn.run": "r2"}}},
                        {"Config": {"Labels": {"kythira.cairn.run.x": "r1"}}},
                        {"Config": {"Labels": None}}, {"Config": {}}, {}, [], None,
                        {"Labels": {"kythira.cairn.run": "r1"}}):
            self.assertFalse(proxy.in_scope(CHAOS, inspect), inspect)

    def test_with_container_id(self):
        self.assertEqual(proxy.with_container_id("/v1.51/containers/chaos_node_1/kill?signal=KILL",
                                                 "chaos_node_1", NODE_ID),
                         f"/v1.51/containers/{NODE_ID}/kill?signal=KILL")
        self.assertEqual(proxy.with_container_id("/containers/a%20b/exec", "a b", NODE_ID),
                         f"/containers/{NODE_ID}/exec")


# ── through a running proxy ──────────────────────────────────────────────────


class FakeApi(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    """A container API on a unix socket: chaos_node_1 carries the run's
    label, engine does not. It records every request that reaches it."""

    daemon_threads = True

    def __init__(self, path):
        self.calls = []
        self.lock = threading.Lock()
        super().__init__(path, FakeApiHandler)


class FakeApiHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    containers = {
        "chaos_node_1": (NODE_ID, {"kythira.cairn.run": "r1"}),
        "engine": (OTHER_ID, {"kythira.cairn.run": "other"}),
    }

    def _send(self, status, body=b"", headers=()):
        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _find(self, ref):
        for name, (cid, labels) in self.containers.items():
            if ref in (name, cid):
                return name, cid, labels
        return None

    def _handle(self):
        length = int(self.headers.get("Content-Length", "0") or "0")
        body = self.rfile.read(length) if length else b""
        with self.server.lock:
            self.server.calls.append((self.command, self.path, body))
        path = proxy.VERSION_RE.sub("", self.path.split("?")[0])
        parts = path.strip("/").split("/")
        if path == "/_ping":
            self._send(200, b"OK" if self.command == "GET" else b"",
                       [("Api-Version", "1.51"), ("Connection", "close")])
            return
        if parts[0] == "containers" and len(parts) == 3:
            found = self._find(parts[1])
            if found is None:
                self._send(404, b'{"message":"no such container"}')
                return
            name, cid, labels = found
            if parts[2] == "json":
                self._send(200, json.dumps({"Id": cid, "Name": "/" + name,
                                            "Config": {"Labels": labels}}).encode())
            elif parts[2] == "exec":
                self._send(201, json.dumps({"Id": EXEC_ID}).encode())
            else:
                self._send(204)
            return
        if parts[0] == "exec" and parts[2] == "start":
            # A hijacked stream: the raw response, then the output, then EOF.
            self.wfile.write(b"HTTP/1.1 101 UPGRADED\r\nConnection: Upgrade\r\nUpgrade: tcp\r\n"
                             b"Content-Type: application/vnd.docker.raw-stream\r\n\r\n"
                             b"exec output\n")
            self.wfile.flush()
            self.close_connection = True
            return
        if parts[0] == "exec" and parts[2] == "json":
            self._send(200, b'{"ExitCode":0,"Running":false}')
            return
        self._send(500, b'{"message":"fake api: unexpected call"}')

    do_GET = do_POST = do_HEAD = do_DELETE = do_PUT = _handle

    def address_string(self):
        return "fake-api"

    def log_message(self, fmt, *args):
        pass


class ThroughProxyTest(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        sock = os.path.join(self.dir.name, "api.sock")
        self.api = FakeApi(sock)
        threading.Thread(target=self.api.serve_forever, daemon=True).start()
        self.proxy = proxy.serve(CHAOS, sock, 0, host="127.0.0.1")
        self.port = self.proxy.server_address[1]
        threading.Thread(target=self.proxy.serve_forever, daemon=True).start()
        self._stderr = sys.stderr
        sys.stderr = open(os.devnull, "w")

    def tearDown(self):
        sys.stderr.close()
        sys.stderr = self._stderr
        self.proxy.shutdown()
        self.proxy.server_close()
        self.api.shutdown()
        self.api.server_close()
        self.dir.cleanup()

    def request(self, method, target, body=b"", headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        try:
            conn.request(method, target, body=body, headers=headers or {})
            res = conn.getresponse()
            return res.status, res.read(), dict(res.getheaders())
        finally:
            conn.close()

    def upstream_calls(self):
        with self.api.lock:
            return list(self.api.calls)

    def test_a_refused_call_never_reaches_the_api(self):
        for method, target, body in [
            ("POST", "/containers/create", b"{}"),
            ("DELETE", "/containers/chaos_node_1", b""),
            ("GET", "/containers/json", b""),
            ("POST", "/containers/chaos_node_1/exec", exec_body(["sh"])),
            ("POST", f"/exec/{EXEC_ID}/start", b"{}"),
            ("PUT", "/containers/chaos_node_1/archive?path=/", b"x"),
            ("PATCH", "/containers/chaos_node_1", b""),
            ("OPTIONS", "/_ping", b""),
        ]:
            status, data, _ = self.request(method, target, body)
            self.assertEqual(status, 403, f"{method} {target}: {data!r}")
        self.assertEqual(self.upstream_calls(), [])

    def test_out_of_scope_container_is_refused_after_inspect_only(self):
        for method, target, body in [
            ("GET", "/containers/engine/json", b""),
            ("POST", "/containers/engine/kill", b""),
            ("POST", f"/containers/{OTHER_ID}/pause", b""),
            ("POST", "/containers/engine/exec", exec_body(FAULT_CMDS[6])),
            ("POST", "/containers/missing/start", b""),
        ]:
            status, data, _ = self.request(method, target, body)
            self.assertEqual(status, 403, f"{method} {target}: {data!r}")
        for method, path, _ in self.upstream_calls():
            self.assertEqual(method, "GET")
            self.assertTrue(path.endswith("/json"), path)

    def test_in_scope_call_goes_upstream_by_id(self):
        status, _, _ = self.request("POST", "/v1.51/containers/chaos_node_1/kill?signal=KILL")
        self.assertEqual(status, 204)
        self.assertEqual(self.upstream_calls()[-1],
                         ("POST", f"/v1.51/containers/{NODE_ID}/kill?signal=KILL", b""))
        status, data, _ = self.request("GET", "/containers/chaos_node_1/json")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(data)["Id"], NODE_ID)

    def test_ping_keeps_the_version_header(self):
        status, _, headers = self.request("HEAD", "/_ping")
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("Api-Version"), "1.51")
        self.assertNotIn("close", headers.get("Connection", ""))

    def test_exec_round_trip(self):
        status, data, _ = self.request("POST", "/v1.51/containers/chaos_node_1/exec",
                                       exec_body(FAULT_CMDS[7]))
        self.assertEqual(status, 201, data)
        self.assertEqual(json.loads(data)["Id"], EXEC_ID)

        # The start is hijacked: a raw relay both ways until both close.
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=10)
        body = b'{"Detach":false,"Tty":false}'
        sock.sendall(f"POST /v1.51/exec/{EXEC_ID}/start HTTP/1.1\r\nHost: api\r\n"
                     f"Connection: Upgrade\r\nUpgrade: tcp\r\n"
                     f"Content-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n"
                     .encode() + body)
        sock.shutdown(socket.SHUT_WR)
        got = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            got += chunk
        sock.close()
        self.assertTrue(got.startswith(b"HTTP/1.1 101"), got)
        self.assertTrue(got.endswith(b"exec output\n"), got)
        method, path, sent = self.upstream_calls()[-1]
        self.assertEqual((method, path, sent), ("POST", f"/v1.51/exec/{EXEC_ID}/start", body))

        status, data, _ = self.request("GET", f"/v1.51/exec/{EXEC_ID}/json")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(data)["ExitCode"], 0)

    def test_chunked_body_is_refused(self):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        conn.request("POST", "/containers/chaos_node_1/exec", body=iter([exec_body(["x"])]),
                     headers={"Transfer-Encoding": "chunked"}, encode_chunked=True)
        self.assertEqual(conn.getresponse().status, 411)
        conn.close()
        self.assertEqual(self.upstream_calls(), [])


if __name__ == "__main__":
    unittest.main()
