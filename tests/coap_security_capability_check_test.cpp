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

// When LIBCOAP_AVAILABLE is defined, the capability the OSCORE provider needs
// is the *absence* of libcoap's own OSCORE: Kythira does OSCORE itself, and a
// libcoap built with it decrypts (or drops) every protected request before a
// handler sees it (oscore_provider's class comment, vcpkg-overlays/libcoap).
// Whichever libcoap is linked, the provider must agree with it.
BOOST_AUTO_TEST_CASE(oscore_provider_matches_the_linked_libcoap,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    coap_startup();
    auto provider = make_security_provider(make_oscore_config(), coap_security_role::server);
    coap_context_t* ctx = coap_new_context(nullptr);
    BOOST_REQUIRE(ctx != nullptr);
    if (coap_oscore_is_supported() != 0) {
        BOOST_CHECK_THROW(provider->configure_session(ctx), coap_unsupported_security_mode_error);
    } else {
        BOOST_CHECK_NO_THROW(provider->configure_session(ctx));
    }
    coap_free_context(ctx);
}

#endif

BOOST_AUTO_TEST_SUITE_END()
