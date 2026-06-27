#pragma once

#include <chrono>
#include <string>

#ifdef KYTHIRA_HAS_AWS_SDK
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <memory>
#endif

namespace kythira {

// Shared AWS connection settings for both quorum manager implementations.
struct aws_client_config {
    std::string region;             // empty = SDK default / $AWS_DEFAULT_REGION
    std::string endpoint_override;  // non-empty overrides the service endpoint (e.g. LocalStack)
    std::chrono::seconds api_timeout{30};

#ifdef KYTHIRA_HAS_AWS_SDK
    // Credentials provider chain used when constructing AWS service clients.
    // When null, DefaultAWSCredentialsProviderChain is used (env vars,
    // ~/.aws/credentials, IAM instance profile, ECS task role, etc.).
    std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> credentials_provider;
#endif
};

}  // namespace kythira
