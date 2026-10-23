// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_client_config.hpp
/// @brief Shared AWS connection settings for the EC2 and ASG quorum manager implementations.

#include <chrono>
#include <string>

#ifdef KYTHIRA_HAS_AWS_SDK
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <memory>
#endif

namespace kythira {

/// @brief Shared AWS connection settings consumed by both quorum manager implementations.
struct aws_client_config {
    /// AWS region string (e.g., `"us-east-1"`).  Empty falls back to the SDK chain,
    /// first match wins: `$AWS_DEFAULT_REGION`, then `$AWS_REGION`, then `region` in
    /// the shared config profile (`$AWS_CONFIG_FILE` or `~/.aws/config`, profile
    /// from `$AWS_PROFILE`), then EC2 instance metadata unless IMDS is disabled,
    /// then `us-east-1`.  Measured against aws-sdk-cpp 1.11.665; see
    /// `.kiro/specs/object-backup-s3-endpoint/design.md`.
    std::string region;

    /// When non-empty, overrides the service endpoint.  Use this to point at a
    /// LocalStack or Moto instance during integration tests.
    std::string endpoint_override;

    /// Maximum time allowed for a single AWS API call.  Applies to both the connect
    /// and request phases of the HTTP client.
    std::chrono::seconds api_timeout{30};

#ifdef KYTHIRA_HAS_AWS_SDK
    /// Credentials provider chain used when constructing AWS service clients.
    ///
    /// When `nullptr`, the SDK's `DefaultAWSCredentialsProviderChain` is used, which
    /// probes environment variables, `~/.aws/credentials`, the EC2 instance-metadata
    /// service, and ECS task-role endpoints in order.
    std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> credentials_provider;
#endif
};

}  // namespace kythira
