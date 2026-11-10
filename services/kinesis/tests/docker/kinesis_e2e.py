#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""boto3 producer and polling consumer for the Kinesis container scenario.

.kiro/specs/kinesis-compatible-streams/ task 7. Run inside the scenario's
`boto3` service (services/kinesis/docker/kinesis_clients/Dockerfile) by
kinesis_node_scenario_test, against the gateways by compose service name:

    kinesis_e2e.py create  --endpoint URL --stream S --shards N
    kinesis_e2e.py produce --endpoint URL --stream S --count N --tag T
    kinesis_e2e.py consume --endpoints URL,URL --stream S --expect N

`produce` writes records `T-0` .. `T-<N-1>` with PutRecords in batches of
100, retrying records that fail, and prints how many went to each shard.
`consume` reads every shard from TRIM_HORIZON with GetShardIterator and
GetRecords, each page from the next endpoint in turn (so one consumer's
iterators cross every node), until it holds `--expect` records or the
deadline passes. It checks that sequence numbers rise within each shard
and that no record arrives twice or goes missing, then prints one JSON
line with the counts. Credentials come from
AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY. Any failure exits non-zero with
the reason on stderr.
"""

import argparse
import json
import sys
import time

import boto3
from botocore.config import Config

CONFIG = Config(
    retries={"max_attempts": 10, "mode": "standard"},
    connect_timeout=5,
    read_timeout=20,
)


def client(endpoint):
    return boto3.client(
        "kinesis", endpoint_url=endpoint, region_name="us-east-1", config=CONFIG
    )


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def create(args):
    c = client(args.endpoint)
    c.create_stream(StreamName=args.stream, ShardCount=args.shards)
    deadline = time.time() + 60
    while time.time() < deadline:
        s = c.describe_stream_summary(StreamName=args.stream)
        if s["StreamDescriptionSummary"]["StreamStatus"] == "ACTIVE":
            print(json.dumps({"stream": args.stream, "shards": args.shards}))
            return
        time.sleep(1)
    fail(f"{args.stream} never became ACTIVE")


def produce(args):
    c = client(args.endpoint)
    pending = [(f"{args.tag}-{i}", f"pk-{args.tag}-{i}") for i in range(args.count)]
    per_shard = {}
    deadline = time.time() + 120
    while pending:
        if time.time() > deadline:
            fail(f"{len(pending)} records still failing after 120 s")
        batch, pending = pending[:100], pending[100:]
        out = c.put_records(
            StreamName=args.stream,
            Records=[{"Data": d.encode(), "PartitionKey": pk} for d, pk in batch],
        )
        retry = []
        for (d, pk), r in zip(batch, out["Records"]):
            if "ErrorCode" in r:
                retry.append((d, pk))
            else:
                per_shard[r["ShardId"]] = per_shard.get(r["ShardId"], 0) + 1
        if retry:
            # Order matters within a partition key; these keys are unique,
            # so a retried record can go last.
            time.sleep(0.5)
            pending = retry + pending
    print(json.dumps({"produced": args.count, "per_shard": per_shard}))


def consume(args):
    clients = [client(e) for e in args.endpoints.split(",")]
    first = clients[0]
    shards = [s["ShardId"] for s in first.list_shards(StreamName=args.stream)["Shards"]]
    iterators = {
        sid: first.get_shard_iterator(
            StreamName=args.stream, ShardId=sid, ShardIteratorType="TRIM_HORIZON"
        )["ShardIterator"]
        for sid in shards
    }
    seen = set()
    last_seq = {}
    per_shard = {sid: 0 for sid in shards}
    turn = 0
    deadline = time.time() + args.timeout
    while len(seen) < args.expect and time.time() < deadline:
        progressed = False
        for sid in shards:
            c = clients[turn % len(clients)]
            turn += 1
            out = c.get_records(ShardIterator=iterators[sid], Limit=args.limit)
            for r in out["Records"]:
                seq = int(r["SequenceNumber"])
                if seq <= last_seq.get(sid, -1):
                    fail(f"{sid}: sequence {seq} after {last_seq[sid]}")
                last_seq[sid] = seq
                data = r["Data"].decode()
                if data in seen:
                    fail(f"{data} arrived twice")
                seen.add(data)
                per_shard[sid] += 1
                progressed = True
            if "NextShardIterator" not in out:
                fail(f"{sid}: no NextShardIterator on an open shard")
            iterators[sid] = out["NextShardIterator"]
        if not progressed:
            time.sleep(0.5)
    if len(seen) < args.expect:
        fail(f"read {len(seen)} of {args.expect} records before the deadline")
    if len(seen) > args.expect:
        fail(f"read {len(seen)} records, expected {args.expect}")
    print(json.dumps({"consumed": len(seen), "per_shard": per_shard}))


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="command", required=True)
    c = sub.add_parser("create")
    c.add_argument("--endpoint", required=True)
    c.add_argument("--stream", required=True)
    c.add_argument("--shards", type=int, required=True)
    pr = sub.add_parser("produce")
    pr.add_argument("--endpoint", required=True)
    pr.add_argument("--stream", required=True)
    pr.add_argument("--count", type=int, required=True)
    pr.add_argument("--tag", required=True)
    co = sub.add_parser("consume")
    co.add_argument("--endpoints", required=True)
    co.add_argument("--stream", required=True)
    co.add_argument("--expect", type=int, required=True)
    co.add_argument("--limit", type=int, default=37)
    co.add_argument("--timeout", type=int, default=120)
    args = p.parse_args()
    {"create": create, "produce": produce, "consume": consume}[args.command](args)


if __name__ == "__main__":
    main()
