// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_refreshing_credentials_provider.hpp
/// @brief Self-refreshing AWS credentials provider for long-running real-EC2
/// integration tests that authenticate via GitHub Actions OIDC.
///
/// `aws-actions/configure-aws-credentials` federates GitHub's OIDC identity
/// into AWS once, at job start, and exports the resulting temporary session
/// credentials as *static* environment variables (`AWS_ACCESS_KEY_ID` etc.).
/// Those credentials stop working the moment the assumed-role session ends —
/// one hour by default. A single real-EC2 test module provisions and tears
/// down VPCs, NAT gateways and EC2 fleets across ten cases and runs well past
/// that hour, so every case after the session lifetime failed with
/// "The security token included in the request is expired". The SDK's default
/// credential chain re-reads the same expired environment variables and never
/// recovers.
///
/// Rather than widening the role's maximum session duration — a blunt,
/// account-wide, security-relevant knob that only pushes the cliff back — this
/// provider re-federates on demand: whenever the live credentials are within
/// `kRefreshWindow` of expiry it mints a fresh OIDC token from the Actions
/// token endpoint (`ACTIONS_ID_TOKEN_REQUEST_URL` / `..._TOKEN`, injected into
/// every step granted `id-token: write`) and calls STS
/// `AssumeRoleWithWebIdentity` again. Because GitHub re-issues OIDC tokens for
/// the entire life of the job, the process holds valid credentials for as long
/// as it runs.

#ifdef KYTHIRA_HAS_AWS_SDK

#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/stream/ResponseStream.h>
#include <aws/sts/STSClient.h>
#include <aws/sts/model/AssumeRoleWithWebIdentityRequest.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace kythira::testing::aws_real_ec2 {

/// Credentials provider that federates GitHub Actions OIDC into AWS and
/// transparently re-federates before the temporary credentials expire.
///
/// Thread-safe: `GetAWSCredentials()` is called by the SDK once per signed
/// request (potentially concurrently), and the refresh path is serialised
/// under `mtx_`.
class github_oidc_refreshing_credentials_provider : public Aws::Auth::AWSCredentialsProvider {
public:
    github_oidc_refreshing_credentials_provider(Aws::String role_arn, Aws::String session_name,
                                                const Aws::String& region,
                                                Aws::String audience = "sts.amazonaws.com")
        : role_arn_(std::move(role_arn)),
          session_name_(std::move(session_name)),
          audience_(std::move(audience)) {
        Aws::Client::ClientConfiguration cfg;
        if (!region.empty()) {
            cfg.region = region;
        }
        http_client_ = Aws::Http::CreateHttpClient(cfg);
        // AssumeRoleWithWebIdentity is an unsigned STS action — the web-identity
        // token is the only credential it needs — so this internal STS client is
        // anonymous. Using the default chain here would be circular (it is the
        // very thing we are trying to supply).
        sts_ = Aws::MakeShared<Aws::STS::STSClient>(
            kAllocationTag,
            Aws::MakeShared<Aws::Auth::AnonymousAWSCredentialsProvider>(kAllocationTag), cfg);
    }

    auto GetAWSCredentials() -> Aws::Auth::AWSCredentials override {
        std::lock_guard<std::mutex> lk{mtx_};
        if (needs_refresh()) {
            // The SDK invokes this on the request-signing path and does not
            // expect it to throw. Surface a refresh failure on stderr and hand
            // back whatever we hold — the AWS call then fails with an ordinary
            // credentials error the caller already checks for, instead of an
            // exception escaping mid-sign.
            try {
                reload();
            } catch (const std::exception& e) {
                std::cerr << "[oidc-credentials] refresh failed: " << e.what() << "\n";
            }
        }
        return creds_;
    }

    /// Whether the GitHub Actions OIDC token endpoint is reachable from this
    /// process. False for a developer running the test locally (there the
    /// default credential chain, e.g. `~/.aws/credentials` or exported keys,
    /// is the right source and this provider must not be used).
    static auto environment_available() -> bool {
        return std::getenv("ACTIONS_ID_TOKEN_REQUEST_URL") != nullptr &&
               std::getenv("ACTIONS_ID_TOKEN_REQUEST_TOKEN") != nullptr;
    }

private:
    static constexpr const char* kAllocationTag = "kyt-oidc-credentials-provider";
    // Re-federate once the live credentials fall within this window of expiry.
    // Comfortably larger than a worst-case AWS API call so no request is ever
    // signed with credentials that expire mid-flight.
    static constexpr std::chrono::minutes kRefreshWindow{10};

    [[nodiscard]] auto needs_refresh() const -> bool {
        if (creds_.IsEmpty()) {
            return true;
        }
        const auto now_ms = Aws::Utils::DateTime::Now().Millis();
        const auto expiry_ms = creds_.GetExpiration().Millis();
        const auto window_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(kRefreshWindow).count();
        return expiry_ms - now_ms <= window_ms;
    }

    void reload() {
        const Aws::String token = fetch_github_oidc_token();

        Aws::STS::Model::AssumeRoleWithWebIdentityRequest req;
        req.SetRoleArn(role_arn_);
        req.SetRoleSessionName(session_name_);
        req.SetWebIdentityToken(token);

        auto out = sts_->AssumeRoleWithWebIdentity(req);
        if (!out.IsSuccess()) {
            throw std::runtime_error(
                "AssumeRoleWithWebIdentity failed while refreshing credentials: " +
                std::string(out.GetError().GetMessage()));
        }
        const auto& c = out.GetResult().GetCredentials();
        creds_ = Aws::Auth::AWSCredentials(c.GetAccessKeyId(), c.GetSecretAccessKey(),
                                           c.GetSessionToken(), c.GetExpiration());
    }

    [[nodiscard]] auto fetch_github_oidc_token() const -> Aws::String {
        const char* url_env = std::getenv("ACTIONS_ID_TOKEN_REQUEST_URL");
        const char* tok_env = std::getenv("ACTIONS_ID_TOKEN_REQUEST_TOKEN");
        if (url_env == nullptr || tok_env == nullptr) {
            throw std::runtime_error(
                "GitHub Actions OIDC token endpoint not available "
                "(ACTIONS_ID_TOKEN_REQUEST_URL / ACTIONS_ID_TOKEN_REQUEST_TOKEN unset)");
        }
        // The request URL already carries a query string; the audience is
        // appended per GitHub's documented token-exchange contract.
        const Aws::String url = Aws::String(url_env) + "&audience=" + audience_;
        auto request =
            Aws::Http::CreateHttpRequest(url, Aws::Http::HttpMethod::HTTP_GET,
                                         Aws::Utils::Stream::DefaultResponseStreamFactoryMethod);
        request->SetHeaderValue("authorization", Aws::String("Bearer ") + tok_env);
        request->SetHeaderValue("accept", "application/json");

        auto response = http_client_->MakeRequest(request);
        if (response == nullptr) {
            throw std::runtime_error("GitHub OIDC token request produced no response");
        }
        if (response->GetResponseCode() != Aws::Http::HttpResponseCode::OK) {
            throw std::runtime_error("GitHub OIDC token request failed with HTTP status " +
                                     std::to_string(static_cast<int>(response->GetResponseCode())));
        }
        Aws::Utils::Json::JsonValue json(response->GetResponseBody());
        if (!json.WasParseSuccessful()) {
            throw std::runtime_error("GitHub OIDC token response was not valid JSON");
        }
        const auto view = json.View();
        if (!view.KeyExists("value")) {
            throw std::runtime_error("GitHub OIDC token response did not contain a 'value' field");
        }
        return view.GetString("value");
    }

    Aws::String role_arn_;
    Aws::String session_name_;
    Aws::String audience_;
    std::shared_ptr<Aws::Http::HttpClient> http_client_;
    std::shared_ptr<Aws::STS::STSClient> sts_;
    mutable std::mutex mtx_;
    Aws::Auth::AWSCredentials creds_;
};

/// Wraps a single provider in an `AWSCredentialsProviderChain`, the type the
/// quorum-manager configuration accepts. `AddProvider` is protected on the
/// base, so a tiny public subclass is the sanctioned way to build a one-entry
/// chain.
class single_provider_chain : public Aws::Auth::AWSCredentialsProviderChain {
public:
    explicit single_provider_chain(
        const std::shared_ptr<Aws::Auth::AWSCredentialsProvider>& provider) {
        AddProvider(provider);
    }
};

}  // namespace kythira::testing::aws_real_ec2

#endif  // KYTHIRA_HAS_AWS_SDK
