// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file azure_client_config.hpp
/// @brief Shared Azure connection settings for the VM/VMSS quorum manager
///        implementations and the Key Vault CA provider.

#include <chrono>
#include <string>

#ifdef KYTHIRA_HAS_AZURE_SDK
#include <azure/core/credentials/credentials.hpp>
#include <azure/identity/azure_cli_credential.hpp>
#include <azure/identity/chained_token_credential.hpp>
#include <azure/identity/environment_credential.hpp>

#include <memory>
#include <vector>
#endif

namespace kythira {

/// @brief Shared Azure connection settings consumed by both quorum manager
///        implementations and by `azure_key_vault_ca_provider`.
struct azure_client_config {
    /// Azure subscription ID (a GUID). Required by both quorum managers.
    std::string subscription_id;

    /// Resource group name that all managed VMs/VMSS/NICs live in. Required by
    /// both quorum managers.
    std::string resource_group;

    /// Azure region (e.g. "eastus"). Required by both quorum managers.
    std::string location;

    /// When non-empty, overrides the ARM endpoint (`https://management.azure.com`
    /// by default). Empty for `azure_key_vault_ca_provider`, which instead talks
    /// directly to `vault_url`.
    std::string arm_endpoint_override;

    /// Maximum time allowed for a single ARM/Key Vault API call.
    std::chrono::seconds api_timeout{30};

#ifdef KYTHIRA_HAS_AZURE_SDK
    /// Token credential used to authenticate every request. When `nullptr`,
    /// `make_default_credential_chain()` is used instead.
    std::shared_ptr<Azure::Core::Credentials::TokenCredential> credential;
#endif
};

#ifdef KYTHIRA_HAS_AZURE_SDK
/// @brief Builds Kythira's stand-in for the `DefaultAzureCredential` type other
///        Azure language SDKs ship but the Azure SDK for C++ does not.
///
/// Tries, in order: `EnvironmentCredential` (service principal via
/// `AZURE_CLIENT_ID`/`AZURE_CLIENT_SECRET`/`AZURE_TENANT_ID`), then
/// `AzureCliCredential` (`az login` session — convenient for local development
/// and what CI's `azure/login@v2` step leaves authenticated for GitHub Actions
/// via OIDC federation).
///
/// Deliberately does NOT include `ManagedIdentityCredential`, unlike most
/// `DefaultAzureCredential` implementations: azure-identity-cpp 1.13.2's
/// `ManagedIdentityCredential::GetToken()` segfaults (not throws) when the
/// IMDS endpoint is unreachable, which is the case in every environment this
/// project actually runs in — dev machines, containers, and GitHub Actions
/// runners are never Azure-hosted compute. A crash instead of a clean
/// fallthrough to the next credential in the chain defeats the entire point
/// of a fallback chain, so it's excluded from the default rather than worked
/// around. Callers that genuinely run on Azure-hosted compute and want
/// managed identity can still construct one explicitly and set it on
/// `azure_client_config::credential` — this function only backs the case
/// where that field is left null.
[[nodiscard]] inline auto make_default_credential_chain()
    -> std::shared_ptr<Azure::Core::Credentials::TokenCredential> {
    Azure::Identity::ChainedTokenCredential::Sources sources{
        std::make_shared<Azure::Identity::EnvironmentCredential>(),
        std::make_shared<Azure::Identity::AzureCliCredential>(),
    };
    return std::make_shared<Azure::Identity::ChainedTokenCredential>(std::move(sources));
}
#endif  // KYTHIRA_HAS_AZURE_SDK

}  // namespace kythira
