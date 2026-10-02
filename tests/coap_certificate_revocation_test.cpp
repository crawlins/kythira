// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * Certificate revocation checking for CoAP DTLS peers (coap-transport
 * Requirements 6.5 and 11.3).
 *
 * The certificate validators used to look for a CRL distribution point and
 * an OCSP responder in the peer's certificate, log that they were there,
 * and accept the certificate regardless. These cases build a throwaway CA, a
 * good and a revoked client certificate and a CRL listing the revoked one,
 * then check every layer that should now refuse it: the shared
 * coap_revocation::check(), both legacy validators, the dtls_pki_provider
 * callback, and finally a real DTLS handshake against a coap_server.
 */
#define BOOST_TEST_MODULE coap_certificate_revocation_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>

#ifdef LIBCOAP_AVAILABLE
#include <raft/coap_revocation.hpp>
#include <raft/coap_security_impl.hpp>

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

using test_transport_types =
    kythira::default_transport_types<kythira::future_default<kythira::request_vote_response<>>,
                                     kythira::json_rpc_serializer<std::vector<std::byte>>,
                                     kythira::noop_metrics, kythira::console_logger>;

#ifdef LIBCOAP_AVAILABLE
namespace {

auto make_key() -> EVP_PKEY* {
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    BOOST_REQUIRE(pctx != nullptr);
    BOOST_REQUIRE(EVP_PKEY_keygen_init(pctx) == 1);
    BOOST_REQUIRE(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) == 1);
    EVP_PKEY* key = nullptr;
    BOOST_REQUIRE(EVP_PKEY_keygen(pctx, &key) == 1);
    EVP_PKEY_CTX_free(pctx);
    return key;
}

auto add_extension(X509* cert, X509* issuer, int nid, const char* value) -> void {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    BOOST_REQUIRE(ext != nullptr);
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
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
        add_extension(cert, cert, NID_basic_constraints, "critical,CA:TRUE");
        add_extension(cert, cert, NID_key_usage, "critical,keyCertSign,cRLSign");
    }
    BOOST_REQUIRE(X509_sign(cert, issuer_key, EVP_sha256()) > 0);
    return cert;
}

auto make_crl(X509* ca, EVP_PKEY* ca_key, const std::vector<long>& revoked_serials) -> X509_CRL* {
    X509_CRL* crl = X509_CRL_new();
    BOOST_REQUIRE(crl != nullptr);
    X509_CRL_set_version(crl, 1);
    X509_CRL_set_issuer_name(crl, X509_get_subject_name(ca));
    ASN1_TIME* last = ASN1_TIME_adj(nullptr, std::time(nullptr), 0, -60);
    ASN1_TIME* next = ASN1_TIME_adj(nullptr, std::time(nullptr), 1, 0);
    X509_CRL_set1_lastUpdate(crl, last);
    X509_CRL_set1_nextUpdate(crl, next);
    for (long serial : revoked_serials) {
        X509_REVOKED* entry = X509_REVOKED_new();
        ASN1_INTEGER* number = ASN1_INTEGER_new();
        ASN1_INTEGER_set(number, serial);
        X509_REVOKED_set_serialNumber(entry, number);
        X509_REVOKED_set_revocationDate(entry, last);
        X509_CRL_add0_revoked(crl, entry);
        ASN1_INTEGER_free(number);
    }
    ASN1_TIME_free(last);
    ASN1_TIME_free(next);
    X509_CRL_sort(crl);
    BOOST_REQUIRE(X509_CRL_sign(crl, ca_key, EVP_sha256()) > 0);
    return crl;
}

auto pem_of(X509* cert) -> std::string {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    std::string pem(data, static_cast<std::size_t>(len));
    BIO_free(bio);
    return pem;
}

auto der_of(X509* cert) -> std::vector<std::uint8_t> {
    unsigned char* der = nullptr;
    const int len = i2d_X509(cert, &der);
    std::vector<std::uint8_t> bytes(der, der + len);
    OPENSSL_free(der);
    return bytes;
}

// One CA, a server certificate, a good and a revoked client certificate, a
// CRL revoking the latter, and an unrelated CA's CRL, all on disk.
struct revocation_pki {
    std::filesystem::path dir;
    std::string ca_file, crl_file, foreign_crl_file;
    std::string server_cert_file, server_key_file;
    std::string good_cert_file, good_key_file;
    std::string revoked_cert_file, revoked_key_file;
    X509* good_cert = nullptr;
    X509* revoked_cert = nullptr;

    revocation_pki() {
        dir = std::filesystem::temp_directory_path() /
              ("coap_revocation_test_" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(dir);

        EVP_PKEY* ca_key = make_key();
        X509* ca = make_cert("kythira-test-ca", 1, ca_key, nullptr, ca_key);
        EVP_PKEY* server_key = make_key();
        X509* server = make_cert("localhost", 2, server_key, ca, ca_key);
        EVP_PKEY* good_key = make_key();
        good_cert = make_cert("good-client", 3, good_key, ca, ca_key);
        EVP_PKEY* revoked_key = make_key();
        revoked_cert = make_cert("revoked-client", 4, revoked_key, ca, ca_key);
        X509_CRL* crl = make_crl(ca, ca_key, {4});

        EVP_PKEY* foreign_key = make_key();
        X509* foreign_ca = make_cert("unrelated-ca", 1, foreign_key, nullptr, foreign_key);
        X509_CRL* foreign_crl = make_crl(foreign_ca, foreign_key, {});

        ca_file = write("ca.pem", [&](FILE* f) { PEM_write_X509(f, ca); });
        crl_file = write("crl.pem", [&](FILE* f) { PEM_write_X509_CRL(f, crl); });
        foreign_crl_file =
            write("foreign_crl.pem", [&](FILE* f) { PEM_write_X509_CRL(f, foreign_crl); });
        server_cert_file = write("server.pem", [&](FILE* f) { PEM_write_X509(f, server); });
        server_key_file = write_key("server.key", server_key);
        good_cert_file = write("good.pem", [&](FILE* f) { PEM_write_X509(f, good_cert); });
        good_key_file = write_key("good.key", good_key);
        revoked_cert_file = write("revoked.pem", [&](FILE* f) { PEM_write_X509(f, revoked_cert); });
        revoked_key_file = write_key("revoked.key", revoked_key);

        X509_CRL_free(foreign_crl);
        X509_free(foreign_ca);
        EVP_PKEY_free(foreign_key);
        X509_CRL_free(crl);
        EVP_PKEY_free(revoked_key);
        EVP_PKEY_free(good_key);
        X509_free(server);
        EVP_PKEY_free(server_key);
        X509_free(ca);
        EVP_PKEY_free(ca_key);
    }

    ~revocation_pki() {
        X509_free(good_cert);
        X509_free(revoked_cert);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    revocation_pki(const revocation_pki&) = delete;
    auto operator=(const revocation_pki&) -> revocation_pki& = delete;

    [[nodiscard]] auto checking(std::string crl = {}, bool allow_missing = false) const
        -> certificate_revocation_config {
        return certificate_revocation_config{true, std::move(crl), allow_missing};
    }

private:
    template<typename Writer> auto write(const char* name, Writer&& writer) -> std::string {
        const auto path = (dir / name).string();
        FILE* f = std::fopen(path.c_str(), "w");
        BOOST_REQUIRE(f != nullptr);
        writer(f);
        std::fclose(f);
        return path;
    }

    auto write_key(const char* name, EVP_PKEY* key) -> std::string {
        return write(name, [&](FILE* f) {
            PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr);
        });
    }
};

auto pki_server_config(const revocation_pki& pki) -> coap_server_config {
    coap_server_config config;
    config.enable_dtls = true;
    config.cert_file = pki.server_cert_file;
    config.key_file = pki.server_key_file;
    config.ca_file = pki.ca_file;
    config.verify_peer_cert = true;
    config.revocation = pki.checking(pki.crl_file);
    return config;
}

auto pki_client_config(const revocation_pki& pki, const std::string& cert, const std::string& key)
    -> coap_client_config {
    coap_client_config config;
    config.enable_dtls = true;
    config.cert_file = cert;
    config.key_file = key;
    config.ca_file = pki.ca_file;
    config.verify_peer_cert = true;
    return config;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_revocation_check)

BOOST_AUTO_TEST_CASE(revoked_certificate_is_rejected) {
    revocation_pki pki;
    const auto failure =
        coap_revocation::check(pki.revoked_cert, pki.ca_file, pki.checking(pki.crl_file));
    BOOST_REQUIRE(failure.has_value());
    BOOST_TEST(*failure == "certificate revoked");
}

BOOST_AUTO_TEST_CASE(good_certificate_passes) {
    revocation_pki pki;
    BOOST_TEST(!coap_revocation::check(pki.good_cert, pki.ca_file, pki.checking(pki.crl_file)));
}

BOOST_AUTO_TEST_CASE(disabled_check_passes_even_a_revoked_certificate) {
    revocation_pki pki;
    auto config = pki.checking(pki.crl_file);
    config.enabled = false;
    BOOST_TEST(!coap_revocation::check(pki.revoked_cert, pki.ca_file, config));
}

// Fail closed: no CRL for the issuer is a rejection unless explicitly allowed.
BOOST_AUTO_TEST_CASE(missing_crl_rejects_unless_allowed) {
    revocation_pki pki;
    BOOST_TEST(coap_revocation::check(pki.good_cert, pki.ca_file, pki.checking()).has_value());
    BOOST_TEST(!coap_revocation::check(pki.good_cert, pki.ca_file, pki.checking({}, true)));
}

// A CRL from some other CA says nothing about this certificate's issuer.
BOOST_AUTO_TEST_CASE(crl_from_another_issuer_does_not_count) {
    revocation_pki pki;
    BOOST_TEST(
        coap_revocation::check(pki.good_cert, pki.ca_file, pki.checking(pki.foreign_crl_file))
            .has_value());
}

BOOST_AUTO_TEST_CASE(unreadable_crl_file_rejects) {
    revocation_pki pki;
    const auto failure = coap_revocation::check(pki.good_cert, pki.ca_file,
                                                pki.checking((pki.dir / "absent.pem").string()));
    BOOST_REQUIRE(failure.has_value());
    BOOST_TEST(failure->find("failed to load CRL file") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(missing_ca_file_rejects) {
    revocation_pki pki;
    BOOST_TEST(coap_revocation::check(pki.good_cert, "", pki.checking(pki.crl_file)).has_value());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(coap_revocation_wiring)

BOOST_AUTO_TEST_CASE(client_validator_rejects_revoked_server_certificate) {
    revocation_pki pki;
    auto config = pki_client_config(pki, pki.good_cert_file, pki.good_key_file);
    config.revocation = pki.checking(pki.crl_file);
    test_transport_types::metrics_type metrics;
    coap_client<test_transport_types> client({}, config, metrics);

    BOOST_TEST(client.validate_peer_certificate(pem_of(pki.good_cert)));
    BOOST_CHECK_THROW(client.validate_peer_certificate(pem_of(pki.revoked_cert)),
                      coap_security_error);
}

BOOST_AUTO_TEST_CASE(server_validator_rejects_revoked_client_certificate) {
    revocation_pki pki;
    test_transport_types::metrics_type metrics;
    coap_server<test_transport_types> server("127.0.0.1", 0, pki_server_config(pki), metrics);

    BOOST_TEST(server.validate_client_certificate(pem_of(pki.good_cert)));
    BOOST_CHECK_THROW(server.validate_client_certificate(pem_of(pki.revoked_cert)),
                      coap_security_error);
}

BOOST_AUTO_TEST_CASE(pki_provider_callback_rejects_revoked_certificate) {
    revocation_pki pki;
    pki_credentials creds;
    creds.cert_file = pki.server_cert_file;
    creds.key_file = pki.server_key_file;
    creds.ca_file = pki.ca_file;
    creds.revocation = pki.checking(pki.crl_file);
    dtls_pki_provider provider(creds, coap_security_role::server);

    const auto good = der_of(pki.good_cert);
    const auto revoked = der_of(pki.revoked_cert);
    BOOST_TEST(dtls_pki_provider::validate_cn("good-client", good.data(), good.size(), nullptr, 0,
                                              1, &provider) == 1);
    BOOST_TEST(dtls_pki_provider::validate_cn("revoked-client", revoked.data(), revoked.size(),
                                              nullptr, 0, 1, &provider) == 0);
    // Revocation adds to libcoap's verdict; it never overturns a rejection.
    BOOST_TEST(dtls_pki_provider::validate_cn("good-client", good.data(), good.size(), nullptr, 0,
                                              0, &provider) == 0);
}

// translate_legacy_fields() is what carries the legacy config's revocation
// settings into an explicit-mode provider.
BOOST_AUTO_TEST_CASE(legacy_fields_carry_revocation_into_pki_credentials) {
    coap_client_config config;
    config.cert_file = "cert.pem";
    config.key_file = "key.pem";
    config.ca_file = "ca.pem";
    config.revocation = certificate_revocation_config{true, "crl.pem", true};
    const auto security = translate_legacy_fields(config);
    const auto& creds = std::get<pki_credentials>(security.credentials);
    BOOST_TEST(creds.revocation.enabled);
    BOOST_TEST(creds.revocation.crl_file == "crl.pem");
    BOOST_TEST(creds.revocation.allow_missing_crl);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(coap_revocation_handshake)

// The server checks client certificates against the CRL during the DTLS
// handshake itself, so a revoked client never gets a session.
BOOST_AUTO_TEST_CASE(server_refuses_handshake_from_revoked_client,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    revocation_pki pki;
    test_transport_types::metrics_type server_metrics;
    coap_server<test_transport_types> server("127.0.0.1", 0, pki_server_config(pki),
                                             server_metrics);
    server.start();
    const auto endpoint = std::format("coaps://127.0.0.1:{}", server.bound_port());

    {
        test_transport_types::metrics_type metrics;
        coap_client<test_transport_types> good(
            {{1, endpoint}}, pki_client_config(pki, pki.good_cert_file, pki.good_key_file),
            metrics);
        BOOST_TEST(good.initiate_dtls_handshake(endpoint));
        BOOST_TEST(good.complete_dtls_handshake(endpoint));
    }
    {
        test_transport_types::metrics_type metrics;
        coap_client<test_transport_types> revoked(
            {{1, endpoint}}, pki_client_config(pki, pki.revoked_cert_file, pki.revoked_key_file),
            metrics);
        BOOST_TEST(revoked.initiate_dtls_handshake(endpoint));
        BOOST_TEST(!revoked.complete_dtls_handshake(endpoint));
    }

    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()

#else
BOOST_AUTO_TEST_CASE(coap_certificate_revocation_test_requires_libcoap) {
    BOOST_TEST_MESSAGE("libcoap not available; revocation checks are part of its DTLS path");
}
#endif  // LIBCOAP_AVAILABLE
