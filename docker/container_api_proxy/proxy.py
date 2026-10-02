#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""A filtering proxy in front of the container API socket.

The quorum-healing and elastic-capacity scenario tests run nodes whose
docker_quorum_manager creates and removes containers. Mounting the Docker
(or rootless Podman) socket straight into those nodes hands anyone who gets
code execution in one of them the whole daemon: one `POST /containers/create`
with a host bind mount and `Privileged: true` is root on the host.

This proxy is the only container that mounts the socket. It listens on the
compose network (nothing is published) and forwards exactly the calls
docker_quorum_manager makes, for one cluster's containers, and refuses the
rest with 403:

  GET    /containers/json        list, only when filtered to the cluster label
  GET    /containers/<name>/json inspect
  POST   /containers/create      create, from a body docker_quorum_manager
                                 could have written (see _check_create)
  POST   /containers/<name>/start
  DELETE /containers/<name>      remove, optionally with force

<name> must be `kythira-<cluster>-<n>`, the names the manager gives its
containers. A create body must name the configured image, label the
configured cluster, join only the configured network, and carry no
HostConfig at all: no binds, no privileges, no capabilities, no host
namespaces. So a compromised node can, at worst, start and remove more of
its own cluster's nodes.

Configuration, from the environment:

  PROXY_CLUSTER  the cluster name (required)
  PROXY_IMAGE    the one image a created container may run (required)
  PROXY_NETWORK  the one network a created container may join (required)
  PROXY_SOCKET   the API socket (default /var/run/docker.sock)
  PROXY_PORT     the TCP port to listen on (default 2375)
"""

import http.client
import http.server
import json
import os
import re
import socket
import sys
import urllib.parse

CLUSTER = os.environ.get("PROXY_CLUSTER", "")
IMAGE = os.environ.get("PROXY_IMAGE", "")
NETWORK = os.environ.get("PROXY_NETWORK", "")
SOCKET = os.environ.get("PROXY_SOCKET", "/var/run/docker.sock")
PORT = int(os.environ.get("PROXY_PORT", "2375"))

NAME_RE = re.compile(r"^kythira-" + re.escape(CLUSTER) + r"-[0-9]+$")
# The client may pin an API version: /v1.43/containers/json.
VERSION_RE = re.compile(r"^/v[0-9]+(\.[0-9]+)?(?=/)")
CONTAINER_RE = re.compile(r"^/containers/([^/]+)(/json|/start)?$")

# The keys docker_quorum_manager::provision writes. Anything else, HostConfig
# above all, is refused rather than forwarded.
CREATE_KEYS = {"Image", "Labels", "Hostname", "Env", "Cmd", "NetworkingConfig"}


class Refused(Exception):
    pass


def _strings(value, what):
    if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
        raise Refused(f"{what} must be a list of strings")


def _check_create(name, body):
    try:
        spec = json.loads(body or b"null")
    except ValueError as ex:
        raise Refused(f"create body is not JSON: {ex}") from None
    if not isinstance(spec, dict):
        raise Refused("create body must be an object")
    extra = set(spec) - CREATE_KEYS
    if extra:
        raise Refused(f"create may not set {sorted(extra)}")
    if spec.get("Image") != IMAGE:
        raise Refused(f"create may only run image {IMAGE}")
    labels = spec.get("Labels")
    if not isinstance(labels, dict) or labels.get("kythira.cluster") != CLUSTER:
        raise Refused(f"create must label kythira.cluster={CLUSTER}")
    if "Hostname" in spec and spec["Hostname"] != name:
        raise Refused("create's Hostname must be its name")
    for key in ("Env", "Cmd"):
        if key in spec:
            _strings(spec[key], key)
    networking = spec.get("NetworkingConfig", {})
    endpoints = networking.get("EndpointsConfig") if isinstance(networking, dict) else None
    if (not isinstance(endpoints, dict) or set(networking) != {"EndpointsConfig"}
            or set(endpoints) != {NETWORK}):
        raise Refused(f"create may only join network {NETWORK}")
    if endpoints[NETWORK] not in ({}, None):
        raise Refused("create may not configure its endpoint")


def _check_list(query):
    if set(query) - {"all", "filters"}:
        raise Refused("list takes only all= and filters=")
    try:
        filters = json.loads(query.get("filters", ["{}"])[0])
    except ValueError:
        raise Refused("list filters are not JSON") from None
    labels = filters.get("label", []) if isinstance(filters, dict) else []
    if f"kythira.cluster={CLUSTER}" not in labels:
        raise Refused(f"list must filter on label kythira.cluster={CLUSTER}")


def check(method, target, body):
    """Raises Refused unless docker_quorum_manager could have sent this."""
    url = urllib.parse.urlsplit(target)
    path = VERSION_RE.sub("", url.path)
    query = urllib.parse.parse_qs(url.query, keep_blank_values=True)

    if method == "GET" and path == "/containers/json":
        _check_list(query)
        return
    if method == "POST" and path == "/containers/create":
        names = query.get("name", [])
        if set(query) != {"name"} or len(names) != 1 or not NAME_RE.match(names[0]):
            raise Refused(f"create must be named kythira-{CLUSTER}-<n>")
        _check_create(names[0], body)
        return

    m = CONTAINER_RE.match(path)
    if not m or not NAME_RE.match(urllib.parse.unquote(m.group(1))):
        raise Refused(f"{method} {path} is not a call this proxy forwards")
    action = m.group(2)
    if method == "GET" and action == "/json" and not query:
        return
    if method == "POST" and action == "/start" and not query:
        return
    if method == "DELETE" and action is None and set(query) <= {"force"}:
        return
    raise Refused(f"{method} {path} is not a call this proxy forwards")


class UnixConnection(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__("localhost")
        self._path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self._path)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _reply(self, status, body, content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _handle(self):
        if "chunked" in self.headers.get("Transfer-Encoding", "").lower():
            self._reply(411, b'{"message":"container_api_proxy: send a Content-Length"}')
            return
        length = int(self.headers.get("Content-Length", "0") or "0")
        body = self.rfile.read(length) if length else b""
        try:
            check(self.command, self.path, body)
        except Refused as ex:
            sys.stderr.write(f"refused {self.command} {self.path}: {ex}\n")
            msg = json.dumps({"message": f"container_api_proxy: {ex}"}).encode()
            self._reply(403, msg)
            return

        upstream = UnixConnection(SOCKET)
        try:
            headers = {"Host": "localhost"}
            if body or self.command == "POST":
                headers["Content-Type"] = self.headers.get("Content-Type", "application/json")
            upstream.request(self.command, self.path, body=body, headers=headers)
            res = upstream.getresponse()
            data = res.read()
        except OSError as ex:
            self._reply(502, json.dumps({"message": f"container_api_proxy: {ex}"}).encode())
            return
        finally:
            upstream.close()
        self._reply(res.status, data, res.getheader("Content-Type", "application/json"))

    do_GET = _handle
    do_POST = _handle
    do_DELETE = _handle

    def _refuse(self):
        self._reply(403, b'{"message":"container_api_proxy: method not forwarded"}')

    do_PUT = _refuse
    do_HEAD = _refuse
    do_PATCH = _refuse
    do_OPTIONS = _refuse

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))


def main():
    missing = [n for n, v in (("PROXY_CLUSTER", CLUSTER), ("PROXY_IMAGE", IMAGE),
                              ("PROXY_NETWORK", NETWORK)) if not v]
    if missing:
        sys.exit(f"container_api_proxy: set {', '.join(missing)}")
    server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    sys.stderr.write(f"container_api_proxy: cluster={CLUSTER} image={IMAGE} "
                     f"network={NETWORK} socket={SOCKET} port={PORT}\n")
    server.serve_forever()


if __name__ == "__main__":
    main()
