// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file elasticache_actions.hpp
/// @brief The `elasticache` actions the Redis gateway serves, with their
///        resource ARN templates and condition keys.
///
/// Taken from the AWS Service Authorization Reference
/// (https://docs.aws.amazon.com/service-authorization/latest/reference/list_amazonelasticache.html),
/// limited to `Connect`, the one action an ElastiCache IAM auth token is
/// checked against (`.kiro/specs/iam-compatible-auth/` Requirement 13.3),
/// and maintained by hand. The gateway authorizes it on both the user and
/// the replication group ARN.

#include <raft/aws_request_authorizer.hpp>

namespace kythira::iam::services {

[[nodiscard]] inline auto elasticache_actions() -> aws_auth::aws_action_table {
    return {
        .service_prefix = "elasticache",
        .actions =
            {
                {"Connect",
                 {"arn:${Partition}:elasticache:${Region}:${Account}:replicationgroup:"
                  "${ReplicationGroupId}",
                  "arn:${Partition}:elasticache:${Region}:${Account}:user:${UserId}"},
                 {"aws:ResourceTag/${TagKey}"}},
            },
    };
}

}  // namespace kythira::iam::services
