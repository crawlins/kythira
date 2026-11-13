# DynamoDB wire evidence

What the clients the spec names (`.kiro/specs/dynamodb-compatible-api/`,
Requirement 4.1) actually send, recorded against
`tools/evidence/recording_stub.py`: an HTTP server that answers just enough
of DynamoDB, DynamoDB Streams and Kinesis for each client to finish a normal
flow, and writes every request to `<client>.jsonl` (signatures, dates and
payload hashes masked). The stub's answers are not evidence of anything;
the requests are.

| File | Client | Version |
|---|---|---|
| `aws-cli.jsonl` | AWS CLI (`amazon/aws-cli` image) | 2.31.30 |
| `boto3.jsonl` | boto3 / botocore, client and resource layers | 1.43.126 |
| `java-sdk.jsonl` | aws-sdk-java-v2, low-level and enhanced clients | 2.57.11 |
| `lock-client.jsonl` | `dynamodb-lock-client` | 1.5.0 |
| `kcl2.jsonl` | Kinesis Client Library lease table | 2.7.4 |
| `kcl3.jsonl` | Kinesis Client Library (coordinator state, worker metrics, GSI) | 3.5.4 |
| `streams-adapter.jsonl` | `dynamodb-streams-kinesis-adapter` on KCL 3 | 2.5.0 |

`tests/dynamodb_wire_evidence_test.cpp` parses every DynamoDB and Streams
request here with the core AWS JSON codec against the pinned models, unknown
members refused. Kinesis requests (KCL's) are left to the Kinesis module.

To re-record (Docker or rootless Podman; `KYTHIRA_CONTAINER_RUNTIME` and
`KYTHIRA_COMPOSE_COMMAND` pick the runtime):

    tools/evidence/record.sh compose /tmp/evidence
    tools/evidence/summarize.py evidence --diff /tmp/evidence

`record.sh native OUT_DIR` runs the clients on the host instead (Python,
JDK 21 and Maven; the AWS CLI only if v2 is on `PATH`). The `evidence` job
in `.github/workflows/dynamodb-module.yml` re-records under both runtimes,
runs the evidence test on the fresh recording and reports the diff. To
replace the checked-in recording, record into this directory and run
`summarize.py evidence`, which rewrites `summary.json` and the tables below.

Every client also runs on Kythira itself (tasks 11 and 24), with
Terraform's `aws_dynamodb_table` besides:
`tools/evidence/clients_on_kythira.sh compose|native OUT_DIR [CLIENT...]`
starts the stub with `--dynamodb-upstream`, so each client's DynamoDB
requests reach a `dynamodb_gateway_node` (a three-node cluster in compose
mode) unchanged while the stub still serves the KCL's Kinesis calls, then
`tools/evidence/check_client_runs.py` checks what Kythira answered. The
`build` job in `dynamodb-module.yml` runs it under both runtimes.

<!-- BEGIN GENERATED (summarize.py) -->

### Operations by client

| Operation | aws-cli | boto3 | java-sdk | kcl2 | kcl3 | lock-client | streams-adapter |
|---|---|---|---|---|---|---|---|
| `streams:DescribeStream` |  |  |  |  |  |  | x |
| `streams:GetRecords` |  |  |  |  |  |  | x |
| `streams:GetShardIterator` |  |  |  |  |  |  | x |
| `BatchGetItem` |  | x |  |  |  |  |  |
| `BatchWriteItem` | x | x | x |  |  |  |  |
| `CreateTable` | x | x | x | x | x | x | x |
| `DeleteItem` | x | x | x |  | x | x | x |
| `DeleteTable` | x | x | x |  |  |  |  |
| `DescribeLimits` | x |  |  |  |  |  |  |
| `DescribeTable` | x | x | x | x | x | x | x |
| `DescribeTimeToLive` | x | x |  |  |  |  |  |
| `GetItem` | x | x | x | x | x | x | x |
| `ListTables` | x | x |  |  |  |  |  |
| `ListTagsOfResource` |  | x |  |  |  |  |  |
| `PutItem` | x | x | x | x | x | x | x |
| `Query` | x | x | x |  | x |  | x |
| `Scan` | x | x | x | x | x |  | x |
| `TagResource` |  | x |  |  |  |  |  |
| `TransactGetItems` |  | x |  |  |  |  |  |
| `TransactWriteItems` |  | x | x |  |  |  |  |
| `UntagResource` |  | x |  |  |  |  |  |
| `UpdateItem` | x | x | x | x | x | x | x |
| `UpdateTable` |  |  |  |  | x |  | x |
| `UpdateTimeToLive` | x | x |  |  |  |  |  |
| `kinesis:GetRecords` |  |  |  | x | x |  |  |
| `kinesis:GetShardIterator` |  |  |  | x | x |  |  |
| `kinesis:ListShards` |  |  |  | x | x |  |  |

### SDKs

| Client | User agent | Headers sent |
|---|---|---|
| aws-cli | `aws-cli/2.31.30` | `accept-encoding`, `authorization`, `content-length`, `content-type`, `host`, `user-agent`, `x-amz-date`, `x-amz-target` |
| boto3 | `Boto3/1.43.126` | `accept-encoding`, `amz-sdk-invocation-id`, `amz-sdk-request`, `authorization`, `content-length`, `content-type`, `host`, `user-agent`, `x-amz-date`, `x-amz-target` |
| java-sdk | `aws-sdk-java/2.57.11` | `accept`, `amz-sdk-invocation-id`, `amz-sdk-request`, `authorization`, `connection`, `content-length`, `content-type`, `host`, `user-agent`, `x-amz-content-sha256`, `x-amz-date`, `x-amz-target` |
| kcl2 | `aws-sdk-java/2.33.0` | `amz-sdk-invocation-id`, `amz-sdk-request`, `authorization`, `content-length`, `content-type`, `host`, `user-agent`, `x-amz-content-sha256`, `x-amz-date`, `x-amz-target` |
| kcl3 | `aws-sdk-java/2.41.21` | `amz-sdk-invocation-id`, `amz-sdk-request`, `authorization`, `content-length`, `content-type`, `host`, `user-agent`, `x-amz-content-sha256`, `x-amz-date`, `x-amz-target` |
| lock-client | `aws-sdk-java/2.57.11` | `accept`, `amz-sdk-invocation-id`, `amz-sdk-request`, `authorization`, `connection`, `content-length`, `content-type`, `host`, `user-agent`, `x-amz-content-sha256`, `x-amz-date`, `x-amz-target` |
| streams-adapter | `aws-sdk-java/2.56.0` | `amz-sdk-invocation-id`, `amz-sdk-request`, `authorization`, `connection`, `content-length`, `content-type`, `host`, `user-agent`, `x-amz-content-sha256`, `x-amz-date`, `x-amz-target` |

### Request members by operation

Union over all clients; `[]` is a list element, `{}` a key that is data (an attribute name or a table name).

- `streams:DescribeStream`: `StreamArn`
- `streams:GetRecords`: `Limit`, `ShardIterator`
- `streams:GetShardIterator`: `ShardId`, `ShardIteratorType`, `StreamArn`
- `BatchGetItem`: `RequestItems`
- `BatchWriteItem`: `RequestItems`
- `CreateTable`: `AttributeDefinitions`, `BillingMode`, `DeletionProtectionEnabled`, `GlobalSecondaryIndexes`, `KeySchema`, `ProvisionedThroughput`, `StreamSpecification`, `TableName`, `Tags`
- `DeleteItem`: `ConditionExpression`, `ExpressionAttributeNames`, `ExpressionAttributeValues`, `Key`, `ReturnValues`, `TableName`
- `DeleteTable`: `TableName`
- `DescribeLimits`: (empty body)
- `DescribeTable`: `TableName`
- `DescribeTimeToLive`: `TableName`
- `GetItem`: `ConsistentRead`, `ExpressionAttributeNames`, `Key`, `ProjectionExpression`, `TableName`
- `ListTables`: `Limit`
- `ListTagsOfResource`: `ResourceArn`
- `PutItem`: `ConditionExpression`, `Expected`, `ExpressionAttributeNames`, `Item`, `ReturnConsumedCapacity`, `ReturnItemCollectionMetrics`, `ReturnValuesOnConditionCheckFailure`, `TableName`
- `Query`: `ExpressionAttributeNames`, `ExpressionAttributeValues`, `FilterExpression`, `IndexName`, `KeyConditionExpression`, `Limit`, `ScanIndexForward`, `TableName`
- `Scan`: `ExclusiveStartKey`, `ExpressionAttributeNames`, `ExpressionAttributeValues`, `FilterExpression`, `Limit`, `Segment`, `Select`, `TableName`, `TotalSegments`
- `TagResource`: `ResourceArn`, `Tags`
- `TransactGetItems`: `TransactItems`
- `TransactWriteItems`: `ClientRequestToken`, `TransactItems`
- `UntagResource`: `ResourceArn`, `TagKeys`
- `UpdateItem`: `AttributeUpdates`, `ConditionExpression`, `Expected`, `ExpressionAttributeNames`, `ExpressionAttributeValues`, `Key`, `ReturnValues`, `ReturnValuesOnConditionCheckFailure`, `TableName`, `UpdateExpression`
- `UpdateTable`: `AttributeDefinitions`, `GlobalSecondaryIndexUpdates`, `TableName`
- `UpdateTimeToLive`: `TableName`, `TimeToLiveSpecification`
- `kinesis:GetRecords`: `Limit`, `ShardIterator`
- `kinesis:GetShardIterator`: `ShardId`, `ShardIteratorType`, `StreamName`
- `kinesis:ListShards`: `ShardFilter`, `StreamName`

<!-- END GENERATED -->
