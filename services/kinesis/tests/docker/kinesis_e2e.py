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
    kinesis_e2e.py ordered-produce --endpoints URL,URL --stream S --count N --keys K --tag T
    kinesis_e2e.py lineage-consume --endpoints URL,URL --stream S --count N --keys K --tag T
    kinesis_e2e.py batch-produce --endpoints URL,URL --stream S --count N --keys K --tag T
    kinesis_e2e.py fan-out-consume --endpoints URL,URL --stream S --count N --keys K --tag T
    kinesis_e2e.py s3-control --url URL [--put-delay MS] [--fail-puts N --prefix P]

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

The last two are the resharding case (task 11). `ordered-produce` writes
`T-0` .. `T-<N-1>` one PutRecord at a time over `K` partition keys
(`T-i` under key `i mod K`), waiting for each to succeed before the next,
and moves to the next endpoint when a node does not answer, so the stream
is written straight through a node kill and a reshard. A write retried
after a lost answer may land twice. `lineage-consume` reads every shard,
open or closed, from TRIM_HORIZON to its end or the stream's present, and
starts a child only once its parents are read to their ends, as KCL does;
it reads the stream again until every record is there or the deadline
passes. It fails unless, under each partition key, records arrived in the
order they were written. A record may arrive twice (a retried write), and
the two copies are then next to each other.

The last two are the chaos case (task 17). `batch-produce` is
`ordered-produce` with PutRecords: each batch holds the next `K` records,
one per key, and its failed entries are retried, through the next endpoint
when a call fails outright, before the next batch starts, so each key's
records still go in order. `fan-out-consume` registers a consumer (or
reuses one) and subscribes to every shard with SubscribeToShard, one
thread per shard. When a subscription ends, by its lifetime, an exception
event or a lost node, it subscribes again through the next endpoint, after
its continuation sequence number. It fails unless every record arrives,
in order under each key, by the deadline.

`s3-control` drives the S3 mock's control side for the tiering case (task
17): it optionally sets the PUT delay and arms a number of failing PUTs
under a key prefix, then prints the mock's counters as one JSON line.
"""

import argparse
import json
import sys
import threading
import time
import urllib.request

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


def ordered_produce(args):
    clients = [client(e) for e in args.endpoints.split(",")]
    turn = 0
    retries = 0
    deadline = time.time() + args.timeout
    for i in range(args.count):
        while True:
            if time.time() > deadline:
                fail(f"record {i} still failing after {args.timeout} s")
            try:
                clients[turn % len(clients)].put_record(
                    StreamName=args.stream,
                    Data=f"{args.tag}-{i}".encode(),
                    PartitionKey=f"pk-{args.tag}-{i % args.keys}",
                )
                break
            except Exception:  # noqa: BLE001 - any failure: next node, again
                retries += 1
                turn += 1
                time.sleep(0.2)
    print(json.dumps({"produced": args.count, "retries": retries}))


def lineage_pass(clients, args):
    """One read of the whole stream in lineage order: per key, the indexes
    in arrival order. A closed shard is read to its end, an open one to the
    present."""
    turn = 0
    prefix = f"{args.tag}-"
    by_key = {}
    shards = clients[0].list_shards(StreamName=args.stream)["Shards"]
    done = set()
    pending = list(shards)
    while pending:
        ready = [
            s
            for s in pending
            if all(
                s[k] in done for k in ("ParentShardId", "AdjacentParentShardId") if k in s
            )
        ]
        if not ready:
            fail("shards whose parents are not listed: " + ",".join(s["ShardId"] for s in pending))
        for s in ready:
            pending.remove(s)
            sid = s["ShardId"]
            it = clients[turn % len(clients)].get_shard_iterator(
                StreamName=args.stream, ShardId=sid, ShardIteratorType="TRIM_HORIZON"
            )["ShardIterator"]
            while it:
                c = clients[turn % len(clients)]
                turn += 1
                out = c.get_records(ShardIterator=it, Limit=args.limit)
                for r in out["Records"]:
                    data = r["Data"].decode()
                    if data.startswith(prefix):
                        i = int(data[len(prefix):])
                        by_key.setdefault(i % args.keys, []).append(i)
                it = out.get("NextShardIterator")
                if not out["Records"] and out.get("MillisBehindLatest", 0) == 0:
                    break
            done.add(sid)
    return by_key, len(shards)


def lineage_consume(args):
    clients = [client(e) for e in args.endpoints.split(",")]
    deadline = time.time() + args.timeout
    while True:
        by_key, shards = lineage_pass(clients, args)
        seen = {i for got in by_key.values() for i in got}
        missing = sorted(set(range(args.count)) - seen)
        if not missing:
            break
        if time.time() > deadline:
            fail(f"{len(missing)} of {args.count} records never read, first {missing[0]}")
        time.sleep(2)
    for key, got in by_key.items():
        for a, b in zip(got, got[1:]):
            if b < a:
                fail(f"key {key}: {b} arrived after {a}")
        for i in set(got):
            if got.count(i) > 2:
                fail(f"key {key}: {i} arrived {got.count(i)} times")
    dups = sum(len(g) - len(set(g)) for g in by_key.values())
    print(json.dumps({"consumed": len(seen), "shards": shards, "duplicates": dups}))


def batch_produce(args):
    clients = [client(e) for e in args.endpoints.split(",")]
    turn = 0
    retries = 0
    deadline = time.time() + args.timeout
    for start in range(0, args.count, args.keys):
        pending = [
            {"Data": f"{args.tag}-{i}".encode(), "PartitionKey": f"pk-{args.tag}-{i % args.keys}"}
            for i in range(start, min(start + args.keys, args.count))
        ]
        while pending:
            if time.time() > deadline:
                fail(f"batch at {start} still failing after {args.timeout} s")
            try:
                out = clients[turn % len(clients)].put_records(
                    StreamName=args.stream, Records=pending
                )
            except Exception:  # noqa: BLE001 - any failure: next node, again
                retries += 1
                turn += 1
                time.sleep(0.2)
                continue
            pending = [r for r, res in zip(pending, out["Records"]) if "ErrorCode" in res]
            if pending:
                retries += 1
                time.sleep(0.1)
    print(json.dumps({"produced": args.count, "retries": retries}))


def fan_out_consume(args):
    clients = [client(e) for e in args.endpoints.split(",")]
    c0 = clients[0]
    arn = c0.describe_stream_summary(StreamName=args.stream)["StreamDescriptionSummary"][
        "StreamARN"
    ]
    name = f"chaos-{args.tag}"
    try:
        consumer = c0.register_stream_consumer(StreamARN=arn, ConsumerName=name)["Consumer"]
    except c0.exceptions.ResourceInUseException:
        consumer = c0.describe_stream_consumer(StreamARN=arn, ConsumerName=name)[
            "ConsumerDescription"
        ]
    consumer_arn = consumer["ConsumerARN"]
    prefix = f"{args.tag}-"
    lock = threading.Lock()
    by_key = {}
    seen = set()
    resubscribes = [0]
    deadline = time.time() + args.timeout
    done = threading.Event()

    def follow(shard_id, turn):
        position = {"Type": "TRIM_HORIZON"}
        while not done.is_set() and time.time() < deadline:
            c = clients[turn % len(clients)]
            turn += 1
            try:
                events = c.subscribe_to_shard(
                    ConsumerARN=consumer_arn, ShardId=shard_id, StartingPosition=position
                )["EventStream"]
                for ev in events:
                    e = ev.get("SubscribeToShardEvent")
                    if e is None:
                        break
                    with lock:
                        for r in e["Records"]:
                            data = r["Data"].decode()
                            if data.startswith(prefix):
                                i = int(data[len(prefix):])
                                by_key.setdefault(i % args.keys, []).append(i)
                                seen.add(i)
                        if len(seen) >= args.count:
                            done.set()
                    if e.get("ContinuationSequenceNumber"):
                        position = {
                            "Type": "AFTER_SEQUENCE_NUMBER",
                            "SequenceNumber": e["ContinuationSequenceNumber"],
                        }
                    elif "ChildShards" in e:
                        return
                    if done.is_set() or time.time() > deadline:
                        return
            except Exception:  # noqa: BLE001 - a lost node or busy slot: again elsewhere
                pass
            with lock:
                resubscribes[0] += 1
            time.sleep(0.5)

    shards = c0.list_shards(StreamName=args.stream)["Shards"]
    threads = [
        threading.Thread(target=follow, args=(s["ShardId"], n), daemon=True)
        for n, s in enumerate(shards)
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join(max(0.0, deadline - time.time()) + 30)
    done.set()
    with lock:
        missing = sorted(set(range(args.count)) - seen)
        if missing:
            fail(f"{len(missing)} of {args.count} records never pushed, first {missing[0]}")
        for key, got in by_key.items():
            for a, b in zip(got, got[1:]):
                if b < a:
                    fail(f"key {key}: {b} pushed after {a}")
        dups = sum(len(g) - len(set(g)) for g in by_key.values())
        print(
            json.dumps(
                {"consumed": len(seen), "duplicates": dups, "resubscribes": resubscribes[0]}
            )
        )


def s3_control(args):
    def call(path, body=None):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(
            args.url + path,
            data=data,
            method="GET" if body is None else "POST",
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.read().decode()

    if args.put_delay is not None:
        call("/control/put_delay", {"ms": args.put_delay})
    if args.fail_puts is not None:
        call("/control/fail_puts", {"prefix": args.prefix, "count": args.fail_puts})
    print(call("/control"))


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
    for name in ("ordered-produce", "lineage-consume", "batch-produce", "fan-out-consume"):
        o = sub.add_parser(name)
        o.add_argument("--endpoints", required=True)
        o.add_argument("--stream", required=True)
        o.add_argument("--count", type=int, required=True)
        o.add_argument("--keys", type=int, required=True)
        o.add_argument("--tag", required=True)
        o.add_argument("--limit", type=int, default=100)
        o.add_argument("--timeout", type=int, default=600)
    s3 = sub.add_parser("s3-control")
    s3.add_argument("--url", required=True)
    s3.add_argument("--put-delay", type=int)
    s3.add_argument("--fail-puts", type=int)
    s3.add_argument("--prefix", default="")
    args = p.parse_args()
    {
        "create": create,
        "produce": produce,
        "consume": consume,
        "ordered-produce": ordered_produce,
        "lineage-consume": lineage_consume,
        "batch-produce": batch_produce,
        "fan-out-consume": fan_out_consume,
        "s3-control": s3_control,
    }[args.command](args)


if __name__ == "__main__":
    main()
