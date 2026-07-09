// Exercises the real-libcoap (LIBCOAP_AVAILABLE) code paths of every
// coap_security_provider that coap_dtls_rpk_test.cpp / coap_oscore_
// integration_test.cpp don't already cover directly: no_auth_provider and
// dtls_psk_provider/dtls_pki_provider's configure_session()/
// create_client_session() bodies, their identity/CN-validation callbacks
// (made public specifically for this — see coap_security_impl.hpp's
// class-level comments), and oscore_provider's client-role
// configure_session()/add_recipient(). None of these require a live
// two-party handshake to reach: configure_session()/create_client_session()
// just need a real coap_context_t to wire into, and the validation
// callbacks are plain static functions callable with synthetic arguments.
//
// A no-op fallback runs when built without LIBCOAP_AVAILABLE (the default —
// see tests/CMakeLists.txt for how this target opts in).
#define BOOST_TEST_MODULE coap_security_provider_libcoap_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 30

#include <raft/coap_security_impl.hpp>

#ifdef LIBCOAP_AVAILABLE

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

using namespace kythira;

namespace {

// A minimal, self-signed EC certificate + key pair, written to temp files —
// everything dtls_pki_provider::configure_session() needs (it loads
// cert_file/key_file from disk via libcoap/OpenSSL, not from in-memory PEM).
struct self_signed_cert_material {
    std::string cert_file;
    std::string key_file;

    self_signed_cert_material() {
        EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        BOOST_REQUIRE(pctx != nullptr);
        BOOST_REQUIRE(EVP_PKEY_keygen_init(pctx) == 1);
        BOOST_REQUIRE(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) == 1);
        EVP_PKEY* pkey = nullptr;
        BOOST_REQUIRE(EVP_PKEY_keygen(pctx, &pkey) == 1);
        EVP_PKEY_CTX_free(pctx);

        X509* cert = X509_new();
        BOOST_REQUIRE(cert != nullptr);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60L * 24L * 365L);
        X509_set_pubkey(cert, pkey);
        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("coap-security-test"), -1,
                                   -1, 0);
        X509_set_issuer_name(cert, name);
        BOOST_REQUIRE(X509_sign(cert, pkey, EVP_sha256()) > 0);

        auto tmp_dir = std::filesystem::temp_directory_path();
        auto suffix = std::to_string(std::random_device{}());
        cert_file = (tmp_dir / ("coap_security_test_cert_" + suffix + ".pem")).string();
        key_file = (tmp_dir / ("coap_security_test_key_" + suffix + ".pem")).string();

        FILE* cert_fp = std::fopen(cert_file.c_str(), "w");
        BOOST_REQUIRE(cert_fp != nullptr);
        PEM_write_X509(cert_fp, cert);
        std::fclose(cert_fp);

        FILE* key_fp = std::fopen(key_file.c_str(), "w");
        BOOST_REQUIRE(key_fp != nullptr);
        PEM_write_PrivateKey(key_fp, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        std::fclose(key_fp);

        X509_free(cert);
        EVP_PKEY_free(pkey);
    }

    ~self_signed_cert_material() {
        std::error_code ec;
        std::filesystem::remove(cert_file, ec);
        std::filesystem::remove(key_file, ec);
    }
};

auto make_context() -> coap_context_t* {
    coap_startup();
    auto* ctx = coap_new_context(nullptr);
    BOOST_REQUIRE(ctx != nullptr);
    return ctx;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_security_provider_libcoap_tests)

BOOST_AUTO_TEST_CASE(no_auth_provider_creates_real_session, *boost::unit_test::timeout(15)) {
    no_auth_provider provider;
    auto* ctx = make_context();
    BOOST_CHECK_NO_THROW(provider.configure_session(ctx));
    coap_free_context(ctx);
}

BOOST_AUTO_TEST_CASE(psk_provider_configures_client_and_server_sessions,
                     *boost::unit_test::timeout(15)) {
    psk_credentials creds{"node-1", std::vector<std::byte>(16, std::byte{0x42})};

    dtls_psk_provider client_provider(creds, coap_security_role::client);
    auto* client_ctx = make_context();
    BOOST_CHECK_NO_THROW(client_provider.configure_session(client_ctx));
    coap_free_context(client_ctx);

    dtls_psk_provider server_provider(creds, coap_security_role::server);
    auto* server_ctx = make_context();
    BOOST_CHECK_NO_THROW(server_provider.configure_session(server_ctx));
    coap_free_context(server_ctx);
}

BOOST_AUTO_TEST_CASE(psk_identity_callback_matches_and_rejects, *boost::unit_test::timeout(15)) {
    psk_credentials creds{"node-1", std::vector<std::byte>(16, std::byte{0x42})};
    dtls_psk_provider provider(creds, coap_security_role::server);

    coap_bin_const_t matching;
    matching.s = reinterpret_cast<const uint8_t*>("node-1");
    matching.length = 6;
    const auto* result = dtls_psk_provider::validate_id_callback(&matching, nullptr, &provider);
    BOOST_REQUIRE(result != nullptr);
    BOOST_CHECK_EQUAL(result->length, creds.key.size());

    coap_bin_const_t mismatching;
    mismatching.s = reinterpret_cast<const uint8_t*>("node-2");
    mismatching.length = 6;
    BOOST_CHECK(dtls_psk_provider::validate_id_callback(&mismatching, nullptr, &provider) ==
                nullptr);
}

BOOST_AUTO_TEST_CASE(pki_provider_configures_client_and_server_sessions,
                     *boost::unit_test::timeout(15)) {
    self_signed_cert_material material;
    pki_credentials creds{material.cert_file, material.key_file, "", true, {}, nullptr};

    dtls_pki_provider client_provider(creds, coap_security_role::client);
    auto* client_ctx = make_context();
    BOOST_CHECK_NO_THROW(client_provider.configure_session(client_ctx));
    coap_free_context(client_ctx);

    dtls_pki_provider server_provider(creds, coap_security_role::server);
    auto* server_ctx = make_context();
    BOOST_CHECK_NO_THROW(server_provider.configure_session(server_ctx));
    auto* session = server_provider.create_client_session(server_ctx, nullptr, nullptr,
                                                          /*COAP_PROTO_UDP=*/0);
    // No destination address given, so no real session is expected — this
    // just exercises the call path, matching the other providers' pattern.
    (void)session;
    coap_free_context(server_ctx);
}

BOOST_AUTO_TEST_CASE(pki_cn_callback_trusts_tls_result_by_default, *boost::unit_test::timeout(15)) {
    pki_credentials creds{"unused.pem", "unused.pem", "", true, {}, nullptr};
    dtls_pki_provider provider(creds, coap_security_role::server);

    // With no cn_validator configured, the callback must trust whatever
    // `validated` libcoap's own TLS-layer check already produced, without
    // touching the (here, deliberately invalid) ASN.1 bytes.
    BOOST_CHECK_EQUAL(dtls_pki_provider::validate_cn(nullptr, nullptr, 0, nullptr, 0,
                                                     /*validated=*/1, &provider),
                      1);
    BOOST_CHECK_EQUAL(dtls_pki_provider::validate_cn(nullptr, nullptr, 0, nullptr, 0,
                                                     /*validated=*/0, &provider),
                      0);
}

BOOST_AUTO_TEST_CASE(pki_cn_callback_invokes_custom_validator, *boost::unit_test::timeout(15)) {
    self_signed_cert_material material;
    bool validator_called = false;
    pki_credentials creds{
        material.cert_file, material.key_file, "", true, {}, [&](const std::string& pem) {
            validator_called = true;
            return pem.find("BEGIN CERTIFICATE") != std::string::npos;
        }};
    dtls_pki_provider provider(creds, coap_security_role::server);

    // Build a real DER certificate (read back the one we just generated) so
    // the callback's ASN.1-to-PEM conversion has valid input to work with.
    std::ifstream cert_stream(material.cert_file, std::ios::binary);
    std::string cert_pem((std::istreambuf_iterator<char>(cert_stream)),
                         std::istreambuf_iterator<char>());
    BIO* bio = BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size()));
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BOOST_REQUIRE(cert != nullptr);
    BIO_free(bio);
    int der_len = i2d_X509(cert, nullptr);
    BOOST_REQUIRE(der_len > 0);
    std::vector<uint8_t> der(static_cast<std::size_t>(der_len));
    auto* der_ptr = der.data();
    i2d_X509(cert, &der_ptr);
    X509_free(cert);

    int result = dtls_pki_provider::validate_cn(nullptr, der.data(), der.size(), nullptr, 0,
                                                /*validated=*/1, &provider);
    BOOST_CHECK_EQUAL(result, 1);
    BOOST_CHECK(validator_called);
}

BOOST_AUTO_TEST_CASE(rpk_create_client_session_and_callback, *boost::unit_test::timeout(15)) {
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    BOOST_REQUIRE(EVP_PKEY_keygen_init(pctx) == 1);
    BOOST_REQUIRE(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) == 1);
    EVP_PKEY* pkey = nullptr;
    BOOST_REQUIRE(EVP_PKEY_keygen(pctx, &pkey) == 1);
    EVP_PKEY_CTX_free(pctx);

    BIO* pub_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(pub_bio, pkey);
    char* pub_data = nullptr;
    long pub_len = BIO_get_mem_data(pub_bio, &pub_data);
    std::vector<std::byte> pub_pem(static_cast<std::size_t>(pub_len));
    std::memcpy(pub_pem.data(), pub_data, static_cast<std::size_t>(pub_len));
    BIO_free(pub_bio);

    int der_len = i2d_PUBKEY(pkey, nullptr);
    std::vector<std::byte> pub_der(static_cast<std::size_t>(der_len));
    auto* der_ptr = reinterpret_cast<unsigned char*>(pub_der.data());
    i2d_PUBKEY(pkey, &der_ptr);
    EVP_PKEY_free(pkey);

    rpk_credentials creds{pub_pem, pub_pem, {pub_der}};
    dtls_rpk_provider provider(creds, coap_security_role::client);
    auto* ctx = make_context();
    auto* session = provider.create_client_session(ctx, nullptr, nullptr, /*COAP_PROTO_UDP=*/0);
    (void)session;

    BOOST_CHECK_EQUAL(dtls_rpk_provider::validate_peer_key(
                          nullptr, reinterpret_cast<const uint8_t*>(pub_der.data()), pub_der.size(),
                          nullptr, 0, 0, &provider),
                      1);
    coap_free_context(ctx);
}

BOOST_AUTO_TEST_CASE(oscore_client_role_configure_session_is_noop_and_add_recipient_works,
                     *boost::unit_test::timeout(15)) {
    oscore_credentials creds;
    creds.sender_id = {std::byte{0x00}};
    creds.recipient_id = {std::byte{0x01}};
    creds.master_secret = std::vector<std::byte>(16, std::byte{0x55});

    oscore_provider client_provider(creds, coap_security_role::client);
    auto* client_ctx = make_context();
    BOOST_CHECK_NO_THROW(client_provider.configure_session(client_ctx));
    coap_free_context(client_ctx);

    oscore_provider server_provider(creds, coap_security_role::server);
    auto* server_ctx = make_context();
    server_provider.configure_session(server_ctx);
    std::vector<std::byte> extra_peer = {std::byte{0x02}};
    BOOST_CHECK_NO_THROW(server_provider.add_recipient(server_ctx, extra_peer));
    coap_free_context(server_ctx);
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(coap_security_provider_libcoap_test_requires_libcoap) {
    BOOST_TEST_MESSAGE(
        "coap_security_provider_libcoap_test built without LIBCOAP_AVAILABLE; skipping "
        "real-libcoap provider smoke tests (see tests/CMakeLists.txt).");
}

#endif  // LIBCOAP_AVAILABLE
