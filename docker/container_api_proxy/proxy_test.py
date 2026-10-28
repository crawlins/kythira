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
WORKER_IMAGE = "kythira-cairn-worker:dev"
WORKER_LABEL = "kythira.cairn.worker=r1"
DISPATCHER = proxy.Config(mode="cairn_dispatcher", image=WORKER_IMAGE, network="r1-cairn_net",
                          label=WORKER_LABEL)

NODE_ID = "a" * 64
WORKER_ID = "e" * 64
OTHER_ID = "b" * 64
EXEC_ID = "c" * 64


def exec_body(cmd, **over):
    """The body the docker CLI writes for `docker exec <c> <cmd...>`."""
    body = {"User": "", "Privileged": False, "Tty": False, "AttachStdin": False,
            "AttachStderr": True, "AttachStdout": True, "Detach": False, "DetachKeys": "",
            "Env": None, "WorkingDir": "", "Cmd": cmd}
    body.update(over)
    return json.dumps(body).encode()


def worker_create(**over):
    """The body docker-py 7.1 writes for the dispatcher's containers.run()
    (captured from cairn v0.2.1's ContainerManager against rootless Podman)."""
    body = {"Tty": False, "OpenStdin": False, "StdinOnce": False, "AttachStdin": False,
            "AttachStdout": False, "AttachStderr": False, "Cmd": ["sleep", "infinity"],
            "Image": WORKER_IMAGE, "NetworkDisabled": False,
            "HostConfig": {"NetworkMode": "r1-cairn_net"}}
    body.update(over)
    return json.dumps(body).encode()


def worker_exec(cmd, **over):
    """docker-py's exec_create() body, as ManagedProcess sends it."""
    body = {"Container": WORKER_ID, "User": "", "Privileged": False, "Tty": False,
            "AttachStdin": False, "AttachStdout": True, "AttachStderr": True, "Cmd": cmd,
            "Env": ["PI_MODEL=qwen3-coder"]}
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


class CheckCairnDispatcherTest(unittest.TestCase):
    """cairn_dispatcher mode: every call cairn's ContainerManager makes, and
    the dangerous neighbours of each."""

    def allowed(self, method, target, body=b"", known=frozenset()):
        return proxy.check(DISPATCHER, method, target, body, known)

    def refused(self, method, target, body=b"", known=frozenset()):
        with self.assertRaises(proxy.Refused, msg=f"{method} {target} {body!r}"):
            proxy.check(DISPATCHER, method, target, body, known)

    def test_the_dispatchers_calls_are_forwarded(self):
        self.assertIsNone(self.allowed("GET", "/version").container)
        self.allowed("HEAD", "/_ping")
        for name in ("cairn-dispatch-proj-1", "cairn-startup-healthcheck-9af84308d6d6"):
            fwd = self.allowed("POST", f"/v1.41/containers/create?name={name}", worker_create())
            labels = json.loads(fwd.body)["Labels"]
            self.assertEqual(labels, {"kythira.cairn.worker": "r1"})
        fwd = self.allowed("GET", "/v1.41/containers/json?limit=-1&all=1&size=0&trunc_cmd=0")
        self.assertTrue(fwd.filter_list)
        for method, target, body in [
            ("GET", f"/v1.41/containers/{WORKER_ID}/json", b""),
            ("GET", "/v1.41/containers/cairn-dispatch-proj-1/json", b""),
            ("POST", f"/v1.41/containers/{WORKER_ID}/start", b""),
            ("POST", f"/v1.41/containers/{WORKER_ID}/stop?t=1", b""),
            ("DELETE", f"/v1.41/containers/{WORKER_ID}?v=False&link=False&force=True", b""),
            ("PUT", f"/v1.41/containers/{WORKER_ID}/archive?path=%2Froot", b"tar bytes"),
        ]:
            fwd = self.allowed(method, target, body)
            self.assertTrue(fwd.hide_missing, target)
            self.assertIsNotNone(fwd.container, target)
        for cmd in (["timeout", "-k", "5s", "300s", "pi", "-p", "go"], ["kill", "-KILL", "1088"],
                    ["sh", "-lc", "kill -KILL 1088"]):
            fwd = self.allowed("POST", f"/v1.41/containers/{WORKER_ID}/exec", worker_exec(cmd))
            self.assertTrue(fwd.records_exec and fwd.exec_container)
        self.allowed("POST", f"/v1.41/containers/{WORKER_ID}/exec",
                     worker_exec(["id"], AttachStdout=False, AttachStderr=False, Env=None))
        known = frozenset({EXEC_ID})
        self.assertTrue(self.allowed("POST", f"/exec/{EXEC_ID}/start",
                                     b'{"Tty": false, "Detach": false}', known).hijack)
        self.allowed("GET", f"/exec/{EXEC_ID}/json", b"", known)

    def test_create_may_not_escape_the_worker_shape(self):
        name = "cairn-dispatch-p"
        for body in [
            worker_create(Image="kythira-chaos-node:dev"),
            worker_create(Cmd=["sh", "-c", "id"]),
            worker_create(Cmd=None),
            worker_create(HostConfig={"NetworkMode": "host"}),
            worker_create(HostConfig={"NetworkMode": "r1-raft_net"}),
            worker_create(HostConfig={}),
            worker_create(HostConfig=None),
            worker_create(HostConfig={"NetworkMode": "r1-cairn_net", "Privileged": True}),
            worker_create(HostConfig={"NetworkMode": "r1-cairn_net", "Binds": ["/:/host"]}),
            worker_create(HostConfig={"NetworkMode": "r1-cairn_net", "CapAdd": ["NET_ADMIN"]}),
            worker_create(HostConfig={"NetworkMode": "r1-cairn_net", "PidMode": "host"}),
            worker_create(HostConfig={"NetworkMode": "r1-cairn_net", "Devices": []}),
            worker_create(Volumes={"/host": {}}),
            worker_create(User="0"),
            worker_create(Entrypoint=["sh"]),
            worker_create(Env=["X=1"]),
            worker_create(Tty=True),
            worker_create(OpenStdin=True),
            worker_create(NetworkDisabled=True),
            worker_create(AttachStdout="yes"),
            worker_create(Labels=["x"]),
            worker_create(Labels={"x": 1}),
            b"", b"[]", b"{",
        ]:
            self.refused("POST", f"/containers/create?name={name}", body)
        for target in ("/containers/create", "/containers/create?name=engine",
                       "/containers/create?name=cairn-dispatch-", "/containers/create?name=x"
                       "&name=cairn-dispatch-p", "/containers/create?name=cairn-dispatch-a/b",
                       "/containers/create?name=cairn-dispatch-p&platform=x"):
            self.refused("POST", target, worker_create())
        # A label the dispatcher could set cannot impersonate a scope.
        fwd = self.allowed("POST", f"/containers/create?name={name}",
                           worker_create(Labels={"kythira.cairn.worker": "other", "a": "b"}))
        self.assertEqual(json.loads(fwd.body)["Labels"],
                         {"kythira.cairn.worker": "r1", "a": "b"})

    def test_exec_runs_unprivileged_as_the_images_user(self):
        target = f"/containers/{WORKER_ID}/exec"
        for over in ({"Privileged": True}, {"User": "0"}, {"User": "root"}, {"Tty": True},
                     {"AttachStdin": True}, {"DetachKeys": "ctrl-p"}, {"Cmd": []},
                     {"Cmd": "id"}, {"Cmd": ["id", 1]}, {"Env": "X=1"}, {"Env": [1]},
                     {"WorkingDir": 1}, {"Container": 7}, {"HostConfig": {}},
                     {"Capabilities": ["CAP_SYS_ADMIN"]}):
            self.refused("POST", target, worker_exec(["id"], **over))
        self.refused("POST", target + "?x=1", worker_exec(["id"]))
        self.refused("POST", f"/exec/{'d' * 64}/start", b"{}", frozenset({EXEC_ID}))

    def test_every_other_call_is_refused(self):
        w = WORKER_ID
        for method, target, body in [
            ("GET", "/containers/json?size=1", b""),
            ("GET", "/containers/json?all=1&before=x", b""),
            ("GET", f"/containers/{w}/json?size=1", b""),
            ("POST", f"/containers/{w}/kill", b""),
            ("POST", f"/containers/{w}/pause", b""),
            ("POST", f"/containers/{w}/restart", b""),
            ("POST", f"/containers/{w}/start", b'{"Binds":["/:/host"]}'),
            ("POST", f"/containers/{w}/start?detachKeys=x", b""),
            ("POST", f"/containers/{w}/stop?t=-1", b""),
            ("POST", f"/containers/{w}/stop?signal=HUP", b""),
            ("DELETE", f"/containers/{w}?v=True", b""),
            ("DELETE", f"/containers/{w}?link=1", b""),
            ("DELETE", f"/containers/{w}?x=1", b""),
            ("PUT", f"/containers/{w}/archive?path=root", b"x"),
            ("PUT", f"/containers/{w}/archive", b"x"),
            ("PUT", f"/containers/{w}/archive?path=/&copyUIDGID=1", b"x"),
            ("GET", f"/containers/{w}/archive?path=/", b""),
            ("GET", f"/containers/{w}/logs?stdout=1", b""),
            ("POST", f"/containers/{w}/attach?stream=1", b""),
            ("POST", f"/containers/{w}/update", b'{"Privileged":true}'),
            ("POST", f"/containers/{w}/commit", b""),
            ("GET", f"/containers/{w}/export", b""),
            ("POST", "/containers/prune", b""),
            ("GET", "/version?x=1", b""),
            ("GET", "/info", b""),
            ("GET", "/images/json", b""),
            ("POST", "/images/create?fromImage=alpine", b""),
            ("POST", "/build", b""),
            ("GET", "/networks", b""),
            ("POST", "/networks/r1-cairn_net/connect", b'{"Container":"x"}'),
            ("POST", "/volumes/create", b"{}"),
            ("GET", "/events", b""),
            ("POST", "/libpod/containers/create", b"{}"),
            ("GET", "/", b""),
        ]:
            self.refused(method, target, body)

    def test_list_reply_keeps_only_labelled_containers(self):
        data = json.dumps([
            {"Id": WORKER_ID, "Names": ["/cairn-dispatch-p"],
             "Labels": {"kythira.cairn.worker": "r1"}},
            {"Id": NODE_ID, "Names": ["/r1-node1"], "Labels": {"kythira.cairn.run": "r1"}},
            {"Id": OTHER_ID, "Names": ["/cairn-dispatch-q"],
             "Labels": {"kythira.cairn.worker": "r2"}},
            {"Id": EXEC_ID, "Names": ["/x"], "Labels": None},
            "junk",
        ]).encode()
        kept = json.loads(proxy.filter_list(DISPATCHER, data))
        self.assertEqual([c["Id"] for c in kept], [WORKER_ID])
        with self.assertRaises(proxy.Upstream):
            proxy.filter_list(DISPATCHER, b"{}")


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
        self.assertEqual(DISPATCHER.missing(), [])
        self.assertEqual(len(proxy.Config(mode="cairn_dispatcher").missing()), 3)
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
        # A remove has no segment after the container.
        self.assertEqual(proxy.with_container_id("/v1.41/containers/cairn-dispatch-p?force=True",
                                                 "cairn-dispatch-p", WORKER_ID),
                         f"/v1.41/containers/{WORKER_ID}?force=True")
        self.assertEqual(proxy.with_container_id("/containers/node1x/start", "node1", NODE_ID),
                         "/containers/node1x/start")


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
        "cairn-dispatch-p1": (WORKER_ID, {"kythira.cairn.worker": "r1"}),
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
        if self.command == "POST" and path == "/containers/create":
            self._send(201, json.dumps({"Id": "f" * 64}).encode())
            return
        if self.command == "GET" and path == "/containers/json":
            self._send(200, json.dumps([{"Id": cid, "Names": ["/" + name], "Labels": labels}
                                        for name, (cid, labels) in self.containers.items()])
                       .encode())
            return
        if parts[0] == "containers" and len(parts) == 2 and self.command == "DELETE":
            self._send(204 if self._find(parts[1]) else 404)
            return
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
            elif parts[2] == "archive":
                self._send(200)
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
        # Headers only, then read: the proxy answers and hangs up before any
        # body, so a client still sending chunks would race that hang-up
        # (EPIPE) and fail the test at random.
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as sock:
            sock.sendall(b"POST /containers/chaos_node_1/exec HTTP/1.1\r\n"
                         b"Host: 127.0.0.1\r\nContent-Type: application/json\r\n"
                         b"Transfer-Encoding: chunked\r\n\r\n")
            resp = http.client.HTTPResponse(sock)
            resp.begin()
            self.assertEqual(resp.status, 411)
        self.assertEqual(self.upstream_calls(), [])


class DispatcherThroughProxyTest(ThroughProxyTest):
    """cairn_dispatcher mode against the fake API."""

    def setUp(self):
        super().setUp()
        self.proxy.shutdown()
        self.proxy.server_close()
        self.proxy = proxy.serve(DISPATCHER, self.api.server_address, 0, host="127.0.0.1")
        self.port = self.proxy.server_address[1]
        threading.Thread(target=self.proxy.serve_forever, daemon=True).start()

    def test_a_refused_call_never_reaches_the_api(self):
        status, _, _ = self.request("POST", "/containers/create?name=cairn-dispatch-p",
                                    worker_create(HostConfig={"NetworkMode": "host"}))
        self.assertEqual(status, 403)
        self.assertEqual(self.upstream_calls(), [])

    def test_out_of_scope_container_is_refused_after_inspect_only(self):
        # Hidden rather than refused: docker-py turns a 404 into NotFound,
        # which the dispatcher handles.
        for method, target, body in [
            ("GET", "/containers/chaos_node_1/json", b""),
            ("POST", "/containers/engine/start", b""),
            ("DELETE", f"/containers/{NODE_ID}?force=True", b""),
            ("POST", "/containers/chaos_node_1/exec", worker_exec(["id"])),
            ("PUT", "/containers/engine/archive?path=/", b"x"),
            ("GET", "/containers/missing/json", b""),
        ]:
            status, data, _ = self.request(method, target, body)
            self.assertEqual(status, 404, f"{method} {target}: {data!r}")
        for method, path, _ in self.upstream_calls():
            self.assertEqual(method, "GET")
            self.assertTrue(path.endswith("/json"), path)

    def test_in_scope_call_goes_upstream_by_id(self):
        status, _, _ = self.request("DELETE", "/v1.41/containers/cairn-dispatch-p1?force=True")
        self.assertEqual(status, 204)
        self.assertEqual(self.upstream_calls()[-1],
                         ("DELETE", f"/v1.41/containers/{WORKER_ID}?force=True", b""))
        status, _, _ = self.request("PUT", "/containers/cairn-dispatch-p1/archive?path=%2Froot",
                                    b"tar bytes")
        self.assertEqual(status, 200)
        self.assertEqual(self.upstream_calls()[-1],
                         ("PUT", f"/containers/{WORKER_ID}/archive?path=%2Froot", b"tar bytes"))

    def test_create_carries_the_label_and_list_hides_the_rest(self):
        status, _, _ = self.request("POST", "/v1.41/containers/create?name=cairn-dispatch-p2",
                                    worker_create())
        self.assertEqual(status, 201)
        sent = json.loads(self.upstream_calls()[-1][2])
        self.assertEqual(sent["Labels"], {"kythira.cairn.worker": "r1"})
        status, data, _ = self.request("GET", "/v1.41/containers/json?all=1")
        self.assertEqual(status, 200)
        self.assertEqual([c["Id"] for c in json.loads(data)], [WORKER_ID])

    def test_exec_round_trip(self):
        status, data, _ = self.request("POST", "/containers/cairn-dispatch-p1/exec",
                                       worker_exec(["pi", "-p", "x"], Container="cairn-dispatch-p1"))
        self.assertEqual(status, 201, data)
        method, path, sent = self.upstream_calls()[-1]
        self.assertEqual(path, f"/containers/{WORKER_ID}/exec")
        self.assertEqual(json.loads(sent)["Container"], WORKER_ID)
        status, data, _ = self.request("GET", f"/exec/{EXEC_ID}/json")
        self.assertEqual(status, 200)

    def test_ping_keeps_the_version_header(self):
        status, _, _ = self.request("GET", "/version")
        self.assertEqual(status, 500)  # forwarded; the fake has no /version

    def test_chunked_body_is_refused(self):
        super().test_chunked_body_is_refused()


if __name__ == "__main__":
    unittest.main()
