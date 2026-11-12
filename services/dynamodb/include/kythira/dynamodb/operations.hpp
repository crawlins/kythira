// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file operations.hpp
/// @brief Which operations of the pinned models this module implements
///        (`.kiro/specs/dynamodb-compatible-api/` Requirement 2).
///
/// Every operation in `model/dynamodb-2012-08-10.json` and
/// `model/dynamodbstreams-2012-08-10.json` is either named here or listed in
/// `doc/unsupported.md` with the error it returns, never both;
/// `dynamodb_model_coverage_test` holds the two to that. A task that
/// implements an operation adds it here, registers its handler with the
/// front end's `aws_json::dispatcher`, and removes its row from
/// `unsupported.md` in the same change.

#include <kythira/dynamodb/model/dynamodb_model.hpp>
#include <kythira/dynamodb/model/dynamodbstreams_model.hpp>

#include <array>
#include <string_view>

namespace kythira::dynamodb {

/// The two services the module answers, by their `X-Amz-Target` prefixes.
inline constexpr std::string_view k_dynamodb_target_prefix = "DynamoDB_20120810";
inline constexpr std::string_view k_streams_target_prefix = "DynamoDBStreams_20120810";

/// The Coral namespace DynamoDB qualifies its modeled errors with
/// (`com.amazonaws.dynamodb.v20120810#ResourceNotFoundException`), for
/// `aws_json::error_style::coral`.
inline constexpr std::string_view k_dynamodb_error_namespace = "com.amazonaws.dynamodb.v20120810";

/// Implemented DynamoDB operations: DescribeEndpoints (task 4, built into
/// the front end), the control plane (`table_api.hpp`) and single-item
/// API (`item_api.hpp`) of task 7, and the range reads (`query_api.hpp`)
/// and batches (`batch_api.hpp`) of task 8, and the transactions
/// (`txn_api.hpp`) of task 10, time to live (`ttl.hpp`) of task 13, PartiQL
/// (`partiql.hpp`) of task 14, and Kinesis streaming destinations
/// (`kinesis_destination.hpp`) of task 17, backups (`backup.hpp`) of task
/// 18, point-in-time recovery (`pitr.hpp`) of task 19, and exports,
/// imports (`export_import.hpp`) and bulk puts (`bulk_put.hpp`) of task 20,
/// and resource policies (`resource_policy.hpp`) and Contributor Insights
/// (`insights_api.hpp`) of task 21, and vector search (`vector_search.hpp`)
/// of task 26.
inline constexpr std::array<std::string_view, 54> k_implemented_dynamodb_operations{
    "DescribeEndpoints",
    "CreateTable",
    "DescribeTable",
    "ListTables",
    "UpdateTable",
    "DeleteTable",
    "DescribeLimits",
    "TagResource",
    "UntagResource",
    "ListTagsOfResource",
    "GetItem",
    "PutItem",
    "UpdateItem",
    "DeleteItem",
    "Query",
    "Scan",
    "BatchGetItem",
    "BatchWriteItem",
    "TransactGetItems",
    "TransactWriteItems",
    "UpdateTimeToLive",
    "DescribeTimeToLive",
    "ExecuteStatement",
    "BatchExecuteStatement",
    "ExecuteTransaction",
    "EnableKinesisStreamingDestination",
    "DisableKinesisStreamingDestination",
    "DescribeKinesisStreamingDestination",
    "UpdateKinesisStreamingDestination",
    "CreateBackup",
    "DescribeBackup",
    "ListBackups",
    "DeleteBackup",
    "RestoreTableFromBackup",
    "UpdateContinuousBackups",
    "DescribeContinuousBackups",
    "RestoreTableToPointInTime",
    "ExportTableToPointInTime",
    "DescribeExport",
    "ListExports",
    "ImportTable",
    "DescribeImport",
    "ListImports",
    "StartBulkPutItems",
    "DescribeBulkOperation",
    "ListBulkOperations",
    "CancelBulkOperation",
    "PutResourcePolicy",
    "GetResourcePolicy",
    "DeleteResourcePolicy",
    "UpdateContributorInsights",
    "DescribeContributorInsights",
    "ListContributorInsights",
    "SearchVectors"};

/// Implemented DynamoDB Streams operations (task 16).
inline constexpr std::array<std::string_view, 4> k_implemented_streams_operations{
    "ListStreams", "DescribeStream", "GetShardIterator", "GetRecords"};

/// Whether `op` is a DynamoDB Streams operation (sent with
/// `k_streams_target_prefix`).
[[nodiscard]] inline auto is_streams_operation(std::string_view op) -> bool {
    return dynamodbstreams_model::k_model.find_operation(op) != nullptr;
}

}  // namespace kythira::dynamodb
