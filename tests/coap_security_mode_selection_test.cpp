// **Feature: coap-transport-security, Requirement 9.1, 9.2**
// Each of the five coap_auth_mode values selects the correct
// coap_security_provider implementation; inconsistent mode/credential
// combinations fail construction with coap_security_config_error
// (Requirement 1.3).
#define BOOST_TEST_MODULE coap_security_mode_selection_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 30

#include <raft/coap_security_impl.hpp>

using namespace kythira;

BOOST_AUTO_TEST_SUITE(coap_security_mode_selection_tests)

BOOST_AUTO_TEST_CASE(none_mode_selects_no_auth_provider, *boost::unit_test::timeout(10)) {
    coap_security_config config;
    config.mode = coap_auth_mode::none;
    auto provider = make_security_provider(config, coap_security_role::client);
    BOOST_REQUIRE(provider != nullptr);
    BOOST_CHECK(provider->mode() == coap_auth_mode::none);
    BOOST_CHECK(dynamic_cast<no_auth_provider*>(provider.get()) != nullptr);
}

BOOST_AUTO_TEST_CASE(none_provider_hooks_are_identity, *boost::unit_test::timeout(10)) {
    coap_security_config config;
    config.mode = coap_auth_mode::none;
    auto provider = make_security_provider(config, coap_security_role::client);
    BOOST_CHECK_NO_THROW(provider->configure_session(nullptr));
    auto* pdu = reinterpret_cast<coap_pdu_t*>(0x1234);
    BOOST_CHECK_EQUAL(provider->protect(pdu), pdu);
    BOOST_CHECK_EQUAL(provider->unprotect(pdu), pdu);
}

BOOST_AUTO_TEST_CASE(dtls_psk_mode_selects_dtls_psk_provider, *boost::unit_test::timeout(10)) {
    coap_security_config config;
    config.mode = coap_auth_mode::dtls_psk;
    config.credentials = psk_credentials{"node-1", std::vector<std::byte>(16, std::byte{0x42})};
    auto provider = make_security_provider(config, coap_security_role::client);
    BOOST_REQUIRE(provider != nullptr);
    BOOST_CHECK(provider->mode() == coap_auth_mode::dtls_psk);
    auto* psk_provider = dynamic_cast<dtls_psk_provider*>(provider.get());
    BOOST_REQUIRE(psk_provider != nullptr);
    // credentials() round-trips the exact struct passed in — confirms the
    // factory doesn't copy/reconstruct it lossily along the way.
    BOOST_CHECK_EQUAL(psk_provider->credentials().identity, "node-1");
    BOOST_CHECK_EQUAL(psk_provider->credentials().key.size(), 16u);
}

BOOST_AUTO_TEST_CASE(dtls_pki_mode_selects_dtls_pki_provider, *boost::unit_test::timeout(10)) {
    coap_security_config config;
    config.mode = coap_auth_mode::dtls_pki;
    config.credentials = pki_credentials{"/tmp/cert.pem", "/tmp/key.pem", "", true, {}, nullptr};
    auto provider = make_security_provider(config, coap_security_role::client);
    BOOST_REQUIRE(provider != nullptr);
    BOOST_CHECK(provider->mode() == coap_auth_mode::dtls_pki);
    auto* pki_provider = dynamic_cast<dtls_pki_provider*>(provider.get());
    BOOST_REQUIRE(pki_provider != nullptr);
    BOOST_CHECK_EQUAL(pki_provider->credentials().cert_file, "/tmp/cert.pem");
    BOOST_CHECK_EQUAL(pki_provider->credentials().key_file, "/tmp/key.pem");
}

BOOST_AUTO_TEST_CASE(dtls_rpk_mode_selects_dtls_rpk_provider, *boost::unit_test::timeout(10)) {
    coap_security_config config;
    config.mode = coap_auth_mode::dtls_rpk;
    config.credentials = rpk_credentials{std::vector<std::byte>(32, std::byte{0x01}),
                                         std::vector<std::byte>(32, std::byte{0x02}),
                                         {}};
    auto provider = make_security_provider(config, coap_security_role::client);
    BOOST_REQUIRE(provider != nullptr);
    BOOST_CHECK(provider->mode() == coap_auth_mode::dtls_rpk);
    auto* rpk_provider = dynamic_cast<dtls_rpk_provider*>(provider.get());
    BOOST_REQUIRE(rpk_provider != nullptr);
    BOOST_CHECK_EQUAL(rpk_provider->credentials().public_key.size(), 32u);
}

BOOST_AUTO_TEST_CASE(oscore_mode_selects_oscore_provider, *boost::unit_test::timeout(10)) {
    coap_security_config config;
    config.mode = coap_auth_mode::oscore;
    oscore_credentials creds;
    creds.sender_id = {std::byte{0x00}};
    creds.recipient_id = {std::byte{0x01}};
    creds.master_secret = std::vector<std::byte>(16, std::byte{0xAA});
    config.credentials = creds;
    auto provider = make_security_provider(config, coap_security_role::client);
    BOOST_REQUIRE(provider != nullptr);
    BOOST_CHECK(provider->mode() == coap_auth_mode::oscore);
    auto* oscore = dynamic_cast<oscore_provider*>(provider.get());
    BOOST_REQUIRE(oscore != nullptr);
    BOOST_CHECK_EQUAL(oscore->credentials().master_secret.size(), 16u);
}

BOOST_AUTO_TEST_CASE(mismatched_mode_and_credentials_throws_config_error,
                     *boost::unit_test::timeout(10)) {
    // mode == dtls_pki but credentials holds psk_credentials.
    coap_security_config config;
    config.mode = coap_auth_mode::dtls_pki;
    config.credentials = psk_credentials{"node-1", std::vector<std::byte>(16, std::byte{0x42})};
    BOOST_CHECK_THROW(make_security_provider(config, coap_security_role::client),
                      coap_security_config_error);
}

BOOST_AUTO_TEST_CASE(mode_with_no_credentials_throws_config_error, *boost::unit_test::timeout(10)) {
    coap_security_config config;
    config.mode = coap_auth_mode::oscore;
    // credentials left as std::monostate.
    BOOST_CHECK_THROW(make_security_provider(config, coap_security_role::client),
                      coap_security_config_error);
}

BOOST_AUTO_TEST_CASE(dtls_psk_rejects_out_of_range_key_length, *boost::unit_test::timeout(10)) {
    coap_security_config config;
    config.mode = coap_auth_mode::dtls_psk;
    config.credentials = psk_credentials{"node-1", std::vector<std::byte>(2, std::byte{0x42})};
    BOOST_CHECK_THROW(make_security_provider(config, coap_security_role::client),
                      coap_security_error);
}

BOOST_AUTO_TEST_SUITE_END()
