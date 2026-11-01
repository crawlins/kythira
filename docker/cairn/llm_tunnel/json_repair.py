#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""The llm-tunnel's reply repair (.kiro/specs/cairn-chaos-testing/ task 11).

Cairn's dispatcher parses some of a worker's answers as a bare JSON object;
its bootstrap step refuses anything else ("bootstrap parse failed ...
accepted must be true or false"). In the M1 campaigns a local model got the
JSON right but sometimes wrapped it: an empty or full <think> block from a
reasoning model, a ```json fence, or a line of prose before it. One such
reply in three bootstrap attempts ended a run. Cairn and Pi run unmodified
(task 1), so the fix sits in the model path instead: this proxy listens
where workers reach the model, forwards to llama-server through the SSH
tunnel, and unwraps a final answer whose JSON is plainly there.

The repair is deliberately narrow. Only a /v1/chat/completions choice with
text content and no tool calls is touched, and only when, after removing
<think> blocks and one surrounding code fence, what is left either is a
JSON object or ends with one. Anything else, including every tool call and
every reply that is prose with no trailing object, passes through
unchanged. A reply is never invented or edited inside: the content becomes
exactly the object text the model wrote.

Streaming: Pi asks for a stream. To see a whole answer before repairing it,
the proxy asks llama-server for a non-streamed reply and re-sends it as one
server-sent-events chunk, with any tool calls as indexed deltas, then a
finish chunk and [DONE]. Workers see slower first tokens and nothing else.

Environment: JSON_REPAIR_PORT (default 8080), JSON_REPAIR_UPSTREAM (default
http://127.0.0.1:8081), JSON_REPAIR (default on; "off" passes everything
through, for comparison runs). Each repair is logged to stderr with what was
removed, so a run's log shows how often it fired, and so is every final
answer that is still not a bare object, with its text.
"""

import http.client
import http.server
import json
import os
import re
import sys
import urllib.parse

MAX_BODY = 16 * 1024 * 1024

THINK = re.compile(r"<think>.*?</think>", re.DOTALL)
FENCE = re.compile(r"\A```[A-Za-z0-9_-]*[ \t]*\n(.*)\n```\Z", re.DOTALL)


def repair_content(text):
    """Return the bare JSON object `text` carries, or None to leave it."""
    if not isinstance(text, str):
        return None
    body = THINK.sub("", text).strip()
    fenced = FENCE.match(body)
    if fenced:
        body = fenced.group(1).strip()
    if body.startswith("{"):
        try:
            if isinstance(json.loads(body), dict):
                return None if body == text else body
        except ValueError:
            pass
    # A trailing object: the leftmost '{' from which one object runs
    # exactly to the end of the text, which is the outermost such object.
    decoder = json.JSONDecoder()
    for i, ch in enumerate(body):
        if ch != "{":
            continue
        try:
            obj, end = decoder.raw_decode(body, i)
        except ValueError:
            continue
        if end == len(body) and isinstance(obj, dict):
            return body[i:]
    return None


def is_object(text):
    """True if `text` is exactly one JSON object."""
    try:
        return isinstance(json.loads(text), dict)
    except (TypeError, ValueError):
        return False


def repair_reply(reply):
    """Repair every eligible choice in a chat completion; return the notes.

    A note says either what a repair removed, or, for a final answer that
    is still not a bare object, what the model wrote (its first 2,000
    characters). The second kind is how a run's log shows exactly what
    text failed Cairn's parse; the dispatcher's own preview is cut off
    before the reply.
    """
    notes = []
    for choice in reply.get("choices") or []:
        msg = choice.get("message") or {}
        if msg.get("tool_calls"):
            continue
        fixed = repair_content(msg.get("content"))
        if fixed is not None:
            notes.append({"repaired": True, "removed_chars": len(msg["content"]) - len(fixed), "before": msg["content"][:200]})
            msg["content"] = fixed
        elif isinstance(msg.get("content"), str) and not is_object(msg["content"]):
            notes.append({"repaired": False, "content": msg["content"][:2000]})
    return notes


def sse_chunks(reply):
    """Re-send a non-streamed completion as OpenAI stream chunks."""
    base = {k: reply[k] for k in ("id", "created", "model", "system_fingerprint") if k in reply}
    base["object"] = "chat.completion.chunk"
    out = []
    for choice in reply.get("choices") or []:
        msg = choice.get("message") or {}
        delta = {"role": msg.get("role", "assistant")}
        if msg.get("content") is not None:
            delta["content"] = msg["content"]
        if msg.get("reasoning_content"):
            delta["reasoning_content"] = msg["reasoning_content"]
        if msg.get("tool_calls"):
            delta["tool_calls"] = [dict(tc, index=n) for n, tc in enumerate(msg["tool_calls"])]
        idx = choice.get("index", 0)
        out.append(dict(base, choices=[{"index": idx, "delta": delta, "finish_reason": None}]))
        out.append(dict(base, choices=[{"index": idx, "delta": {}, "finish_reason": choice.get("finish_reason", "stop")}]))
    if "usage" in reply:
        out.append(dict(base, choices=[], usage=reply["usage"]))
    return b"".join(b"data: " + json.dumps(c).encode() + b"\n\n" for c in out) + b"data: [DONE]\n\n"


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, upstream, enabled):
        u = urllib.parse.urlsplit(upstream)
        self.up_host, self.up_port = u.hostname, u.port or 80
        self.enabled = enabled
        super().__init__(address, Handler)


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "kythira-llm-json-repair"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("json_repair: " + (fmt % args) + "\n")

    def _upstream(self, method, body, headers):
        conn = http.client.HTTPConnection(self.server.up_host, self.server.up_port, timeout=900)
        conn.request(method, self.path, body=body, headers=headers)
        return conn, conn.getresponse()

    def _relay(self, resp):
        """Pass an upstream response through, streaming it as it arrives."""
        self.send_response(resp.status)
        chunked = resp.getheader("Content-Length") is None
        for k, v in resp.getheaders():
            if k.lower() in ("connection", "transfer-encoding", "content-length", "keep-alive"):
                continue
            self.send_header(k, v)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", resp.getheader("Content-Length"))
        self.end_headers()
        while True:
            data = resp.read1(65536) if chunked else resp.read(65536)
            if not data:
                break
            if chunked:
                self.wfile.write(b"%x\r\n%s\r\n" % (len(data), data))
            else:
                self.wfile.write(data)
        if chunked:
            self.wfile.write(b"0\r\n\r\n")

    def _reply(self, status, ctype, data):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _forward(self, method):
        length = int(self.headers.get("Content-Length") or 0)
        if length > MAX_BODY:
            self.close_connection = True
            self._reply(413, "application/json", b'{"error":"request too large"}')
            return
        body = self.rfile.read(length) if length else None
        headers = {k: v for k, v in self.headers.items() if k.lower() not in ("host", "content-length", "connection")}
        repairable = (
            self.server.enabled
            and method == "POST"
            and self.path.split("?")[0].rstrip("/").endswith("/chat/completions")
        )
        req = None
        if repairable:
            try:
                req = json.loads(body or b"")
            except ValueError:
                req = None
            if not isinstance(req, dict):
                repairable = False
        if not repairable:
            try:
                conn, resp = self._upstream(method, body, headers)
            except OSError as e:
                self._reply(502, "application/json", json.dumps({"error": f"upstream: {e}"}).encode())
                return
            try:
                self._relay(resp)
            finally:
                conn.close()
            return

        stream = bool(req.get("stream"))
        if stream:
            req["stream"] = False
            req.pop("stream_options", None)
        data = json.dumps(req).encode()
        try:
            conn, resp = self._upstream("POST", data, dict(headers, **{"Content-Type": "application/json"}))
        except OSError as e:
            self._reply(502, "application/json", json.dumps({"error": f"upstream: {e}"}).encode())
            return
        try:
            raw = resp.read()
            status = resp.status
        finally:
            conn.close()
        try:
            reply = json.loads(raw)
        except ValueError:
            reply = None
        if status != 200 or not isinstance(reply, dict):
            self._reply(status, "application/json", raw)
            return
        for note in repair_reply(reply):
            kind = "repaired " if note.pop("repaired") else "not JSON, passed through "
            sys.stderr.write("json_repair: " + kind + json.dumps(note) + "\n")
        if stream:
            self._reply(200, "text/event-stream", sse_chunks(reply))
        else:
            self._reply(200, "application/json", json.dumps(reply).encode())

    def do_GET(self):
        self._forward("GET")

    def do_POST(self):
        self._forward("POST")


def main():
    port = int(os.environ.get("JSON_REPAIR_PORT", "8080"))
    upstream = os.environ.get("JSON_REPAIR_UPSTREAM", "http://127.0.0.1:8081")
    enabled = os.environ.get("JSON_REPAIR", "on") != "off"
    server = Server(("0.0.0.0", port), upstream, enabled)
    sys.stderr.write(f"json_repair: :{port} -> {upstream}, repair {'on' if enabled else 'off'}\n")
    server.serve_forever()


if __name__ == "__main__":
    main()
