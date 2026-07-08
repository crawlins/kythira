#pragma once

/// @file aws_acm_pca_provider_impl.hpp
/// @brief Method definitions for `aws_acm_pca_provider`. Included only by
///        `src/aws_acm_pca_provider.cpp`. Compiled only when
///        `KYTHIRA_HAS_AWS_ACM_PCA` is defined.

#include <raft/aws_acm_pca_provider.hpp>

#ifdef KYTHIRA_HAS_AWS_ACM_PCA

#include <aws/acm-pca/model/GetCertificateAuthorityCertificateRequest.h>
#include <aws/acm-pca/model/GetCertificateRequest.h>
#include <aws/acm-pca/model/IssueCertificateRequest.h>
#include <aws/acm-pca/model/RevokeCertificateRequest.h>
#include <aws/acm-pca/model/SigningAlgorithm.h>
#include <aws/acm-pca/model/Validity.h>
#include <aws/acm-pca/model/ValidityPeriodType.h>
#include <aws/core/utils/Array.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

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

}  // namespace detail

inline aws_acm_pca_provider::aws_acm_pca_provider(aws_acm_pca_provider_config config)
    : _client(detail::make_acm_pca_client(config.aws)), _config(std::move(config)) {
    if (_config.certificate_authority_arn.empty()) {
        throw std::invalid_argument(
            "aws_acm_pca_provider: certificate_authority_arn must be non-empty");
    }
}

inline auto aws_acm_pca_provider::root_certificate_pem() -> kythira::Future<std::string> {
    try {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_cached_root_pem) {
                return kythira::FutureFactory::makeReadyFuture(std::string(*_cached_root_pem));
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
        return kythira::FutureFactory::makeReadyFuture(std::move(root_pem));
    } catch (const std::exception& ex) {
        return kythira::FutureFactory::makeExceptionalFuture<std::string>(std::runtime_error(
            std::string("aws_acm_pca_provider::root_certificate_pem: ") + ex.what()));
    }
}

inline auto aws_acm_pca_provider::sign_csr(std::string csr_pem, csr_signing_options options)
    -> kythira::Future<pem_material> {
    try {
        fiu_do_on("raft/aws/acm_pca/issue_certificate",
                  throw std::runtime_error("fault: raft/aws/acm_pca/issue_certificate"););

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
                return kythira::FutureFactory::makeReadyFuture(std::move(out));
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
    } catch (const std::exception& ex) {
        return kythira::FutureFactory::makeExceptionalFuture<pem_material>(
            std::runtime_error(std::string("aws_acm_pca_provider::sign_csr: ") + ex.what()));
    }
}

inline auto aws_acm_pca_provider::revoke(const std::string& certificate_serial)
    -> kythira::Future<void> {
    try {
        fiu_do_on("raft/aws/acm_pca/revoke_certificate",
                  throw std::runtime_error("fault: raft/aws/acm_pca/revoke_certificate"););

        Aws::ACMPCA::Model::RevokeCertificateRequest req;
        req.SetCertificateAuthorityArn(_config.certificate_authority_arn);
        req.SetCertificateSerial(certificate_serial);

        auto outcome = _client.RevokeCertificate(req);
        if (!outcome.IsSuccess()) {
            throw std::runtime_error("acm-pca RevokeCertificate: " +
                                     std::string(outcome.GetError().GetMessage()));
        }
        return kythira::FutureFactory::makeFuture();
    } catch (const std::exception& ex) {
        return kythira::FutureFactory::makeExceptionalFuture<void>(
            std::runtime_error(std::string("aws_acm_pca_provider::revoke: ") + ex.what()));
    }
}

}  // namespace raft::testing

#endif  // KYTHIRA_HAS_AWS_ACM_PCA
