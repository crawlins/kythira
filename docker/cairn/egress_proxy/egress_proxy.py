#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""The cairn fence's only way out (.kiro/specs/cairn-chaos-testing/
Requirement 7.2, design §5).

Agent containers sit on networks marked `internal: true`, which have no
route off the host. This proxy is the one container on both such a network
and an ordinary one, and it forwards only to the model endpoints:

  CONNECT host:port              tunnels TLS (HTTPS_PROXY), when host:port
                                 is allowed
  <METHOD> http://host:port/...  forwards plain HTTP (HTTP_PROXY, e.g. a
                                 llama.cpp server), when host:port is allowed

Anything else, an origin-form request included, is refused with 403. The
allowlist matches the name the client asked for, exactly and with its port,
so `api.anthropic.com:443` does not admit `api.anthropic.com.evil:443` or
port 80. Package mirrors, general hosts and the container runtime are
therefore unreachable, which is what the fence probe (fence_probe.py)
checks from inside the fence.

Configuration, from the environment:

  EGRESS_ALLOW  comma-separated host:port pairs (required)
  EGRESS_PORT   the TCP port to listen on (default 3128)
"""

import os
import socket
import socketserver
import sys
import threading
import urllib.parse

CONNECT_TIMEOUT_S = 10
HEAD_LIMIT = 65536


def parse_allow(text):
    """{(host, port)} from `h1:p1,h2:p2`; raises ValueError on a bad entry."""
    out = set()
    for item in (t.strip() for t in text.split(",")):
        if not item:
            continue
        host, sep, port = item.rpartition(":")
        if not sep or not host or not port.isdigit() or not 0 < int(port) < 65536:
            raise ValueError(f"EGRESS_ALLOW entry {item!r} is not host:port")
        out.add((host.lower(), int(port)))
    return out


class Refused(Exception):
    pass


def route(allow, request_line):
    """(host, port, line to send upstream or None for a tunnel) for an
    allowed request line; raises Refused otherwise."""
    parts = request_line.split(" ")
    if len(parts) != 3 or not parts[2].startswith("HTTP/1."):
        raise Refused("malformed request line")
    method, target, version = parts
    if method == "CONNECT":
        host, sep, port = target.rpartition(":")
        if not sep or not port.isdigit():
            raise Refused("CONNECT needs host:port")
        dest = (host.lower().strip("[]"), int(port))
        if dest not in allow:
            raise Refused(f"{target} is not an allowed model host")
        return dest[0], dest[1], None
    url = urllib.parse.urlsplit(target)
    if url.scheme != "http" or not url.hostname:
        raise Refused("only CONNECT and absolute http:// requests are forwarded")
    if url.username or url.password:
        raise Refused("credentials in the URL are not forwarded")
    dest = (url.hostname.lower(), url.port or 80)
    if dest not in allow:
        raise Refused(f"{url.hostname}:{dest[1]} is not an allowed model host")
    path = urllib.parse.urlunsplit(("", "", url.path or "/", url.query, ""))
    return dest[0], dest[1], f"{method} {path} {version}"


def _pipe(src, dst):
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    try:
        dst.shutdown(socket.SHUT_WR)
    except OSError:
        pass


def _read_head(sock):
    """The request head (through the blank line) and any bytes past it."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise Refused("connection closed before the request head ended")
        buf += chunk
        if len(buf) > HEAD_LIMIT:
            raise Refused("request head too large")
    head, _, rest = buf.partition(b"\r\n\r\n")
    return head.decode("latin-1"), rest


class Handler(socketserver.BaseRequestHandler):
    allow = frozenset()

    def _refuse(self, reason):
        sys.stderr.write(f"egress_proxy: refused {self.client_address[0]}: {reason}\n")
        body = f"egress_proxy: {reason}\n".encode()
        try:
            self.request.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\n"
                                 b"Connection: close\r\nContent-Length: " +
                                 str(len(body)).encode() + b"\r\n\r\n" + body)
        except OSError:
            pass

    def handle(self):
        client = self.request
        client.settimeout(30)
        try:
            head, rest = _read_head(client)
            lines = head.split("\r\n")
            host, port, first = route(self.allow, lines[0])
        except Refused as ex:
            self._refuse(ex)
            return
        except (OSError, UnicodeError):
            return
        try:
            upstream = socket.create_connection((host, port), timeout=CONNECT_TIMEOUT_S)
        except OSError as ex:
            sys.stderr.write(f"egress_proxy: {host}:{port} unreachable: {ex}\n")
            try:
                client.sendall(b"HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n"
                               b"Content-Length: 0\r\n\r\n")
            except OSError:
                pass
            return
        client.settimeout(None)
        upstream.settimeout(None)
        sys.stderr.write(f"egress_proxy: {self.client_address[0]} -> {host}:{port}\n")
        with upstream:
            if first is None:
                client.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
                if rest:
                    upstream.sendall(rest)
            else:
                # One request per connection: the next one on a kept-alive
                # client connection would bypass route().
                kept = [h for h in lines[1:] if h.split(":", 1)[0].strip().lower() not in
                        ("proxy-connection", "connection", "keep-alive", "proxy-authorization")]
                upstream.sendall(("\r\n".join([first, *kept, "Connection: close"]) +
                                  "\r\n\r\n").encode("latin-1") + rest)
            back = threading.Thread(target=_pipe, args=(upstream, client), daemon=True)
            back.start()
            _pipe(client, upstream)
            back.join()


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def serve(allow, port, host="0.0.0.0"):
    handler = type("ConfiguredHandler", (Handler,), {"allow": frozenset(allow)})
    return Server((host, port), handler)


def main():
    try:
        allow = parse_allow(os.environ.get("EGRESS_ALLOW", ""))
    except ValueError as ex:
        sys.exit(f"egress_proxy: {ex}")
    if not allow:
        sys.exit("egress_proxy: set EGRESS_ALLOW to the model hosts, as host:port")
    port = int(os.environ.get("EGRESS_PORT", "3128"))
    server = serve(allow, port)
    names = ",".join(f"{h}:{p}" for h, p in sorted(allow))
    sys.stderr.write(f"egress_proxy: allow={names} port={port}\n")
    server.serve_forever()


if __name__ == "__main__":
    main()
