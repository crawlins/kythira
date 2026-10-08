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
#include <aws/acm-pca/model/ListCertificateAuthoritiesRequest.h>
#include <aws/core/Aws.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#include <memory>
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
#include <memory>
#include <string>

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
        _init = std::make_unique<folly::Init>(&argc, &argv, false);
    }
    std::unique_ptr<folly::Init> _init;
};
#endif

auto client_config() -> Aws::Client::ClientConfiguration {
    Aws::Client::ClientConfiguration c;
    c.region = DUMMY_REGION;
    c.endpointOverride = LOCALSTACK_ENDPOINT;
    c.requestTimeoutMs = 10000;
    c.connectTimeoutMs = 10000;
    return c;
}

/// Two ACTIVE self-signed root CAs: one with OCSP enabled (revocation works)
/// and one with no revocation configuration (Requirement 10.7's error path).
/// Both come from `ephemeral_acm_pca`, the bootstrap the real-AWS suite uses,
/// without its tags or signal wiring: LocalStack state is throwaway
/// (acm-pca-ephemeral-ca Requirement 7.3). Torn down in the destructor;
/// Boost destroys global fixtures in reverse, so this outlives every case.
struct LocalStackCaFixture {
    std::string ocsp_ca_arn;
    std::string bare_ca_arn;

    LocalStackCaFixture() {
        Aws::InitAPI(sdk_options);
        // Same placement as aws_quorum_manager_localstack_test.cpp: the probe
        // runs right after InitAPI in this constructor, not in a separate
        // global fixture, so the ordering can't go wrong.
        Aws::ACMPCA::ACMPCAClient client{client_config()};
        auto listed = client.ListCertificateAuthorities(model::ListCertificateAuthoritiesRequest{});
        if (!listed.IsSuccess()) {
            std::cerr << "SKIP: the acm-pca service is unusable at " << LOCALSTACK_ENDPOINT
                      << " (ACM Private CA emulation is LocalStack Pro only): "
                      << listed.GetError().GetMessage() << "\n";
            Aws::ShutdownAPI(sdk_options);
            std::exit(77);
        }
        // A failure here means this LocalStack can't emulate the setup, not
        // that the provider is wrong, so it is a skip (Requirement 7.2).
        try {
            ocsp_ca_arn =
                provision(ocsp_ca, acm_pca::ca_revocation::ocsp, "kythira localstack root (ocsp)");
            bare_ca_arn = provision(bare_ca, acm_pca::ca_revocation::none,
                                    "kythira localstack root (no revocation)");
        } catch (const acm_pca::ephemeral_ca_unavailable& ex) {
            std::cerr << "SKIP: LocalStack could not provision an ACTIVE root CA: " << ex.what()
                      << "\n";
            teardown();
            Aws::ShutdownAPI(sdk_options);
            std::exit(77);
        }
    }

    ~LocalStackCaFixture() {
        teardown();
        Aws::ShutdownAPI(sdk_options);
    }

    LocalStackCaFixture(const LocalStackCaFixture&) = delete;
    auto operator=(const LocalStackCaFixture&) -> LocalStackCaFixture& = delete;

private:
    Aws::SDKOptions sdk_options;
    std::unique_ptr<acm_pca::ephemeral_acm_pca> ocsp_ca;
    std::unique_ptr<acm_pca::ephemeral_acm_pca> bare_ca;

    static auto provision(std::unique_ptr<acm_pca::ephemeral_acm_pca>& slot,
                          acm_pca::ca_revocation revocation, const char* common_name)
        -> std::string {
        acm_pca::ephemeral_acm_pca::options opts;
        opts.client = client_config();
        opts.revocation = revocation;
        opts.common_name = common_name;
        // What this suite always used: LocalStack's bootstrap is quick, and
        // a general-purpose CA with a 30-day root.
        opts.step_timeout = std::chrono::seconds(60);
        opts.short_lived = false;
        opts.root_validity_days = 30;
        slot = std::make_unique<acm_pca::ephemeral_acm_pca>(std::move(opts));
        slot->provision();
        return slot->arn();
    }

    /// Errors are printed and never thrown: LocalStack state is throwaway,
    /// but a stale CA would still confuse a reused container.
    void teardown() noexcept {
        for (auto* ca : {ocsp_ca.get(), bare_ca.get()}) {
            if (ca != nullptr) {
                ca->teardown();
            }
        }
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
