#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""The multi-region strong consistency scenario's client
(.kiro/specs/dynamodb-compatible-api/ task 27.8), driven by run_strong.sh.

  wait     until every region answers ListTables and its strong host is up
  setup    create the table in us-east-1, make it strong with replicas in
           us-east-2 and us-west-2, and wait until every region lists it
           with both other replicas ACTIVE
  check    a write in each region is read by a strongly consistent GetItem
           in every region; then the regions race to create the same keys
           under attribute_not_exists, and each key has exactly one winner,
           which every region reads
  write    COUNT puts from each of REGIONS, ids PREFIX-<region>-<n>, each
           retried until acknowledged; writes OUT/acked-PREFIX.txt
  verify   every id in OUT/acked-PREFIX.txt is read by a strongly
           consistent GetItem in REGION, and a consistent Scan there holds
           exactly the ids acknowledged so far
  refused  a put in REGION fails (no quorum) and an eventually consistent
           read there still answers

ENDPOINTS is a comma-separated list of region=url pairs.
"""

import argparse
import concurrent.futures
import os
import sys
import time

import boto3
from botocore.config import Config
from botocore.exceptions import BotoCoreError, ClientError

TABLE = "accounts"
REGIONS = ["us-east-1", "us-east-2", "us-west-2"]
KEY_ID = "STRONG-driver"
SECRET = "strong-secret-not-a-credential"

ENDPOINTS = dict(
    pair.split("=", 1)
    for pair in os.environ.get("ENDPOINTS", "").split(",")
    if "=" in pair
)
_config = Config(
    retries={"max_attempts": 1, "mode": "standard"},
    connect_timeout=2,
    read_timeout=30,
)
_clients = {}


def client(region):
    if region not in _clients:
        _clients[region] = boto3.client(
            "dynamodb",
            endpoint_url=ENDPOINTS[region],
            region_name=region,
            aws_access_key_id=KEY_ID,
            aws_secret_access_key=SECRET,
            config=_config,
        )
    return _clients[region]


def retried(fn, seconds=120):
    """fn() until it neither raises a transport error nor answers a 5xx."""
    deadline = time.monotonic() + seconds
    while True:
        try:
            return fn()
        except ClientError as e:
            status = e.response.get("ResponseMetadata", {}).get("HTTPStatusCode", 0)
            if status < 500 or time.monotonic() > deadline:
                raise
        except BotoCoreError:
            if time.monotonic() > deadline:
                raise
        time.sleep(0.5)


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def describe(region):
    return retried(lambda: client(region).describe_table(TableName=TABLE))["Table"]


def wait(args):
    deadline = time.monotonic() + args.seconds
    pending = set(REGIONS)
    while pending and time.monotonic() < deadline:
        for region in sorted(pending):
            try:
                client(region).list_tables()
                pending.discard(region)
            except (BotoCoreError, ClientError):
                pass
        time.sleep(1)
    if pending:
        fail(f"no answer from {sorted(pending)}")


def setup(_args):
    retried(
        lambda: client("us-east-1").create_table(
            TableName=TABLE,
            BillingMode="PAY_PER_REQUEST",
            AttributeDefinitions=[{"AttributeName": "id", "AttributeType": "S"}],
            KeySchema=[{"AttributeName": "id", "KeyType": "HASH"}],
        )
    )
    deadline = time.monotonic() + 120
    while describe("us-east-1")["TableStatus"] != "ACTIVE":
        if time.monotonic() > deadline:
            fail("the table never became ACTIVE")
        time.sleep(0.5)
    # The strong catalog may still be electing its first leader.
    retried(
        lambda: client("us-east-1").update_table(
            TableName=TABLE,
            MultiRegionConsistency="STRONG",
            ReplicaUpdates=[
                {"Create": {"RegionName": "us-east-2"}},
                {"Create": {"RegionName": "us-west-2"}},
            ],
        ),
        seconds=180,
    )
    deadline = time.monotonic() + 180
    for region in REGIONS:
        while True:
            try:
                t = describe(region)
            except ClientError:
                t = {}
            replicas = t.get("Replicas", [])
            if (
                t.get("MultiRegionConsistency") == "STRONG"
                and len(replicas) == 2
                and all(r["ReplicaStatus"] == "ACTIVE" for r in replicas)
            ):
                break
            if time.monotonic() > deadline:
                fail(f"{region} never listed the strong table with both replicas ACTIVE: {t}")
            time.sleep(0.5)
    print("the table is strong in all three regions")


def put(region, item_id, condition=None):
    item = {"id": {"S": item_id}, "by": {"S": region}}
    kwargs = {"TableName": TABLE, "Item": item}
    if condition:
        kwargs["ConditionExpression"] = condition
    return retried(lambda: client(region).put_item(**kwargs))


def strong_get(region, item_id):
    return retried(
        lambda: client(region).get_item(
            TableName=TABLE, Key={"id": {"S": item_id}}, ConsistentRead=True
        )
    ).get("Item")


def check(_args):
    for writer in REGIONS:
        put(writer, f"check-{writer}")
        for reader in REGIONS:
            if strong_get(reader, f"check-{writer}") is None:
                fail(f"a write in {writer} was not read strongly in {reader}")
    print("every region strongly reads every region's write")

    keys = [f"race-{n}" for n in range(20)]

    def race(region):
        outcome = {}
        for key in keys:
            try:
                put(region, key, condition="attribute_not_exists(id)")
                outcome[key] = "won"
            except ClientError as e:
                if e.response["Error"]["Code"] != "ConditionalCheckFailedException":
                    raise
                outcome[key] = "lost"
        return region, outcome

    with concurrent.futures.ThreadPoolExecutor(len(REGIONS)) as pool:
        results = dict(pool.map(race, REGIONS))
    for key in keys:
        winners = [r for r in REGIONS if results[r][key] == "won"]
        if len(winners) != 1:
            fail(f"{key} had {len(winners)} winners: {winners}")
        seen = {strong_get(r, key)["by"]["S"] for r in REGIONS}
        if seen != set(winners):
            fail(f"{key} won by {winners[0]}, but the regions read {sorted(seen)}")
    print(f"{len(keys)} racing conditional writes: one winner each, read alike everywhere")


def write(args):
    acked = []

    def writer(region):
        ids = []
        for n in range(args.count):
            item_id = f"{args.prefix}-{region}-{n}"
            try:
                put(region, item_id, condition="attribute_not_exists(id)")
            except ClientError as e:
                # A retry after a put that committed but whose answer was
                # lost finds its own item.
                if e.response["Error"]["Code"] != "ConditionalCheckFailedException":
                    raise
            ids.append(item_id)
        return ids

    with concurrent.futures.ThreadPoolExecutor(len(args.regions)) as pool:
        for ids in pool.map(writer, args.regions):
            acked.extend(ids)
    with open(os.path.join(args.out, f"acked-{args.prefix}.txt"), "w") as f:
        f.write("\n".join(acked) + "\n")
    print(f"{len(acked)} writes acknowledged from {', '.join(args.regions)}")


def acked_ids(out):
    ids = set()
    for name in os.listdir(out):
        if name.startswith("acked-") and name.endswith(".txt"):
            with open(os.path.join(out, name)) as f:
                ids.update(line.strip() for line in f if line.strip())
    return ids


def verify(args):
    with open(os.path.join(args.out, f"acked-{args.prefix}.txt")) as f:
        ids = [line.strip() for line in f if line.strip()]
    missing = [i for i in ids if strong_get(args.region, i) is None]
    if missing:
        fail(f"{args.region} does not read {len(missing)} acknowledged writes, e.g. {missing[:5]}")
    found = set()
    kwargs = {"TableName": TABLE, "ConsistentRead": True}
    while True:
        page = retried(lambda: client(args.region).scan(**kwargs))
        found.update(i["id"]["S"] for i in page["Items"])
        if "LastEvaluatedKey" not in page:
            break
        kwargs["ExclusiveStartKey"] = page["LastEvaluatedKey"]
    expected = acked_ids(args.out)
    # The lone region's write answered a server error, whose outcome is
    # unknown, as on AWS: it stays in the leader's log and commits once a
    # quorum is back.
    tested = {i for i in found if i.startswith(("check-", "race-")) or i == "no-quorum"}
    if found - tested != expected:
        fail(
            f"{args.region} scans {len(found - tested)} written items, "
            f"{len(expected)} were acknowledged: missing {sorted(expected - found)[:5]}, "
            f"extra {sorted(found - tested - expected)[:5]}"
        )
    print(f"{args.region} reads all {len(ids)} {args.prefix} writes; its scan matches")


def refused(args):
    try:
        client(args.region).put_item(
            TableName=TABLE, Item={"id": {"S": "no-quorum"}, "by": {"S": args.region}}
        )
        fail(f"{args.region} took a write without a quorum")
    except ClientError as e:
        status = e.response["ResponseMetadata"]["HTTPStatusCode"]
        if status < 500:
            fail(f"{args.region} refused the write with {status}, not a server error")
    except BotoCoreError:
        pass
    got = client(args.region).get_item(TableName=TABLE, Key={"id": {"S": "check-us-east-1"}})
    if "Item" not in got:
        fail(f"{args.region} gave no eventually consistent read")
    print(f"{args.region} alone refuses writes and still serves eventually consistent reads")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("wait")
    p.add_argument("--seconds", type=int, default=120)
    p.set_defaults(fn=wait)
    sub.add_parser("setup").set_defaults(fn=setup)
    sub.add_parser("check").set_defaults(fn=check)
    p = sub.add_parser("write")
    p.add_argument("--prefix", required=True)
    p.add_argument("--count", type=int, default=50)
    p.add_argument("--out", required=True)
    p.add_argument("regions", nargs="+", choices=REGIONS)
    p.set_defaults(fn=write)
    p = sub.add_parser("verify")
    p.add_argument("--prefix", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("region", choices=REGIONS)
    p.set_defaults(fn=verify)
    p = sub.add_parser("refused")
    p.add_argument("region", choices=REGIONS)
    p.set_defaults(fn=refused)
    args = parser.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
