#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Tests for egress_proxy.py, the cairn fence's way out (task 8.2). Local
sockets only: a fake model host on 127.0.0.1 stands in for the real ones.

Run: python3 -m unittest discover -s docker/cairn/egress_proxy -p '*_test.py'
"""

import os
import socket
import socketserver
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import egress_proxy  # noqa: E402


class Echo(socketserver.BaseRequestHandler):
    """A model host: records what arrives, answers with an HTTP response
    naming the first line it got."""

    def handle(self):
        data = b""
        self.request.settimeout(5)
        try:
            while b"\r\n\r\n" not in data:
                chunk = self.request.recv(4096)
                if not chunk:
                    break
                data += chunk
        except OSError:
            pass
        self.server.seen.append(data)
        first = data.split(b"\r\n", 1)[0]
        body = b"got " + first
        self.request.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: " + str(len(body)).encode() +
                             b"\r\n\r\n" + body)


class EchoServer(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self):
        self.seen = []
        super().__init__(("127.0.0.1", 0), Echo)


def exchange(port, raw):
    with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
        s.sendall(raw)
        s.shutdown(socket.SHUT_WR)
        out = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                return out
            out += chunk


class RouteTest(unittest.TestCase):
    ALLOW = {("api.anthropic.com", 443), ("llama", 8080)}

    def test_allowed(self):
        self.assertEqual(egress_proxy.route(self.ALLOW, "CONNECT api.anthropic.com:443 HTTP/1.1"),
                         ("api.anthropic.com", 443, None))
        self.assertEqual(egress_proxy.route(self.ALLOW, "CONNECT API.Anthropic.com:443 HTTP/1.1")
                         [:2], ("api.anthropic.com", 443))
        self.assertEqual(
            egress_proxy.route(self.ALLOW, "POST http://llama:8080/v1/chat?x=1 HTTP/1.1"),
            ("llama", 8080, "POST /v1/chat?x=1 HTTP/1.1"))

    def test_refused(self):
        for line in ("CONNECT api.anthropic.com:80 HTTP/1.1",
                     "CONNECT api.anthropic.com.evil.net:443 HTTP/1.1",
                     "CONNECT evil.net:443 HTTP/1.1",
                     "CONNECT api.anthropic.com HTTP/1.1",
                     "CONNECT 1.1.1.1:443 HTTP/1.1",
                     "GET http://llama:8081/ HTTP/1.1",
                     "GET http://llama/ HTTP/1.1",
                     "GET http://user:pw@llama:8080/ HTTP/1.1",
                     "GET https://api.anthropic.com/ HTTP/1.1",
                     "GET / HTTP/1.1",
                     "GET http://api.anthropic.com:443/ HTTP/2",
                     "GET  http://llama:8080/ HTTP/1.1",
                     ""):
            with self.assertRaises(egress_proxy.Refused, msg=line):
                egress_proxy.route(self.ALLOW, line)

    def test_parse_allow(self):
        self.assertEqual(egress_proxy.parse_allow(" api.anthropic.com:443, llama:8080 ,"),
                         {("api.anthropic.com", 443), ("llama", 8080)})
        for bad in ("api.anthropic.com", ":443", "h:0", "h:70000", "h:x"):
            with self.assertRaises(ValueError, msg=bad):
                egress_proxy.parse_allow(bad)


class ThroughProxyTest(unittest.TestCase):

    def setUp(self):
        self.model = EchoServer()
        self.model_port = self.model.server_address[1]
        threading.Thread(target=self.model.serve_forever, daemon=True).start()
        self.proxy = egress_proxy.serve({("127.0.0.1", self.model_port)}, 0, host="127.0.0.1")
        self.port = self.proxy.server_address[1]
        threading.Thread(target=self.proxy.serve_forever, daemon=True).start()
        self._stderr = sys.stderr
        sys.stderr = open(os.devnull, "w")

    def tearDown(self):
        sys.stderr.close()
        sys.stderr = self._stderr
        for s in (self.proxy, self.model):
            s.shutdown()
            s.server_close()

    def test_connect_tunnels_to_an_allowed_host(self):
        out = exchange(self.port, f"CONNECT 127.0.0.1:{self.model_port} HTTP/1.1\r\n\r\n"
                       "GET /inner HTTP/1.1\r\n\r\n".encode())
        self.assertTrue(out.startswith(b"HTTP/1.1 200 Connection Established\r\n\r\n"), out)
        self.assertTrue(out.endswith(b"got GET /inner HTTP/1.1"), out)

    def test_plain_http_is_forwarded_in_origin_form(self):
        out = exchange(self.port, f"GET http://127.0.0.1:{self.model_port}/v1/models HTTP/1.1\r\n"
                       f"Host: x\r\nProxy-Connection: keep-alive\r\n\r\n".encode())
        self.assertTrue(out.endswith(b"got GET /v1/models HTTP/1.1"), out)
        seen = self.model.seen[-1]
        self.assertIn(b"Connection: close", seen)
        self.assertNotIn(b"Proxy-Connection", seen)

    def test_anything_else_is_refused_and_never_connects(self):
        for raw in (b"CONNECT 127.0.0.1:1 HTTP/1.1\r\n\r\n",
                    b"CONNECT example.com:443 HTTP/1.1\r\n\r\n",
                    f"GET http://127.0.0.1:{self.model_port + 1}/ HTTP/1.1\r\n\r\n".encode(),
                    b"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
                    b"\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03\r\n\r\n"):
            out = exchange(self.port, raw)
            self.assertTrue(out.startswith(b"HTTP/1.1 403 "), (raw, out))
        self.assertEqual(self.model.seen, [])


if __name__ == "__main__":
    unittest.main()
