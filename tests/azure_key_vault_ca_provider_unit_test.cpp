// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE azure_key_vault_ca_provider_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>

#ifdef KYTHIRA_HAS_AZURE_KEY_VAULT

#include <raft/azure_key_vault_ca_provider_impl.hpp>
#include <raft/certificate_provider.hpp>

#include <azure/core/base64.hpp>
#include <azure/core/credentials/credentials.hpp>

#include <boost/json.hpp>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
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

/// Signs `digest` the way Key Vault's `Sign` operation does for `alg`: RSA
/// signatures raw, PS256 with MGF1-SHA-256 and a digest-length salt, ECDSA
/// signatures as fixed-width `r || s` (JWS encoding) rather than DER.
auto sign_like_key_vault(EVP_PKEY* key, const std::string& alg,
                         const std::vector<std::uint8_t>& digest) -> std::vector<std::uint8_t> {
    const EVP_MD* md = alg.ends_with("512")   ? EVP_sha512()
                       : alg.ends_with("384") ? EVP_sha384()
                                              : EVP_sha256();
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx{EVP_PKEY_CTX_new(key, nullptr),
                                                                    EVP_PKEY_CTX_free};
    BOOST_REQUIRE(ctx != nullptr);
    BOOST_REQUIRE_EQUAL(EVP_PKEY_sign_init(ctx.get()), 1);
    BOOST_REQUIRE_EQUAL(EVP_PKEY_CTX_set_signature_md(ctx.get(), md), 1);
    if (alg.starts_with("RS")) {
        BOOST_REQUIRE_EQUAL(EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_PADDING), 1);
    } else if (alg.starts_with("PS")) {
        BOOST_REQUIRE_EQUAL(EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_PSS_PADDING), 1);
        BOOST_REQUIRE_EQUAL(EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx.get(), RSA_PSS_SALTLEN_DIGEST), 1);
        BOOST_REQUIRE_EQUAL(EVP_PKEY_CTX_set_rsa_mgf1_md(ctx.get(), md), 1);
    }
    std::size_t len = 0;
    BOOST_REQUIRE_EQUAL(EVP_PKEY_sign(ctx.get(), nullptr, &len, digest.data(), digest.size()), 1);
    std::vector<std::uint8_t> sig(len);
    BOOST_REQUIRE_EQUAL(EVP_PKEY_sign(ctx.get(), sig.data(), &len, digest.data(), digest.size()),
                        1);
    sig.resize(len);
    if (!alg.starts_with("ES")) {
        return sig;
    }
    const unsigned char* p = sig.data();
    ECDSA_SIG* ecdsa = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(sig.size()));
    BOOST_REQUIRE(ecdsa != nullptr);
    const auto field = static_cast<std::size_t>((EVP_PKEY_get_bits(key) + 7) / 8);
    std::vector<std::uint8_t> jws(2 * field);
    BN_bn2binpad(ECDSA_SIG_get0_r(ecdsa), jws.data(), static_cast<int>(field));
    BN_bn2binpad(ECDSA_SIG_get0_s(ecdsa), jws.data() + field, static_cast<int>(field));
    ECDSA_SIG_free(ecdsa);
    return jws;
}

/// Intercepts Key Vault's `POST .../sign` call and returns a signature
/// computed locally with `_ca_key` (a private key never sent over the wire in
/// this test) instead of forwarding to a real Key Vault — the
/// `stub_http_transport_policy` technique tasks.md's Task 7 describes,
/// applied to `CryptographyClient` (itself built over the same
/// `Azure::Core::Http::_internal::HttpPipeline` `KeyClient` uses). The
/// request's `alg` is recorded in `seen_alg` and selects how to sign.
/// `truncate_to` (when non-zero) shortens the returned signature, to model a
/// malformed Key Vault response.
class StubKeyVaultSignPolicy : public Azure::Core::Http::Policies::HttpPolicy {
public:
    explicit StubKeyVaultSignPolicy(std::shared_ptr<EVP_PKEY> ca_key,
                                    std::shared_ptr<std::string> seen_alg = nullptr,
                                    std::size_t truncate_to = 0)
        : _ca_key(std::move(ca_key)), _seen_alg(std::move(seen_alg)), _truncate_to(truncate_to) {}

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
        std::string alg(parsed.at("alg").as_string());
        std::string digest_b64url(parsed.at("value").as_string());
        auto digest = Azure::Core::_internal::Base64Url::Base64UrlDecode(digest_b64url);
        if (_seen_alg) {
            *_seen_alg = alg;
        }

        auto signature = sign_like_key_vault(_ca_key.get(), alg, digest);
        if (_truncate_to != 0) {
            signature.resize(_truncate_to);
        }

        boost::json::object response_json;
        response_json["kid"] = "https://fake-vault.vault.azure.net/keys/test-key/000";
        response_json["value"] = Azure::Core::_internal::Base64Url::Base64UrlEncode(signature);
        std::string response_str = boost::json::serialize(response_json);

        auto response = std::make_unique<Azure::Core::Http::RawResponse>(
            1, 1, Azure::Core::Http::HttpStatusCode::Ok, "OK");
        response->SetBody(std::vector<std::uint8_t>(response_str.begin(), response_str.end()));
        response->SetHeader("Content-Type", "application/json");
        return response;
    }

private:
    std::shared_ptr<EVP_PKEY> _ca_key;
    std::shared_ptr<std::string> _seen_alg;
    std::size_t _truncate_to;
};

auto parse_private_key(const std::string& pem) -> std::shared_ptr<EVP_PKEY> {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    BOOST_REQUIRE(pkey != nullptr);
    return std::shared_ptr<EVP_PKEY>(pkey, EVP_PKEY_free);
}

auto make_config_with_stub(const raft::testing::certificate_authority& ca,
                           std::shared_ptr<EVP_PKEY> ca_key,
                           raft::testing::azure_key_vault_signing_algorithm algorithm =
                               raft::testing::azure_key_vault_signing_algorithm::rs256,
                           std::shared_ptr<std::string> seen_alg = nullptr,
                           std::size_t truncate_to = 0)
    -> raft::testing::azure_key_vault_ca_provider_config {
    raft::testing::azure_key_vault_ca_provider_config cfg;
    cfg.vault_url = "https://fake-vault.vault.azure.net";
    cfg.key_name = "test-key";
    cfg.ca_certificate_pem = ca.root_certificate_pem();
    cfg.signing_algorithm = algorithm;
    cfg.azure.credential = std::make_shared<FakeTokenCredential>();
    cfg.client_options.PerRetryPolicies.push_back(
        std::make_unique<StubKeyVaultSignPolicy>(ca_key, std::move(seen_alg), truncate_to));
    return cfg;
}

auto ca_key_of(const raft::testing::certificate_authority& ca) -> std::shared_ptr<EVP_PKEY> {
    return parse_private_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca));
}

auto leaf_csr() -> std::string {
    return raft::testing::generate_key_and_csr(
               {.subject = {.common_name = "leaf.example.com"}, .dns_names = {"leaf.example.com"}})
        .csr_pem;
}

auto leaf_options() -> raft::testing::csr_signing_options {
    raft::testing::csr_signing_options options;
    options.dns_names = {"leaf.example.com"};
    return options;
}

auto parse_cert(const std::string& pem) -> std::unique_ptr<X509, decltype(&X509_free)> {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    BOOST_REQUIRE(cert != nullptr);
    return {cert, X509_free};
}

/// True when `fn` throws an exception whose message contains `needle`.
template<typename Fn> auto throws_containing(Fn&& fn, const std::string& needle) -> bool {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::exception& ex) {
        BOOST_TEST_MESSAGE("threw: " << ex.what());
        return std::string(ex.what()).find(needle) != std::string::npos;
    }
    return false;
}

struct algorithm_case {
    raft::testing::key_algorithm ca_key;
    raft::testing::azure_key_vault_signing_algorithm signing;
    const char* key_vault_name;  // the `alg` Key Vault's Sign request must carry
    int signature_nid;           // the certificate's signatureAlgorithm
    int digest_nid;              // the digest X509_get_signature_info reports
};

}  // namespace

BOOST_AUTO_TEST_SUITE(construction_validation)

BOOST_AUTO_TEST_CASE(empty_vault_url_throws) {
    raft::testing::certificate_authority ca{{.algorithm = raft::testing::key_algorithm::rsa_2048}};
    auto cfg = make_config_with_stub(
        ca,
        parse_private_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca)));
    cfg.vault_url.clear();
    BOOST_CHECK_THROW((raft::testing::azure_key_vault_ca_provider{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_key_name_throws) {
    raft::testing::certificate_authority ca{{.algorithm = raft::testing::key_algorithm::rsa_2048}};
    auto cfg = make_config_with_stub(
        ca,
        parse_private_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca)));
    cfg.key_name.clear();
    BOOST_CHECK_THROW((raft::testing::azure_key_vault_ca_provider{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_ca_certificate_pem_throws) {
    raft::testing::certificate_authority ca{{.algorithm = raft::testing::key_algorithm::rsa_2048}};
    auto cfg = make_config_with_stub(
        ca,
        parse_private_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca)));
    cfg.ca_certificate_pem.clear();
    BOOST_CHECK_THROW((raft::testing::azure_key_vault_ca_provider{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(root_certificate)

BOOST_AUTO_TEST_CASE(returns_configured_pem_unmodified) {
    raft::testing::certificate_authority ca{{.algorithm = raft::testing::key_algorithm::rsa_2048}};
    auto cfg = make_config_with_stub(
        ca,
        parse_private_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca)));
    raft::testing::azure_key_vault_ca_provider provider{cfg};
    auto pem = std::move(provider.root_certificate_pem()).get();
    BOOST_CHECK_EQUAL(pem, ca.root_certificate_pem());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(sign_csr_happy_path)

BOOST_AUTO_TEST_CASE(assembled_certificate_signature_validates) {
    raft::testing::certificate_authority ca{{.algorithm = raft::testing::key_algorithm::rsa_2048}};
    auto ca_key =
        parse_private_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca));
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

BOOST_AUTO_TEST_SUITE(sign_csr_algorithms)

// Every Key Vault algorithm issues a certificate that verifies against the CA
// certificate and names that algorithm. Before this was fixed only rs256
// worked: rs384/rs512 were signed over a SHA-256 digest and ps256/es256/es384
// threw std::logic_error. Several issuances per algorithm vary the TBS bytes,
// so ECDSA r/s values with leading zeros and RSA signatures ending in 0x00
// get a chance to show up.
BOOST_AUTO_TEST_CASE(every_algorithm_issues_a_verifiable_certificate) {
    using raft::testing::azure_key_vault_signing_algorithm;
    using raft::testing::key_algorithm;
    const std::vector<algorithm_case> cases{
        {key_algorithm::rsa_2048, azure_key_vault_signing_algorithm::rs256, "RS256",
         NID_sha256WithRSAEncryption, NID_sha256},
        {key_algorithm::rsa_2048, azure_key_vault_signing_algorithm::rs384, "RS384",
         NID_sha384WithRSAEncryption, NID_sha384},
        {key_algorithm::rsa_2048, azure_key_vault_signing_algorithm::rs512, "RS512",
         NID_sha512WithRSAEncryption, NID_sha512},
        {key_algorithm::rsa_2048, azure_key_vault_signing_algorithm::ps256, "PS256", NID_rsassaPss,
         NID_sha256},
        {key_algorithm::ecdsa_p256, azure_key_vault_signing_algorithm::es256, "ES256",
         NID_ecdsa_with_SHA256, NID_sha256},
        {key_algorithm::ecdsa_p384, azure_key_vault_signing_algorithm::es384, "ES384",
         NID_ecdsa_with_SHA384, NID_sha384},
    };
    for (const auto& c : cases) {
        BOOST_TEST_CONTEXT("algorithm " << c.key_vault_name) {
            raft::testing::certificate_authority ca{{.algorithm = c.ca_key}};
            auto seen_alg = std::make_shared<std::string>();
            auto cfg = make_config_with_stub(ca, ca_key_of(ca), c.signing, seen_alg);
            raft::testing::azure_key_vault_ca_provider provider{cfg};
            auto ca_cert = parse_cert(cfg.ca_certificate_pem);
            std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> ca_pubkey{
                X509_get_pubkey(ca_cert.get()), EVP_PKEY_free};
            BOOST_REQUIRE(ca_pubkey != nullptr);

            for (int i = 0; i < 16; ++i) {
                auto material = std::move(provider.sign_csr(leaf_csr(), leaf_options())).get();
                BOOST_CHECK_EQUAL(*seen_alg, c.key_vault_name);
                auto leaf = parse_cert(material.certificate_pem);
                BOOST_CHECK_EQUAL(X509_verify(leaf.get(), ca_pubkey.get()), 1);
                BOOST_CHECK_EQUAL(X509_get_signature_nid(leaf.get()), c.signature_nid);
                int md_nid = NID_undef;
                int pk_nid = NID_undef;
                BOOST_CHECK_EQUAL(
                    X509_get_signature_info(leaf.get(), &md_nid, &pk_nid, nullptr, nullptr), 1);
                BOOST_CHECK_EQUAL(md_nid, c.digest_nid);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(ecdsa_algorithm_with_rsa_ca_key_is_refused) {
    raft::testing::certificate_authority ca{{.algorithm = raft::testing::key_algorithm::rsa_2048}};
    auto cfg = make_config_with_stub(ca, ca_key_of(ca),
                                     raft::testing::azure_key_vault_signing_algorithm::es256);
    raft::testing::azure_key_vault_ca_provider provider{cfg};
    BOOST_CHECK(throws_containing(
        [&] { std::move(provider.sign_csr(leaf_csr(), leaf_options())).get(); }, "prime256v1"));
}

BOOST_AUTO_TEST_CASE(es384_with_p256_ca_key_is_refused) {
    raft::testing::certificate_authority ca{
        {.algorithm = raft::testing::key_algorithm::ecdsa_p256}};
    auto cfg = make_config_with_stub(ca, ca_key_of(ca),
                                     raft::testing::azure_key_vault_signing_algorithm::es384);
    raft::testing::azure_key_vault_ca_provider provider{cfg};
    BOOST_CHECK(throws_containing(
        [&] { std::move(provider.sign_csr(leaf_csr(), leaf_options())).get(); }, "secp384r1"));
}

BOOST_AUTO_TEST_CASE(rsa_algorithm_with_ec_ca_key_is_refused) {
    raft::testing::certificate_authority ca{
        {.algorithm = raft::testing::key_algorithm::ecdsa_p256}};
    auto cfg = make_config_with_stub(ca, ca_key_of(ca),
                                     raft::testing::azure_key_vault_signing_algorithm::ps256);
    raft::testing::azure_key_vault_ca_provider provider{cfg};
    BOOST_CHECK(throws_containing(
        [&] { std::move(provider.sign_csr(leaf_csr(), leaf_options())).get(); }, "not RSA"));
}

// The vault key named by key_name is not the one behind ca_certificate_pem:
// the certificate would never validate, so sign_csr must fail rather than
// hand it out.
BOOST_AUTO_TEST_CASE(vault_key_not_matching_ca_certificate_is_refused) {
    raft::testing::certificate_authority ca{{.algorithm = raft::testing::key_algorithm::rsa_2048}};
    raft::testing::certificate_authority other{
        {.algorithm = raft::testing::key_algorithm::rsa_2048}};
    auto cfg = make_config_with_stub(ca, ca_key_of(other));
    raft::testing::azure_key_vault_ca_provider provider{cfg};
    BOOST_CHECK(
        throws_containing([&] { std::move(provider.sign_csr(leaf_csr(), leaf_options())).get(); },
                          "does not verify"));
}

BOOST_AUTO_TEST_CASE(wrong_length_ecdsa_signature_is_refused) {
    raft::testing::certificate_authority ca{
        {.algorithm = raft::testing::key_algorithm::ecdsa_p256}};
    auto cfg = make_config_with_stub(
        ca, ca_key_of(ca), raft::testing::azure_key_vault_signing_algorithm::es256, nullptr, 63);
    raft::testing::azure_key_vault_ca_provider provider{cfg};
    BOOST_CHECK(
        throws_containing([&] { std::move(provider.sign_csr(leaf_csr(), leaf_options())).get(); },
                          "63-byte ECDSA signature"));
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
        ca,
        parse_private_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca)));
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
        ca,
        parse_private_key(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca)));
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
