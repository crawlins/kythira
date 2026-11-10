# Kinesis operations not implemented

`.kiro/specs/kinesis-compatible-streams/` Requirement 2.1: every operation in
the pinned model (`../model/service-2.json`, botocore 1.43.100) is either
implemented, and then listed in `../implemented-operations.txt`, or listed
here with its reason and the exact error it returns.
`tools/check_coverage.py` (the ctest `kinesis_model_coverage`) fails when an
operation is in neither list, so a model upgrade that adds one fails until it
is placed.

The front end (task 4) answers an operation listed here, once the request
is signed, like an `X-Amz-Target` the model does not have:
`UnknownOperationException` with HTTP status 400, which is what AWS's JSON
protocol returns for an unknown target and what every SDK surfaces as an
unknown-operation error rather than a retryable fault.

| Operation | Planned | Reason | Error |
|---|---|---|---|
| `AddTagsToStream` | task 15 | Not built yet (tags, policies, monitoring, account). | `UnknownOperationException`, HTTP 400 |
| `CreateChannel` | none | Not in the spec. Channels deliver a stream's records to Amazon S3 or S3 Tables (Iceberg), a delivery service like Firehose, which Requirement 1 leaves out of scope. Open question for the owner. | `UnknownOperationException`, HTTP 400 |
| `DecreaseStreamRetentionPeriod` | task 10 | Not built yet (retention and tiering). | `UnknownOperationException`, HTTP 400 |
| `DeleteChannel` | none | Not in the spec. Channels deliver a stream's records to Amazon S3 or S3 Tables (Iceberg), a delivery service like Firehose, which Requirement 1 leaves out of scope. Open question for the owner. | `UnknownOperationException`, HTTP 400 |
| `DeleteResourcePolicy` | task 15 | Not built yet (tags, policies, monitoring, account). | `UnknownOperationException`, HTTP 400 |
| `DeregisterStreamConsumer` | task 13 | Not built yet (enhanced fan-out). | `UnknownOperationException`, HTTP 400 |
| `DescribeAccountSettings` | task 15 | Not built yet (tags, policies, monitoring, account). | `UnknownOperationException`, HTTP 400 |
| `DescribeChannel` | none | Not in the spec. Channels deliver a stream's records to Amazon S3 or S3 Tables (Iceberg), a delivery service like Firehose, which Requirement 1 leaves out of scope. Open question for the owner. | `UnknownOperationException`, HTTP 400 |
| `DescribeStreamConsumer` | task 13 | Not built yet (enhanced fan-out). | `UnknownOperationException`, HTTP 400 |
| `DisableEnhancedMonitoring` | task 15 | Not built yet (tags, policies, monitoring, account). | `UnknownOperationException`, HTTP 400 |
| `EnableEnhancedMonitoring` | task 15 | Not built yet (tags, policies, monitoring, account). | `UnknownOperationException`, HTTP 400 |
| `GetResourcePolicy` | task 15 | Not built yet (tags, policies, monitoring, account). | `UnknownOperationException`, HTTP 400 |
| `IncreaseStreamRetentionPeriod` | task 10 | Not built yet (retention and tiering). | `UnknownOperationException`, HTTP 400 |
| `ListChannels` | none | Not in the spec. Channels deliver a stream's records to Amazon S3 or S3 Tables (Iceberg), a delivery service like Firehose, which Requirement 1 leaves out of scope. Open question for the owner. | `UnknownOperationException`, HTTP 400 |
| `ListStreamConsumers` | task 13 | Not built yet (enhanced fan-out). | `UnknownOperationException`, HTTP 400 |
| `ListTagsForResource` | task 15 | Not built yet (tags, policies, monitoring, account). | `UnknownOperationException`, HTTP 400 |
| `ListTagsForStream` | task 15 | Not built yet (tags, policies, monitoring, account). | `UnknownOperationException`, HTTP 400 |
| `MergeShards` | task 11 | Not built yet (resharding). | `UnknownOperationException`, HTTP 400 |
| `PutResourcePolicy` | task 15 | Not built yet (tags, policies, monitoring, account). | `UnknownOperationException`, HTTP 400 |
| `RegisterStreamConsumer` | task 13 | Not built yet (enhanced fan-out). | `UnknownOperationException`, HTTP 400 |
| `RemoveTagsFromStream` | task 15 | Not built yet (tags, policies, monitoring, account). | `UnknownOperationException`, HTTP 400 |
| `SplitShard` | task 11 | Not built yet (resharding). | `UnknownOperationException`, HTTP 400 |
| `StartStreamEncryption` | task 14 | Not built yet (encryption at rest). | `UnknownOperationException`, HTTP 400 |
| `StopStreamEncryption` | task 14 | Not built yet (encryption at rest). | `UnknownOperationException`, HTTP 400 |
| `SubscribeToShard` | task 13 | Not built yet (enhanced fan-out). | `UnknownOperationException`, HTTP 400 |
| `TagResource` | task 15 | Not built yet (tags, policies, monitoring, account). | `UnknownOperationException`, HTTP 400 |
| `UntagResource` | task 15 | Not built yet (tags, policies, monitoring, account). | `UnknownOperationException`, HTTP 400 |
| `UpdateAccountSettings` | task 15 | Not built yet (tags, policies, monitoring, account). | `UnknownOperationException`, HTTP 400 |
| `UpdateChannel` | none | Not in the spec. Channels deliver a stream's records to Amazon S3 or S3 Tables (Iceberg), a delivery service like Firehose, which Requirement 1 leaves out of scope. Open question for the owner. | `UnknownOperationException`, HTTP 400 |
| `UpdateMaxRecordSize` | task 15 | Not built yet (tags, policies, monitoring, account). | `UnknownOperationException`, HTTP 400 |
| `UpdateShardCount` | task 11 | Not built yet (resharding). | `UnknownOperationException`, HTTP 400 |
| `UpdateStreamMode` | task 12 | Not built yet (on-demand mode). | `UnknownOperationException`, HTTP 400 |
| `UpdateStreamWarmThroughput` | task 12 | Not built yet (on-demand mode). | `UnknownOperationException`, HTTP 400 |
