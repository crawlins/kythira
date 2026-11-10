// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kinesis_errors.hpp
/// @brief Kinesis error shapes (`.kiro/specs/kinesis-compatible-streams/`
///        Requirement 3.4, 4.3).
///
/// Kinesis is a bare-named JSON 1.1 service: `__type` is the exception
/// name with no Coral namespace (`ResourceNotFoundException`), in JSON and
/// CBOR alike, and the text is under `message`.

#include <raft/aws_json_protocol.hpp>

#include <string>

namespace kythira::kinesis {

[[nodiscard]] inline auto kinesis_error_style() -> aws_json::error_style {
    return aws_json::error_style::bare();
}

/// `InvalidArgumentException` (HTTP 400) carrying `message` verbatim.
[[nodiscard]] inline auto invalid_argument(std::string message) -> aws_json::error {
    return kinesis_error_style().modeled("InvalidArgumentException", std::move(message));
}

/// `ResourceNotFoundException` (HTTP 400) carrying `message` verbatim.
[[nodiscard]] inline auto resource_not_found(std::string message) -> aws_json::error {
    return kinesis_error_style().modeled("ResourceNotFoundException", std::move(message));
}

/// `InternalFailure` (HTTP 500), with AWS's terse text and no detail.
[[nodiscard]] inline auto internal_failure() -> aws_json::error {
    aws_json::error e;
    e.status = 500;
    e.type = kinesis_error_style().internal_type;
    e.message = "Internal service failure.";
    return e;
}

}  // namespace kythira::kinesis
