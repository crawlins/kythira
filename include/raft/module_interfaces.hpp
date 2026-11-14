// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file module_interfaces.hpp
/// @brief The in-process interfaces co-located modules publish to each
///        other (.kiro/specs/service-colocation/ Requirement 7), and the
///        names they are published under.
///
/// A consumer looks an interface up once, at start, from the
/// `module_registry`. A miss means the provider is not co-located, and the
/// consumer keeps using its network client (Requirement 7.3). Both sides
/// depend only on this header, never on each other's module.

#include <raft/service_module.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::service_node {

/// The names interfaces are published under.
namespace interface_names {
/// Kinesis's producer calls (`aws_json_endpoint`): PutRecords, PutRecord
/// and DescribeStreamSummary, which is what DynamoDB's Kinesis streaming
/// destination sends.
inline constexpr std::string_view kinesis_put_records = "kinesis.put_records";
}  // namespace interface_names

/// One AWS JSON-protocol request, signed with SigV4 exactly as it would be
/// sent over the network (`Host`, `X-Amz-Target`, `X-Amz-Date`,
/// `Authorization` and so on in `headers`).
struct aws_json_request {
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

/// The answer: the HTTP status and body the network call would have had.
struct aws_json_response {
    int status = 0;
    std::string body;
};

/// A co-located AWS-compatible API taking signed requests without a
/// network hop.
///
/// The request is authenticated and authorized by its signature, exactly
/// as a network request is, so co-location never widens access
/// (Requirement 7.2): the principal is whoever signed it, which for a
/// module's background work is the credentials the module is configured
/// with. `who` names the calling module for the provider's logs.
class aws_json_endpoint {
public:
    aws_json_endpoint() = default;
    aws_json_endpoint(const aws_json_endpoint&) = delete;
    auto operator=(const aws_json_endpoint&) -> aws_json_endpoint& = delete;
    aws_json_endpoint(aws_json_endpoint&&) = delete;
    auto operator=(aws_json_endpoint&&) -> aws_json_endpoint& = delete;
    virtual ~aws_json_endpoint() = default;

    /// Thread-safe. After the provider stops, every call answers 503.
    [[nodiscard]] virtual auto call(const caller& who, const aws_json_request& request)
        -> aws_json_response = 0;
};

}  // namespace kythira::service_node
