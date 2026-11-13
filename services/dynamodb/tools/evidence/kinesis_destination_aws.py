#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Kinesis streaming destination against real AWS (.kiro/specs/
dynamodb-compatible-api/ task 29).

A Kythira DynamoDB gateway publishes a table's changes to a real Amazon
Kinesis Data Streams stream. This script makes the stream, points a table
at it, writes, and reads the stream back from AWS to check that:

- every change arrives at least once, and in write order per item;
- records are in AWS's format, with the precision asked for;
- a one-shard stream under a burst of large items loses nothing. The
  script reports CloudWatch's count of throttled writes, but does not
  require one: AWS took a 3.8 MiB call and 12 MB in a minute on one
  shard without refusing anything, so a short burst cannot be relied on
  to throttle. The retry path is covered by the unit test
  a_failed_record_is_resent_without_reordering_its_item;
- disabling the destination stops delivery.

It deletes the stream at the end, whatever happened, and prints the
stream's lifetime and the PUT payload units used, for the cost note.

The gateway must already run, with AWS credentials for the stream's
account in its environment (AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY,
AWS_SESSION_TOKEN) and a static admin key for this script. The script
reads the stream with the AWS profile given (default chain if none).

Usage:
  kinesis_destination_aws.py --gateway http://127.0.0.1:8000 \\
      --key AKID:SECRET [--region us-east-1] [--profile NAME] [--out DIR]
"""

import argparse
import datetime
import json
import sys
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import boto3
from botocore.config import Config


def kythira_client(args):
    key_id, secret = args.key.split(":", 1)
    return boto3.client(
        "dynamodb",
        endpoint_url=args.gateway,
        region_name=args.region,
        aws_access_key_id=key_id,
        aws_secret_access_key=secret,
        config=Config(retries={"max_attempts": 10, "mode": "standard"}),
    )


def wait_stream(kinesis, name, status):
    for _ in range(120):
        d = kinesis.describe_stream_summary(StreamName=name)["StreamDescriptionSummary"]
        if d["StreamStatus"] == status:
            return d
        time.sleep(5)
    raise RuntimeError(f"stream {name} never became {status}")


def wait_table(ddb, name):
    for _ in range(120):
        if ddb.describe_table(TableName=name)["Table"]["TableStatus"] == "ACTIVE":
            return
        time.sleep(1)
    raise RuntimeError(f"table {name} never became ACTIVE")


def wait_destination(ddb, table, arn, status):
    for _ in range(180):
        ds = ddb.describe_kinesis_streaming_destination(TableName=table)[
            "KinesisDataStreamDestinations"
        ]
        for d in ds:
            if d["StreamArn"] == arn:
                if d["DestinationStatus"] == status:
                    return d
                if d["DestinationStatus"] == "ENABLE_FAILED":
                    raise RuntimeError(f"destination failed: {d}")
        time.sleep(1)
    raise RuntimeError(f"destination never became {status}")


def read_all(kinesis, name):
    """Every record in the stream, each shard from its start, by shard."""
    records = []
    shards = kinesis.list_shards(StreamName=name)["Shards"]
    for s in shards:
        it = kinesis.get_shard_iterator(
            StreamName=name, ShardId=s["ShardId"], ShardIteratorType="TRIM_HORIZON"
        )["ShardIterator"]
        empty = 0
        while it and empty < 3:
            r = kinesis.get_records(ShardIterator=it, Limit=10000)
            for rec in r["Records"]:
                records.append(json.loads(rec["Data"]))
            empty = empty + 1 if not r["Records"] and r.get("MillisBehindLatest", 0) == 0 else 0
            it = r.get("NextShardIterator")
            time.sleep(0.25)
    return records


def check_format(rec, table, region, precision):
    problems = []
    for k in ("awsRegion", "eventID", "eventName", "recordFormat", "tableName", "dynamodb",
              "eventSource"):
        if k not in rec:
            problems.append(f"missing {k}")
    if rec.get("eventSource") != "aws:dynamodb":
        problems.append(f"eventSource {rec.get('eventSource')}")
    if rec.get("recordFormat") != "application/json":
        problems.append(f"recordFormat {rec.get('recordFormat')}")
    if rec.get("tableName") != table:
        problems.append(f"tableName {rec.get('tableName')}")
    if rec.get("awsRegion") != region:
        problems.append(f"awsRegion {rec.get('awsRegion')}")
    body = rec.get("dynamodb", {})
    if body.get("ApproximateCreationDateTimePrecision") != precision:
        problems.append(f"precision {body.get('ApproximateCreationDateTimePrecision')}")
    t = body.get("ApproximateCreationDateTime", 0)
    now_us = time.time() * 1e6
    scale = 1 if precision == "MICROSECOND" else 1000
    if not (now_us - 3600e6 < t * scale < now_us + 60e6):
        problems.append(f"ApproximateCreationDateTime {t} not in the last hour at {precision}")
    if rec.get("eventName") not in ("INSERT", "MODIFY", "REMOVE"):
        problems.append(f"eventName {rec.get('eventName')}")
    return problems


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--gateway", required=True)
    p.add_argument("--key", required=True, help="Kythira admin key, ID:SECRET")
    p.add_argument("--region", default="us-east-1")
    p.add_argument("--profile")
    p.add_argument("--items", type=int, default=50)
    p.add_argument("--burst", type=int, default=60)
    p.add_argument("--pad", type=int, default=200000,
                   help="bytes per burst item. The gateway publishes once a second, one "
                        "PutRecords call (up to 4 MiB) at a time, so many small items never "
                        "reach one shard's 1 MiB/s; a few large ones fill a call past it")
    p.add_argument("--writers", type=int, default=16)
    p.add_argument("--out", default=".")
    args = p.parse_args()

    session = boto3.Session(profile_name=args.profile, region_name=args.region)
    kinesis = session.client("kinesis")
    account = session.client("sts").get_caller_identity()["Account"]
    ddb = kythira_client(args)
    run = uuid.uuid4().hex[:8]
    stream = f"kythira-ddb-dest-{run}"
    table = f"dest_{run}"
    report = {"stream": stream, "table": table, "account": account, "region": args.region,
              "checks": {}}
    created = None
    ok = True
    try:
        kinesis.create_stream(StreamName=stream, StreamModeDetails={"StreamMode": "PROVISIONED"},
                              ShardCount=1)
        created = time.time()
        arn = wait_stream(kinesis, stream, "ACTIVE")["StreamARN"]
        print(f"stream {arn} active", flush=True)

        ddb.create_table(TableName=table, BillingMode="PAY_PER_REQUEST",
                         AttributeDefinitions=[{"AttributeName": "pk", "AttributeType": "S"}],
                         KeySchema=[{"AttributeName": "pk", "KeyType": "HASH"}])
        wait_table(ddb, table)
        ddb.enable_kinesis_streaming_destination(
            TableName=table, StreamArn=arn,
            EnableKinesisStreamingConfiguration={
                "ApproximateCreationDateTimePrecision": "MICROSECOND"})
        wait_destination(ddb, table, arn, "ACTIVE")
        print("destination active", flush=True)

        # Writes: per item an insert, two updates and (for every third) a
        # delete. `seq` in the item orders them.
        expected = {}
        for i in range(args.items):
            pk = f"item-{i:04d}"
            expected[pk] = []
            ddb.put_item(TableName=table, Item={"pk": {"S": pk}, "seq": {"N": "0"}})
            expected[pk].append(("INSERT", 0))
            for s in (1, 2):
                ddb.update_item(TableName=table, Key={"pk": {"S": pk}},
                                UpdateExpression="SET seq = :s",
                                ExpressionAttributeValues={":s": {"N": str(s)}})
                expected[pk].append(("MODIFY", s))
            if i % 3 == 0:
                ddb.delete_item(TableName=table, Key={"pk": {"S": pk}})
                expected[pk].append(("REMOVE", 2))

        # Burst: large items written within one publishing pass, so a
        # single PutRecords call carries several times what one shard takes
        # in a second (1 MiB) and AWS refuses part of it; the publisher must
        # retry those records.
        burst_start = time.time()
        pad = "x" * args.pad

        def put_burst(j):
            ddb.put_item(TableName=table, Item={"pk": {"S": f"burst-{j:05d}"},
                                                "seq": {"N": "0"}, "pad": {"S": pad}})

        with ThreadPoolExecutor(max_workers=args.writers) as pool:
            list(pool.map(put_burst, range(args.burst)))
        for j in range(args.burst):
            expected[f"burst-{j:05d}"] = [("INSERT", 0)]
        burst_seconds = time.time() - burst_start
        report["burst"] = {"items": args.burst, "pad_bytes": args.pad,
                           "seconds": round(burst_seconds, 1)}
        print(f"burst of {args.burst} written in {burst_seconds:.1f} s", flush=True)

        # Wait for delivery: until every key has all its records, or 10 min.
        deadline = time.time() + 600
        while True:
            records = read_all(kinesis, stream)
            got = {}
            for r in records:
                pk = r["dynamodb"]["Keys"]["pk"]["S"]
                image = r["dynamodb"].get("NewImage") or r["dynamodb"].get("OldImage") or {}
                got.setdefault(pk, []).append(
                    (r["eventName"], int(image.get("seq", {}).get("N", "-1"))))
            missing = [pk for pk, ev in expected.items()
                       if not all(e in got.get(pk, []) for e in ev)]
            if not missing or time.time() > deadline:
                break
            print(f"{len(records)} records, {len(missing)} keys incomplete; waiting", flush=True)
            time.sleep(15)

        report["records"] = len(records)
        report["checks"]["all_delivered"] = not missing
        if missing:
            report["missing_sample"] = missing[:10]

        # In order per item: the first time each expected event appears,
        # they appear in write order (redelivery may repeat an event).
        out_of_order = []
        for pk, ev in expected.items():
            seen = got.get(pk, [])
            firsts = [seen.index(e) for e in ev if e in seen]
            if firsts != sorted(firsts):
                out_of_order.append(pk)
        report["checks"]["in_order_per_item"] = not out_of_order
        report["duplicates"] = sum(len(v) for v in got.values()) - sum(
            len(set(v)) for v in got.values())

        bad = {}
        for r in records:
            probs = check_format(r, table, args.region, "MICROSECOND")
            if probs:
                bad[r.get("eventID", "?")] = probs
        report["checks"]["aws_format"] = not bad
        if bad:
            report["format_problems_sample"] = dict(list(bad.items())[:5])

        # Disable: later writes do not reach the stream.
        ddb.disable_kinesis_streaming_destination(TableName=table, StreamArn=arn)
        wait_destination(ddb, table, arn, "DISABLED")
        before = len(read_all(kinesis, stream))
        for k in range(10):
            ddb.put_item(TableName=table, Item={"pk": {"S": f"after-{k}"}})
        time.sleep(30)
        after = read_all(kinesis, stream)
        late = [r for r in after if r["dynamodb"]["Keys"]["pk"]["S"].startswith("after-")]
        report["checks"]["disable_stops_delivery"] = not late
        report["records_after_disable"] = len(after) - before

        # Throttling: CloudWatch's count of refused writes for the run.
        # Its data points lag by a minute or two.
        cloudwatch = session.client("cloudwatch")
        throttled = 0
        for _ in range(8):
            now = datetime.datetime.now(datetime.timezone.utc)
            stats = cloudwatch.get_metric_statistics(
                Namespace="AWS/Kinesis", MetricName="WriteProvisionedThroughputExceeded",
                Dimensions=[{"Name": "StreamName", "Value": stream}],
                StartTime=datetime.datetime.fromtimestamp(created - 60, datetime.timezone.utc),
                EndTime=now, Period=60, Statistics=["Sum"])
            throttled = sum(d["Sum"] for d in stats["Datapoints"])
            if throttled > 0:
                break
            time.sleep(15)
        # Reported, not required: AWS lets a shard absorb short bursts
        # well past 1 MiB/s.
        report["write_throttled_events"] = throttled

        ok = all(report["checks"].values())
        report["payload_bytes"] = sum(len(json.dumps(r)) for r in after)
    finally:
        try:
            ddb.delete_table(TableName=table)
        except Exception as e:  # noqa: BLE001 - cleanup must go on
            print(f"table cleanup: {e}", file=sys.stderr)
        if created is not None:
            kinesis.delete_stream(StreamName=stream, EnforceConsumerDeletion=True)
            try:
                kinesis.get_waiter("stream_not_exists").wait(StreamName=stream)
            except Exception as e:  # noqa: BLE001
                print(f"stream deletion wait: {e}", file=sys.stderr)
            report["stream_seconds"] = round(time.time() - created)
            print(f"stream {stream} deleted after {report['stream_seconds']} s", flush=True)

    # One shard-hour is $0.015 and PUT payload units are $0.014 per
    # million (25 KB each) in us-east-1, list price.
    hours = max(1, -(-report.get("stream_seconds", 0) // 3600))
    units = report.get("payload_bytes", 0) // 25000 + report.get("records", 0)
    report["estimated_cost_usd"] = round(hours * 0.015 + units * 0.014 / 1e6, 4)
    out = Path(args.out) / "kinesis_destination_aws.json"
    out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
