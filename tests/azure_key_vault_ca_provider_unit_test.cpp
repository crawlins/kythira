#define BOOST_TEST_MODULE azure_key_vault_ca_provider_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>

#ifdef KYTHIRA_HAS_AZURE_KEY_VAULT

#include <raft/azure_key_vault_ca_provider_impl.hpp>
#include <raft/certificate_provider.hpp>

#include <azure/core/base64.hpp>
#include <azure/core/credentials/credentials.hpp>

#include <boost/json.hpp>

#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef FIU_ENABLE
#include <fiu-control.h>
#endif

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

namespace {

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        folly::init(&argc, &argv, false);
    }
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

#ifdef FIU_ENABLE
struct FiuInitFixture {
    FiuInitFixture() { fiu_init(0); }
};
BOOST_GLOBAL_FIXTURE(FiuInitFixture);
#endif

/// Never performs real network I/O: `GetToken` returns a fixed fake token
/// synchronously. Needed because `BearerTokenAuthenticationPolicy` (a
/// per-retry policy, positioned before the client-supplied
/// `PerRetryPolicies` where `StubKeyVaultSignPolicy` below is inserted) always
/// runs first and would otherwise probe real credential sources.
class FakeTokenCredential : public Azure::Core::Credentials::TokenCredential {
public:
    FakeTokenCredential() : TokenCredential("FakeTokenCredential") {}

    auto GetToken(Azure::Core::Credentials::TokenRequestContext const&,
                  Azure::Core::Context const&) const
        -> Azure::Core::Credentials::AccessToken override {
        Azure::Core::Credentials::AccessToken token;
        token.Token = "fake-token";
        token.ExpiresOn = std::chrono::system_clock::now() + std::chrono::hours(1);
        return token;
    }
};

/// Intercepts Key Vault's `POST .../sign` call and returns a signature
/// computed locally with `_ca_key` (an RSA private key never sent over the
/// wire in this test) instead of forwarding to a real Key Vault — the
/// `stub_http_transport_policy` technique tasks.md's Task 7 describes,
/// applied to `CryptographyClient` (itself built over the same
/// `Azure::Core::Http::_internal::HttpPipeline` `KeyClient` uses).
class StubKeyVaultSignPolicy : public Azure::Core::Http::Policies::HttpPolicy {
public:
    explicit StubKeyVaultSignPolicy(std::shared_ptr<EVP_PKEY> ca_key)
        : _ca_key(std::move(ca_key)) {}

    [[nodiscard]] auto Clone() const -> std::unique_ptr<HttpPolicy> override {
        return std::make_unique<StubKeyVaultSignPolicy>(*this);
    }

    [[nodiscard]] auto Send(Azure::Core::Http::Request& request,
                            Azure::Core::Http::Policies::NextHttpPolicy,
                            Azure::Core::Context const& context) const
        -> std::unique_ptr<Azure::Core::Http::RawResponse> override {
        auto* stream = request.GetBodyStream();
        BOOST_REQUIRE(stream != nullptr);
        auto body_bytes = stream->ReadToEnd(context);
        std::string body_str(body_bytes.begin(), body_bytes.end());
        auto parsed = boost::json::parse(body_str);
        std::string digest_b64url(parsed.at("value").as_string());
        auto digest = Azure::Core::_internal::Base64Url::Base64UrlDecode(digest_b64url);

        // EVP_PKEY_get1_RSA/RSA_size/RSA_sign/RSA_free are OSSL_DEPRECATEDIN_3_0
        // but this test deliberately uses the same low-level RSA API the
        // production RSA_METHOD external-signer path is built on, to compute
        // "what Key Vault would have returned" locally with a test-only key.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        RSA* rsa = EVP_PKEY_get1_RSA(_ca_key.get());
        BOOST_REQUIRE(rsa != nullptr);
        std::vector<unsigned char> sigret(static_cast<std::size_t>(RSA_size(rsa)));
        unsigned int siglen = 0;
        int ok = RSA_sign(NID_sha256, digest.data(), static_cast<unsigned int>(digest.size()),
                          sigret.data(), &siglen, rsa);
        RSA_free(rsa);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
        BOOST_REQUIRE_EQUAL(ok, 1);
        sigret.resize(siglen);

        boost::json::object response_json;
        response_json["kid"] = "https://fake-vault.vault.azure.net/keys/test-key/000";
        response_json["value"] = Azure::Core::_internal::Base64Url::Base64UrlEncode(
            std::vector<std::uint8_t>(sigret.begin(), sigret.end()));
        std::string response_str = boost::json::serialize(response_json);

        auto response = std::make_unique<Azure::Core::Http::RawResponse>(
            1, 1, Azure::Core::Http::HttpStatusCode::Ok, "OK");
        response->SetBody(std::vector<std::uint8_t>(response_str.begin(), response_str.end()));
        response->SetHeader("Content-Type", "application/json");
        return response;
    }

private:
    std::shared_ptr<EVP_PKEY> _ca_key;
};

auto parse_rsa_key(const std::string& pem) -> std::shared_ptr<EVP_PKEY> {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    BOOST_REQUIRE(pkey != nullptr);
    return std::shared_ptr<EVP_PKEY>(pkey, EVP_PKEY_free);
}

auto make_config_with_stub(const raft::testing::certificate_authority& ca,
                           std::shared_ptr<EVP_PKEY> ca_key)
    -> raft::testing::azure_key_vault_ca_provider_config {
    raft::testing::azure_key_vault_ca_provider_config cfg;
    cfg.vault_url = "https://fake-vault.vault.azure.net";
    cfg.key_name = "test-key";
    cfg.ca_certificate_pem = ca.root_certificate_pem();
    cfg.azure.credential = std::make_shared<FakeTokenCredential>();
    cfg.client_options.PerRetryPolicies.push_back(std::make_unique<StubKeyVaultSignPolicy>(ca_key));
    return cfg;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(construction_validation)

BOOST_AUTO_TEST_CASE(empty_vault_url_throws) {
    raft::testing::certificate_authority ca{{.algorithm = raft::testing::key_algorithm::rsa_2048}};
    auto cfg = make_config_with_stub(
        ca, parse_rsa_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca)));
    cfg.vault_url.clear();
    BOOST_CHECK_THROW((raft::testing::azure_key_vault_ca_provider{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_key_name_throws) {
    raft::testing::certificate_authority ca{{.algorithm = raft::testing::key_algorithm::rsa_2048}};
    auto cfg = make_config_with_stub(
        ca, parse_rsa_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca)));
    cfg.key_name.clear();
    BOOST_CHECK_THROW((raft::testing::azure_key_vault_ca_provider{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_ca_certificate_pem_throws) {
    raft::testing::certificate_authority ca{{.algorithm = raft::testing::key_algorithm::rsa_2048}};
    auto cfg = make_config_with_stub(
        ca, parse_rsa_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca)));
    cfg.ca_certificate_pem.clear();
    BOOST_CHECK_THROW((raft::testing::azure_key_vault_ca_provider{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(root_certificate)

BOOST_AUTO_TEST_CASE(returns_configured_pem_unmodified) {
    raft::testing::certificate_authority ca{{.algorithm = raft::testing::key_algorithm::rsa_2048}};
    auto cfg = make_config_with_stub(
        ca, parse_rsa_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca)));
    raft::testing::azure_key_vault_ca_provider provider{cfg};
    auto pem = std::move(provider.root_certificate_pem()).get();
    BOOST_CHECK_EQUAL(pem, ca.root_certificate_pem());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(sign_csr_happy_path)

BOOST_AUTO_TEST_CASE(assembled_certificate_signature_validates) {
    raft::testing::certificate_authority ca{{.algorithm = raft::testing::key_algorithm::rsa_2048}};
    auto ca_key =
        parse_rsa_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca));
    auto cfg = make_config_with_stub(ca, ca_key);
    raft::testing::azure_key_vault_ca_provider provider{cfg};

    auto csr = raft::testing::generate_key_and_csr(
        {.subject = {.common_name = "leaf.example.com"}, .dns_names = {"leaf.example.com"}});

    raft::testing::csr_signing_options options;
    options.dns_names = {"leaf.example.com"};

    auto material = std::move(provider.sign_csr(csr.csr_pem, options)).get();
    BOOST_CHECK(!material.certificate_pem.empty());
    BOOST_CHECK(material.private_key_pem.empty());

    BIO* bio = BIO_new_mem_buf(material.certificate_pem.data(),
                               static_cast<int>(material.certificate_pem.size()));
    X509* leaf = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    BOOST_REQUIRE(leaf != nullptr);

    BIO* ca_bio = BIO_new_mem_buf(cfg.ca_certificate_pem.data(),
                                  static_cast<int>(cfg.ca_certificate_pem.size()));
    X509* ca_cert = PEM_read_bio_X509(ca_bio, nullptr, nullptr, nullptr);
    BIO_free(ca_bio);
    BOOST_REQUIRE(ca_cert != nullptr);

    EVP_PKEY* ca_pubkey = X509_get_pubkey(ca_cert);
    BOOST_REQUIRE(ca_pubkey != nullptr);
    int verify_result = X509_verify(leaf, ca_pubkey);
    EVP_PKEY_free(ca_pubkey);
    X509_free(ca_cert);
    X509_free(leaf);

    BOOST_CHECK_EQUAL(verify_result, 1);
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef FIU_ENABLE

BOOST_AUTO_TEST_SUITE(fault_injection)

struct FaultPointFixture {
    explicit FaultPointFixture(std::string name) : _name(std::move(name)) {
        fiu_enable(_name.c_str(), 1, nullptr, 0);
    }
    ~FaultPointFixture() { fiu_disable(_name.c_str()); }
    std::string _name;
};

BOOST_AUTO_TEST_CASE(keyvault_sign_fault) {
    FaultPointFixture fp("raft/azure/keyvault/sign");
    raft::testing::certificate_authority ca{{.algorithm = raft::testing::key_algorithm::rsa_2048}};
    auto cfg = make_config_with_stub(
        ca, parse_rsa_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca)));
    raft::testing::azure_key_vault_ca_provider provider{cfg};
    auto csr = raft::testing::generate_key_and_csr(
        {.subject = {.common_name = "leaf.example.com"}, .dns_names = {"leaf.example.com"}});
    raft::testing::csr_signing_options options;
    options.dns_names = {"leaf.example.com"};
    BOOST_CHECK_THROW(std::move(provider.sign_csr(csr.csr_pem, options)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(keyvault_get_key_fault) {
    FaultPointFixture fp("raft/azure/keyvault/get_key");
    raft::testing::certificate_authority ca{{.algorithm = raft::testing::key_algorithm::rsa_2048}};
    auto cfg = make_config_with_stub(
        ca, parse_rsa_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca)));
    raft::testing::azure_key_vault_ca_provider provider{cfg};
    BOOST_CHECK_THROW(std::move(provider.root_certificate_pem()).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // FIU_ENABLE

#else  // !KYTHIRA_HAS_AZURE_KEY_VAULT

BOOST_AUTO_TEST_CASE(azure_key_vault_not_available) {
    BOOST_TEST_MESSAGE(
        "Azure Key Vault Keys SDK not available at build time; azure_key_vault_ca_provider tests "
        "skipped");
}

#endif  // KYTHIRA_HAS_AZURE_KEY_VAULT
