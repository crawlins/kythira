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
    /// AWS region string (e.g., `"us-east-1"`).  Empty value falls back to the SDK
    /// default, which honours the `$AWS_DEFAULT_REGION` environment variable.
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
