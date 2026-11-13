#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""The DynamoDB chaos scenario's client (.kiro/specs/dynamodb-compatible-api/
task 24), driven by run_chaos.sh while it SIGKILLs the cluster's nodes.

  wait     until some node answers ListTables
  setup    create the table: a GSI on `g`, a stream of new images
  write    for SECONDS, WRITERS threads put items with unique ids, each
           under attribute_not_exists(pk) and retried on any node until it
           is acknowledged (a retry that finds its own earlier put is told
           ConditionalCheckFailed, which acknowledges it too). Writes
           OUT/attempted.txt and OUT/acked.txt.
  verify   after the kills, fail unless:
             * every acknowledged item is in the table, with its value,
               and no item is there that was never attempted;
             * the GSI holds exactly one entry for each item and nothing
               else (it is eventually consistent, so the check waits);
             * the stream, read across every shard (a split closes a shard
               and opens its children), holds exactly one INSERT for each
               item and no other record, and no sequence number twice in a shard;
             * the stream has more than one shard, so a split happened
               while the nodes were being killed.

ENDPOINTS is a comma-separated list of node URLs; each request goes to the
next one, so a killed node costs a retry, not the run.
"""

import argparse
import itertools
import json
import os
import sys
import threading
import time

import boto3
from botocore.config import Config
from botocore.exceptions import BotoCoreError, ClientError

TABLE = "chaos"
INDEX = "by-g"
GROUPS = 16
PAD = "x" * 200
KEY_ID = "CHAOS-driver"
SECRET = "chaos-secret-not-a-credential"

ENDPOINTS = [e for e in os.environ.get("ENDPOINTS", "http://127.0.0.1:8000").split(",") if e]
_config = Config(
    retries={"max_attempts": 1, "mode": "standard"},
    connect_timeout=2,
    read_timeout=15,
)
_clients = {}
_clients_lock = threading.Lock()
_next = itertools.count()


def client(service="dynamodb"):
    """The next node's client, round robin."""
    endpoint = ENDPOINTS[next(_next) % len(ENDPOINTS)]
    with _clients_lock:
        key = (service, endpoint, threading.get_ident())
        if key not in _clients:
            _clients[key] = boto3.client(
                service,
                endpoint_url=endpoint,
                region_name="us-east-1",
                aws_access_key_id=KEY_ID,
                aws_secret_access_key=SECRET,
                config=_config,
            )
        return _clients[key]


def retry(call, service="dynamodb", deadline=None, accept=()):
    """call(client) on node after node until it answers, or the deadline.

    A refusal named in `accept` counts as an answer and is returned as its
    code. Any other refusal or a transport failure is retried.
    """
    last = None
    while deadline is None or time.monotonic() < deadline:
        try:
            return call(client(service))
        except ClientError as e:
            code = e.response["Error"]["Code"]
            if code in accept:
                return code
            last = e
        except BotoCoreError as e:
            last = e
        time.sleep(0.2)
    raise TimeoutError("no node answered: %s" % last)


def item_of(i):
    return {
        "pk": {"S": "item-%07d" % i},
        "g": {"S": "g%02d" % (i % GROUPS)},
        "v": {"N": str(i)},
        "pad": {"S": PAD},
    }


def cmd_wait(args):
    retry(lambda c: c.list_tables(), deadline=time.monotonic() + args.seconds)
    print("a node answers")


def cmd_setup(args):
    deadline = time.monotonic() + 120
    retry(
        lambda c: c.create_table(
            TableName=TABLE,
            AttributeDefinitions=[
                {"AttributeName": "pk", "AttributeType": "S"},
                {"AttributeName": "g", "AttributeType": "S"},
            ],
            KeySchema=[{"AttributeName": "pk", "KeyType": "HASH"}],
            GlobalSecondaryIndexes=[
                {
                    "IndexName": INDEX,
                    "KeySchema": [
                        {"AttributeName": "g", "KeyType": "HASH"},
                        {"AttributeName": "pk", "KeyType": "RANGE"},
                    ],
                    "Projection": {"ProjectionType": "KEYS_ONLY"},
                }
            ],
            BillingMode="PAY_PER_REQUEST",
            StreamSpecification={"StreamEnabled": True, "StreamViewType": "NEW_IMAGE"},
        ),
        deadline=deadline,
        accept=("ResourceInUseException",),
    )
    while time.monotonic() < deadline:
        d = retry(lambda c: c.describe_table(TableName=TABLE), deadline=deadline)["Table"]
        if d["TableStatus"] == "ACTIVE" and all(
            g["IndexStatus"] == "ACTIVE" for g in d.get("GlobalSecondaryIndexes", [])
        ):
            print("table %s is ACTIVE" % TABLE)
            return
        time.sleep(0.5)
    raise TimeoutError("the table did not become ACTIVE")


def cmd_write(args):
    stop = time.monotonic() + args.seconds
    ids = itertools.count()
    lock = threading.Lock()
    attempted, acked = [], []

    def writer():
        while time.monotonic() < stop:
            with lock:
                i = next(ids)
                attempted.append(i)
            item = item_of(i)
            try:
                retry(
                    lambda c: c.put_item(
                        TableName=TABLE,
                        Item=item,
                        ConditionExpression="attribute_not_exists(pk)",
                    ),
                    deadline=time.monotonic() + 60,
                    accept=("ConditionalCheckFailedException",),
                )
            except TimeoutError as e:
                print("item %d never acknowledged: %s" % (i, e), file=sys.stderr)
                continue
            with lock:
                acked.append(i)

    threads = [threading.Thread(target=writer) for _ in range(args.writers)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    with open(os.path.join(args.out, "attempted.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(map(str, sorted(attempted))) + "\n")
    with open(os.path.join(args.out, "acked.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(map(str, sorted(acked))) + "\n")
    print("%d writes attempted, %d acknowledged" % (len(attempted), len(acked)))


def read_ids(path):
    with open(path, encoding="utf-8") as f:
        return {int(line) for line in f if line.strip()}


def scan_all(index=None, consistent=True):
    items, start = [], None
    deadline = time.monotonic() + 300
    while True:
        kw = {"TableName": TABLE, "Limit": 500}
        if index:
            kw["IndexName"] = index
        else:
            kw["ConsistentRead"] = consistent
        if start:
            kw["ExclusiveStartKey"] = start
        page = retry(lambda c: c.scan(**kw), deadline=deadline)
        items += page["Items"]
        start = page.get("LastEvaluatedKey")
        if not start:
            return items


def id_of(item):
    return int(item["pk"]["S"].split("-")[1])


def read_stream():
    """Every record of the table's stream, across every shard."""
    deadline = time.monotonic() + 300
    arn = retry(lambda c: c.describe_table(TableName=TABLE), deadline=deadline)["Table"][
        "LatestStreamArn"
    ]
    shards, start = [], None
    while True:
        kw = {"StreamArn": arn}
        if start:
            kw["ExclusiveStartShardId"] = start
        d = retry(lambda c: c.describe_stream(**kw), "dynamodbstreams", deadline)[
            "StreamDescription"
        ]
        shards += d["Shards"]
        start = d.get("LastEvaluatedShardId")
        if not start:
            break
    records = []
    for shard in shards:
        it = retry(
            lambda c: c.get_shard_iterator(
                StreamArn=arn, ShardId=shard["ShardId"], ShardIteratorType="TRIM_HORIZON"
            ),
            "dynamodbstreams",
            deadline,
        )["ShardIterator"]
        empty = 0
        # A closed shard ends without NextShardIterator; an open one is
        # read until it has come back empty a few times in a row.
        while it and empty < 3:
            page = retry(
                lambda c: c.get_records(ShardIterator=it, Limit=1000), "dynamodbstreams", deadline
            )
            records += [dict(r, shard=shard["ShardId"]) for r in page["Records"]]
            empty = 0 if page["Records"] else empty + 1
            it = page.get("NextShardIterator")
    return shards, records


def cmd_verify(args):
    attempted = read_ids(os.path.join(args.out, "attempted.txt"))
    acked = read_ids(os.path.join(args.out, "acked.txt"))
    problems = []

    items = scan_all()
    present = {}
    for it in items:
        i = id_of(it)
        if i in present:
            problems.append("item %d scanned twice" % i)
        present[i] = it
        if int(it["v"]["N"]) != i or it["g"]["S"] != "g%02d" % (i % GROUPS):
            problems.append("item %d holds %s" % (i, json.dumps(it)[:200]))
    lost = sorted(acked - present.keys())
    if lost:
        problems.append("%d acknowledged items lost, e.g. %s" % (len(lost), lost[:10]))
    phantom = sorted(present.keys() - attempted)
    if phantom:
        problems.append("%d items never written, e.g. %s" % (len(phantom), phantom[:10]))

    # The index converges on the table.
    deadline = time.monotonic() + 120
    while True:
        entries = scan_all(index=INDEX)
        ids = [id_of(e) for e in entries]
        if (len(ids) == len(set(ids)) and set(ids) == present.keys()) or (
            time.monotonic() > deadline
        ):
            break
        time.sleep(2)
    dup = len(ids) - len(set(ids))
    if dup:
        problems.append("%d duplicate GSI entries" % dup)
    missing = sorted(present.keys() - set(ids))
    if missing:
        problems.append("%d items missing from the GSI, e.g. %s" % (len(missing), missing[:10]))
    stale = sorted(set(ids) - present.keys())
    if stale:
        problems.append("%d GSI entries for no item, e.g. %s" % (len(stale), stale[:10]))

    shards, records = read_stream()
    # Sequence numbers order records within a shard; shards may reuse them.
    seqs = [(r["shard"], r["dynamodb"]["SequenceNumber"]) for r in records]
    if len(seqs) != len(set(seqs)):
        problems.append(
            "%d stream records repeat a sequence number in their shard"
            % (len(seqs) - len(set(seqs)))
        )
    inserts = {}
    for r in records:
        i = id_of(r["dynamodb"]["Keys"])
        if r["eventName"] != "INSERT":
            problems.append("stream record %s for item %d" % (r["eventName"], i))
        inserts[i] = inserts.get(i, 0) + 1
    twice = sorted(i for i, n in inserts.items() if n > 1)
    if twice:
        problems.append("%d items inserted twice in the stream, e.g. %s" % (len(twice), twice[:10]))
    unrecorded = sorted(present.keys() - inserts.keys())
    if unrecorded:
        problems.append(
            "%d items with no stream record, e.g. %s" % (len(unrecorded), unrecorded[:10])
        )
    unknown = sorted(inserts.keys() - present.keys())
    if unknown:
        problems.append("%d stream records for no item, e.g. %s" % (len(unknown), unknown[:10]))
    if len(shards) < 2:
        problems.append("the stream has %d shard: no split happened" % len(shards))

    print(
        "%d attempted, %d acknowledged, %d in the table, %d GSI entries, "
        "%d stream records in %d shards"
        % (len(attempted), len(acked), len(present), len(ids), len(records), len(shards))
    )
    for p in problems:
        print("FAIL " + p, file=sys.stderr)
    if not problems:
        print("dynamodb chaos: every check passed")
    return 1 if problems else 0


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("wait")
    w.add_argument("--seconds", type=float, default=90)
    sub.add_parser("setup")
    w = sub.add_parser("write")
    w.add_argument("--seconds", type=float, default=90)
    w.add_argument("--writers", type=int, default=4)
    w.add_argument("--out", required=True)
    v = sub.add_parser("verify")
    v.add_argument("--out", required=True)
    args = p.parse_args()
    return {"wait": cmd_wait, "setup": cmd_setup, "write": cmd_write, "verify": cmd_verify}[
        args.cmd
    ](args) or 0


if __name__ == "__main__":
    sys.exit(main())
