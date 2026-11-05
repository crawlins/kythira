// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Azure SDK credentials from a Kythira OIDC token (`.kiro/specs/iam-compatible-auth/`
// Requirement 2.2, task 4).
//
// Every Azure component in the tree authenticates through
// `azure_client_config::credential`, a
// `shared_ptr<Azure::Core::Credentials::TokenCredential>`: `azure_blob_client`,
// the Key Vault CA provider and the VM and VMSS quorum managers. So one
// adapter covers them all: a TokenCredential that asks an
// `azure_federated_source` for the token of whatever scope the caller names.
//
//     auto tokens = std::make_shared<cloud_credentials::kythira_oidc_token_source>(token_cfg);
//     auto entra = std::make_shared<cloud_credentials::azure_federated_source>(tokens, app_cfg);
//     azure_client_config azure;
//     azure.credential =
//         std::make_shared<cloud_credentials::azure_federated_token_credential>(entra);
//
// The source caches per scope and refreshes, so the SDK's call is a cache hit.
// A failed exchange with nothing valid cached throws
// `Azure::Core::Credentials::AuthenticationException`, the type the SDK's own
// credentials throw and its pipeline expects.

#ifdef KYTHIRA_HAS_AZURE_SDK

#include <raft/cloud_credential_exchange.hpp>

#include <azure/core/credentials/credentials.hpp>
#include <azure/core/datetime.hpp>

#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace kythira::cloud_credentials {

class azure_federated_token_credential final : public Azure::Core::Credentials::TokenCredential {
public:
    explicit azure_federated_token_credential(std::shared_ptr<azure_federated_source> source)
        : Azure::Core::Credentials::TokenCredential("KythiraFederatedCredential"),
          _source(std::move(source)) {}

    [[nodiscard]] auto GetToken(const Azure::Core::Credentials::TokenRequestContext& request,
                                const Azure::Core::Context& /*context*/) const
        -> Azure::Core::Credentials::AccessToken override {
        // Entra's v2.0 endpoint takes one resource per request; the SDK
        // passes exactly one scope in practice. More are joined as the
        // space-separated list the endpoint's grammar allows.
        std::string scope;
        for (const auto& s : request.Scopes) {
            if (!scope.empty()) {
                scope += ' ';
            }
            scope += s;
        }
        try {
            const auto t = scope.empty() ? _source->current() : _source->token(scope);
            Azure::Core::Credentials::AccessToken out;
            out.Token = t.access_token;
            out.ExpiresOn = Azure::DateTime(t.expires_at);
            return out;
        } catch (const std::exception& e) {
            throw Azure::Core::Credentials::AuthenticationException(
                std::string("Kythira federated credential: ") + e.what());
        }
    }

private:
    std::shared_ptr<azure_federated_source> _source;
};

}  // namespace kythira::cloud_credentials

#endif  // KYTHIRA_HAS_AZURE_SDK
