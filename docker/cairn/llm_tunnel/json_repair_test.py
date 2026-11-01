# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Tests for json_repair.py against a stand-in llama-server.

Run with: python3 -m unittest discover -s docker/cairn/llm_tunnel -p '*_test.py'
"""

import http.client
import http.server
import json
import os
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import json_repair  # noqa: E402

BARE = '{"accepted": true, "reason": "ok"}'


class RepairContentTest(unittest.TestCase):
    def test_bare_object_is_left_alone(self):
        self.assertIsNone(json_repair.repair_content(BARE))

    def test_empty_think_block_is_removed(self):
        self.assertEqual(json_repair.repair_content("<think></think>\n\n" + BARE), BARE)

    def test_full_think_block_is_removed(self):
        text = "<think>The cluster has a leader, so {accept}.</think>\n" + BARE
        self.assertEqual(json_repair.repair_content(text), BARE)

    def test_fence_is_removed(self):
        self.assertEqual(json_repair.repair_content("```json\n" + BARE + "\n```"), BARE)

    def test_prose_then_trailing_object(self):
        text = 'I checked cluster.status and node 1 leads {term 3}.\n\n' + BARE
        self.assertEqual(json_repair.repair_content(text), BARE)

    def test_nested_object_is_kept_whole(self):
        nested = '{"accepted": false, "detail": {"why": "no leader"}}'
        self.assertEqual(json_repair.repair_content("Result:\n" + nested), nested)

    def test_whitespace_around_object(self):
        self.assertEqual(json_repair.repair_content("\n  " + BARE + "\n"), BARE)

    def test_prose_without_trailing_object_is_left_alone(self):
        for text in ("I will now call chaos tools.", 'Use {"node": 2} next, then check.', "", "[1, 2]"):
            self.assertIsNone(json_repair.repair_content(text), text)

    def test_non_string_content_is_left_alone(self):
        self.assertIsNone(json_repair.repair_content(None))
        self.assertIsNone(json_repair.repair_content([{"type": "text", "text": BARE}]))

    def test_unrepairable_final_answer_is_reported_not_changed(self):
        reply = {"choices": [{"message": {"content": "I cannot decide yet."}}]}
        self.assertEqual(json_repair.repair_reply(reply), [{"repaired": False, "content": "I cannot decide yet."}])
        self.assertEqual(reply["choices"][0]["message"]["content"], "I cannot decide yet.")

    def test_bare_answer_gives_no_note(self):
        self.assertEqual(json_repair.repair_reply({"choices": [{"message": {"content": BARE}}]}), [])

    def test_tool_call_choices_are_not_touched(self):
        reply = {"choices": [{"message": {"content": "<think></think>" + BARE, "tool_calls": [{"id": "t"}]}}]}
        self.assertEqual(json_repair.repair_reply(reply), [])
        self.assertEqual(reply["choices"][0]["message"]["content"], "<think></think>" + BARE)


class FakeLlama(http.server.BaseHTTPRequestHandler):
    """Answers chat completions with whatever the test queued."""

    protocol_version = "HTTP/1.1"
    reply = None
    seen = []

    def log_message(self, *args):
        pass

    def do_GET(self):
        data = b'{"status":"ok"}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        FakeLlama.seen.append(body)
        data = json.dumps(FakeLlama.reply).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def completion(content, tool_calls=None):
    msg = {"role": "assistant", "content": content}
    if tool_calls:
        msg["tool_calls"] = tool_calls
    return {
        "id": "c1",
        "object": "chat.completion",
        "created": 1,
        "model": "m",
        "choices": [{"index": 0, "message": msg, "finish_reason": "tool_calls" if tool_calls else "stop"}],
        "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
    }


class ProxyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.llama = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeLlama)
        threading.Thread(target=cls.llama.serve_forever, daemon=True).start()
        upstream = f"http://127.0.0.1:{cls.llama.server_address[1]}"
        cls.proxy = json_repair.Server(("127.0.0.1", 0), upstream, enabled=True)
        threading.Thread(target=cls.proxy.serve_forever, daemon=True).start()
        cls.port = cls.proxy.server_address[1]

    @classmethod
    def tearDownClass(cls):
        for s in (cls.proxy, cls.llama):
            s.shutdown()
            s.server_close()

    def setUp(self):
        FakeLlama.seen = []

    def post(self, body):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        conn.request("POST", "/v1/chat/completions", body=json.dumps(body), headers={"Content-Type": "application/json"})
        resp = conn.getresponse()
        data = resp.read()
        conn.close()
        return resp, data

    def test_non_streamed_reply_is_repaired(self):
        FakeLlama.reply = completion("<think></think>\n\n" + BARE)
        resp, data = self.post({"messages": [], "stream": False})
        self.assertEqual(resp.status, 200)
        self.assertEqual(json.loads(data)["choices"][0]["message"]["content"], BARE)

    def test_streamed_request_is_asked_unstreamed_and_resent_as_events(self):
        FakeLlama.reply = completion("<think></think>" + BARE)
        resp, data = self.post({"messages": [], "stream": True, "stream_options": {"include_usage": True}})
        self.assertEqual(resp.status, 200)
        self.assertEqual(resp.getheader("Content-Type"), "text/event-stream")
        self.assertFalse(FakeLlama.seen[0]["stream"])
        self.assertNotIn("stream_options", FakeLlama.seen[0])
        events = [line[len("data: "):] for line in data.decode().split("\n\n") if line.startswith("data: ")]
        self.assertEqual(events[-1], "[DONE]")
        chunks = [json.loads(e) for e in events[:-1]]
        self.assertEqual(chunks[0]["object"], "chat.completion.chunk")
        self.assertEqual(chunks[0]["choices"][0]["delta"]["content"], BARE)
        self.assertEqual(chunks[1]["choices"][0]["finish_reason"], "stop")
        self.assertEqual(chunks[2]["usage"]["total_tokens"], 15)

    def test_streamed_tool_calls_keep_their_index(self):
        calls = [{"id": "a", "type": "function", "function": {"name": "bash", "arguments": "{\"cmd\":\"chaos tools\"}"}}]
        FakeLlama.reply = completion(None, calls)
        _, data = self.post({"messages": [], "stream": True})
        first = json.loads(data.decode().split("\n\n")[0][len("data: "):])
        self.assertEqual(first["choices"][0]["delta"]["tool_calls"][0]["index"], 0)
        self.assertEqual(first["choices"][0]["delta"]["tool_calls"][0]["function"]["name"], "bash")

    def test_other_paths_pass_through(self):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        conn.request("GET", "/health")
        resp = conn.getresponse()
        self.assertEqual((resp.status, json.loads(resp.read())), (200, {"status": "ok"}))
        conn.close()

    def test_repair_off_passes_replies_through(self):
        off = json_repair.Server(("127.0.0.1", 0), f"http://127.0.0.1:{self.llama.server_address[1]}", enabled=False)
        threading.Thread(target=off.serve_forever, daemon=True).start()
        try:
            FakeLlama.reply = completion("<think></think>" + BARE)
            conn = http.client.HTTPConnection("127.0.0.1", off.server_address[1], timeout=10)
            conn.request("POST", "/v1/chat/completions", body=json.dumps({"messages": []}))
            content = json.loads(conn.getresponse().read())["choices"][0]["message"]["content"]
            conn.close()
            self.assertEqual(content, "<think></think>" + BARE)
        finally:
            off.shutdown()
            off.server_close()


if __name__ == "__main__":
    unittest.main()
