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
| DynamoDB | `ExportTableToPointInTime` | planned, task 20 |
| DynamoDB | `DescribeExport` | planned, task 20 |
| DynamoDB | `ListExports` | planned, task 20 |
| DynamoDB | `ImportTable` | planned, task 20 |
| DynamoDB | `DescribeImport` | planned, task 20 |
| DynamoDB | `ListImports` | planned, task 20 |
| DynamoDB | `PutResourcePolicy` | planned, task 21 |
| DynamoDB | `GetResourcePolicy` | planned, task 21 |
| DynamoDB | `DeleteResourcePolicy` | planned, task 21 |
| DynamoDB | `UpdateContributorInsights` | planned, task 21 |
| DynamoDB | `DescribeContributorInsights` | planned, task 21 |
| DynamoDB | `ListContributorInsights` | planned, task 21 |
| DynamoDB | `CreateGlobalTable` | global tables version 2017.11.29 (legacy); task 22 builds version 2019.11.21 through `UpdateTable`, task 23 decides whether the legacy calls are mapped onto it |
| DynamoDB | `DescribeGlobalTable` | global tables version 2017.11.29 (legacy); task 22 builds version 2019.11.21 through `UpdateTable`, task 23 decides whether the legacy calls are mapped onto it |
| DynamoDB | `DescribeGlobalTableSettings` | global tables version 2017.11.29 (legacy); task 22 builds version 2019.11.21 through `UpdateTable`, task 23 decides whether the legacy calls are mapped onto it |
| DynamoDB | `ListGlobalTables` | global tables version 2017.11.29 (legacy); task 22 builds version 2019.11.21 through `UpdateTable`, task 23 decides whether the legacy calls are mapped onto it |
| DynamoDB | `UpdateGlobalTable` | global tables version 2017.11.29 (legacy); task 22 builds version 2019.11.21 through `UpdateTable`, task 23 decides whether the legacy calls are mapped onto it |
| DynamoDB | `UpdateGlobalTableSettings` | global tables version 2017.11.29 (legacy); task 22 builds version 2019.11.21 through `UpdateTable`, task 23 decides whether the legacy calls are mapped onto it |
| DynamoDB | `DescribeTableReplicaAutoScaling` | out of scope: Application Auto Scaling is a separate AWS service (Introduction, "Out of scope") |
| DynamoDB | `UpdateTableReplicaAutoScaling` | out of scope: Application Auto Scaling is a separate AWS service (Introduction, "Out of scope") |
| DynamoDB | `StartBulkPutItems` | planned, task 20 |
| DynamoDB | `DescribeBulkOperation` | planned, task 20 |
| DynamoDB | `ListBulkOperations` | planned, task 20 |
| DynamoDB | `CancelBulkOperation` | planned, task 20 |
| DynamoDB | `SearchVectors` | planned, task 26 |
