// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * DTLS cipher-suite restriction for the CoAP transport (coap-transport
 * Requirement 6.4).
 *
 * coap_client_config/coap_server_config::cipher_suites and
 * pki_credentials::cipher_suites used to be validated and logged on the
 * libcoap backend but never handed to the TLS library, so every handshake ran
 * on libcoap's defaults whatever was configured; cantcoap and libnyoci passed
 * the configured IANA names straight to OpenSSL, which does not understand
 * them. These cases check the shared name translation and, on libcoap, that a
 * real handshake succeeds only when both sides' restrictions overlap.
 */
#define BOOST_TEST_MODULE coap_dtls_cipher_suites_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>

#include <raft/coap_dtls_cipher_suites.hpp>

#ifdef LIBCOAP_AVAILABLE
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

#include <cstdio>
#include <filesystem>
#include <format>
#include <random>
#include <string>
#include <vector>

#include "test_timeout_scale.hpp"

using namespace kythira;

BOOST_AUTO_TEST_SUITE(coap_dtls_cipher_list)

BOOST_AUTO_TEST_CASE(empty_configuration_keeps_the_defaults) {
    BOOST_TEST(detail::dtls_cipher_list({}).empty());
}

BOOST_AUTO_TEST_CASE(iana_names_are_translated_to_openssl_names) {
    BOOST_TEST(detail::dtls_cipher_list({"TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256",
                                         "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384"}) ==
               "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384");
}

BOOST_AUTO_TEST_CASE(openssl_names_pass_through) {
    BOOST_TEST(detail::dtls_cipher_list({"ECDHE-ECDSA-AES128-GCM-SHA256"}) ==
               "ECDHE-ECDSA-AES128-GCM-SHA256");
}

// SSL_CTX_set_cipher_list() accepts a list as long as *one* entry matches, so
// without a per-entry check a typo would vanish next to a valid name.
BOOST_AUTO_TEST_CASE(an_unknown_name_is_rejected_even_beside_a_valid_one) {
    BOOST_CHECK_THROW(
        detail::dtls_cipher_list({"TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256", "NOT-A-CIPHER"}),
        coap_security_config_error);
}

// TLS 1.3 suites cannot be negotiated over DTLS 1.2.
BOOST_AUTO_TEST_CASE(tls13_suites_are_rejected) {
    BOOST_CHECK_THROW(detail::dtls_cipher_list({"TLS_AES_128_GCM_SHA256"}),
                      coap_security_config_error);
}

BOOST_AUTO_TEST_CASE(empty_names_are_rejected) {
    BOOST_CHECK_THROW(detail::dtls_cipher_list({""}), coap_security_config_error);
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef LIBCOAP_AVAILABLE
namespace {

using test_transport_types =
    kythira::default_transport_types<kythira::future_default<kythira::request_vote_response<>>,
                                     kythira::json_rpc_serializer<std::vector<std::byte>>,
                                     kythira::noop_metrics, kythira::console_logger>;

constexpr const char* aes128 = "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256";
constexpr const char* aes256 = "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384";

auto make_key() -> EVP_PKEY* {
    EVP_PKEY* key = EVP_EC_gen("P-256");
    BOOST_REQUIRE(key != nullptr);
    return key;
}

auto make_cert(const char* common_name, long serial, EVP_PKEY* subject_key, X509* issuer,
               EVP_PKEY* issuer_key) -> X509* {
    X509* cert = X509_new();
    BOOST_REQUIRE(cert != nullptr);
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), serial);
    X509_gmtime_adj(X509_getm_notBefore(cert), -60);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60L * 24L);
    X509_set_pubkey(cert, subject_key);
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0);
    X509_set_issuer_name(cert, issuer != nullptr ? X509_get_subject_name(issuer) : name);
    if (issuer == nullptr) {
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
        X509_EXTENSION* ext =
            X509V3_EXT_conf_nid(nullptr, &ctx, NID_basic_constraints, "critical,CA:TRUE");
        BOOST_REQUIRE(ext != nullptr);
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
    }
    BOOST_REQUIRE(X509_sign(cert, issuer_key, EVP_sha256()) > 0);
    return cert;
}

// A CA plus an EC server and client certificate on disk. EC keys keep every
// suite under test an ECDHE-ECDSA one, so the only thing that can make a
// handshake fail is the cipher-suite restriction itself.
struct test_pki {
    std::filesystem::path dir;
    std::string ca_file, server_cert_file, server_key_file, client_cert_file, client_key_file;

    test_pki() {
        dir = std::filesystem::temp_directory_path() /
              ("coap_cipher_suites_test_" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(dir);
        EVP_PKEY* ca_key = make_key();
        X509* ca = make_cert("kythira-test-ca", 1, ca_key, nullptr, ca_key);
        EVP_PKEY* server_key = make_key();
        X509* server = make_cert("localhost", 2, server_key, ca, ca_key);
        EVP_PKEY* client_key = make_key();
        X509* client = make_cert("client", 3, client_key, ca, ca_key);
        ca_file = write_cert("ca.pem", ca);
        server_cert_file = write_cert("server.pem", server);
        server_key_file = write_key("server.key", server_key);
        client_cert_file = write_cert("client.pem", client);
        client_key_file = write_key("client.key", client_key);
        X509_free(client);
        EVP_PKEY_free(client_key);
        X509_free(server);
        EVP_PKEY_free(server_key);
        X509_free(ca);
        EVP_PKEY_free(ca_key);
    }

    ~test_pki() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    test_pki(const test_pki&) = delete;
    auto operator=(const test_pki&) -> test_pki& = delete;

    [[nodiscard]] auto server_config(std::vector<std::string> suites) const -> coap_server_config {
        coap_server_config config;
        config.enable_dtls = true;
        config.cert_file = server_cert_file;
        config.key_file = server_key_file;
        config.ca_file = ca_file;
        config.verify_peer_cert = true;
        config.cipher_suites = std::move(suites);
        return config;
    }

    [[nodiscard]] auto client_config(std::vector<std::string> suites) const -> coap_client_config {
        coap_client_config config;
        config.enable_dtls = true;
        config.cert_file = client_cert_file;
        config.key_file = client_key_file;
        config.ca_file = ca_file;
        config.verify_peer_cert = true;
        config.cipher_suites = std::move(suites);
        return config;
    }

private:
    auto write_cert(const char* name, X509* cert) -> std::string {
        const auto path = (dir / name).string();
        FILE* f = std::fopen(path.c_str(), "w");
        BOOST_REQUIRE(f != nullptr);
        PEM_write_X509(f, cert);
        std::fclose(f);
        return path;
    }

    auto write_key(const char* name, EVP_PKEY* key) -> std::string {
        const auto path = (dir / name).string();
        FILE* f = std::fopen(path.c_str(), "w");
        BOOST_REQUIRE(f != nullptr);
        PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr);
        std::fclose(f);
        return path;
    }
};

// Run one client handshake against a server restricted to `server_suites`.
auto handshake(const test_pki& pki, std::vector<std::string> server_suites,
               std::vector<std::string> client_suites) -> bool {
    test_transport_types::metrics_type server_metrics;
    coap_server<test_transport_types> server(
        "127.0.0.1", 0, pki.server_config(std::move(server_suites)), server_metrics);
    server.start();
    const auto endpoint = std::format("coaps://127.0.0.1:{}", server.bound_port());
    test_transport_types::metrics_type client_metrics;
    coap_client<test_transport_types> client(
        {{1, endpoint}}, pki.client_config(std::move(client_suites)), client_metrics);
    const bool ok =
        client.initiate_dtls_handshake(endpoint) && client.complete_dtls_handshake(endpoint);
    server.stop();
    return ok;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_dtls_cipher_suites_handshake)

BOOST_AUTO_TEST_CASE(matching_restrictions_complete_the_handshake,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    test_pki pki;
    BOOST_TEST(handshake(pki, {aes128}, {aes128}));
}

BOOST_AUTO_TEST_CASE(a_restricted_server_still_serves_an_unrestricted_client,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    test_pki pki;
    BOOST_TEST(handshake(pki, {aes256}, {}));
}

BOOST_AUTO_TEST_CASE(a_restricted_client_still_reaches_an_unrestricted_server,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    test_pki pki;
    BOOST_TEST(handshake(pki, {}, {aes256}));
}

// Before the fix both sides silently ran libcoap's defaults, which share
// both suites, so this handshake succeeded.
BOOST_AUTO_TEST_CASE(disjoint_restrictions_refuse_the_handshake,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    test_pki pki;
    BOOST_TEST(!handshake(pki, {aes128}, {aes256}));
}

BOOST_AUTO_TEST_CASE(an_unknown_suite_fails_construction,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    test_pki pki;
    test_transport_types::metrics_type metrics;
    BOOST_CHECK_THROW(
        coap_client<test_transport_types>({{1, "coaps://127.0.0.1:5684"}},
                                          pki.client_config({"NOT-A-CIPHER"}), metrics),
        coap_security_config_error);
    BOOST_CHECK_THROW(coap_server<test_transport_types>(
                          "127.0.0.1", 0, pki.server_config({"NOT-A-CIPHER"}), metrics),
                      coap_security_config_error);
}

BOOST_AUTO_TEST_SUITE_END()

#else
BOOST_AUTO_TEST_CASE(coap_dtls_cipher_suites_handshake_requires_libcoap) {
    BOOST_TEST_MESSAGE("libcoap not available; only the cipher-list translation was checked");
}
#endif  // LIBCOAP_AVAILABLE
