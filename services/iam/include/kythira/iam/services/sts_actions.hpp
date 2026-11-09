// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file sts_actions.hpp
/// @brief The `sts` service's actions, resource ARN templates and condition keys.
///
/// Taken from the AWS Service Authorization Reference
/// (https://docs.aws.amazon.com/service-authorization/latest/reference/list_awssecuritytokenservice.html),
/// limited to the actions Kythira serves or plans to
/// (`.kiro/specs/iam-compatible-auth/`), and maintained by hand.

#include <raft/aws_request_authorizer.hpp>

namespace kythira::iam::services {

[[nodiscard]] inline auto sts_actions() -> aws_auth::aws_action_table {
    return {
        .service_prefix = "sts",
        .actions =
            {
                {"AssumeRole",
                 {"arn:${Partition}:iam::${Account}:role/${RoleNameWithPath}"},
                 {"aws:RequestTag/${TagKey}", "aws:TagKeys", "sts:ExternalId",
                  "sts:RoleSessionName", "sts:SourceIdentity", "sts:TransitiveTagKeys"}},
                {"AssumeRoleWithWebIdentity",
                 {"arn:${Partition}:iam::${Account}:role/${RoleNameWithPath}"},
                 {"sts:RoleSessionName", "sts:SourceIdentity", "aws:RequestTag/${TagKey}",
                  "aws:TagKeys"}},
                {"TagSession",
                 {"arn:${Partition}:iam::${Account}:role/${RoleNameWithPath}"},
                 {"aws:RequestTag/${TagKey}", "aws:TagKeys", "sts:TransitiveTagKeys"}},
                {"SetSourceIdentity",
                 {"arn:${Partition}:iam::${Account}:role/${RoleNameWithPath}"},
                 {"sts:SourceIdentity"}},
                {"GetSessionToken", {"*"}, {}},
                {"GetFederationToken",
                 {"arn:${Partition}:sts::${Account}:federated-user/${UserName}"},
                 {"aws:RequestTag/${TagKey}", "aws:TagKeys"}},
                {"GetCallerIdentity", {"*"}, {}},
                {"DecodeAuthorizationMessage", {"*"}, {}},
                {"GetAccessKeyInfo", {"*"}, {}},
            },
    };
}

}  // namespace kythira::iam::services
