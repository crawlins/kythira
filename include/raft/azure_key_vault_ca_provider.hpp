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

/// Signing algorithm requested from Key Vault's `Sign` operation. The digest
/// hash algorithm is implied: SHA-256 for rs256/ps256/es256, SHA-384 for
/// rs384/es384, SHA-512 for rs512.
///
/// Local assembly of the final DER certificate (parsing the CSR, building the
/// TBSCertificate, and splicing in the externally-computed signature without
/// ever holding the CA private key) is fully implemented for the RSA
/// PKCS#1v1.5 family (rs256/rs384/rs512) via a custom `RSA_METHOD` whose
/// `sign` callback delegates to Key Vault. `ps256` (RSA-PSS) and `es256`/
/// `es384` (ECDSA) are accepted here and forwarded to Key Vault's `Sign` call
/// correctly, but the local certificate-assembly path for those three algorithm
/// families is not yet implemented (`sign_csr` throws `std::logic_error` for
/// them) — a scoped, documented follow-up, not a silent gap. See
/// design.md's "Sharing the X.509-building code path" section for why rs256
/// was prioritized (it is `azure_key_vault_ca_provider_config::signing_algorithm`'s
/// default).
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
