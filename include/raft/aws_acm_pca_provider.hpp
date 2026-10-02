// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

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
#include <raft/future_default.hpp>

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
    /// RFC 5280 reason `revoke()` sends when the caller names none. One of the
    /// ACM Private CA `RevocationReason` names (`UNSPECIFIED`,
    /// `KEY_COMPROMISE`, `CERTIFICATE_AUTHORITY_COMPROMISE`,
    /// `AFFILIATION_CHANGED`, `SUPERSEDED`, `CESSATION_OF_OPERATION`,
    /// `PRIVILEGE_WITHDRAWN`, `A_A_COMPROMISE`); validated at construction.
    std::string revocation_reason{"UNSPECIFIED"};
};

/// `certificate_provider` backed by `Aws::ACMPCA::ACMPCAClient`. See
/// `certificate_provider.hpp` for the concept this satisfies.
class aws_acm_pca_provider {
public:
    explicit aws_acm_pca_provider(aws_acm_pca_provider_config config);

    /// Calls `GetCertificateAuthorityCertificate`, caching the result after the
    /// first successful call.
    [[nodiscard]] auto root_certificate_pem() -> kythira::future_default<std::string>;

    /// Calls `IssueCertificate` with the CSR bytes, then polls `GetCertificate`
    /// (bounded by `aws_client_config::api_timeout`, with backoff between polls)
    /// until the certificate is available or the timeout elapses.
    [[nodiscard]] auto sign_csr(std::string csr_pem, csr_signing_options options)
        -> kythira::future_default<pem_material>;

    /// Calls `DescribeCertificateAuthority` and reports whether the CA has a
    /// CRL or OCSP configuration enabled, which `RevokeCertificate` needs.
    /// Not cached: an operator can change the configuration at any time.
    [[nodiscard]] auto revocation_configured() -> kythira::future_default<bool>;

    /// Calls `RevokeCertificate` with `certificate_serial` (the hexadecimal
    /// serial, as `openssl x509 -serial` or `-text` prints it) and `reason`,
    /// or `aws_acm_pca_provider_config::revocation_reason` when `reason` is
    /// empty. An unknown reason rejects with `std::invalid_argument` before any
    /// AWS call. Requires the target CA to already have a CRL/OCSP
    /// configuration (an out-of-band operator setup); a call against a CA
    /// without one surfaces the resulting AWS error to the caller rather than
    /// falling back to any local behavior.
    [[nodiscard]] auto revoke(const std::string& certificate_serial, const std::string& reason = {})
        -> kythira::future_default<void>;

private:
    Aws::ACMPCA::ACMPCAClient _client;
    aws_acm_pca_provider_config _config;
    std::mutex _mutex;
    std::optional<std::string> _cached_root_pem;
};

static_assert(certificate_provider<aws_acm_pca_provider>);

}  // namespace raft::testing

#endif  // KYTHIRA_HAS_AWS_ACM_PCA
