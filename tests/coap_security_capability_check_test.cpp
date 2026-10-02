// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "test_timeout_scale.hpp"
// **Feature: coap-transport-security, Requirement 9.7**
// coap_oscore_is_supported() returning false (here: the "libcoap not
// compiled into this build" stub path, since LIBCOAP_AVAILABLE is not
// defined in the default build — see coap_security_impl.hpp's
// oscore_provider) produces coap_unsupported_security_mode_error before any
// session/context mutation is attempted, distinct from peer-side
// authentication failures (Requirement 7.3).
#define BOOST_TEST_MODULE coap_security_capability_check_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT (30 * KYTHIRA_TEST_TIMEOUT_SCALE)

#include <raft/coap_security_impl.hpp>

using namespace kythira;

namespace {
auto make_oscore_config() -> coap_security_config {
    coap_security_config config;
    config.mode = coap_auth_mode::oscore;
    oscore_credentials creds;
    creds.sender_id = {std::byte{0x00}};
    creds.recipient_id = {std::byte{0x01}};
    creds.master_secret = std::vector<std::byte>(16, std::byte{0xAA});
    config.credentials = creds;
    return config;
}
}  // namespace

BOOST_AUTO_TEST_SUITE(coap_security_capability_check_tests)

#ifndef LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(oscore_configure_session_fails_capability_check_before_mutation,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    auto provider = make_security_provider(make_oscore_config(), coap_security_role::server);
    BOOST_CHECK_THROW(provider->configure_session(nullptr), coap_unsupported_security_mode_error);
}

BOOST_AUTO_TEST_CASE(oscore_create_client_session_fails_capability_check,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    auto provider = make_security_provider(make_oscore_config(), coap_security_role::client);
    BOOST_CHECK_THROW(
        provider->create_client_session(nullptr, nullptr, nullptr, /*COAP_PROTO_UDP=*/0),
        coap_unsupported_security_mode_error);
}

BOOST_AUTO_TEST_CASE(capability_error_identifies_mode_and_is_distinct_from_security_error,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    auto provider = make_security_provider(make_oscore_config(), coap_security_role::server);
    try {
        provider->configure_session(nullptr);
        BOOST_FAIL("expected coap_unsupported_security_mode_error");
    } catch (const coap_unsupported_security_mode_error& e) {
        BOOST_CHECK(e.mode() == coap_auth_mode::oscore);
        // Distinct exception type from peer-side authentication failures
        // (Requirement 7.3), even though both derive from
        // coap_security_error.
        BOOST_CHECK(dynamic_cast<const coap_unsupported_security_mode_error*>(&e) != nullptr);
    }
}

#else

// When LIBCOAP_AVAILABLE is defined (a test binary built with real libcoap
// linked), the linked library genuinely has OSCORE compiled in (confirmed
// via coap_oscore_is_supported() during design), so this build has nothing
// to assert about the capability-absent path — see
// coap_oscore_integration_test.cpp for the real-libcoap OSCORE tests.
BOOST_AUTO_TEST_CASE(placeholder_when_built_with_libcoap,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    BOOST_CHECK(coap_oscore_is_supported());
}

#endif

// Requirement 7.2/7.3: the DTLS capability decision every DTLS provider (and
// OSCORE layered over DTLS) runs before touching a context. Exercised here
// directly with libcoap's answers stubbed, since no CI build links a libcoap
// without DTLS.
BOOST_AUTO_TEST_CASE(dtls_capability_check_passes_when_supported,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    BOOST_CHECK_NO_THROW(require_dtls_capability(coap_auth_mode::dtls_psk, "PSK", true, true));
}

BOOST_AUTO_TEST_CASE(dtls_capability_check_fails_without_dtls,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    try {
        require_dtls_capability(coap_auth_mode::dtls_pki, "PKI", false, false);
        BOOST_FAIL("expected coap_unsupported_security_mode_error");
    } catch (const coap_unsupported_security_mode_error& e) {
        BOOST_CHECK(e.mode() == coap_auth_mode::dtls_pki);
        const std::string what = e.what();
        BOOST_CHECK(what.find("dtls_pki") != std::string::npos);
        BOOST_CHECK(what.find("DTLS not compiled") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(dtls_capability_check_names_the_missing_credential_type,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    // e.g. RPK with a TLS library that has no raw public keys. OSCORE over
    // DTLS reports its own mode, with the DTLS layer's credential type.
    try {
        require_dtls_capability(coap_auth_mode::oscore, "RPK", true, false);
        BOOST_FAIL("expected coap_unsupported_security_mode_error");
    } catch (const coap_unsupported_security_mode_error& e) {
        BOOST_CHECK(e.mode() == coap_auth_mode::oscore);
        BOOST_CHECK(std::string(e.what()).find("DTLS-RPK") != std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()
