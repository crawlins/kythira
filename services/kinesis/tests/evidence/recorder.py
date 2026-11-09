#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""A recording Kinesis stub: what clients put on the wire.

.kiro/specs/kinesis-compatible-streams/ task 1 ("recording stub and
checked-in wire evidence"). Each client in clients/ runs one scripted
scenario against this stub; the stub answers just enough of the Kinesis API
for the scenario to finish (one stream, one shard, records kept in memory)
and records every request it receives, normalised so a rerun with the same
client version produces the same file:

- the request line, the HTTP version and every header, with the values that
  change per call (signature, date, invocation id, request length when it
  only reflects random ids) replaced by a placeholder that still shows the
  value's shape;
- the body decoded by its content type (JSON 1.1 or CBOR 1.1), with bytes
  as {"$bytes": <base64>} and CBOR tags as {"$tag": n, "value": ...};
- the protocol, which the evidence tables summarise per client.

It answers in the protocol the request used. An operation it does not
script gets UnknownOperationException, which is also what the gateway
returns for an operation listed in doc/unsupported.md.

    recorder.py --port 0 --out FILE --client NAME   (prints the port)

The process records until it receives SIGTERM or SIGINT, or a request to
POST /__evidence/stop, then writes FILE.
"""

import argparse
import base64
import datetime
import hashlib
import http.server
import json
import os
import re
import signal
import ssl
import sys
import threading
import urllib.parse

import cbor2

JSON_CT = "application/x-amz-json-1.1"
CBOR_CT = "application/x-amz-cbor-1.1"
STREAM = "evidence"
SHARD = "shardId-000000000000"
ACCOUNT = "123456789012"
REGION = "us-east-1"
ARN = f"arn:aws:kinesis:{REGION}:{ACCOUNT}:stream/{STREAM}"
CREATED = datetime.datetime(2026, 11, 9, tzinfo=datetime.timezone.utc)

STS_IDENTITY = f"""<GetCallerIdentityResponse xmlns="https://sts.amazonaws.com/doc/2011-06-15/">
  <GetCallerIdentityResult>
    <Arn>arn:aws:iam::{ACCOUNT}:user/evidence</Arn>
    <UserId>AIDAEVIDENCE</UserId>
    <Account>{ACCOUNT}</Account>
  </GetCallerIdentityResult>
  <ResponseMetadata><RequestId>00000000-0000-0000-0000-000000000000</RequestId></ResponseMetadata>
</GetCallerIdentityResponse>
"""
STS_INVALID = """<ErrorResponse><Error><Type>Sender</Type><Code>InvalidAction</Code>
<Message>not scripted</Message></Error></ErrorResponse>
"""

# Header values that differ on every call. The placeholder keeps the shape.
VOLATILE = {
    "x-amz-date": "<amz-date>",
    "amz-sdk-invocation-id": "<uuid>",
    "x-amz-security-token": "<token>",
    "date": "<date>",
}
AUTH_RE = re.compile(r"Signature=[0-9a-f]+")
CRED_DATE_RE = re.compile(r"Credential=([^/]+)/\d{8}/")


def jsonable(v):
    """CBOR and JSON values as plain JSON, losslessly enough for evidence."""
    if isinstance(v, bytes):
        return {"$bytes": base64.b64encode(v).decode()}
    if isinstance(v, cbor2.CBORTag):
        return {"$tag": v.tag, "value": jsonable(v.value)}
    if isinstance(v, datetime.datetime):
        return {"$datetime": v.isoformat()}
    if isinstance(v, dict):
        return {str(k): jsonable(x) for k, x in v.items()}
    if isinstance(v, list):
        return [jsonable(x) for x in v]
    if isinstance(v, float) and v.is_integer() and abs(v) > 1e9:
        # Timestamps as epoch seconds: keep the type visible, drop the value.
        return {"$epoch_seconds": "<float>"}
    return v


class Stub:
    """One stream with one shard, enough for the scripted scenarios."""

    def __init__(self):
        self.lock = threading.Lock()
        self.exists = False
        self.records = []  # (seq, partition key, data bytes, arrival)
        self.seq = 49_650_000_000_000_000_000_000_000_000_000_000_000_000_000_000_000

    def shard(self):
        return {
            "ShardId": SHARD,
            "HashKeyRange": {"StartingHashKey": "0",
                             "EndingHashKey": str(2**128 - 1)},
            "SequenceNumberRange": {"StartingSequenceNumber": str(self.seq)},
        }

    def put(self, key, data):
        self.seq += 1
        self.records.append((str(self.seq), key, data, CREATED))
        return str(self.seq)

    def handle(self, op, req):
        with self.lock:
            return self._handle(op, req)

    def _handle(self, op, req):
        summary = {"StreamName": STREAM, "StreamARN": ARN, "StreamStatus": "ACTIVE",
                   "StreamModeDetails": {"StreamMode": "PROVISIONED"},
                   "RetentionPeriodHours": 24, "StreamCreationTimestamp": CREATED,
                   "EnhancedMonitoring": [{"ShardLevelMetrics": []}],
                   "EncryptionType": "NONE", "OpenShardCount": 1, "ConsumerCount": 0}
        if op == "CreateStream":
            self.exists = True
            return 200, {}
        if op == "DeleteStream":
            self.exists = False
            return 200, {}
        if op in ("DescribeStreamSummary", "DescribeStream", "ListShards") and not self.exists:
            return 400, {"__type": "ResourceNotFoundException",
                         "message": f"Stream {STREAM} under account {ACCOUNT} not found."}
        if op == "DescribeStreamSummary":
            return 200, {"StreamDescriptionSummary": summary}
        if op == "DescribeStream":
            desc = {k: v for k, v in summary.items()
                    if k not in ("OpenShardCount", "ConsumerCount")}
            desc.update({"Shards": [self.shard()], "HasMoreShards": False})
            return 200, {"StreamDescription": desc}
        if op == "ListShards":
            return 200, {"Shards": [self.shard()]}
        if op == "ListStreams":
            names = [STREAM] if self.exists else []
            return 200, {"StreamNames": names, "HasMoreStreams": False,
                         "StreamSummaries": [{"StreamName": STREAM, "StreamARN": ARN,
                                              "StreamStatus": "ACTIVE",
                                              "StreamModeDetails": {"StreamMode": "PROVISIONED"},
                                              "StreamCreationTimestamp": CREATED}]
                         if self.exists else []}
        if op == "PutRecord":
            seq = self.put(req.get("PartitionKey"), as_bytes(req.get("Data")))
            return 200, {"ShardId": SHARD, "SequenceNumber": seq, "EncryptionType": "NONE"}
        if op == "PutRecords":
            out = [{"ShardId": SHARD, "SequenceNumber": self.put(r.get("PartitionKey"),
                                                                 as_bytes(r.get("Data")))}
                   for r in req.get("Records", [])]
            return 200, {"FailedRecordCount": 0, "Records": out, "EncryptionType": "NONE"}
        if op == "GetShardIterator":
            return 200, {"ShardIterator": "AAAAAAAAAAE" + "x" * 40}
        if op == "GetRecords":
            recs = [{"SequenceNumber": s, "ApproximateArrivalTimestamp": t, "Data": d,
                     "PartitionKey": k, "EncryptionType": "NONE"}
                    for s, k, d, t in self.records]
            return 200, {"Records": recs, "NextShardIterator": "AAAAAAAAAAI" + "x" * 40,
                         "MillisBehindLatest": 0}
        if op == "DescribeLimits":
            return 200, {"ShardLimit": 500, "OpenShardCount": 1 if self.exists else 0,
                         "OnDemandStreamCount": 0, "OnDemandStreamCountLimit": 50}
        return 400, {"__type": "UnknownOperationException", "message": None}


def as_bytes(v):
    if isinstance(v, bytes):
        return v
    if isinstance(v, str):
        return base64.b64decode(v)
    return b""


def encode(ct, body):
    if ct == CBOR_CT:
        return cbor2.dumps(body, datetime_as_timestamp=True, timezone=datetime.timezone.utc)

    def default(o):
        if isinstance(o, datetime.datetime):
            return o.timestamp()
        if isinstance(o, bytes):
            return base64.b64encode(o).decode()
        raise TypeError(type(o))

    return json.dumps({k: v for k, v in body.items() if v is not None},
                      default=default).encode()


def normalise_headers(items):
    out = []
    for name, value in items:
        low = name.lower()
        if low in VOLATILE:
            value = VOLATILE[low]
        elif low == "authorization":
            value = CRED_DATE_RE.sub(r"Credential=\1/<yyyymmdd>/", AUTH_RE.sub(
                "Signature=<hex>", value))
        elif low == "host":
            value = re.sub(r":\d+$", ":<port>", value)
        elif low == "user-agent":
            value = re.sub(r"os/(linux|Linux)#\S+", r"os/\1#<kernel>", value)
            # KPL spells out uname: release and version.
            value = value.replace(os.uname().version, "<kernel-version>").replace(
                os.uname().release, "<kernel>")
        elif low == "x-amz-content-sha256" and re.fullmatch(r"[0-9a-f]{64}", value):
            value = "<sha256-hex>"
        out.append([name, value])
    return out


class Recorder:
    def __init__(self, client):
        self.client = client
        self.entries = []
        self.lock = threading.Lock()
        self.stub = Stub()
        self.done = threading.Event()

    def record(self, entry):
        with self.lock:
            self.entries.append(entry)

    def document(self):
        protocols = sorted({e["content_type"] for e in self.entries if e["content_type"]})
        return {"client": self.client, "requests": self.entries,
                "summary": {"operations": [e["operation"] for e in self.entries],
                            "content_types": protocols,
                            "http_versions": sorted({e["http_version"] for e in self.entries})}}


def make_handler(rec):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def do_POST(self):  # noqa: N802 (http.server's naming)
            if self.path == "/__evidence/stop":
                self.reply(200, "text/plain", b"stopping\n")
                rec.done.set()
                return
            length = int(self.headers.get("Content-Length", "0") or 0)
            raw = self.rfile.read(length) if length else b""
            ct = (self.headers.get("Content-Type") or "").split(";")[0].strip()
            target = self.headers.get("X-Amz-Target", "")
            op = target.split(".", 1)[1] if "." in target else target
            try:
                body = cbor2.loads(raw) if ct == CBOR_CT and raw else (
                    json.loads(raw) if raw else {})
            except ValueError:
                body = {"$undecodable": base64.b64encode(raw).decode()}
            if ct == "application/x-www-form-urlencoded":
                # STS Query (KPL asks GetCallerIdentity for the stream ARN).
                form = urllib.parse.parse_qs(raw.decode())
                op = "sts:" + form.get("Action", [""])[0]
                body = {k: v[0] for k, v in form.items()}
                self.record_and_reply_query(op, ct, raw, body)
                return
            status, resp = rec.stub.handle(op, body if isinstance(body, dict) else {})
            rec.record({
                "operation": op,
                "method": self.command,
                "path": self.path,
                "http_version": self.request_version,
                "content_type": ct,
                "headers": normalise_headers(self.headers.items()),
                "body_sha256": hashlib.sha256(raw).hexdigest() if raw else None,
                "body": jsonable(body),
                "response_status": status,
                "response_type": resp.get("__type") if status != 200 else None,
            })
            out_ct = CBOR_CT if ct == CBOR_CT else JSON_CT
            self.reply(status, out_ct, encode(out_ct, resp),
                       {"x-amzn-RequestId": "00000000-0000-0000-0000-000000000000"})

        def record_and_reply_query(self, op, ct, raw, body):
            ok = op == "sts:GetCallerIdentity"
            rec.record({
                "operation": op, "method": self.command, "path": self.path,
                "http_version": self.request_version, "content_type": ct,
                "headers": normalise_headers(self.headers.items()),
                "body_sha256": hashlib.sha256(raw).hexdigest() if raw else None,
                "body": body, "response_status": 200 if ok else 400,
                "response_type": None if ok else "InvalidAction",
            })
            xml = (STS_IDENTITY if ok else STS_INVALID).encode()
            self.reply(200 if ok else 400, "text/xml", xml)

        def reply(self, status, ct, payload, extra=None):
            self.send_response(status)
            self.send_header("Content-Type", ct)
            self.send_header("Content-Length", str(len(payload)))
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(payload)

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--out", required=True)
    ap.add_argument("--client", required=True)
    ap.add_argument("--stream-exists", action="store_true",
                    help="start with the stream created (producers that never create it)")
    ap.add_argument("--tls-cert", help="serve HTTPS with this PEM chain (KPL needs TLS)")
    ap.add_argument("--tls-key")
    args = ap.parse_args()
    rec = Recorder(args.client)
    rec.stub.exists = args.stream_exists
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(rec))
    if args.tls_cert:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(args.tls_cert, args.tls_key)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print(srv.server_address[1], flush=True)
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: rec.done.set())
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    rec.done.wait()
    srv.shutdown()
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(rec.document(), f, indent=1, sort_keys=False)
        f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
