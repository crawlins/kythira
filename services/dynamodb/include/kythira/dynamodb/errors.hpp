// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file errors.hpp
/// @brief The error style DynamoDB answers with, and constructors for the
///        errors the module raises itself (`.kiro/specs/dynamodb-compatible-api/`
///        Requirement 4.2).
///
/// DynamoDB qualifies validation and protocol faults with Coral's own
/// namespaces (`com.amazon.coral.validate#ValidationException`) and its
/// modeled errors with `com.amazonaws.dynamodb.v20120810#...`. Every error
/// the module builds goes through here, so the wording and the type names
/// change in one place when task 8's differential or a recording corrects
/// them.

#include <kythira/dynamodb/operations.hpp>

#include <raft/aws_json_protocol.hpp>

#include <string>
#include <string_view>

namespace kythira::dynamodb {

/// The style the DynamoDB front end and codec use.
[[nodiscard]] inline auto dynamodb_error_style() -> aws_json::error_style {
    return aws_json::error_style::coral(k_dynamodb_error_namespace);
}

/// A `ValidationException` carrying `message` verbatim.
[[nodiscard]] inline auto validation_error(std::string message) -> aws_json::error {
    aws_json::error e;
    e.type = dynamodb_error_style().validation_type;
    e.message = std::move(message);
    return e;
}

/// The prefix AWS puts on most item and key validation messages.
inline constexpr std::string_view k_invalid_parameters =
    "One or more parameter values were invalid: ";

}  // namespace kythira::dynamodb
