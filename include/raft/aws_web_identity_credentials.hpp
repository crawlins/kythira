// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// AWS SDK credentials from a Kythira OIDC token (`.kiro/specs/iam-compatible-auth/`
// Requirement 2.2, task 3).
//
// Every AWS-backed component in the tree takes its credentials from
// `aws_client_config::credentials_provider`, a
// `shared_ptr<Aws::Auth::AWSCredentialsProviderChain>`: `aws_s3_client`, the
// ACM-PCA certificate provider, the EC2 and ASG quorum managers and EC2 peer
// discovery. So one adapter covers them all: a chain whose only link asks an
// `aws_web_identity_source` for the current STS credentials.
//
//     auto tokens = std::make_shared<cloud_credentials::kythira_oidc_token_source>(token_cfg);
//     auto sts = std::make_shared<cloud_credentials::aws_web_identity_source>(tokens, role_cfg);
//     aws_client_config aws;
//     aws.credentials_provider = cloud_credentials::make_web_identity_provider_chain(sts);
//
// The source caches and refreshes, so the SDK's call on every request is a
// cache hit. When the exchange fails and nothing valid is cached, the adapter
// returns empty credentials, which is the SDK's own convention for "no
// credentials" (its built-in providers do the same), and logs why through the
// SDK logger. An exception thrown out of GetAWSCredentials would unwind
// through SDK frames that do not expect one.

#ifdef KYTHIRA_HAS_AWS_SDK

#include <raft/cloud_credential_exchange.hpp>

#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/memory/AWSMemory.h>

#include <exception>
#include <functional>
#include <memory>
#include <utility>

namespace kythira::cloud_credentials {

/// The SDK's form of a set of STS credentials, expiry included, so the SDK
/// can tell they are temporary.
[[nodiscard]] inline auto to_aws_sdk_credentials(const aws_sts_credentials& c)
    -> Aws::Auth::AWSCredentials {
    return Aws::Auth::AWSCredentials(Aws::String(c.access_key_id), Aws::String(c.secret_access_key),
                                     Aws::String(c.session_token),
                                     Aws::Utils::DateTime(c.expires_at));
}

/// An `AWSCredentialsProvider` over any function returning `aws_sts_credentials`
/// (normally `aws_web_identity_source::current`).
class web_identity_credentials_provider final : public Aws::Auth::AWSCredentialsProvider {
public:
    explicit web_identity_credentials_provider(std::function<aws_sts_credentials()> source)
        : _source(std::move(source)) {}

    auto GetAWSCredentials() -> Aws::Auth::AWSCredentials override {
        try {
            return to_aws_sdk_credentials(_source());
        } catch (const std::exception& e) {
            AWS_LOGSTREAM_ERROR("kythira_web_identity",
                                "AssumeRoleWithWebIdentity credentials unavailable: " << e.what());
            return {};
        }
    }

private:
    std::function<aws_sts_credentials()> _source;
};

/// `aws_client_config::credentials_provider` takes a chain, and the chain's
/// constructor and `AddProvider` are protected, hence a subclass.
class web_identity_provider_chain final : public Aws::Auth::AWSCredentialsProviderChain {
public:
    explicit web_identity_provider_chain(std::function<aws_sts_credentials()> source) {
        AddProvider(Aws::MakeShared<web_identity_credentials_provider>("kythira_web_identity",
                                                                       std::move(source)));
    }
};

/// The chain to put in `aws_client_config::credentials_provider`.
[[nodiscard]] inline auto make_web_identity_provider_chain(
    std::shared_ptr<aws_web_identity_source> source)
    -> std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> {
    return Aws::MakeShared<web_identity_provider_chain>(
        "kythira_web_identity", [source = std::move(source)] { return source->current(); });
}

}  // namespace kythira::cloud_credentials

#endif  // KYTHIRA_HAS_AWS_SDK
