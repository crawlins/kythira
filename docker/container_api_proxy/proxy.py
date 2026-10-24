#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""A filtering proxy in front of the container API socket.

Mounting the Docker (or rootless Podman) socket into a container hands
anyone who gets code execution in it the whole daemon: one
`POST /containers/create` with a host bind mount and `Privileged: true` is
root on the host. This proxy is the only container that mounts the socket.
It listens on a compose network (nothing is published), forwards exactly the
calls one client makes, on one set of containers, and refuses the rest with
403. It has two modes, PROXY_MODE=quorum (the default) and chaos_ctl.

quorum: the quorum-healing and elastic-capacity scenario tests run nodes
whose docker_quorum_manager creates and removes containers. It may send

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

chaos_ctl: the cairn tool server (cmd/chaos_ctl/, .kiro/specs/
cairn-chaos-testing/ Requirement 3.4, design §3.2) drives the `docker` CLI
at this proxy to inject faults into one run's cluster. It may send

  HEAD|GET /_ping                       the CLI's version negotiation
  GET    /containers/<c>/json           inspect
  POST   /containers/<c>/{kill,stop,start,pause,unpause}
  POST   /containers/<c>/exec           create an exec, only of a command
                                        tests/docker_chaos/os_faults.hpp
                                        builds (see _check_fault_cmd)
  POST   /exec/<id>/start               run it, only for an id this proxy
                                        created; the stream is relayed raw
  GET    /exec/<id>/json                its exit code, likewise

<c> may be any name or id, but the proxy inspects it first and refuses
unless it carries the label PROXY_LABEL (`key=value`), which marks this
run's cluster nodes. The call then goes upstream by the container's id, not
the name it was given, so a name cannot be re-pointed between the check and
the call. Inspect is scoped the same way, since an inspect body carries a
container's environment. Nothing can be created, removed, listed, logged or
attached to, and an exec runs as the container's default user with no
privileges, no environment and no TTY. So a compromised tool server can, at
worst, kill, pause and partition this run's cluster, which is its job.

Configuration, from the environment:

  PROXY_MODE     quorum (default) or chaos_ctl
  PROXY_CLUSTER  quorum: the cluster name (required)
  PROXY_IMAGE    quorum: the one image a created container may run (required)
  PROXY_NETWORK  quorum: the one network a created container may join (required)
  PROXY_LABEL    chaos_ctl: `key=value` every container acted on must carry
                 (required)
  PROXY_SOCKET   the API socket (default /var/run/docker.sock)
  PROXY_PORT     the TCP port to listen on (default 2375)
"""

import collections
import http.client
import http.server
import ipaddress
import json
import os
import re
import socket
import sys
import threading
import urllib.parse

# The client may pin an API version: /v1.43/containers/json.
VERSION_RE = re.compile(r"^/v[0-9]+(\.[0-9]+)?(?=/)")
CONTAINER_RE = re.compile(r"^/containers/([^/]+)(/json|/start)?$")
CHAOS_CONTAINER_RE = re.compile(r"^/containers/([^/]+)/(json|kill|stop|start|pause|unpause|exec)$")
EXEC_RE = re.compile(r"^/exec/([0-9a-f]{12,128})/(start|json)$")

# The keys docker_quorum_manager::provision writes. Anything else, HostConfig
# above all, is refused rather than forwarded.
CREATE_KEYS = {"Image", "Labels", "Hostname", "Env", "Cmd", "NetworkingConfig"}

# The keys the docker CLI writes for `docker exec <c> <cmd...>` (and Podman's
# compat API accepts), each with the only value it may take, except Cmd and
# the attach flags, which are checked on their own. A key not listed here
# (Privileged, User, Env, WorkingDir set, a TTY, stdin) is refused.
EXEC_DEFAULTS = {
    "User": ("",),
    "Privileged": (False,),
    "Tty": (False,),
    "AttachStdin": (False,),
    "Detach": (False,),
    "DetachKeys": ("",),
    "Env": (None, []),
    "WorkingDir": ("",),
}
EXEC_ATTACH = {"AttachStdout", "AttachStderr"}
EXEC_START_KEYS = {"Detach", "Tty", "ConsoleSize"}

KILL_SIGNALS = {"", "KILL", "SIGKILL", "9", "TERM", "SIGTERM", "15"}

PCT_RE = re.compile(r"^[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?%$")
TIME_RE = re.compile(r"^[0-9]+(\.[0-9]+)?(us|ms|s)$")

# Exec ids the proxy remembers as its own; the oldest are forgotten first.
KNOWN_EXECS_MAX = 1024


class Refused(Exception):
    pass


class Config:
    """What one proxy instance forwards. Built from the environment by
    from_env(); tests build it directly."""

    def __init__(self, mode="quorum", cluster="", image="", network="", label=""):
        self.mode = mode
        self.cluster = cluster
        self.image = image
        self.network = network
        self.label = label
        self.name_re = re.compile(r"^kythira-" + re.escape(cluster) + r"-[0-9]+$")
        key, _, value = label.partition("=")
        self.label_key = key
        self.label_value = value

    @classmethod
    def from_env(cls, env=os.environ):
        return cls(mode=env.get("PROXY_MODE", "quorum") or "quorum",
                   cluster=env.get("PROXY_CLUSTER", ""), image=env.get("PROXY_IMAGE", ""),
                   network=env.get("PROXY_NETWORK", ""), label=env.get("PROXY_LABEL", ""))

    def missing(self):
        """The settings this mode needs and lacks, or why the mode is bad."""
        if self.mode == "quorum":
            return [n for n, v in (("PROXY_CLUSTER", self.cluster), ("PROXY_IMAGE", self.image),
                                   ("PROXY_NETWORK", self.network)) if not v]
        if self.mode == "chaos_ctl":
            return [] if self.label_key and self.label_value else ["PROXY_LABEL (key=value)"]
        return [f"PROXY_MODE quorum or chaos_ctl, not {self.mode!r}"]

    def describe(self):
        if self.mode == "chaos_ctl":
            return f"mode=chaos_ctl label={self.label}"
        return f"mode=quorum cluster={self.cluster} image={self.image} network={self.network}"


class Forward:
    """What check() approved. `container` is set when the call acts on a
    container that must first be found in scope; `exec_id` when it acts on
    an exec; `hijack` when the response is a raw stream; `records_exec`
    when the response's Id is an exec this proxy now owns."""

    def __init__(self, container=None, exec_id=None, hijack=False, records_exec=False):
        self.container = container
        self.exec_id = exec_id
        self.hijack = hijack
        self.records_exec = records_exec


def _strings(value, what):
    if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
        raise Refused(f"{what} must be a list of strings")


def _json_body(body, what):
    try:
        spec = json.loads(body or b"null")
    except ValueError as ex:
        raise Refused(f"{what} body is not JSON: {ex}") from None
    return spec


# ── quorum mode ──────────────────────────────────────────────────────────────


def _check_create(cfg, name, body):
    spec = _json_body(body, "create")
    if not isinstance(spec, dict):
        raise Refused("create body must be an object")
    extra = set(spec) - CREATE_KEYS
    if extra:
        raise Refused(f"create may not set {sorted(extra)}")
    if spec.get("Image") != cfg.image:
        raise Refused(f"create may only run image {cfg.image}")
    labels = spec.get("Labels")
    if not isinstance(labels, dict) or labels.get("kythira.cluster") != cfg.cluster:
        raise Refused(f"create must label kythira.cluster={cfg.cluster}")
    if "Hostname" in spec and spec["Hostname"] != name:
        raise Refused("create's Hostname must be its name")
    for key in ("Env", "Cmd"):
        if key in spec:
            _strings(spec[key], key)
    networking = spec.get("NetworkingConfig", {})
    endpoints = networking.get("EndpointsConfig") if isinstance(networking, dict) else None
    if (not isinstance(endpoints, dict) or set(networking) != {"EndpointsConfig"}
            or set(endpoints) != {cfg.network}):
        raise Refused(f"create may only join network {cfg.network}")
    if endpoints[cfg.network] not in ({}, None):
        raise Refused("create may not configure its endpoint")


def _check_list(cfg, query):
    if set(query) - {"all", "filters"}:
        raise Refused("list takes only all= and filters=")
    try:
        filters = json.loads(query.get("filters", ["{}"])[0])
    except ValueError:
        raise Refused("list filters are not JSON") from None
    labels = filters.get("label", []) if isinstance(filters, dict) else []
    if f"kythira.cluster={cfg.cluster}" not in labels:
        raise Refused(f"list must filter on label kythira.cluster={cfg.cluster}")


def _check_quorum(cfg, method, path, query, body):
    if method == "GET" and path == "/containers/json":
        _check_list(cfg, query)
        return Forward()
    if method == "POST" and path == "/containers/create":
        names = query.get("name", [])
        if set(query) != {"name"} or len(names) != 1 or not cfg.name_re.match(names[0]):
            raise Refused(f"create must be named kythira-{cfg.cluster}-<n>")
        _check_create(cfg, names[0], body)
        return Forward()

    m = CONTAINER_RE.match(path)
    if not m or not cfg.name_re.match(urllib.parse.unquote(m.group(1))):
        raise Refused(f"{method} {path} is not a call this proxy forwards")
    action = m.group(2)
    if method == "GET" and action == "/json" and not query:
        return Forward()
    if method == "POST" and action == "/start" and not query:
        return Forward()
    if method == "DELETE" and action is None and set(query) <= {"force"}:
        return Forward()
    raise Refused(f"{method} {path} is not a call this proxy forwards")


# ── chaos_ctl mode ───────────────────────────────────────────────────────────


def _ipv4(token):
    try:
        ipaddress.IPv4Address(token)
    except ValueError:
        return False
    return True


def _check_netem(opts):
    """`loss <pct>`, `delay <time> [<jitter>]` and `corrupt <pct>`, each at
    most once, at least one: the options tc_netem_cmd, tc_delay_cmd and
    tc_loss_cmd write after `netem`."""
    seen = set()
    i = 0
    while i < len(opts):
        key = opts[i]
        if key in seen or key not in ("loss", "delay", "corrupt"):
            return False
        seen.add(key)
        if i + 1 >= len(opts):
            return False
        value = opts[i + 1]
        if key == "delay":
            if not TIME_RE.match(value):
                return False
            i += 2
            if i < len(opts) and TIME_RE.match(opts[i]):
                i += 1
            continue
        if not PCT_RE.match(value):
            return False
        i += 2
    return bool(seen)


def _check_fault_cmd(cmd):
    """Raises Refused unless cmd is one of the argument vectors
    tests/docker_chaos/os_faults.hpp builds after `<runtime> exec <c>`."""
    _strings(cmd, "Cmd")
    if cmd[:7] == ["tc", "qdisc", "replace", "dev", "eth0", "root", "netem"]:
        if _check_netem(cmd[7:]):
            return
    elif cmd == ["tc", "qdisc", "del", "dev", "eth0", "root"]:
        return
    elif cmd in (["iptables", "-F", "CHAOS"], ["iptables", "-F", "OUTPUT"]):
        return
    elif (len(cmd) == 7 and cmd[0] == "iptables" and cmd[1] == "-A" and cmd[5:] == ["-j", "DROP"]
          and (cmd[2], cmd[3]) in (("CHAOS", "-s"), ("OUTPUT", "-d")) and _ipv4(cmd[4])):
        return
    raise Refused(f"exec may only run the tc and iptables commands os_faults.hpp builds, "
                  f"not {cmd!r}")


def _check_exec_create(body):
    spec = _json_body(body, "exec")
    if not isinstance(spec, dict):
        raise Refused("exec body must be an object")
    allowed = set(EXEC_DEFAULTS) | EXEC_ATTACH | {"Cmd", "ConsoleSize"}
    extra = set(spec) - allowed
    if extra:
        raise Refused(f"exec may not set {sorted(extra)}")
    for key, values in EXEC_DEFAULTS.items():
        if key in spec and not any(spec[key] == v and type(spec[key]) is type(v) for v in values):
            raise Refused(f"exec may not set {key}={spec[key]!r}")
    for key in EXEC_ATTACH:
        if key in spec and not isinstance(spec[key], bool):
            raise Refused(f"exec's {key} must be a boolean")
    _check_console_size(spec)
    if "Cmd" not in spec:
        raise Refused("exec must name a Cmd")
    _check_fault_cmd(spec["Cmd"])


def _check_console_size(spec):
    size = spec.get("ConsoleSize")
    if size is not None and (not isinstance(size, list) or len(size) != 2 or not all(
            isinstance(v, int) and not isinstance(v, bool) and v >= 0 for v in size)):
        raise Refused("ConsoleSize must be two non-negative integers")


def _check_exec_start(body):
    spec = _json_body(body, "exec start") if body else {}
    if not isinstance(spec, dict):
        raise Refused("exec start body must be an object")
    extra = set(spec) - EXEC_START_KEYS
    if extra:
        raise Refused(f"exec start may not set {sorted(extra)}")
    if spec.get("Detach", False) is not False or spec.get("Tty", False) is not False:
        raise Refused("exec start may not detach or take a TTY")
    _check_console_size(spec)


def _check_chaos_ctl(method, path, query, body, known_execs):
    if method in ("GET", "HEAD") and path == "/_ping" and not query:
        return Forward()

    m = CHAOS_CONTAINER_RE.match(path)
    if m:
        name = urllib.parse.unquote(m.group(1))
        action = m.group(2)
        if method == "GET" and action == "json" and set(query) <= {"size"}:
            if query.get("size", ["false"]) not in (["false"], ["0"], [""]):
                raise Refused("inspect may not ask for size")
            return Forward(container=name)
        if method != "POST" or body and action != "exec":
            raise Refused(f"{method} {path} is not a call this proxy forwards")
        if action == "kill":
            signals = query.get("signal", [""])
            if set(query) - {"signal"} or len(signals) != 1 or signals[0] not in KILL_SIGNALS:
                raise Refused("kill takes only signal=KILL or TERM")
            return Forward(container=name)
        if action == "stop":
            times = query.get("t", ["10"])
            if set(query) - {"t"} or len(times) != 1 or not re.fullmatch(r"[0-9]{1,3}", times[0]):
                raise Refused("stop takes only t=<seconds>")
            return Forward(container=name)
        if action in ("start", "pause", "unpause"):
            if query:
                raise Refused(f"{action} takes no query")
            return Forward(container=name)
        # action == "exec"
        if query:
            raise Refused("exec takes no query")
        _check_exec_create(body)
        return Forward(container=name, records_exec=True)

    m = EXEC_RE.match(path)
    if m:
        exec_id, action = m.group(1), m.group(2)
        if exec_id not in known_execs:
            raise Refused(f"exec {exec_id} was not created through this proxy")
        if method == "POST" and action == "start" and not query:
            _check_exec_start(body)
            return Forward(exec_id=exec_id, hijack=True)
        if method == "GET" and action == "json" and not query and not body:
            return Forward(exec_id=exec_id)
    raise Refused(f"{method} {path} is not a call this proxy forwards")


def in_scope(cfg, inspect):
    """Whether an inspect body (GET /containers/<c>/json) is a container
    this chaos_ctl-mode proxy may act on."""
    config = inspect.get("Config") if isinstance(inspect, dict) else None
    labels = config.get("Labels") if isinstance(config, dict) else None
    return isinstance(labels, dict) and labels.get(cfg.label_key) == cfg.label_value


def check(cfg, method, target, body, known_execs=frozenset()):
    """Raises Refused unless this proxy's client could have sent the call;
    otherwise returns what the forwarder must still do (see Forward)."""
    url = urllib.parse.urlsplit(target)
    path = VERSION_RE.sub("", url.path)
    query = urllib.parse.parse_qs(url.query, keep_blank_values=True)
    if cfg.mode == "chaos_ctl":
        return _check_chaos_ctl(method, path, query, body, known_execs)
    if method == "HEAD":
        raise Refused("method not forwarded")
    return _check_quorum(cfg, method, path, query, body)


def with_container_id(target, name, container_id):
    """target with its container segment replaced by container_id."""
    url = urllib.parse.urlsplit(target)
    quoted = re.escape(urllib.parse.quote(name, safe="")) + "|" + re.escape(name)
    path = re.sub(r"(/containers/)(?:" + quoted + r")(/)",
                  lambda m: m.group(1) + container_id + m.group(2), url.path, count=1)
    return urllib.parse.urlunsplit(url._replace(path=path))


# ── forwarding ───────────────────────────────────────────────────────────────


class UnixConnection(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__("localhost")
        self._path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self._path)


# Headers a reply never copies from upstream: the proxy frames its own body
# and writes its own Date, Server and Content-Type.
HOP_HEADERS = {"connection", "keep-alive", "transfer-encoding", "content-length", "upgrade",
               "proxy-connection", "te", "trailer", "date", "server", "content-type"}


class Upstream(Exception):
    pass


def _relay(src, dst):
    """Copies src to dst until src ends, then half-closes dst."""
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


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    # Set on the class by serve() and by tests.
    cfg = Config()
    socket_path = "/var/run/docker.sock"
    known_execs = collections.OrderedDict()
    execs_lock = threading.Lock()

    def _reply(self, status, body, content_type="application/json", headers=()):
        self.send_response(status)
        for key, value in headers:
            self.send_header(key, value)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _refuse(self, reason):
        sys.stderr.write(f"refused {self.command} {self.path}: {reason}\n")
        msg = json.dumps({"message": f"container_api_proxy: {reason}"}).encode()
        self._reply(403, msg)

    def _upstream(self, method, target, body=b""):
        upstream = UnixConnection(self.socket_path)
        try:
            headers = {"Host": "localhost"}
            if body or method == "POST":
                headers["Content-Type"] = self.headers.get("Content-Type", "application/json")
            upstream.request(method, target, body=body, headers=headers)
            res = upstream.getresponse()
            data = res.read()
        except OSError as ex:
            raise Upstream(str(ex)) from None
        finally:
            upstream.close()
        return res, data

    def _resolve(self, name):
        """The id of container `name` if it is in scope; else Refused."""
        res, data = self._upstream("GET", "/containers/" + urllib.parse.quote(name, safe="") +
                                   "/json")
        if res.status != 200:
            raise Refused(f"container {name} not found")
        try:
            inspect = json.loads(data)
        except ValueError:
            raise Refused(f"container {name}: upstream inspect is not JSON") from None
        if not in_scope(self.cfg, inspect):
            raise Refused(f"container {name} does not carry {self.cfg.label}")
        container_id = inspect.get("Id")
        if not isinstance(container_id, str) or not re.fullmatch(r"[0-9a-f]{12,128}",
                                                                  container_id):
            raise Refused(f"container {name}: upstream inspect has no id")
        return container_id, res, data

    def _remember_exec(self, data):
        try:
            exec_id = json.loads(data).get("Id")
        except (ValueError, AttributeError):
            return
        if isinstance(exec_id, str):
            with self.execs_lock:
                self.known_execs[exec_id] = True
                while len(self.known_execs) > KNOWN_EXECS_MAX:
                    self.known_execs.popitem(last=False)

    def _hijack(self, target, body):
        """Forwards a stream-upgrading call, then relays bytes both ways
        until both sides have finished."""
        up = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            up.connect(self.socket_path)
        except OSError as ex:
            up.close()
            raise Upstream(str(ex)) from None
        head = [f"POST {target} HTTP/1.1", "Host: localhost",
                f"Content-Type: {self.headers.get('Content-Type', 'application/json')}",
                f"Content-Length: {len(body)}"]
        if self.headers.get("Upgrade"):
            head += ["Connection: Upgrade", "Upgrade: tcp"]
        up.sendall(("\r\n".join(head) + "\r\n\r\n").encode() + body)
        self.wfile.flush()
        client = self.connection
        back = threading.Thread(target=_relay, args=(up, client), daemon=True)
        back.start()
        # Anything the client already sent past the request (stdin it was
        # not allowed to ask for, in practice nothing) is in rfile's buffer.
        _relay(client, up)
        back.join()
        up.close()
        self.close_connection = True

    def _handle(self):
        if "chunked" in self.headers.get("Transfer-Encoding", "").lower():
            # The unread body would parse as the next request: hang up.
            self.close_connection = True
            self._reply(411, b'{"message":"container_api_proxy: send a Content-Length"}')
            return
        length = int(self.headers.get("Content-Length", "0") or "0")
        body = self.rfile.read(length) if length else b""
        with self.execs_lock:
            known = frozenset(self.known_execs)
        try:
            fwd = check(self.cfg, self.command, self.path, body, known)
            target = self.path
            if fwd.container is not None:
                container_id, res, data = self._resolve(fwd.container)
                if self.command == "GET":
                    self._reply(res.status, data, res.getheader("Content-Type", "application/json"))
                    return
                target = with_container_id(target, fwd.container, container_id)
        except Refused as ex:
            self._refuse(ex)
            return
        except Upstream as ex:
            self._reply(502, json.dumps({"message": f"container_api_proxy: {ex}"}).encode())
            return

        try:
            if fwd.hijack:
                self._hijack(target, body)
                return
            res, data = self._upstream(self.command, target, body)
        except Upstream as ex:
            self._reply(502, json.dumps({"message": f"container_api_proxy: {ex}"}).encode())
            return
        if fwd.records_exec and 200 <= res.status < 300:
            self._remember_exec(data)
        headers = [(k, v) for k, v in res.getheaders() if k.lower() not in HOP_HEADERS]
        self._reply(res.status, data, res.getheader("Content-Type", "application/json"), headers)

    do_GET = _handle
    do_POST = _handle
    do_DELETE = _handle
    do_HEAD = _handle

    def _method_refused(self):
        self._refuse("method not forwarded")

    do_PUT = _method_refused
    do_PATCH = _method_refused
    do_OPTIONS = _method_refused

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))


def serve(cfg, socket_path, port, host="0.0.0.0"):
    """A server forwarding for cfg; the caller runs serve_forever()."""
    handler = type("ConfiguredHandler", (Handler,), {
        "cfg": cfg,
        "socket_path": socket_path,
        "known_execs": collections.OrderedDict(),
        "execs_lock": threading.Lock(),
    })
    return http.server.ThreadingHTTPServer((host, port), handler)


def main():
    cfg = Config.from_env()
    missing = cfg.missing()
    if missing:
        sys.exit(f"container_api_proxy: set {', '.join(missing)}")
    socket_path = os.environ.get("PROXY_SOCKET", "/var/run/docker.sock")
    port = int(os.environ.get("PROXY_PORT", "2375"))
    server = serve(cfg, socket_path, port)
    sys.stderr.write(f"container_api_proxy: {cfg.describe()} socket={socket_path} port={port}\n")
    server.serve_forever()


if __name__ == "__main__":
    main()
