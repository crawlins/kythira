// **Feature: coap-transport-security, Requirement 8.3, 9.8**
// translate_legacy_fields() reproduces today's field-inference behavior
// exactly when security.mode is left at its default, and rejects
// inconsistent legacy-field-plus-explicit-mode configurations
// (Requirement 2.3).
#define BOOST_TEST_MODULE coap_legacy_config_migration_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 30

#include <raft/coap_security_impl.hpp>

using namespace kythira;

BOOST_AUTO_TEST_SUITE(coap_legacy_config_migration_tests)

BOOST_AUTO_TEST_CASE(no_legacy_fields_and_no_explicit_mode_yields_none,
                     *boost::unit_test::timeout(10)) {
    coap_client_config cfg;
    auto security = translate_legacy_fields(cfg);
    BOOST_CHECK(security.mode == coap_auth_mode::none);
    BOOST_CHECK(std::holds_alternative<std::monostate>(security.credentials));
}

BOOST_AUTO_TEST_CASE(legacy_cert_file_infers_dtls_pki, *boost::unit_test::timeout(10)) {
    coap_client_config cfg;
    cfg.cert_file = "/tmp/node.crt";
    cfg.key_file = "/tmp/node.key";
    cfg.ca_file = "/tmp/ca.crt";
    cfg.verify_peer_cert = true;
    cfg.cipher_suites = {"TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256"};

    auto security = translate_legacy_fields(cfg);
    BOOST_CHECK(security.mode == coap_auth_mode::dtls_pki);
    BOOST_REQUIRE(std::holds_alternative<pki_credentials>(security.credentials));
    const auto& creds = std::get<pki_credentials>(security.credentials);
    BOOST_CHECK_EQUAL(creds.cert_file, cfg.cert_file);
    BOOST_CHECK_EQUAL(creds.key_file, cfg.key_file);
    BOOST_CHECK_EQUAL(creds.ca_file, cfg.ca_file);
    BOOST_CHECK_EQUAL(creds.verify_peer_cert, cfg.verify_peer_cert);
    BOOST_CHECK_EQUAL_COLLECTIONS(creds.cipher_suites.begin(), creds.cipher_suites.end(),
                                  cfg.cipher_suites.begin(), cfg.cipher_suites.end());

    // Equivalent provider type to what the new explicit-mode config selects.
    auto provider = make_security_provider(security, coap_security_role::client);
    BOOST_CHECK(dynamic_cast<dtls_pki_provider*>(provider.get()) != nullptr);
}

BOOST_AUTO_TEST_CASE(legacy_psk_identity_infers_dtls_psk, *boost::unit_test::timeout(10)) {
    coap_client_config cfg;
    cfg.psk_identity = "node-42";
    cfg.psk_key = std::vector<std::byte>(16, std::byte{0x7A});

    auto security = translate_legacy_fields(cfg);
    BOOST_CHECK(security.mode == coap_auth_mode::dtls_psk);
    BOOST_REQUIRE(std::holds_alternative<psk_credentials>(security.credentials));
    const auto& creds = std::get<psk_credentials>(security.credentials);
    BOOST_CHECK_EQUAL(creds.identity, cfg.psk_identity);
    BOOST_CHECK(creds.key == cfg.psk_key);

    auto provider = make_security_provider(security, coap_security_role::client);
    BOOST_CHECK(dynamic_cast<dtls_psk_provider*>(provider.get()) != nullptr);
}

BOOST_AUTO_TEST_CASE(explicit_mode_with_empty_legacy_fields_passes_through,
                     *boost::unit_test::timeout(10)) {
    coap_client_config cfg;
    cfg.security.mode = coap_auth_mode::dtls_rpk;
    cfg.security.credentials = rpk_credentials{std::vector<std::byte>(32, std::byte{0x01}),
                                               std::vector<std::byte>(32, std::byte{0x02}),
                                               {}};

    auto security = translate_legacy_fields(cfg);
    BOOST_CHECK(security.mode == coap_auth_mode::dtls_rpk);
    BOOST_CHECK(std::holds_alternative<rpk_credentials>(security.credentials));
}

BOOST_AUTO_TEST_CASE(explicit_mode_plus_legacy_cert_file_throws, *boost::unit_test::timeout(10)) {
    coap_client_config cfg;
    cfg.cert_file = "/tmp/node.crt";  // legacy field populated...
    cfg.key_file = "/tmp/node.key";
    cfg.security.mode = coap_auth_mode::dtls_pki;  // ...alongside an explicit mode.
    cfg.security.credentials =
        pki_credentials{"/tmp/other.crt", "/tmp/other.key", "", true, {}, nullptr};

    BOOST_CHECK_THROW(translate_legacy_fields(cfg), coap_security_config_error);
}

BOOST_AUTO_TEST_CASE(explicit_mode_plus_legacy_psk_identity_throws,
                     *boost::unit_test::timeout(10)) {
    coap_client_config cfg;
    cfg.psk_identity = "node-1";
    cfg.psk_key = std::vector<std::byte>(16, std::byte{0x11});
    cfg.security.mode = coap_auth_mode::dtls_pki;
    cfg.security.credentials =
        pki_credentials{"/tmp/other.crt", "/tmp/other.key", "", true, {}, nullptr};

    BOOST_CHECK_THROW(translate_legacy_fields(cfg), coap_security_config_error);
}

BOOST_AUTO_TEST_CASE(server_config_legacy_translation_matches_client,
                     *boost::unit_test::timeout(10)) {
    coap_server_config cfg;
    cfg.psk_identity = "server-1";
    cfg.psk_key = std::vector<std::byte>(20, std::byte{0x33});

    auto security = translate_legacy_fields(cfg);
    BOOST_CHECK(security.mode == coap_auth_mode::dtls_psk);
    BOOST_REQUIRE(std::holds_alternative<psk_credentials>(security.credentials));
    BOOST_CHECK_EQUAL(std::get<psk_credentials>(security.credentials).identity, cfg.psk_identity);
}

BOOST_AUTO_TEST_SUITE_END()
