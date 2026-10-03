// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A throwaway CA for CoAP revocation tests: a server certificate, a good and
// a revoked client certificate, a CRL listing the revoked one, and an
// unrelated CA's CRL, all on disk. Shared by every backend's revocation
// suite so there is one certificate generator in the tree.

#include <raft/coap_security.hpp>

#include <boost/test/unit_test.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace kythira::testing::revocation {

inline auto make_key() -> EVP_PKEY* {
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    BOOST_REQUIRE(pctx != nullptr);
    BOOST_REQUIRE(EVP_PKEY_keygen_init(pctx) == 1);
    BOOST_REQUIRE(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) == 1);
    EVP_PKEY* key = nullptr;
    BOOST_REQUIRE(EVP_PKEY_keygen(pctx, &key) == 1);
    EVP_PKEY_CTX_free(pctx);
    return key;
}

inline auto add_extension(X509* cert, X509* issuer, int nid, const char* value) -> void {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    BOOST_REQUIRE(ext != nullptr);
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
}

inline auto make_cert(const char* common_name, long serial, EVP_PKEY* subject_key, X509* issuer,
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

inline auto make_crl(X509* ca, EVP_PKEY* ca_key, const std::vector<long>& revoked_serials)
    -> X509_CRL* {
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

inline auto pem_of(X509* cert) -> std::string {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    std::string pem(data, static_cast<std::size_t>(len));
    BIO_free(bio);
    return pem;
}

inline auto der_of(X509* cert) -> std::vector<std::uint8_t> {
    unsigned char* der = nullptr;
    const int len = i2d_X509(cert, &der);
    std::vector<std::uint8_t> bytes(der, der + len);
    OPENSSL_free(der);
    return bytes;
}

// One CA, a good and a revoked server certificate, a good and a revoked
// client certificate, a CRL revoking the two revoked ones, and an unrelated
// CA's CRL, all on disk.
struct revocation_pki {
    std::filesystem::path dir;
    std::string ca_file, crl_file, foreign_crl_file;
    std::string server_cert_file, server_key_file;
    std::string revoked_server_cert_file, revoked_server_key_file;
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
        EVP_PKEY* revoked_server_key = make_key();
        X509* revoked_server = make_cert("revoked-server", 5, revoked_server_key, ca, ca_key);
        X509_CRL* crl = make_crl(ca, ca_key, {4, 5});

        EVP_PKEY* foreign_key = make_key();
        X509* foreign_ca = make_cert("unrelated-ca", 1, foreign_key, nullptr, foreign_key);
        X509_CRL* foreign_crl = make_crl(foreign_ca, foreign_key, {});

        ca_file = write("ca.pem", [&](FILE* f) { PEM_write_X509(f, ca); });
        crl_file = write("crl.pem", [&](FILE* f) { PEM_write_X509_CRL(f, crl); });
        foreign_crl_file =
            write("foreign_crl.pem", [&](FILE* f) { PEM_write_X509_CRL(f, foreign_crl); });
        server_cert_file = write("server.pem", [&](FILE* f) { PEM_write_X509(f, server); });
        server_key_file = write_key("server.key", server_key);
        revoked_server_cert_file =
            write("revoked_server.pem", [&](FILE* f) { PEM_write_X509(f, revoked_server); });
        revoked_server_key_file = write_key("revoked_server.key", revoked_server_key);
        good_cert_file = write("good.pem", [&](FILE* f) { PEM_write_X509(f, good_cert); });
        good_key_file = write_key("good.key", good_key);
        revoked_cert_file = write("revoked.pem", [&](FILE* f) { PEM_write_X509(f, revoked_cert); });
        revoked_key_file = write_key("revoked.key", revoked_key);

        X509_CRL_free(foreign_crl);
        X509_free(foreign_ca);
        EVP_PKEY_free(foreign_key);
        X509_CRL_free(crl);
        X509_free(revoked_server);
        EVP_PKEY_free(revoked_server_key);
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

}  // namespace kythira::testing::revocation
