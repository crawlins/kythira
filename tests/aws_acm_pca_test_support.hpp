// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_acm_pca_test_support.hpp
/// @brief Helpers shared by the ACM Private CA LocalStack and real tests:
///        `ephemeral_acm_pca`, a root CA the test creates, bootstraps to
///        ACTIVE and deletes again (.kiro/specs/acm-pca-ephemeral-ca/), plus
///        OpenSSL helpers that build a CSR the provider's CSR policy accepts,
///        read a certificate's serial in the hex form `RevokeCertificate`
///        expects, and verify an issued chain against the CA's root.

#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>

#include <aws/acm-pca/ACMPCAClient.h>
#include <aws/acm-pca/ACMPCAErrors.h>
#include <aws/acm-pca/model/ASN1Subject.h>
#include <aws/acm-pca/model/CertificateAuthorityConfiguration.h>
#include <aws/acm-pca/model/CertificateAuthorityStatus.h>
#include <aws/acm-pca/model/CertificateAuthorityType.h>
#include <aws/acm-pca/model/CertificateAuthorityUsageMode.h>
#include <aws/acm-pca/model/CreateCertificateAuthorityRequest.h>
#include <aws/acm-pca/model/DeleteCertificateAuthorityRequest.h>
#include <aws/acm-pca/model/DescribeCertificateAuthorityRequest.h>
#include <aws/acm-pca/model/GetCertificateAuthorityCsrRequest.h>
#include <aws/acm-pca/model/GetCertificateRequest.h>
#include <aws/acm-pca/model/ImportCertificateAuthorityCertificateRequest.h>
#include <aws/acm-pca/model/IssueCertificateRequest.h>
#include <aws/acm-pca/model/KeyAlgorithm.h>
#include <aws/acm-pca/model/OcspConfiguration.h>
#include <aws/acm-pca/model/RevocationConfiguration.h>
#include <aws/acm-pca/model/SigningAlgorithm.h>
#include <aws/acm-pca/model/Tag.h>
#include <aws/acm-pca/model/UpdateCertificateAuthorityRequest.h>
#include <aws/acm-pca/model/Validity.h>
#include <aws/acm-pca/model/ValidityPeriodType.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/Array.h>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::testing::acm_pca {

/// A CSR plus the matching signing options, so `sign_csr`'s CSR policy check
/// passes. One-day validity keeps the request legal on a short-lived-mode CA
/// (which caps validity at seven days) as well as a general-purpose one.
struct leaf_request {
    raft::testing::csr_material csr;
    raft::testing::csr_signing_options options;
};

inline auto make_leaf_request(const std::string& common_name) -> leaf_request {
    raft::testing::leaf_certificate_options leaf;
    leaf.subject.common_name = common_name;
    leaf.dns_names = {common_name + ".internal"};
    leaf.server_auth = true;
    leaf.client_auth = true;

    leaf_request out;
    out.csr = raft::testing::generate_key_and_csr(leaf);
    out.options.dns_names = leaf.dns_names;
    out.options.server_auth = true;
    out.options.client_auth = true;
    out.options.validity = std::chrono::hours(24);
    return out;
}

using x509_ptr = std::unique_ptr<X509, decltype(&X509_free)>;

/// Every certificate in `pem`, in order. Throws when there is none.
inline auto read_certificates(const std::string& pem) -> std::vector<x509_ptr> {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free);
    std::vector<x509_ptr> certs;
    while (X509* cert = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)) {
        certs.emplace_back(cert, &X509_free);
    }
    if (certs.empty()) {
        throw std::runtime_error("no certificate in PEM input");
    }
    return certs;
}

/// The first certificate's serial as colon-separated lowercase hex
/// (`4c:6d:...`), the form `openssl x509 -text` prints and AWS documents for
/// `RevokeCertificate`'s `CertificateSerial`.
inline auto hex_serial(const std::string& certificate_pem) -> std::string {
    auto certs = read_certificates(certificate_pem);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> bn(
        ASN1_INTEGER_to_BN(X509_get0_serialNumber(certs.front().get()), nullptr), &BN_free);
    if (!bn) {
        throw std::runtime_error("unreadable certificate serial");
    }
    std::unique_ptr<char, void (*)(char*)> hex(BN_bn2hex(bn.get()),
                                               [](char* p) { OPENSSL_free(p); });
    std::string digits(hex.get());
    if (digits.size() % 2 != 0) {
        digits.insert(digits.begin(), '0');
    }
    std::string out;
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        if (!out.empty()) {
            out += ':';
        }
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(digits[i])));
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(digits[i + 1])));
    }
    return out;
}

/// True when the first certificate in `chain_pem` verifies up to `root_pem`,
/// with the rest of `chain_pem` as untrusted intermediates. Covers both a root
/// CA (the chain is just the root) and a subordinate one.
inline auto chain_verifies(const std::string& chain_pem, const std::string& root_pem) -> bool {
    auto chain = read_certificates(chain_pem);
    auto roots = read_certificates(root_pem);

    std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store(X509_STORE_new(),
                                                                  &X509_STORE_free);
    for (auto& root : roots) {
        X509_STORE_add_cert(store.get(), root.get());
    }
    std::unique_ptr<STACK_OF(X509), void (*)(STACK_OF(X509)*)> untrusted(
        sk_X509_new_null(), [](STACK_OF(X509) * s) { sk_X509_free(s); });
    for (std::size_t i = 1; i < chain.size(); ++i) {
        sk_X509_push(untrusted.get(), chain[i].get());
    }
    std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> ctx(X509_STORE_CTX_new(),
                                                                        &X509_STORE_CTX_free);
    if (X509_STORE_CTX_init(ctx.get(), store.get(), chain.front().get(), untrusted.get()) != 1) {
        return false;
    }
    return X509_verify_cert(ctx.get()) == 1;
}

// ── ephemeral_acm_pca (acm-pca-ephemeral-ca Requirements 1-3, 7) ───────────

/// Which revocation configuration a new CA gets. OCSP rather than a CRL: a
/// CRL needs an S3 bucket and bucket policy, OCSP needs nothing.
enum class ca_revocation {
    ocsp,
    none
};

/// Thrown when a CA cannot be created or bootstrapped (access denied, quota,
/// a step that never left REQUEST_IN_PROGRESS). Callers turn it into a skip:
/// it says the account or emulator cannot host the CA, not that the provider
/// under test is wrong.
struct ephemeral_ca_unavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// A root CA owned by one test run: created, bootstrapped to ACTIVE and
/// deleted again. It never touches a CA it did not create.
///
/// Construction makes no AWS call; `provision()` does. The split exists for
/// the signal path: a fixture holds the instance before any CA exists, so a
/// signal arriving mid-bootstrap can still reach `teardown()` through it. A
/// constructor that provisioned would leave nothing to call until it
/// returned, which is exactly the slow window a ctest TIMEOUT lands in.
class ephemeral_acm_pca {
public:
    struct options {
        Aws::Client::ClientConfiguration client;
        ca_revocation revocation{ca_revocation::ocsp};
        std::string common_name;
        /// Applied in CreateCertificateAuthority's own `Tags`, so the CA is
        /// never untagged, even briefly (Requirement 3.1).
        std::vector<std::pair<std::string, std::string>> tags;
        /// Bound on each REQUEST_IN_PROGRESS poll, and on the wait for ACTIVE.
        std::chrono::seconds step_timeout{120};
        /// SHORT_LIVED_CERTIFICATE: about an eighth of the general-purpose
        /// hourly rate, and leaves are capped at seven days, which the suites'
        /// one-day leaves are well inside.
        bool short_lived{true};
        int root_validity_days{365};
        /// Called with the ARN the moment CreateCertificateAuthority returns,
        /// before bootstrap continues (Requirement 2.5).
        std::function<void(const std::string&)> on_created;
    };

    explicit ephemeral_acm_pca(options opts) : _opts(std::move(opts)) {}

    ~ephemeral_acm_pca() { teardown(); }

    ephemeral_acm_pca(const ephemeral_acm_pca&) = delete;
    auto operator=(const ephemeral_acm_pca&) -> ephemeral_acm_pca& = delete;
    ephemeral_acm_pca(ephemeral_acm_pca&&) = delete;
    auto operator=(ephemeral_acm_pca&&) -> ephemeral_acm_pca& = delete;

    /// Creates the CA and bootstraps it to ACTIVE: create, fetch its CSR,
    /// self-issue the root with the RootCACertificate template, import it,
    /// wait for ACTIVE. Throws `ephemeral_ca_unavailable` naming the failing
    /// call. A CA created before the failure stays owned; `teardown()`
    /// deletes it.
    void provision() {
        namespace model = Aws::ACMPCA::Model;
        Aws::ACMPCA::ACMPCAClient client{_opts.client};

        model::ASN1Subject subject;
        subject.SetCommonName(_opts.common_name);
        model::CertificateAuthorityConfiguration config;
        config.SetKeyAlgorithm(model::KeyAlgorithm::RSA_2048);
        config.SetSigningAlgorithm(model::SigningAlgorithm::SHA256WITHRSA);
        config.SetSubject(subject);

        model::CreateCertificateAuthorityRequest create;
        create.SetCertificateAuthorityConfiguration(config);
        create.SetCertificateAuthorityType(model::CertificateAuthorityType::ROOT);
        if (_opts.short_lived) {
            create.SetUsageMode(model::CertificateAuthorityUsageMode::SHORT_LIVED_CERTIFICATE);
        }
        if (_opts.revocation == ca_revocation::ocsp) {
            model::OcspConfiguration ocsp;
            ocsp.SetEnabled(true);
            model::RevocationConfiguration revocation;
            revocation.SetOcspConfiguration(ocsp);
            create.SetRevocationConfiguration(revocation);
        }
        for (const auto& [key, value] : _opts.tags) {
            model::Tag tag;
            tag.SetKey(key);
            tag.SetValue(value);
            create.AddTags(tag);
        }
        auto created = client.CreateCertificateAuthority(create);
        require(created, "CreateCertificateAuthority");
        _arn = created.GetResult().GetCertificateAuthorityArn();
        _created_at = std::chrono::steady_clock::now();
        // Published after `_arn` is written, so a teardown on another thread
        // (the signal path) that sees the flag also sees the ARN.
        _created.store(true, std::memory_order_release);
        if (_opts.on_created) {
            _opts.on_created(_arn);
        }

        model::GetCertificateAuthorityCsrRequest csr_req;
        csr_req.SetCertificateAuthorityArn(_arn);
        auto csr = poll_until_ready([&] { return client.GetCertificateAuthorityCsr(csr_req); },
                                    "GetCertificateAuthorityCsr");

        model::IssueCertificateRequest issue;
        issue.SetCertificateAuthorityArn(_arn);
        const std::string& csr_pem = csr.GetResult().GetCsr();
        issue.SetCsr(to_buffer(csr_pem));
        issue.SetSigningAlgorithm(model::SigningAlgorithm::SHA256WITHRSA);
        issue.SetTemplateArn("arn:aws:acm-pca:::template/RootCACertificate/V1");
        model::Validity validity;
        validity.SetType(model::ValidityPeriodType::DAYS);
        validity.SetValue(_opts.root_validity_days);
        issue.SetValidity(validity);
        auto issued = client.IssueCertificate(issue);
        require(issued, "IssueCertificate (root)");
        _root_issued.store(true, std::memory_order_release);

        model::GetCertificateRequest get;
        get.SetCertificateAuthorityArn(_arn);
        get.SetCertificateArn(issued.GetResult().GetCertificateArn());
        auto root =
            poll_until_ready([&] { return client.GetCertificate(get); }, "GetCertificate (root)");

        model::ImportCertificateAuthorityCertificateRequest import;
        import.SetCertificateAuthorityArn(_arn);
        const std::string& root_pem = root.GetResult().GetCertificate();
        import.SetCertificate(to_buffer(root_pem));
        require(client.ImportCertificateAuthorityCertificate(import),
                "ImportCertificateAuthorityCertificate");

        wait_until_active(client);
    }

    /// Empty until `provision()` has created the CA.
    [[nodiscard]] auto arn() const -> const std::string& { return _arn; }

    [[nodiscard]] auto created() const -> bool { return _created.load(std::memory_order_acquire); }

    /// When CreateCertificateAuthority returned; the start of the CA's billing.
    [[nodiscard]] auto created_at() const -> std::chrono::steady_clock::time_point {
        return _created_at;
    }

    /// When DeleteCertificateAuthority returned, or empty if it has not (yet).
    [[nodiscard]] auto deleted_at() const -> std::optional<std::chrono::steady_clock::time_point> {
        return _deleted_at;
    }

    /// Whether the self-issued root certificate was issued, so billed.
    [[nodiscard]] auto root_issued() const -> bool {
        return _root_issued.load(std::memory_order_acquire);
    }

    /// Deletes the CA if this instance created it. Runs at most once, every
    /// step runs even when an earlier one fails, and nothing throws: errors go
    /// to stderr with the ARN and a line telling the operator to delete the
    /// CA by hand (Requirement 2.1-2.3).
    ///
    /// Status-aware, since AWS rejects deleting an ACTIVE CA and deletes a
    /// CREATING or PENDING_CERTIFICATE one immediately:
    ///   ACTIVE                          DISABLE, then DELETE(7 days)
    ///   DISABLED, EXPIRED, FAILED, ...  DELETE(7 days)
    ///   CREATING, PENDING_CERTIFICATE   DELETE (AWS ignores the window)
    ///   DELETED                         nothing
    ///   describe failed                 DISABLE and DELETE anyway
    void teardown() noexcept {
        if (!_created.load(std::memory_order_acquire)) {
            return;
        }
        if (_torn_down.test_and_set(std::memory_order_acq_rel)) {
            return;
        }
        namespace model = Aws::ACMPCA::Model;
        std::vector<std::string> errors;
        try {
            Aws::ACMPCA::ACMPCAClient client{_opts.client};

            std::optional<model::CertificateAuthorityStatus> status;
            model::DescribeCertificateAuthorityRequest describe;
            describe.SetCertificateAuthorityArn(_arn);
            auto described = client.DescribeCertificateAuthority(describe);
            if (described.IsSuccess()) {
                status = described.GetResult().GetCertificateAuthority().GetStatus();
            } else {
                errors.push_back("DescribeCertificateAuthority: " +
                                 std::string(described.GetError().GetMessage()));
            }

            if (status == model::CertificateAuthorityStatus::DELETED) {
                _deleted_at = std::chrono::steady_clock::now();
                return;
            }
            if (!status || status == model::CertificateAuthorityStatus::ACTIVE) {
                model::UpdateCertificateAuthorityRequest disable;
                disable.SetCertificateAuthorityArn(_arn);
                disable.SetStatus(model::CertificateAuthorityStatus::DISABLED);
                auto disabled = client.UpdateCertificateAuthority(disable);
                if (!disabled.IsSuccess()) {
                    errors.push_back("UpdateCertificateAuthority(DISABLED): " +
                                     std::string(disabled.GetError().GetMessage()));
                }
            }

            model::DeleteCertificateAuthorityRequest del;
            del.SetCertificateAuthorityArn(_arn);
            // The minimum restore window. A DELETED CA still counts against
            // the per-region CA quota until it ends, but is not billed.
            del.SetPermanentDeletionTimeInDays(7);
            auto deleted = client.DeleteCertificateAuthority(del);
            if (deleted.IsSuccess()) {
                _deleted_at = std::chrono::steady_clock::now();
            } else {
                errors.push_back("DeleteCertificateAuthority: " +
                                 std::string(deleted.GetError().GetMessage()));
            }
        } catch (const std::exception& ex) {
            errors.emplace_back(std::string("teardown threw: ") + ex.what());
        } catch (...) {
            errors.emplace_back("teardown threw a non-standard exception");
        }

        for (const auto& e : errors) {
            std::cerr << "[ephemeral_acm_pca] teardown " << _arn << ": " << e << "\n";
        }
        if (!_deleted_at) {
            std::cerr << "[ephemeral_acm_pca] CA " << _arn
                      << " may still exist and bill: disable and delete it manually, or run "
                         "scripts/aws-acm-pca-leaks.sh sweep\n";
        }
    }

private:
    options _opts;
    std::string _arn;
    std::chrono::steady_clock::time_point _created_at{};
    std::optional<std::chrono::steady_clock::time_point> _deleted_at;
    std::atomic<bool> _created{false};
    std::atomic<bool> _root_issued{false};
    /// Default-constructs clear since C++20.
    std::atomic_flag _torn_down{};

    static auto to_buffer(const std::string& s) -> Aws::Utils::ByteBuffer {
        return {reinterpret_cast<const unsigned char*>(s.data()), s.size()};
    }

    template<typename Outcome> static void require(const Outcome& outcome, const char* call) {
        if (!outcome.IsSuccess()) {
            throw ephemeral_ca_unavailable(std::string(call) + ": " +
                                           std::string(outcome.GetError().GetExceptionName()) +
                                           ": " + std::string(outcome.GetError().GetMessage()));
        }
    }

    /// Retries while ACM Private CA answers REQUEST_IN_PROGRESS, as it does
    /// right after CreateCertificateAuthority and IssueCertificate, backing
    /// off from 0.5 s to 5 s, for at most `step_timeout` (Requirement 1.3).
    template<typename Call> auto poll_until_ready(Call call, const char* name) -> decltype(call()) {
        const auto deadline = std::chrono::steady_clock::now() + _opts.step_timeout;
        auto delay = std::chrono::milliseconds(500);
        while (true) {
            auto outcome = call();
            if (outcome.IsSuccess()) {
                return outcome;
            }
            if (outcome.GetError().GetErrorType() !=
                    Aws::ACMPCA::ACMPCAErrors::REQUEST_IN_PROGRESS ||
                std::chrono::steady_clock::now() >= deadline) {
                require(outcome, name);
            }
            std::this_thread::sleep_for(delay);
            delay = std::min(delay * 2, std::chrono::milliseconds(5000));
        }
    }

    /// Import moves the CA to ACTIVE asynchronously; no case may run first.
    void wait_until_active(Aws::ACMPCA::ACMPCAClient& client) {
        namespace model = Aws::ACMPCA::Model;
        const auto deadline = std::chrono::steady_clock::now() + _opts.step_timeout;
        model::DescribeCertificateAuthorityRequest describe;
        describe.SetCertificateAuthorityArn(_arn);
        auto delay = std::chrono::milliseconds(500);
        while (true) {
            auto described = client.DescribeCertificateAuthority(describe);
            require(described, "DescribeCertificateAuthority");
            auto status = described.GetResult().GetCertificateAuthority().GetStatus();
            if (status == model::CertificateAuthorityStatus::ACTIVE) {
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw ephemeral_ca_unavailable(
                    "DescribeCertificateAuthority: not ACTIVE within " +
                    std::to_string(_opts.step_timeout.count()) + " s (status " +
                    std::string(model::CertificateAuthorityStatusMapper::
                                    GetNameForCertificateAuthorityStatus(status)) +
                    ")");
            }
            std::this_thread::sleep_for(delay);
            delay = std::min(delay * 2, std::chrono::milliseconds(5000));
        }
    }
};

}  // namespace kythira::testing::acm_pca
