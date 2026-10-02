// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE aws_acm_pca_provider_localstack_test
#include <boost/test/unit_test.hpp>

// LocalStack integration test for aws_acm_pca_provider (certificate-authority
// task 11). Drives the provider's real SDK calls end to end, which the unit
// test cannot: the IssueCertificate → GetCertificate poll, the cached root,
// DescribeCertificateAuthority's revocation check and RevokeCertificate with a
// RevocationReason.
//
// ACM Private CA emulation is a LocalStack Pro feature. The suite exits 77
// (ctest "Not Run") when LocalStack is unreachable, when its acm-pca service
// is unusable, or when it cannot stand up the two root CAs the cases need, so
// community-edition LocalStack never fails the run. Failures in the provider
// calls themselves, once both CAs exist, are real failures.
//
// The fixture, not the provider, creates the CAs: Requirement 10.2 keeps CA
// provisioning out of aws_acm_pca_provider.

#ifdef KYTHIRA_HAS_AWS_ACM_PCA

#include <raft/aws_acm_pca_provider.hpp>
#include <raft/aws_acm_pca_provider_impl.hpp>

#include "aws_acm_pca_test_support.hpp"

#include <aws/acm-pca/ACMPCAClient.h>
#include <aws/acm-pca/model/ASN1Subject.h>
#include <aws/acm-pca/model/CertificateAuthorityConfiguration.h>
#include <aws/acm-pca/model/CertificateAuthorityStatus.h>
#include <aws/acm-pca/model/CertificateAuthorityType.h>
#include <aws/acm-pca/model/CreateCertificateAuthorityRequest.h>
#include <aws/acm-pca/model/DeleteCertificateAuthorityRequest.h>
#include <aws/acm-pca/model/DescribeCertificateAuthorityRequest.h>
#include <aws/acm-pca/model/GetCertificateAuthorityCsrRequest.h>
#include <aws/acm-pca/model/GetCertificateRequest.h>
#include <aws/acm-pca/model/ImportCertificateAuthorityCertificateRequest.h>
#include <aws/acm-pca/model/IssueCertificateRequest.h>
#include <aws/acm-pca/model/KeyAlgorithm.h>
#include <aws/acm-pca/model/ListCertificateAuthoritiesRequest.h>
#include <aws/acm-pca/model/OcspConfiguration.h>
#include <aws/acm-pca/model/RevocationConfiguration.h>
#include <aws/acm-pca/model/SigningAlgorithm.h>
#include <aws/acm-pca/model/UpdateCertificateAuthorityRequest.h>
#include <aws/acm-pca/model/Validity.h>
#include <aws/acm-pca/model/ValidityPeriodType.h>
#include <aws/core/Aws.h>
#include <aws/core/utils/Array.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace raft::testing;
namespace acm_pca = kythira::testing::acm_pca;
namespace model = Aws::ACMPCA::Model;

namespace {

constexpr const char* LOCALSTACK_ENDPOINT = "http://localhost:4566";
constexpr const char* LOCALSTACK_HOST = "localhost";
constexpr const char* LOCALSTACK_PORT = "4566";
constexpr const char* DUMMY_REGION = "us-east-1";

// AWS-SDK-free TCP probe, run before Aws::InitAPI(); same as
// aws_quorum_manager_localstack_test.cpp.
bool localstack_reachable(std::chrono::milliseconds timeout) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(LOCALSTACK_HOST, LOCALSTACK_PORT, &hints, &res) != 0) {
        return false;
    }
    bool connected = false;
    for (addrinfo* p = res; p != nullptr && !connected; p = p->ai_next) {
        int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, p->ai_addr, p->ai_addrlen);
        if (rc == 0) {
            connected = true;
        } else if (errno == EINPROGRESS) {
            pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
            if (poll(&pfd, 1, static_cast<int>(timeout.count())) > 0) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
                    connected = true;
                }
            }
        }
        close(fd);
    }
    freeaddrinfo(res);
    return connected;
}

struct PreflightSkipFixture {
    PreflightSkipFixture() {
        if (!localstack_reachable(std::chrono::milliseconds{2000})) {
            std::cerr << "SKIP: LocalStack not reachable at " << LOCALSTACK_ENDPOINT << "\n";
            std::exit(77);
        }
        // LocalStack accepts any credentials; supply dummy ones so the default
        // chain doesn't wander off to IMDS. Never overrides a caller's.
        ::setenv("AWS_ACCESS_KEY_ID", "test", 0);
        ::setenv("AWS_SECRET_ACCESS_KEY", "test", 0);
    }
};

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        folly::init(&argc, &argv, false);
    }
};
#endif

auto make_client() -> Aws::ACMPCA::ACMPCAClient {
    Aws::Client::ClientConfiguration c;
    c.region = DUMMY_REGION;
    c.endpointOverride = LOCALSTACK_ENDPOINT;
    c.requestTimeoutMs = 10000;
    c.connectTimeoutMs = 10000;
    return Aws::ACMPCA::ACMPCAClient{c};
}

auto to_buffer(const std::string& s) -> Aws::Utils::ByteBuffer {
    return Aws::Utils::ByteBuffer(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

/// Thrown by CA provisioning; turns into a skip, not a failure, because it
/// means this LocalStack can't emulate the setup rather than that the provider
/// is wrong.
struct setup_unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template<typename Outcome> void require_setup(const Outcome& outcome, const char* call) {
    if (!outcome.IsSuccess()) {
        throw setup_unsupported(std::string(call) + ": " + outcome.GetError().GetMessage());
    }
}

/// Two ACTIVE self-signed root CAs: one with OCSP enabled (revocation works)
/// and one with no revocation configuration (Requirement 10.7's error path).
/// Torn down in the destructor; Boost destroys global fixtures in reverse, so
/// this outlives every case.
struct LocalStackCaFixture {
    std::string ocsp_ca_arn;
    std::string bare_ca_arn;

    LocalStackCaFixture() {
        Aws::InitAPI(sdk_options);
        // Same placement as aws_quorum_manager_localstack_test.cpp: the probe
        // runs right after InitAPI in this constructor, not in a separate
        // global fixture, so the ordering can't go wrong.
        auto client = make_client();
        auto listed = client.ListCertificateAuthorities(model::ListCertificateAuthoritiesRequest{});
        if (!listed.IsSuccess()) {
            std::cerr << "SKIP: the acm-pca service is unusable at " << LOCALSTACK_ENDPOINT
                      << " (ACM Private CA emulation is LocalStack Pro only): "
                      << listed.GetError().GetMessage() << "\n";
            Aws::ShutdownAPI(sdk_options);
            std::exit(77);
        }
        try {
            ocsp_ca_arn = create_active_root_ca(client, /*ocsp=*/true);
            bare_ca_arn = create_active_root_ca(client, /*ocsp=*/false);
        } catch (const setup_unsupported& ex) {
            std::cerr << "SKIP: LocalStack could not provision an ACTIVE root CA: " << ex.what()
                      << "\n";
            teardown(client);
            Aws::ShutdownAPI(sdk_options);
            std::exit(77);
        }
    }

    ~LocalStackCaFixture() {
        auto client = make_client();
        teardown(client);
        Aws::ShutdownAPI(sdk_options);
    }

    LocalStackCaFixture(const LocalStackCaFixture&) = delete;
    auto operator=(const LocalStackCaFixture&) -> LocalStackCaFixture& = delete;

private:
    Aws::SDKOptions sdk_options;
    std::vector<std::string> created;

    /// The real-AWS root CA bootstrap: create, sign the CA's own CSR with the
    /// RootCACertificate template, import the result.
    auto create_active_root_ca(Aws::ACMPCA::ACMPCAClient& client, bool ocsp) -> std::string {
        model::ASN1Subject subject;
        subject.SetCommonName(ocsp ? "kythira localstack root (ocsp)"
                                   : "kythira localstack root (no revocation)");
        model::CertificateAuthorityConfiguration config;
        config.SetKeyAlgorithm(model::KeyAlgorithm::RSA_2048);
        config.SetSigningAlgorithm(model::SigningAlgorithm::SHA256WITHRSA);
        config.SetSubject(subject);

        model::CreateCertificateAuthorityRequest create;
        create.SetCertificateAuthorityConfiguration(config);
        create.SetCertificateAuthorityType(model::CertificateAuthorityType::ROOT);
        if (ocsp) {
            // OCSP rather than a CRL: a CRL needs an S3 bucket, OCSP nothing.
            model::OcspConfiguration ocsp_config;
            ocsp_config.SetEnabled(true);
            model::RevocationConfiguration revocation;
            revocation.SetOcspConfiguration(ocsp_config);
            create.SetRevocationConfiguration(revocation);
        }
        auto created_ca = client.CreateCertificateAuthority(create);
        require_setup(created_ca, "CreateCertificateAuthority");
        std::string arn = created_ca.GetResult().GetCertificateAuthorityArn();
        created.push_back(arn);

        model::GetCertificateAuthorityCsrRequest csr_req;
        csr_req.SetCertificateAuthorityArn(arn);
        auto csr = poll_until_ready([&] { return client.GetCertificateAuthorityCsr(csr_req); },
                                    "GetCertificateAuthorityCsr");

        model::IssueCertificateRequest issue;
        issue.SetCertificateAuthorityArn(arn);
        issue.SetCsr(to_buffer(csr.GetResult().GetCsr()));
        issue.SetSigningAlgorithm(model::SigningAlgorithm::SHA256WITHRSA);
        issue.SetTemplateArn("arn:aws:acm-pca:::template/RootCACertificate/V1");
        model::Validity validity;
        validity.SetType(model::ValidityPeriodType::DAYS);
        validity.SetValue(30);
        issue.SetValidity(validity);
        auto issued = client.IssueCertificate(issue);
        require_setup(issued, "IssueCertificate (root)");

        model::GetCertificateRequest get;
        get.SetCertificateAuthorityArn(arn);
        get.SetCertificateArn(issued.GetResult().GetCertificateArn());
        auto root =
            poll_until_ready([&] { return client.GetCertificate(get); }, "GetCertificate (root)");

        model::ImportCertificateAuthorityCertificateRequest import;
        import.SetCertificateAuthorityArn(arn);
        import.SetCertificate(to_buffer(root.GetResult().GetCertificate()));
        require_setup(client.ImportCertificateAuthorityCertificate(import),
                      "ImportCertificateAuthorityCertificate");
        return arn;
    }

    /// Retries while ACM Private CA answers REQUEST_IN_PROGRESS, as it does
    /// right after CreateCertificateAuthority and IssueCertificate.
    template<typename Call>
    static auto poll_until_ready(Call call, const char* name) -> decltype(call()) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (true) {
            auto outcome = call();
            if (outcome.IsSuccess()) {
                return outcome;
            }
            if (outcome.GetError().GetErrorType() !=
                    Aws::ACMPCA::ACMPCAErrors::REQUEST_IN_PROGRESS ||
                std::chrono::steady_clock::now() >= deadline) {
                require_setup(outcome, name);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    /// Disable, then delete, every CA this fixture created. Errors are printed
    /// and never thrown: LocalStack state is throwaway, but a stale CA would
    /// still confuse a reused container.
    void teardown(Aws::ACMPCA::ACMPCAClient& client) noexcept {
        for (const auto& arn : created) {
            model::UpdateCertificateAuthorityRequest disable;
            disable.SetCertificateAuthorityArn(arn);
            disable.SetStatus(model::CertificateAuthorityStatus::DISABLED);
            auto disabled = client.UpdateCertificateAuthority(disable);
            if (!disabled.IsSuccess()) {
                std::cerr << "teardown: UpdateCertificateAuthority " << arn << ": "
                          << disabled.GetError().GetMessage() << "\n";
            }
            model::DeleteCertificateAuthorityRequest del;
            del.SetCertificateAuthorityArn(arn);
            del.SetPermanentDeletionTimeInDays(7);
            auto deleted = client.DeleteCertificateAuthority(del);
            if (!deleted.IsSuccess()) {
                std::cerr << "teardown: DeleteCertificateAuthority " << arn << ": "
                          << deleted.GetError().GetMessage() << "\n";
            }
        }
        created.clear();
    }
};

auto cas() -> LocalStackCaFixture& {
    static LocalStackCaFixture fixture;
    return fixture;
}

struct CaSetupFixture {
    CaSetupFixture() { cas(); }
};

BOOST_GLOBAL_FIXTURE(PreflightSkipFixture);
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif
BOOST_GLOBAL_FIXTURE(CaSetupFixture);

auto make_config(const std::string& arn) -> aws_acm_pca_provider_config {
    aws_acm_pca_provider_config cfg;
    cfg.certificate_authority_arn = arn;
    cfg.aws.region = DUMMY_REGION;
    cfg.aws.endpoint_override = LOCALSTACK_ENDPOINT;
    cfg.aws.api_timeout = std::chrono::seconds(30);
    return cfg;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(acm_pca_localstack)

BOOST_AUTO_TEST_CASE(root_certificate_pem_returns_imported_root_and_caches_it) {
    aws_acm_pca_provider provider{make_config(cas().ocsp_ca_arn)};
    auto root = std::move(provider.root_certificate_pem()).get();
    BOOST_TEST(root.find("BEGIN CERTIFICATE") != std::string::npos);
    BOOST_TEST(std::move(provider.root_certificate_pem()).get() == root);
}

BOOST_AUTO_TEST_CASE(sign_csr_polls_until_issued_and_chains_to_root) {
    aws_acm_pca_provider provider{make_config(cas().ocsp_ca_arn)};
    auto request = acm_pca::make_leaf_request("kythira-localstack-leaf");
    auto material = std::move(provider.sign_csr(request.csr.csr_pem, request.options)).get();

    BOOST_TEST(material.certificate_pem.find("BEGIN CERTIFICATE") != std::string::npos);
    BOOST_TEST(material.chain_pem.starts_with(material.certificate_pem));
    auto root = std::move(provider.root_certificate_pem()).get();
    BOOST_TEST(acm_pca::chain_verifies(material.chain_pem, root));
}

BOOST_AUTO_TEST_CASE(revocation_configured_reflects_the_ca) {
    aws_acm_pca_provider with_ocsp{make_config(cas().ocsp_ca_arn)};
    aws_acm_pca_provider without{make_config(cas().bare_ca_arn)};
    BOOST_TEST(std::move(with_ocsp.revocation_configured()).get());
    BOOST_TEST(!std::move(without.revocation_configured()).get());
}

BOOST_AUTO_TEST_CASE(revoke_issued_certificate_with_reason) {
    aws_acm_pca_provider provider{make_config(cas().ocsp_ca_arn)};
    auto request = acm_pca::make_leaf_request("kythira-localstack-revoked");
    auto material = std::move(provider.sign_csr(request.csr.csr_pem, request.options)).get();

    auto serial = acm_pca::hex_serial(material.certificate_pem);
    BOOST_CHECK_NO_THROW(std::move(provider.revoke(serial, "KEY_COMPROMISE")).get());
}

BOOST_AUTO_TEST_CASE(revoke_with_unknown_reason_is_rejected_before_aws) {
    aws_acm_pca_provider provider{make_config(cas().ocsp_ca_arn)};
    BOOST_CHECK_THROW(std::move(provider.revoke("01", "NOT_A_REASON")).get(),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !KYTHIRA_HAS_AWS_ACM_PCA

BOOST_AUTO_TEST_CASE(skipped_no_acm_pca_sdk_component) {
    BOOST_TEST_MESSAGE("KYTHIRA_HAS_AWS_ACM_PCA not defined — skipping");
}

#endif  // KYTHIRA_HAS_AWS_ACM_PCA
