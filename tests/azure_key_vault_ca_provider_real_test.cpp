/// @file azure_key_vault_ca_provider_real_test.cpp
/// @brief Real-Azure integration tests for `azure_key_vault_ca_provider`.
///        Signs against a real, pre-provisioned Key Vault RSA key.
///
/// Unlike `azure_quorum_manager_real_test.cpp`'s fixture, this file creates or
/// deletes no Key Vault resources at all (design.md's "Integration test
/// fixture" section: Key Vault's soft-delete-by-default and optional
/// purge-protection make automated per-run vault lifecycle management
/// unsuitable) — operators pre-provision one vault + RSA key and supply its
/// coordinates via env vars, reused across every run.
///
/// Compiled only when `KYTHIRA_AZURE_REAL_TESTS` is defined; only registered
/// as a CTest case when the CMake option of the same name is `ON` (see
/// tests/CMakeLists.txt), matching azure_quorum_manager_real_test.cpp.

#define BOOST_TEST_MODULE azure_key_vault_ca_provider_real_test
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_AZURE_REAL_TESTS
#ifdef KYTHIRA_HAS_AZURE_KEY_VAULT

#include <raft/azure_key_vault_ca_provider_impl.hpp>
#include <raft/certificate_provider.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstdlib>
#include <optional>
#include <string>

#ifdef FIU_ENABLE
#include <fiu-control.h>
struct FiuInitFixture {
    FiuInitFixture() { fiu_init(0); }
};
BOOST_GLOBAL_FIXTURE(FiuInitFixture);
#endif

namespace {

auto env_opt(const char* name) -> std::optional<std::string> {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return std::nullopt;
    }
    return std::string(v);
}

/// nullopt when any required env var is missing — every test case checks
/// this and skips (BOOST_TEST_MESSAGE + early return) rather than failing,
/// the same "skip the whole suite gracefully" spirit as
/// AzureIntegrationFixture's credential preflight, just per-case here since
/// this file has no shared fixture object.
auto make_real_config() -> std::optional<raft::testing::azure_key_vault_ca_provider_config> {
    auto vault_url = env_opt("AZURE_TEST_KEY_VAULT_URL");
    auto key_name = env_opt("AZURE_TEST_KEY_VAULT_KEY_NAME");
    auto ca_cert_pem_path = env_opt("AZURE_TEST_KEY_VAULT_CA_CERT_FILE");
    if (!vault_url || !key_name || !ca_cert_pem_path) {
        return std::nullopt;
    }

    BIO* bio = BIO_new_file(ca_cert_pem_path->c_str(), "r");
    if (bio == nullptr) {
        return std::nullopt;
    }
    std::string pem;
    char buf[4096];
    int n = 0;
    while ((n = BIO_read(bio, buf, sizeof(buf))) > 0) {
        pem.append(buf, static_cast<std::size_t>(n));
    }
    BIO_free(bio);
    if (pem.empty()) {
        return std::nullopt;
    }

    raft::testing::azure_key_vault_ca_provider_config cfg;
    cfg.vault_url = *vault_url;
    cfg.key_name = *key_name;
    cfg.ca_certificate_pem = pem;
    return cfg;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(azure_key_vault_ca_provider_real)

BOOST_AUTO_TEST_CASE(sign_csr_against_real_vault) {
    auto cfg = make_real_config();
    if (!cfg) {
        BOOST_TEST_MESSAGE(
            "Skipping: AZURE_TEST_KEY_VAULT_URL/AZURE_TEST_KEY_VAULT_KEY_NAME/"
            "AZURE_TEST_KEY_VAULT_CA_CERT_FILE not fully set");
        return;
    }

    raft::testing::azure_key_vault_ca_provider provider{*cfg};

    auto csr = raft::testing::generate_key_and_csr(
        {.subject = {.common_name = "leaf.example.com"}, .dns_names = {"leaf.example.com"}});
    raft::testing::csr_signing_options options;
    options.dns_names = {"leaf.example.com"};

    auto material = std::move(provider.sign_csr(csr.csr_pem, options)).get();
    BOOST_CHECK(!material.certificate_pem.empty());
    BOOST_CHECK(material.private_key_pem.empty());

    BIO* leaf_bio = BIO_new_mem_buf(material.certificate_pem.data(),
                                    static_cast<int>(material.certificate_pem.size()));
    X509* leaf = PEM_read_bio_X509(leaf_bio, nullptr, nullptr, nullptr);
    BIO_free(leaf_bio);
    BOOST_REQUIRE(leaf != nullptr);

    BIO* ca_bio = BIO_new_mem_buf(cfg->ca_certificate_pem.data(),
                                  static_cast<int>(cfg->ca_certificate_pem.size()));
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

#ifdef FIU_ENABLE

BOOST_AUTO_TEST_CASE(fault_points_short_circuit_before_network_call) {
    // Deliberately does NOT check make_real_config()/skip: the whole point of
    // this case is that enabling the fault point rejects the Future without
    // ever reaching the vault, so it should pass even with no real Key Vault
    // configured at all (a placeholder vault_url/key_name is fine — the fault
    // point fires before either is used).
    raft::testing::azure_key_vault_ca_provider_config cfg;
    cfg.vault_url = "https://unreachable-placeholder.vault.azure.net";
    cfg.key_name = "placeholder-key";
    cfg.ca_certificate_pem = "-----BEGIN CERTIFICATE-----\nMA==\n-----END CERTIFICATE-----\n";

    raft::testing::azure_key_vault_ca_provider provider{cfg};

    fiu_enable("raft/azure/keyvault/sign", 1, nullptr, 0);
    auto csr = raft::testing::generate_key_and_csr(
        {.subject = {.common_name = "x.example.com"}, .dns_names = {"x.example.com"}});
    raft::testing::csr_signing_options options;
    options.dns_names = {"x.example.com"};
    BOOST_CHECK_THROW(std::move(provider.sign_csr(csr.csr_pem, options)).get(), std::exception);
    fiu_disable("raft/azure/keyvault/sign");

    fiu_enable("raft/azure/keyvault/get_key", 1, nullptr, 0);
    BOOST_CHECK_THROW(std::move(provider.root_certificate_pem()).get(), std::exception);
    fiu_disable("raft/azure/keyvault/get_key");
}

#endif  // FIU_ENABLE

BOOST_AUTO_TEST_SUITE_END()

#else  // !KYTHIRA_HAS_AZURE_KEY_VAULT

BOOST_AUTO_TEST_CASE(azure_key_vault_not_available) {
    BOOST_TEST_MESSAGE(
        "Azure Key Vault Keys SDK not available at build time; real-Azure CA provider tests "
        "skipped");
}

#endif  // KYTHIRA_HAS_AZURE_KEY_VAULT

#else  // !KYTHIRA_AZURE_REAL_TESTS

BOOST_AUTO_TEST_CASE(real_azure_tests_disabled) {
    BOOST_TEST_MESSAGE("KYTHIRA_AZURE_REAL_TESTS not defined; this binary is a compile-only stub");
}

#endif  // KYTHIRA_AZURE_REAL_TESTS
