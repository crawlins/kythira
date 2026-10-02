// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "test_timeout_scale.hpp"
// **Feature: coap-transport-security, Requirement 9.3**
// RPK peer-key match succeeds; mismatch is rejected (Requirement 3.2).
// dtls_rpk_provider::is_trusted_peer_key() is exercised directly here. The
// live DTLS-RPK handshake, with Raft RPCs over it and both directions of
// untrusted key refused, is in coap_dtls_raft_rpc_test.cpp. When built with
// LIBCOAP_AVAILABLE (see tests/CMakeLists.txt), the last case checks that
// configure_session() wires real RPK material into a live coap_context_t
// when the linked libcoap supports RPK, and refuses it when it does not.
#define BOOST_TEST_MODULE coap_dtls_rpk_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT (30 * KYTHIRA_TEST_TIMEOUT_SCALE)

#include <raft/coap_security_impl.hpp>

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

using namespace kythira;

namespace {

struct ec_keypair {
    std::vector<std::byte> public_key_pem;
    std::vector<std::byte> private_key_pem;
    std::vector<std::byte> public_key_der;  // SubjectPublicKeyInfo, DER
};

auto generate_ec_keypair() -> ec_keypair {
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    BOOST_REQUIRE(pctx != nullptr);
    BOOST_REQUIRE(EVP_PKEY_keygen_init(pctx) == 1);
    BOOST_REQUIRE(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) == 1);
    EVP_PKEY* pkey = nullptr;
    BOOST_REQUIRE(EVP_PKEY_keygen(pctx, &pkey) == 1);
    EVP_PKEY_CTX_free(pctx);

    ec_keypair result;

    BIO* pub_bio = BIO_new(BIO_s_mem());
    BOOST_REQUIRE(PEM_write_bio_PUBKEY(pub_bio, pkey) == 1);
    char* pub_data = nullptr;
    long pub_len = BIO_get_mem_data(pub_bio, &pub_data);
    result.public_key_pem.resize(static_cast<std::size_t>(pub_len));
    std::memcpy(result.public_key_pem.data(), pub_data, static_cast<std::size_t>(pub_len));
    BIO_free(pub_bio);

    BIO* priv_bio = BIO_new(BIO_s_mem());
    BOOST_REQUIRE(PEM_write_bio_PrivateKey(priv_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) ==
                  1);
    char* priv_data = nullptr;
    long priv_len = BIO_get_mem_data(priv_bio, &priv_data);
    result.private_key_pem.resize(static_cast<std::size_t>(priv_len));
    std::memcpy(result.private_key_pem.data(), priv_data, static_cast<std::size_t>(priv_len));
    BIO_free(priv_bio);

    int der_len = i2d_PUBKEY(pkey, nullptr);
    BOOST_REQUIRE(der_len > 0);
    result.public_key_der.resize(static_cast<std::size_t>(der_len));
    auto* der_ptr = reinterpret_cast<unsigned char*>(result.public_key_der.data());
    i2d_PUBKEY(pkey, &der_ptr);

    EVP_PKEY_free(pkey);
    return result;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_dtls_rpk_tests)

BOOST_AUTO_TEST_CASE(peer_key_in_trust_set_is_accepted,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto local = generate_ec_keypair();
    auto peer = generate_ec_keypair();

    rpk_credentials creds;
    creds.public_key = local.public_key_pem;
    creds.private_key = local.private_key_pem;
    creds.trusted_peer_keys = {peer.public_key_der};

    dtls_rpk_provider provider(creds, coap_security_role::client);
    BOOST_CHECK(provider.is_trusted_peer_key(peer.public_key_der));
}

BOOST_AUTO_TEST_CASE(peer_key_not_in_trust_set_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto local = generate_ec_keypair();
    auto trusted_peer = generate_ec_keypair();
    auto untrusted_peer = generate_ec_keypair();

    rpk_credentials creds;
    creds.public_key = local.public_key_pem;
    creds.private_key = local.private_key_pem;
    creds.trusted_peer_keys = {trusted_peer.public_key_der};

    dtls_rpk_provider provider(creds, coap_security_role::client);
    BOOST_CHECK(!provider.is_trusted_peer_key(untrusted_peer.public_key_der));
}

BOOST_AUTO_TEST_CASE(empty_trust_set_rejects_every_peer,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto local = generate_ec_keypair();
    auto peer = generate_ec_keypair();

    rpk_credentials creds;
    creds.public_key = local.public_key_pem;
    creds.private_key = local.private_key_pem;
    // trusted_peer_keys left empty.

    dtls_rpk_provider provider(creds, coap_security_role::client);
    BOOST_CHECK(!provider.is_trusted_peer_key(peer.public_key_der));
}

BOOST_AUTO_TEST_CASE(mode_selection_via_factory,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto local = generate_ec_keypair();
    coap_security_config config;
    config.mode = coap_auth_mode::dtls_rpk;
    config.credentials = rpk_credentials{local.public_key_pem, local.private_key_pem, {}};
    auto provider = make_security_provider(config, coap_security_role::server);
    BOOST_CHECK(provider->mode() == coap_auth_mode::dtls_rpk);
}

#ifdef LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(configure_session_wires_rpk_material_only_when_supported,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto local = generate_ec_keypair();
    auto peer = generate_ec_keypair();

    rpk_credentials creds;
    creds.public_key = local.public_key_pem;
    creds.private_key = local.private_key_pem;
    creds.trusted_peer_keys = {peer.public_key_der};

    dtls_rpk_provider provider(creds, coap_security_role::server);

    coap_startup();
    coap_context_t* ctx = coap_new_context(nullptr);
    BOOST_REQUIRE(ctx != nullptr);
    if (coap_dtls_rpk_is_supported() != 0) {
        BOOST_CHECK_NO_THROW(provider.configure_session(ctx));
    } else {
        // libcoap's OpenSSL backend: an RPK context would fail every
        // handshake, so it must not be built at all.
        BOOST_CHECK_THROW(provider.configure_session(ctx), coap_unsupported_security_mode_error);
    }
    coap_free_context(ctx);
}

#endif  // LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_SUITE_END()
