#pragma once

/// @file gcp_privateca_certificate_provider.hpp
/// @brief `certificate_provider` implementation backed by Google Cloud
///        Certificate Authority Service (CAS). Public config/class declaration;
///        see `gcp_privateca_certificate_provider_impl.hpp` for the
///        `google-cloud-cpp` call implementations.
///
/// Structural analogue of `aws_acm_pca_provider`: a `gcp_client_config`
/// embedded for project/credentials/endpoint/timeout, `fiu_do_on()` fault
/// points around every API call, errors surfaced as rejected futures. Does NOT
/// create or delete a CA pool or CA — provisioning one is an out-of-band
/// operator action, the same scope decision as `aws_acm_pca_provider`.

#include <raft/certificate_provider.hpp>
#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>
#include <raft/gcp_client_config.hpp>

#ifdef KYTHIRA_HAS_GCP_PRIVATECA

#include <google/cloud/privateca/v1/certificate_authority_client.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace raft::testing {

/// Configuration for `gcp_privateca_certificate_provider`.
struct gcp_privateca_certificate_provider_config {
    /// GCP client settings (project, credentials, endpoint override, timeout).
    kythira::gcp_client_config gcp;
    /// CAS location (region), e.g. `"us-central1"`. Required.
    std::string location;
    /// Pre-existing CA pool ID. Required.
    std::string ca_pool_id;
    /// Specific CA within the pool to pin issuance to; empty = let CAS choose
    /// within the pool per its own selection policy.
    std::string certificate_authority_id{};
    /// CAS certificate-template resource name; empty = no template applied.
    std::string certificate_template{};
    /// Requested certificate lifetime. Default: 30 days.
    std::chrono::seconds validity{std::chrono::hours(24 * 30)};
};

/// `certificate_provider` backed by
/// `google::cloud::privateca_v1::CertificateAuthorityServiceClient`. See
/// `certificate_provider.hpp` for the concept this satisfies.
class gcp_privateca_certificate_provider {
public:
    explicit gcp_privateca_certificate_provider(gcp_privateca_certificate_provider_config config);

    /// Calls `GetCertificateAuthority` (or `ListCertificateAuthorities` and
    /// picks the pool's first `ENABLED` CA when `certificate_authority_id` is
    /// empty), caching the result after the first successful call.
    [[nodiscard]] auto root_certificate_pem() -> kythira::future_default<std::string>;

    /// Calls `CreateCertificate` against the configured CA pool, template, and
    /// validity. Unlike ACM Private CA, CAS issuance is synchronous — the issued
    /// certificate is read directly from the `CreateCertificate` response, with
    /// no `GetCertificate` poll loop.
    [[nodiscard]] auto sign_csr(std::string csr_pem, csr_signing_options options)
        -> kythira::future_default<pem_material>;

    /// Optional: calls `RevokeCertificate`. Mirrors `aws_acm_pca_provider`'s
    /// optional `revoke`.
    [[nodiscard]] auto revoke(std::string serial) -> kythira::future_default<void>;

private:
    gcp_privateca_certificate_provider_config _config;
    google::cloud::privateca_v1::CertificateAuthorityServiceClient _client;
    std::mutex _root_cache_mutex;
    std::optional<std::string> _root_cache;
};

static_assert(certificate_provider<gcp_privateca_certificate_provider>);

}  // namespace raft::testing

#endif  // KYTHIRA_HAS_GCP_PRIVATECA
