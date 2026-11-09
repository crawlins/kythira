#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""The evidence scenario through boto3. Usage: boto3_client.py ENDPOINT"""

import sys

import boto3

kinesis = boto3.client("kinesis", endpoint_url=sys.argv[1], region_name="us-east-1")
kinesis.create_stream(StreamName="evidence", ShardCount=1)
kinesis.get_waiter("stream_exists").wait(StreamName="evidence")
kinesis.list_shards(StreamName="evidence")
kinesis.put_record(StreamName="evidence", PartitionKey="pk-1", Data=b"hello")
kinesis.put_records(StreamName="evidence", Records=[
    {"PartitionKey": "pk-2", "Data": b"one"},
    {"PartitionKey": "pk-3", "Data": b"two", "ExplicitHashKey": "42"},
])
it = kinesis.get_shard_iterator(StreamName="evidence", ShardId="shardId-000000000000",
                                ShardIteratorType="TRIM_HORIZON")["ShardIterator"]
records = kinesis.get_records(ShardIterator=it, Limit=100)["Records"]
assert [r["Data"] for r in records] == [b"hello", b"one", b"two"], records
kinesis.delete_stream(StreamName="evidence", EnforceConsumerDeletion=True)
