#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Differential test of Kythira's Kinesis API against recordings of AWS.

.kiro/specs/kinesis-compatible-streams/ task 17 (Requirement 17.1). There is
no emulator in the loop (Clark chose recordings only): a seeded, random but
reproducible operation mix is run once against real AWS Kinesis and the
normalised answers are written to a recording; the same mix is then run
against Kythira and compared step by step.

    kinesis_differential.py run --real-aws --seed N --steps M --out FILE
    kinesis_differential.py run --endpoint URL --seed N --steps M --out FILE
    kinesis_differential.py compare --endpoint URL --recording FILE|DIR [--allow FILE]
    kinesis_differential.py self-check --endpoints URL,URL --seed N --steps M

The mix's choices depend only on the seed and on normalised answers, so two
services that answer alike are sent the same requests. Normalising replaces
what legitimately differs between runs: sequence numbers become tokens in
order of first appearance (identity is compared, not digits), iterators,
paging tokens, timestamps and MillisBehindLatest become placeholders, and the
account, region and stream name are replaced in every string. Errors compare
as type and HTTP status; their messages are compared only with
--strict-messages, and otherwise listed as notes. A comparison stops at the
first step whose request differs, since everything after it diverges.

The mix covers PutRecord(s) (with ExplicitHashKey and
SequenceNumberForOrdering), every iterator type, paged reads to the tip,
ListShards, DescribeStream(Summary), tags, retention, SplitShard,
MergeShards, UpdateShardCount and deliberate errors. It leaves out what costs
by the hour on AWS or needs other services: enhanced fan-out, on-demand mode
and KMS encryption. It keeps at most 4 open shards and deletes its stream at
the end, even after a failure.

--real-aws talks to AWS with the ambient credentials (AWS_PROFILE etc.) and
costs a few cents per run. It needs Clark's go each time.
"""

import argparse
import base64
import json
import os
import random
import re
import sys
import time

TOOL_VERSION = 1
KEYS = ["k%d" % i for i in range(8)]
MAX_HASH = (1 << 128) - 1
SEQ_FIELDS = {
    "SequenceNumber",
    "StartingSequenceNumber",
    "EndingSequenceNumber",
    "ContinuationSequenceNumber",
    "SequenceNumberForOrdering",
}
PLACEHOLDERS = {
    "ShardIterator": "<iterator>",
    "NextShardIterator": "<iterator>",
    "NextToken": "<token>",
    "ApproximateArrivalTimestamp": "<time>",
    "StreamCreationTimestamp": "<time>",
    "MillisBehindLatest": "<millis>",
    "StreamId": "<stream-id>",
}


# ---- normalising ---------------------------------------------------------


class Normaliser:
    """Turns one run's raw answers into comparable values."""

    def __init__(self):
        self.tokens = {}  # raw sequence number -> token
        self.raw = {}  # token -> raw sequence number
        self.replace = []  # (text, placeholder), longest first

    def learn(self, text, placeholder):
        if text and all(t != text for t, _ in self.replace):
            self.replace.append((text, placeholder))
            self.replace.sort(key=lambda p: -len(p[0]))

    def token(self, seq):
        if seq not in self.tokens:
            t = "SEQ#%d" % len(self.tokens)
            self.tokens[seq] = t
            self.raw[t] = seq
        return self.tokens[seq]

    def text(self, s):
        for t, p in self.replace:
            s = s.replace(t, p)
        # The clock in AWS's future-timestamp message.
        return re.sub(r"currentTimestampInMillis: \d+", "currentTimestampInMillis: <now>", s)

    def value(self, v, key=None):
        if isinstance(v, dict):
            return {
                k: self.value(x, k) for k, x in sorted(v.items()) if k != "ResponseMetadata"
            }
        if isinstance(v, list):
            return [self.value(x) for x in v]
        if v is None:
            return None
        if key in PLACEHOLDERS:
            return PLACEHOLDERS[key]
        if key in SEQ_FIELDS and isinstance(v, str):
            return self.token(v)
        if isinstance(v, (bytes, bytearray)):
            return "b64:" + base64.b64encode(bytes(v)).decode()
        if hasattr(v, "isoformat"):
            return "<time>"
        if isinstance(v, str):
            return self.text(v)
        return v

    def error(self, e):
        r = e.response
        return {
            "error": r.get("Error", {}).get("Code", "?"),
            "status": r.get("ResponseMetadata", {}).get("HTTPStatusCode"),
            "message": self.text(r.get("Error", {}).get("Message", "") or ""),
        }


# ---- the operation mix ---------------------------------------------------


class Mix:
    """Runs the seeded mix against one client, recording normalised steps."""

    def __init__(self, client, seed, steps, stream, pace=0.2):
        self.c = client
        self.rng = random.Random(seed)
        self.seed = seed
        self.count = steps
        self.stream = stream
        self.pace = pace
        self.n = Normaliser()
        self.n.learn(stream, "STREAM")
        self.steps = []
        self.shards = []  # ListShards entries, raw
        self.key_seq = {}  # partition key -> token of its last record
        self.tokens_by_shard = {}  # shard id -> tokens seen there
        self.reshards = 0

    # One recorded call. Retries two things AWS does that are not answers
    # about the request: its control-plane rate limit, and a stream status
    # that lags behind DescribeStreamSummary's (ListShards can still see
    # CREATING or UPDATING for a few seconds after the summary says ACTIVE,
    # since AWS's control plane is eventually consistent). The mix only
    # calls with the stream ACTIVE, so neither is ever the expected answer.
    def call(self, op, request, record=True):
        method = getattr(self.c, _snake(op))
        for attempt in range(6):
            try:
                out = method(**_resolve(request, self.n.raw))
                result = self.n.value(out)
                break
            except Exception as e:  # botocore ClientError
                if not hasattr(e, "response"):
                    raise
                err = e.response.get("Error", {})
                limited = err.get("Code") in (
                    "LimitExceededException",
                    "ProvisionedThroughputExceededException",
                    "ThrottlingException",
                ) and "ate exceeded" in (err.get("Message") or "")
                if limited or _stale_status(err):
                    if attempt < 5:
                        time.sleep(0.5 * (2**attempt))
                        continue
                out, result = None, self.n.error(e)
                break
        if record:
            self.steps.append({"op": op, "request": _symbolic(request, self.n), "response": result})
        time.sleep(self.pace)
        return out, result

    def wait_active(self, settled=None):
        """Waits until the stream is ACTIVE, ListShards agrees and, when
        given, `settled(summary, shards)` holds.

        Not recorded: how long a stream takes to become ACTIVE differs. AWS
        can still answer ACTIVE with the old state just after a change it
        accepted (SplitShard, a retention change), so a caller that made a
        change passes `settled` to wait for the change itself.
        """
        deadline = time.time() + 300
        while True:
            summary = self.c.describe_stream_summary(StreamName=self.stream)["StreamDescriptionSummary"]
            if summary["StreamStatus"] == "ACTIVE":
                try:
                    out = self.c.list_shards(StreamName=self.stream)
                    if settled is None or settled(summary, out["Shards"]):
                        self.shards = out["Shards"]
                        return self.n.value(out)
                except Exception as e:  # botocore ClientError
                    if not (hasattr(e, "response") and _stale_status(e.response.get("Error", {}))):
                        raise
            if time.time() > deadline:
                raise SystemExit("stream %s never became ACTIVE" % self.stream)
            time.sleep(1)

    def wait_closed(self, shard_ids):
        """Waits until reads of each shard end (no NextShardIterator).

        Not recorded. AWS's data plane lags its control plane: for a while
        after ListShards shows a parent closed, writes still land in it and
        reads of it do not end. Kythira closes it at once.
        """
        deadline = time.time() + 300
        for shard in shard_ids:
            while True:
                it = self.c.get_shard_iterator(StreamName=self.stream, ShardId=shard,
                                               ShardIteratorType="LATEST")["ShardIterator"]
                if not self.c.get_records(ShardIterator=it, Limit=1).get("NextShardIterator"):
                    break
                if time.time() > deadline:
                    raise SystemExit("shard %s of %s never closed" % (shard, self.stream))
                time.sleep(1)

    def open_shards(self):
        return [s for s in self.shards if "EndingSequenceNumber" not in s["SequenceNumberRange"]]

    def note_put(self, key, shard, seq):
        t = self.n.token(seq)
        self.key_seq[key] = t
        self.tokens_by_shard.setdefault(shard, []).append(t)

    # ---- steps ----

    def put_record(self):
        key = self.rng.choice(KEYS)
        data = bytes(self.rng.randrange(256) for _ in range(self.rng.randint(1, 48)))
        req = {"StreamName": self.stream, "PartitionKey": key, "Data": data}
        roll = self.rng.random()
        if roll < 0.2:
            req["ExplicitHashKey"] = str(self.rng.randint(0, MAX_HASH))
        elif roll < 0.45 and key in self.key_seq:
            req["SequenceNumberForOrdering"] = {"$seq": self.key_seq[key]}
        out, _ = self.call("PutRecord", req)
        if out:
            self.note_put(key, out["ShardId"], out["SequenceNumber"])

    def put_records(self):
        recs = []
        for _ in range(self.rng.randint(1, 20)):
            key = self.rng.choice(KEYS)
            recs.append(
                {
                    "PartitionKey": key,
                    "Data": bytes(self.rng.randrange(256) for _ in range(self.rng.randint(1, 32))),
                }
            )
        out, _ = self.call("PutRecords", {"StreamName": self.stream, "Records": recs})
        if out:
            for r, entry in zip(recs, out["Records"]):
                if "SequenceNumber" in entry:
                    self.note_put(r["PartitionKey"], entry["ShardId"], entry["SequenceNumber"])

    def read_shard(self):
        if not self.shards:
            return
        shard = self.rng.choice(self.shards)["ShardId"]
        known = self.tokens_by_shard.get(shard, [])
        kinds = ["TRIM_HORIZON", "LATEST", "AT_TIMESTAMP"] + (
            ["AT_SEQUENCE_NUMBER", "AFTER_SEQUENCE_NUMBER"] if known else []
        )
        kind = self.rng.choice(kinds)
        req = {"StreamName": self.stream, "ShardId": shard, "ShardIteratorType": kind}
        if kind in ("AT_SEQUENCE_NUMBER", "AFTER_SEQUENCE_NUMBER"):
            req["StartingSequenceNumber"] = {"$seq": self.rng.choice(known)}
        if kind == "AT_TIMESTAMP":
            req["Timestamp"] = 0 if self.rng.random() < 0.5 else 4102444800  # 1970, 2100
        out, _ = self.call("GetShardIterator", req)
        if not out:
            return
        it = out["ShardIterator"]
        limit = self.rng.randint(1, 50)
        records, last = [], None
        for _ in range(40):
            time.sleep(0.21)  # 5 GetRecords per second per shard
            try:
                page = self.c.get_records(ShardIterator=it, Limit=limit)
            except Exception as e:
                if hasattr(e, "response"):
                    last = self.n.error(e)
                    break
                raise
            records += page["Records"]
            last = page
            it = page.get("NextShardIterator")
            if not it or (not page["Records"] and page.get("MillisBehindLatest", 0) == 0):
                break
        summary = {
            "records": self.n.value(records),
            "end": "error" if isinstance(last, dict) and "error" in last else
                   ("closed" if last is not None and not last.get("NextShardIterator") else "tip"),
        }
        if isinstance(last, dict) and "error" in last:
            summary["error"] = last
        elif last is not None and "ChildShards" in last:
            summary["ChildShards"] = self.n.value(last["ChildShards"])
        self.steps.append({"op": "ReadShard", "request": {"Limit": limit}, "response": summary})

    def describe(self):
        if self.rng.random() < 0.5:
            self.call("DescribeStreamSummary", {"StreamName": self.stream})
        else:
            self.call("ListShards", {"StreamName": self.stream})

    def tags(self):
        roll = self.rng.random()
        if roll < 0.5:
            tags = {"t%d" % self.rng.randrange(30): "v%d" % self.rng.randrange(5)
                    for _ in range(self.rng.randint(1, 12))}
            self.call("AddTagsToStream", {"StreamName": self.stream, "Tags": tags})
        elif roll < 0.75:
            keys = ["t%d" % self.rng.randrange(30) for _ in range(self.rng.randint(1, 5))]
            self.call("RemoveTagsFromStream", {"StreamName": self.stream, "TagKeys": keys})
        else:
            self.call("ListTagsForStream", {"StreamName": self.stream, "Limit": self.rng.randint(1, 10)})

    def retention(self):
        hours = self.rng.choice([12, 24, 25, 48, 168, 8760, 8761])
        op = self.rng.choice(["IncreaseStreamRetentionPeriod", "DecreaseStreamRetentionPeriod"])
        out, _ = self.call(op, {"StreamName": self.stream, "RetentionPeriodHours": hours})
        if out is not None:
            # AWS applies a retention change a little later; wait for it so
            # the next step sees it (not recorded).
            self.wait_active(lambda summary, _: summary["RetentionPeriodHours"] == hours)

    def reshard(self):
        if self.reshards >= 4:
            return self.describe()
        open_ = self.open_shards()
        before = sorted(s["ShardId"] for s in self.shards)
        roll = self.rng.random()
        if roll < 0.45 and len(open_) < 4:
            s = self.rng.choice(open_)
            lo, hi = int(s["HashKeyRange"]["StartingHashKey"]), int(s["HashKeyRange"]["EndingHashKey"])
            out, _ = self.call("SplitShard", {"StreamName": self.stream, "ShardToSplit": s["ShardId"],
                                              "NewStartingHashKey": str(lo + (hi - lo) // 2 + 1)})
        elif roll < 0.8 and len(open_) > 1:
            by_start = sorted(open_, key=lambda s: int(s["HashKeyRange"]["StartingHashKey"]))
            i = self.rng.randrange(len(by_start) - 1)
            out, _ = self.call("MergeShards", {"StreamName": self.stream,
                                               "ShardToMerge": by_start[i]["ShardId"],
                                               "AdjacentShardToMerge": by_start[i + 1]["ShardId"]})
        else:
            target = max(1, min(4, len(open_) + self.rng.choice([-1, 1])))
            out, _ = self.call("UpdateShardCount", {"StreamName": self.stream, "TargetShardCount": target,
                                                    "ScalingType": "UNIFORM_SCALING"})
        if out is not None:
            self.reshards += 1
            # Wait for the new shards themselves: AWS can answer ACTIVE
            # with the old table just after accepting a reshard.
            changed = self.wait_active(lambda _, shards: sorted(s["ShardId"] for s in shards) != before)
            self.wait_closed([s["ShardId"] for s in open_
                              if s["ShardId"] not in {x["ShardId"] for x in self.open_shards()}])
            self.steps.append({"op": "AfterReshard", "request": {}, "response": changed})

    def errors(self):
        kind = self.rng.randrange(8)
        s = self.stream
        if kind == 0:
            self.call("PutRecord", {"StreamName": s + "-missing", "PartitionKey": "k", "Data": b"x"})
        elif kind == 1:
            self.call("GetShardIterator", {"StreamName": s, "ShardId": "shardId-000000099999",
                                           "ShardIteratorType": "TRIM_HORIZON"})
        elif kind == 2:
            self.call("GetRecords", {"ShardIterator": "AAAA"})
        elif kind == 3:
            s0 = self.shards[0] if self.shards else None
            if s0:
                self.call("SplitShard", {"StreamName": s, "ShardToSplit": s0["ShardId"],
                                         "NewStartingHashKey": s0["HashKeyRange"]["StartingHashKey"]})
        elif kind == 4:
            self.call("PutRecord", {"StreamName": s, "PartitionKey": "k", "Data": b"x",
                                    "SequenceNumberForOrdering": "12x"})
        elif kind == 5:
            self.call("PutRecords", {"StreamName": s, "Records": [
                {"PartitionKey": "k", "Data": b"x"} for _ in range(501)]})
        elif kind == 6:
            self.call("PutRecord", {"StreamName": s, "PartitionKey": "k", "Data": b"x" * (1024 * 1024)})
        else:
            self.call("ListShards", {"StreamName": s, "NextToken": "garbage"})

    STEP_WEIGHTS = [
        ("put_record", 25), ("put_records", 15), ("read_shard", 18), ("describe", 8),
        ("tags", 8), ("retention", 5), ("reshard", 5), ("errors", 10),
    ]

    def run(self):
        shards = self.rng.randint(1, 3)
        try:
            self.call("CreateStream", {"StreamName": self.stream, "ShardCount": shards})
            self.steps.append({"op": "AfterCreate", "request": {}, "response": self.wait_active()})
            arn = self.c.describe_stream_summary(StreamName=self.stream)[
                "StreamDescriptionSummary"]["StreamARN"]
            parts = arn.split(":")
            self.n.learn(parts[4], "ACCOUNT")
            self.n.learn(parts[3], "REGION")
            names, weights = zip(*self.STEP_WEIGHTS)
            for _ in range(self.count):
                getattr(self, self.rng.choices(names, weights)[0])()
            self.call("DescribeStream", {"StreamName": self.stream})
        finally:
            try:
                self.call("DeleteStream", {"StreamName": self.stream, "EnforceConsumerDeletion": True})
            except Exception as e:  # never leave a billable stream behind silently
                print("DeleteStream failed for %s: %s" % (self.stream, e), file=sys.stderr)
        return self.steps


def _stale_status(err):
    """A ResourceInUseException that only says the stream is not yet ACTIVE."""
    msg = err.get("Message") or ""
    return err.get("Code") == "ResourceInUseException" and (
        "CREATING" in msg or "UPDATING" in msg)


def _snake(op):
    out = ""
    for ch in op:
        out += ("_" + ch.lower()) if ch.isupper() and out else ch.lower()
    return out


def _resolve(req, raw):
    if isinstance(req, dict):
        if set(req) == {"$seq"}:
            return raw[req["$seq"]]
        return {k: _resolve(v, raw) for k, v in req.items()}
    if isinstance(req, list):
        return [_resolve(v, raw) for v in req]
    return req


def _symbolic(req, n):
    if isinstance(req, dict):
        if set(req) == {"$seq"}:
            return req
        return {k: _symbolic(v, n) for k, v in sorted(req.items())}
    if isinstance(req, list):
        if len(req) > 50:
            return {"$list": len(req), "first": _symbolic(req[0], n)}
        return [_symbolic(v, n) for v in req]
    if isinstance(req, (bytes, bytearray)):
        if len(req) > 64:
            return {"$bytes": len(req)}
        return "b64:" + base64.b64encode(bytes(req)).decode()
    if isinstance(req, str):
        return n.text(req)
    return req


# ---- comparing -----------------------------------------------------------


def diff(a, b, path=""):
    """Paths where `a` (the recording) and `b` differ."""
    if isinstance(a, dict) and isinstance(b, dict):
        out = []
        for k in sorted(set(a) | set(b)):
            p = path + "." + k if path else k
            if k not in a:
                out.append((p, "<absent>", b[k]))
            elif k not in b:
                out.append((p, a[k], "<absent>"))
            else:
                out += diff(a[k], b[k], p)
        return out
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return [(path + ".length", len(a), len(b))]
        out = []
        for i, (x, y) in enumerate(zip(a, b)):
            out += diff(x, y, "%s[%d]" % (path, i))
        return out
    return [] if a == b else [(path, a, b)]


def compare(recorded, got, allow=(), strict_messages=False):
    """(failures, notes) comparing two step lists."""
    failures, notes = [], []
    allowed = {(x["op"], x["path"]) for x in allow}
    for i, (a, b) in enumerate(zip(recorded, got)):
        where = "step %d %s" % (i, a["op"])
        if a["op"] != b["op"] or a["request"] != b["request"]:
            failures.append("%s: requests diverge (recorded %s, got %s %s); stopping" % (
                where, json.dumps(a["request"])[:200], b["op"], json.dumps(b["request"])[:200]))
            return failures, notes
        for path, x, y in diff(a["response"], b["response"]):
            line = "%s: %s recorded %s, got %s" % (where, path, json.dumps(x)[:160], json.dumps(y)[:160])
            if (a["op"], path) in allowed:
                notes.append("allowed " + line)
            elif path.endswith("message") and not strict_messages:
                notes.append(line)
            else:
                failures.append(line)
    if len(recorded) != len(got):
        failures.append("step counts differ: recorded %d, got %d" % (len(recorded), len(got)))
    return failures, notes


# ---- the command line ----------------------------------------------------


def make_client(endpoint, region):
    import boto3
    from botocore.config import Config

    cfg = Config(retries={"max_attempts": 1}, parameter_validation=False,
                 connect_timeout=5, read_timeout=30)
    if endpoint:
        return boto3.client("kinesis", endpoint_url=endpoint, region_name=region, config=cfg)
    return boto3.client("kinesis", region_name=region, config=cfg)


def run_mix(endpoint, region, seed, steps):
    name = "kythira-diff-%d-%s" % (seed, os.urandom(3).hex())
    return Mix(make_client(endpoint, region), seed, steps, name).run()


def recordings(path):
    """`path` itself, or every aws-seed-*.json in the directory `path`."""
    if not os.path.isdir(path):
        return [path]
    found = sorted(os.path.join(path, f) for f in os.listdir(path)
                   if f.startswith("aws-seed-") and f.endswith(".json"))
    if not found:
        raise SystemExit("no aws-seed-*.json recordings in " + path)
    return found


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--endpoint")
    r.add_argument("--real-aws", action="store_true")
    r.add_argument("--region", default="us-east-1")
    r.add_argument("--seed", type=int, required=True)
    r.add_argument("--steps", type=int, default=150)
    r.add_argument("--out", required=True)
    c = sub.add_parser("compare")
    c.add_argument("--endpoint", required=True)
    c.add_argument("--region", default="us-east-1")
    c.add_argument("--recording", required=True)
    c.add_argument("--allow")
    c.add_argument("--strict-messages", action="store_true")
    s = sub.add_parser("self-check")
    s.add_argument("--endpoints", required=True)
    s.add_argument("--region", default="us-east-1")
    s.add_argument("--seed", type=int, default=7)
    s.add_argument("--steps", type=int, default=80)
    a = p.parse_args(argv)

    if a.cmd == "run":
        if bool(a.endpoint) == a.real_aws:
            p.error("give exactly one of --endpoint or --real-aws")
        steps = run_mix(a.endpoint, a.region, a.seed, a.steps)
        import botocore

        rec = {"version": TOOL_VERSION, "seed": a.seed, "steps_requested": a.steps,
               "target": "aws" if a.real_aws else "endpoint", "region": a.region,
               "botocore": botocore.__version__, "recorded_at": int(time.time()), "steps": steps}
        with open(a.out, "w") as f:
            json.dump(rec, f, indent=1, sort_keys=True)
            f.write("\n")
        print(json.dumps({"steps": len(steps), "out": a.out}))
        return 0
    if a.cmd == "compare":
        allow = []
        if a.allow:
            with open(a.allow) as f:
                allow = json.load(f)
        failures, notes = [], []
        for path in recordings(a.recording):
            with open(path) as f:
                rec = json.load(f)
            got = run_mix(a.endpoint, a.region, rec["seed"], rec["steps_requested"])
            fs, ns = compare(rec["steps"], got, allow, a.strict_messages)
            name = os.path.basename(path)
            failures += ["%s %s" % (name, x) for x in fs]
            notes += ["%s %s" % (name, x) for x in ns]
    else:
        e1, e2 = a.endpoints.split(",")[:2]
        first = run_mix(e1, a.region, a.seed, a.steps)
        second = run_mix(e2, a.region, a.seed, a.steps)
        failures, notes = compare(first, second, (), True)
    for n in notes:
        print("note: " + n)
    for f in failures:
        print("FAIL: " + f, file=sys.stderr)
    print(json.dumps({"failures": len(failures), "notes": len(notes)}))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
