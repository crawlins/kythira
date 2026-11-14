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

#include <raft/aws_request_authorizer.hpp>
#include <raft/credential_verifier.hpp>
#include <raft/service_module.hpp>
#include <raft/static_or_iam_authorizer.hpp>

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
/// IAM's authorizer over the co-located IAM module's state
/// (`iam_evaluator`): what a consumer's embedded IAM replica gives it
/// stand-alone.
inline constexpr std::string_view iam_evaluator = "iam.evaluator";
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

/// Where a Redis gateway's cache sits in IAM's resource space: the
/// account, region and cache name its `elasticache:Connect` resource ARN
/// names.
struct elasticache_scope {
    std::string account;
    std::string region;
    std::string cache_name;
};

/// The co-located IAM module's authorizer, for the modules that
/// authenticate and authorize requests against IAM (Requirements 5.3 and
/// 7.1): what each consumer gets stand-alone from an embedded IAM replica
/// (`iam_gateway_auth.hpp`, Redis's `iam_auth.cpp`), without a second copy
/// of IAM's state in the process.
///
/// A consumer adds its service's action tables at start, before it serves,
/// then takes the hooks or the verifier. Both refer to the provider, which
/// outlives every consumer: the node stops consumers first.
class iam_evaluator {
public:
    iam_evaluator() = default;
    iam_evaluator(const iam_evaluator&) = delete;
    auto operator=(const iam_evaluator&) -> iam_evaluator& = delete;
    iam_evaluator(iam_evaluator&&) = delete;
    auto operator=(iam_evaluator&&) -> iam_evaluator& = delete;
    virtual ~iam_evaluator() = default;

    /// Registers a service's actions (`kinesis_actions()`,
    /// `dynamodb_actions()`, ...), so policies naming them are evaluated.
    /// Adding a table already present is a no-op.
    /// @throws std::invalid_argument for a malformed table.
    virtual void add_actions(const aws_auth::aws_action_table& table) = 0;

    /// SigV4 authentication and policy evaluation, for
    /// `static_or_iam_authorizer::set_mode`.
    [[nodiscard]] virtual auto hooks() -> aws_auth::iam_hooks = 0;

    /// Redis AUTH (`elasticache:Connect` on the cache `scope` names), for
    /// `redis_gateway_config::_credential_verifier`. Registers
    /// ElastiCache's actions itself: AUTH is Redis's only use of IAM.
    [[nodiscard]] virtual auto credential_verifier(const elasticache_scope& scope)
        -> credential_verifier_fn = 0;
};

}  // namespace kythira::service_node
