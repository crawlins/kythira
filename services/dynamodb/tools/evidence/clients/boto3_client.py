#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""The boto3 evidence client (`.kiro/specs/dynamodb-compatible-api/` task 1).

Runs a typical application flow against the recording stub: create a table
and wait for it with boto3's waiter, write and read items through both the
low-level client and the resource layer (whose Key/Attr builders produce
expressions with name and value placeholders), conditional writes, update
expressions, a paginated Query and Scan, batch reads and writes, a
transaction, TTL, tags, then delete the table.

Usage: ENDPOINT=http://localhost:8000 boto3_client.py
"""

import os

import boto3
from boto3.dynamodb.conditions import Attr, Key
from botocore.config import Config
from botocore.exceptions import ClientError

ENDPOINT = os.environ.get("ENDPOINT", "http://localhost:8000")
TABLE = "evidence-boto3"


def main():
    session = boto3.session.Session(
        aws_access_key_id="EVIDENCE-boto3",
        aws_secret_access_key="evidence-secret-not-a-credential",
        region_name="us-east-1",
    )
    cfg = Config(retries={"max_attempts": 2, "mode": "standard"})
    ddb = session.client("dynamodb", endpoint_url=ENDPOINT, config=cfg)
    res = session.resource("dynamodb", endpoint_url=ENDPOINT, config=cfg)

    ddb.create_table(
        TableName=TABLE,
        AttributeDefinitions=[
            {"AttributeName": "pk", "AttributeType": "S"},
            {"AttributeName": "sk", "AttributeType": "N"},
        ],
        KeySchema=[
            {"AttributeName": "pk", "KeyType": "HASH"},
            {"AttributeName": "sk", "KeyType": "RANGE"},
        ],
        BillingMode="PAY_PER_REQUEST",
        Tags=[{"Key": "team", "Value": "evidence"}],
    )
    ddb.get_waiter("table_exists").wait(TableName=TABLE)
    ddb.describe_table(TableName=TABLE)
    ddb.list_tables(Limit=10)

    # Low-level client: typed attribute values.
    ddb.put_item(
        TableName=TABLE,
        Item={
            "pk": {"S": "user#1"},
            "sk": {"N": "1"},
            "name": {"S": "Ada"},
            "tags": {"SS": ["a", "b"]},
            "blob": {"B": b"\x00\x01"},
            "nested": {"M": {"l": {"L": [{"N": "1"}, {"BOOL": True}, {"NULL": True}]}}},
        },
        ConditionExpression="attribute_not_exists(pk)",
        ReturnConsumedCapacity="TOTAL",
    )
    try:
        ddb.put_item(
            TableName=TABLE,
            Item={"pk": {"S": "user#1"}, "sk": {"N": "1"}},
            ConditionExpression="attribute_not_exists(pk)",
            ReturnValuesOnConditionCheckFailure="ALL_OLD",
        )
    except ClientError as e:
        assert e.response["Error"]["Code"] == "ConditionalCheckFailedException", e
    ddb.get_item(
        TableName=TABLE,
        Key={"pk": {"S": "user#1"}, "sk": {"N": "1"}},
        ConsistentRead=True,
        ProjectionExpression="#n, nested.l[0]",
        ExpressionAttributeNames={"#n": "name"},
    )
    ddb.update_item(
        TableName=TABLE,
        Key={"pk": {"S": "user#1"}, "sk": {"N": "1"}},
        # BLOB is a DynamoDB reserved word, so it needs a placeholder too.
        UpdateExpression="SET #n = :n, visits = if_not_exists(visits, :zero) + :one REMOVE #b",
        ConditionExpression="attribute_exists(pk)",
        ExpressionAttributeNames={"#n": "name", "#b": "blob"},
        ExpressionAttributeValues={":n": {"S": "Ada L"}, ":zero": {"N": "0"}, ":one": {"N": "1"}},
        ReturnValues="ALL_NEW",
    )

    # Resource layer: Python types, Key/Attr condition builders.
    table = res.Table(TABLE)
    table.put_item(Item={"pk": "user#2", "sk": 1, "score": 10})
    table.update_item(
        Key={"pk": "user#2", "sk": 1},
        UpdateExpression="ADD score :d",
        ExpressionAttributeValues={":d": 5},
    )
    table.query(
        KeyConditionExpression=Key("pk").eq("user#1") & Key("sk").between(0, 10),
        FilterExpression=Attr("name").begins_with("Ada"),
        ScanIndexForward=False,
        Limit=1,
    )
    table.scan(FilterExpression=Attr("score").gt(1), Select="ALL_ATTRIBUTES")

    # Paginators.
    for _ in ddb.get_paginator("scan").paginate(TableName=TABLE, PaginationConfig={"PageSize": 1}):
        pass
    for _ in ddb.get_paginator("query").paginate(
        TableName=TABLE,
        KeyConditionExpression="pk = :p",
        ExpressionAttributeValues={":p": {"S": "user#1"}},
    ):
        pass

    # Batches (the resource batch_writer buffers and resends unprocessed).
    # DynamoDB refuses a batch naming one key twice, so the writer drops
    # the put of sk 2 that the delete supersedes.
    with table.batch_writer(overwrite_by_pkeys=["pk", "sk"]) as bw:
        for i in range(3):
            bw.put_item(Item={"pk": "batch", "sk": i})
        bw.delete_item(Key={"pk": "batch", "sk": 2})
    ddb.batch_get_item(
        RequestItems={
            TABLE: {
                "Keys": [{"pk": {"S": "batch"}, "sk": {"N": str(i)}} for i in range(2)],
                "ConsistentRead": True,
            }
        }
    )

    # Transactions.
    ddb.transact_write_items(
        TransactItems=[
            {
                "Put": {
                    "TableName": TABLE,
                    "Item": {"pk": {"S": "txn"}, "sk": {"N": "1"}},
                    "ConditionExpression": "attribute_not_exists(pk)",
                }
            },
            {
                "Update": {
                    "TableName": TABLE,
                    "Key": {"pk": {"S": "user#2"}, "sk": {"N": "1"}},
                    "UpdateExpression": "SET score = score - :d",
                    "ExpressionAttributeValues": {":d": {"N": "1"}},
                }
            },
            {
                "ConditionCheck": {
                    "TableName": TABLE,
                    "Key": {"pk": {"S": "user#1"}, "sk": {"N": "1"}},
                    "ConditionExpression": "attribute_exists(pk)",
                }
            },
        ],
        ClientRequestToken="evidence-boto3-token-1",
    )
    ddb.transact_get_items(
        TransactItems=[{"Get": {"TableName": TABLE, "Key": {"pk": {"S": "txn"}, "sk": {"N": "1"}}}}]
    )

    ddb.update_time_to_live(
        TableName=TABLE, TimeToLiveSpecification={"Enabled": True, "AttributeName": "expires"}
    )
    ddb.describe_time_to_live(TableName=TABLE)
    arn = ddb.describe_table(TableName=TABLE)["Table"]["TableArn"]
    ddb.tag_resource(ResourceArn=arn, Tags=[{"Key": "k", "Value": "v"}])
    ddb.list_tags_of_resource(ResourceArn=arn)
    ddb.untag_resource(ResourceArn=arn, TagKeys=["k"])

    ddb.delete_item(
        TableName=TABLE,
        Key={"pk": {"S": "user#2"}, "sk": {"N": "1"}},
        ReturnValues="ALL_OLD",
    )
    ddb.delete_table(TableName=TABLE)
    print("boto3 evidence flow done")


if __name__ == "__main__":
    main()
