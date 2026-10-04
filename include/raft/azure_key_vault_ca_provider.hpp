// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file azure_key_vault_ca_provider.hpp
/// @brief `certificate_provider` implementation backed by Azure Key Vault Keys.
///        Public config/class declaration; see
///        `azure_key_vault_ca_provider_impl.hpp` for the Azure SDK call
///        implementations.
///
/// Follows the same public-declaration/impl-detail split as
/// `aws_acm_pca_provider.hpp`/`aws_acm_pca_provider_impl.hpp`. Unlike ACM
/// Private CA (which owns the whole CA certificate chain internally),
/// `azure_key_vault_ca_provider` holds the CA's PUBLIC certificate locally
/// (`config.ca_certificate_pem`) and parses/assembles the leaf certificate's
/// TBSCertificate itself; only the final signature comes from Key Vault. The
/// vault's private key material never leaves Key Vault.

#include <raft/azure_client_config.hpp>
#include <raft/certificate_provider.hpp>
#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>

#ifdef KYTHIRA_HAS_AZURE_KEY_VAULT

#include <azure/keyvault/keys/cryptography/cryptography_client.hpp>

#include <chrono>
#include <cstdint>
#include <string>

namespace raft::testing {

/// Signing algorithm requested from Key Vault's `Sign` operation, and written
/// into the issued certificate's signature AlgorithmIdentifier. The digest
/// hash algorithm is implied: SHA-256 for rs256/ps256/es256, SHA-384 for
/// rs384/es384, SHA-512 for rs512. The rs* and ps256 algorithms need an RSA
/// CA key; es256 needs a P-256 key and es384 a P-384 key (Key Vault ties each
/// ES algorithm to one curve), and `sign_csr` refuses a mismatch. ps256 uses
/// MGF1-SHA-256 and a 32-byte salt, as Key Vault does (RFC 7518 section 3.5).
enum class azure_key_vault_signing_algorithm : std::uint8_t {
    rs256,
    rs384,
    rs512,
    ps256,
    es256,
    es384,
};

/// Configuration for `azure_key_vault_ca_provider`.
struct azure_key_vault_ca_provider_config {
    /// Azure subscription/resource-group/location/credential settings. Only
    /// `azure.credential` (and `azure.api_timeout`) are consulted; Key Vault
    /// operations don't need a subscription/resource-group/location.
    kythira::azure_client_config azure{};
    /// Key Vault URL, e.g. "https://my-vault.vault.azure.net". Required.
    std::string vault_url;
    /// Name of the Key Vault key used for signing. Required.
    std::string key_name;
    /// Specific key version. Empty = latest.
    std::string key_version;
    /// PEM-encoded CA certificate whose private key lives in Key Vault.
    /// Required. Returned verbatim by `root_certificate_pem()`.
    std::string ca_certificate_pem;
    /// Algorithm passed to Key Vault's `Sign` operation.
    azure_key_vault_signing_algorithm signing_algorithm{azure_key_vault_signing_algorithm::rs256};
    /// Leaf certificate validity period.
    std::chrono::seconds validity{std::chrono::hours(24 * 30)};
    /// Options passed through to the underlying `CryptographyClient`. Exists
    /// primarily so unit tests can insert a stub `HttpPolicy` into
    /// `PerRetryPolicies` ahead of the transport policy (the same technique
    /// `stub_http_transport_policy` uses for the quorum managers' unit tests)
    /// and exercise `sign_csr` without live Key Vault credentials or network
    /// access. Ordinary callers can leave this at its default.
    Azure::Security::KeyVault::Keys::Cryptography::CryptographyClientOptions client_options{};
};

/// `certificate_provider` backed by
/// `Azure::Security::KeyVault::Keys::Cryptography::CryptographyClient::Sign`.
/// See `certificate_provider.hpp` for the concept this satisfies. Does NOT
/// implement `revoke()` — this component does not create or manage a CRL/OCSP
/// responder, the same scope carve-out `aws_acm_pca_provider` documents for
/// CAs without a configured revocation mechanism.
class azure_key_vault_ca_provider {
public:
    explicit azure_key_vault_ca_provider(azure_key_vault_ca_provider_config config);

    /// Returns `config.ca_certificate_pem` wrapped in an immediately-resolved
    /// Future. No network call — the CA certificate is public and supplied at
    /// construction time, unlike ACM Private CA where it must be fetched.
    [[nodiscard]] auto root_certificate_pem() -> kythira::future_default<std::string>;

    /// Parses and validates `csr_pem` locally, builds the leaf TBSCertificate,
    /// hashes it per `config.signing_algorithm`, calls Key Vault's `Sign`
    /// operation for the raw signature, and assembles the final certificate.
    /// The requester's private key is never involved (CSR contains only the
    /// public key); the CA's private key never leaves Key Vault.
    [[nodiscard]] auto sign_csr(std::string csr_pem, csr_signing_options options)
        -> kythira::future_default<pem_material>;

private:
    Azure::Security::KeyVault::Keys::Cryptography::CryptographyClient _client;
    azure_key_vault_ca_provider_config _config;
};

static_assert(certificate_provider<azure_key_vault_ca_provider>);

}  // namespace raft::testing

#endif  // KYTHIRA_HAS_AZURE_KEY_VAULT
