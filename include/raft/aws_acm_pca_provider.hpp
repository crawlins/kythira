#pragma once

/// @file aws_acm_pca_provider.hpp
/// @brief `certificate_provider` implementation backed by AWS Certificate Manager
///        Private CA. Public config/class declaration; see
///        `aws_acm_pca_provider_impl.hpp` for the AWS SDK call implementations.
///
/// Follows the same structure as `aws_ec2_quorum_manager`: an `aws_client_config`
/// embedded for region/endpoint/credentials/timeout, `fiu_do_on()` fault points
/// around every AWS API call, and errors surfaced as rejected futures rather than
/// silently swallowed. This component does NOT create or delete a Private CA —
/// provisioning one is an out-of-band operator action, given its ongoing per-CA
/// cost.

#include <raft/aws_client_config.hpp>
#include <raft/certificate_provider.hpp>
#include <raft/fault_injection.hpp>

#ifdef KYTHIRA_HAS_AWS_ACM_PCA

#include <aws/acm-pca/ACMPCAClient.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace raft::testing {

/// Configuration for `aws_acm_pca_provider`.
struct aws_acm_pca_provider_config {
    /// AWS client settings (region, endpoint override, credentials, timeout).
    kythira::aws_client_config aws;
    /// ARN of a pre-existing ACM Private CA. Required.
    std::string certificate_authority_arn;
    /// ACM Private CA certificate template ARN applied to every issuance.
    std::string template_arn{"arn:aws:acm-pca:::template/EndEntityCertificate/V1"};
    /// Signing algorithm passed to `IssueCertificate`.
    std::string signing_algorithm{"SHA256WITHRSA"};
    /// Certificate validity period.
    std::chrono::seconds validity{std::chrono::hours(24 * 30)};
};

/// `certificate_provider` backed by `Aws::ACMPCA::ACMPCAClient`. See
/// `certificate_provider.hpp` for the concept this satisfies.
class aws_acm_pca_provider {
public:
    explicit aws_acm_pca_provider(aws_acm_pca_provider_config config);

    /// Calls `GetCertificateAuthorityCertificate`, caching the result after the
    /// first successful call.
    [[nodiscard]] auto root_certificate_pem() -> kythira::Future<std::string>;

    /// Calls `IssueCertificate` with the CSR bytes, then polls `GetCertificate`
    /// (bounded by `aws_client_config::api_timeout`, with backoff between polls)
    /// until the certificate is available or the timeout elapses.
    [[nodiscard]] auto sign_csr(std::string csr_pem, csr_signing_options options)
        -> kythira::Future<pem_material>;

    /// Calls `RevokeCertificate`. Requires the target CA to already have a
    /// CRL/OCSP configuration (an out-of-band operator setup); a call against a
    /// CA without one surfaces the resulting AWS error to the caller rather than
    /// falling back to any local behavior.
    [[nodiscard]] auto revoke(const std::string& certificate_serial) -> kythira::Future<void>;

private:
    Aws::ACMPCA::ACMPCAClient _client;
    aws_acm_pca_provider_config _config;
    std::mutex _mutex;
    std::optional<std::string> _cached_root_pem;
};

static_assert(certificate_provider<aws_acm_pca_provider>);

}  // namespace raft::testing

#endif  // KYTHIRA_HAS_AWS_ACM_PCA
