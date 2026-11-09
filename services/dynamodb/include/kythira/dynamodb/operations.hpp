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

/// Implemented DynamoDB operations. None yet: task 4 adds
/// DescribeEndpoints, task 7 the control plane and single-item API.
inline constexpr std::array<std::string_view, 0> k_implemented_dynamodb_operations{};

/// Implemented DynamoDB Streams operations (task 16).
inline constexpr std::array<std::string_view, 0> k_implemented_streams_operations{};

}  // namespace kythira::dynamodb
