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
| `CreateChannel` | none | Not in the spec. Channels deliver a stream's records to Amazon S3 or S3 Tables (Iceberg), a delivery service like Firehose, which Requirement 1 leaves out of scope. Open question for the owner. | `UnknownOperationException`, HTTP 400 |
| `DeleteChannel` | none | Not in the spec. Channels deliver a stream's records to Amazon S3 or S3 Tables (Iceberg), a delivery service like Firehose, which Requirement 1 leaves out of scope. Open question for the owner. | `UnknownOperationException`, HTTP 400 |
| `DescribeChannel` | none | Not in the spec. Channels deliver a stream's records to Amazon S3 or S3 Tables (Iceberg), a delivery service like Firehose, which Requirement 1 leaves out of scope. Open question for the owner. | `UnknownOperationException`, HTTP 400 |
| `ListChannels` | none | Not in the spec. Channels deliver a stream's records to Amazon S3 or S3 Tables (Iceberg), a delivery service like Firehose, which Requirement 1 leaves out of scope. Open question for the owner. | `UnknownOperationException`, HTTP 400 |
| `UpdateChannel` | none | Not in the spec. Channels deliver a stream's records to Amazon S3 or S3 Tables (Iceberg), a delivery service like Firehose, which Requirement 1 leaves out of scope. Open question for the owner. | `UnknownOperationException`, HTTP 400 |
| `UpdateStreamWarmThroughput` | none | Warm throughput is unlocked by an AWS account's `MinimumThroughputBillingCommitment`, a billing arrangement with no Kythira equivalent; the on-demand controller (task 12) scales from observed writes instead. `UpdateStreamMode` refuses its `WarmThroughputMiBps` the same way. | `UnknownOperationException`, HTTP 400 |
