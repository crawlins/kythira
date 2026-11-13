# Unsupported operations

Every operation in the pinned models (`model/`) that this module does not
implement yet, with why and what it returns
(`.kiro/specs/dynamodb-compatible-api/` Requirement 2.1). An implemented
operation is named in `include/kythira/dynamodb/operations.hpp` instead;
`dynamodb_model_coverage_test` fails if an operation is in neither place,
in both, or listed here but missing from the models (Requirement 2.2).

Since task 23 the only rows allowed are out-of-scope ones, each saying why;
`dynamodb_model_coverage_test` checks that too.

Every operation below answers HTTP 400 with
`{"__type":"com.amazon.coral.service#UnknownOperationException"}`, which
is what the module's `aws_json::dispatcher` sends for a modeled operation
with no handler, and what AWS sends for an operation it does not know. The
test reads the table rows: keep one operation per row, in the
`` | Service | `Operation` | Why | `` form.

| Service | Operation | Why |
|---|---|---|
| DynamoDB | `CreateGlobalTable` | out of scope: global tables version 2017.11.29 (legacy), which AWS asks new tables not to use; version 2019.11.21 works through `UpdateTable` and `DescribeTable` (task 22), and AWS's legacy calls do not see those tables either (task 23's decision) |
| DynamoDB | `DescribeGlobalTable` | out of scope: global tables version 2017.11.29 (legacy), which AWS asks new tables not to use; version 2019.11.21 works through `UpdateTable` and `DescribeTable` (task 22), and AWS's legacy calls do not see those tables either (task 23's decision) |
| DynamoDB | `DescribeGlobalTableSettings` | out of scope: global tables version 2017.11.29 (legacy), which AWS asks new tables not to use; version 2019.11.21 works through `UpdateTable` and `DescribeTable` (task 22), and AWS's legacy calls do not see those tables either (task 23's decision) |
| DynamoDB | `ListGlobalTables` | out of scope: global tables version 2017.11.29 (legacy), which AWS asks new tables not to use; version 2019.11.21 works through `UpdateTable` and `DescribeTable` (task 22), and AWS's legacy calls do not see those tables either (task 23's decision) |
| DynamoDB | `UpdateGlobalTable` | out of scope: global tables version 2017.11.29 (legacy), which AWS asks new tables not to use; version 2019.11.21 works through `UpdateTable` and `DescribeTable` (task 22), and AWS's legacy calls do not see those tables either (task 23's decision) |
| DynamoDB | `UpdateGlobalTableSettings` | out of scope: global tables version 2017.11.29 (legacy), which AWS asks new tables not to use; version 2019.11.21 works through `UpdateTable` and `DescribeTable` (task 22), and AWS's legacy calls do not see those tables either (task 23's decision) |
| DynamoDB | `DescribeTableReplicaAutoScaling` | out of scope: Application Auto Scaling is a separate AWS service (Introduction, "Out of scope") |
| DynamoDB | `UpdateTableReplicaAutoScaling` | out of scope: Application Auto Scaling is a separate AWS service (Introduction, "Out of scope") |

## Global tables: what `ReplicaUpdates` does not do

Version 2019.11.21 global tables (task 22) take `Create` and `Delete`
replica updates. These parts of the API answer `ValidationException`
("Kythira does not support ... yet"), or differ, as listed:

- `ReplicaUpdates` `Update`, and the per-replica overrides on `Create`
  (`KMSMasterKeyId`, `ProvisionedThroughputOverride`,
  `OnDemandThroughputOverride`, `GlobalSecondaryIndexes`,
  `TableClassOverride`): replicas take the source table's settings.
- `MultiRegionConsistency` and `GlobalTableWitnessUpdates`: multi-region
  strong consistency is planned (task 27, Requirement 18.3). It needs every
  write to commit in a quorum spanning clusters, and a Kythira group's
  voters all sit in one cluster today; `doc/dynamodb.md` explains.
- `DeleteTable` of a table with replicas is refused; AWS deletes that
  replica. Remove the replicas with `ReplicaUpdates` `Delete` first.
- `ReplicaUpdates` cannot be combined with other `UpdateTable` changes in
  one request, and a table changes one replica at a time.
- A table need not have a DynamoDB stream to become global: replication
  reads Kythira's change log.
