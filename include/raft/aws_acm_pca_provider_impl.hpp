// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_acm_pca_provider_impl.hpp
/// @brief Method definitions for `aws_acm_pca_provider`. Included only by
///        `src/aws_acm_pca_provider.cpp`. Compiled only when
///        `KYTHIRA_HAS_AWS_ACM_PCA` is defined.

#include <raft/aws_acm_pca_provider.hpp>
#include <raft/future_default.hpp>

#ifdef KYTHIRA_HAS_AWS_ACM_PCA

#include <raft/csr_policy.hpp>

#include <aws/acm-pca/model/DescribeCertificateAuthorityRequest.h>
#include <aws/acm-pca/model/GetCertificateAuthorityCertificateRequest.h>
#include <aws/acm-pca/model/GetCertificateRequest.h>
#include <aws/acm-pca/model/IssueCertificateRequest.h>
#include <aws/acm-pca/model/RevocationReason.h>
#include <aws/acm-pca/model/RevokeCertificateRequest.h>
#include <aws/acm-pca/model/SigningAlgorithm.h>
#include <aws/acm-pca/model/Validity.h>
#include <aws/acm-pca/model/ValidityPeriodType.h>
#include <aws/core/utils/Array.h>

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace raft::testing {

namespace detail {

inline auto make_acm_pca_client(const kythira::aws_client_config& aws)
    -> Aws::ACMPCA::ACMPCAClient {
    Aws::Client::ClientConfiguration client_cfg;
    if (!aws.region.empty()) {
        client_cfg.region = aws.region;
    }
    if (!aws.endpoint_override.empty()) {
        client_cfg.endpointOverride = aws.endpoint_override;
    }
    auto ms = static_cast<long>(aws.api_timeout.count() * 1000);
    client_cfg.requestTimeoutMs = ms;
    client_cfg.connectTimeoutMs = ms;
    if (aws.credentials_provider) {
        return Aws::ACMPCA::ACMPCAClient(aws.credentials_provider, client_cfg);
    }
    return Aws::ACMPCA::ACMPCAClient(client_cfg);
}

inline auto signing_algorithm_from_string(const std::string& name)
    -> Aws::ACMPCA::Model::SigningAlgorithm {
    return Aws::ACMPCA::Model::SigningAlgorithmMapper::GetSigningAlgorithmForName(name);
}

/// Maps an ACM Private CA `RevocationReason` name to its enum value. The SDK's
/// own mapper can't validate: it keeps unknown names in an overflow container
/// and hands back a synthetic enum value that AWS then rejects, so the accepted
/// names are spelled out here.
inline auto revocation_reason_from_string(const std::string& name)
    -> Aws::ACMPCA::Model::RevocationReason {
    using Aws::ACMPCA::Model::RevocationReason;
    static constexpr std::array<std::pair<std::string_view, RevocationReason>, 8> k_reasons{{
        {"UNSPECIFIED", RevocationReason::UNSPECIFIED},
        {"KEY_COMPROMISE", RevocationReason::KEY_COMPROMISE},
        {"CERTIFICATE_AUTHORITY_COMPROMISE", RevocationReason::CERTIFICATE_AUTHORITY_COMPROMISE},
        {"AFFILIATION_CHANGED", RevocationReason::AFFILIATION_CHANGED},
        {"SUPERSEDED", RevocationReason::SUPERSEDED},
        {"CESSATION_OF_OPERATION", RevocationReason::CESSATION_OF_OPERATION},
        {"PRIVILEGE_WITHDRAWN", RevocationReason::PRIVILEGE_WITHDRAWN},
        {"A_A_COMPROMISE", RevocationReason::A_A_COMPROMISE},
    }};
    for (const auto& [reason_name, value] : k_reasons) {
        if (reason_name == name) {
            return value;
        }
    }
    throw std::invalid_argument("aws_acm_pca_provider: unknown revocation reason '" + name + "'");
}

}  // namespace detail

inline aws_acm_pca_provider::aws_acm_pca_provider(aws_acm_pca_provider_config config)
    : _client(detail::make_acm_pca_client(config.aws)), _config(std::move(config)) {
    if (_config.certificate_authority_arn.empty()) {
        throw std::invalid_argument(
            "aws_acm_pca_provider: certificate_authority_arn must be non-empty");
    }
    // Fail at startup, not on the first revocation.
    (void)detail::revocation_reason_from_string(_config.revocation_reason);
}

inline auto aws_acm_pca_provider::root_certificate_pem() -> kythira::future_default<std::string> {
    try {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_cached_root_pem) {
                return kythira::future_factory_default::makeReadyFuture(
                    std::string(*_cached_root_pem));
            }
        }

        fiu_do_on("raft/aws/acm_pca/get_certificate_authority_certificate",
                  throw std::runtime_error(
                      "fault: raft/aws/acm_pca/get_certificate_authority_certificate"););

        Aws::ACMPCA::Model::GetCertificateAuthorityCertificateRequest req;
        req.SetCertificateAuthorityArn(_config.certificate_authority_arn);

        auto outcome = _client.GetCertificateAuthorityCertificate(req);
        if (!outcome.IsSuccess()) {
            throw std::runtime_error("acm-pca GetCertificateAuthorityCertificate: " +
                                     std::string(outcome.GetError().GetMessage()));
        }

        std::string root_pem = outcome.GetResult().GetCertificate();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _cached_root_pem = root_pem;
        }
        return kythira::future_factory_default::makeReadyFuture(std::move(root_pem));
    } catch (const std::exception& ex) {
        return kythira::future_factory_default::makeExceptionalFuture<std::string>(
            std::make_exception_ptr(std::runtime_error(
                std::string("aws_acm_pca_provider::root_certificate_pem: ") + ex.what())));
    }
}

inline auto aws_acm_pca_provider::sign_csr(std::string csr_pem, csr_signing_options options)
    -> kythira::future_default<pem_material> {
    try {
        fiu_do_on("raft/aws/acm_pca/issue_certificate",
                  throw std::runtime_error("fault: raft/aws/acm_pca/issue_certificate"););

        // CSR-authoritative CA: it signs the names in the CSR, not
        // `options`, so refuse a CSR asking for anything unapproved.
        enforce_csr_matches_options(csr_pem, options);

        Aws::ACMPCA::Model::IssueCertificateRequest issue_req;
        issue_req.SetCertificateAuthorityArn(_config.certificate_authority_arn);
        issue_req.SetCsr(Aws::Utils::ByteBuffer(
            reinterpret_cast<const unsigned char*>(csr_pem.data()), csr_pem.size()));
        issue_req.SetTemplateArn(_config.template_arn);
        issue_req.SetSigningAlgorithm(
            detail::signing_algorithm_from_string(_config.signing_algorithm));

        auto validity_days =
            std::chrono::duration_cast<std::chrono::hours>(options.validity).count() / 24;
        Aws::ACMPCA::Model::Validity validity;
        validity.SetType(Aws::ACMPCA::Model::ValidityPeriodType::DAYS);
        validity.SetValue(validity_days > 0 ? validity_days : 1);
        issue_req.SetValidity(validity);

        auto issue_outcome = _client.IssueCertificate(issue_req);
        if (!issue_outcome.IsSuccess()) {
            throw std::runtime_error("acm-pca IssueCertificate: " +
                                     std::string(issue_outcome.GetError().GetMessage()));
        }
        std::string certificate_arn = issue_outcome.GetResult().GetCertificateArn();

        // ACM Private CA issuance is asynchronous: poll GetCertificate with
        // backoff, bounded by api_timeout total.
        Aws::ACMPCA::Model::GetCertificateRequest get_req;
        get_req.SetCertificateAuthorityArn(_config.certificate_authority_arn);
        get_req.SetCertificateArn(certificate_arn);

        auto deadline = std::chrono::steady_clock::now() + _config.aws.api_timeout;
        std::chrono::milliseconds backoff{200};
        while (true) {
            fiu_do_on("raft/aws/acm_pca/get_certificate",
                      throw std::runtime_error("fault: raft/aws/acm_pca/get_certificate"););

            auto get_outcome = _client.GetCertificate(get_req);
            if (get_outcome.IsSuccess()) {
                pem_material out;
                out.certificate_pem = get_outcome.GetResult().GetCertificate();
                out.chain_pem = out.certificate_pem + get_outcome.GetResult().GetCertificateChain();
                // serial is left 0 — ACM Private CA identifies certificates by ARN,
                // not the local monotonic-counter scheme certificate_authority uses.
                return kythira::future_factory_default::makeReadyFuture(std::move(out));
            }

            bool still_issuing = get_outcome.GetError().GetErrorType() ==
                                 Aws::ACMPCA::ACMPCAErrors::REQUEST_IN_PROGRESS;
            if (!still_issuing || std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("acm-pca GetCertificate: " +
                                         std::string(get_outcome.GetError().GetMessage()));
            }

            std::this_thread::sleep_for(backoff);
            backoff = std::min(backoff * 2, std::chrono::milliseconds(5000));
        }
    } catch (const std::invalid_argument&) {
        // A rejected request (e.g. by enforce_csr_matches_options) stays an
        // invalid_argument so HTTP callers answer 400, not 502.
        return kythira::future_factory_default::makeExceptionalFuture<pem_material>(
            std::current_exception());
    } catch (const std::exception& ex) {
        return kythira::future_factory_default::makeExceptionalFuture<pem_material>(
            std::make_exception_ptr(
                std::runtime_error(std::string("aws_acm_pca_provider::sign_csr: ") + ex.what())));
    }
}

inline auto aws_acm_pca_provider::revocation_configured() -> kythira::future_default<bool> {
    try {
        fiu_do_on(
            "raft/aws/acm_pca/describe_certificate_authority",
            throw std::runtime_error("fault: raft/aws/acm_pca/describe_certificate_authority"););

        Aws::ACMPCA::Model::DescribeCertificateAuthorityRequest req;
        req.SetCertificateAuthorityArn(_config.certificate_authority_arn);

        auto outcome = _client.DescribeCertificateAuthority(req);
        if (!outcome.IsSuccess()) {
            throw std::runtime_error("acm-pca DescribeCertificateAuthority: " +
                                     std::string(outcome.GetError().GetMessage()));
        }
        const auto& revocation =
            outcome.GetResult().GetCertificateAuthority().GetRevocationConfiguration();
        bool configured = revocation.GetCrlConfiguration().GetEnabled() ||
                          revocation.GetOcspConfiguration().GetEnabled();
        return kythira::future_factory_default::makeReadyFuture(configured);
    } catch (const std::exception& ex) {
        return kythira::future_factory_default::makeExceptionalFuture<bool>(
            std::make_exception_ptr(std::runtime_error(
                std::string("aws_acm_pca_provider::revocation_configured: ") + ex.what())));
    }
}

inline auto aws_acm_pca_provider::revoke(const std::string& certificate_serial,
                                         const std::string& reason)
    -> kythira::future_default<void> {
    try {
        // AWS rejects a RevokeCertificate without a RevocationReason, so one is
        // always sent; an unknown name is the caller's error, not AWS's.
        auto revocation_reason = detail::revocation_reason_from_string(
            reason.empty() ? _config.revocation_reason : reason);

        fiu_do_on("raft/aws/acm_pca/revoke_certificate",
                  throw std::runtime_error("fault: raft/aws/acm_pca/revoke_certificate"););

        Aws::ACMPCA::Model::RevokeCertificateRequest req;
        req.SetCertificateAuthorityArn(_config.certificate_authority_arn);
        req.SetCertificateSerial(certificate_serial);
        req.SetRevocationReason(revocation_reason);

        auto outcome = _client.RevokeCertificate(req);
        if (!outcome.IsSuccess()) {
            throw std::runtime_error("acm-pca RevokeCertificate: " +
                                     std::string(outcome.GetError().GetMessage()));
        }
        return kythira::future_factory_default::makeFuture();
    } catch (const std::invalid_argument&) {
        // Stays an invalid_argument so HTTP callers answer 400, not 502.
        return kythira::future_factory_default::makeExceptionalFuture<void>(
            std::current_exception());
    } catch (const std::exception& ex) {
        return kythira::future_factory_default::makeExceptionalFuture<void>(std::make_exception_ptr(
            std::runtime_error(std::string("aws_acm_pca_provider::revoke: ") + ex.what())));
    }
}

}  // namespace raft::testing

#endif  // KYTHIRA_HAS_AWS_ACM_PCA
