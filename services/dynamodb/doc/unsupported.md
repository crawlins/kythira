# Unsupported operations

Every operation in the pinned models (`model/`) that this module does not
implement yet, with why and what it returns
(`.kiro/specs/dynamodb-compatible-api/` Requirement 2.1). An implemented
operation is named in `include/kythira/dynamodb/operations.hpp` instead;
`dynamodb_model_coverage_test` fails if an operation is in neither place,
in both, or listed here but missing from the models (Requirement 2.2).

At spec close the only rows allowed are the out-of-scope ones. Until then,
a row says which task implements the operation.

Every operation below answers HTTP 400 with
`{"__type":"com.amazon.coral.service#UnknownOperationException"}`, which
is what the module's `aws_json::dispatcher` sends for a modeled operation
with no handler, and what AWS sends for an operation it does not know. The
test reads the table rows: keep one operation per row, in the
`` | Service | `Operation` | Why | `` form.

| Service | Operation | Why |
|---|---|---|
| DynamoDB | `CreateGlobalTable` | global tables version 2017.11.29 (legacy); version 2019.11.21 is built through `UpdateTable` (task 22); task 23 decides whether the legacy calls are mapped onto it |
| DynamoDB | `DescribeGlobalTable` | global tables version 2017.11.29 (legacy); version 2019.11.21 is built through `UpdateTable` (task 22); task 23 decides whether the legacy calls are mapped onto it |
| DynamoDB | `DescribeGlobalTableSettings` | global tables version 2017.11.29 (legacy); version 2019.11.21 is built through `UpdateTable` (task 22); task 23 decides whether the legacy calls are mapped onto it |
| DynamoDB | `ListGlobalTables` | global tables version 2017.11.29 (legacy); version 2019.11.21 is built through `UpdateTable` (task 22); task 23 decides whether the legacy calls are mapped onto it |
| DynamoDB | `UpdateGlobalTable` | global tables version 2017.11.29 (legacy); version 2019.11.21 is built through `UpdateTable` (task 22); task 23 decides whether the legacy calls are mapped onto it |
| DynamoDB | `UpdateGlobalTableSettings` | global tables version 2017.11.29 (legacy); version 2019.11.21 is built through `UpdateTable` (task 22); task 23 decides whether the legacy calls are mapped onto it |
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
  strong consistency waits for task 25's decision (Requirement 18.3).
- `DeleteTable` of a table with replicas is refused; AWS deletes that
  replica. Remove the replicas with `ReplicaUpdates` `Delete` first.
- `ReplicaUpdates` cannot be combined with other `UpdateTable` changes in
  one request, and a table changes one replica at a time.
- A table need not have a DynamoDB stream to become global: replication
  reads Kythira's change log.
