#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Checks the clients' runs on Kythira (clients_on_kythira.sh, tasks 11 and
24).

Reads <client>.upstream.jsonl, the DynamoDB exchanges the stub passed to
Kythira, for each client named, and fails unless:

  * Kythira answered every request, and every refusal is one the client
    expects: ResourceNotFoundException on DescribeTable (clients probe for
    tables before they create them, and wait for them to go after a
    delete), ConditionalCheckFailedException (a write under a condition
    that another write beat: a lease, a lock, boto3's deliberate
    duplicate), or ResourceInUseException on a CreateTable sent again
    after one that succeeded (an SDK retry);
  * every client created a table (the AWS CLI, boto3, aws-sdk-java-v2, the
    lock client and Terraform also exit non-zero on any error their flows
    do not expect, which fails the run before this check);
  * each KCL client created its lease table and wrote a lease;
  * for KCL 3.x, the UpdateTable that adds LeaseOwnerToLeaseKeyIndex was
    accepted, and a Query on that index found the lease this worker took
    (the GSI is eventually consistent, so not every Query has to);
  * for the Streams Kinesis Adapter (task 16), which reads a real Kythira
    stream, every Streams call was answered and GetRecords returned
    records. The client itself fails unless it saw every item it wrote
    exactly once.

Usage: check_client_runs.py DIR CLIENT...
"""

import json
import os
import sys

EXPECTED_REFUSALS = {
    ("DescribeTable", "ResourceNotFoundException"),
    ("PutItem", "ConditionalCheckFailedException"),
    ("UpdateItem", "ConditionalCheckFailedException"),
    ("DeleteItem", "ConditionalCheckFailedException"),
}
KCL_CLIENTS = {"kcl2", "kcl3", "streams-adapter"}
LEASE_INDEX = "LeaseOwnerToLeaseKeyIndex"


def short_type(response):
    return str(response.get("__type", "")).rpartition("#")[2]


def check(path, client):
    problems = []
    try:
        with open(path, encoding="utf-8") as f:
            records = [json.loads(line) for line in f if line.strip()]
    except OSError as e:
        return ["%s: %s" % (client, e)]
    ops = {}
    created_tables = set()
    for r in records:
        op, status, resp = r["operation"], r["status"], r["response"]
        ops[op] = ops.get(op, 0) + 1
        table = r["body"].get("TableName") if isinstance(r["body"], dict) else None
        if op == "CreateTable" and status == 200:
            created_tables.add(table)
        # An SDK that gave up waiting on a CreateTable Kythira went on to
        # finish sends it again and is told the table exists; the clients
        # treat that as success, and so does the check, but only after a
        # CreateTable of that table was answered 200.
        retried_create = (
            op == "CreateTable"
            and short_type(resp) == "ResourceInUseException"
            and table in created_tables
        )
        if (
            status != 200
            and not retried_create
            and (op, short_type(resp)) not in EXPECTED_REFUSALS
        ):
            problems.append(
                "%s: seq %d %s answered %d %s"
                % (client, r["seq"], op, status, json.dumps(resp)[:300])
            )
    created = any(
        r["operation"] == "CreateTable" and r["status"] == 200 for r in records
    )
    if not created:
        problems.append("%s: no CreateTable succeeded" % client)
    if client not in KCL_CLIENTS:
        summary = ", ".join("%s %d" % kv for kv in sorted(ops.items()))
        print("%s: %d DynamoDB requests (%s)" % (client, len(records), summary))
        return problems
    lease_written = any(
        r["operation"] == "PutItem"
        and r["status"] == 200
        and "shardId-" in str(r["body"].get("Item", {}).get("leaseKey", {}).get("S", ""))
        for r in records
    )
    if not lease_written:
        problems.append("%s: no shard lease was written" % client)
    if client == "kcl3":
        index_created = any(
            r["operation"] == "UpdateTable"
            and r["status"] == 200
            and any(
                u.get("Create", {}).get("IndexName") == LEASE_INDEX
                for u in r["body"].get("GlobalSecondaryIndexUpdates", [])
            )
            for r in records
        )
        if not index_created:
            problems.append(
                "%s: the UpdateTable adding %s did not succeed" % (client, LEASE_INDEX)
            )
        found = any(
            r["operation"] == "Query"
            and r["status"] == 200
            and r["body"].get("IndexName") == LEASE_INDEX
            and any(
                str(i.get("leaseKey", {}).get("S", "")).startswith("shardId-")
                for i in r["response"].get("Items", [])
            )
            for r in records
        )
        if not found:
            problems.append(
                "%s: no Query on %s found the worker's lease" % (client, LEASE_INDEX)
            )
    if client == "streams-adapter":
        read = any(
            r["operation"] == "GetRecords"
            and r["status"] == 200
            and r["response"].get("Records")
            for r in records
        )
        if not read:
            problems.append("%s: no GetRecords returned records" % client)
    summary = ", ".join("%s %d" % kv for kv in sorted(ops.items()))
    print("%s: %d DynamoDB and Streams requests (%s)" % (client, len(records), summary))
    return problems


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    problems = []
    for client in argv[1:]:
        problems += check(os.path.join(argv[0], client + ".upstream.jsonl"), client)
    for p in problems:
        print("FAIL " + p, file=sys.stderr)
    if not problems:
        print("clients on Kythira: every check passed")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
