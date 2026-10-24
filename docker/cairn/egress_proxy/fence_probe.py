#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Checks the cairn fence from inside it (.kiro/specs/cairn-chaos-testing/
task 8.4, Requirement 7.2).

Run in a container placed where an agent's worker sits: on cairn_net only,
with HTTP(S)_PROXY pointing at the egress proxy, as a worker is configured.
It passes only if

  - every model host (PROBE_ALLOWED) is reachable through the egress proxy,
    with a verified TLS handshake on port 443;
  - every other host (PROBE_DENIED) is refused by the proxy;
  - nothing (PROBE_DIRECT, the model hosts included) is reachable without
    the proxy;
  - the container API, its filtering proxy and the cluster nodes
    (PROBE_UNREACHABLE) cannot be reached at all, and no runtime socket is
    mounted;
  - package installs fail (pip, and apk where present).

Prints one JSON line per check and exits 0 only if all of them passed.

Configuration, from the environment (lists are comma-separated host:port):

  PROBE_PROXY        the egress proxy (default egress-proxy:3128)
  PROBE_ALLOWED      default api.anthropic.com:443
  PROBE_DENIED       default example.com:443,pypi.org:443,
                     dl-cdn.alpinelinux.org:443,deb.debian.org:80,
                     github.com:443
  PROBE_DIRECT       default 1.1.1.1:443,8.8.8.8:53, plus PROBE_ALLOWED
  PROBE_UNREACHABLE  default container-api-proxy:2375,node1:8080,node1:9000
"""

import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile

TIMEOUT_S = 5
SOCKETS = ("/var/run/docker.sock", "/run/docker.sock", "/run/podman/podman.sock",
           "/var/run/podman/podman.sock")


def pairs(text):
    out = []
    for item in (t.strip() for t in text.split(",")):
        if item:
            host, _, port = item.rpartition(":")
            out.append((host, int(port)))
    return out


def env_pairs(name, default):
    return pairs(os.environ.get(name, default))


def connect_via(proxy, host, port):
    """Status code of a CONNECT through proxy, and the tunnel socket."""
    sock = socket.create_connection(proxy, timeout=TIMEOUT_S)
    sock.sendall(f"CONNECT {host}:{port} HTTP/1.1\r\nHost: {host}:{port}\r\n\r\n".encode())
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = sock.recv(4096)
        if not chunk:
            break
        head += chunk
    status = int(head.split(b" ", 2)[1]) if head.startswith(b"HTTP/") else 0
    return status, sock


def http_via(proxy, host, port):
    """Status code of an absolute-form GET through proxy."""
    with socket.create_connection(proxy, timeout=TIMEOUT_S) as sock:
        sock.sendall(f"GET http://{host}:{port}/ HTTP/1.1\r\nHost: {host}:{port}\r\n"
                     f"Connection: close\r\n\r\n".encode())
        head = sock.recv(4096)
    return int(head.split(b" ", 2)[1]) if head.startswith(b"HTTP/") else 0


def can_connect(host, port):
    try:
        with socket.create_connection((host, port), timeout=TIMEOUT_S):
            return True
    except OSError:
        return False


def run_installer(argv):
    """Whether argv (a package install) succeeded within a minute."""
    try:
        return subprocess.run(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=60, check=False).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def main():
    proxy_host, proxy_port = pairs(os.environ.get("PROBE_PROXY", "egress-proxy:3128"))[0]
    proxy = (proxy_host, proxy_port)
    allowed = env_pairs("PROBE_ALLOWED", "api.anthropic.com:443")
    denied = env_pairs("PROBE_DENIED", "example.com:443,pypi.org:443,"
                       "dl-cdn.alpinelinux.org:443,deb.debian.org:80,github.com:443")
    direct = env_pairs("PROBE_DIRECT", "1.1.1.1:443,8.8.8.8:53") + allowed
    unreachable = env_pairs("PROBE_UNREACHABLE",
                            "container-api-proxy:2375,node1:8080,node1:9000")
    results = []

    def record(check, target, passed, detail=""):
        results.append(passed)
        print(json.dumps({"check": check, "target": target, "pass": passed, "detail": detail}),
              flush=True)

    for host, port in allowed:
        target = f"{host}:{port}"
        try:
            if port == 443:
                status, sock = connect_via(proxy, host, port)
                with sock:
                    if status != 200:
                        record("model host via proxy", target, False, f"CONNECT got {status}")
                        continue
                    ctx = ssl.create_default_context()
                    with ctx.wrap_socket(sock, server_hostname=host) as tls:
                        record("model host via proxy", target, True, tls.version())
            else:
                status = http_via(proxy, host, port)
                record("model host via proxy", target, status not in (0, 403, 502),
                       f"HTTP {status}")
        except (OSError, ssl.SSLError, ValueError) as ex:
            record("model host via proxy", target, False, str(ex))

    for host, port in denied:
        target = f"{host}:{port}"
        try:
            status, sock = connect_via(proxy, host, port)
            sock.close()
            plain = http_via(proxy, host, port)
            record("other host refused by proxy", target, status == 403 and plain == 403,
                   f"CONNECT {status}, GET {plain}")
        except (OSError, ValueError) as ex:
            record("other host refused by proxy", target, False, str(ex))

    for host, port in direct:
        record("no direct route", f"{host}:{port}", not can_connect(host, port))

    for host, port in unreachable:
        record("control plane unreachable", f"{host}:{port}", not can_connect(host, port))

    for path in SOCKETS:
        record("no runtime socket", path, not os.path.exists(path))

    with tempfile.TemporaryDirectory() as tmp:
        pip = [sys.executable, "-m", "pip", "download", "--no-deps", "--no-cache-dir",
               "--retries", "0", "--timeout", "5", "-d", tmp, "six"]
        record("package install fails", "pip", not run_installer(pip))
    if shutil.which("apk"):
        record("package install fails", "apk",
               not run_installer(["apk", "add", "--no-cache", "curl"]))

    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
